package main

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"
)

var desktopClipboard atomic.Pointer[clipBridge]

// droppedFiles is one drop request. Point is set when the files were released
// onto the VM window at a known guest-display coordinate.
type droppedFiles struct {
	paths []string
	point []int
}

func droppedFilesEvent(line string) ([]string, *[2]int, bool) {
	if len(line) > 1<<20 {
		return nil, nil, false
	}
	var event struct {
		Event string `json:"event"`
		Data  struct {
			Display int      `json:"display"`
			X       *int     `json:"x"`
			Y       *int     `json:"y"`
			Files   []string `json:"files"`
		} `json:"data"`
	}
	if json.Unmarshal([]byte(line), &event) != nil || event.Event != "DISPLAY_FILE_DROP" || event.Data.Display < 0 || event.Data.Display >= maximumGuestDisplays || len(event.Data.Files) == 0 || len(event.Data.Files) > 1000 {
		return nil, nil, false
	}
	bytes := 0
	for _, path := range event.Data.Files {
		bytes += len(path)
		if !filepath.IsAbs(path) || strings.ContainsRune(path, 0) || bytes > 131072 {
			return nil, nil, false
		}
	}
	var point *[2]int
	if event.Data.X != nil && event.Data.Y != nil {
		point = &[2]int{*event.Data.X, *event.Data.Y}
	}
	return event.Data.Files, point, true
}

func sendDroppedFiles(paths []string) error {
	return sendDroppedFilesAt(paths, nil)
}

func sendDroppedFilesAt(paths []string, point []int) error {
	b := desktopClipboard.Load()
	if b == nil {
		return fmt.Errorf("Omarchy is still starting")
	}
	dropped := droppedFiles{paths: append([]string(nil), paths...)}
	if len(point) == 2 && point[0] >= 0 && point[1] >= 0 {
		dropped.point = append([]int(nil), point...)
	}
	select {
	case b.dropRequests <- dropped:
		return nil
	default:
		return fmt.Errorf("finish another file transfer before dropping more files")
	}
}

func (b *clipBridge) offerDroppedFiles(dropped droppedFiles) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pullConn == nil || !b.transferEnabled {
		return fmt.Errorf("the guest file-transfer service is not connected yet")
	}
	progress := b.progress("Preparing dropped files")
	ticket, err := b.transfers.Offer(progress.ctx, dropped.paths, progress.report)
	if err != nil {
		progress.finish()
		return err
	}
	if len(dropped.point) == 2 {
		ticket.Point = append([]int(nil), dropped.point...)
	}
	data, _ := json.Marshal(ticket)
	frame := encodeClipFrame(clipItem{Kind: clipDrop, Data: data})
	b.pullConn.SetWriteDeadline(time.Now().Add(20 * time.Second))
	n, err := b.pullConn.Write([]byte(frame))
	if err != nil || n != len(frame) {
		b.transfers.Cancel(ticket.ID)
		progress.finish()
		return fmt.Errorf("the guest disconnected before receiving the dropped files")
	}
	go monitorClipboardTransfer(progress, b.transfers, ticket.ID)
	return nil
}
