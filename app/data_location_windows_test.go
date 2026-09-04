//go:build windows

package main

import "testing"

func TestDataLocationIsLocal(t *testing.T) {
	if !dataLocationIsLocal(t.TempDir()) {
		t.Fatal("temporary directory was not recognized as local")
	}
	if dataLocationIsLocal(`\\server\share\TryOmarchy`) {
		t.Fatal("UNC location was recognized as local")
	}
}
