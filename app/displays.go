package main

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"
)

const maximumGuestDisplays = 16

func guestDisplayCount(count int) int {
	if count == 0 {
		return 1
	}
	return count
}

// displayDevice describes the guest GPU. Blob resources and hostmem stay on
// without Venus: virgl maps host-visible buffers through the same window.
func displayDevice(cfg *config, hostmem uint64) string {
	if guestDisplayCount(cfg.displays) == 1 {
		if cfg.useGpu {
			device := "virtio-vga-gl,blob=on,hostmem=" + strconv.FormatUint(hostmem, 10)
			if cfg.venus {
				device += ",venus=on"
			}
			return device
		}
		return "virtio-gpu-pci,id=gpu0"
	}
	width, height := cfg.displayWidth, cfg.displayHeight
	if width < 640 || width > 8192 {
		width = 1920
	}
	if height < 480 || height > 8192 {
		height = 1080
	}
	outputs := make([]map[string]any, guestDisplayCount(cfg.displays))
	for i := range outputs {
		outputs[i] = map[string]any{"name": fmt.Sprintf("Omarchy %d", i+1), "xres": width, "yres": height}
	}
	device := map[string]any{"driver": "virtio-gpu-pci", "id": "gpu0", "max_outputs": len(outputs), "outputs": outputs}
	if cfg.useGpu {
		device["driver"] = "virtio-vga-gl"
		device["blob"] = true
		device["hostmem"] = hostmem
		if cfg.venus {
			device["venus"] = true
		}
	}
	data, _ := json.Marshal(device)
	return string(data)
}

// displayWindowTitle is the title of the window showing guest display index.
func displayWindowTitle(index int) string {
	if index == 0 {
		return appTitle
	}
	return fmt.Sprintf("%s display %d", appTitle, index+1)
}

// isRunningInstanceWindow reports whether a window with this class and title
// is a running Try Omarchy: its VM window (under our title or QEMU's), or the
// setup window shown while Omarchy prepares and starts.
func isRunningInstanceWindow(class, title string) bool {
	if class == "TryOmarchySetup" {
		return true
	}
	if _, ok := displayIndexFromTitle(title); ok {
		return class == "SDL_app"
	}
	return class == "SDL_app" && (title == appTitle || strings.HasPrefix(title, appTitle+" display "))
}

func displayIndexFromTitle(title string) (int, bool) {
	prefix := "QEMU (" + appTitle + "-"
	if !strings.HasPrefix(title, prefix) {
		return 0, false
	}
	tail := strings.TrimPrefix(title, prefix)
	end := strings.IndexByte(tail, ')')
	if end < 1 {
		return 0, false
	}
	index, err := strconv.Atoi(tail[:end])
	return index, err == nil && index >= 0 && index < maximumGuestDisplays
}

// displayTitleGrabbed reports QEMU's SDL grab state from the title it writes
// when the grab changes. The launcher rewrites that title immediately, so this
// is read by the window-event hook before the replacement.
func displayTitleGrabbed(title string) bool {
	return strings.Contains(title, " to exit grab")
}

func displayPlacementFilename(index int) string {
	if index == 0 {
		return windowPlacementFilename
	}
	return fmt.Sprintf("window-placement-display-%d.json", index+1)
}

func initialDisplayPlacement(index int, monitors []screenRect) *windowPlacement {
	if len(monitors) == 0 {
		return nil
	}
	monitor := monitors[index%len(monitors)]
	if index < len(monitors) {
		return &windowPlacement{Normal: screenRect{monitor.Left + 24, monitor.Top + 24, monitor.Right - 24, monitor.Bottom - 24}, Maximized: true}
	}
	// Extra virtual displays on one physical monitor remain independently usable.
	offset := int32(24 * (1 + index%5))
	return &windowPlacement{Normal: screenRect{monitor.Left + offset, monitor.Top + offset, monitor.Right - 24, monitor.Bottom - 24}}
}
