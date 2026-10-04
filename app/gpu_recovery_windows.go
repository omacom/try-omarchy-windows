//go:build windows

package main

// QEMU has already been reaped before this dialog opens. A close/Escape or
// dialog failure closes the launcher; there is never an unattended retry.
func recoverGPUFreeze(cfg *config, bundle string, snapshotErr error, suggest bool) bool {
	body := uiText("recovery.gpu.body")
	if suggest {
		body += "\n\n" + uiText("recovery.gpu.suggest")
	}
	if snapshotErr == nil {
		body += "\n\n" + uiTextWith("recovery.gpu.saved", map[string]string{"path": bundle})
	} else {
		body += "\n\n" + uiTextWith("recovery.gpu.save_failed", map[string]string{"error": snapshotErr.Error()})
	}
	action, err := chooseAction(uiText("recovery.gpu.title"), body,
		uiText("recovery.gpu.cpu_once"), uiText("recovery.gpu.gpu"), uiText("recovery.gpu.close"), uiText("recovery.gpu.cpu_always"))
	if err != nil {
		logf("GPU freeze recovery dialog failed: %v", err)
		errorBox(body)
		return false
	}
	decision := decideGPURecovery(action)
	if err := saveGPURecoveryChoice(cfg.dir, decision); err != nil {
		logf("could not save permanent CPU rendering choice: %v", err)
		errorBox(uiTextWith("recovery.gpu.settings_failed", map[string]string{"error": err.Error()}))
		return false
	}
	if !decision.restart {
		return false
	}
	cfg.useGpu = decision.gpu
	cfg.temporaryCPU = decision.temporaryCPU
	if decision.persistCPU {
		cfg.renderMode = renderCPU
		cfg.noGpu = true
	}
	logf("GPU freeze recovery: action=%d GPU=%v temporaryCPU=%v", action, cfg.useGpu, cfg.temporaryCPU)
	return true
}
