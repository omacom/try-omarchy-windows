package main

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"slices"
	"strings"
	"testing"
)

func openFileTestLine(rel string) string {
	return "open-file " + base64.StdEncoding.EncodeToString([]byte(rel)) + "\n"
}

func TestReviewedFileTypes(t *testing.T) {
	want := strings.Fields(".pdf .txt .md .csv .docx .xlsx .pptx .png .jpg .jpeg .gif .webp .bmp .mp3 .wav .flac .mp4 .mkv .webm")
	if !slices.Equal(ReviewedFileTypes(), want) {
		t.Fatalf("catalog = %v", ReviewedFileTypes())
	}
	copy := ReviewedFileTypes()
	copy[0] = ".exe"
	if !slices.Equal(ReviewedFileTypes(), want) {
		t.Fatal("caller mutated catalog")
	}
	seen := map[string]bool{}
	for _, ext := range reviewedFileTypes {
		if seen[ext] || !validFileTypeExtension(ext) || slices.Contains(neverAllowedFileTypes, ext) {
			t.Errorf("invalid or duplicate catalog entry %s", ext)
		}
		seen[ext] = true
	}
	for _, ext := range neverAllowedFileTypes {
		if validFileTypeExtension(ext) {
			t.Errorf("never-allowed type accepted: %s", ext)
		}
	}
}

func TestApprovedFileTypeValidation(t *testing.T) {
	id := strings.Repeat("a", 32)
	for _, tc := range []struct {
		name    string
		entries []approvedFileType
		ok      bool
	}{
		{"empty", nil, true},
		{"default", []approvedFileType{{Ext: ".pdf"}}, true},
		{"approved app", []approvedFileType{{Ext: ".pdf", App: id}}, true},
		{"bad app ID", []approvedFileType{{Ext: ".pdf", App: "bad"}}, false},
		{"duplicates", []approvedFileType{{Ext: ".pdf"}, {Ext: ".pdf", App: id}}, false},
		{"too many", make([]approvedFileType, 33), false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			prefs := approvedFileTypePreferences{SchemaVersion: 1, Types: tc.entries}
			if err := prefs.validate(); (err == nil) != tc.ok {
				t.Fatalf("validate = %v", err)
			}
		})
	}
	for _, ext := range []string{"", ".", "pdf", ".PDF", ".p df", ".pdf\x00", ".p_df", ".p-df", ".é", ".mKv", ".K", ".unknown", ".hwpx", "." + strings.Repeat("a", 16), ".exe", ".docm", ".rtf", ".zip"} {
		prefs := approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{{Ext: ext, App: id}}}
		if err := prefs.validate(); err == nil {
			t.Errorf("accepted %q", ext)
		}
	}
	if err := (approvedFileTypePreferences{SchemaVersion: 2}).validate(); err == nil {
		t.Fatal("accepted unknown schema")
	}
}

func TestApprovedFileTypeOpener(t *testing.T) {
	id := strings.Repeat("b", 32)
	apps := approvedAppPreferences{SchemaVersion: 1, Apps: []approvedWindowsApp{{ID: id, Name: "Reader", Path: `C:\Apps\reader.exe`}}}
	prefs := approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{{Ext: ".pdf", App: id}, {Ext: ".txt"}, {Ext: ".mkv"}}}
	for _, tc := range []struct {
		rel, want string
		ok        bool
	}{
		{"folder/Report.PDF", apps.Apps[0].Path, true}, {"notes.txt", "", true},
		{"movie.MKV", "", true}, {"movie.mKv", "", false}, {"file.exe", "", false},
		{"../notes.txt", "", false}, {"notes.txt:stream", "", false},
	} {
		got, err := prefs.opener(tc.rel, apps)
		if (err == nil) != tc.ok || got != tc.want {
			t.Errorf("opener(%q) = %q, %v", tc.rel, got, err)
		}
	}
	apps.Apps = nil
	if _, err := prefs.opener("report.pdf", apps); err == nil {
		t.Fatal("revoked app fell back to default")
	}
	prefs.Types = append(prefs.Types, approvedFileType{Ext: ".exe"})
	if _, err := prefs.opener("notes.txt", apps); err == nil {
		t.Fatal("open did not revalidate preferences")
	}
}

func TestApprovedFileTypesStoreAndLine(t *testing.T) {
	dir := t.TempDir()
	loaded, err := loadApprovedFileTypes(dir)
	if err != nil || len(loaded.Types) != 0 {
		t.Fatalf("missing file: %+v, %v", loaded, err)
	}
	prefs := approvedFileTypePreferences{Types: []approvedFileType{{Ext: ".pdf", App: strings.Repeat("c", 32)}, {Ext: ".txt"}}}
	if err := saveApprovedFileTypes(dir, prefs); err != nil {
		t.Fatal(err)
	}
	loaded, err = loadApprovedFileTypes(dir)
	if err != nil || loaded.SchemaVersion != 1 || !slices.Equal(loaded.Types, prefs.Types) {
		t.Fatalf("round trip: %+v, %v", loaded, err)
	}
	want := "file-types " + base64.StdEncoding.EncodeToString([]byte(`[".pdf",".txt"]`)) + "\n"
	if got := fileTypesLine(loaded); got != want {
		t.Fatalf("line = %q", got)
	}
	if got := fileTypesLine(approvedFileTypePreferences{SchemaVersion: 1}); got != "file-types W10=\n" {
		t.Fatal(got)
	}
	if got := fileTypesLine(approvedFileTypePreferences{}); got != "file-types W10=\n" {
		t.Fatal(got)
	}
	if err := saveApprovedFileTypes(dir, approvedFileTypePreferences{}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(approvedFileTypesPath(dir))
	if err != nil || !strings.Contains(string(data), `"types": []`) {
		t.Fatalf("empty store = %s, %v", data, err)
	}
	prefs.Types = []approvedFileType{{Ext: ".exe"}}
	if err := saveApprovedFileTypes(dir, prefs); err == nil {
		t.Fatal("saved unsafe type")
	}
}

func TestApprovedFileTypesRejectInvalidFiles(t *testing.T) {
	dir := t.TempDir()
	for _, data := range []string{
		`{`, `{"schemaVersion":2,"types":[]}`, `{"schemaVersion":1,"types":[],"extra":true}`,
		`{"schemaVersion":1,"types":[{"ext":".pdf","app":"","extra":true}]}`,
		`{"schemaVersion":1,"types":[]} {}`, `{"schemaVersion":1,"types":[{"ext":".exe","app":""}]}`,
		`{"schemaVersion":1,"types":[{"ext":".pdf","app":""},{"ext":".pdf","app":""}]}`,
		strings.Repeat(" ", maximumApprovedFileTypesBytes+1),
	} {
		if err := os.WriteFile(approvedFileTypesPath(dir), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		prefs, err := loadApprovedFileTypes(dir)
		if err == nil || len(prefs.Types) != 0 {
			t.Fatalf("invalid file retained approvals: %+v, %v", prefs, err)
		}
	}
	if err := os.Remove(approvedFileTypesPath(dir)); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(approvedFileTypesPath(dir), 0700); err != nil {
		t.Fatal(err)
	}
	if _, err := loadApprovedFileTypes(dir); err == nil {
		t.Fatal("accepted directory")
	}
}

func TestParseOpenFileRequest(t *testing.T) {
	longPath := strings.Repeat(strings.Repeat("a", 1023)+"/", 3) + strings.Repeat("b", 1020) + ".txt"
	if len(longPath) != maximumOpenFileBytes {
		t.Fatal("bad boundary fixture")
	}
	for _, rel := range []string{"notes.txt", "folder/Report.PDF", "문서/보고서.pdf", "a space/file name.txt", "a/../file.txt", ""} {
		want := rel != "" && rel != "a/../file.txt"
		got, ok := parseOpenFileRequest(openFileTestLine(rel))
		if ok != want || ok && got != rel {
			t.Errorf("%q: %q, %t", rel, got, ok)
		}
	}
	if got, ok := parseOpenFileRequest(openFileTestLine(longPath)); !ok || got != longPath {
		t.Fatal("rejected length boundary")
	}
	for _, rel := range []string{
		"", "../file.txt", "a/../file.txt", "./file.txt", ".", "..", "/file.txt", "//host/file.txt", "a//file.txt", "a/", "a/./file.txt",
		`a\file.txt`, `C:/file.txt`, "file.txt:stream", "file.txt:", "file.txt.", "file.txt ", "a./file.txt", "a /file.txt",
		"CON", "nul.txt", "COM0", "COM9.txt", "LPT0", "LPT¹", "com².txt", "COM³.txt", "LPT².txt", "lpt³.txt", "CONIN$", "COM1 .txt", "NUL .pdf", "lpt¹ .md", "AUX  .csv", "CONOUT$.txt",
		"file\x00.txt", "file\n.txt", "file\r.txt", "file\t.txt", "file\x1f.txt", "file\x7f.txt", "file\u0085.txt", "file\xff.txt",
		"a<.txt", "a>.txt", "a|.txt", "a?.txt", "a*.txt", "a\".txt", strings.Repeat("a", 1025), longPath + "x",
	} {
		if got, ok := parseOpenFileRequest(openFileTestLine(rel)); ok {
			t.Errorf("accepted invalid path %q as %q", rel, got)
		}
	}
	for _, line := range []string{
		"open-file \n", "open-file !!!!\n", "open-file Zg\n", "open-file Zg=\n", "open-file Zh==\n", "open-file Zg===\n",
		"open-file Zg==\r\n", "open-file Z\ng==\n", "open-file Zg== \n", "open-file /w==\n", "open-file Zg==", "open-file Zg==\n\n", "other Zg==\n",
	} {
		if _, ok := parseOpenFileRequest(line); ok {
			t.Errorf("accepted malformed line %q", line)
		}
	}
}

func TestFileTypesLineContainsOnlyExtensions(t *testing.T) {
	prefs := approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{{Ext: ".pdf", App: strings.Repeat("d", 32)}}}
	line := fileTypesLine(prefs)
	data, err := base64.StdEncoding.DecodeString(strings.TrimSuffix(strings.TrimPrefix(line, "file-types "), "\n"))
	var exts []string
	if err != nil || json.Unmarshal(data, &exts) != nil || !slices.Equal(exts, []string{".pdf"}) {
		t.Fatalf("wire payload = %s, %v", data, err)
	}
}
