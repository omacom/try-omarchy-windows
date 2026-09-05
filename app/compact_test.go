package main

import (
	"bytes"
	"testing"
)

func TestPunchZeroBlocksCoalescesRunsAndSkipsData(t *testing.T) {
	// Blocks: data, zero, zero, data, zero(partial tail)
	disk := make([]byte, 4*compactBlock+512)
	copy(disk[0:], []byte("data"))
	disk[3*compactBlock+7] = 1
	var punched [][2]int64
	reclaimed, err := punchZeroBlocks(bytes.NewReader(disk), int64(len(disk)), func(offset, length int64) error {
		punched = append(punched, [2]int64{offset, length})
		return nil
	}, nil)
	if err != nil {
		t.Fatal(err)
	}
	want := [][2]int64{{compactBlock, 2 * compactBlock}, {4 * compactBlock, 512}}
	if len(punched) != len(want) || punched[0] != want[0] || punched[1] != want[1] {
		t.Fatalf("punched %v, want %v", punched, want)
	}
	if reclaimed != 2*compactBlock+512 {
		t.Fatalf("reclaimed %d", reclaimed)
	}
	if reclaimed, err := punchZeroBlocks(bytes.NewReader([]byte("all data")), 8, func(int64, int64) error { t.Fatal("punched data"); return nil }, nil); err != nil || reclaimed != 0 {
		t.Fatalf("data-only disk: %d %v", reclaimed, err)
	}
}
