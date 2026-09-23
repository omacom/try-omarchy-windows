//go:build windows

package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"time"
)

// The guest chooses a stable Core Audio endpoint ID. The host alone turns
// that ID into an SDL name and writes QEMU's private live-route control file.
// This avoids exposing any host command execution through the virtio port.
type audioBridgeDevice struct {
	UID  string `json:"deviceUID"`
	Name string `json:"name"`
}

type audioBridgeCatalog struct {
	Type              string              `json:"type"`
	Outputs           []audioBridgeDevice `json:"outputs"`
	Inputs            []audioBridgeDevice `json:"inputs"`
	SelectedOutputUID *string             `json:"selectedOutputUID"`
	SelectedInputUID  *string             `json:"selectedInputUID"`
}

type audioBridgeRequest struct {
	Type      string  `json:"type"`
	Direction string  `json:"direction,omitempty"`
	UID       *string `json:"deviceUID"`
}

func audioBridgeDevices(endpoints []audioEndpointInfo, sdlNames []string) []audioBridgeDevice {
	result := make([]audioBridgeDevice, 0, len(endpoints))
	sdlCount := make(map[string]int, len(sdlNames))
	endpointCount := make(map[string]int, len(endpoints))
	for _, name := range sdlNames {
		sdlCount[name]++
	}
	for _, endpoint := range endpoints {
		endpointCount[endpoint.Name]++
	}
	for _, endpoint := range endpoints {
		// SDL routes by name. Never offer an ambiguous or missing name as a
		// stable-ID choice that might silently select the wrong physical device.
		if endpoint.ID != "" && endpoint.Name != "" &&
			sdlCount[endpoint.Name] == 1 && endpointCount[endpoint.Name] == 1 {
			result = append(result, audioBridgeDevice{UID: endpoint.ID, Name: endpoint.Name})
		}
	}
	return result
}

func audioBridgeSelected(id, name string, devices []audioBridgeDevice) *string {
	for _, device := range devices {
		if device.UID == id && id != "" {
			selected := id
			return &selected
		}
	}
	if id == "" && name != "" {
		for _, device := range devices {
			if device.Name == name {
				selected := device.UID
				return &selected
			}
		}
	}
	return nil
}

func currentAudioBridgeCatalog(dataDir, qemu string, microphoneDisabledAtBoot bool) (audioBridgeCatalog, error) {
	catalog := audioBridgeCatalog{Type: "catalog", Outputs: []audioBridgeDevice{}, Inputs: []audioBridgeDevice{}}
	names, err := loadAudioPreferences(dataDir)
	if err != nil {
		return catalog, err
	}
	ids, err := loadAudioEndpoints(dataDir)
	if err != nil {
		return catalog, err
	}
	desktop, err := loadDesktopPreferences(dataDir)
	if err != nil {
		return catalog, err
	}
	endpoints, err := listAudioEndpoints()
	if err != nil {
		return catalog, err
	}
	sdl, err := listAudioDevices(qemu)
	if err != nil {
		return catalog, err
	}
	catalog.Outputs = audioBridgeDevices(endpoints.Output, sdl.Output)
	catalog.SelectedOutputUID = audioBridgeSelected(ids.OutputID, names.Output, catalog.Outputs)
	if !desktop.MicrophoneDisabled && !microphoneDisabledAtBoot {
		catalog.Inputs = audioBridgeDevices(endpoints.Input, sdl.Input)
		catalog.SelectedInputUID = audioBridgeSelected(ids.InputID, names.Input, catalog.Inputs)
	}
	return catalog, nil
}

func parseAudioBridgeRequest(line []byte) (audioBridgeRequest, error) {
	var request audioBridgeRequest
	decoder := json.NewDecoder(bytes.NewReader(line))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		return request, err
	}
	if decoder.Decode(&struct{}{}) != io.EOF {
		return request, fmt.Errorf("trailing audio bridge data")
	}
	switch request.Type {
	case "get-catalog":
		if request.Direction != "" || request.UID != nil {
			return request, fmt.Errorf("invalid catalog request")
		}
	case "select":
		if request.Direction != "output" && request.Direction != "input" {
			return request, fmt.Errorf("invalid audio direction")
		}
		if request.UID != nil && *request.UID == "" {
			return request, fmt.Errorf("empty audio endpoint ID")
		}
	default:
		return request, fmt.Errorf("unknown audio bridge request")
	}
	return request, nil
}

func applyAudioBridgeSelection(dataDir string, catalog audioBridgeCatalog, request audioBridgeRequest, microphoneDisabledAtBoot bool) error {
	devices := catalog.Outputs
	if request.Direction == "input" {
		devices = catalog.Inputs
	}
	name := ""
	id := ""
	if request.UID != nil {
		found := false
		for _, device := range devices {
			if device.UID == *request.UID {
				name, id, found = device.Name, device.UID, true
				break
			}
		}
		if !found {
			return fmt.Errorf("audio endpoint is no longer available")
		}
	}
	names, err := loadAudioPreferences(dataDir)
	if err != nil {
		return err
	}
	ids, err := loadAudioEndpoints(dataDir)
	if err != nil {
		return err
	}
	desktop, err := loadDesktopPreferences(dataDir)
	if err != nil {
		return err
	}
	if request.Direction == "input" {
		if desktop.MicrophoneDisabled || microphoneDisabledAtBoot {
			return fmt.Errorf("microphone access is disabled")
		}
		names.Input, ids.InputID = name, id
	} else {
		names.Output, ids.OutputID = name, id
	}
	if err := saveAudioSelection(dataDir, names, ids); err != nil {
		return err
	}
	return publishSavedAudioRoutes(dataDir, names, desktop.MicrophoneDisabled)
}

func writeAudioBridgeCatalog(conn net.Conn, catalog audioBridgeCatalog) ([]byte, error) {
	message, err := json.Marshal(catalog)
	if err != nil {
		return nil, err
	}
	if len(message) > 65535 {
		return nil, fmt.Errorf("audio catalog is too large")
	}
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return nil, err
	}
	payload := append(message, '\n')
	n, err := conn.Write(payload)
	if err == nil && n != len(payload) {
		err = io.ErrShortWrite
	}
	return message, err
}

func serveAudioBridge(conn net.Conn, dataDir, qemu string, microphoneDisabledAtBoot bool) error {
	defer conn.Close()
	lines := make(chan []byte, 1)
	readErr := make(chan error, 1)
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		scanner := bufio.NewScanner(conn)
		scanner.Buffer(make([]byte, 4096), 8192)
		for scanner.Scan() {
			select {
			case lines <- append([]byte(nil), scanner.Bytes()...):
			case <-stop:
				return
			}
		}
		select {
		case readErr <- scanner.Err():
		case <-stop:
		}
	}()
	ticker := time.NewTicker(3 * time.Second)
	defer ticker.Stop()
	var previous []byte
	refresh := func(force bool) error {
		catalog, err := currentAudioBridgeCatalog(dataDir, qemu, microphoneDisabledAtBoot)
		if err != nil {
			return err
		}
		encoded, err := json.Marshal(catalog)
		if err != nil {
			return err
		}
		if force || !bytes.Equal(previous, encoded) {
			previous, err = writeAudioBridgeCatalog(conn, catalog)
		}
		return err
	}
	if err := refresh(true); err != nil {
		return err
	}
	for {
		select {
		case line := <-lines:
			request, err := parseAudioBridgeRequest(line)
			if err != nil {
				return err
			}
			if request.Type == "select" {
				catalog, err := currentAudioBridgeCatalog(dataDir, qemu, microphoneDisabledAtBoot)
				if err != nil {
					return err
				}
				if err := applyAudioBridgeSelection(dataDir, catalog, request, microphoneDisabledAtBoot); err != nil {
					logf("audio bridge: selection ignored: %v", err)
				}
			}
			if err := refresh(true); err != nil {
				return err
			}
		case <-ticker.C:
			if err := refresh(false); err != nil {
				logf("audio bridge: catalog refresh: %v", err)
			}
		case err := <-readErr:
			if err == nil || errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
	}
}

func runAudioBridge(dataDir, qemu string, microphoneDisabledAtBoot bool) {
	listener, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", audioBridgePort))
	if err != nil {
		fatal("Try Omarchy audio port %d is in use.", audioBridgePort)
	}
	logf("audio: bridge listening on %d", audioBridgePort)
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				logf("audio bridge: accept: %v", err)
				return
			}
			go func() {
				if err := serveAudioBridge(conn, dataDir, qemu, microphoneDisabledAtBoot); err != nil {
					logf("audio bridge: %v", err)
				}
			}()
		}
	}()
}
