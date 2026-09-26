package main

// Windows reserves Ctrl+Alt+Delete for its security screen before any hook
// sees it, so Ctrl+Alt+End stands in for it while Omarchy is focused, as in
// Hyper-V. The guest receives one complete Ctrl+Alt+Delete press and release
// when End goes down, so a lost End release can never leave keys held there.
type ctrlAltEndAction int

const (
	ctrlAltEndPass    ctrlAltEndAction = iota // leave End to Windows and SDL
	ctrlAltEndSend                            // swallow End and send one Ctrl+Alt+Delete
	ctrlAltEndSwallow                         // swallow a repeat or release of a sent chord
)

func classifyCtrlAltEnd(focused, ctrl, alt, sent, down bool) ctrlAltEndAction {
	switch {
	case sent:
		return ctrlAltEndSwallow
	case focused && down && ctrl && alt:
		return ctrlAltEndSend
	}
	return ctrlAltEndPass
}
