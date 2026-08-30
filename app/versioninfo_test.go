package main

import (
	"bytes"
	"fmt"
	"os"
	"regexp"
	"testing"
	"unicode/utf16"
)

func utf16le(s string) []byte {
	out := make([]byte, 0, len(s)*2)
	for _, u := range utf16.Encode([]rune(s)) {
		out = append(out, byte(u), byte(u>>8))
	}
	return out
}

// The version block ships inside a committed resource object, so bumping
// currentVersion without regenerating it would quietly ship stale metadata to
// Explorer, Task Manager and the UAC prompt.
func TestVersionInfoResourceMatchesCurrentVersion(t *testing.T) {
	syso, err := os.ReadFile("rsrc_windows_amd64.syso")
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains(syso, utf16le(currentVersion)) {
		t.Fatalf("rsrc_windows_amd64.syso does not carry %q - rebuild it from versioninfo.rc", currentVersion)
	}

	rc, err := os.ReadFile("versioninfo.rc")
	if err != nil {
		t.Fatal(err)
	}
	parts := regexp.MustCompile(`^v(\d+)\.(\d+)\.(\d+)`).FindStringSubmatch(currentVersion)
	if parts == nil {
		t.Fatalf("currentVersion %q is not vMAJOR.MINOR.PATCH", currentVersion)
	}
	want := fmt.Sprintf("FILEVERSION %s, %s, %s, 0", parts[1], parts[2], parts[3])
	if !bytes.Contains(rc, []byte(want)) {
		t.Fatalf("versioninfo.rc has no %q for currentVersion %q", want, currentVersion)
	}
}
