package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"strings"
	"testing"
)

func validHelloRequest() helloRequest {
	return helloRequest{
		Type: "authorize", Version: helloProtocolVersion,
		Operation: "sudo", GuestID: strings.Repeat("a", 64),
		RequestID: "c1d0243d-705e-4b64-a5c3-458d4e2b1f9d",
		Challenge: strings.Repeat("b", 64), User: "root",
		RequestingUser: "omarchy", Service: "sudo", TTY: "/dev/pts/3",
		CredentialID: base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{7}, 32)),
	}
}

func TestHelloRequestRejectsSpoofedOrAmbiguousContext(t *testing.T) {
	valid := validHelloRequest()
	line := mustHelloRequestLine(valid)
	if _, err := parseHelloRequest(line); err != nil {
		t.Fatalf("valid request: %v", err)
	}
	enroll := valid
	enroll.Operation, enroll.User, enroll.RequestingUser, enroll.TTY, enroll.CredentialID = "enroll", "", "", "", ""
	if _, err := parseHelloRequest(mustHelloRequestLine(enroll)); err != nil {
		t.Fatalf("valid enrollment: %v", err)
	}
	for name, mutate := range map[string]func(*helloRequest){
		"foreign service":     func(r *helloRequest) { r.Service = "login" },
		"nonlocal tty":        func(r *helloRequest) { r.TTY = "/dev/ttyUSB0" },
		"spoofed account":     func(r *helloRequest) { r.RequestingUser = "omarchy;sudo" },
		"uppercase id":        func(r *helloRequest) { r.GuestID = strings.ToUpper(r.GuestID) },
		"missing nonce":       func(r *helloRequest) { r.Challenge = "" },
		"bad operation":       func(r *helloRequest) { r.Operation = "login" },
		"old version":         func(r *helloRequest) { r.Version = 1 },
		"missing credential":  func(r *helloRequest) { r.CredentialID = "" },
		"short credential":    func(r *helloRequest) { r.CredentialID = "AAAA" },
		"unpadded credential": func(r *helloRequest) { r.CredentialID = strings.TrimRight(r.CredentialID, "=") },
		"enroll names a key":  func(r *helloRequest) { r.Operation, r.User, r.RequestingUser, r.TTY = "enroll", "", "", "" },
		"disable without a key": func(r *helloRequest) {
			r.Operation, r.User, r.RequestingUser, r.TTY, r.CredentialID = "disable", "", "", "", ""
		},
	} {
		t.Run(name, func(t *testing.T) {
			candidate := valid
			mutate(&candidate)
			if _, err := parseHelloRequest(mustHelloRequestLine(candidate)); err == nil {
				t.Fatal("unsafe request accepted")
			}
		})
	}
	for name, candidate := range map[string][]byte{
		"duplicate field": bytes.Replace(line, []byte(`"type":"authorize"`), []byte(`"type":"authorize","type":"authorize"`), 1),
		"unknown field":   bytes.Replace(line, []byte(`"type":"authorize"`), []byte(`"type":"authorize","allow":true`), 1),
		"trailing object": append(bytes.TrimSuffix(line, []byte("\n")), []byte("{}\n")...),
		"oversize":        append(bytes.Repeat([]byte(" "), helloMaximumLineBytes), '\n'),
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := parseHelloRequest(candidate); err == nil {
				t.Fatal("ambiguous request accepted")
			}
		})
	}
}

func TestHelloClientDataBindsEveryRequestField(t *testing.T) {
	request := validHelloRequest()
	original := helloClientData(request)
	want := `{"type":"try-omarchy.windows-hello.sudo","version":2,"operation":"sudo","guestId":"` +
		request.GuestID + `","requestId":"` + request.RequestID + `","challenge":"` + request.Challenge +
		`","user":"root","requestingUser":"omarchy","service":"sudo","tty":"/dev/pts/3","credentialId":"` +
		request.CredentialID + `"}`
	if string(original) != want {
		t.Fatalf("client data is not the canonical form the guest rebuilds:\n%s\n%s", original, want)
	}
	changes := []func(*helloRequest){
		func(r *helloRequest) { r.GuestID = strings.Repeat("e", 64) },
		func(r *helloRequest) { r.RequestID = "c1d0243d-705e-4b64-a5c3-458d4e2b1f9e" },
		func(r *helloRequest) { r.Challenge = strings.Repeat("d", 64) },
		func(r *helloRequest) { r.User = "omarchy" },
		func(r *helloRequest) { r.RequestingUser = "root" },
		func(r *helloRequest) { r.TTY = "/dev/pts/4" },
		func(r *helloRequest) { r.CredentialID = base64.StdEncoding.EncodeToString(bytes.Repeat([]byte{8}, 32)) },
		func(r *helloRequest) { r.Operation = "enroll"; r.User = ""; r.RequestingUser = ""; r.TTY = "" },
	}
	for index, change := range changes {
		candidate := request
		change(&candidate)
		if bytes.Equal(original, helloClientData(candidate)) {
			t.Fatalf("change %d did not change signed bytes", index)
		}
	}
}

func TestHelloDenialCarriesNoApprovalMaterial(t *testing.T) {
	request := validHelloRequest()
	response := helloDenied(request)
	encoded, err := json.Marshal(response)
	if err != nil {
		t.Fatal(err)
	}
	if response.Approved || response.Signature != "" || response.AuthenticatorData != "" ||
		response.CredentialID != request.CredentialID || !bytes.Contains(encoded, []byte(`"approved":false`)) {
		t.Fatalf("denial contained authorization material: %s", encoded)
	}
}
