package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// windowPlacementFilename remembers where the VM window was last left, so the
// next launch opens it there instead of maximized on the primary display.
const windowPlacementFilename = "window-placement.json"

// The window must still be large enough to use and mostly on a display that
// still exists; otherwise the launch falls back to the maximized default.
const (
	minimumRememberedWidth   = 480
	minimumRememberedHeight  = 320
	minimumVisibleWidth      = 200
	minimumVisibleHeight     = 120
	windowTitleBarHeight     = 31
	windowPlacementSchemaNow = 1
)

type screenRect struct{ Left, Top, Right, Bottom int32 }

type windowPlacement struct {
	Schema int `json:"schema"`
	// Normal is the window's restored (non-maximized) rectangle in screen
	// coordinates, the way Windows keeps it in WINDOWPLACEMENT.
	Normal    screenRect `json:"normal"`
	Maximized bool       `json:"maximized"`
	SavedAt   time.Time  `json:"savedAt"`
}

func (r screenRect) width() int32  { return r.Right - r.Left }
func (r screenRect) height() int32 { return r.Bottom - r.Top }

func loadWindowPlacement(dir string) (*windowPlacement, error) {
	data, err := os.ReadFile(filepath.Join(dir, windowPlacementFilename))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(data) > maxSettingsBytes {
		return nil, fmt.Errorf("%s is too large", windowPlacementFilename)
	}
	var p windowPlacement
	if err := json.Unmarshal(data, &p); err != nil {
		return nil, err
	}
	if p.Schema != windowPlacementSchemaNow {
		return nil, fmt.Errorf("%s has an unsupported schema", windowPlacementFilename)
	}
	return &p, nil
}

func saveWindowPlacement(dir string, p windowPlacement) error {
	p.Schema = windowPlacementSchemaNow
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, windowPlacementFilename)
	tmp := path + ".part"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// usable reports whether the remembered rectangle can be restored on the
// displays present now. A window left on a monitor that is gone, or shrunk to
// a sliver, is not worth restoring.
func (p *windowPlacement) usable(monitors []screenRect) bool {
	if p == nil {
		return false
	}
	r := p.Normal
	if r.width() < minimumRememberedWidth || r.height() < minimumRememberedHeight {
		return false
	}
	for _, m := range monitors {
		left, top := max(r.Left, m.Left), max(r.Top, m.Top)
		right, bottom := min(r.Right, m.Right), min(r.Bottom, m.Bottom)
		if right-left >= minimumVisibleWidth && bottom-top >= minimumVisibleHeight {
			return true
		}
	}
	return false
}

// sameAs ignores the timestamp so an unchanged window does not rewrite the
// file every second.
func (p *windowPlacement) sameAs(o *windowPlacement) bool {
	return p != nil && o != nil && p.Normal == o.Normal && p.Maximized == o.Maximized
}

// consoleSize is the guest console resolution that fills the remembered
// window, so the picture matches the window from the first frame.
func (p *windowPlacement) consoleSize() (int, int) {
	return int(p.Normal.width()), int(p.Normal.height()) - windowTitleBarHeight
}
