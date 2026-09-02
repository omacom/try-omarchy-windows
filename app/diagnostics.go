package main

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// Diagnostics bundle for issue reports: the launcher and QEMU logs, the
// guest's serial output, settings, install and update state, and the
// authenticated manifest, plus a facts file about the machine. No disk
// images, no payloads, nothing outside the data directory. Paths inside the
// bundle mirror the data directory so a reader knows where each file lived.
var diagnosticFiles = []string{
	"settings.json",
	installReceiptFilename,
	runtimeReceiptFilename,
	updateStateFilename,
	payloadUpdateStateFilename,
	"vm/shell.log",
	"vm/qemu-stderr.log",
	"vm/qemu.log",
	"vm/serial.log",
	"vm/serial-gpu.log",
	"guest/guest-manifest.json",
	"guest/build-spec.json",
	"portable-host/whp-restart-pending",
}

// diagnosticTailBytes caps each log so a bundle stays mailable even after
// weeks of appended shell.log. The newest part is the useful part.
const diagnosticTailBytes = 2 * 1024 * 1024

// writeDiagnostics creates the bundle under dir/diagnostics and returns its
// path. facts are written first as facts.txt in insertion-stable order.
func writeDiagnostics(dir string, facts map[string]string) (string, error) {
	outDir := filepath.Join(dir, "diagnostics")
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", err
	}
	name := fmt.Sprintf("try-omarchy-diagnostics-%s.zip", time.Now().Format("20060102-150405"))
	path := filepath.Join(outDir, name)
	tmp := path + ".part"
	f, err := os.Create(tmp)
	if err != nil {
		return "", err
	}
	w := zip.NewWriter(f)
	fail := func(err error) (string, error) {
		w.Close()
		f.Close()
		os.Remove(tmp)
		return "", err
	}

	keys := make([]string, 0, len(facts))
	for k := range facts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var sb strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&sb, "%s: %s\n", k, facts[k])
	}
	if err := addDiagnosticText(w, "facts.txt", sb.String()); err != nil {
		return fail(err)
	}

	var included []string
	for _, relative := range diagnosticFiles {
		source := filepath.Join(dir, filepath.FromSlash(relative))
		info, err := os.Stat(source)
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		if err := addDiagnosticFile(w, relative, source, info); err != nil {
			return fail(fmt.Errorf("%s: %w", relative, err))
		}
		included = append(included, relative)
	}
	if err := addDiagnosticText(w, "contents.txt", strings.Join(included, "\n")+"\n"); err != nil {
		return fail(err)
	}
	if err := w.Close(); err != nil {
		f.Close()
		os.Remove(tmp)
		return "", err
	}
	if err := f.Close(); err != nil {
		os.Remove(tmp)
		return "", err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return "", err
	}
	return path, nil
}

func addDiagnosticText(w *zip.Writer, name, text string) error {
	entry, err := w.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate, Modified: time.Now()})
	if err != nil {
		return err
	}
	_, err = io.WriteString(entry, text)
	return err
}

func addDiagnosticFile(w *zip.Writer, name, source string, info os.FileInfo) error {
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	entry, err := w.CreateHeader(&zip.FileHeader{Name: name, Method: zip.Deflate, Modified: info.ModTime()})
	if err != nil {
		return err
	}
	if info.Size() > diagnosticTailBytes {
		if _, err := in.Seek(info.Size()-diagnosticTailBytes, io.SeekStart); err != nil {
			return err
		}
		fmt.Fprintf(entry, "[truncated: last %d of %d bytes]\n", diagnosticTailBytes, info.Size())
	}
	_, err = io.Copy(entry, in)
	return err
}

// launcherFacts is the part of the facts file every platform can fill in.
func launcherFacts(cfg *config) map[string]string {
	facts := map[string]string{
		"launcher.version":  currentVersion,
		"launcher.dataDir":  cfg.dir,
		"launcher.portable": fmt.Sprint(cfg.portable),
		"launcher.qemu":     cfg.qemu,
		"launcher.gpu":      fmt.Sprint(cfg.useGpu),
		"launcher.args":     strings.Join(os.Args[1:], " "),
		"time":              time.Now().Format(time.RFC3339),
	}
	for k, v := range hostFacts() {
		facts[k] = v
	}
	return facts
}
