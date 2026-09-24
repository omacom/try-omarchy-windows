package main

// Windows reserves Ctrl+Alt+Delete for its security screen before any hook
// sees it, so Ctrl+Alt+End stands in for it while Omarchy is focused, as in
// Hyper-V. Once forwarding starts, the release is forwarded too.
func forwardsCtrlAltDelete(focused, ctrl, alt, forwarding, down bool) bool {
	return focused && (forwarding || down && ctrl && alt)
}
