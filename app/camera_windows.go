//go:build windows

package main

import (
	"fmt"
	"net"
	"os"
	"time"
)

// newCameraFrameSource picks the capture backend. TRYOMARCHY_FAKE_CAMERA makes
// the bridge emit a moving test pattern instead of touching hardware, so the
// guest half can be exercised end to end without a camera.
func newCameraFrameSource() cameraFrameSource {
	if os.Getenv("TRYOMARCHY_FAKE_CAMERA") != "" {
		logf("camera: using the synthetic frame source")
		return &syntheticCameraSource{}
	}
	return &mfCameraSource{}
}

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

// syntheticCameraSource emits a slow-moving bar as video-range NV12. It exists
// only for testing the bridge without hardware.
type syntheticCameraSource struct {
	frames chan []byte
	stopCh chan struct{}
}

func (s *syntheticCameraSource) start() (<-chan []byte, error) {
	s.frames = make(chan []byte, 2)
	s.stopCh = make(chan struct{})
	stopCh, frames := s.stopCh, s.frames
	go func() {
		ticker := time.NewTicker(time.Second / 30)
		defer ticker.Stop()
		var column int
		for {
			select {
			case <-stopCh:
				return
			case <-ticker.C:
				select {
				case frames <- syntheticFrame(column):
				default: // drop when the reader is behind
				}
				column = (column + 8) % cameraWidth
			}
		}
	}()
	return s.frames, nil
}

func (s *syntheticCameraSource) stop() {
	if s.stopCh != nil {
		close(s.stopCh)
		s.stopCh = nil
	}
}

func syntheticFrame(column int) []byte {
	frame := cameraBlackFrame()
	luma := cameraWidth * cameraHeight
	for y := 0; y < cameraHeight; y++ {
		for x := 0; x < 64; x++ {
			px := (column + x) % cameraWidth
			frame[y*cameraWidth+px] = 180
		}
	}
	for i := luma; i < cameraFrameBytes; i++ {
		frame[i] = 96
	}
	return frame
}
