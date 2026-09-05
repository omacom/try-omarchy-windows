package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"unsafe"
)

const fsctlSetSparse = 0x000900C4

var (
	kernel32            = syscall.NewLazyDLL("kernel32.dll")
	procDeviceIoControl = kernel32.NewProc("DeviceIoControl")
)

func setSparse(f *os.File) error {
	var returned uint32
	r, _, err := procDeviceIoControl.Call(f.Fd(), fsctlSetSparse, 0, 0, 0, 0, uintptr(unsafe.Pointer(&returned)), 0)
	if r == 0 {
		return err
	}
	return nil
}

// sparseWrite copies r into a new sparse file at target, skipping all-zero
// 1 MiB blocks, and returns the size and sha256 written.
func sparseWrite(target string, r io.Reader) (int64, string, error) {
	if err := os.MkdirAll(filepath.Dir(target), 0700); err != nil {
		return 0, "", err
	}
	f, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return 0, "", err
	}
	defer f.Close()
	if err := setSparse(f); err != nil {
		return 0, "", fmt.Errorf("set sparse: %w", err)
	}
	buf := make([]byte, 1<<20)
	zero := make([]byte, 1<<20)
	h := sha256.New()
	var off int64
	for {
		n, rerr := io.ReadFull(r, buf)
		if n > 0 {
			h.Write(buf[:n])
			if bytes.Equal(buf[:n], zero[:n]) {
				off += int64(n)
				if _, err := f.Seek(off, io.SeekStart); err != nil {
					return 0, "", err
				}
			} else {
				if _, err := f.Write(buf[:n]); err != nil {
					return 0, "", err
				}
				off += int64(n)
			}
		}
		if rerr == io.EOF || rerr == io.ErrUnexpectedEOF {
			break
		}
		if rerr != nil {
			return 0, "", rerr
		}
	}
	if err := f.Truncate(off); err != nil {
		return 0, "", err
	}
	return off, hex.EncodeToString(h.Sum(nil)), nil
}

type manifest struct {
	Version int `json:"version"`
	Files   []struct {
		Name   string `json:"name"`
		Size   int64  `json:"size"`
		SHA256 string `json:"sha256"`
	} `json:"files"`
}

func extract(zipPath, dest string) error {
	if _, err := os.Lstat(dest); err == nil {
		return fmt.Errorf("destination exists")
	}
	z, err := zip.OpenReader(zipPath)
	if err != nil {
		return err
	}
	defer z.Close()
	files := map[string]*zip.File{}
	for _, f := range z.File {
		files[f.Name] = f
	}
	mf := files["backup.json"]
	if mf == nil {
		return fmt.Errorf("no backup.json")
	}
	mr, err := mf.Open()
	if err != nil {
		return err
	}
	var m manifest
	if err := json.NewDecoder(mr).Decode(&m); err != nil {
		return err
	}
	mr.Close()
	for _, e := range m.Files {
		zf := files[e.Name]
		if zf == nil {
			return fmt.Errorf("missing %s", e.Name)
		}
		r, err := zf.Open()
		if err != nil {
			return err
		}
		n, sum, err := sparseWrite(filepath.Join(dest, filepath.FromSlash(e.Name)), r)
		r.Close()
		if err != nil {
			return fmt.Errorf("%s: %w", e.Name, err)
		}
		if n != e.Size || sum != e.SHA256 {
			return fmt.Errorf("%s: size/hash mismatch", e.Name)
		}
		fmt.Printf("ok %s %d\n", e.Name, n)
	}
	return nil
}

func copyTree(src, dst string) error {
	if _, err := os.Lstat(dst); err == nil {
		return fmt.Errorf("destination exists")
	}
	return filepath.Walk(src, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(src, p)
		target := filepath.Join(dst, rel)
		if info.IsDir() {
			return os.MkdirAll(target, 0700)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("%s: not a regular file", p)
		}
		in, err := os.Open(p)
		if err != nil {
			return err
		}
		defer in.Close()
		n, _, err := sparseWrite(target, in)
		if err != nil {
			return fmt.Errorf("%s: %w", rel, err)
		}
		if n != info.Size() {
			return fmt.Errorf("%s: short copy", rel)
		}
		fmt.Printf("ok %s %d\n", rel, n)
		return nil
	})
}

func hashTree(root string) error {
	var names []string
	err := filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.Mode().IsRegular() {
			names = append(names, p)
		}
		return nil
	})
	if err != nil {
		return err
	}
	sort.Strings(names)
	for _, p := range names {
		f, err := os.Open(p)
		if err != nil {
			return err
		}
		h := sha256.New()
		n, err := io.Copy(h, f)
		f.Close()
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(root, p)
		fmt.Printf("%s %d %s\n", hex.EncodeToString(h.Sum(nil)), n, strings.ReplaceAll(rel, "\\", "/"))
	}
	return nil
}

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: sparsetool extract ZIP DEST | copy SRC DST | hash DIR")
		os.Exit(2)
	}
	var err error
	switch os.Args[1] {
	case "extract":
		err = extract(os.Args[2], os.Args[3])
	case "copy":
		err = copyTree(os.Args[2], os.Args[3])
	case "hash":
		err = hashTree(os.Args[2])
	default:
		err = fmt.Errorf("unknown command")
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
	fmt.Println("done")
}
