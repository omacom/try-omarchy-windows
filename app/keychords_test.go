package main

import "testing"

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
