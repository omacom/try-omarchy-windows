//go:build windows

package main

import (
	"reflect"
	"testing"
)

func TestTraySettingsRetainRuntimeChoice(t *testing.T) {
	cfg := trayLaunchConfig{dataDir: `D:\Omarchy 世界`, winqEmu: `D:\Audio runtime`}
	want := []string{"-dir", cfg.dataDir, "-winq", cfg.winqEmu, "-settings"}
	if got := trayControlArguments(cfg, "-settings"); !reflect.DeepEqual(got, want) {
		t.Fatalf("%q want %q", got, want)
	}
	if got := trayControlArguments(cfg, "-about"); !reflect.DeepEqual(got, []string{"-dir", cfg.dataDir, "-about"}) {
		t.Fatal(got)
	}
	cfg.portable = true
	if got := trayControlArguments(cfg, "-settings"); !reflect.DeepEqual(got, []string{"-portable", "-settings"}) {
		t.Fatal(got)
	}
}
