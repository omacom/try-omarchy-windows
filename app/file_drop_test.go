package main

import (
	"bufio"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestDroppedFilesEvents(t *testing.T) {
	path := filepath.Join(t.TempDir(), "file")
	for _, sample := range []struct {
		event   string
		display int
		paths   []string
		valid   bool
	}{
		{"DISPLAY_FILE_DROP", 0, []string{path}, true},
		{"DISPLAY_FILE_DROP", -1, []string{path}, false},
		{"DISPLAY_FILE_DROP", maximumGuestDisplays, []string{path}, false},
		{"OTHER", 0, []string{path}, false},
		{"DISPLAY_FILE_DROP", 0, []string{"relative"}, false},
		{"DISPLAY_FILE_DROP", 0, nil, false},
	} {
		data, _ := json.Marshal(map[string]any{"event": sample.event, "data": map[string]any{"display": sample.display, "files": sample.paths}})
		_, ok := droppedFilesEvent(string(data))
		if ok != sample.valid {
			t.Fatal(sample, ok)
		}
	}
}

func TestDroppedFilesCarryTheDropPoint(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	source := filepath.Join(t.TempDir(), "drop")
	os.WriteFile(source, []byte("dropped contents"), 0600)
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	guest.SetDeadline(time.Now().Add(3 * time.Second))
	bridge := &clipBridge{transfers: service, transferEnabled: true, pullConn: host}
	done := make(chan error, 1)
	go func() {
		done <- bridge.offerDroppedFiles(droppedFiles{paths: []string{source}, point: []int{640, 360}})
	}()
	line, err := bufio.NewReader(guest).ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	item, ok := decodeClipFrame(line)
	if !ok || item.Kind != clipDrop {
		t.Fatal("drop changed clipboard protocol", line)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	var ticket fileTransferTicket
	if err := json.Unmarshal(item.Data, &ticket); err != nil {
		t.Fatal(err)
	}
	if len(ticket.Point) != 2 || ticket.Point[0] != 640 || ticket.Point[1] != 360 {
		t.Fatalf("ticket point = %v", ticket.Point)
	}
}

func TestDroppedFilesUseSeparateFrameAndCancelCapability(t *testing.T) {
	service := newFileTransferService(t.TempDir(), clipboardTransferLimits)
	defer service.Close()
	source := filepath.Join(t.TempDir(), "drop")
	os.WriteFile(source, []byte("dropped contents"), 0600)
	host, guest := net.Pipe()
	defer host.Close()
	defer guest.Close()
	guest.SetDeadline(time.Now().Add(3 * time.Second))
	bridge := &clipBridge{transfers: service, transferEnabled: true, pullConn: host}
	done := make(chan error, 1)
	go func() { done <- bridge.offerDroppedFiles(droppedFiles{paths: []string{source}}) }()
	line, err := bufio.NewReader(guest).ReadString('\n')
	if err != nil {
		t.Fatal(err)
	}
	item, ok := decodeClipFrame(line)
	if !ok || item.Kind != clipDrop {
		t.Fatal("drop changed clipboard protocol", line)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	var ticket fileTransferTicket
	json.Unmarshal(item.Data, &ticket)
	server := httptest.NewServer(service)
	defer server.Close()
	request, _ := http.NewRequest("DELETE", server.URL+"/download/"+ticket.Token, nil)
	response, err := server.Client().Do(request)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != 204 {
		t.Fatal(response.StatusCode)
	}
	if _, ok := service.Status(ticket.ID); ok {
		t.Fatal("cancelled drop still available")
	}
}
