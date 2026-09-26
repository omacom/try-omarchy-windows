//go:build windows

package main

import (
	"bytes"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"runtime"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Windows Hello through the platform WebAuthn authenticator in webauthn.dll.
// Enrollment creates one credential per guest under helloRelyingPartyID; each
// sudo request asks Windows for one assertion over helloClientData(request),
// which shows a single Windows Hello prompt. Nothing here can reach the
// user's other passkeys: assertions are limited to the guest's pinned
// credential under our relying party ID, and deletion first checks that the
// credential is listed under that ID.

var (
	webauthnDLL                  = syscall.NewLazyDLL("webauthn.dll")
	procWebAuthNAvailable        = webauthnDLL.NewProc("WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable")
	procWebAuthNMakeCredential   = webauthnDLL.NewProc("WebAuthNAuthenticatorMakeCredential")
	procWebAuthNGetAssertion     = webauthnDLL.NewProc("WebAuthNAuthenticatorGetAssertion")
	procWebAuthNFreeAttestation  = webauthnDLL.NewProc("WebAuthNFreeCredentialAttestation")
	procWebAuthNFreeAssertion    = webauthnDLL.NewProc("WebAuthNFreeAssertion")
	procWebAuthNCredentialList   = webauthnDLL.NewProc("WebAuthNGetPlatformCredentialList")
	procWebAuthNFreeCredentials  = webauthnDLL.NewProc("WebAuthNFreePlatformCredentialList")
	procWebAuthNDeleteCredential = webauthnDLL.NewProc("WebAuthNDeletePlatformCredential")
)

const (
	webauthnAttachmentPlatform = 1
	webauthnVerificationNeeded = 1
	webauthnAttestationNone    = 1
	webauthnCoseES256          = -7
	webauthnPromptTimeout      = 60 * time.Second
	ntePermissionUserCancelled = 0x80090036
	nteNotFound                = 0x80090011
)

// These mirror the version 1 layouts in Microsoft's webauthn.h. Passing
// version 1 tells Windows to read only these leading fields; the output
// structures are read only up to the fields every version shares.
type webauthnRP struct {
	Version  uint32
	ID       *uint16
	Name     *uint16
	IconPath *uint16
}

type webauthnUser struct {
	Version     uint32
	IDLength    uint32
	ID          *byte
	Name        *uint16
	IconPath    *uint16
	DisplayName *uint16
}

type webauthnClientData struct {
	Version       uint32
	Length        uint32
	Data          *byte
	HashAlgorithm *uint16
}

type webauthnCoseParameter struct {
	Version   uint32
	Type      *uint16
	Algorithm int32
}

type webauthnCoseParameters struct {
	Count      uint32
	Parameters *webauthnCoseParameter
}

type webauthnCredential struct {
	Version  uint32
	IDLength uint32
	ID       *byte
	Type     *uint16
}

type webauthnCredentials struct {
	Count       uint32
	Credentials *webauthnCredential
}

type webauthnExtensions struct {
	Count      uint32
	Extensions uintptr
}

type webauthnMakeCredentialOptions struct {
	Version            uint32
	TimeoutMS          uint32
	CredentialList     webauthnCredentials
	Extensions         webauthnExtensions
	Attachment         uint32
	RequireResidentKey int32
	UserVerification   uint32
	Attestation        uint32
	Flags              uint32
}

type webauthnGetAssertionOptions struct {
	Version          uint32
	TimeoutMS        uint32
	CredentialList   webauthnCredentials
	Extensions       webauthnExtensions
	Attachment       uint32
	UserVerification uint32
	Flags            uint32
}

type webauthnAttestation struct {
	Version                 uint32
	FormatType              *uint16
	AuthenticatorDataLength uint32
	AuthenticatorData       *byte
	AttestationLength       uint32
	Attestation             *byte
	AttestationDecodeType   uint32
	AttestationDecode       uintptr
	AttestationObjectLength uint32
	AttestationObject       *byte
	CredentialIDLength      uint32
	CredentialID            *byte
}

type webauthnAssertion struct {
	Version                 uint32
	AuthenticatorDataLength uint32
	AuthenticatorData       *byte
	SignatureLength         uint32
	Signature               *byte
	Credential              webauthnCredential
}

type webauthnGetCredentialsOptions struct {
	Version   uint32
	RPID      *uint16
	InPrivate int32
}

type webauthnCredentialDetails struct {
	Version  uint32
	IDLength uint32
	ID       *byte
}

type webauthnCredentialDetailsList struct {
	Count   uint32
	Details **webauthnCredentialDetails
}

var (
	helloPromptMu      sync.Mutex
	helloLastDenial    time.Time
	helloUnavailableMu sync.Once
)

func wideString(value string) *uint16 {
	pointer, _ := syscall.UTF16PtrFromString(value)
	return pointer
}

func copyWindowsBytes(pointer *byte, length uint32) []byte {
	if pointer == nil || length == 0 {
		return nil
	}
	return bytes.Clone(unsafe.Slice(pointer, length))
}

func helloOutcome(hr uintptr) string {
	switch uint32(hr) {
	case 0:
		return "approved"
	case ntePermissionUserCancelled:
		return "canceled"
	case nteNotFound:
		return "credential not found"
	}
	return fmt.Sprintf("HRESULT 0x%08x", uint32(hr))
}

// approveHelloRequest answers one validated guest request. Anything short of
// a completed Windows Hello operation is a denial, which sends the guest to
// its password prompt.
func approveHelloRequest(request helloRequest) helloResponse {
	response := helloDenied(request)
	if err := webauthnDLL.Load(); err != nil {
		helloUnavailableMu.Do(func() { logf("Windows Hello: webauthn.dll unavailable; guest sudo keeps its password") })
		return response
	}
	// webauthn.dll shows its prompt from the calling thread.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	helloPromptMu.Lock()
	defer helloPromptMu.Unlock()

	var err error
	switch request.Operation {
	case "disable":
		err = deleteHelloCredential(request)
		if err == nil {
			response.Approved = true
		}
	case "enroll", "sudo":
		pid := qemuPid.Load()
		owner := qemuHwnd.Load()
		switch {
		case pid == 0 || owner == 0 || foregroundPid() != pid:
			err = errors.New("Omarchy window is not in front")
		case time.Since(helloLastDenial) < 3*time.Second:
			err = errors.New("previous request was just denied")
		default:
			var available int32
			if hr, _, _ := procWebAuthNAvailable.Call(uintptr(unsafe.Pointer(&available))); hr != 0 || available == 0 {
				err = errors.New("no Windows Hello authenticator")
			} else if request.Operation == "enroll" {
				response.AuthenticatorData, err = createHelloCredential(owner, request)
			} else {
				response.AuthenticatorData, response.Signature, err = signHelloRequest(owner, request)
			}
		}
		if err == nil {
			response.Approved = true
		} else {
			helloLastDenial = time.Now()
		}
	}
	if err != nil {
		logf("Windows Hello: %s denied: %v", request.Operation, err)
	} else {
		logf("Windows Hello: %s approved", request.Operation)
	}
	return response
}

func createHelloCredential(owner uintptr, request helloRequest) (string, error) {
	userID, err := hex.DecodeString(request.GuestID)
	if err != nil {
		return "", err
	}
	clientData := helloClientData(request)
	rp := webauthnRP{Version: 1, ID: wideString(helloRelyingPartyID), Name: wideString("Try Omarchy")}
	user := webauthnUser{
		Version: 1, IDLength: uint32(len(userID)), ID: &userID[0],
		Name:        wideString("omarchy-sudo-" + request.GuestID[:8]),
		DisplayName: wideString("Omarchy sudo (" + request.GuestID[:8] + ")"),
	}
	algorithm := webauthnCoseParameter{Version: 1, Type: wideString("public-key"), Algorithm: webauthnCoseES256}
	algorithms := webauthnCoseParameters{Count: 1, Parameters: &algorithm}
	client := webauthnClientData{Version: 1, Length: uint32(len(clientData)), Data: &clientData[0], HashAlgorithm: wideString("SHA-256")}
	options := webauthnMakeCredentialOptions{
		Version: 1, TimeoutMS: uint32(webauthnPromptTimeout / time.Millisecond),
		Attachment: webauthnAttachmentPlatform, UserVerification: webauthnVerificationNeeded,
		Attestation: webauthnAttestationNone,
	}
	var attestation *webauthnAttestation
	hr, _, _ := procWebAuthNMakeCredential.Call(owner,
		uintptr(unsafe.Pointer(&rp)), uintptr(unsafe.Pointer(&user)),
		uintptr(unsafe.Pointer(&algorithms)), uintptr(unsafe.Pointer(&client)),
		uintptr(unsafe.Pointer(&options)), uintptr(unsafe.Pointer(&attestation)))
	runtime.KeepAlive(userID)
	runtime.KeepAlive(clientData)
	if hr != 0 || attestation == nil {
		return "", errors.New(helloOutcome(hr))
	}
	defer procWebAuthNFreeAttestation.Call(uintptr(unsafe.Pointer(attestation)))
	authenticatorData := copyWindowsBytes(attestation.AuthenticatorData, attestation.AuthenticatorDataLength)
	if len(authenticatorData) < 37 {
		return "", errors.New("Windows returned no authenticator data")
	}
	return base64.StdEncoding.EncodeToString(authenticatorData), nil
}

func signHelloRequest(owner uintptr, request helloRequest) (string, string, error) {
	credentialID, err := helloCredentialID(request.CredentialID)
	if err != nil {
		return "", "", err
	}
	clientData := helloClientData(request)
	allowed := webauthnCredential{Version: 1, IDLength: uint32(len(credentialID)), ID: &credentialID[0], Type: wideString("public-key")}
	client := webauthnClientData{Version: 1, Length: uint32(len(clientData)), Data: &clientData[0], HashAlgorithm: wideString("SHA-256")}
	options := webauthnGetAssertionOptions{
		Version: 1, TimeoutMS: uint32(webauthnPromptTimeout / time.Millisecond),
		CredentialList: webauthnCredentials{Count: 1, Credentials: &allowed},
		Attachment:     webauthnAttachmentPlatform, UserVerification: webauthnVerificationNeeded,
	}
	var assertion *webauthnAssertion
	hr, _, _ := procWebAuthNGetAssertion.Call(owner, uintptr(unsafe.Pointer(wideString(helloRelyingPartyID))),
		uintptr(unsafe.Pointer(&client)), uintptr(unsafe.Pointer(&options)), uintptr(unsafe.Pointer(&assertion)))
	runtime.KeepAlive(credentialID)
	runtime.KeepAlive(clientData)
	if hr != 0 || assertion == nil {
		return "", "", errors.New(helloOutcome(hr))
	}
	defer procWebAuthNFreeAssertion.Call(uintptr(unsafe.Pointer(assertion)))
	used := copyWindowsBytes(assertion.Credential.ID, assertion.Credential.IDLength)
	if !bytes.Equal(used, credentialID) {
		return "", "", errors.New("Windows signed with another credential")
	}
	authenticatorData := copyWindowsBytes(assertion.AuthenticatorData, assertion.AuthenticatorDataLength)
	signature := copyWindowsBytes(assertion.Signature, assertion.SignatureLength)
	if len(authenticatorData) < 37 || len(signature) == 0 {
		return "", "", errors.New("Windows returned an empty assertion")
	}
	return base64.StdEncoding.EncodeToString(authenticatorData), base64.StdEncoding.EncodeToString(signature), nil
}

// deleteHelloCredential removes the guest's credential, but only one Windows
// lists under our relying party ID. A credential that is already gone counts
// as removed.
func deleteHelloCredential(request helloRequest) error {
	credentialID, err := helloCredentialID(request.CredentialID)
	if err != nil {
		return err
	}
	if procWebAuthNCredentialList.Find() != nil || procWebAuthNDeleteCredential.Find() != nil {
		return errors.New("this Windows version cannot list passkeys; remove it in Settings > Accounts > Passkeys")
	}
	options := webauthnGetCredentialsOptions{Version: 1, RPID: wideString(helloRelyingPartyID)}
	var list *webauthnCredentialDetailsList
	hr, _, _ := procWebAuthNCredentialList.Call(uintptr(unsafe.Pointer(&options)), uintptr(unsafe.Pointer(&list)))
	if uint32(hr) == nteNotFound {
		return nil
	}
	if hr != 0 || list == nil {
		return errors.New(helloOutcome(hr))
	}
	defer procWebAuthNFreeCredentials.Call(uintptr(unsafe.Pointer(list)))
	found := false
	if list.Count > 0 && list.Details != nil {
		for _, details := range unsafe.Slice(list.Details, list.Count) {
			if details != nil && bytes.Equal(copyWindowsBytes(details.ID, details.IDLength), credentialID) {
				found = true
				break
			}
		}
	}
	if !found {
		return nil
	}
	hr, _, _ = procWebAuthNDeleteCredential.Call(uintptr(len(credentialID)), uintptr(unsafe.Pointer(&credentialID[0])))
	runtime.KeepAlive(credentialID)
	if hr != 0 {
		return errors.New(helloOutcome(hr))
	}
	return nil
}
