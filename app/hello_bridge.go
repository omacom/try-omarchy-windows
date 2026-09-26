package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"time"
)

// serveHelloBridge handles the QEMU authentication character device. Each
// valid request gets exactly one response from approve; a denial sends the
// guest on to its ordinary password prompt. Malformed requests close the
// connection without a reply.
func serveHelloBridge(conn net.Conn, approve func(helloRequest) helloResponse) error {
	defer conn.Close()
	reader := bufio.NewReaderSize(conn, helloMaximumLineBytes)
	for {
		line, err := reader.ReadSlice('\n')
		if len(line) == 0 && (errors.Is(err, io.EOF) || errors.Is(err, io.ErrClosedPipe) || errors.Is(err, net.ErrClosed)) {
			return nil
		}
		if err != nil {
			return fmt.Errorf("read authentication request: %w", err)
		}
		request, err := parseHelloRequest(line)
		if err != nil {
			return fmt.Errorf("invalid authentication request: %w", err)
		}
		response := approve(request)
		encoded, err := json.Marshal(response)
		if err != nil {
			return err
		}
		if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
			return err
		}
		for remaining := append(encoded, '\n'); len(remaining) > 0; {
			count, err := conn.Write(remaining)
			if err != nil {
				return fmt.Errorf("write authentication response: %w", err)
			}
			if count <= 0 {
				return io.ErrShortWrite
			}
			remaining = remaining[count:]
		}
	}
}
