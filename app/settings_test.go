package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadSettingsMissingFileIsDefaults(t *testing.T) {
	s, err := loadSettings(filepath.Join(t.TempDir(), settingsFileName))
	if err != nil || s.SchemaVersion != 0 || s.Fullscreen || s.MemoryMiB != 0 || s.Share != "" || len(s.Forwards) != 0 || s.SSHKey != "" {
		t.Fatalf("missing file: %+v %v", s, err)
	}
}

func TestSettingsRoundTrip(t *testing.T) {
	path := settingsPath(filepath.Join(t.TempDir(), "TryOmarchy"))
	in := settings{Fullscreen: true, MemoryMiB: 6144, Share: `C:\Users\me\Work`,
		Forwards: []string{"tcp:2222:22", "udp:5000:5000"}, SSHKey: `C:\Users\me\.ssh\work.pub`}
	if err := saveSettings(path, in); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path + ".part"); !os.IsNotExist(err) {
		t.Fatal("staging file left behind")
	}
	out, err := loadSettings(path)
	if err != nil {
		t.Fatal(err)
	}
	in.SchemaVersion = settingsSchemaVersion
	if out.SchemaVersion != in.SchemaVersion || out.Fullscreen != in.Fullscreen || out.MemoryMiB != in.MemoryMiB ||
		out.Share != in.Share || out.SSHKey != in.SSHKey || strings.Join(out.Forwards, ",") != strings.Join(in.Forwards, ",") {
		t.Fatalf("round trip changed settings: %+v vs %+v", out, in)
	}
}

func TestLoadSettingsRejectsDamageInsteadOfIgnoringIt(t *testing.T) {
	dir := t.TempDir()
	cases := map[string]string{
		"garbage":   "{not json",
		"schema":    `{"schemaVersion": 2}`,
		"memory":    `{"schemaVersion": 1, "memoryMiB": 512}`,
		"forward":   `{"schemaVersion": 1, "forwards": ["tcp:22"]}`,
		"duplicate": `{"schemaVersion": 1, "forwards": ["tcp:2222:22", "2222:80"]}`,
	}
	for name, content := range cases {
		path := filepath.Join(dir, name+".json")
		os.WriteFile(path, []byte(content), 0o644)
		if _, err := loadSettings(path); err == nil {
			t.Fatalf("%s accepted", name)
		}
	}
}

func TestApplySettingsLetsExplicitFlagsWin(t *testing.T) {
	file := settings{Fullscreen: true, MemoryMiB: 4096, Share: `D:\Share`,
		Forwards: []string{"tcp:2222:22"}, SSHKey: `D:\key.pub`}

	// Nothing on the command line: the file decides every row.
	cfg := &config{}
	var forwards forwardList
	keyPath := ""
	if err := applySettings(cfg, file, map[string]bool{}, &forwards, &keyPath); err != nil {
		t.Fatal(err)
	}
	if !cfg.fullscreen || cfg.memOverrideMiB != 4096 || cfg.share != `D:\Share` || keyPath != `D:\key.pub` || forwards.String() != "tcp:2222:22" {
		t.Fatalf("file not applied: %+v forwards=%s key=%s", cfg, forwards.String(), keyPath)
	}

	// Explicit flags keep their values; an explicit -ssh replaces the list.
	cfg = &config{fullscreen: false, memOverrideMiB: 0, share: ""}
	forwards = forwardList{{"tcp", 2299, 22}}
	keyPath = ""
	explicit := map[string]bool{"fullscreen": true, "memory": true, "share": true, "ssh": true, "ssh-key": true}
	if err := applySettings(cfg, file, explicit, &forwards, &keyPath); err != nil {
		t.Fatal(err)
	}
	if cfg.fullscreen || cfg.memOverrideMiB != 0 || cfg.share != "" || keyPath != "" || forwards.String() != "tcp:2299:22" {
		t.Fatalf("explicit flags overridden: %+v forwards=%s key=%s", cfg, forwards.String(), keyPath)
	}
}
