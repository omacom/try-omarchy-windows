package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
)

const (
	qcow2ClusterBits = 16
	qcow2ClusterSize = int64(1 << qcow2ClusterBits)
	qcow2HeaderSize  = 104
)

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
