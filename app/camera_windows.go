//go:build windows

package main

import (
	"errors"
	"fmt"
	"net"
)

// windowsCameraSource captures the host camera. Media Foundation capture is
// added next; until then the guest is told the camera is unavailable so it
// never leaves /dev/video42 half-configured.
type windowsCameraSource struct{}

func newCameraFrameSource() cameraFrameSource { return windowsCameraSource{} }

func (windowsCameraSource) start() (<-chan []byte, error) {
	return nil, errors.New("camera capture is not available yet")
}

func (windowsCameraSource) stop() {}

// runCameraBridge listens for QEMU's camera chardev and serves the guest
// protocol on each connection. Listening here also fails loudly if another
// copy of the app already owns the port.
func runCameraBridge() {
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", cameraPort))
	if err != nil {
		fatal("Try Omarchy camera port %d is in use.", cameraPort)
	}
	logf("camera: bridge listening on %d", cameraPort)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				logf("camera: accept: %v", err)
				return
			}
			go func() {
				defer conn.Close()
				if err := serveCamera(conn, newCameraFrameSource()); err != nil {
					logf("camera: %v", err)
				}
			}()
		}
	}()
}
