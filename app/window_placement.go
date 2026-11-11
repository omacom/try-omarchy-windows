package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
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
	windowPlacementSchemaNow = 1
	// How much bigger an ordinary resizable window is than its client area,
	// in the 96 DPI units the launcher works in (it is not DPI aware): 8 px
	// borders left, right and bottom, and a 31 px caption with its top
	// border. Windows 10 and 11 agree.
	windowFrameWidth  = 16
	windowFrameHeight = 39
)

type screenRect struct{ Left, Top, Right, Bottom int32 }

type windowPlacement struct {
	Schema int `json:"schema"`
	// Normal uses WINDOWPLACEMENT workspace coordinates, which exclude
	// taskbars at the top or left of the monitor.
	Normal    screenRect `json:"normal"`
	Maximized bool       `json:"maximized"`
	SavedAt   time.Time  `json:"savedAt"`
}

func (r screenRect) width() int32  { return r.Right - r.Left }
func (r screenRect) height() int32 { return r.Bottom - r.Top }

func loadWindowPlacement(dir string) (*windowPlacement, error) { return loadDisplayPlacement(dir, 0) }

func loadDisplayPlacement(dir string, index int) (*windowPlacement, error) {
	filename := displayPlacementFilename(index)
	data, err := os.ReadFile(filepath.Join(dir, filename))
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

func saveWindowPlacement(dir string, p windowPlacement) error { return saveDisplayPlacement(dir, 0, p) }

func saveDisplayPlacement(dir string, index int, p windowPlacement) error {
	p.Schema = windowPlacementSchemaNow
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, displayPlacementFilename(index))
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
		if right-left >= minimumVisibleWidth && bottom-top >= minimumVisibleHeight && r.Top >= m.Top && r.Top+windowFrameHeight <= m.Bottom {
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
// window's client area, so the picture matches the window from the first
// frame. A console bigger than the client area made QEMU grow the window to
// fit it, and the bigger window was remembered for the next launch.
func (p *windowPlacement) consoleSize() (int, int) {
	return int(p.Normal.width()) - windowFrameWidth, int(p.Normal.height()) - windowFrameHeight
}

// fittedTo returns the placement moved, and shrunk if it has to be, so the
// window lies inside the work area (the display minus the taskbar) of the
// display it is mostly on. Windows' invisible resize borders may hang over
// the edges, as they do for a snapped window. Launchers before v0.7.2 could
// remember a window that had grown past the screen.
func (p *windowPlacement) fittedTo(works []screenRect) *windowPlacement {
	if p == nil || len(works) == 0 {
		return p
	}
	r := p.Normal
	best, bestArea := works[0], int64(-1)
	for _, w := range works {
		width := max(0, min(r.Right, w.Right)-max(r.Left, w.Left))
		height := max(0, min(r.Bottom, w.Bottom)-max(r.Top, w.Top))
		if area := int64(width) * int64(height); area > bestArea {
			best, bestArea = w, area
		}
	}
	border := int32(windowFrameWidth / 2)
	area := screenRect{best.Left - border, best.Top, best.Right + border, best.Bottom + border}
	width, height := min(r.width(), area.width()), min(r.height(), area.height())
	left := min(max(r.Left, area.Left), area.Right-width)
	top := min(max(r.Top, area.Top), area.Bottom-height)
	fitted := *p
	fitted.Normal = screenRect{left, top, left + width, top + height}
	return &fitted
}

type hostMonitor struct {
	Name    string
	Bounds  screenRect
	Primary bool
	Work    screenRect
}

func primaryFirstMonitors(monitors []hostMonitor) []hostMonitor {
	monitors = slices.Clone(monitors)
	slices.SortStableFunc(monitors, func(a, b hostMonitor) int {
		if a.Primary == b.Primary {
			return 0
		}
		if a.Primary {
			return -1
		}
		return 1
	})
	return monitors
}

func (r screenRect) offset(x, y int32) screenRect {
	return screenRect{r.Left + x, r.Top + y, r.Right + x, r.Bottom + y}
}

func placementMonitor(r screenRect, monitors []hostMonitor) hostMonitor {
	var best hostMonitor
	bestArea := int64(-1)
	for _, m := range monitors {
		width := max(0, min(r.Right, m.Bounds.Right)-max(r.Left, m.Bounds.Left))
		height := max(0, min(r.Bottom, m.Bounds.Bottom)-max(r.Top, m.Bounds.Top))
		if area := int64(width) * int64(height); area > bestArea {
			best, bestArea = m, area
		}
	}
	return best
}

func (p *windowPlacement) screenPlacement(monitors []hostMonitor) *windowPlacement {
	if p == nil {
		return nil
	}
	m := placementMonitor(p.Normal, monitors)
	screen := *p
	screen.Normal = p.Normal.offset(m.Work.Left-m.Bounds.Left, m.Work.Top-m.Bounds.Top)
	return &screen
}

func (p *windowPlacement) workspacePlacement(monitors []hostMonitor) *windowPlacement {
	if p == nil {
		return nil
	}
	m := placementMonitor(p.Normal, monitors)
	workspace := *p
	workspace.Normal = p.Normal.offset(m.Bounds.Left-m.Work.Left, m.Bounds.Top-m.Work.Top)
	return &workspace
}

func workAreas(monitors []hostMonitor) []screenRect {
	areas := make([]screenRect, 0, len(monitors))
	for _, monitor := range monitors {
		areas = append(areas, monitor.Work)
	}
	return areas
}

func usablePlacement(p *windowPlacement, monitors []hostMonitor) bool {
	return p.screenPlacement(monitors).usable(workAreas(monitors))
}

func fitPlacement(p *windowPlacement, monitors []hostMonitor) *windowPlacement {
	return p.screenPlacement(monitors).fittedTo(workAreas(monitors)).workspacePlacement(monitors)
}

func displayPlacement(dir string, index int, monitors []hostMonitor) *windowPlacement {
	p, err := loadDisplayPlacement(dir, index)
	if err != nil || !usablePlacement(p, monitors) {
		p = initialDisplayPlacement(index, workAreas(monitors)).workspacePlacement(monitors)
	}
	return fitPlacement(p, monitors)
}

func windowedConsoleSize(p *windowPlacement, monitors []hostMonitor) (int, int) {
	if p == nil {
		return 1280, 800
	}
	if !p.Maximized {
		return p.consoleSize()
	}
	monitor := placementMonitor(p.screenPlacement(monitors).Normal, monitors)
	return int(monitor.Work.width()), int(monitor.Work.height()) - (windowFrameHeight - windowFrameWidth/2)
}

// repairDisplayPlacement is deferred until a minimized window is restored or
// a user finishes dragging it. Preserve its current maximized state.
func repairDisplayPlacement(now *windowPlacement, index int, monitors []hostMonitor) *windowPlacement {
	if now == nil || len(monitors) == 0 {
		return nil
	}
	if !usablePlacement(now, monitors) {
		p := initialDisplayPlacement(index, workAreas(monitors)).workspacePlacement(monitors)
		p.Maximized = now.Maximized
		return fitPlacement(p, monitors)
	}
	return fitPlacement(now, monitors)
}

// fullscreenPlacement handles QEMU's own fullscreen toggle (Ctrl+Alt+F) for
// a windowed VM. Nothing is remembered while the window is fullscreen. On the
// way out SDL sizes the window to the guest's fullscreen display, larger than
// the work area, so the placement from before fullscreen goes back instead of
// being saved as the user's. handled is false when neither applies.
func fullscreenPlacement(wasFullscreen, fullscreen bool, last *windowPlacement) (restore *windowPlacement, handled bool) {
	switch {
	case fullscreen:
		return nil, true
	case wasFullscreen:
		return last, true
	}
	return nil, false
}

// placementBeforeFullscreen picks the placement to put back after fullscreen.
// SDL un-maximizes a maximized window on its way into fullscreen, and that
// step can be saved like a user's change just before the window covers the
// screen. A save that recent belongs to the transition, so the placement it
// replaced is the one to restore.
func placementBeforeFullscreen(last, previous *windowPlacement, sinceSave time.Duration) *windowPlacement {
	if previous != nil && sinceSave >= 0 && sinceSave < 2*time.Second {
		return previous
	}
	return last
}

type placementStep int

const (
	placementKeep    placementStep = iota
	placementSave                  // the user placed the window: remember it
	placementRestore               // something else resized it: put it back
)

// placementAfterCurtain is where a window goes once the boot curtain stops
// hiding it, or nil to leave it. QEMU resizes a hidden window to the guest's
// boot display, and the title enforcer leaves hidden windows alone, so the
// placement the window belongs at is put back before any later change can
// be saved as the user's: the guest may already follow the window by then.
func placementAfterCurtain(now, target *windowPlacement) *windowPlacement {
	if now == nil || target == nil || now.sameAs(target) {
		return nil
	}
	return target
}

// nextPlacementStep decides what the title enforcer does with a VM window
// whose placement is now. last is the placement remembered for it and target
// where it belongs. While the guest boots or shuts down (held), QEMU resizes
// the window to whatever the guest's display is; those changes are undone,
// not remembered. Once the guest's desktop follows the window, any change is
// the user's: a drag, Snap, or a tool that arranges windows.
func nextPlacementStep(now, last, target *windowPlacement, userMoved, dragging, held bool) placementStep {
	switch {
	case now == nil || dragging:
		return placementKeep
	case userMoved || last == nil || now.Maximized != last.Maximized:
		return placementSave
	case held:
		if target != nil && !now.Maximized && !target.Maximized && now.Normal != target.Normal {
			return placementRestore
		}
		return placementKeep
	case !now.sameAs(last):
		return placementSave
	}
	return placementKeep
}
