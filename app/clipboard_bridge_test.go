package main

import (
	"bufio"
	"encoding/base64"
	"net"
	"strings"
	"testing"
	"time"
)

func TestClipboardCanCopyPreviouslyReceivedTextAgain(t *testing.T) {
	var state clipboardSyncState
	state.markGuestAccepted("first")
	state.markHostSent("second")
	if !state.shouldSendHost("first") {
		t.Fatal("recopied text was suppressed")
	}
}

func TestClipboardRejectsUnrepresentableWindowsText(t *testing.T) {
	for _, text := range []string{"", "a\x00b", "\xff"} {
		if clipboardTextAllowed(text) {
			t.Fatalf("accepted %q", text)
		}
	}
	if !clipboardTextAllowed("こんにちは\n\n") {
		t.Fatal("valid Unicode rejected")
	}
}

func TestClipboardFailedGuestWriteCanRetry(t *testing.T) {
	calls := 0
	b := &clipBridge{setText: func(string) bool { calls++; return calls > 1 }}
	b.acceptGuestText("retry me")
	b.acceptGuestText("retry me")
	b.acceptGuestText("retry me")
	if calls != 2 {
		t.Fatalf("got %d writes", calls)
	}
}

func TestClipboardFailedHostWriteCanRetry(t *testing.T) {
	host, guest := net.Pipe()
	guest.Close()
	b := &clipBridge{pullConn: host, getText: func() (string, bool) { return "retry me", true }}
	b.sendCurrentHost(host)
	if b.pullConn != nil || !b.state.shouldSendHost("retry me") {
		t.Fatal("failed write consumed clipboard")
	}
	host, guest = net.Pipe()
	defer host.Close()
	defer guest.Close()
	b.pullConn = host
	done := make(chan struct{})
	go func() { b.sendCurrentHost(host); close(done) }()
	guest.SetReadDeadline(time.Now().Add(time.Second))
	line, err := bufio.NewReader(guest).ReadString('\n')
	if err != nil || line != base64.StdEncoding.EncodeToString([]byte("retry me"))+"\n" {
		t.Fatalf("got %q, %v", line, err)
	}
	<-done
	if b.state.shouldSendHost("retry me") {
		t.Fatal("successful delivery not recorded")
	}
}

func TestClipboardReconnectReceivesCurrentHostText(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	const value = "current clipboard\n\n"
	b := &clipBridge{getText: func() (string, bool) { return value, true }}
	done := make(chan struct{})
	go func() { b.acceptPull(listener); close(done) }()
	for i := 0; i < 2; i++ {
		c, err := net.Dial("tcp", listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		c.SetReadDeadline(time.Now().Add(time.Second))
		line, err := bufio.NewReader(c).ReadString('\n')
		c.Close()
		if err != nil || line != base64.StdEncoding.EncodeToString([]byte(value))+"\n" {
			t.Fatalf("connection %d: %q %v", i, line, err)
		}
	}
	listener.Close()
	<-done
	b.mu.Lock()
	b.pullConn.Close()
	b.mu.Unlock()
}

func TestClipboardGuestWriteAndHostPollAreSerialized(t *testing.T) {
	entered, release := make(chan struct{}), make(chan struct{})
	current := "old host text"
	b := &clipBridge{setText: func(s string) bool { close(entered); <-release; current = s; return true }, getText: func() (string, bool) { return current, true }}
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	b.pullConn = host
	accepted, polled := make(chan struct{}), make(chan struct{})
	go func() { b.acceptGuestText("guest text"); close(accepted) }()
	<-entered
	go func() { b.sendCurrentHost(host); close(polled) }()
	close(release)
	<-accepted
	select {
	case <-polled:
	case <-time.After(time.Second):
		t.Fatal("guest clipboard echoed to host connection")
	}
}

func TestClipboardPushPreservesTextAndRejectsMalformedFrames(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	got := make(chan string, 8)
	b := &clipBridge{setText: func(s string) bool { got <- s; return true }}
	done := make(chan struct{})
	go func() { b.acceptPush(listener); close(done) }()
	for _, frame := range []string{"not base64\n", base64.StdEncoding.EncodeToString([]byte("no terminator")), base64.StdEncoding.EncodeToString([]byte("a\x00b")) + "\n", base64.StdEncoding.EncodeToString([]byte("valid\n\n")) + "\n"} {
		c, err := net.Dial("tcp", listener.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		c.Write([]byte(frame))
		c.Close()
	}
	select {
	case value := <-got:
		if value != "valid\n\n" {
			t.Fatalf("got %q", value)
		}
	case <-time.After(time.Second):
		t.Fatal("valid clipboard not delivered")
	}
	listener.Close()
	<-done
}

func TestClipboardSkipsOversizedHostBeforeEncoding(t *testing.T) {
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	b := &clipBridge{pullConn: host, getText: func() (string, bool) { return strings.Repeat("x", maxClipboardTextBytes+1), true }}
	b.sendCurrentHost(host)
	if b.state.lastSeen != "" {
		t.Fatal("oversized text recorded")
	}
}
