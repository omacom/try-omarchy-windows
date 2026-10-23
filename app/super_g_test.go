package main

import (
	"reflect"
	"testing"
)

func TestSuperGOrderedQueue(t *testing.T) {
	for _, super := range []string{"meta_l", "meta_r"} {
		t.Run(super, func(t *testing.T) {
			win := routedKey{qcode: super}
			var chord superGForwarder
			q := newForwardedKeyQueue()
			q.session(1)
			push := func(keys []forwardedKey, swallowed bool) {
				if !swallowed || !q.add(1, keys, false) {
					t.Fatal("guest chord escaped routing")
				}
			}
			push(win.route(true, false, true))
			push(chord.route(true, win.side == keySideGuest, false, false, true))
			// Super can be released before G; ownership must survive it.
			push(win.route(true, false, false))
			push(chord.route(true, false, false, false, false))
			var got []forwardedKey
			for {
				key, ok, reset := q.next()
				if reset {
					t.Fatal("unexpected queue reset")
				}
				if !ok {
					break
				}
				got = append(got, key)
			}
			want := []forwardedKey{{super, true}, {"g", true}, {super, false}, {"g", false}}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("guest order %v; want %v", got, want)
			}
		})
	}
}

func TestSuperGLeavesOtherInputAlone(t *testing.T) {
	for _, tc := range []struct {
		name                                  string
		focused, super, modifiers, held, down bool
	}{
		{"plain G", true, false, false, false, true},
		{"host window", false, true, false, false, true},
		{"modified Super G", true, true, true, false, true},
		{"G pressed before Super", true, true, false, true, true},
		{"unpaired G release", true, true, false, false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var chord superGForwarder
			if keys, swallow := chord.route(tc.focused, tc.super, tc.modifiers, tc.held, tc.down); len(keys) != 0 || swallow {
				t.Fatalf("unrelated input consumed: %v, %v", keys, swallow)
			}
		})
	}
}

func TestSuperGReleasesAfterFocusLoss(t *testing.T) {
	var chord superGForwarder
	chord.route(true, true, false, false, true)
	keys, swallow := chord.route(false, false, false, false, false)
	if want := []forwardedKey{{"g", false}}; !swallow || !reflect.DeepEqual(keys, want) {
		t.Fatalf("lost-focus release %v, %v; want %v, true", keys, swallow, want)
	}
	if keys, swallow := chord.route(false, false, false, false, true); len(keys) != 0 || swallow {
		t.Fatalf("later host G consumed: %v, %v", keys, swallow)
	}
}
