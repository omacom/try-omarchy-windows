package main

import (
	"testing"
	"time"
)

func TestBootCurtainLiftsWhenTheDesktopIsDrawn(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	times := bootCurtainTimes{boot: boot, ready: boot.Add(22 * time.Second), session: boot.Add(37 * time.Second)}
	if reason := times.liftReason(boot.Add(38 * time.Second)); reason != "" {
		t.Fatalf("lifted %q one second into the session with nothing drawn", reason)
	}
	if reason := times.liftReason(boot.Add(37*time.Second + curtainDrawWait - time.Second)); reason != "" {
		t.Fatalf("lifted %q before the draw wait ran out", reason)
	}
	if reason := times.liftReason(boot.Add(37*time.Second + curtainDrawWait)); reason == "" {
		t.Fatal("waited past the draw wait for a guest that does not report the desktop")
	}
	times.drawn = boot.Add(50 * time.Second)
	if reason := times.liftReason(boot.Add(50 * time.Second)); reason != "Omarchy reported its desktop drawn" {
		t.Fatalf("stayed up or lifted %q with the desktop drawn", reason)
	}
}

func TestBootCurtainNeverLiftsOnADrawnReportBeforeTheSession(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	early := bootCurtainTimes{boot: boot, ready: boot.Add(20 * time.Second), drawn: boot.Add(25 * time.Second)}
	if reason := early.liftReason(boot.Add(26 * time.Second)); reason != "" {
		t.Fatalf("lifted %q before the session started", reason)
	}
	early.session = boot.Add(30 * time.Second)
	if reason := early.liftReason(boot.Add(30 * time.Second)); reason != "Omarchy reported its desktop drawn" {
		t.Fatalf("got %q once the session started", reason)
	}
}

func TestBootCurtainWaitsForAMissingSessionAndASlowBoot(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	ready := bootCurtainTimes{boot: boot, ready: boot.Add(20 * time.Second)}
	if reason := ready.liftReason(boot.Add(20*time.Second + curtainSessionWait - time.Second)); reason != "" {
		t.Fatalf("lifted %q before the session wait ran out", reason)
	}
	if reason := ready.liftReason(boot.Add(20*time.Second + curtainSessionWait)); reason == "" {
		t.Fatal("kept a login screen or a broken session invisible")
	}
	stuck := bootCurtainTimes{boot: boot}
	if stuck.checkPaint() {
		t.Fatal("read the screen before userspace was up")
	}
	if reason := stuck.liftReason(boot.Add(curtainBootWait - time.Second)); reason != "" {
		t.Fatalf("lifted %q before the boot wait ran out", reason)
	}
	if reason := stuck.liftReason(boot.Add(curtainBootWait)); reason == "" {
		t.Fatal("kept a boot that never came up invisible")
	}
}

func TestBootCurtainSlowNoticeWaitsOutANormalStart(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	// An older guest on the AMD PC: the session at 26 s, no drawn report, so
	// the curtain lifts on the draw wait 66 s after the boot. Every look until
	// then is part of a normal start.
	old := bootCurtainTimes{boot: boot, ready: boot.Add(17 * time.Second), session: boot.Add(26 * time.Second)}
	for at := time.Duration(0); at <= 26*time.Second+curtainDrawWait; at += 250 * time.Millisecond {
		now := boot.Add(at)
		if at < 26*time.Second {
			old.session = time.Time{}
		} else {
			old.session = boot.Add(26 * time.Second)
		}
		if old.liftReason(now) != "" {
			break
		}
		if old.slowStart(now) {
			t.Fatalf("said the start is slow %v after the boot, before a normal lift", at)
		}
	}
	if old.liftReason(boot.Add(26*time.Second+curtainDrawWait)) == "" {
		t.Fatal("the old guest's start never lifted the curtain")
	}
	// The latest session a normal start has still lifts on the draw wait
	// without the notice.
	late := bootCurtainTimes{boot: boot, session: boot.Add(curtainSlowNotice - time.Second)}
	if late.slowStart(late.session.Add(curtainDrawWait - time.Second)) {
		t.Fatal("said a session that started in time is slow")
	}
}

func TestBootCurtainSlowNoticeForSlowStarts(t *testing.T) {
	boot := time.Date(2026, 10, 1, 6, 0, 0, 0, time.UTC)
	if (bootCurtainTimes{}).slowStart(boot.Add(time.Hour)) {
		t.Fatal("said a boot that has not started is slow")
	}
	// No session: firmware, a disk check, a login screen or a broken config.
	stuck := bootCurtainTimes{boot: boot}
	if stuck.slowStart(boot.Add(curtainSlowNotice - time.Second)) {
		t.Fatal("said the start is slow before the notice wait")
	}
	if !stuck.slowStart(boot.Add(curtainSlowNotice)) {
		t.Fatal("kept quiet about a boot with no session after the notice wait")
	}
	ready := bootCurtainTimes{boot: boot, ready: boot.Add(30 * time.Second)}
	at := boot.Add(curtainSlowNotice)
	if ready.liftReason(at) != "" || !ready.slowStart(at) {
		t.Fatal("kept quiet about a session that has not started while the curtain waits for it")
	}
	// A session that started late keeps the notice the setup window already
	// shows until the lift, instead of taking it back.
	late := bootCurtainTimes{boot: boot, ready: boot.Add(50 * time.Second), session: boot.Add(75 * time.Second)}
	for _, now := range []time.Time{boot.Add(75 * time.Second), boot.Add(75*time.Second + curtainDrawWait - time.Second)} {
		if late.liftReason(now) != "" || !late.slowStart(now) {
			t.Fatalf("dropped the notice for a late session at %v", now.Sub(boot))
		}
	}
}

func TestShownWindowRects(t *testing.T) {
	// A 1280x800 monitor with a 48 pixel taskbar, and Windows 11's frame at
	// 100%: 8 pixel borders and a 23 pixel title bar.
	work := screenRect{0, 0, 1280, 752}
	border := screenRect{8, 31, 8, 8}
	// Maximized without its frame the window covered the whole monitor; with
	// its frame its client area is the work area under the title bar.
	outer, client := shownWindowRects(screenRect{0, 0, 1280, 800}, true, work, border)
	if outer != (screenRect{-8, -8, 1288, 760}) || client != (screenRect{0, 23, 1280, 752}) {
		t.Fatalf("maximized: outer %v client %v", outer, client)
	}
	// A second monitor to the left.
	left := screenRect{-1920, 0, 0, 1040}
	if _, client := shownWindowRects(screenRect{}, true, left, border); client != (screenRect{-1920, 23, 0, 1040}) {
		t.Fatalf("maximized on the left monitor: client %v", client)
	}
	// Placed at its remembered framed rectangle: the frame goes back on the
	// same rectangle, so the client area is what lies inside it.
	placed := screenRect{100, 50, 1100, 750}
	outer, client = shownWindowRects(placed, false, work, border)
	if outer != placed || client != (screenRect{108, 81, 1092, 742}) {
		t.Fatalf("placed: outer %v client %v", outer, client)
	}
}

// frame builds a BGRX frame of one colour, with fill painting parts of it.
func frame(width, height int, bgr [3]byte, fill func(x, y int) ([3]byte, bool)) []byte {
	pixels := make([]byte, width*height*4)
	for y := range height {
		for x := range width {
			color := bgr
			if fill != nil {
				if painted, ok := fill(x, y); ok {
					color = painted
				}
			}
			copy(pixels[(y*width+x)*4:], color[:])
		}
	}
	return pixels
}

func TestFramePaintedIgnoresBootOutputAndAnEmptyDesktop(t *testing.T) {
	const width, height = 984, 581
	black := [3]byte{0, 0, 0}
	hyprland := [3]byte{0x11, 0x11, 0x11}
	text := func(x, y int) ([3]byte, bool) {
		// Firmware text: a few lines at the top left.
		return [3]byte{170, 170, 170}, y < 140 && x < 600 && x%7 < 3 && y%16 < 10
	}
	bar := func(x, y int) ([3]byte, bool) {
		// The bar drawn before the wallpaper, a notification and the pointer.
		switch {
		case y < 26:
			return [3]byte{30, 27, 26}, true
		case x > 700 && x < 960 && y > 40 && y < 120:
			return [3]byte{60, 40, 40}, true
		case x > 480 && x < 500 && y > 280 && y < 300:
			return [3]byte{255, 255, 255}, true
		}
		return [3]byte{}, false
	}
	for name, pixels := range map[string][]byte{
		"black":                  frame(width, height, black, nil),
		"firmware text":          frame(width, height, black, text),
		"Hyprland's background":  frame(width, height, hyprland, nil),
		"bar and a notification": frame(width, height, hyprland, bar),
	} {
		if framePainted(pixels, width, height, width*4) {
			t.Errorf("%s counted as the desktop", name)
		}
	}
}

func TestFramePaintedSeesWallpapers(t *testing.T) {
	const width, height = 984, 581
	gradient := func(x, y int) ([3]byte, bool) {
		return [3]byte{byte(x / 8), byte(y / 4), byte((x + y) / 10)}, true
	}
	// White dots on black over the middle third, like Vantablack's hands.
	dots := func(x, y int) ([3]byte, bool) {
		return [3]byte{230, 230, 230}, y > 190 && y < 400 && x%6 < 2 && y%6 < 2
	}
	// A solid theme colour with a small logo in the middle (catppuccin).
	logo := func(x, y int) ([3]byte, bool) {
		return [3]byte{200, 200, 200}, x > 440 && x < 540 && y > 250 && y < 330
	}
	for name, pixels := range map[string][]byte{
		"photo":       frame(width, height, [3]byte{}, gradient),
		"dotted":      frame(width, height, [3]byte{}, dots),
		"plain theme": frame(width, height, [3]byte{0x2e, 0x1e, 0x1e}, logo),
		"light plain": frame(width, height, [3]byte{0xf5, 0xf0, 0xf2}, logo),
		"tokyo night": frame(width, height, [3]byte{0x26, 0x1b, 0x1a}, nil),
	} {
		if !framePainted(pixels, width, height, width*4) {
			t.Errorf("%s not counted as the desktop", name)
		}
	}
	// Rows padded past the picture.
	stride := width*4 + 64
	padded := make([]byte, stride*height)
	for y := range height {
		for x := range width {
			c, _ := gradient(x, y)
			copy(padded[y*stride+x*4:], c[:])
		}
	}
	if !framePainted(padded, width, height, stride) {
		t.Error("padded rows not read as the desktop")
	}
}

func TestFramePaintedRejectsShortBuffers(t *testing.T) {
	if framePainted(make([]byte, 100), 984, 581, 984*4) {
		t.Fatal("read past a short buffer")
	}
	if framePainted(nil, 10, 10, 40) {
		t.Fatal("judged a frame smaller than the sample grid")
	}
	// Wide and short: the sample blocks must stay inside the frame.
	framePainted(make([]byte, 3840*20*4), 3840, 20, 3840*4)
}

func TestBlankDesktopDue(t *testing.T) {
	start := time.Unix(1000, 0)
	if (bootCurtainTimes{}).blankDesktopDue(start.Add(time.Hour), blankDesktopLooks, 0) {
		t.Fatal("asked before Omarchy reported ready")
	}
	ready := bootCurtainTimes{boot: start, ready: start}
	if ready.blankDesktopDue(start.Add(blankDesktopWait-time.Second), blankDesktopLooks, 0) {
		t.Fatal("asked before the wait")
	}
	if !ready.blankDesktopDue(start.Add(blankDesktopWait), blankDesktopLooks, 0) {
		t.Fatal("no session: the wait counts from ready")
	}
	session := bootCurtainTimes{boot: start, ready: start, session: start.Add(time.Minute)}
	if session.blankDesktopDue(start.Add(blankDesktopWait), blankDesktopLooks, 0) {
		t.Fatal("the wait counts from the session start")
	}
	if !session.blankDesktopDue(start.Add(time.Minute+blankDesktopWait), blankDesktopLooks, 0) {
		t.Fatal("expected to ask after the session wait")
	}
	// A window kept minimized was never looked at, so it is not blank.
	if session.blankDesktopDue(start.Add(time.Hour), blankDesktopLooks-1, 0) {
		t.Fatal("asked without enough looks at the visible window")
	}
}

func TestDesktopStreakNeedsTheDesktopToStay(t *testing.T) {
	start := time.Unix(1000, 0)
	var streak desktopStreak
	// The wallpaper flashes for a second, then the window goes black (#358).
	if streak.look(start, true) || streak.look(start.Add(time.Second), true) {
		t.Fatal("a one-second flash counted as the desktop")
	}
	for i := 2; i < 30; i++ {
		if streak.look(start.Add(time.Duration(i)*5*time.Second), false) {
			t.Fatal("a black window counted as the desktop")
		}
	}
	// A desktop drawn later still counts once it stays.
	later := start.Add(5 * time.Minute)
	if streak.look(later, true) || streak.look(later.Add(5*time.Second), true) || streak.look(later.Add(desktopSteadyWait-time.Second), true) {
		t.Fatal("counted before the steady wait")
	}
	if !streak.look(later.Add(desktopSteadyWait), true) {
		t.Fatal("a desktop that stayed on screen did not count")
	}
	streak.reset()
	if streak.look(later.Add(desktopSteadyWait+time.Second), true) {
		t.Fatal("reset kept the earlier streak")
	}
}

func TestDesktopStreakIgnoresTimeWithoutLooks(t *testing.T) {
	start := time.Unix(1000, 0)
	var streak desktopStreak
	// One painted look, then nobody looked for a minute (a hidden window or
	// a capture held up): that minute does not count.
	streak.look(start, true)
	if streak.look(start.Add(time.Minute), true) {
		t.Fatal("unobserved time counted toward the streak")
	}
	restored := start.Add(time.Minute)
	for i := 1; i < 3; i++ {
		if streak.look(restored.Add(time.Duration(i)*5*time.Second), true) {
			t.Fatal("counted before the steady wait after the gap")
		}
	}
	if !streak.look(restored.Add(desktopSteadyWait), true) {
		t.Fatal("looks after the gap did not count")
	}
}

func TestFramelessPlacementIsTheShownClientArea(t *testing.T) {
	// The AMD PC's frame at 100%: 8 pixel borders and a 31 pixel title bar
	// with the top border. A saved 200,100,1480,860 window shows a 1264x721
	// client area; the hidden frameless window must get exactly that, not one
	// frame less (1248x682, #413).
	frame := screenRect{8, 31, 8, 8}
	client := frameless(screenRect{200, 100, 1480, 860}, frame)
	if client != (screenRect{208, 131, 1472, 852}) || client.width() != 1264 || client.height() != 721 {
		t.Fatalf("client %v (%dx%d)", client, client.width(), client.height())
	}
	if _, shown := shownWindowRects(screenRect{200, 100, 1480, 860}, false, screenRect{}, frame); shown != client {
		t.Fatalf("frameless %v and shownWindowRects %v disagree", client, shown)
	}
}

func TestHiddenWindowNeverCoversItsMonitor(t *testing.T) {
	monitor := hostMonitor{Bounds: screenRect{0, 0, 1920, 1080}, Work: screenRect{0, 0, 1920, 1032}, Primary: true}
	frame := screenRect{8, 31, 8, 8}
	// A frameless window Windows maximized, or QEMU sized to the guest's
	// 1920x1080 display at 0,0: it goes to a maximized window's client area.
	for _, now := range []screenRect{{0, 0, 1920, 1080}, {-8, -8, 1928, 1088}} {
		outer, target, move := hiddenWindowOffMonitor(now, monitor, frame, screenRect{}, false)
		if !move || target != (screenRect{0, 23, 1920, 1032}) || target.covers(monitor.Bounds) {
			t.Fatalf("%v: move %v to %v", now, move, target)
		}
		// The lift frames it as a maximized window.
		if outer != (screenRect{-8, -8, 1928, 1040}) || frameless(outer, frame) != target {
			t.Fatalf("%v: framed at %v around %v", now, outer, target)
		}
	}
	// One the curtain placed goes back to its own client area.
	placed := screenRect{208, 131, 1472, 852}
	if _, target, move := hiddenWindowOffMonitor(screenRect{0, 0, 1920, 1080}, monitor, frame, placed, true); !move || target != placed {
		t.Fatalf("placed window: move %v to %v", move, target)
	}
	// The usual hidden rectangles are left alone.
	for _, now := range []screenRect{{0, 23, 1920, 1032}, {0, 0, 1920, 1001}, {208, 131, 1472, 852}, {-8, -8, 1928, 1040}} {
		if _, _, move := hiddenWindowOffMonitor(now, monitor, frame, screenRect{}, false); move {
			t.Fatalf("moved %v", now)
		}
	}
	// After the display mode changed behind the curtain (1280x800 to
	// 1024x768), Windows maximized the frameless window over the new mode.
	small := hostMonitor{Bounds: screenRect{0, 0, 1024, 768}, Work: screenRect{0, 0, 1024, 720}, Primary: true}
	if outer, target, move := hiddenWindowOffMonitor(small.Bounds, small, frame, screenRect{}, false); !move ||
		target != (screenRect{0, 23, 1024, 720}) || outer != (screenRect{-8, -8, 1032, 728}) {
		t.Fatalf("after a mode change: move %v to %v framed at %v", move, target, outer)
	}
	// A second monitor to the left.
	left := hostMonitor{Bounds: screenRect{-1920, 0, 0, 1080}, Work: screenRect{-1920, 0, 0, 1040}}
	if _, target, move := hiddenWindowOffMonitor(left.Bounds, left, frame, screenRect{}, false); !move || target != (screenRect{-1920, 23, 0, 1040}) {
		t.Fatalf("left monitor: move %v to %v", move, target)
	}
}

func TestCurtainFrameBackSkipsAWindowThatWentFullScreen(t *testing.T) {
	const (
		normal     = 0x16CA0000 // QEMU's window as SDL creates it, held
		fullscreen = 0x96000000 // after SDL made it a full screen popup
		frame      = 0x00CA0000
	)
	if got := curtainFrameBack(frame, normal, normal&^frame); got != frame {
		t.Fatalf("windowed window: frame back %#x, want %#x", got, frame)
	}
	if got := curtainFrameBack(frame, normal, fullscreen); got != 0 {
		t.Fatalf("window that went full screen behind the curtain: frame back %#x, want 0", got)
	}
	// Full screen from the start, as with a runtime that does not hold
	// its windows: whatever the curtain took off goes back.
	if got := curtainFrameBack(0x00020000, fullscreen|0x00020000, fullscreen); got != 0x00020000 {
		t.Fatalf("full screen window: frame back %#x, want 0x20000", got)
	}
}
