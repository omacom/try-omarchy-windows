package main

// Fixed loopback ports used only for communication between the Windows shell,
// QEMU, and the guest. User-configured TCP forwards must not reuse them.
const (
	qmpToolsPort  = 4445
	qmpFwdPort    = 4446
	qmpSupPort    = 4447
	clipPushPort  = 4448
	clipPullPort  = 4449
	lifecyclePort = 4450
	agentPort     = 4451
)
