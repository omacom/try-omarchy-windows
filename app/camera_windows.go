//go:build windows

package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"time"
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

func dialCamera() (net.Conn, error) {
	return net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", cameraPort))
}

// runCameraBridge keeps the camera channel connected across guest reboots and
// QEMU restarts for as long as the launcher supervises the VM.
func runCameraBridge() {
	go func() {
		for {
			conn, err := dialCamera()
			if err != nil {
				time.Sleep(time.Second)
				continue
			}
			logf("camera: bridge connected on %d", cameraPort)
			err = serveCamera(conn, newCameraFrameSource())
			_ = conn.Close()
			if err != nil && err != io.EOF {
				logf("camera: %v", err)
			}
			time.Sleep(time.Second)
		}
	}()
}
