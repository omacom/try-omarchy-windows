package main

import "testing"

func TestFullscreenCursorClipSingleMonitor(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	tests := []struct {
		name    string
		monitor screenRect
		taskbar screenRect
		want    screenRect
		clip    bool
	}{
		{
			name:    "bottom taskbar leaves its edge for the guest",
			taskbar: screenRect{Left: 0, Top: 1040, Right: 1920, Bottom: 1080},
			want:    screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1079},
			clip:    true,
		},
		{
			name:    "top taskbar",
			taskbar: screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 48},
			want:    screenRect{Left: 0, Top: 1, Right: 1920, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "left taskbar",
			taskbar: screenRect{Left: 0, Top: 0, Right: 64, Bottom: 1080},
			want:    screenRect{Left: 1, Top: 0, Right: 1920, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "right taskbar",
			taskbar: screenRect{Left: 1856, Top: 0, Right: 1920, Bottom: 1080},
			want:    screenRect{Left: 0, Top: 0, Right: 1919, Bottom: 1080},
			clip:    true,
		},
		{
			name:    "negative secondary monitor",
			monitor: screenRect{Left: -1920, Top: 0, Right: 0, Bottom: 1080},
			taskbar: screenRect{Left: -1920, Top: 1040, Right: 0, Bottom: 1080},
			want:    screenRect{Left: -1920, Top: 0, Right: 0, Bottom: 1079},
			clip:    true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := tt.monitor
			if m == (screenRect{}) {
				m = monitor
			}
			got, clip := fullscreenCursorClip(true, m, m, tt.taskbar)
			if got != tt.want || clip != tt.clip {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, %t", got, clip, tt.want, tt.clip)
			}
		})
	}
}

func TestFullscreenCursorClipReleases(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	taskbar := screenRect{Left: 0, Top: 1079, Right: 1920, Bottom: 1080}
	tests := []struct {
		name       string
		fullscreen bool
		virtual    screenRect
		monitor    screenRect
		taskbar    screenRect
	}{
		{name: "not fullscreen releases", fullscreen: false, virtual: monitor, monitor: monitor, taskbar: taskbar},
		{name: "no taskbar releases", fullscreen: true, virtual: monitor, monitor: monitor},
		{
			name: "taskbar on another monitor releases", fullscreen: true,
			virtual: screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1080}, monitor: monitor,
			taskbar: screenRect{Left: 1920, Top: 1079, Right: 3840, Bottom: 1080},
		},
		{name: "empty monitor releases", fullscreen: true, virtual: monitor, monitor: screenRect{}, taskbar: taskbar},
		{name: "empty virtual releases", fullscreen: true, virtual: screenRect{}, monitor: monitor, taskbar: taskbar},
		{
			name: "taskbar not on a monitor edge releases", fullscreen: true,
			virtual: monitor, monitor: monitor, taskbar: screenRect{Left: 100, Top: 500, Right: 200, Bottom: 600},
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got, clip := fullscreenCursorClip(tt.fullscreen, tt.virtual, tt.monitor, tt.taskbar); clip || got != (screenRect{}) {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want no clip", got, clip)
			}
		})
	}
}

func TestFullscreenCursorClipSideBySideMonitors(t *testing.T) {
	virtual := screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1080}
	tests := []struct {
		name    string
		monitor screenRect
	}{
		{name: "VM on the left", monitor: screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}},
		{name: "VM on the right", monitor: screenRect{Left: 1920, Top: 0, Right: 3840, Bottom: 1080}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			taskbar := screenRect{Left: tt.monitor.Left, Top: tt.monitor.Bottom - 1, Right: tt.monitor.Right, Bottom: tt.monitor.Bottom}
			got, clip := fullscreenCursorClip(true, virtual, tt.monitor, taskbar)
			want := screenRect{Left: 0, Top: 0, Right: 3840, Bottom: 1079}
			if got != want || !clip {
				t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, true", got, clip, want)
			}
		})
	}
}

func TestFullscreenCursorClipInternalTaskbarEdgeFallsBackToMonitor(t *testing.T) {
	// The VM's monitor sits above another display, so its bottom taskbar edge
	// is internal to the desktop and the clip falls back to the monitor alone.
	virtual := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 2160}
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	taskbar := screenRect{Left: 0, Top: 1079, Right: 1920, Bottom: 1080}
	got, clip := fullscreenCursorClip(true, virtual, monitor, taskbar)
	want := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1079}
	if got != want || !clip {
		t.Fatalf("fullscreenCursorClip() = %#v, %t; want %#v, true", got, clip, want)
	}
}

func TestWindowFillsMonitor(t *testing.T) {
	monitor := screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1080}
	tests := []struct {
		name    string
		window  screenRect
		monitor screenRect
		want    bool
	}{
		{name: "exact fullscreen", window: monitor, monitor: monitor, want: true},
		{name: "fullscreen with border overshoot", window: screenRect{Left: -8, Top: -8, Right: 1928, Bottom: 1088}, monitor: monitor, want: true},
		{name: "maximized windowed leaves the taskbar", window: screenRect{Left: 0, Top: 0, Right: 1920, Bottom: 1040}, monitor: monitor, want: false},
		{name: "floating window", window: screenRect{Left: 100, Top: 100, Right: 900, Bottom: 700}, monitor: monitor, want: false},
		{name: "empty window", window: screenRect{}, monitor: monitor, want: false},
		{name: "empty monitor", window: monitor, monitor: screenRect{}, want: false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := windowFillsMonitor(tt.window, tt.monitor); got != tt.want {
				t.Fatalf("windowFillsMonitor(%#v, %#v) = %t; want %t", tt.window, tt.monitor, got, tt.want)
			}
		})
	}
}
