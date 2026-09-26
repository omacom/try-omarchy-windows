package main

import (
	"bytes"
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
	}
}

func TestHelloRequestRejectsSpoofedOrAmbiguousContext(t *testing.T) {
	valid := validHelloRequest()
	line := mustHelloRequestLine(valid)
	if _, err := parseHelloRequest(line); err != nil {
		t.Fatalf("valid request: %v", err)
	}
	for name, mutate := range map[string]func(*helloRequest){
		"foreign service": func(r *helloRequest) { r.Service = "login" },
		"nonlocal tty":    func(r *helloRequest) { r.TTY = "/dev/ttyUSB0" },
		"spoofed account": func(r *helloRequest) { r.RequestingUser = "omarchy;sudo" },
		"uppercase id":    func(r *helloRequest) { r.GuestID = strings.ToUpper(r.GuestID) },
		"missing nonce":   func(r *helloRequest) { r.Challenge = "" },
		"bad operation":   func(r *helloRequest) { r.Operation = "login" },
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

func TestHelloApprovalBindsEveryRequestField(t *testing.T) {
	request := validHelloRequest()
	keyID := strings.Repeat("c", 64)
	original, err := helloApprovalPayload(request, 12345, keyID)
	if err != nil {
		t.Fatal(err)
	}
	changes := []func(*helloRequest){
		func(r *helloRequest) { r.GuestID = strings.Repeat("e", 64) },
		func(r *helloRequest) { r.RequestID = "c1d0243d-705e-4b64-a5c3-458d4e2b1f9e" },
		func(r *helloRequest) { r.Challenge = strings.Repeat("d", 64) },
		func(r *helloRequest) { r.User = "omarchy" },
		func(r *helloRequest) { r.RequestingUser = "root" },
		func(r *helloRequest) { r.TTY = "/dev/pts/4" },
		func(r *helloRequest) { r.Operation = "enroll"; r.User = ""; r.RequestingUser = ""; r.TTY = "" },
	}
	for index, change := range changes {
		candidate := request
		change(&candidate)
		payload, err := helloApprovalPayload(candidate, 12345, keyID)
		if err != nil {
			t.Fatalf("change %d: %v", index, err)
		}
		if bytes.Equal(original, payload) {
			t.Fatalf("change %d did not change signed bytes", index)
		}
	}
	for _, variant := range []struct {
		issuedAt int64
		keyID    string
	}{
		{12346, keyID}, {12345, strings.Repeat("d", 64)},
	} {
		payload, err := helloApprovalPayload(request, variant.issuedAt, variant.keyID)
		if err != nil || bytes.Equal(original, payload) {
			t.Fatalf("time or key not bound: %v", err)
		}
	}
	disable := request
	disable.Operation, disable.User, disable.RequestingUser, disable.TTY = "disable", "", "", ""
	if _, err := helloApprovalPayload(disable, 12345, keyID); err == nil {
		t.Fatal("disable request received a signable payload")
	}
	response := helloDenied(request)
	encoded, err := json.Marshal(response)
	if err != nil {
		t.Fatal(err)
	}
	if response.Approved || response.Signature != "" || response.PublicKey != "" ||
		!bytes.Contains(encoded, []byte(`"issuedAt":0`)) {
		t.Fatal("denial contained authorization material")
	}
}
