package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"testing"
)

type fakeCameraSource struct {
	frames chan []byte
	stops  int
}

func (f *fakeCameraSource) start() (<-chan []byte, error) {
	f.frames = make(chan []byte, 4)
	return f.frames, nil
}

func (f *fakeCameraSource) stop() { f.stops++ }

type failingCameraSource struct{}

func (failingCameraSource) start() (<-chan []byte, error) {
	return nil, errors.New("no camera device")
}
func (failingCameraSource) stop() {}

func readCameraMessage(t *testing.T, reader *bufio.Reader) (uint8, []byte) {
	t.Helper()
	header := make([]byte, cameraHeaderSize)
	if _, err := io.ReadFull(reader, header); err != nil {
		t.Fatalf("read camera header: %v", err)
	}
	if string(header[0:4]) != cameraMagic {
		t.Fatalf("camera magic = %q", header[0:4])
	}
	if header[4] != cameraVersion {
		t.Fatalf("camera version = %d", header[4])
	}
	kind := header[5]
	length := int(binary.LittleEndian.Uint32(header[8:12]))
	payload := make([]byte, length)
	if _, err := io.ReadFull(reader, payload); err != nil {
		t.Fatalf("read camera payload: %v", err)
	}
	return kind, payload
}

func startCameraServe(t *testing.T, source cameraFrameSource) (net.Conn, *bufio.Reader) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		_ = serveCamera(conn, source)
	}()
	conn, err := net.Dial("tcp", listener.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn, bufio.NewReader(conn)
}

func TestServeCameraStreamsOnlyWhileActive(t *testing.T) {
	source := &fakeCameraSource{}
	conn, reader := startCameraServe(t, source)

	if _, err := fmt.Fprintln(conn, `{"type":"start"}`); err != nil {
		t.Fatalf("send start: %v", err)
	}
	if kind, payload := readCameraMessage(t, reader); kind != cameraKindStatus || !bytes.Contains(payload, []byte("streaming")) {
		t.Fatalf("first message = kind %d payload %s", kind, payload)
	}

	frame := bytes.Repeat([]byte{0x40}, cameraFrameBytes)
	source.frames <- frame
	kind, payload := readCameraMessage(t, reader)
	if kind != cameraKindFrame || !bytes.Equal(payload, frame) {
		t.Fatalf("frame message = kind %d, %d bytes", kind, len(payload))
	}

	if _, err := fmt.Fprintln(conn, `{"type":"stop"}`); err != nil {
		t.Fatalf("send stop: %v", err)
	}
	kind, payload = readCameraMessage(t, reader)
	if kind != cameraKindFrame || !bytes.Equal(payload, cameraBlackFrame()) {
		t.Fatalf("stop should send a black frame, got kind %d, %d bytes", kind, len(payload))
	}
	if kind, payload = readCameraMessage(t, reader); kind != cameraKindStatus || !bytes.Contains(payload, []byte("idle")) {
		t.Fatalf("stop status = kind %d payload %s", kind, payload)
	}
	if source.stops == 0 {
		t.Fatal("stop did not stop the frame source")
	}
}

func TestServeCameraReportsUnavailable(t *testing.T) {
	conn, reader := startCameraServe(t, failingCameraSource{})
	if _, err := fmt.Fprintln(conn, `{"type":"start"}`); err != nil {
		t.Fatalf("send start: %v", err)
	}
	kind, payload := readCameraMessage(t, reader)
	if kind != cameraKindStatus || !bytes.Contains(payload, []byte("unavailable")) {
		t.Fatalf("unavailable status = kind %d payload %s", kind, payload)
	}
}

func TestServeCameraCleansUpEndedCaptureAndRestarts(t *testing.T) {
	source := &fakeCameraSource{}
	conn, reader := startCameraServe(t, source)
	fmt.Fprintln(conn, `{"type":"start"}`)
	readCameraMessage(t, reader)
	close(source.frames)
	kind, payload := readCameraMessage(t, reader)
	if kind != cameraKindStatus || !bytes.Contains(payload, []byte("unavailable")) || source.stops != 1 {
		t.Fatalf("ended capture was not cleaned up: %d %s stops=%d", kind, payload, source.stops)
	}
	fmt.Fprintln(conn, `{"type":"start"}`)
	if kind, payload := readCameraMessage(t, reader); kind != cameraKindStatus || !bytes.Contains(payload, []byte("streaming")) {
		t.Fatalf("restart: %d %s", kind, payload)
	}
	source.frames <- cameraBlackFrame()
	if kind, _ := readCameraMessage(t, reader); kind != cameraKindFrame {
		t.Fatal("no frame after restart")
	}
}

func TestCameraHeaderAndBlackFrame(t *testing.T) {
	header := cameraHeader(cameraKindFrame, cameraFrameBytes, 0x01020304)
	if len(header) != cameraHeaderSize {
		t.Fatalf("header size = %d", len(header))
	}
	if !bytes.Equal(header[0:4], []byte("TOCM")) {
		t.Fatalf("magic = %q", header[0:4])
	}
	if header[5] != cameraKindFrame {
		t.Fatalf("kind = %d", header[5])
	}
	if got := binary.LittleEndian.Uint32(header[8:12]); got != cameraFrameBytes {
		t.Fatalf("length = %d", got)
	}
	if got := binary.LittleEndian.Uint32(header[12:16]); got != 0x01020304 {
		t.Fatalf("sequence = %#x", got)
	}

	frame := cameraBlackFrame()
	if len(frame) != cameraFrameBytes {
		t.Fatalf("black frame size = %d", len(frame))
	}
	luma := cameraWidth * cameraHeight
	for i := 0; i < luma; i++ {
		if frame[i] != 16 {
			t.Fatalf("luma[%d] = %d", i, frame[i])
		}
	}
	for i := luma; i < cameraFrameBytes; i++ {
		if frame[i] != 128 {
			t.Fatalf("chroma[%d] = %d", i, frame[i])
		}
	}
}
