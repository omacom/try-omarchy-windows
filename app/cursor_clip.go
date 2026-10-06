package main

// fullscreenCursorClip returns the rectangle that keeps an SDL cursor grab
// away from an auto-hidden taskbar without confining the pointer to the VM's
// monitor. The clip starts from the whole virtual screen so the pointer can
// still cross to other monitors, and only the edge where the taskbar sits is
// pulled in by one pixel so the pointer can reach that edge and reveal it.
func fullscreenCursorClip(fullscreen bool, virtual, monitor, taskbar screenRect) (screenRect, bool) {
	if !fullscreen || virtual.width() <= 0 || virtual.height() <= 0 || monitor.width() <= 0 || monitor.height() <= 0 {
		return screenRect{}, false
	}
	if taskbar.width() <= 0 || taskbar.height() <= 0 || !overlaps(taskbar, monitor) {
		return screenRect{}, false
	}

	spansWidth := taskbar.Left <= monitor.Left && taskbar.Right >= monitor.Right
	spansHeight := taskbar.Top <= monitor.Top && taskbar.Bottom >= monitor.Bottom
	top := spansWidth && taskbar.Top <= monitor.Top && taskbar.Bottom > monitor.Top
	bottom := spansWidth && taskbar.Bottom >= monitor.Bottom && taskbar.Top < monitor.Bottom
	left := spansHeight && taskbar.Left <= monitor.Left && taskbar.Right > monitor.Left
	right := spansHeight && taskbar.Right >= monitor.Right && taskbar.Left < monitor.Right
	if !top && !bottom && !left && !right {
		return screenRect{}, false
	}

	// A taskbar edge in the middle of the desktop (the VM's monitor touches
	// another display there) is not a true screen edge. A virtual-screen clip
	// would let the pointer cross the shared edge instead of stopping where the
	// taskbar reveals, so confine the pointer to the VM's monitor instead.
	internal := top && monitor.Top > virtual.Top ||
		bottom && monitor.Bottom < virtual.Bottom ||
		left && monitor.Left > virtual.Left ||
		right && monitor.Right < virtual.Right
	clip := virtual
	if internal {
		clip = monitor
	}
	if top {
		clip.Top++
	}
	if bottom {
		clip.Bottom--
	}
	if left {
		clip.Left++
	}
	if right {
		clip.Right--
	}
	if clip.width() <= 0 || clip.height() <= 0 {
		return screenRect{}, false
	}
	return clip, true
}

// windowFillsMonitor reports whether a window rectangle covers its monitor's
// full bounds. SDL's borderless fullscreen fills the monitor, while an
// ordinary windowed window (or a maximized one, which stops at the work area
// above a visible taskbar) does not.
func windowFillsMonitor(window, monitor screenRect) bool {
	if window.width() <= 0 || window.height() <= 0 || monitor.width() <= 0 || monitor.height() <= 0 {
		return false
	}
	return window.Left <= monitor.Left && window.Top <= monitor.Top &&
		window.Right >= monitor.Right && window.Bottom >= monitor.Bottom
}

func overlaps(a, b screenRect) bool {
	return a.Left < b.Right && a.Right > b.Left && a.Top < b.Bottom && a.Bottom > b.Top
}
