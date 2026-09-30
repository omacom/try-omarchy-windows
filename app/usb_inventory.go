package main

import (
	"bytes"
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"time"
)

// A stopped VM has no monitor. A diskless, paused helper lists devices through
// the selected runtime's libusb without claiming any device or booting a guest.
func inventoryUSBDevices(ctx context.Context, qemu string) ([]usbDevice, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	base, err := platformQMPControlDirectory()
	if err != nil {
		return nil, err
	}
	dir, err := os.MkdirTemp(filepath.Dir(base), "tom-u-")
	if err != nil {
		return nil, err
	}
	socket := filepath.Join(dir, "u.sock")
	defer func() { os.Remove(socket); os.Remove(dir) }()
	if len([]byte(socket)) > 103 {
		return nil, fmt.Errorf("private USB inventory path is too long")
	}
	cmd := exec.CommandContext(ctx, qemu, "-machine", "none", "-nodefaults", "-display", "none", "-S", "-qmp", "unix:"+qemuOptionValue(filepath.ToSlash(socket))+",server=on,wait=off")
	configureDiskTool(cmd)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("USB inventory needs the installed graphics runtime: %w", err)
	}
	done := make(chan struct{})
	var processErr error
	go func() { processErr = cmd.Wait(); close(done) }()
	defer func() { cmd.Process.Kill(); <-done }()
	var client *qmpClient
	for {
		conn, e := (&net.Dialer{}).DialContext(ctx, "unix", socket)
		if e == nil {
			client, err = newQMPClient(ctx, conn)
			break
		}
		select {
		case <-done:
			return nil, fmt.Errorf("USB inventory runtime exited: %v", processErr)
		case <-ctx.Done():
			return nil, fmt.Errorf("USB inventory unavailable: %w", ctx.Err())
		case <-time.After(50 * time.Millisecond):
		}
	}
	if err != nil {
		return nil, err
	}
	defer client.Close()
	return (usbBroker{client}).hostDevices(ctx)
}
