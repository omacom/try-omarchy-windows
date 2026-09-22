//go:build !windows

package main

func listAudioEndpoints() (mmDeviceList, error) {
	return mmDeviceList{}, nil
}
