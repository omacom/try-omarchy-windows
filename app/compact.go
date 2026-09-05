package main

import (
	"bytes"
	"fmt"
	"io"
	"os"
)

// Reclaiming Windows disk space. Deleting files inside Omarchy never shrinks
// disk.raw: ext4 does not zero freed blocks and QEMU's Windows backend has no
// discard path, so the sparse file only grows. Reclaim works in two steps:
// the guest agent zero-fills its free space on request, and once the guest
// has powered off the launcher scans disk.raw and turns every all-zero block
// back into a hole.

const compactBlock = 1 << 20

// punchZeroBlocks reads f in fixed blocks and calls punch for each run of
// all-zero blocks. It returns the number of bytes handed to punch. Blocks that
// are already holes read as zeros and cost nothing to punch again on NTFS.
func punchZeroBlocks(f io.ReaderAt, size int64, punch func(offset, length int64) error, report func(done, total int64)) (int64, error) {
	buf := make([]byte, compactBlock)
	zero := make([]byte, compactBlock)
	var reclaimed, runStart, runLength int64
	flush := func() error {
		if runLength == 0 {
			return nil
		}
		if err := punch(runStart, runLength); err != nil {
			return err
		}
		reclaimed += runLength
		runLength = 0
		return nil
	}
	for offset := int64(0); offset < size; offset += compactBlock {
		n := compactBlock
		if remaining := size - offset; remaining < int64(n) {
			n = int(remaining)
		}
		if _, err := f.ReadAt(buf[:n], offset); err != nil && err != io.EOF {
			return reclaimed, fmt.Errorf("reading disk at %d: %w", offset, err)
		}
		if bytes.Equal(buf[:n], zero[:n]) {
			if runLength == 0 {
				runStart = offset
			}
			runLength += int64(n)
		} else if err := flush(); err != nil {
			return reclaimed, err
		}
		if report != nil {
			report(offset+int64(n), size)
		}
	}
	return reclaimed, flush()
}

// compactDisk punches holes through the zero blocks of a stopped guest disk.
func compactDisk(path string, report func(done, total int64)) (int64, error) {
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return 0, err
	}
	if !info.Mode().IsRegular() {
		return 0, fmt.Errorf("%s is not a regular file", path)
	}
	if err := setSparse(f); err != nil {
		return 0, fmt.Errorf("marking the disk sparse: %w", err)
	}
	return punchZeroBlocks(f, info.Size(), func(offset, length int64) error { return punchHole(f, offset, length) }, report)
}
