//go:build windows

package main

import (
	"fmt"
	"net"
	"sync/atomic"
)

func runHelloBridge() {
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", helloBridgePort))
	if err != nil {
		fatal("Try Omarchy authentication port %d is in use.", helloBridgePort)
	}
	logf("Windows Hello: guest port listening on %d", helloBridgePort)
	var active atomic.Bool
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				logf("Windows Hello: accept: %v", err)
				return
			}
			if !active.CompareAndSwap(false, true) {
				conn.Close()
				continue
			}
			go func() {
				defer active.Store(false)
				if err := serveHelloBridge(conn); err != nil {
					logf("Windows Hello: guest request rejected: %v", err)
				}
			}()
		}
	}()
}
