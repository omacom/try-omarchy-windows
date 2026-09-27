package main

import (
	"testing"
	"time"
)

func TestParseDropDragRequest(t *testing.T) {
	if x, y, ok := parseDropDragRequest("drop-drag 100 32767\n"); !ok || x != 100 || y != 32767 {
		t.Fatalf("valid request: %d %d %v", x, y, ok)
	}
	for _, line := range []string{
		"drop-drag 100 200", "drop-drag  100 200\n", "drop-drag 100\n", "drop-drag -1 5\n",
		"drop-drag 100 32768\n", "drop-drag 1e3 5\n", "drop-drag 1 2 3\n", "open-settings\n",
	} {
		if _, _, ok := parseDropDragRequest(line); ok {
			t.Fatalf("accepted %q", line)
		}
	}
}

func TestDropDragIsOncePerRecentDrop(t *testing.T) {
	now := time.Unix(1000, 0)
	recordDrop(recordedDrop{at: now, point: []int{10, 10, 100, 100}})
	if _, err := takeDrop(now.Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	if _, err := takeDrop(now.Add(time.Second)); err == nil {
		t.Fatal("the same drop was used twice")
	}
	recordDrop(recordedDrop{at: now, point: []int{10, 10, 100, 100}})
	if _, err := takeDrop(now.Add(dropDragWindow + time.Second)); err == nil {
		t.Fatal("an old drop was used")
	}
}

func TestDropDragStepsEndOnTheDropPoint(t *testing.T) {
	steps, err := dropDragSteps(1000, 2000, recordedDrop{point: []int{899, 499, 1000, 1000}})
	if err != nil {
		t.Fatal(err)
	}
	first, press, last := steps[0], steps[1], steps[len(steps)-1]
	if first.x != 1000 || first.y != 2000 || first.button != -1 || press.button != 1 || press.x != 1000 {
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
