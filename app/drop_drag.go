package main

import (
	"errors"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Delivering a Windows file drop to the Omarchy app under the pointer. The
// guest receives the files, then shows a small drag source next to the drop
// point and asks for "drop-drag X Y" over the agent port with that source's
// position in absolute tablet units (0-32767). The launcher answers by
// pressing on the source and dragging to the recorded drop point through the
// guest's virtual tablet, so the app gets an ordinary Wayland drop. It does
// that at most once per drop, only soon after it, and only if the Windows
// pointer has not moved and the Omarchy window is still in front; otherwise
// the files stay in Downloads.

const (
	dropDragWindow  = 30 * time.Second
	tabletMaximum   = 32767
	cursorTolerance = 4
)

type recordedDrop struct {
	at     time.Time
	point  []int    // x, y, width, height in display-window client pixels
	cursor [2]int32 // Windows screen position of the pointer at the drop
}

type pointerStep struct {
	x, y   int  // absolute tablet position
	button int8 // -1 leaves the button alone, 1 presses, 0 releases
	pause  time.Duration
}

var lastDrop struct {
	sync.Mutex
	drop *recordedDrop
}

func recordDrop(drop recordedDrop) {
	lastDrop.Lock()
	lastDrop.drop = &drop
	lastDrop.Unlock()
}

// takeDrop returns the recorded drop once; a second drag request for the same
// drop finds nothing.
func takeDrop(now time.Time) (recordedDrop, error) {
	lastDrop.Lock()
	defer lastDrop.Unlock()
	drop := lastDrop.drop
	lastDrop.drop = nil
	if drop == nil {
		return recordedDrop{}, errors.New("no recent drop")
	}
	if now.Sub(drop.at) > dropDragWindow || now.Before(drop.at) {
		return recordedDrop{}, errors.New("the drop is too old")
	}
	return *drop, nil
}

func parseDropDragRequest(line string) (int, int, bool) {
	fields := strings.Fields(strings.TrimSuffix(line, "\n"))
	if len(fields) != 3 || fields[0] != "drop-drag" || line != strings.Join(fields, " ")+"\n" {
		return 0, 0, false
	}
	x, errX := strconv.Atoi(fields[1])
	y, errY := strconv.Atoi(fields[2])
	if errX != nil || errY != nil || x < 0 || y < 0 || x > tabletMaximum || y > tabletMaximum {
		return 0, 0, false
	}
	return x, y, true
}

func cursorMoved(a, b [2]int32) bool {
	dx, dy := a[0]-b[0], a[1]-b[1]
	return dx < -cursorTolerance || dx > cursorTolerance || dy < -cursorTolerance || dy > cursorTolerance
}

// dropDragSteps presses at the guest's drag source and moves to the drop
// point in small steps, so the source sees a drag start and the target sees
// the pointer enter it before the release.
func dropDragSteps(startX, startY int, drop recordedDrop) ([]pointerStep, error) {
	if len(drop.point) != 4 || drop.point[2] < 2 || drop.point[3] < 2 ||
		drop.point[0] < 0 || drop.point[1] < 0 || drop.point[0] >= drop.point[2] || drop.point[1] >= drop.point[3] {
		return nil, errors.New("the drop has no position")
	}
	endX := drop.point[0] * tabletMaximum / (drop.point[2] - 1)
	endY := drop.point[1] * tabletMaximum / (drop.point[3] - 1)
	steps := []pointerStep{
		{x: startX, y: startY, button: -1, pause: 60 * time.Millisecond},
		{x: startX, y: startY, button: 1, pause: 80 * time.Millisecond},
	}
	const moves = 12
	for i := 1; i <= moves; i++ {
		steps = append(steps, pointerStep{
			x: startX + (endX-startX)*i/moves, y: startY + (endY-startY)*i/moves,
			button: -1, pause: 25 * time.Millisecond,
		})
	}
	steps[len(steps)-1].pause = 120 * time.Millisecond
	return append(steps, pointerStep{x: endX, y: endY, button: 0}), nil
}
