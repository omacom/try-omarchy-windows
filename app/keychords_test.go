package main

import "testing"

func TestForwardsCtrlAltDelete(t *testing.T) {
	tests := []struct {
		name                                 string
		focused, ctrl, alt, forwarding, down bool
		want                                 bool
	}{
		{name: "focused ctrl alt down", focused: true, ctrl: true, alt: true, down: true, want: true},
		{name: "not focused", ctrl: true, alt: true, down: true, want: false},
		{name: "ctrl only", focused: true, ctrl: true, down: true, want: false},
		{name: "alt only", focused: true, alt: true, down: true, want: false},
		{name: "neither modifier", focused: true, down: true, want: false},
		{name: "key up while forwarding", focused: true, forwarding: true, want: true},
		{name: "key up while not forwarding", focused: true, want: false},
		{name: "repeat down while forwarding", focused: true, forwarding: true, down: true, want: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got := forwardsCtrlAltDelete(test.focused, test.ctrl, test.alt, test.forwarding, test.down)
			if got != test.want {
				t.Fatalf("forwardsCtrlAltDelete() = %t, want %t", got, test.want)
			}
		})
	}
}
