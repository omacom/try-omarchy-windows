package main

import (
	"slices"
	"sync"
	"sync/atomic"
	"time"
)

// While Omarchy boots, its window shows nothing worth looking at: firmware
// text, "Display output is not active", then black and Hyprland's empty grey
// until the shell draws the wallpaper. The launcher keeps the window
// invisible for that time and the setup window stays up with the starter
// keys. The window appears once the guest's agent says the desktop is drawn
// (the shell's wallpaper and bar are up), or after one of the waits below if
// it never does: an older guest, or a shell that did not start. The hidden
// window's own picture cannot be read (boot_curtain_windows.go).
const (
	// After the desktop session starts, for a guest that does not report
	// the drawn desktop: an older guest, or a shell that never maps its bar.
	// With CPU rendering in a nested Windows VM the report came 18 to 23
	// seconds into the session, and the wallpaper was drawn about 23 seconds
	// in on a Ryzen Embedded PC; a dark wallpaper adds the guest's 8 second
	// wait. Lifting earlier would show a black desktop.
	curtainDrawWait = 40 * time.Second
	// After Omarchy reports that it is up, for a session that never starts
	// (a login screen, a broken config).
	curtainSessionWait = 60 * time.Second
	// From the start of the boot, whatever else happened.
	curtainBootWait = 180 * time.Second
	// A boot whose desktop session has not started this long after the boot
	// started is slower than usual, and the setup window says so. The session
	// started about 25 to 37 seconds in on a Ryzen Embedded PC and in a
	// nested VM with CPU rendering.
	curtainSlowNotice = 60 * time.Second
)

// After the curtain lifts, a GPU boot keeps looking at the window until it
// has kept showing the desktop for a while. Some drivers run the guest but never present its frames
// on Windows (#358), and that boot must not count as working GPU rendering.
const (
	desktopCheckInterval = 5 * time.Second
	// From the start of the desktop session until the user is asked about a
	// window that has not shown the desktop.
	blankDesktopWait = 2 * time.Minute
	// Looks at the visible window needed before asking, so a window kept
	// minimized is never taken for a blank one.
	blankDesktopLooks = 6
	// How long every look must keep showing the desktop before the boot
	// counts as having shown it. Some cards draw the wallpaper for about a
	// second and then leave the window black (#358).
	desktopSteadyWait = 15 * time.Second
	// Looks further apart than this start the streak over, so time nobody
	// looked at the window (hidden, or a capture held up) never counts.
	desktopLookGap = 12 * time.Second
)

var (
	// desktopSeen is set once this boot's window has kept showing the desktop
	// for desktopSteadyWait.
	desktopSeen atomic.Bool
	// desktopLooks counts this boot's looks at the visible window.
	desktopLooks atomic.Int32
	desktopShown desktopStreak
)

// desktopStreak follows the looks at the window that showed the desktop
// without a break.
type desktopStreak struct {
	mu          sync.Mutex
	since, last time.Time
}

// look records one look and reports whether the desktop has been on screen
// for desktopSteadyWait. A look without the desktop starts over.
func (s *desktopStreak) look(now time.Time, painted bool) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !painted {
		s.since, s.last = time.Time{}, time.Time{}
		return false
	}
	if s.since.IsZero() || now.Sub(s.last) > desktopLookGap {
		s.since = now
	}
	s.last = now
	return now.Sub(s.since) >= desktopSteadyWait
}

// reset forgets the streak, for a new boot or a window that was hidden.
func (s *desktopStreak) reset() { s.look(time.Time{}, false) }

type bootCurtainTimes struct {
	boot, ready, session time.Time
	// drawn is when the guest's agent said the desktop is drawn.
	drawn time.Time
	// hostCapture.blindTime when ready and session were recorded, so time
	// the screen could not be seen is left out of the blank-desktop wait.
	readyBlind, sessionBlind time.Duration
}

// liftReason says why the window may be shown now, or "" while it should
// stay invisible. The drawn desktop counts only once the session has started:
// before that, the window would show an empty desktop.
func (t bootCurtainTimes) liftReason(now time.Time) string {
	switch {
	case !t.session.IsZero() && !t.drawn.IsZero():
		return "Omarchy reported its desktop drawn"
	case !t.session.IsZero() && now.Sub(t.session) >= curtainDrawWait:
		return "the desktop session started a while ago and no drawn desktop was reported"
	case !t.ready.IsZero() && t.session.IsZero() && now.Sub(t.ready) >= curtainSessionWait:
		return "no desktop session started"
	case !t.boot.IsZero() && now.Sub(t.boot) >= curtainBootWait:
		return "the boot is taking too long"
	}
	return ""
}

// curtainFrameWait bounds how long the setup window stays up after the
// curtain lifted, waiting for QEMU to show a window with its first frame
// (curtainRevealDone). QEMU itself shows the window a second after it came
// on screen without a frame.
const curtainFrameWait = 2 * time.Second

// curtainRevealDone reports whether the lift can hand over from the setup
// window to Omarchy's: every window QEMU still has is no longer cloaked, or
// curtainFrameWait has passed since the lift. cloaked holds one entry per
// window; a window whose state cannot be read counts as not cloaked.
func curtainRevealDone(cloaked []bool, lifted, now time.Time) (done, late bool) {
	if !now.Before(lifted.Add(curtainFrameWait)) {
		return true, slices.Contains(cloaked, true)
	}
	return !slices.Contains(cloaked, true), false
}

// lateReveals holds the windows a lift handed over from while QEMU still had
// them cloaked, after curtainFrameWait. The shell leaves a cloaked window
// alone, so their taskbar buttons, full screen marks and the focus go again
// once QEMU shows them. tab says the curtain had taken the button off.
type lateReveals struct {
	mu      sync.Mutex
	windows map[uintptr]bool
}

func (l *lateReveals) add(hwnd uintptr, tab bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.windows == nil {
		l.windows = map[uintptr]bool{}
	}
	l.windows[hwnd] = tab
}

// take reports, once, whether hwnd waits for its shell steps, and whether
// its taskbar button goes back.
func (l *lateReveals) take(hwnd uintptr) (waiting, tab bool) {
	l.mu.Lock()
	defer l.mu.Unlock()
	tab, waiting = l.windows[hwnd]
	delete(l.windows, hwnd)
	return waiting, tab
}

// focusAfterLateReveal reports whether a window QEMU showed after the hand
// over takes the focus: only if the user has not gone to another app since,
// so the foreground is nothing, the launcher or QEMU.
func focusAfterLateReveal(foreground uintptr, ours func(uintptr) bool) bool {
	return foreground == 0 || ours(foreground)
}

// slowStart reports whether the setup window should say that Omarchy is
// taking longer than usual. Once the session has started, the curtain lifts
// on the drawn report or curtainDrawWait later, both normal for the guest
// that is running, so only a session that started late keeps the notice up:
// it has been showing since curtainSlowNotice, and turning it off then would
// make the start look normal again. A session that never starts lifts the
// curtain on curtainSessionWait or curtainBootWait, and those starts are slow.
func (t bootCurtainTimes) slowStart(now time.Time) bool {
	if t.boot.IsZero() {
		return false
	}
	if t.session.IsZero() {
		return now.Sub(t.boot) >= curtainSlowNotice
	}
	return t.session.Sub(t.boot) >= curtainSlowNotice
}

// blankDesktopDue reports whether a window that never showed the desktop has
// been watched long enough to ask the user about it. blind is
// hostCapture.blindTime(now): time the screen could not be seen (display
// off, session locked) does not count toward the wait.
func (t bootCurtainTimes) blankDesktopDue(now time.Time, looks int32, blind time.Duration) bool {
	start, startBlind := t.session, t.sessionBlind
	if start.IsZero() {
		start, startBlind = t.ready, t.readyBlind
	}
	return !start.IsZero() && now.Sub(start)-(blind-startBlind) >= blankDesktopWait && looks >= blankDesktopLooks
}

// checkPaint is true once the screen can show something other than boot
// output: firmware text sits on the screen until userspace is up.
func (t bootCurtainTimes) checkPaint() bool { return !t.ready.IsZero() || !t.session.IsZero() }

// shownWindowRects is where a hidden window will be once the curtain gives
// its frame back, and the client area it will have there. QEMU's guest takes
// the size of the client area, so the curtain gives the frameless window
// that client area while the desktop is drawn: otherwise the guest has to
// redraw it at the new size as the curtain lifts, which takes seconds with
// CPU rendering. now is the window's rectangle, frame the size of its frame
// on each side (Left, Top, Right, Bottom) and work the work area of its
// monitor. A maximized window with its frame covers the work area, with its
// side and bottom borders past its edges.
func shownWindowRects(now screenRect, maximized bool, work, frame screenRect) (outer, client screenRect) {
	if maximized {
		outer = screenRect{work.Left - frame.Left, work.Top - frame.Bottom, work.Right + frame.Right, work.Bottom + frame.Bottom}
	} else {
		outer = now
	}
	client = screenRect{outer.Left + frame.Left, outer.Top + frame.Top, outer.Right - frame.Right, outer.Bottom - frame.Bottom}
	return outer, client
}

// frameless takes a framed window rectangle to the client area it has, for a
// window the curtain keeps without its frame.
func frameless(outer, frame screenRect) screenRect {
	return screenRect{outer.Left + frame.Left, outer.Top + frame.Top, outer.Right - frame.Right, outer.Bottom - frame.Bottom}
}

// covers reports whether r covers all of bounds. A window that covers its
// monitor can count as a full-screen app to Windows, which then changes how
// the display is driven: the hidden window must never do that (#413).
func (r screenRect) covers(bounds screenRect) bool {
	return r.Left <= bounds.Left && r.Top <= bounds.Top && r.Right >= bounds.Right && r.Bottom >= bounds.Bottom
}

// fullscreenGrown is the rectangle a full screen window on monitor gets
// instead of the monitor's own: one row or column larger, off the screen,
// and edge names the side it went to. Graphics drivers present an OpenGL
// window that exactly covers its monitor as an exclusive full screen one,
// and anything drawn over it, the pointer included, makes the display leave
// that mode and come back, which shows as a black flicker.
//
// The extra row goes above the monitor, else a column on the left or the
// right, where no other monitor (others) would show it. It never goes on
// the side the taskbar is docked on, the side where the work area falls
// short of the monitor, or hiddenTaskbar, the side of an auto-hidden one
// (taskbarEdge): Windows then no longer treats the window as full screen
// and leaves the taskbar over it, as a row below does with a taskbar at the
// bottom. When every other side has a neighbour, the first of them is used
// and one line of the guest's edge shows on that monitor.
func fullscreenGrown(monitor hostMonitor, others []screenRect, hiddenTaskbar string) (grown screenRect, edge string) {
	b, w := monitor.Bounds, monitor.Work
	edges := []struct {
		name    string
		strip   screenRect
		taskbar bool
	}{
		{"top", screenRect{b.Left, b.Top - 1, b.Right, b.Top}, w.Top > b.Top || hiddenTaskbar == "top"},
		{"left", screenRect{b.Left - 1, b.Top, b.Left, b.Bottom}, w.Left > b.Left || hiddenTaskbar == "left"},
		{"right", screenRect{b.Right, b.Top, b.Right + 1, b.Bottom}, w.Right < b.Right || hiddenTaskbar == "right"},
		{"bottom", screenRect{b.Left, b.Bottom, b.Right, b.Bottom + 1}, w.Bottom < b.Bottom || hiddenTaskbar == "bottom"},
	}
	pick := -1
	for i, e := range edges {
		if e.taskbar {
			continue
		}
		if pick < 0 {
			pick = i
		}
		free := true
		for _, other := range others {
			if overlaps(e.strip, other) {
				free = false
				break
			}
		}
		if free {
			pick = i
			break
		}
	}
	if pick < 0 {
		pick = 0
	}
	e := edges[pick]
	return screenRect{min(b.Left, e.strip.Left), min(b.Top, e.strip.Top), max(b.Right, e.strip.Right), max(b.Bottom, e.strip.Bottom)}, e.name
}

// taskbarEdge is the side of monitor a taskbar at bar is docked on.
func taskbarEdge(bar, monitor screenRect) string {
	switch {
	case bar.width() <= 0 || bar.height() <= 0:
		return ""
	case bar.Left <= monitor.Left && bar.Right >= monitor.Right && bar.Top <= monitor.Top:
		return "top"
	case bar.Left <= monitor.Left && bar.Right >= monitor.Right:
		return "bottom"
	case bar.Left <= monitor.Left:
		return "left"
	}
	return "right"
}

// grownFullscreen reports whether r is a monitor's rectangle grown by one
// line, as fullscreenGrown makes it: still that monitor's full screen
// window, not one to put back on the monitor.
func grownFullscreen(r screenRect, monitors []hostMonitor) bool {
	for _, m := range monitors {
		if r.covers(m.Bounds) && r.width()-m.Bounds.width()+r.height()-m.Bounds.height() == 1 {
			return true
		}
	}
	return false
}

// hiddenWindowOffMonitor says where a hidden, frameless window that covers its
// monitor goes instead: the client area it will have when shown (client, if
// the curtain placed it), else that of a maximized window on the monitor's
// work area, with outer the framed rectangle around it then (left zero for a
// placed window, whose own is kept). Windows maximizes a frameless window
// over the whole monitor, also when the display mode or the work area
// changes while the curtain is up. ok is false when the window does not
// cover the monitor.
func hiddenWindowOffMonitor(now screenRect, monitor hostMonitor, frame, client screenRect, knowClient bool) (outer, target screenRect, ok bool) {
	if !now.covers(monitor.Bounds) {
		return screenRect{}, screenRect{}, false
	}
	if !knowClient {
		outer, client = shownWindowRects(now, true, monitor.Work, frame)
	}
	return outer, client, client != now && !client.covers(monitor.Bounds)
}

// curtainFrameBack is the frame style the curtain gives back at the lift: the
// bits it took off (frame) when the window had style hidden, now that it has
// style now. A window QEMU holds is created with a frame even for a full
// screen start, and SDL makes it a full screen popup only once it is shown,
// behind the curtain. That window has no frame to give back: with one, its
// client area and the guest's display shrink and the guest redraws (#415).
func curtainFrameBack(frame, hidden, now uintptr) uintptr {
	const popup = 0x80000000 // WS_POPUP
	if now&popup != 0 && hidden&popup == 0 {
		return 0
	}
	return frame
}

// A hidden window's taskbar button is taken off every curtainTabInterval
// until the lift. The taskbar applies the window's new identity
// asynchronously and puts the button back when a delete came first: on a
// test VM, 24 of 30 windows kept their button after an immediate delete,
// none when the delete came 500 ms later or was repeated after 1.5 s. A
// button put back shows until the next delete, so the deletes keep a steady
// short pace for the whole curtain.
const curtainTabInterval = 250 * time.Millisecond

// curtainTabDeleteDue reports whether the curtain should take a hidden
// window's taskbar button off now, given when it last did.
func curtainTabDeleteDue(last, now time.Time) bool {
	return last.IsZero() || now.Sub(last) >= curtainTabInterval
}

// fullscreenMarks remembers which windows the launcher told the taskbar are
// full screen (ITaskbarList2::MarkFullscreenWindow), so it tells it only on
// a change and can take every mark back.
type fullscreenMarks struct {
	mu     sync.Mutex
	marked map[uintptr]bool
}

// differs reports whether hwnd's mark must change for a window that is, or
// is not, full screen now.
func (m *fullscreenMarks) differs(hwnd uintptr, fullscreen bool) bool {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.marked[hwnd] != fullscreen
}

// set records that the taskbar took hwnd's mark. Until then differs keeps
// reporting the change, so a mark the taskbar refused is tried again.
func (m *fullscreenMarks) set(hwnd uintptr, fullscreen bool) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if !fullscreen {
		delete(m.marked, hwnd)
		return
	}
	if m.marked == nil {
		m.marked = map[uintptr]bool{}
	}
	m.marked[hwnd] = true
}

// forget drops a destroyed window, whose mark went with it.
func (m *fullscreenMarks) forget(hwnd uintptr) {
	m.mu.Lock()
	delete(m.marked, hwnd)
	m.mu.Unlock()
}

// clear returns every marked window, to be unmarked, and forgets them.
func (m *fullscreenMarks) clear() []uintptr {
	m.mu.Lock()
	defer m.mu.Unlock()
	var all []uintptr
	for hwnd := range m.marked {
		all = append(all, hwnd)
	}
	m.marked = nil
	return all
}

var (
	bootCurtainMu    sync.Mutex
	bootCurtainState bootCurtainTimes
)

func curtainTimes() bootCurtainTimes {
	bootCurtainMu.Lock()
	defer bootCurtainMu.Unlock()
	return bootCurtainState
}

func updateCurtainTimes(update func(*bootCurtainTimes)) {
	bootCurtainMu.Lock()
	update(&bootCurtainState)
	bootCurtainMu.Unlock()
}

// The paint check samples the guest's picture on a grid, leaving out the top
// of the window, where the bar is drawn before the wallpaper. Each sample is
// the average of a small block, so a wallpaper drawn in fine dots or lines
// still reads as drawn.
const (
	paintColumns = 24
	paintRows    = 14
)

// framePainted reports whether a captured frame (32-bit BGRX rows) shows
// more than boot output or an empty desktop. Before the wallpaper, the frame
// is one flat colour, black or Hyprland's #111111, with at most some text, a
// pointer, the bar or a notification on it.
func framePainted(pixels []byte, width, height, stride int) bool {
	if width < paintColumns || height < paintRows || stride < width*4 || len(pixels) < stride*height {
		return false
	}
	block := max(1, min(width/128, height/paintRows))
	lums := make([]int, 0, paintColumns*paintRows)
	var reds, greens, blues []int
	for row := range paintRows {
		top := min(height-block, height*(80+870*(2*row+1)/(2*paintRows))/1000)
		for column := range paintColumns {
			left := min(width-block, width*(40+920*(2*column+1)/(2*paintColumns))/1000)
			r, g, b := 0, 0, 0
			for y := top; y < top+block; y++ {
				for x := left; x < left+block; x++ {
					offset := y*stride + x*4
					b, g, r = b+int(pixels[offset]), g+int(pixels[offset+1]), r+int(pixels[offset+2])
				}
			}
			area := block * block
			r, g, b = r/area, g/area, b/area
			lums = append(lums, (r*299+g*587+b*114)/1000)
			reds, greens, blues = append(reds, r), append(greens, g), append(blues, b)
		}
	}
	median := func(values []int) int {
		sorted := slices.Clone(values)
		slices.Sort(sorted)
		return sorted[len(sorted)/2]
	}
	middle := median(lums)
	varied := 0
	for _, lum := range lums {
		if lum-middle > 6 || middle-lum > 6 {
			varied++
		}
	}
	// A notification on an empty desktop stays under this; the dotted
	// wallpapers come in just over it.
	if varied*100 >= len(lums)*15 {
		return true
	}
	// A flat screen in some other colour is a plain wallpaper.
	r, g, b := median(reds), median(greens), median(blues)
	near := func(level int) bool {
		return max(abs(r-level), abs(g-level), abs(b-level)) <= 6
	}
	return !near(0) && !near(0x11)
}
