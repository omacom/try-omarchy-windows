package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"
)

const (
	qcow2ClusterBits           = 16
	qcow2ClusterSize           = int64(1 << qcow2ClusterBits)
	qcow2HeaderSize            = 104
	portableBackingStateSuffix = ".backing-sha256"
)

func portableBackingStatePath(disk string) string {
	return disk + portableBackingStateSuffix
}

// A QCOW2 overlay depends on the exact bytes of its raw backing image. The
// header stores only the relative path, so keep the authenticated rootfs
// digest beside the overlay and refuse to run if a newer portable payload has
// replaced the backing image underneath persistent guest data.
func portableBackingStateMatches(disk, backingSHA256 string) (bool, error) {
	path := portableBackingStatePath(disk)
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return false, fmt.Errorf("portable disk backing identity is missing; restore it with the matching payload or start with -portable -fresh")
	}
	if err != nil {
		return false, err
	}
	if len(data) > 128 {
		return false, fmt.Errorf("portable disk backing identity is invalid")
	}
	got := normalizedSHA256(string(data))
	if !validSHA256(got) {
		return false, fmt.Errorf("portable disk backing identity is invalid")
	}
	return got == normalizedSHA256(backingSHA256), nil
}

func writePortableBackingState(disk, backingSHA256 string) error {
	backingSHA256 = normalizedSHA256(backingSHA256)
	if !validSHA256(backingSHA256) {
		return fmt.Errorf("portable factory disk identity is invalid")
	}
	path := portableBackingStatePath(disk)
	if data, err := os.ReadFile(path); err == nil {
		if normalizedSHA256(string(data)) == backingSHA256 {
			return nil
		}
		return fmt.Errorf("portable disk backing identity already exists for a different factory image")
	} else if !os.IsNotExist(err) {
		return err
	}
	tmp := path + ".part"
	f, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write([]byte(strings.ToLower(backingSHA256) + "\n")); err != nil {
		f.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := f.Sync(); err != nil {
		f.Close()
		_ = os.Remove(tmp)
		return err
	}
	if err := f.Close(); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	if err := renamePortableFileWithRetry(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// createQcow2Overlay writes the small amount of QCOW2 v3 metadata needed for
// an empty overlay backed by a raw image. QEMU allocates data and L2 tables as
// the guest writes, avoiding an NTFS-only sparse raw copy on removable media.
func createQcow2Overlay(path, backing string, virtualSize int64) (err error) {
	if virtualSize <= 0 {
		return fmt.Errorf("invalid virtual size %d", virtualSize)
	}
	if len(backing) == 0 || len(backing) > int(qcow2ClusterSize-qcow2HeaderSize) {
		return fmt.Errorf("invalid backing path length %d", len(backing))
	}
	l1Coverage := qcow2ClusterSize * (qcow2ClusterSize / 8)
	l1Size := (virtualSize + l1Coverage - 1) / l1Coverage
	l1Bytes := l1Size * 8
	l1Clusters := (l1Bytes + qcow2ClusterSize - 1) / qcow2ClusterSize
	if l1Clusters <= 0 || l1Clusters > qcow2ClusterSize/2-3 {
		return fmt.Errorf("virtual disk is too large")
	}
	const (
		refcountTableOffset = qcow2ClusterSize
		refcountBlockOffset = 2 * qcow2ClusterSize
		l1TableOffset       = 3 * qcow2ClusterSize
	)
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer func() {
		if closeErr := f.Close(); err == nil {
			err = closeErr
		}
		if err != nil {
			os.Remove(path)
		}
	}()
	header := make([]byte, qcow2HeaderSize)
	binary.BigEndian.PutUint32(header[0:4], 0x514649fb)
	binary.BigEndian.PutUint32(header[4:8], 3)
	binary.BigEndian.PutUint64(header[8:16], qcow2HeaderSize)
	binary.BigEndian.PutUint32(header[16:20], uint32(len(backing)))
	binary.BigEndian.PutUint32(header[20:24], qcow2ClusterBits)
	binary.BigEndian.PutUint64(header[24:32], uint64(virtualSize))
	binary.BigEndian.PutUint32(header[36:40], uint32(l1Size))
	binary.BigEndian.PutUint64(header[40:48], uint64(l1TableOffset))
	binary.BigEndian.PutUint64(header[48:56], uint64(refcountTableOffset))
	binary.BigEndian.PutUint32(header[56:60], 1)
	binary.BigEndian.PutUint32(header[96:100], 4)
	binary.BigEndian.PutUint32(header[100:104], qcow2HeaderSize)
	if _, err = f.WriteAt(header, 0); err != nil {
		return err
	}
	if _, err = f.WriteAt([]byte(backing), qcow2HeaderSize); err != nil {
		return err
	}
	entry := make([]byte, 8)
	binary.BigEndian.PutUint64(entry, uint64(refcountBlockOffset))
	if _, err = f.WriteAt(entry, refcountTableOffset); err != nil {
		return err
	}
	refcount := make([]byte, 2*(3+l1Clusters))
	for i := int64(0); i < 3+l1Clusters; i++ {
		binary.BigEndian.PutUint16(refcount[i*2:i*2+2], 1)
	}
	if _, err = f.WriteAt(refcount, refcountBlockOffset); err != nil {
		return err
	}
	if err = f.Truncate(l1TableOffset + l1Bytes); err != nil {
		return err
	}
	return f.Sync()
}

func qcow2OverlayMatches(path, backing string, virtualSize int64) (bool, error) {
	info, err := os.Lstat(path)
	if os.IsNotExist(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	if !info.Mode().IsRegular() {
		return false, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return false, err
	}
	defer f.Close()
	header := make([]byte, qcow2HeaderSize)
	if _, err := io.ReadFull(f, header); err != nil {
		return false, nil
	}
	if binary.BigEndian.Uint32(header[0:4]) != 0x514649fb ||
		binary.BigEndian.Uint32(header[4:8]) != 3 ||
		binary.BigEndian.Uint64(header[8:16]) != qcow2HeaderSize ||
		binary.BigEndian.Uint32(header[20:24]) != qcow2ClusterBits ||
		binary.BigEndian.Uint64(header[24:32]) != uint64(virtualSize) {
		return false, nil
	}
	backingLen := int(binary.BigEndian.Uint32(header[16:20]))
	if backingLen != len(backing) {
		return false, nil
	}
	backingData := make([]byte, backingLen)
	if _, err := f.ReadAt(backingData, qcow2HeaderSize); err != nil {
		return false, nil
	}
	return string(backingData) == backing, nil
}
