package main

import "testing"

func TestLauncherEntryPolicy(t *testing.T) {
	for _, tc := range []struct {
		name                  string
		flags                 map[string]bool
		launcher, start, want bool
	}{
		{"desktop", nil, false, false, true},
		{"moved shortcut", map[string]bool{"dir": true}, false, false, true},
		{"portable", map[string]bool{"portable": true}, false, false, true},
		{"scripted boot", map[string]bool{"ssh": true}, false, false, false},
		{"settings", map[string]bool{"settings": true}, false, false, false},
		{"update helper", map[string]bool{"apply-launcher-update": true}, false, false, false},
		{"explicit menu", map[string]bool{"memory": true, "launcher": true}, true, false, true},
		{"launch child", map[string]bool{"launcher": true, "start": true}, true, true, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if got := shouldOpenLauncher(tc.flags, tc.launcher, tc.start); got != tc.want {
				t.Fatalf("got %v, want %v", got, tc.want)
			}
		})
	}
}
