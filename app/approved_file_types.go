package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"slices"
	"strings"
	"unicode"
	"unicode/utf8"
)

const approvedFileTypesFilename = "approved-file-types.json"
const maximumApprovedFileTypes = 32
const maximumApprovedFileTypesBytes = 64 << 10
const maximumOpenFileBytes = 4096

var reviewedFileTypes = []string{
	".pdf", ".txt", ".md", ".csv", ".docx", ".xlsx", ".pptx",
	".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".mp3", ".wav",
	".flac", ".mp4", ".mkv", ".webm",
}

// ReviewedFileTypes returns a copy of the catalog for Settings. An approved
// executable never grants permission to open a type outside this catalog.
func ReviewedFileTypes() []string { return slices.Clone(reviewedFileTypes) }

var neverAllowedFileTypes = strings.Fields(`
.exe .com .bat .cmd .ps1 .psm1 .psd1 .ps1xml .vbs .vbe .js .jse .wsf .wsh
.hta .msi .msp .msu .scr .pif .cpl .msc .reg .chm .jar .lnk .url .appref-ms
.settingcontent-ms .appx .appxbundle .msix .msixbundle .application .dll .sys
.ocx .inf .py .pyw .sh .iso .img .vhd .vhdx
.docm .dotm .xlsm .xltm .xlam .xlsb .pptm .potm .ppsm .ppam .sldm
.doc .dot .xls .xlt .xla .ppt .pot .pps .ppa .slk .iqy .odt .ods .odp .odg .hwp
.svg .html .htm .mht .mhtml .rtf .zip`)

type approvedFileType struct {
	Ext string `json:"ext"`
	App string `json:"app"`
}

type approvedFileTypePreferences struct {
	SchemaVersion int                `json:"schemaVersion"`
	Types         []approvedFileType `json:"types"`
}

func approvedFileTypesPath(dir string) string {
	return filepath.Join(dir, approvedFileTypesFilename)
}

func validFileTypeExtension(ext string) bool {
	if len(ext) < 2 || len(ext) > 16 || ext[0] != '.' {
		return false
	}
	for i := 1; i < len(ext); i++ {
		c := ext[i]
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9') {
			return false
		}
	}
	return !slices.Contains(neverAllowedFileTypes, ext) && slices.Contains(reviewedFileTypes, ext)
}

func (prefs approvedFileTypePreferences) validate() error {
	if prefs.SchemaVersion != 1 || len(prefs.Types) > maximumApprovedFileTypes {
		return fmt.Errorf("invalid allowed file types file")
	}
	seen := make(map[string]bool, len(prefs.Types))
	for _, entry := range prefs.Types {
		if !validFileTypeExtension(entry.Ext) || seen[entry.Ext] || entry.App != "" && !validApprovedAppID(entry.App) {
			return fmt.Errorf("invalid allowed file type")
		}
		seen[entry.Ext] = true
	}
	return nil
}

func loadApprovedFileTypes(dir string) (approvedFileTypePreferences, error) {
	empty := approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{}}
	data, err := readBoundedRegularFile(approvedFileTypesPath(dir), maximumApprovedFileTypesBytes)
	if errors.Is(err, os.ErrNotExist) {
		return empty, nil
	}
	if err != nil {
		return empty, err
	}
	var prefs approvedFileTypePreferences
	if err := decodeStrictJSON(data, &prefs); err != nil {
		return empty, err
	}
	if err := prefs.validate(); err != nil {
		return empty, err
	}
	return prefs, nil
}

func saveApprovedFileTypes(dir string, prefs approvedFileTypePreferences) error {
	prefs.SchemaVersion = 1
	if prefs.Types == nil {
		prefs.Types = []approvedFileType{}
	}
	if err := prefs.validate(); err != nil {
		return err
	}
	data, err := json.MarshalIndent(prefs, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	return replaceFile(approvedFileTypesPath(dir), append(data, '\n'))
}

// opener validates the current preferences and resolves an explicit mapping
// against the current approved apps. Revocation never selects the default app.
func (prefs approvedFileTypePreferences) opener(rel string, apps approvedAppPreferences) (string, error) {
	if err := prefs.validate(); err != nil {
		return "", err
	}
	if !validOpenFilePath(rel) {
		return "", fmt.Errorf("invalid relative file path")
	}
	// Lowercase ASCII only; Unicode folding would turn the Kelvin sign into k.
	ext := []byte(path.Ext(rel))
	for i, c := range ext {
		if c >= 'A' && c <= 'Z' {
			ext[i] = c + 'a' - 'A'
		}
	}
	for _, entry := range prefs.Types {
		if entry.Ext != string(ext) {
			continue
		}
		if entry.App == "" {
			return "", nil
		}
		if err := apps.validate(); err != nil {
			return "", fmt.Errorf("invalid approved apps file")
		}
		for _, app := range apps.Apps {
			if app.ID == entry.App {
				return app.Path, nil
			}
		}
		return "", fmt.Errorf("file opener is no longer approved")
	}
	return "", fmt.Errorf("file type is not allowed")
}

func fileTypesLine(prefs approvedFileTypePreferences) string {
	exts := []string{}
	if prefs.validate() == nil {
		for _, entry := range prefs.Types {
			exts = append(exts, entry.Ext)
		}
	}
	data, _ := json.Marshal(exts)
	return "file-types " + base64.StdEncoding.EncodeToString(data) + "\n"
}

func validOpenFilePath(rel string) bool {
	if rel == "" || len(rel) > maximumOpenFileBytes || !utf8.ValidString(rel) || strings.HasSuffix(rel, "/") {
		return false
	}
	for _, c := range rel {
		if unicode.IsControl(c) {
			return false
		}
	}
	for _, part := range strings.Split(rel, "/") {
		if !clipboardFileName(part) {
			return false
		}
		// Win32 drops spaces before the extension, so "NUL .txt" is still a device.
		base := strings.TrimRight(strings.ToUpper(strings.SplitN(part, ".", 2)[0]), " ")
		if slices.Contains([]string{"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}, base) {
			return false
		}
		for _, prefix := range []string{"COM", "LPT"} {
			if suffix, ok := strings.CutPrefix(base, prefix); ok && (len(suffix) == 1 && suffix[0] >= '0' && suffix[0] <= '9' || suffix == "¹" || suffix == "²" || suffix == "³") {
				return false
			}
		}
	}
	return true
}

func parseOpenFileRequest(line string) (rel string, ok bool) {
	payload, ok := strings.CutPrefix(line, "open-file ")
	if !ok || !strings.HasSuffix(payload, "\n") {
		return "", false
	}
	payload = strings.TrimSuffix(payload, "\n")
	if payload == "" || len(payload) > base64.StdEncoding.EncodedLen(maximumOpenFileBytes) || strings.ContainsAny(payload, "\r\n") {
		return "", false
	}
	data, err := base64.StdEncoding.Strict().DecodeString(payload)
	if err != nil || !validOpenFilePath(string(data)) {
		return "", false
	}
	return string(data), true
}
