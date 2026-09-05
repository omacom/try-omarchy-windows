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
	if w, h := got.consoleSize(); w != 1280 || h != 800-windowTitleBarHeight {
		t.Fatalf("console size %dx%d", w, h)
	}
}

func TestWindowPlacementUsableOnlyOnAPresentDisplay(t *testing.T) {
	primary := screenRect{0, 0, 1920, 1080}
	second := screenRect{1920, 0, 3840, 1080}
	cases := []struct {
		name  string
		p     *windowPlacement
		mons  []screenRect
		usable bool
	}{
		{"nil", nil, []screenRect{primary}, false},
		{"on primary", &windowPlacement{Normal: screenRect{100, 100, 1300, 900}}, []screenRect{primary}, true},
		{"on second monitor present", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary, second}, true},
		{"on second monitor gone", &windowPlacement{Normal: screenRect{2000, 100, 3200, 900}}, []screenRect{primary}, false},
		{"mostly off screen", &windowPlacement{Normal: screenRect{1800, 1000, 3000, 1800}}, []screenRect{primary}, false},
		{"too small", &windowPlacement{Normal: screenRect{0, 0, 300, 200}}, []screenRect{primary}, false},
		{"partly off the edge", &windowPlacement{Normal: screenRect{-200, -50, 1000, 700}}, []screenRect{primary}, true},
	}
	for _, c := range cases {
		if got := c.p.usable(c.mons); got != c.usable {
			t.Errorf("%s: usable=%v, want %v", c.name, got, c.usable)
		}
	}
}
