package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"regexp"
	"strconv"
	"strings"
)

// The authentication port accepts one bounded JSON line per request. These
// types deliberately live outside the Windows UI implementation so both sides
// of the bridge can be checked without invoking a Hello prompt.
const (
	helloProtocolVersion  = 1
	helloMaximumLineBytes = 4096
	helloApprovalSeconds  = 15
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
}

var helloRequestFields = map[string]bool{
	"type": true, "version": true, "operation": true, "guestId": true,
	"requestId": true, "challenge": true, "user": true,
	"requestingUser": true, "service": true, "tty": true,
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
	return request, nil
}

type helloResponse struct {
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
	Approved       bool   `json:"approved"`
	IssuedAt       int64  `json:"issuedAt"`
	ExpiresAt      int64  `json:"expiresAt"`
	KeyID          string `json:"keyId"`
	PublicKey      string `json:"publicKey"`
	Signature      string `json:"signature"`
}

func helloDenied(request helloRequest) helloResponse {
	return helloResponse{
		Type: "authorization-result", Version: helloProtocolVersion,
		Operation: request.Operation, GuestID: request.GuestID,
		RequestID: request.RequestID, Challenge: request.Challenge,
		User: request.User, RequestingUser: request.RequestingUser,
		Service: request.Service, TTY: request.TTY,
	}
}

func helloKeyID(publicKeyDER []byte) string {
	digest := sha256.Sum256(publicKeyDER)
	return hex.EncodeToString(digest[:])
}

// The same NUL-delimited byte sequence must be verified by the guest. Fields
// are validated ASCII before reaching this function, avoiding JSON ambiguity.
func helloApprovalPayload(request helloRequest, issuedAt int64, keyID string) ([]byte, error) {
	if _, err := parseHelloRequest(mustHelloRequestLine(request)); err != nil {
		return nil, fmt.Errorf("invalid approval request: %w", err)
	}
	if request.Operation == "disable" {
		return nil, errors.New("disable requests cannot be signed")
	}
	if issuedAt <= 0 || !helloHex32.MatchString(keyID) {
		return nil, errors.New("invalid approval time or key")
	}
	fields := []string{
		"try-omarchy-windows-hello-v1", request.GuestID, request.Operation,
		request.RequestID, request.Challenge, request.User, request.RequestingUser,
		request.Service, request.TTY, strconv.FormatInt(issuedAt, 10),
		strconv.FormatInt(issuedAt+helloApprovalSeconds, 10), keyID,
	}
	return []byte(strings.Join(fields, "\x00")), nil
}

func mustHelloRequestLine(request helloRequest) []byte {
	line, _ := json.Marshal(request)
	return append(line, '\n')
}
