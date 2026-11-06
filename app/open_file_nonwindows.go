//go:build !windows

package main

import "fmt"

func openSharedWindowsFile(dir, rel string) error {
	return fmt.Errorf("opening shared files requires Windows")
}
