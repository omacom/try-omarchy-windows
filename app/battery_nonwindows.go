//go:build !windows

package main

func hostBatteryLine() (string, error) { return "", nil }
