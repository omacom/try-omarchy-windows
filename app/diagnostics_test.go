package main

import (
	"archive/zip"
	"bytes"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWriteDiagnosticsBundlesLogsStateAndFactsOnly(t *testing.T) {
	dir := t.TempDir()
	write := func(relative, content string) {
		p := filepath.Join(dir, filepath.FromSlash(relative))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("vm/shell.log", "12:00:00 booting\n")
	write("vm/qemu-stderr.log", "WHPX: Failed to enable nested virtualization, hr=80370302\n")
	write("settings.json", `{"schemaVersion":1}`)
	write("guest/guest-manifest.json", `{"kind":"try-omarchy-guest-artifacts"}`)
	write("vm/disk.raw", strings.Repeat("x", 4096))
	write("guest/rootfs.ext4", "not a log")
	big := bytes.Repeat([]byte("old line\n"), diagnosticTailBytes/9+2000)
	copy(big[len(big)-9:], []byte("NEWEST!!\n"))
	write("vm/serial.log", string(big))

	path, err := writeDiagnostics(dir, map[string]string{"launcher.version": "v9.9.9", "host.os": "test"})
	if err != nil {
		t.Fatal(err)
	}
	if filepath.Dir(path) != filepath.Join(dir, "diagnostics") || !strings.HasPrefix(filepath.Base(path), "try-omarchy-diagnostics-") {
		t.Fatalf("bundle path %s", path)
	}
	if _, err := os.Stat(path + ".part"); !os.IsNotExist(err) {
		t.Fatal("staging file left behind")
	}

	r, err := zip.OpenReader(path)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	contents := map[string]string{}
	for _, f := range r.File {
		rc, err := f.Open()
		if err != nil {
			t.Fatal(err)
		}
		data, _ := io.ReadAll(rc)
		rc.Close()
		contents[f.Name] = string(data)
	}
	for _, want := range []string{"facts.txt", "contents.txt", "vm/shell.log", "vm/qemu-stderr.log", "settings.json", "guest/guest-manifest.json", "vm/serial.log"} {
		if _, ok := contents[want]; !ok {
			t.Fatalf("bundle lacks %s; has %v", want, keys(contents))
		}
	}
	for _, forbidden := range []string{"vm/disk.raw", "guest/rootfs.ext4"} {
		if _, ok := contents[forbidden]; ok {
			t.Fatalf("bundle includes %s", forbidden)
		}
	}
	if !strings.Contains(contents["facts.txt"], "host.os: test\n") || !strings.Contains(contents["facts.txt"], "launcher.version: v9.9.9\n") {
		t.Fatalf("facts.txt = %q", contents["facts.txt"])
	}
	serial := contents["vm/serial.log"]
	if !strings.HasPrefix(serial, "[truncated: last") || !strings.HasSuffix(serial, "NEWEST!!\n") || len(serial) > diagnosticTailBytes+200 {
		t.Fatalf("large log not tailed: len=%d head=%q", len(serial), serial[:40])
	}
	if !strings.Contains(contents["contents.txt"], "vm/qemu-stderr.log\n") {
		t.Fatalf("contents.txt = %q", contents["contents.txt"])
	}
}

func TestWriteDiagnosticsWithNothingToCollectStillWritesFacts(t *testing.T) {
	dir := t.TempDir()
	path, err := writeDiagnostics(dir, launcherFacts(&config{dir: dir}))
	if err != nil {
		t.Fatal(err)
	}
	r, err := zip.OpenReader(path)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	if len(r.File) != 2 {
		t.Fatalf("expected facts and contents only, got %d entries", len(r.File))
	}
}

func keys(m map[string]string) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}
