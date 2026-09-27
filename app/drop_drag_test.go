package main

import (
	"testing"
	"time"
)

func TestParseDropDragRequest(t *testing.T) {
	const id = "0123456789abcdef0123456789abcdef"
	if got, x, y, ok := parseDropDragRequest("drop-drag " + id + " 100 32767\n"); !ok || got != id || x != 100 || y != 32767 {
		t.Fatalf("valid request: %q %d %d %v", got, x, y, ok)
	}
	for _, line := range []string{
		"drop-drag " + id + " 100 200", "drop-drag  " + id + " 100 200\n", "drop-drag 100 200\n",
		"drop-drag " + id + " -1 5\n", "drop-drag " + id + " 100 32768\n", "drop-drag " + id + " 1e3 5\n",
		"drop-drag " + id[:31] + " 1 2\n", "drop-drag " + id[:31] + "G 1 2\n", "drop-drag " + id + " 1 2 3\n", "open-settings\n",
	} {
		if _, _, _, ok := parseDropDragRequest(line); ok {
			t.Fatalf("accepted %q", line)
		}
	}
}

func TestDropDragIsOncePerRecentDropAndPerID(t *testing.T) {
	now := time.Unix(1000, 0)
	recordDrop("aaaa", recordedDrop{at: now, point: []int{10, 10, 100, 100}})
	recordDrop("bbbb", recordedDrop{at: now.Add(time.Second), point: []int{50, 50, 100, 100}})
	a, err := takeDrop("aaaa", now.Add(2*time.Second))
	if err != nil || a.point[0] != 10 {
		t.Fatalf("drop A: %+v %v", a, err)
	}
	if _, err := takeDrop("aaaa", now.Add(2*time.Second)); err == nil {
		t.Fatal("the same drop was used twice")
	}
	if b, err := takeDrop("bbbb", now.Add(2*time.Second)); err != nil || b.point[0] != 50 {
		t.Fatalf("a later drop replaced an earlier one: %+v %v", b, err)
	}
	recordDrop("cccc", recordedDrop{at: now, point: []int{10, 10, 100, 100}})
	if _, err := takeDrop("cccc", now.Add(dropDragWindow+time.Second)); err == nil {
		t.Fatal("an old drop was used")
	}
}

func TestDropDragStepsEndOnTheDropPoint(t *testing.T) {
	steps, err := dropDragSteps(26000, 16000, recordedDrop{point: []int{899, 499, 1000, 1000}})
	if err != nil {
		t.Fatal(err)
	}
	first, press, last := steps[0], steps[1], steps[len(steps)-1]
	if first.x != 26000 || first.y != 16000 || first.button != -1 || press.button != 1 || press.x != 26000 {
		t.Fatalf("drag does not start with a press on the source: %+v %+v", first, press)
	}
	if last.button != 0 || last.x != 899*32767/999 || last.y != 499*32767/999 {
		t.Fatalf("drag does not release on the drop point: %+v", last)
	}
	for _, step := range steps[2 : len(steps)-1] {
		if step.button != -1 {
			t.Fatalf("extra button event mid-drag: %+v", step)
		}
	}
	if _, err := dropDragSteps(0, 0, recordedDrop{point: []int{899, 499, 1000, 1000}}); err == nil {
		t.Fatal("a drag from across the desktop was accepted")
	}
	for _, point := range [][]int{nil, {1, 2}, {5, 5, 1, 1}, {100, 5, 100, 50}} {
		if _, err := dropDragSteps(0, 0, recordedDrop{point: point}); err == nil {
			t.Fatalf("accepted drop point %v", point)
		}
	}
}

func TestCursorMoved(t *testing.T) {
	if cursorMoved([2]int32{100, 100}, [2]int32{103, 96}) || !cursorMoved([2]int32{100, 100}, [2]int32{105, 100}) {
		t.Fatal("cursor tolerance is wrong")
	}
}
