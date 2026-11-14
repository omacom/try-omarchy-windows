// Used through Go's file overlay; never installed into the launcher sources.
package main

import (
	"encoding/json"
	"os"
	"testing"
)

func TestNativeGPUArgumentExport(t *testing.T) {
	path := os.Getenv("OMARCHY_NATIVE_GPU_ARGS_OUTPUT")
	if path == "" {
		t.Skip("requires an explicit diagnostic output file")
	}
	type profile struct {
		Name      string   `json:"name"`
		Supported bool     `json:"supported"`
		Arguments []string `json:"arguments"`
	}
	var profiles []profile
	for _, name := range []string{"cursor-off", "cursor-on", "cpu", "venus", "multiple-displays"} {
		cfg := &config{
			useGpu: true, memMiB: 6144, cpus: 4, hostTotalMiB: 16384,
			displays: 1, audio: "none", diskFormat: "qcow2",
			disk: `C:\Native GPU Fixture\root.qcow2`, guestDir: `C:\Native GPU Fixture\guest`,
			vmDir: `C:\Native GPU Fixture\vm`, qmpDir: `C:\Native GPU Fixture\controls`,
		}
		switch name {
		case "cursor-on":
			cfg.hostCursor = true
		case "cpu":
			cfg.useGpu = false
		case "venus":
			cfg.venus = true
		case "multiple-displays":
			cfg.displays = 2
		}
		profiles = append(profiles, profile{name, name == "cursor-off" || name == "cursor-on",
			buildQemuArgs(cfg, "root=/dev/vda quiet")})
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.NewEncoder(f).Encode(profiles); err != nil {
		f.Close()
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
}
