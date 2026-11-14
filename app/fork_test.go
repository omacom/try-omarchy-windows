package main

import "testing"

func TestForkBuildCannotEnableUpstreamReplacement(t *testing.T) {
	old := forkIdentity
	t.Cleanup(func() { forkIdentity = old })
	forkIdentity = "7Wdev"
	if automaticUpdatesEnabled(&config{}, false, defaultReleaseURL, defaultSumsSHA256) {
		t.Fatal("fork build would replace itself with an upstream release")
	}
	if got := launcherProjectURL("https://example.com/upstream"); got != "https://github.com/7Wdev/try-omarchy-windows" {
		t.Fatalf("fork source link: %s", got)
	}
	forkIdentity = ""
	if !automaticUpdatesEnabled(&config{}, false, defaultReleaseURL, defaultSumsSHA256) {
		t.Fatal("ordinary upstream build lost its update path")
	}
}
