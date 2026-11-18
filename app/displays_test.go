package main

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

func TestMultipleDisplayDeviceIncludesEnabledOutputs(t *testing.T) {
	for _, gpu := range []bool{false, true} {
		cfg := &config{displays: 3, useGpu: gpu, displayWidth: 1440, displayHeight: 900}
		var device struct {
			Driver  string `json:"driver"`
			Outputs []struct {
				Name   string `json:"name"`
				Width  int    `json:"xres"`
				Height int    `json:"yres"`
			} `json:"outputs"`
			Count   int    `json:"max_outputs"`
			HostMem uint64 `json:"hostmem"`
		}
		if err := json.Unmarshal([]byte(displayDevice(cfg, 512<<20)), &device); err != nil {
			t.Fatal(err)
		}
		if device.Count != 3 || len(device.Outputs) != 3 {
			t.Fatal("missing outputs")
		}
		if gpu && device.HostMem != 512<<20 {
			t.Fatal("GPU JSON hostmem must be an integer byte count")
		}
		for i, output := range device.Outputs {
			if output.Width != 1440 || output.Height != 900 || output.Name == "" {
				t.Fatalf("invalid display %d", i)
			}
		}
		if gpu && device.Driver != "virtio-vga-gl" || !gpu && device.Driver != "virtio-gpu-pci" {
			t.Fatal("changed render path")
		}
	}
}

// QEMU's SDL frontend titles a grabbed window like this on every focus change.
// The launcher must recognise it and replace it with its own title.
func TestQEMUGrabTitleBecomesOurs(t *testing.T) {
	for title, want := range map[string]string{
		"QEMU (" + appTitle + "-0) - Press Ctrl-Alt-G to exit grab":       appTitle,
		"QEMU (" + appTitle + "-0)":                                       appTitle,
		"QEMU (" + appTitle + "-1) [Stopped]":                             appTitle + " display 2",
		"QEMU (" + appTitle + "-2) - Press Ctrl-Alt-Shift-G to exit grab": appTitle + " display 3",
	} {
		index, ok := displayIndexFromTitle(title)
		if !ok || displayWindowTitle(index) != want {
			t.Fatalf("%q -> %d %v %q", title, index, ok, displayWindowTitle(index))
		}
	}
	for index := 0; index < maximumGuestDisplays; index++ {
		if _, ok := displayIndexFromTitle(displayWindowTitle(index)); ok {
			t.Fatalf("our own title for display %d looks like QEMU's", index)
		}
	}
}

func TestDisplayTitleGrabState(t *testing.T) {
	tests := []struct {
		title string
		want  bool
	}{
		{"QEMU (" + appTitle + "-0) - Press Ctrl-Alt-G to exit grab", true},
		{"QEMU (" + appTitle + "-1) - Press Ctrl-Alt-Shift-G to exit grab", true},
		{"QEMU (" + appTitle + "-0)", false},
		{"QEMU (" + appTitle + "-1) [Stopped]", false},
		{appTitle, false},
	}
	for _, tt := range tests {
		if got := displayTitleGrabbed(tt.title); got != tt.want {
			t.Errorf("displayTitleGrabbed(%q) = %t; want %t", tt.title, got, tt.want)
		}
	}
}

func TestDisplayIdentityAndIndependentPlacements(t *testing.T) {
	index, ok := displayIndexFromTitle("QEMU (" + appTitle + "-2) [Stopped]")
	if !ok || index != 2 {
		t.Fatal("lost console identity")
	}
	for _, title := range []string{appTitle, "QEMU error", "QEMU (" + appTitle + "--1)", "QEMU (" + appTitle + "-16)"} {
		if _, ok := displayIndexFromTitle(title); ok {
			t.Fatalf("accepted %q", title)
		}
	}
	dir := t.TempDir()
	monitors := []screenRect{{0, 0, 1920, 1080}, {1920, 0, 3840, 1080}}
	for index := 0; index < 3; index++ {
		p := initialDisplayPlacement(index, monitors)
		if p == nil || !p.usable(monitors) {
			t.Fatal("unusable placement")
		}
		if err := saveDisplayPlacement(dir, index, *p); err != nil {
			t.Fatal(err)
		}
	}
	first, _ := loadDisplayPlacement(dir, 0)
	second, _ := loadDisplayPlacement(dir, 1)
	if first.Normal == second.Normal {
		t.Fatal("display placements overwrite each other")
	}
	if second.usable(monitors[:1]) {
		t.Fatal("restored placement on a removed monitor")
	}
}

func TestDisplaySettingsValidateAndRestore(t *testing.T) {
	for _, count := range []int{-1, maximumGuestDisplays + 1} {
		if err := (settings{Displays: count}).validate(); err == nil {
			t.Fatal("accepted unsupported display count")
		}
	}
	cfg := &config{}
	var forwards forwardList
	key := ""
	if err := applySettings(cfg, settings{Displays: 3}, map[string]bool{}, &forwards, &key); err != nil || cfg.displays != 3 {
		t.Fatalf("did not apply displays: %v", err)
	}
	cfg.displays = 2
	if err := applySettings(cfg, settings{Displays: 3}, map[string]bool{"displays": true}, &forwards, &key); err != nil || cfg.displays != 2 {
		t.Fatal("ignored explicit display count")
	}
	dir, archive := backupFixture(t)
	placement := windowPlacement{Normal: screenRect{1920, 0, 3200, 900}}
	if err := saveDisplayPlacement(dir, 1, placement); err != nil {
		t.Fatal(err)
	}
	if err := writeVMBackup(dir, archive); err != nil {
		t.Fatal(err)
	}
	restored := filepath.Join(t.TempDir(), "restored")
	if err := restoreVMBackup(archive, restored); err != nil {
		t.Fatal(err)
	}
	got, err := loadDisplayPlacement(restored, 1)
	if err != nil || got == nil || got.Normal != placement.Normal {
		t.Fatal("backup lost the second display's layout")
	}
}

func TestRunningInstanceWindowsAreRecognised(t *testing.T) {
	for _, c := range []struct {
		class, title string
		want         bool
	}{
		{"SDL_app", appTitle, true},
		{"SDL_app", appTitle + " display 2", true},
		{"SDL_app", "QEMU (" + appTitle + "-0) - Press Ctrl-Alt-G to exit grab", true},
		{"TryOmarchySetup", appTitle, true},
		{"SDL_app", "Some other SDL game", false},
		{"Chrome_WidgetWin_1", appTitle + " - Chromium", false},
	} {
		if got := isRunningInstanceWindow(c.class, c.title); got != c.want {
			t.Errorf("%q %q: %v", c.class, c.title, got)
		}
	}
}

func tabletArgs(args []string) []string {
	var tablets []string
	for i, arg := range args {
		if i > 0 && args[i-1] == "-device" && (strings.HasPrefix(arg, "virtio-tablet-pci") || strings.Contains(arg, `"driver":"virtio-tablet-pci"`)) {
			tablets = append(tablets, arg)
		}
	}
	return tablets
}

// One display keeps the single unbound tablet it always had.
func TestOneDisplayKeepsItsSingleTablet(t *testing.T) {
	for _, displays := range []int{0, 1} {
		for _, gpu := range []bool{false, true} {
			cfg := &config{vmDir: "/vm", guestDir: "/guest", disk: "/vm/disk.raw", diskFormat: "raw",
				memMiB: 4096, audio: "none", displays: displays, useGpu: gpu}
			args := buildQemuArgs(cfg, "root=/dev/vda")
			if tablets := tabletArgs(args); !slices.Equal(tablets, []string{"virtio-tablet-pci"}) {
				t.Fatalf("displays=%d gpu=%v: tablets %q", displays, gpu, tablets)
			}
			want := "-device virtio-keyboard-pci -device virtio-tablet-pci -device virtio-net-pci,netdev=n0,romfile= "
			if joined := strings.Join(args, " "); !strings.Contains(joined, want) {
				t.Fatalf("displays=%d gpu=%v: input devices changed: %s", displays, gpu, joined)
			}
		}
	}
}

// QEMU reports a window's pointer as 0 to 100% of that window. Each display
// needs its own tablet, bound to its head and named for the guest.
func TestEachDisplayGetsATabletOnItsHead(t *testing.T) {
	for _, displays := range []int{2, 3, maximumGuestDisplays} {
		for _, gpu := range []bool{false, true} {
			cfg := &config{vmDir: "/vm", guestDir: "/guest", disk: "/vm/disk.raw", diskFormat: "raw",
				memMiB: 4096, audio: "none", displays: displays, useGpu: gpu}
			args := buildQemuArgs(cfg, "root=/dev/vda")
			tablets := tabletArgs(args)
			if len(tablets) != displays {
				t.Fatalf("displays=%d gpu=%v: %d tablets", displays, gpu, len(tablets))
			}
			serials := map[string]bool{}
			for head, tablet := range tablets {
				var device struct {
					Driver  string `json:"driver"`
					Display string `json:"display"`
					Head    *int   `json:"head"`
					Serial  string `json:"serial"`
				}
				if err := json.Unmarshal([]byte(tablet), &device); err != nil {
					t.Fatalf("displays=%d gpu=%v: tablet %d is not JSON: %q", displays, gpu, head, tablet)
				}
				if device.Driver != "virtio-tablet-pci" || device.Display != "gpu0" || device.Head == nil || *device.Head != head ||
					device.Serial != fmt.Sprintf("try-omarchy-head-%d", head) {
					t.Fatalf("displays=%d gpu=%v: tablet %d is %q", displays, gpu, head, tablet)
				}
				serials[device.Serial] = true
			}
			if len(serials) != displays {
				t.Fatalf("displays=%d: serials repeat", displays)
			}
			// A tablet binds to its head when it is created. QEMU creates
			// JSON devices after all others, in order, so the GPU has to be
			// JSON and come first.
			gpuAt, tabletAt := slices.Index(args, displayDevice(cfg, gpuHostMem(cfg.memMiB, cfg.hostTotalMiB))), slices.Index(args, tablets[0])
			if gpuAt < 0 || tabletAt < gpuAt || !strings.HasPrefix(args[gpuAt], "{") {
				t.Fatalf("displays=%d gpu=%v: display device at %d, first tablet at %d", displays, gpu, gpuAt, tabletAt)
			}
			var device struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal([]byte(args[gpuAt]), &device); err != nil || device.ID != guestDisplayDeviceID {
				t.Fatalf("displays=%d gpu=%v: display device id %q (%v)", displays, gpu, device.ID, err)
			}
		}
	}
}

func TestDragEventsNameTheFirstHeadOnlyWithSeveralTablets(t *testing.T) {
	step := pointerStep{x: 100, y: 32767, button: 1}
	single := `{"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":100}},{"type":"abs","data":{"axis":"y","value":32767}},{"type":"btn","data":{"down":true,"button":"left"}}]}}`
	if got := pointerStepCommand(step, false); got != single {
		t.Fatalf("one display: %s", got)
	}
	if got := pointerReleaseCommand(false); got != `{"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}` {
		t.Fatalf("one display release: %s", got)
	}
	for _, command := range []string{pointerStepCommand(step, true), pointerReleaseCommand(true)} {
		var parsed struct {
			Execute   string `json:"execute"`
			Arguments struct {
				Device string            `json:"device"`
				Head   *int              `json:"head"`
				Events []json.RawMessage `json:"events"`
			} `json:"arguments"`
		}
		if err := json.Unmarshal([]byte(command), &parsed); err != nil {
			t.Fatal(err)
		}
		if parsed.Execute != "input-send-event" || parsed.Arguments.Device != guestDisplayDeviceID ||
			parsed.Arguments.Head == nil || *parsed.Arguments.Head != 0 || len(parsed.Arguments.Events) == 0 {
			t.Fatalf("several displays: %s", command)
		}
	}
}
