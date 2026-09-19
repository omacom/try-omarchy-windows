package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"
)

// The host half of the camera bridge. The guest side
// (usr/local/bin/omarchy-windows-camera-bridge) opens the virtio port named
// dev.tryomarchy.camera and asks this side to start capture when a camera
// consumer appears. Frames travel as the same framed messages the macOS guest
// bridge uses, so the two guests share the wire format:
//
//	header: 4s magic "TOCM", u8 version, u8 kind, u16 reserved, u32 length, u32 sequence
//	kind 1:  JSON status (streaming/idle/unavailable)
//	kind 2:  one NV12 frame
const (
	cameraMagic      = "TOCM"
	cameraVersion    = 1
	cameraKindStatus = 1
	cameraKindFrame  = 2

	cameraWidth      = 1280
	cameraHeight     = 720
	cameraFrameBytes = cameraWidth * cameraHeight * 3 / 2
	cameraHeaderSize = 16
)

// cameraFrameSource delivers NV12 frames until stop is called. start returns a
// closed channel when capture ends.
type cameraFrameSource interface {
	start() (<-chan []byte, error)
	stop()
}

func cameraHeader(kind uint8, length int, sequence uint32) []byte {
	header := make([]byte, cameraHeaderSize)
	copy(header[0:4], cameraMagic)
	header[4] = cameraVersion
	header[5] = kind
	binary.LittleEndian.PutUint16(header[6:8], 0)
	binary.LittleEndian.PutUint32(header[8:12], uint32(length))
	binary.LittleEndian.PutUint32(header[12:16], sequence)
	return header
}

// cameraBlackFrame is video-range black: luma 16, neutral chroma 128.
func cameraBlackFrame() []byte {
	frame := make([]byte, cameraFrameBytes)
	for i := 0; i < cameraWidth*cameraHeight; i++ {
		frame[i] = 16
	}
	for i := cameraWidth * cameraHeight; i < cameraFrameBytes; i++ {
		frame[i] = 128
	}
	return frame
}

func cameraStatus(value map[string]any) []byte {
	encoded, _ := json.Marshal(value)
	return encoded
}

type cameraControl struct {
	Type string `json:"type"`
}

// cameraConnection serializes framed writes. Only the bridge loop writes, so a
// mutex is enough to keep a header and its payload adjacent.
type cameraConnection struct {
	conn     net.Conn
	mu       sync.Mutex
	sequence uint32
}

func (c *cameraConnection) message(kind uint8, payload []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return err
	}
	header := cameraHeader(kind, len(payload), c.sequence)
	if kind == cameraKindFrame {
		c.sequence++
	}
	if _, err := c.conn.Write(header); err != nil {
		return err
	}
	_, err := c.conn.Write(payload)
	return err
}

func (c *cameraConnection) frame(frame []byte) error {
	if len(frame) != cameraFrameBytes {
		return fmt.Errorf("camera frame has %d bytes, expected %d", len(frame), cameraFrameBytes)
	}
	return c.message(cameraKindFrame, frame)
}

func (c *cameraConnection) status(value map[string]any) error {
	return c.message(cameraKindStatus, cameraStatus(value))
}

// serveCamera drives one connection: it reads guest control lines and streams
// frames only while capture is requested.
func serveCamera(conn net.Conn, source cameraFrameSource) error {
	connection := &cameraConnection{conn: conn}
	lines := make(chan string, 8)
	readErr := make(chan error, 1)
	done := make(chan struct{})
	defer close(done)
	defer conn.Close()
	go func() {
		reader := bufio.NewScanner(conn)
		reader.Buffer(make([]byte, 1024), 4096)
		for reader.Scan() {
			select {
			case lines <- strings.TrimSpace(reader.Text()):
			case <-done:
				return
			}
		}
		readErr <- reader.Err()
	}()

	var frames <-chan []byte
	stop := func() {
		if frames != nil {
			source.stop()
			frames = nil
		}
	}
	defer stop()

	for {
		select {
		case err := <-readErr:
			return err
		case line := <-lines:
			var control cameraControl
			if err := json.Unmarshal([]byte(line), &control); err != nil {
				continue
			}
			switch control.Type {
			case "start":
				if frames != nil {
					continue
				}
				stream, err := source.start()
				if err != nil {
					_ = connection.status(map[string]any{"status": "unavailable", "reason": err.Error()})
					continue
				}
				frames = stream
				_ = connection.status(map[string]any{"status": "streaming", "name": "Windows Camera"})
			case "stop":
				stop()
				_ = connection.frame(cameraBlackFrame())
				_ = connection.status(map[string]any{"status": "idle"})
			}
		case frame, ok := <-frames:
			if !ok {
				stop()
				if err := connection.status(map[string]any{"status": "unavailable", "reason": "camera capture ended"}); err != nil {
					return err
				}
				continue
			}
			if err := connection.frame(frame); err != nil {
				return err
			}
		}
	}
}
