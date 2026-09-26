package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"net"
	"testing"
	"time"
)

func TestHelloPortFallsThroughToPassword(t *testing.T) {
	guest, host := net.Pipe()
	done := make(chan error, 1)
	go func() { done <- serveHelloBridge(host) }()
	defer guest.Close()
	guest.SetDeadline(time.Now().Add(2 * time.Second))
	request := validHelloRequest()
	if _, err := guest.Write(mustHelloRequestLine(request)); err != nil {
		t.Fatal(err)
	}
	line, err := bufio.NewReader(guest).ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var response helloResponse
	if err := json.Unmarshal(line, &response); err != nil {
		t.Fatal(err)
	}
	if response.Approved || response.RequestID != request.RequestID ||
		response.Signature != "" || response.KeyID != "" || response.PublicKey != "" {
		t.Fatalf("denial did not preserve request context: %+v", response)
	}
	guest.Close()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestHelloPortDropsMalformedRequestWithoutReply(t *testing.T) {
	guest, host := net.Pipe()
	done := make(chan error, 1)
	go func() { done <- serveHelloBridge(host) }()
	guest.SetDeadline(time.Now().Add(2 * time.Second))
	line := bytes.Replace(mustHelloRequestLine(validHelloRequest()), []byte(`"service":"sudo"`), []byte(`"service":"login"`), 1)
	if _, err := guest.Write(line); err != nil {
		t.Fatal(err)
	}
	var byteBuffer [1]byte
	if count, err := guest.Read(byteBuffer[:]); count != 0 || err == nil {
		t.Fatalf("malformed request received a reply: count=%d error=%v", count, err)
	}
	if err := <-done; err == nil {
		t.Fatal("malformed request accepted")
	}
}
