package main

import (
	"slices"
	"testing"
)

func TestClassifyCtrlAltEnd(t *testing.T) {
	tests := []struct {
		name                           string
		focused, ctrl, alt, sent, down bool
		want                           ctrlAltEndAction
	}{
		{name: "send focused ctrl alt down", focused: true, ctrl: true, alt: true, down: true, want: ctrlAltEndSend},
		{name: "pass when unfocused", ctrl: true, alt: true, down: true, want: ctrlAltEndPass},
		{name: "pass with ctrl only", focused: true, ctrl: true, down: true, want: ctrlAltEndPass},
		{name: "pass with alt only", focused: true, alt: true, down: true, want: ctrlAltEndPass},
		{name: "pass with no modifiers", focused: true, down: true, want: ctrlAltEndPass},
		{name: "pass key up before sending", focused: true, ctrl: true, alt: true, want: ctrlAltEndPass},
		{name: "swallow repeat down after sending", focused: true, sent: true, down: true, want: ctrlAltEndSwallow},
		{name: "swallow key up after sending", focused: true, sent: true, want: ctrlAltEndSwallow},
		{name: "swallow unfocused key up after sending", sent: true, want: ctrlAltEndSwallow},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got := classifyCtrlAltEnd(test.focused, test.ctrl, test.alt, test.sent, test.down)
			if got != test.want {
				t.Fatalf("classifyCtrlAltEnd() = %v, want %v", got, test.want)
			}
		})
	}
}

func TestAltTabForwarderKeepsAltHeldAcrossTabs(t *testing.T) {
	var f altTabForwarder
	var got []forwardedKey
	press := func(forward, down, wantSwallow bool) {
		t.Helper()
		keys, swallow := f.tab(forward, down)
		if swallow != wantSwallow {
			t.Fatalf("tab(%v, %v) swallow = %v, want %v", forward, down, swallow, wantSwallow)
		}
		got = append(got, keys...)
	}
	press(true, true, true)
	press(true, true, true) // autorepeat
	press(true, false, true)
	press(true, true, true)
	press(true, false, true)
	got = append(got, f.altReleased()...)
	got = append(got, f.altReleased()...)
	want := []forwardedKey{
		{"alt", true}, {"tab", true}, {"tab", false},
		{"tab", true}, {"tab", false}, {"alt", false},
	}
	if !slices.Equal(got, want) {
		t.Fatalf("keys = %v, want %v", got, want)
	}
}

func TestAltTabForwarderReleasesOnFocusLoss(t *testing.T) {
	var f altTabForwarder
	f.tab(true, true)
	keys, swallow := f.tab(false, false)
	if swallow {
		t.Fatal("unfocused Tab was swallowed")
	}
	want := []forwardedKey{{"tab", false}, {"alt", false}}
	if !slices.Equal(keys, want) {
		t.Fatalf("keys = %v, want %v", keys, want)
	}
	if keys, _ := f.tab(false, true); len(keys) != 0 {
		t.Fatalf("unfocused Tab forwarded %v", keys)
	}
	if keys := f.altReleased(); len(keys) != 0 {
		t.Fatalf("Alt released twice: %v", keys)
	}
}
