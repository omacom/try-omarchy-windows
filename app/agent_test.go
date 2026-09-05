package main

import (
	"bufio"
	"net"
	"strings"
	"testing"
	"time"
)

func TestGuestAgentSendsTimeOnConnectAndResume(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	fixed := time.Date(2026, 9, 5, 12, 0, 0, 0, time.UTC)
	a := &guestAgent{now: func() time.Time { return fixed }}
	resumed := make(chan struct{})
	go a.run(l, resumed)
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write([]byte("hello 1\n"))
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	line, err := r.ReadString('\n')
	if err != nil || strings.TrimSpace(line) != "time 1788609600" {
		t.Fatalf("on connect got %q, %v", line, err)
	}
	resumed <- struct{}{}
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if line, err = r.ReadString('\n'); err != nil || strings.TrimSpace(line) != "time 1788609600" {
		t.Fatalf("on resume got %q, %v", line, err)
	}
}

func TestGuestAgentWithoutConnectionIsSilent(t *testing.T) {
	a := newGuestAgent()
	if a.sendTime("test") {
		t.Fatal("sent without a connection")
	}
}

func TestGuestAgentReplacesAnEarlierConnection(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	go a.accept(l)
	first, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	bufio.NewReader(first).ReadString('\n')
	second, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	r := bufio.NewReader(second)
	second.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := r.ReadString('\n'); err != nil {
		t.Fatalf("second connection got no time: %v", err)
	}
	first.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1)
	if _, err := first.Read(buf); err == nil {
		t.Fatal("first connection was not closed")
	}
}

func TestGuestAgentZeroFillRoundTrip(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	if a.requestZeroFill(1024) {
		t.Fatal("request accepted without a guest")
	}
	go a.accept(l)
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	r.ReadString('\n') // time on connect
	deadline := time.Now().Add(2 * time.Second)
	for !a.requestZeroFill(1024) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if line, err := r.ReadString('\n'); err != nil || line != "zero-fill 1024\n" {
		t.Fatalf("guest got %q, %v", line, err)
	}
	c.Write([]byte("zero-fill done\n"))
	for !a.compactPending() && time.Now().Before(deadline.Add(2*time.Second)) {
		time.Sleep(20 * time.Millisecond)
	}
	if !a.compactPending() {
		t.Fatal("zero-fill completion not recorded")
	}
}

func TestReclaimBudgetFollowsHostFreeSpace(t *testing.T) {
	for free, want := range map[int64]int64{2 << 30: 0, 4300 << 20: 0, 6 << 30: 2048, 40 << 30: 8192} {
		if got := reclaimBudgetMiB(free); got != want {
			t.Errorf("free %d: budget %d, want %d", free, got, want)
		}
	}
}
