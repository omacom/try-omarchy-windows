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
			// Only this launcher's QEMU may use the port. Any other local
			// process, including one in another Windows session, could
			// otherwise take the single slot or ask for prompts.
			if pid, err := loopbackPeerPID(conn); err != nil || pid == 0 || pid != qemuPid.Load() {
				logf("Windows Hello: refused a connection that is not from Omarchy's QEMU")
				conn.Close()
				continue
			}
			if !active.CompareAndSwap(false, true) {
				conn.Close()
				continue
			}
			go func() {
				defer active.Store(false)
				if err := serveHelloBridge(conn, approveHelloRequest); err != nil {
					logf("Windows Hello: guest request rejected: %v", err)
				}
			}()
		}
	}()
}
