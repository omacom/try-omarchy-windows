package main

type forwardedKey struct {
	qcode string
	down  bool
}

// Windows consumes Alt+Tab before SDL can deliver the chord to the guest, so
// the hook forwards Tab itself while Omarchy is focused. It also presses Alt
// once in case SDL never delivered it. That Alt stays down across repeated Tab
// taps and is released with the physical Alt, so a guest switcher that stays
// open while Alt is held can keep cycling.
type altTabForwarder struct {
	tabDown, altDown bool
}

// tab returns the guest keys for a Tab change and whether the host should
// swallow it. When forward is false, focus has left Omarchy, so anything
// still held in the guest is released and Tab goes to Windows.
func (f *altTabForwarder) tab(forward, down bool) (keys []forwardedKey, swallow bool) {
	if forward && down {
		if !f.altDown {
			f.altDown = true
			keys = append(keys, forwardedKey{qcode: "alt", down: true})
		}
		if !f.tabDown {
			f.tabDown = true
			keys = append(keys, forwardedKey{qcode: "tab", down: true})
		}
		return keys, true
	}
	if f.tabDown {
		f.tabDown = false
		keys = append(keys, forwardedKey{qcode: "tab", down: false})
	}
	if !forward {
		keys = append(keys, f.altReleased()...)
	}
	return keys, forward
}

// altReleased releases the forwarded Alt when the physical Alt comes up.
func (f *altTabForwarder) altReleased() []forwardedKey {
	if !f.altDown {
		return nil
	}
	f.altDown = false
	return []forwardedKey{{qcode: "alt", down: false}}
}

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
