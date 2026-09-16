//go:build !windows

package main

import "errors"

type unavailableCameraSource struct{}

func newCameraFrameSource() cameraFrameSource { return unavailableCameraSource{} }

func (unavailableCameraSource) start() (<-chan []byte, error) {
	return nil, errors.New("camera capture is only available on Windows")
}

func (unavailableCameraSource) stop() {}
