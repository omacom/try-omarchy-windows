package main

import (
	"archive/zip"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
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
	storageSettingsFilename,
	"guest/" + installReceiptFilename,
	"runtime/" + runtimeReceiptFilename,
	updateStateFilename,
	payloadUpdateStateFilename,
	"vm/shell.log",
	"vm/qemu-stderr.log",
	"vm/qemu.log",
	"vm/serial.log",
	"vm/serial-gpu.log",
	"guest/guest-manifest.json",
	"guest/build-spec.json",
}

// diagnosticTailBytes caps each log so a bundle stays mailable even after
// weeks of appended shell.log. The newest part is the useful part.
const diagnosticTailBytes = 2 * 1024 * 1024

// writeDiagnostics creates the bundle under dir/diagnostics and returns its
// path. Facts are written first in deterministic key order.
func writeDiagnostics(dir string, facts map[string]string) (string, error) {
	outDir := filepath.Join(dir, "diagnostics")
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", err
	}
	name := fmt.Sprintf("try-omarchy-diagnostics-%s.zip", time.Now().Format("20060102-150405.000000000"))
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
	if settingsText, ok := sanitizedDiagnosticSettings(dir); ok {
		if err := addDiagnosticText(w, "settings.redacted.json", settingsText); err != nil {
			return fail(err)
		}
		included = append(included, "settings.redacted.json")
	}

	redactions := diagnosticRedactions(dir)
	for _, relative := range diagnosticFiles {
		source := filepath.Join(dir, filepath.FromSlash(relative))
		info, err := os.Lstat(source)
		if err != nil || !info.Mode().IsRegular() {
			continue
		}
		if err := addDiagnosticFile(w, relative, source, info, redactions); err != nil {
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

func addDiagnosticFile(w *zip.Writer, name, source string, info os.FileInfo, redactions []string) error {
	in, err := os.Open(source)
	if err != nil {
		return err
	}
	defer in.Close()
	openedInfo, err := in.Stat()
	if err != nil {
		return err
	}
	if !openedInfo.Mode().IsRegular() || !os.SameFile(info, openedInfo) {
		return fmt.Errorf("file changed while diagnostics were being collected")
	}
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
	data, err := io.ReadAll(in)
	if err != nil {
		return err
	}
	_, err = io.WriteString(entry, redactDiagnosticText(string(data), redactions))
	return err
}

func sanitizedDiagnosticSettings(dir string) (string, bool) {
	path := settingsPath(dir)
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return "", false
	}
	s, err := loadSettings(path)
	value := struct {
		Status           string `json:"status"`
		SchemaVersion    int    `json:"schemaVersion,omitempty"`
		Fullscreen       bool   `json:"fullscreen,omitempty"`
		MemoryMiB        int    `json:"memoryMiB,omitempty"`
		ShareConfigured  bool   `json:"shareConfigured,omitempty"`
		ForwardCount     int    `json:"forwardCount,omitempty"`
		SSHKeyConfigured bool   `json:"sshKeyConfigured,omitempty"`
	}{Status: "valid"}
	if err != nil {
		value.Status = "invalid"
	} else {
		value.SchemaVersion = s.SchemaVersion
		value.Fullscreen = s.Fullscreen
		value.MemoryMiB = s.MemoryMiB
		value.ShareConfigured = s.Share != ""
		value.ForwardCount = len(s.Forwards)
		value.SSHKeyConfigured = s.SSHKey != ""
	}
	data, marshalErr := json.MarshalIndent(value, "", "  ")
	if marshalErr != nil {
		return "", false
	}
	return string(data) + "\n", true
}

func diagnosticRedactions(dir string) []string {
	values := []string{dir, os.Getenv("USERPROFILE"), os.Getenv("LOCALAPPDATA")}
	if home, err := os.UserHomeDir(); err == nil {
		values = append(values, home)
	}
	seen := map[string]bool{}
	var out []string
	for _, value := range values {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		for _, form := range []string{value, filepath.ToSlash(value)} {
			if !seen[form] {
				seen[form] = true
				out = append(out, form)
			}
		}
	}
	sort.Slice(out, func(i, j int) bool { return len(out[i]) > len(out[j]) })
	return out
}

func redactDiagnosticText(text string, redactions []string) string {
	for _, value := range redactions {
		// Windows paths are case-insensitive, and different APIs do not always
		// preserve their original casing in logs.
		text = regexp.MustCompile(`(?i)`+regexp.QuoteMeta(value)).ReplaceAllString(text, "<redacted-path>")
	}
	// Public keys are not private, but their comments commonly contain a user
	// and computer name. The key is not needed to diagnose guest startup.
	const keyPrefix = "tryomarchy.sshkey="
	const keyReplacement = "tryomarchy.sshkey=<redacted>"
	for offset := 0; ; {
		relative := strings.Index(text[offset:], keyPrefix)
		if relative < 0 {
			break
		}
		start := offset + relative
		end := start
		for end < len(text) && text[end] != ' ' && text[end] != '\r' && text[end] != '\n' && text[end] != '\t' {
			end++
		}
		text = text[:start] + keyReplacement + text[end:]
		offset = start + len(keyReplacement)
	}
	return text
}

// launcherFacts is the part of the facts file every platform can fill in.
func launcherFacts(cfg *config) map[string]string {
	facts := map[string]string{
		"launcher.version":  currentVersion,
		"launcher.portable": fmt.Sprint(cfg.portable),
		"launcher.noGpu":    fmt.Sprint(cfg.noGpu),
		"time":              time.Now().Format(time.RFC3339),
	}
	for k, v := range hostFacts() {
		facts[k] = v
	}
	return facts
}
