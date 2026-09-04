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
	write("vm/shell.log", "12:00:00 booting "+strings.ToUpper(dir)+" tryomarchy.sshkey=AAA-user-at-pc\n")
	write("vm/qemu-stderr.log", "WHPX: Failed to enable nested virtualization, hr=80370302\n")
	write("settings.json", `{"schemaVersion":1,"share":"C:\\Users\\secret\\Work","forwards":["tcp:2222:22"],"sshKey":"C:\\Users\\secret\\.ssh\\id.pub"}`)
	write("guest/install-state.json", `{"version":1}`)
	write("runtime/runtime-install-state.json", `{"version":1}`)
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
	for _, want := range []string{"facts.txt", "contents.txt", "vm/shell.log", "vm/qemu-stderr.log", "settings.redacted.json", "guest/install-state.json", "runtime/runtime-install-state.json", "guest/guest-manifest.json", "vm/serial.log"} {
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
	if strings.Contains(contents["settings.redacted.json"], `C:\\Users`) ||
		!strings.Contains(contents["settings.redacted.json"], `"shareConfigured": true`) ||
		!strings.Contains(contents["settings.redacted.json"], `"sshKeyConfigured": true`) {
		t.Fatalf("settings were not safely summarized: %s", contents["settings.redacted.json"])
	}
	if strings.Contains(strings.ToLower(contents["vm/shell.log"]), strings.ToLower(dir)) || strings.Contains(contents["vm/shell.log"], "AAA-user-at-pc") {
		t.Fatalf("sensitive diagnostic values were not redacted: %s", contents["vm/shell.log"])
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
	facts := launcherFacts(&config{dir: dir, winqEmu: `C:\WINQ-EMU`, noGpu: true})
	if facts["launcher.noGpu"] != "true" {
		t.Fatalf("diagnostic configuration facts = %#v", facts)
	}
	for _, private := range []string{"launcher.winqPath", "launcher.dataDir", "launcher.args", "host.computer", "host.localAppData"} {
		if _, present := facts[private]; present {
			t.Fatalf("diagnostics included private fact %s: %#v", private, facts)
		}
	}
	if _, misleading := facts["launcher.gpu"]; misleading {
		t.Fatalf("diagnostics claimed a selected launch mode before launch: %#v", facts)
	}
	path, err := writeDiagnostics(dir, facts)
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
