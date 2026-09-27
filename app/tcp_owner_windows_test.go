//go:build windows

package main

import (
	"net"
	"os"
	"testing"
)

func TestLoopbackPeerPIDFindsTheDialingProcess(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	accepted := make(chan net.Conn, 1)
	go func() {
		conn, err := listener.Accept()
		if err == nil {
			accepted <- conn
		}
	}()
	client, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	server := <-accepted
	defer server.Close()
	pid, err := loopbackPeerPID(server)
	if err != nil {
		t.Fatal(err)
	}
	if pid != uint32(os.Getpid()) {
		t.Fatalf("peer PID = %d, want %d", pid, os.Getpid())
	}
}
