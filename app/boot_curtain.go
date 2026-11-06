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
// keys. The window appears once the wallpaper is on screen, or after one of
// the waits below if it never seems to be.
const (
	// After the desktop session starts. The wallpaper took about 6 seconds
	// on the test laptop; a few wallpapers cannot be told from an empty screen.
	curtainPaintWait = 10 * time.Second
	// After Omarchy reports that it is up, for a session that never starts
	// (a login screen, a broken config).
	curtainSessionWait = 60 * time.Second
	// From the start of the boot, whatever else happened.
	curtainBootWait = 180 * time.Second
	// When the setup window starts saying that this is taking a while.
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
}

// liftReason says why the window may be shown now, or "" while it should
// stay invisible. painted is true once the wallpaper has been seen.
func (t bootCurtainTimes) liftReason(now time.Time, painted bool) string {
	switch {
	case painted:
		return "the desktop is on screen"
	case !t.session.IsZero() && now.Sub(t.session) >= curtainPaintWait:
		return "the desktop session started a while ago"
	case !t.ready.IsZero() && t.session.IsZero() && now.Sub(t.ready) >= curtainSessionWait:
		return "no desktop session started"
	case !t.boot.IsZero() && now.Sub(t.boot) >= curtainBootWait:
		return "the boot is taking too long"
	}
	return ""
}

// blankDesktopDue reports whether a window that never showed the desktop has
// been watched long enough to ask the user about it.
func (t bootCurtainTimes) blankDesktopDue(now time.Time, looks int32) bool {
	start := t.session
	if start.IsZero() {
		start = t.ready
	}
	return !start.IsZero() && now.Sub(start) >= blankDesktopWait && looks >= blankDesktopLooks
}

// checkPaint is true once the screen can show something other than boot
// output: firmware text sits on the screen until userspace is up.
func (t bootCurtainTimes) checkPaint() bool { return !t.ready.IsZero() || !t.session.IsZero() }

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
