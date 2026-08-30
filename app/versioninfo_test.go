package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"regexp"
	"strconv"
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

// fixedFileInfo reads the FILEVERSION and PRODUCTVERSION out of the compiled
// resource's VS_FIXEDFILEINFO block, which is what Windows actually reports.
func fixedFileInfo(t *testing.T, syso []byte) (file, product [4]uint16) {
	t.Helper()
	sig := make([]byte, 4)
	binary.LittleEndian.PutUint32(sig, 0xFEEF04BD)
	i := bytes.Index(syso, sig)
	if i < 0 || i+24 > len(syso) {
		t.Fatal("no VS_FIXEDFILEINFO block in rsrc_windows_amd64.syso")
	}
	read := func(off int) [4]uint16 {
		ms := binary.LittleEndian.Uint32(syso[i+off:])
		ls := binary.LittleEndian.Uint32(syso[i+off+4:])
		return [4]uint16{uint16(ms >> 16), uint16(ms), uint16(ls >> 16), uint16(ls)}
	}
	return read(8), read(16)
}

// The version block ships inside a committed resource object, so bumping
// currentVersion without regenerating it would quietly ship stale metadata to
// Explorer, Task Manager and the UAC prompt.
func TestVersionInfoResourceMatchesCurrentVersion(t *testing.T) {
	parts := regexp.MustCompile(`^v(\d+)\.(\d+)\.(\d+)`).FindStringSubmatch(currentVersion)
	if parts == nil {
		t.Fatalf("currentVersion %q is not vMAJOR.MINOR.PATCH", currentVersion)
	}
	var want [4]uint16
	for i := range 3 {
		n, err := strconv.Atoi(parts[i+1])
		if err != nil || n > 0xffff {
			t.Fatalf("version component %q does not fit a resource field", parts[i+1])
		}
		want[i] = uint16(n)
	}

	syso, err := os.ReadFile("rsrc_windows_amd64.syso")
	if err != nil {
		t.Fatal(err)
	}
	// Match the trailing NUL of the resource string: without it a shortened
	// currentVersion would pass as a prefix of a stale one, e.g. "v0.0.7"
	// matching inside a leftover "v0.0.7-preview".
	if n := bytes.Count(syso, utf16le(currentVersion+"\x00")); n < 2 {
		t.Errorf("rsrc_windows_amd64.syso carries %q %d time(s), want it in both FileVersion "+
			"and ProductVersion - rebuild it from versioninfo.rc", currentVersion, n)
	}
	file, product := fixedFileInfo(t, syso)
	if file != want {
		t.Errorf("compiled FILEVERSION = %v, want %v", file, want)
	}
	if product != want {
		t.Errorf("compiled PRODUCTVERSION = %v, want %v", product, want)
	}

	rc, err := os.ReadFile("versioninfo.rc")
	if err != nil {
		t.Fatal(err)
	}
	nums := fmt.Sprintf("%s, %s, %s, 0", parts[1], parts[2], parts[3])
	for _, field := range []string{"FILEVERSION", "PRODUCTVERSION"} {
		if !bytes.Contains(rc, []byte(field+" "+nums)) {
			t.Errorf("versioninfo.rc has no %q for currentVersion %q", field+" "+nums, currentVersion)
		}
	}
	for _, field := range []string{"FileVersion", "ProductVersion"} {
		line := fmt.Sprintf("VALUE %q, %q", field, currentVersion)
		if !bytes.Contains(rc, []byte(line)) {
			t.Errorf("versioninfo.rc has no %s", line)
		}
	}
}
