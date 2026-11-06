//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strings"
	"syscall"
	"unsafe"
)

var (
	procOpenFileNtCreateFile   = syscall.NewLazyDLL("ntdll.dll").NewProc("NtCreateFile")
	procOpenFileAttributeInfo  = kernel32.NewProc("GetFileInformationByHandleEx")
	procOpenFileFinalPath      = kernel32.NewProc("GetFinalPathNameByHandleW")
	procOpenFileCompareOrdinal = kernel32.NewProc("CompareStringOrdinal")
)

const (
	openFileReadAttributes = 0x80
	openFileTraverse       = 0x20
	openFileSynchronize    = 0x100000
	openFileDirectory      = 0x1
	openFileNonDirectory   = 0x40
	openFileSynchronous    = 0x20       // FILE_SYNCHRONOUS_IO_NONALERT
	openFileReparsePoint   = 0x00200000 // FILE_OPEN_REPARSE_POINT (NT, not Win32 flags)
	openFileShareMode      = syscall.FILE_SHARE_READ | syscall.FILE_SHARE_WRITE | syscall.FILE_SHARE_DELETE
)

// These layouts use pointer-sized fields so the NT ABI's alignment is also
// correct when cross-compiling. UNICODE_STRING lengths count bytes, not runes.
type openFileUnicodeString struct {
	Length, MaximumLength uint16
	Buffer                *uint16
}

type openFileObjectAttributes struct {
	Length                                       uint32
	RootDirectory                                syscall.Handle
	ObjectName                                   *openFileUnicodeString
	Attributes                                   uint32
	SecurityDescriptor, SecurityQualityOfService uintptr
}

type openFileIOStatusBlock struct {
	Status, Information uintptr
}

func openFileRelative(parent syscall.Handle, component string, directory bool) (syscall.Handle, error) {
	name, err := syscall.UTF16FromString(component)
	if err != nil || len(name)*2 > 65535 {
		return syscall.InvalidHandle, fmt.Errorf("invalid file component")
	}
	unicodeName := openFileUnicodeString{
		Length: uint16((len(name) - 1) * 2), MaximumLength: uint16(len(name) * 2), Buffer: &name[0],
	}
	attributes := openFileObjectAttributes{
		RootDirectory: parent, ObjectName: &unicodeName, Attributes: 0x40, // OBJ_CASE_INSENSITIVE
	}
	attributes.Length = uint32(unsafe.Sizeof(attributes))
	access := uint32(openFileReadAttributes | openFileSynchronize)
	options := uint32(openFileReparsePoint | openFileSynchronous | openFileNonDirectory)
	if directory {
		access |= openFileTraverse
		options = openFileReparsePoint | openFileSynchronous | openFileDirectory
	}
	var handle syscall.Handle
	var status openFileIOStatusBlock
	r, _, _ := procOpenFileNtCreateFile.Call(
		uintptr(unsafe.Pointer(&handle)), uintptr(access), uintptr(unsafe.Pointer(&attributes)),
		uintptr(unsafe.Pointer(&status)), 0, 0, openFileShareMode, 1, // FILE_OPEN
		uintptr(options), 0, 0,
	)
	runtime.KeepAlive(name)
	runtime.KeepAlive(unicodeName)
	runtime.KeepAlive(attributes)
	if int32(r) < 0 {
		return syscall.InvalidHandle, fmt.Errorf("cannot open file component (NTSTATUS %#x)", uint32(r))
	}
	return handle, nil
}

func allowedOpenFileReparseTag(attributes, tag uint32) bool {
	// OneDrive placeholders do not redirect to another path. No other
	// reparse tags, including unknown future tags, are trusted here.
	return attributes&fileAttributeReparsePoint == 0 || tag&0xFFFF0FFF == 0x9000001A
}

func checkOpenFileHandle(handle syscall.Handle, directory bool) error {
	var tagInfo struct{ Attributes, Tag uint32 }
	r, _, _ := procOpenFileAttributeInfo.Call(uintptr(handle), 9, // FileAttributeTagInfo
		uintptr(unsafe.Pointer(&tagInfo)), unsafe.Sizeof(tagInfo))
	if r == 0 {
		return fmt.Errorf("cannot read file reparse tag")
	}
	if !allowedOpenFileReparseTag(tagInfo.Attributes, tagInfo.Tag) {
		return fmt.Errorf("link or unsupported reparse point")
	}
	if (tagInfo.Attributes&syscall.FILE_ATTRIBUTE_DIRECTORY != 0) != directory {
		return fmt.Errorf("unexpected file component type")
	}
	return nil
}

func openFileWin32Directory(path string) (syscall.Handle, error) {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return syscall.InvalidHandle, fmt.Errorf("invalid share path")
	}
	handle, err := syscall.CreateFile(name, openFileReadAttributes|openFileTraverse|openFileSynchronize,
		openFileShareMode, nil, syscall.OPEN_EXISTING,
		syscall.FILE_FLAG_OPEN_REPARSE_POINT|syscall.FILE_FLAG_BACKUP_SEMANTICS, 0)
	if err != nil {
		return syscall.InvalidHandle, fmt.Errorf("cannot open share directory")
	}
	if err := checkOpenFileHandle(handle, true); err != nil {
		syscall.CloseHandle(handle)
		return syscall.InvalidHandle, err
	}
	return handle, nil
}

func openFileFinalPath(handle syscall.Handle) (string, error) {
	// FILE_NAME_NORMALIZED | VOLUME_NAME_DOS are both zero. Never fall back
	// to FILE_NAME_OPENED, which would hide short names or normalization.
	buf := make([]uint16, 32768)
	n, _, _ := procOpenFileFinalPath.Call(uintptr(handle), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)), 0)
	if n == 0 || n >= uintptr(len(buf)) {
		return "", fmt.Errorf("cannot resolve final file path")
	}
	return syscall.UTF16ToString(buf[:n]), nil
}

func openFileOrdinalEqual(a, b string) bool {
	left, err := syscall.UTF16FromString(a)
	if err != nil {
		return false
	}
	right, err := syscall.UTF16FromString(b)
	if err != nil {
		return false
	}
	r, _, _ := procOpenFileCompareOrdinal.Call(uintptr(unsafe.Pointer(&left[0])), uintptr(len(left)-1),
		uintptr(unsafe.Pointer(&right[0])), uintptr(len(right)-1), 1)
	return r == 2 // CSTR_EQUAL; failure is zero
}

func localOpenFilePath(path string) (string, bool) {
	if len(path) < 7 || !strings.HasPrefix(path, `\\?\`) ||
		!((path[4] >= 'A' && path[4] <= 'Z') || (path[4] >= 'a' && path[4] <= 'z')) || path[5:7] != `:\` {
		return "", false
	}
	return path[4:], true
}

// validateSharedFile opens only metadata handles and returns the normalized
// local path after closing ALL handles. The receiving app reopens by path;
// replacement between validation and that open remains possible by design.
func validateSharedFile(share, rel string) (string, error) {
	if !validOpenFilePath(rel) {
		return "", fmt.Errorf("invalid relative file path")
	}
	if len(share) < 3 || !((share[0] >= 'A' && share[0] <= 'Z') || (share[0] >= 'a' && share[0] <= 'z')) || share[1:3] != `:\` {
		return "", fmt.Errorf("share is not a local drive path")
	}
	var rootParts []string
	if len(share) > 3 {
		rootParts = strings.Split(share[3:], `\`)
		for _, part := range rootParts {
			if strings.Contains(part, "/") || !validOpenFilePath(part) {
				return "", fmt.Errorf("invalid share component")
			}
		}
	}
	rootPath := `\\?\` + share[:3]
	root, err := openFileWin32Directory(rootPath)
	if err != nil {
		return "", err
	}
	handles := []syscall.Handle{root}
	defer func() {
		for i := len(handles) - 1; i >= 0; i-- {
			syscall.CloseHandle(handles[i])
		}
	}()
	for _, part := range rootParts {
		rootPath = strings.TrimSuffix(rootPath, `\`) + `\` + part
		// Explicitly inspect every prefix of the configured share with the
		// Win32 no-follow flags. Also open it relative to the verified parent:
		// a raced replacement of an earlier prefix cannot redirect this walk.
		probe, err := openFileWin32Directory(rootPath)
		if err != nil {
			return "", err
		}
		syscall.CloseHandle(probe)
		next, err := openFileRelative(root, part, true)
		if err != nil {
			return "", err
		}
		handles = append(handles, next)
		if err := checkOpenFileHandle(next, true); err != nil {
			return "", err
		}
		root = next
	}
	target := root
	parts := strings.Split(rel, "/")
	for i, part := range parts {
		directory := i < len(parts)-1
		next, err := openFileRelative(target, part, directory)
		if err != nil {
			return "", err
		}
		handles = append(handles, next)
		if err := checkOpenFileHandle(next, directory); err != nil {
			return "", err
		}
		target = next
	}
	var info syscall.ByHandleFileInformation
	if err := syscall.GetFileInformationByHandle(target, &info); err != nil {
		return "", fmt.Errorf("cannot read final file information")
	}
	if info.FileAttributes&syscall.FILE_ATTRIBUTE_DIRECTORY != 0 || info.NumberOfLinks != 1 {
		return "", fmt.Errorf("target is a directory or has multiple hard links")
	}
	rootFinal, err := openFileFinalPath(root)
	if err != nil {
		return "", err
	}
	if !openFileOrdinalEqual(strings.TrimSuffix(rootFinal, `\`), strings.TrimSuffix(`\\?\`+share, `\`)) {
		return "", fmt.Errorf("share final path differs from the configured share")
	}
	targetFinal, err := openFileFinalPath(target)
	if err != nil {
		return "", err
	}
	if _, ok := localOpenFilePath(rootFinal); !ok {
		return "", fmt.Errorf("share final path is not a local drive path")
	}
	// The separator is part of the comparison, so share-other cannot match
	// share. Comparing the whole expected path also compares the remainder
	// with the request, rejecting 8.3 aliases rather than accepting them.
	expected := strings.TrimSuffix(rootFinal, `\`) + `\` + strings.ReplaceAll(rel, "/", `\`)
	if !openFileOrdinalEqual(targetFinal, expected) {
		return "", fmt.Errorf("final path differs from the requested file inside the share")
	}
	finalPath, ok := localOpenFilePath(targetFinal)
	if !ok {
		return "", fmt.Errorf("target final path is not a local drive path")
	}
	return finalPath, nil
}

func validateFileOpener(appPath string) error {
	if err := validateApprovedExecutable(appPath); err != nil {
		return fmt.Errorf("approved file opener is unavailable")
	}
	if isFileOpenerInterpreter(appPath) {
		return fmt.Errorf("interpreters cannot be file openers")
	}
	if share := currentShare(); share != "" && pathsOverlapWindows(appPath, share) {
		return fmt.Errorf("file opener overlaps the running share")
	}
	return nil
}

func isFileOpenerInterpreter(appPath string) bool {
	base := strings.ToLower(strings.TrimSuffix(filepath.Base(appPath), filepath.Ext(appPath)))
	return strings.HasPrefix(base, "python") || slices.Contains([]string{"py", "pyw", "powershell", "pwsh", "cmd", "wscript", "cscript", "mshta", "rundll32", "regsvr32", "node", "java", "javaw", "bash", "wsl", "conhost", "perl", "ruby", "php", "lua", "tclsh", "wish", "dotnet", "msiexec", "forfiles", "pcalua", "msbuild", "installutil", "regasm", "regsvcs", "cmstp", "hh", "wmic", "bitsadmin", "certutil", "explorer"}, base)
}

func openSharedWindowsFile(dir, rel string) error {
	share := currentShare()
	if share == "" {
		return fmt.Errorf("no share in the current VM session")
	}
	prefs, err := loadApprovedFileTypes(dir)
	if err != nil {
		return fmt.Errorf("allowed file types are unreadable")
	}
	apps, err := loadApprovedWindowsApps(dir)
	if err != nil {
		return fmt.Errorf("approved apps are unreadable")
	}
	appPath, err := prefs.opener(rel, apps)
	if err != nil {
		return err
	}
	if appPath != "" {
		if err := validateFileOpener(appPath); err != nil {
			return err
		}
	}
	finalPath, err := validateSharedFile(share, rel)
	if err != nil {
		return err
	}
	if appPath == "" {
		systemRoot := os.Getenv("SystemRoot")
		if _, ok := localOpenFilePath(`\\?\` + systemRoot); !ok {
			return fmt.Errorf("SystemRoot is unavailable")
		}
		verb, _ := syscall.UTF16PtrFromString("open")
		file, err := syscall.UTF16PtrFromString(finalPath)
		if err != nil {
			return fmt.Errorf("invalid final file path")
		}
		directory, err := syscall.UTF16PtrFromString(filepath.Join(systemRoot, "System32"))
		if err != nil {
			return fmt.Errorf("invalid system directory")
		}
		info := shellExecuteInfo{fMask: 0x400, lpVerb: verb, lpFile: file, lpDirectory: directory, nShow: 1} // SEE_MASK_FLAG_NO_UI, SW_SHOWNORMAL
		info.cbSize = uint32(unsafe.Sizeof(info))
		if r, _, _ := procShellExecuteExW.Call(uintptr(unsafe.Pointer(&info))); r == 0 {
			return fmt.Errorf("default file opener failed")
		}
	} else {
		cmd := exec.Command(appPath, finalPath)
		cmd.Dir = filepath.Dir(appPath)
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			return fmt.Errorf("approved file opener failed")
		}
		_ = cmd.Process.Release()
	}
	if hwnd := qemuHwnd.Load(); hwnd != 0 {
		procShowWindow.Call(hwnd, swShowMinimized)
	}
	return nil
}
