package main

// Set by scripts/fork/build.ps1. Ordinary upstream builds retain their update
// behavior; fork builds must not replace themselves with an upstream launcher.
var forkIdentity string

func launcherProjectURL(upstream string) string {
	if forkIdentity != "" {
		return "https://github.com/" + forkIdentity + "/try-omarchy-windows"
	}
	return upstream
}
