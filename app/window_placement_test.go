package main

import (
	"testing"
	"time"
)

func TestWindowPlacementRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if p, err := loadWindowPlacement(dir); err != nil || p != nil {
		t.Fatalf("missing file should be nil: %v %v", p, err)
	}
	want := windowPlacement{Normal: screenRect{100, 50, 1380, 850}, Maximized: false, SavedAt: time.Date(2026, 9, 5, 0, 0, 0, 0, time.UTC)}
	if err := saveWindowPlacement(dir, want); err != nil {
		t.Fatal(err)
	}
	got, err := loadWindowPlacement(dir)
	if err != nil || got == nil || !got.sameAs(&want) || got.Schema != windowPlacementSchemaNow {
		t.Fatalf("round trip: %+v %v", got, err)
	}
	// Measured on Windows 11: a 1000x620 window has a 984x581 client area.
	if w, h := (&windowPlacement{Normal: screenRect{150, 40, 1150, 660}}).consoleSize(); w != 984 || h != 581 {
		t.Fatalf("console size %dx%d", w, h)
	}
	if w, h := got.consoleSize(); w != 1280-windowFrameWidth || h != 800-windowFrameHeight {
		t.Fatalf("console size %dx%d", w, h)
	}
}

func TestWindowPlacementUsableOnlyOnAPresentDisplay(t *testing.T) {
	primary := screenRect{0, 0, 1920, 1080}
	second := screenRect{1920, 0, 3840, 1080}
	cases := []struct {
		name   string
		p      *windowPlacement
		mons   []screenRect
		usable bool
	}{
		{"nil", nil, []screenRect{primary}, false},
		{"on primary", &windowPlacement{Normal: screenRect{100, 100, 1300, 900}}, []screenRect{primary}, true},
		{"on second monitor present", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary, second}, true},
		{"on second monitor gone", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary}, false},
		{"mostly off screen", &windowPlacement{Normal: screenRect{1800, 1000, 3000, 1800}}, []screenRect{primary}, false},
		{"too small", &windowPlacement{Normal: screenRect{0, 0, 300, 200}}, []screenRect{primary}, false},
		{"caption above the screen", &windowPlacement{Normal: screenRect{-200, -50, 1000, 700}}, []screenRect{primary}, false},
		{"partly off the left edge", &windowPlacement{Normal: screenRect{-200, 50, 1000, 700}}, []screenRect{primary}, true},
	}
	for _, c := range cases {
		if got := c.p.usable(c.mons); got != c.usable {
			t.Errorf("%s: usable=%v, want %v", c.name, got, c.usable)
		}
	}
}

func TestOnlyTheUsersMovesAreRemembered(t *testing.T) {
	placed := &windowPlacement{Normal: screenRect{150, 40, 1150, 660}}
	grown := &windowPlacement{Normal: screenRect{150, 40, 1598, 800}}
	maximized := &windowPlacement{Normal: placed.Normal, Maximized: true}
	cases := []struct {
		name                      string
		now, last, target         *windowPlacement
		userMoved, dragging, held bool
		want                      placementStep
	}{
		{"unchanged", placed, placed, placed, false, false, true, placementKeep},
		{"QEMU grew it while the guest boots", grown, placed, placed, false, false, true, placementRestore},
		{"moved once the guest follows the window (Snap, tools)", grown, placed, placed, false, false, false, placementSave},
		{"unchanged once the guest follows the window", placed, placed, placed, false, false, false, placementKeep},
		{"the user dragged it", grown, placed, placed, true, false, true, placementSave},
		{"the user is still dragging", grown, placed, placed, false, true, true, placementKeep},
		{"the user maximized it", maximized, placed, placed, false, false, true, placementSave},
		{"maximized windows are not held", maximized, maximized, maximized, false, false, true, placementKeep},
		{"minimized", nil, placed, placed, false, false, true, placementKeep},
		{"nothing remembered yet", placed, nil, nil, false, false, true, placementSave},
	}
	for _, c := range cases {
		if got := nextPlacementStep(c.now, c.last, c.target, c.userMoved, c.dragging, c.held, true); got != c.want {
			t.Errorf("%s: %v, want %v", c.name, got, c.want)
		}
	}
}

func TestTheCurtainDoesNotChangeTheRememberedPlacement(t *testing.T) {
	placed := &windowPlacement{Normal: screenRect{150, 40, 1150, 660}}
	booted := &windowPlacement{Normal: screenRect{150, 40, 1174, 808}}
	maximized := &windowPlacement{Normal: placed.Normal, Maximized: true}
	cases := []struct {
		name        string
		now, target *windowPlacement
		want        *windowPlacement
	}{
		{"QEMU resized it while hidden", booted, placed, placed},
		{"QEMU un-maximized it while hidden", placed, maximized, maximized},
		{"unchanged", placed, placed, nil},
		{"nothing remembered", booted, nil, nil},
		{"minimized", nil, placed, nil},
	}
	for _, c := range cases {
		if got := placementAfterCurtain(c.now, c.target); got != c.want {
			t.Errorf("%s: %v, want %v", c.name, got, c.want)
		}
	}
	// Once put back, the guest following the window does not save the
	// boot size over it.
	if got := nextPlacementStep(placed, placed, placed, false, false, false, true); got != placementKeep {
		t.Errorf("after the curtain: %v, want keep", got)
	}
}

func TestRememberedWindowsAreFittedToTheirDisplay(t *testing.T) {
	laptop := screenRect{0, 0, 1366, 720} // 1366x768 minus the taskbar
	second := screenRect{1366, 0, 3286, 1040}
	cases := []struct {
		name string
		in   screenRect
		want screenRect
	}{
		{"inside stays put", screenRect{150, 40, 1150, 660}, screenRect{150, 40, 1150, 660}},
		{"grown past the right edge (v0.7.1)", screenRect{150, 40, 1536, 792}, screenRect{-8, 0, 1374, 728}},
		{"off the right edge moves back", screenRect{700, 40, 1700, 660}, screenRect{374, 40, 1374, 660}},
		{"snapped left keeps its invisible border", screenRect{-7, 0, 690, 727}, screenRect{-7, 0, 690, 727}},
		{"on the second display", screenRect{1500, 100, 2700, 900}, screenRect{1500, 100, 2700, 900}},
	}
	for _, c := range cases {
		got := (&windowPlacement{Normal: c.in}).fittedTo([]screenRect{laptop, second})
		if got.Normal != c.want {
			t.Errorf("%s: %+v, want %+v", c.name, got.Normal, c.want)
		}
	}
	if (*windowPlacement)(nil).fittedTo([]screenRect{laptop}) != nil {
		t.Fatal("nil placement")
	}
}

func TestConsoleAndPlacementUseTheSameMonitor(t *testing.T) {
	secondary := hostMonitor{Name: "secondary", Bounds: screenRect{-2560, 0, 0, 1440}, Work: screenRect{-2560, 0, 0, 1400}}
	primary := hostMonitor{Name: "primary", Primary: true, Bounds: screenRect{0, 0, 1920, 1080}, Work: screenRect{0, 0, 1920, 1040}}
	input := []hostMonitor{secondary, primary}
	monitors := primaryFirstMonitors(input)
	if monitors[0] != primary || input[0] != secondary {
		t.Fatal("primary ordering must not mutate the enumeration")
	}
	dir := t.TempDir()
	p := displayPlacement(dir, 0, monitors)
	if !p.Maximized || placementMonitor(p.screenPlacement(monitors).Normal, monitors) != primary {
		t.Fatalf("default placement: %+v", p)
	}
	if w, h := windowedConsoleSize(p, monitors); w != 1920 || h != 1009 {
		t.Fatalf("primary console: %dx%d", w, h)
	}
	p = &windowPlacement{Normal: screenRect{-2400, 100, -1000, 1000}, Maximized: true}
	if err := saveWindowPlacement(dir, *p); err != nil {
		t.Fatal(err)
	}
	p = displayPlacement(dir, 0, monitors)
	if w, h := windowedConsoleSize(p, monitors); w != 2560 || h != 1369 {
		t.Fatalf("remembered maximized secondary console: %dx%d", w, h)
	}
	p.Maximized = false
	if w, h := windowedConsoleSize(p, monitors); w != 1384 || h != 861 {
		t.Fatalf("remembered restored console: %dx%d", w, h)
	}
}

func TestPlacementCaptionUsesWorkAreaCoordinates(t *testing.T) {
	monitors := []hostMonitor{{Bounds: screenRect{-1920, -1080, 0, 0}, Work: screenRect{-1880, -1040, 0, 0}}}
	p := &windowPlacement{Normal: screenRect{-1800, -1000, -600, -200}}
	screen := p.screenPlacement(monitors)
	if screen.Normal != (screenRect{-1760, -960, -560, -160}) || !usablePlacement(p, monitors) {
		t.Fatalf("workspace to screen: %+v", screen)
	}
	if !screen.workspacePlacement(monitors).sameAs(p) {
		t.Fatal("workspace conversion failed to round trip")
	}
	p.Normal = screenRect{-1800, -1100, -600, -300}
	if usablePlacement(p, monitors) {
		t.Fatal("accepted an inaccessible caption above the work area")
	}
	fitted := fitPlacement(p, monitors)
	if !usablePlacement(fitted, monitors) || fitted.screenPlacement(monitors).Normal.Top != -1040 {
		t.Fatalf("caption did not fit: %+v", fitted)
	}
	if !fitPlacement(fitted, monitors).sameAs(fitted) {
		t.Fatal("fitting caused taskbar coordinate creep")
	}
}

func TestLivePlacementRepairAfterWorkAreaChanges(t *testing.T) {
	monitors := []hostMonitor{{Bounds: screenRect{0, 0, 1920, 1080}, Work: screenRect{0, 0, 1920, 960}}}
	now := &windowPlacement{Normal: screenRect{100, 100, 1800, 1040}}
	repaired := repairDisplayPlacement(now, 0, monitors)
	if repaired.Normal.Bottom != 968 || repaired.Maximized {
		t.Fatalf("work area repair: %+v", repaired)
	}
	offscreen := &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}
	for _, maximized := range []bool{false, true} {
		offscreen.Maximized = maximized
		repaired = repairDisplayPlacement(offscreen, 0, monitors)
		if repaired.Maximized != maximized || !usablePlacement(repaired, monitors) {
			t.Fatalf("removed monitor repair changed state: %+v", repaired)
		}
	}
	if repairDisplayPlacement(nil, 0, monitors) != nil || repairDisplayPlacement(now, 0, nil) != nil {
		t.Fatal("repaired an unavailable window or desktop")
	}
}

func TestFullscreenPlacementRestoresTheWindowedPlacement(t *testing.T) {
	before := &windowPlacement{Normal: screenRect{Left: 100, Top: 80, Right: 1100, Bottom: 700}, Maximized: true}
	if restore, handled := fullscreenPlacement(false, true, before); !handled || restore != nil {
		t.Fatalf("entering fullscreen = %v, %t; want nothing remembered", restore, handled)
	}
	if restore, handled := fullscreenPlacement(true, true, before); !handled || restore != nil {
		t.Fatalf("staying fullscreen = %v, %t; want nothing remembered", restore, handled)
	}
	if restore, handled := fullscreenPlacement(true, false, before); !handled || restore != before {
		t.Fatalf("leaving fullscreen = %v, %t; want the placement from before", restore, handled)
	}
	if restore, handled := fullscreenPlacement(true, false, nil); !handled || restore != nil {
		t.Fatalf("leaving fullscreen with nothing remembered = %v, %t; want no restore", restore, handled)
	}
	if _, handled := fullscreenPlacement(false, false, before); handled {
		t.Fatal("ordinary windowed pass was handled; want the usual placement rules")
	}
}

func TestTheWayIntoFullscreenIsNotSaved(t *testing.T) {
	maximized := &windowPlacement{Normal: screenRect{Left: 24, Top: 24, Right: 1256, Bottom: 728}, Maximized: true}
	// SDL has taken the frame and the maximized state off and not yet
	// covered the monitor (acceptance 2026-11-18, saved as the window's
	// size and put back un-maximized on every later leave).
	midway := &windowPlacement{Normal: screenRect{Left: -8, Top: -8, Right: 1288, Bottom: 760}}
	for _, held := range []bool{false, true} {
		if got := nextPlacementStep(midway, maximized, maximized, false, false, held, false); got != placementKeep {
			t.Fatalf("held=%v: %v, want keep", held, got)
		}
	}
	if got := nextPlacementStep(midway, maximized, maximized, false, false, false, true); got != placementSave {
		t.Fatalf("the user restoring a framed window: %v, want save", got)
	}
}

// A window the curtain hides when the display mode shrinks is placed for the
// new mode as the repair after the lift would place it, so the lift does not
// resize it again.
func TestHiddenWindowPlacementFollowsAModeChange(t *testing.T) {
	small := []hostMonitor{{Bounds: screenRect{0, 0, 1024, 768}, Work: screenRect{0, 0, 1024, 720}, Primary: true}}
	target := &windowPlacement{Schema: 1, Normal: screenRect{40, 40, 1240, 740}}
	fitted := repairDisplayPlacement(target, 0, small)
	if fitted == nil {
		t.Fatal("restored window: no placement")
	}
	// Its invisible borders may overhang the work area, as a shown window's do.
	if n := fitted.Normal; fitted.Maximized || n.Left < -8 || n.Top < 0 || n.Right > 1032 || n.Bottom > 728 {
		t.Fatalf("restored window: fitted to %+v", fitted)
	}
	if again := repairDisplayPlacement(fitted, 0, small); again == nil || !again.sameAs(fitted) {
		t.Fatalf("the repair after the lift moves it again: %+v to %+v", fitted, again)
	}
	maximized := &windowPlacement{Schema: 1, Normal: screenRect{24, 24, 1256, 728}, Maximized: true}
	if fitted := repairDisplayPlacement(maximized, 0, small); fitted == nil || !fitted.Maximized {
		t.Fatalf("maximized window: fitted to %+v", fitted)
	}
}

func TestLeavingAFullscreenStartPlacesTheWindow(t *testing.T) {
	if !leftStartFullscreen(true, true, false) {
		t.Fatal("a full screen start's window that has its frame back was not placed")
	}
	// Still frameless: still full screen, also while a display mode change
	// leaves it short of its monitor for a moment.
	if leftStartFullscreen(true, false, false) {
		t.Fatal("a frameless window was placed as a window")
	}
	// Before it was ever full screen, as while QEMU sets it up.
	if leftStartFullscreen(false, true, false) {
		t.Fatal("a window that was not full screen was placed")
	}
	// The frame is back but SDL has not given it its windowed size yet: it
	// would be moved to the work area's corner and stay there (acceptance
	// 2026-11-18, the window left full screen at 1280x801).
	if leftStartFullscreen(true, true, true) {
		t.Fatal("a window halfway out of full screen was placed")
	}
}

func TestFullscreenRect(t *testing.T) {
	monitors := []hostMonitor{
		{Bounds: screenRect{0, 0, 1280, 800}, Work: screenRect{0, 0, 1280, 752}},
		{Bounds: screenRect{1280, 0, 3200, 1080}, Work: screenRect{1280, 0, 3200, 1032}},
	}
	for _, r := range []screenRect{{0, 0, 1280, 800}, {0, -1, 1280, 800}, {1280, 0, 3200, 1080}, {1279, 0, 3200, 1080}} {
		if !fullscreenRect(r, monitors) {
			t.Fatalf("%v is a full screen rectangle", r)
		}
	}
	// A window, and SDL's guest-sized window spilling off the screen.
	for _, r := range []screenRect{{152, 63, 1128, 666}, {-8, -20, 1288, 820}, {0, 0, 1280, 802}} {
		if fullscreenRect(r, monitors) {
			t.Fatalf("%v is not a full screen rectangle", r)
		}
	}
}

func TestWindowIntoWorkArea(t *testing.T) {
	work := screenRect{0, 0, 1920, 1032}
	// A full screen start's window taken out of full screen: the guest's
	// 1920x1081 plus a frame, centred by SDL and spilling off the screen.
	if to, move := windowIntoWorkArea(screenRect{637, 274, 2563, 1384}, work); !move || to != (screenRect{0, 0, 1926, 1110}) {
		t.Fatalf("too large: %v, %v", to, move)
	}
	if to, move := windowIntoWorkArea(screenRect{1500, 900, 2500, 1600}, work); !move || to != (screenRect{460, 166, 1460, 866}) {
		t.Fatalf("fits but off screen: %v, %v", to, move)
	}
	if _, move := windowIntoWorkArea(screenRect{100, 80, 1100, 700}, work); move {
		t.Fatal("moved a window already inside")
	}
	// A work area that does not start at 0,0 (taskbar on the left, second monitor).
	if to, _ := windowIntoWorkArea(screenRect{1900, 274, 3826, 1384}, screenRect{1982, 0, 3840, 1080}); to.Left != 1982 || to.Top != 0 {
		t.Fatalf("offset work area: %v", to)
	}
}
