package main

// Win+G can lose its ordinary G event even after the hook forwards Super.
// Route this one chord over the same ordered QMP queue.
// Once G belongs to the guest, keep its release there even if Super comes
// up first or focus leaves the VM.
type superGForwarder struct {
	key routedKey
}

func (f *superGForwarder) route(focused, guestSuper, otherModifiers, hostHeld, down bool) ([]forwardedKey, bool) {
	if f.key.side == keySideNone && !(focused && guestSuper && !otherModifiers && !hostHeld && down) {
		return nil, false
	}
	f.key.qcode = "g"
	return f.key.route(focused, hostHeld, down)
}
