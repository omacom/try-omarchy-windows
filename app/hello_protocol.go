package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"regexp"
)

// The authentication port accepts one bounded JSON line per request. These
// types deliberately live outside the Windows UI implementation so both sides
// of the bridge can be checked without invoking a Hello prompt.
//
// Windows Hello answers through the platform WebAuthn authenticator. One
// prompt both verifies the user and signs, so each sudo request costs one PIN.
// The signed client data is helloClientData(request); the guest rebuilds the
// same bytes and checks the ES256 signature over the authenticator data and
// their SHA-256 with the key it pinned at enrollment.
const (
	helloProtocolVersion  = 2
	helloMaximumLineBytes = 4096
	helloRelyingPartyID   = "try-omarchy.invalid"
	helloClientDataType   = "try-omarchy.windows-hello.sudo"
)

var (
	helloHex32   = regexp.MustCompile(`^[0-9a-f]{64}$`)
	helloUUID    = regexp.MustCompile(`^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$`)
	helloAccount = regexp.MustCompile(`^[a-z_][a-z0-9_-]{0,31}$`)
	helloTTY     = regexp.MustCompile(`^/dev/(pts/[0-9]+|tty[0-9]+)$`)
)

type helloRequest struct {
	Type           string `json:"type"`
	Version        int    `json:"version"`
	Operation      string `json:"operation"`
	GuestID        string `json:"guestId"`
	RequestID      string `json:"requestId"`
	Challenge      string `json:"challenge"`
	User           string `json:"user"`
	RequestingUser string `json:"requestingUser"`
	Service        string `json:"service"`
	TTY            string `json:"tty"`
	CredentialID   string `json:"credentialId"`
}

var helloRequestFields = map[string]bool{
	"type": true, "version": true, "operation": true, "guestId": true,
	"requestId": true, "challenge": true, "user": true,
	"requestingUser": true, "service": true, "tty": true, "credentialId": true,
}

// helloCredentialID decodes a pinned credential ID. Windows Hello uses 32
// bytes; allow the range authenticators use in practice.
func helloCredentialID(value string) ([]byte, error) {
	decoded, err := base64.StdEncoding.Strict().DecodeString(value)
	if err != nil || len(decoded) < 16 || len(decoded) > 255 {
		return nil, errors.New("invalid credential ID")
	}
	return decoded, nil
}

func parseHelloRequest(line []byte) (helloRequest, error) {
	var request helloRequest
	if len(line) == 0 || len(line) > helloMaximumLineBytes || line[len(line)-1] != '\n' {
		return request, errors.New("invalid authentication line length or terminator")
	}
	line = line[:len(line)-1]
	if bytes.IndexByte(line, '\n') >= 0 {
		return request, errors.New("authentication request contains another line")
	}
	decoder := json.NewDecoder(bytes.NewReader(line))
	opening, err := decoder.Token()
	if err != nil || opening != json.Delim('{') {
		return request, errors.New("authentication request is not a JSON object")
	}
	fields := make(map[string]json.RawMessage, len(helloRequestFields))
	for decoder.More() {
		keyToken, err := decoder.Token()
		if err != nil {
			return request, errors.New("invalid authentication field")
		}
		key, ok := keyToken.(string)
		if !ok || !helloRequestFields[key] {
			return request, errors.New("unknown authentication field")
		}
		if _, duplicate := fields[key]; duplicate {
			return request, errors.New("duplicate authentication field")
		}
		var raw json.RawMessage
		if err := decoder.Decode(&raw); err != nil {
			return request, errors.New("invalid authentication value")
		}
		fields[key] = raw
	}
	if len(fields) != len(helloRequestFields) {
		return request, errors.New("missing authentication field")
	}
	closing, err := decoder.Token()
	if err != nil || closing != json.Delim('}') {
		return request, errors.New("invalid authentication object end")
	}
	if _, err := decoder.Token(); err != io.EOF {
		return request, errors.New("trailing authentication data")
	}
	if err := json.Unmarshal(line, &request); err != nil {
		return request, errors.New("invalid authentication request types")
	}
	if request.Type != "authorize" || request.Version != helloProtocolVersion ||
		!helloHex32.MatchString(request.GuestID) ||
		!helloUUID.MatchString(request.RequestID) ||
		!helloHex32.MatchString(request.Challenge) || request.Service != "sudo" {
		return request, errors.New("invalid authentication identity or service")
	}
	switch request.Operation {
	case "enroll", "disable":
		if request.User != "" || request.RequestingUser != "" || request.TTY != "" {
			return request, errors.New("control request contains sudo context")
		}
	case "sudo":
		if !helloAccount.MatchString(request.User) ||
			!helloAccount.MatchString(request.RequestingUser) ||
			!helloTTY.MatchString(request.TTY) {
			return request, errors.New("invalid sudo context")
		}
	default:
		return request, errors.New("unknown authentication operation")
	}
	if request.Operation == "enroll" {
		if request.CredentialID != "" {
			return request, errors.New("enrollment names an existing credential")
		}
	} else if _, err := helloCredentialID(request.CredentialID); err != nil {
		return request, err
	}
	return request, nil
}

// helloClientData is the exact byte string Windows signs. Every field has
// been validated as plain ASCII without JSON escapes, so the guest's
// json.dumps(..., separators=(",", ":")) over the same ordered fields
// produces identical bytes.
func helloClientData(request helloRequest) []byte {
	data, _ := json.Marshal(struct {
		Type           string `json:"type"`
		Version        int    `json:"version"`
		Operation      string `json:"operation"`
		GuestID        string `json:"guestId"`
		RequestID      string `json:"requestId"`
		Challenge      string `json:"challenge"`
		User           string `json:"user"`
		RequestingUser string `json:"requestingUser"`
		Service        string `json:"service"`
		TTY            string `json:"tty"`
		CredentialID   string `json:"credentialId"`
	}{
		helloClientDataType, request.Version, request.Operation, request.GuestID,
		request.RequestID, request.Challenge, request.User, request.RequestingUser,
		request.Service, request.TTY, request.CredentialID,
	})
	return data
}

// An approved enrollment carries the new credential's authenticator data,
// from which the guest pins the credential ID and public key. An approved
// sudo request carries the assertion's authenticator data and signature. An
// approved disable only reports that the Windows credential is gone.
type helloResponse struct {
	Type              string `json:"type"`
	Version           int    `json:"version"`
	Operation         string `json:"operation"`
	GuestID           string `json:"guestId"`
	RequestID         string `json:"requestId"`
	Challenge         string `json:"challenge"`
	User              string `json:"user"`
	RequestingUser    string `json:"requestingUser"`
	Service           string `json:"service"`
	TTY               string `json:"tty"`
	CredentialID      string `json:"credentialId"`
	Approved          bool   `json:"approved"`
	AuthenticatorData string `json:"authenticatorData"`
	Signature         string `json:"signature"`
}

func helloDenied(request helloRequest) helloResponse {
	return helloResponse{
		Type: "authorization-result", Version: helloProtocolVersion,
		Operation: request.Operation, GuestID: request.GuestID,
		RequestID: request.RequestID, Challenge: request.Challenge,
		User: request.User, RequestingUser: request.RequestingUser,
		Service: request.Service, TTY: request.TTY,
		CredentialID: request.CredentialID,
	}
}

func mustHelloRequestLine(request helloRequest) []byte {
	line, _ := json.Marshal(request)
	return append(line, '\n')
}
