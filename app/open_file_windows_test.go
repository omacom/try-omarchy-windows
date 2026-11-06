//go:build windows

package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// openFileTempDir returns a test directory without 8.3 components. CI temp paths
// can contain them, and validateSharedFile refuses such a share.
func openFileTempDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	name, err := syscall.UTF16PtrFromString(dir)
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]uint16, 32768)
	n, _, callErr := kernel32.NewProc("GetLongPathNameW").Call(uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if n == 0 || n >= uintptr(len(buf)) {
		t.Fatalf("GetLongPathNameW: %v", callErr)
	}
	return syscall.UTF16ToString(buf[:n])
}

func writeOpenFileFixture(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("shared file test"), 0600); err != nil {
		t.Fatal(err)
	}
}

func makeOpenFileJunction(t *testing.T, link, target string) {
	t.Helper()
	cmd := exec.Command(system32("cmd.exe"), "/d", "/c", "mklink", "/J", link, target)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("mklink /J: %v: %s", err, output)
	}
	t.Cleanup(func() {
		if err := os.Remove(link); err != nil {
			t.Errorf("remove junction: %v", err)
		}
	})
}

func TestValidateSharedFile(t *testing.T) {
	share := openFileTempDir(t)
	writeOpenFileFixture(t, filepath.Join(share, "folder", "Report.txt"))
	for _, rel := range []string{"folder/Report.txt", "folder/report.TXT"} {
		got, err := validateSharedFile(share, rel)
		if err != nil || !openFileOrdinalEqual(got, filepath.Join(share, "folder", "Report.txt")) {
			t.Fatalf("%q: %q, %v", rel, got, err)
		}
		if strings.HasPrefix(got, `\\?\`) {
			t.Fatal("extended prefix passed to opener")
		}
	}
	for _, rel := range []string{"folder", "folder/missing.txt", "../outside.txt", "folder/Report.txt:stream"} {
		if got, err := validateSharedFile(share, rel); err == nil {
			t.Errorf("accepted %q: %q", rel, got)
		}
	}
	for _, root := range []string{"", ".", `\\server\share`, `\\?\C:\share`, share + `\..`, share + `\`} {
		if _, err := validateSharedFile(root, "folder/Report.txt"); err == nil {
			t.Errorf("accepted share %q", root)
		}
	}
	if _, ok := parseOpenFileRequest(openFileTestLine("folder/Report.txt:stream")); ok {
		t.Fatal("ADS survived request parsing")
	}
}

func TestValidateSharedFileRejectsSymlinks(t *testing.T) {
	share := openFileTempDir(t)
	writeOpenFileFixture(t, filepath.Join(share, "real", "file.txt"))
	link := filepath.Join(share, "link.txt")
	if err := os.Symlink(filepath.Join(share, "real", "file.txt"), link); err != nil {
		if errors.Is(err, syscall.Errno(1314)) || errors.Is(err, syscall.ERROR_ACCESS_DENIED) {
			t.Skip("symlink privilege unavailable")
		}
		t.Fatal(err)
	}
	if _, err := validateSharedFile(share, "link.txt"); err == nil {
		t.Fatal("accepted file symlink")
	}
	dirLink := filepath.Join(share, "link-dir")
	if err := os.Symlink(filepath.Join(share, "real"), dirLink); err != nil {
		t.Fatal(err)
	}
	if _, err := validateSharedFile(share, "link-dir/file.txt"); err == nil {
		t.Fatal("accepted intermediate symlink")
	}
	if _, err := validateSharedFile(dirLink, "file.txt"); err == nil {
		t.Fatal("accepted symlink share root")
	}
}

func TestValidateSharedFileRejectsJunctions(t *testing.T) {
	base := openFileTempDir(t)
	share := filepath.Join(base, "share")
	target := filepath.Join(base, "target")
	writeOpenFileFixture(t, filepath.Join(share, "file.txt"))
	writeOpenFileFixture(t, filepath.Join(target, "nested", "file.txt"))
	middle := filepath.Join(share, "junction")
	makeOpenFileJunction(t, middle, target)
	if _, err := validateSharedFile(share, "file.txt"); err != nil {
		t.Fatalf("ordinary file in share with junction: %v", err)
	}
	if _, err := validateSharedFile(share, "junction/nested/file.txt"); err == nil {
		t.Fatal("accepted junction in requested path")
	}
	if _, err := validateSharedFile(middle, "nested/file.txt"); err == nil {
		t.Fatal("accepted junction share root")
	}
	if _, err := validateSharedFile(filepath.Join(middle, "nested"), "file.txt"); err == nil {
		t.Fatal("accepted junction above share root")
	}
}

func TestValidateSharedFileRejectsHardLinks(t *testing.T) {
	base := openFileTempDir(t)
	share := filepath.Join(base, "share")
	original := filepath.Join(share, "file.txt")
	writeOpenFileFixture(t, original)
	if err := os.Link(original, filepath.Join(base, "outside.txt")); err != nil {
		t.Fatal(err)
	}
	if _, err := validateSharedFile(share, "file.txt"); err == nil {
		t.Fatal("accepted multiply linked file")
	}
}

func TestValidateSharedFileRejectsShortNames(t *testing.T) {
	share := openFileTempDir(t)
	longName := filepath.Join(share, "Long directory name", "Long document name.txt")
	writeOpenFileFixture(t, longName)
	proc := kernel32.NewProc("GetShortPathNameW")
	name, err := syscall.UTF16PtrFromString(longName)
	if err != nil {
		t.Fatal(err)
	}
	buf := make([]uint16, 32768)
	n, _, callErr := proc.Call(uintptr(unsafe.Pointer(name)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
	if n == 0 || n >= uintptr(len(buf)) {
		t.Fatalf("GetShortPathNameW: %v", callErr)
	}
	short := syscall.UTF16ToString(buf[:n])
	shortFile, shortDir := filepath.Base(short), filepath.Base(filepath.Dir(short))
	fileAlias := !openFileOrdinalEqual(shortFile, filepath.Base(longName))
	dirAlias := !openFileOrdinalEqual(shortDir, filepath.Base(filepath.Dir(longName)))
	if !fileAlias && !dirAlias {
		t.Skip("8.3 short names are disabled")
	}
	if fileAlias {
		if _, err := validateSharedFile(share, "Long directory name/"+shortFile); err == nil {
			t.Fatal("accepted file short name")
		}
	}
	if dirAlias {
		if _, err := validateSharedFile(filepath.Dir(short), "Long document name.txt"); err == nil {
			t.Fatal("accepted share with short name")
		}
		if _, err := validateSharedFile(share, shortDir+"/Long document name.txt"); err == nil {
			t.Fatal("accepted directory short name")
		}
	}
}

func TestOpenFileReparseTagPolicy(t *testing.T) {
	if !allowedOpenFileReparseTag(0, 0) {
		t.Fatal("rejected ordinary file")
	}
	for i := uint32(0); i < 16; i++ {
		if !allowedOpenFileReparseTag(fileAttributeReparsePoint, 0x9000001A|(i<<12)) {
			t.Errorf("rejected cloud tag %d", i)
		}
	}
	for _, tag := range []uint32{0, ioReparseTagSymlink, ioReparseTagMountPoint, 0x8000001B, 0x9001001A, 0xA000001A} {
		if allowedOpenFileReparseTag(fileAttributeReparsePoint, tag) {
			t.Errorf("accepted reparse tag %#x", tag)
		}
	}
}

func TestOpenFileFinalPathAndOrdinalComparison(t *testing.T) {
	if !openFileOrdinalEqual(`C:\share\보고서.TXT`, `c:\SHARE\보고서.txt`) {
		t.Fatal("ordinal case comparison failed")
	}
	for _, other := range []string{`C:\share-other\file.txt`, `C:\share\FILE~1.TXT`, `C:\elsewhere\file.txt`} {
		if openFileOrdinalEqual(`C:\share\file.txt`, other) {
			t.Errorf("accepted different path %s", other)
		}
	}
	for _, path := range []string{`\\?\UNC\server\share\file.txt`, `\\.\C:\file.txt`, `C:\file.txt`, `\\?\Volume{test}\file.txt`, `\\?\C:file.txt`} {
		if _, ok := localOpenFilePath(path); ok {
			t.Errorf("accepted non-DOS final path %s", path)
		}
	}
}

func TestFileOpenersRejectInterpreters(t *testing.T) {
	dir := t.TempDir()
	for _, name := range strings.Fields("python pythonw python3 python3.14 python-custom py pyw powershell pwsh cmd wscript cscript mshta rundll32 regsvr32 node java javaw bash wsl conhost") {
		path := filepath.Join(dir, strings.ToUpper(name)+".EXE")
		writeOpenFileFixture(t, path)
		if err := validateFileOpener(path); err == nil {
			t.Errorf("accepted interpreter %s", name)
		}
	}
	reader := filepath.Join(dir, "reader.exe")
	writeOpenFileFixture(t, reader)
	if err := validateFileOpener(reader); err != nil {
		t.Fatal(err)
	}
	if err := validateFileOpener(filepath.Join(dir, "missing.exe")); err == nil {
		t.Fatal("accepted missing executable")
	}
}

func TestOpenSharedFileRequiresCurrentSessionShare(t *testing.T) {
	previous := bootShare.Load()
	bootShare.Store(nil)
	t.Cleanup(func() { bootShare.Store(previous) })
	if err := openSharedWindowsFile(t.TempDir(), "file.txt"); err == nil {
		t.Fatal("opened without current session share")
	}
}

func TestOpenSharedFilePolicyRefusals(t *testing.T) {
	share := openFileTempDir(t)
	previous := bootShare.Load()
	bootShare.Store(&share)
	t.Cleanup(func() { bootShare.Store(previous) })
	// No target file exists and the fake executables cannot run. Assert the
	// policy error so filesystem refusal cannot mask a missing policy check.
	external := t.TempDir()
	interpreter := filepath.Join(external, "python3.exe")
	inside := filepath.Join(share, "reader.exe")
	writeOpenFileFixture(t, interpreter)
	writeOpenFileFixture(t, inside)
	id := strings.Repeat("a", 32)
	for _, tc := range []struct {
		name, ext, appID, appPath, want string
	}{
		{"type not allowed", ".pdf", "", "", "file type is not allowed"},
		{"revoked app", ".txt", id, "", "file opener is no longer approved"},
		{"interpreter", ".txt", id, interpreter, "interpreters cannot be file openers"},
		{"app in share", ".txt", id, inside, "file opener overlaps the running share"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			dir := t.TempDir()
			apps := approvedAppPreferences{SchemaVersion: 1}
			if tc.appPath != "" {
				apps.Apps = []approvedWindowsApp{{ID: id, Name: "Opener", Path: tc.appPath}}
			}
			if err := saveApprovedWindowsApps(dir, apps); err != nil {
				t.Fatal(err)
			}
			if err := saveApprovedFileTypes(dir, approvedFileTypePreferences{Types: []approvedFileType{{Ext: tc.ext, App: tc.appID}}}); err != nil {
				t.Fatal(err)
			}
			if err := openSharedWindowsFile(dir, "file.txt"); err == nil || err.Error() != tc.want {
				t.Fatalf("policy refusal = %v, want %q", err, tc.want)
			}
		})
	}
}

func TestFileTypesSettingsNative(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires an interactive Windows desktop and a candidate launcher")
	}
	dir := t.TempDir()
	appID := strings.Repeat("a", 32)
	if err := saveApprovedWindowsApps(dir, approvedAppPreferences{Apps: []approvedWindowsApp{{ID: appID, Name: "Example", Path: `C:\Example.exe`}}}); err != nil {
		t.Fatal(err)
	}
	session := func(edit func(window, list uintptr)) {
		t.Helper()
		cmd := exec.Command(launcher, "-dir", dir, "-settings")
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		done := make(chan error, 1)
		go func() { done <- cmd.Wait() }()
		defer cmd.Process.Kill()
		class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
		var window, list uintptr
		for deadline := time.Now().Add(15 * time.Second); list == 0 && time.Now().Before(deadline); time.Sleep(25 * time.Millisecond) {
			window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
			if window != 0 {
				var owner uint32
				procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
				if owner == uint32(cmd.Process.Pid) {
					list = findSettingsControl(window, settingsFileListID)
				}
			}
		}
		if list == 0 {
			t.Fatal("file types section did not appear")
		}
		procSendMessageW.Call(window, wmCommand, settingsPageBase+4, 0)
		edit(window, list)
		procPostMessageW.Call(window, wmCommand, settingsSaveID, 0)
		select {
		case err := <-done:
			if err != nil {
				t.Fatal(err)
			}
		case <-time.After(10 * time.Second):
			t.Fatal("file types settings did not save")
		}
	}
	const lbGetCount, lbSetCurSel, cbSetCurSel = 0x18B, 0x186, 0x14E
	session(func(window, list uintptr) {
		for opener := uintptr(0); opener < 2; opener++ {
			procSendMessageW.Call(findSettingsControl(window, settingsFileTypeID), cbSetCurSel, 0, 0)
			procSendMessageW.Call(findSettingsControl(window, settingsFileOpenerID), cbSetCurSel, opener, 0)
			procSendMessageW.Call(window, wmCommand, settingsFileAddID, 0)
		}
		if count, _, _ := procSendMessageW.Call(list, lbGetCount, 0, 0); count != 2 {
			t.Fatalf("file type rows=%d", count)
		}
	})
	loaded, err := loadApprovedFileTypes(dir)
	if err != nil || len(loaded.Types) != 2 || loaded.Types[0].App != "" || loaded.Types[1].App != appID {
		t.Fatalf("saved file types: %+v, %v", loaded, err)
	}
	session(func(window, list uintptr) {
		if count, _, _ := procSendMessageW.Call(list, lbGetCount, 0, 0); count != 2 {
			t.Fatalf("reopened file type rows=%d", count)
		}
		apps := findSettingsControl(window, settingsAppListID)
		procSendMessageW.Call(apps, lbSetCurSel, 0, 0)
		procSendMessageW.Call(window, wmCommand, settingsAppRemoveID, 0)
		if count, _, _ := procSendMessageW.Call(list, lbGetCount, 0, 0); count != 1 {
			t.Fatalf("rows after removing app=%d", count)
		}
		procSendMessageW.Call(list, lbSetCurSel, 0, 0)
		procSendMessageW.Call(window, wmCommand, settingsFileRemoveID, 0)
		if count, _, _ := procSendMessageW.Call(list, lbGetCount, 0, 0); count != 0 {
			t.Fatalf("rows after removing type=%d", count)
		}
	})
	loaded, err = loadApprovedFileTypes(dir)
	if err != nil || len(loaded.Types) != 0 {
		t.Fatalf("removed file types still allowed: %+v, %v", loaded, err)
	}
}

// TestOpenSharedFileNative opens real windows, so the caller closes them.
func TestOpenSharedFileNative(t *testing.T) {
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop")
	}
	dir := t.TempDir()
	share := openFileTempDir(t)
	previous := bootShare.Load()
	bootShare.Store(&share)
	t.Cleanup(func() { bootShare.Store(previous) })
	writeOpenFileFixture(t, filepath.Join(share, "folder", "한글 default opener.txt"))
	writeOpenFileFixture(t, filepath.Join(share, "folder", "linked opener.md"))
	appID := strings.Repeat("a", 32)
	if err := saveApprovedWindowsApps(dir, approvedAppPreferences{Apps: []approvedWindowsApp{{ID: appID, Name: "Notepad", Path: system32("notepad.exe")}}}); err != nil {
		t.Fatal(err)
	}
	if err := saveApprovedFileTypes(dir, approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{{Ext: ".txt"}, {Ext: ".md", App: appID}}}); err != nil {
		t.Fatal(err)
	}
	for _, rel := range []string{"folder/한글 default opener.txt", "folder/linked opener.md"} {
		if err := openSharedWindowsFile(dir, rel); err != nil {
			t.Fatalf("%s: %v", rel, err)
		}
	}
	time.Sleep(4 * time.Second)
}

func TestFileOpenerInsideShareRefusedNative(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires an interactive Windows desktop and a candidate launcher")
	}
	dir := t.TempDir()
	share := openFileTempDir(t)
	appPath := filepath.Join(share, "tools", "viewer.exe")
	writeOpenFileFixture(t, appPath)
	if err := saveSettings(settingsPath(dir), settings{SchemaVersion: settingsSchemaVersion, MemoryMiB: 4096, CPUs: 2, Share: share}); err != nil {
		t.Fatal(err)
	}
	if err := saveApprovedWindowsApps(dir, approvedAppPreferences{Apps: []approvedWindowsApp{{ID: strings.Repeat("b", 32), Name: "Viewer", Path: appPath}}}); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(launcher, "-dir", dir, "-settings")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer cmd.Process.Kill()
	owned := func(window uintptr) bool {
		var owner uint32
		procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
		return owner == uint32(cmd.Process.Pid)
	}
	class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var window, list uintptr
	for deadline := time.Now().Add(15 * time.Second); list == 0 && time.Now().Before(deadline); time.Sleep(25 * time.Millisecond) {
		window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		if window != 0 && owned(window) {
			list = findSettingsControl(window, settingsFileListID)
		}
	}
	if list == 0 {
		t.Fatal("file types section did not appear")
	}
	procSendMessageW.Call(window, wmCommand, settingsPageBase+4, 0)
	procSendMessageW.Call(findSettingsControl(window, settingsFileTypeID), 0x14E, 0, 0)   // CB_SETCURSEL
	procSendMessageW.Call(findSettingsControl(window, settingsFileOpenerID), 0x14E, 1, 0) // the app inside the share
	procPostMessageW.Call(window, wmCommand, settingsFileAddID, 0)
	dialogClass, _ := syscall.UTF16PtrFromString("#32770")
	title, _ := syscall.UTF16PtrFromString(appTitle)
	var box uintptr
	for deadline := time.Now().Add(5 * time.Second); box == 0 && time.Now().Before(deadline); time.Sleep(25 * time.Millisecond) {
		if found, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(dialogClass)), uintptr(unsafe.Pointer(title))); found != 0 && owned(found) {
			box = found
		}
	}
	if box == 0 {
		t.Fatal("no error for an opener inside the share")
	}
	procPostMessageW.Call(box, wmCommand, 1, 0) // IDOK on the message box only
	time.Sleep(300 * time.Millisecond)
	if count, _, _ := procSendMessageW.Call(list, 0x18B, 0, 0); count != 0 { // LB_GETCOUNT
		t.Fatalf("opener inside the share was added: rows=%d", count)
	}
}
