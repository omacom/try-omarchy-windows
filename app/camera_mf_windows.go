//go:build windows

package main

import (
	"errors"
	"fmt"
	"sync"
	"syscall"
	"unsafe"
)

// Media Foundation camera capture. The source reader runs in asynchronous mode
// with a Go-implemented IMFSourceReaderCallback, so stopping capture is a Flush
// and a release rather than interrupting a blocked call. Frames arrive as NV12
// and are repacked to tightly packed rows before they reach the wire.
//
// This is hand-rolled COM on the app's existing comCall helper (see taskbar.go),
// so the launcher keeps its no-cgo, single-dependency build.

type hresult = int32

const (
	sOK                            hresult = 0
	sFalse                         hresult = 1
	coInitMultithreaded                    = 0x0
	mfVersion                              = 0x00020070
	mfStartupLite                          = 0x1
	mfSourceReaderFirstVideoStream         = 0xfffffffc
	mfVideoInterlaceProgressive            = 2
)

const rpcEChangedMode = 0x80010106

// Media Foundation GUIDs, taken verbatim from the mingw-w64 headers.
var (
	guidDeviceSourceType       = newGUID(0xc60ac5fe, 0x252a, 0x478f, 0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3)
	guidDeviceSourceTypeVidcap = newGUID(0x8ac3587a, 0x4ae7, 0x42d8, 0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f)
	guidMajorType              = newGUID(0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f)
	guidSubtype                = newGUID(0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5)
	guidFrameSize              = newGUID(0x1652c33d, 0xd6b2, 0x4012, 0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d)
	guidFrameRate              = newGUID(0xc459a2e8, 0x3d2c, 0x4e44, 0xb1, 0x32, 0xfe, 0xe5, 0x15, 0x6c, 0x7b, 0xb0)
	guidInterlaceMode          = newGUID(0xe2724bb8, 0xe676, 0x4806, 0xb4, 0xb2, 0xa8, 0xd6, 0xef, 0xb4, 0x4c, 0xcd)
	guidDefaultStride          = newGUID(0x644b4e48, 0x1e02, 0x4516, 0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6)
	guidMediaTypeVideo         = newGUID(0x73646976, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
	guidVideoFormatNV12        = newGUID(0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71)
	guidAsyncCallback          = newGUID(0x1e3dbeac, 0xbb43, 0x4c35, 0xb5, 0x07, 0xcd, 0x64, 0x44, 0x64, 0xc9, 0x65)
	guidEnableVideoProcessing  = newGUID(0xfb394f3d, 0xccf1, 0x42ee, 0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d)
)

func newGUID(data1 uint32, data2, data3 uint16, rest ...byte) comGUID {
	var value comGUID
	value.d1 = data1
	value.d2 = data2
	value.d3 = data3
	copy(value.d4[:], rest)
	return value
}

var (
	modMfplat      = syscall.NewLazyDLL("mfplat.dll")
	modMfreadwrite = syscall.NewLazyDLL("mfreadwrite.dll")

	procMFStartup           = modMfplat.NewProc("MFStartup")
	procMFCreateAttributes  = modMfplat.NewProc("MFCreateAttributes")
	procMFCreateMediaType   = modMfplat.NewProc("MFCreateMediaType")
	procMFSetAttributeSize  = modMfplat.NewProc("MFSetAttributeSize")
	procMFSetAttributeRatio = modMfplat.NewProc("MFSetAttributeRatio")
	procMFEnumDeviceSources = modMfplat.NewProc("MFEnumDeviceSources")

	procMFCreateSourceReaderFromMediaSource = modMfreadwrite.NewProc("MFCreateSourceReaderFromMediaSource")
	procCoUninitialize                      = ole32.NewProc("CoUninitialize")

	mfStartupOnce sync.Once
	mfStartupErr  hresult
)

// mfCall invokes a COM method through the object's vtable.
func mfCall(obj unsafe.Pointer, method int, args ...uintptr) hresult {
	if obj == nil {
		return -1
	}
	return hresult(int32(uint32(comCall(uintptr(obj), method, args...))))
}

func mfRelease(obj *unsafe.Pointer) {
	if obj != nil && *obj != nil {
		mfCall(*obj, 2) // IUnknown::Release
		*obj = nil
	}
}

func procCall(proc *syscall.LazyProc, args ...uintptr) hresult {
	result, _, _ := proc.Call(args...)
	return hresult(int32(uint32(result)))
}

func setGUID(obj unsafe.Pointer, key *comGUID, value *comGUID) {
	mfCall(obj, 24, uintptr(unsafe.Pointer(key)), uintptr(unsafe.Pointer(value)))
}

func setUint32(obj unsafe.Pointer, key *comGUID, value uint32) {
	mfCall(obj, 21, uintptr(unsafe.Pointer(key)), uintptr(value))
}

func setUnknown(obj unsafe.Pointer, key *comGUID, value unsafe.Pointer) {
	mfCall(obj, 27, uintptr(unsafe.Pointer(key)), uintptr(value))
}

func getUint32(obj unsafe.Pointer, key *comGUID) uint32 {
	var value uint32
	mfCall(obj, 7, uintptr(unsafe.Pointer(key)), uintptr(unsafe.Pointer(&value))) // IMFAttributes::GetUINT32
	return value
}

// mfCameraSource -------------------------------------------------------------

type mfCameraSource struct {
	frames         chan []byte
	reader         unsafe.Pointer
	attrs          unsafe.Pointer
	media          unsafe.Pointer
	activate       unsafe.Pointer
	devices        *unsafe.Pointer
	callback       *cameraCallback
	stride         int
	stopped        bool
	comInitialized bool
	mu             sync.Mutex
}

type cameraCallback struct {
	vtable *[6]uintptr
	source *mfCameraSource
}

func newCameraCallback(source *mfCameraSource) *cameraCallback {
	callback := &cameraCallback{source: source}
	callback.vtable = &[6]uintptr{
		syscall.NewCallback(callbackQueryInterface),
		syscall.NewCallback(callbackAddRef),
		syscall.NewCallback(callbackRelease),
		syscall.NewCallback(callbackOnReadSample),
		syscall.NewCallback(callbackOnFlush),
		syscall.NewCallback(callbackOnEvent),
	}
	return callback
}

func callbackQueryInterface(this, riid, ppv uintptr) uintptr {
	*(*uintptr)(unsafe.Pointer(ppv)) = this
	return uintptr(sOK)
}

func callbackAddRef(uintptr) uintptr { return 1 }

func callbackRelease(uintptr) uintptr { return 1 }

func callbackOnFlush(uintptr, uintptr) uintptr { return uintptr(sOK) }

func callbackOnEvent(uintptr, uintptr, uintptr) uintptr { return uintptr(sOK) }

func callbackOnReadSample(this, hrStatus, streamIndex, streamFlags, timestamp, sample uintptr) uintptr {
	callback := (*cameraCallback)(unsafe.Pointer(this))
	callback.source.handleSample(hresult(int32(uint32(hrStatus))), sample)
	return uintptr(sOK)
}

func startMediaFoundation() error {
	mfStartupOnce.Do(func() {
		mfStartupErr = procCall(procMFStartup, mfVersion, mfStartupLite)
	})
	if mfStartupErr < 0 {
		return fmt.Errorf("Media Foundation startup failed (0x%08x)", uint32(mfStartupErr))
	}
	return nil
}

func (s *mfCameraSource) start() (<-chan []byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.frames != nil {
		return s.frames, nil
	}
	if err := startMediaFoundation(); err != nil {
		return nil, err
	}
	if hr := procCall(procCoInitializeEx, 0, coInitMultithreaded); hr >= 0 {
		s.comInitialized = true
	} else if uint32(hr) != rpcEChangedMode {
		return nil, fmt.Errorf("COM initialization failed (0x%08x)", uint32(hr))
	}
	if err := s.open(); err != nil {
		s.closeLocked()
		return nil, err
	}
	s.frames = make(chan []byte, 4)
	s.stopped = false
	if hr := mfCall(s.reader, 9, mfSourceReaderFirstVideoStream, 0, 0, 0, 0, 0); hr < 0 { // ReadSample
		s.closeLocked()
		return nil, fmt.Errorf("camera capture failed to start (0x%08x)", uint32(hr))
	}
	return s.frames, nil
}

func (s *mfCameraSource) open() error {
	var attributes unsafe.Pointer
	if hr := procCall(procMFCreateAttributes, uintptr(unsafe.Pointer(&attributes)), 1); hr < 0 {
		return fmt.Errorf("MFCreateAttributes failed (0x%08x)", uint32(hr))
	}
	setGUID(attributes, &guidDeviceSourceType, &guidDeviceSourceTypeVidcap)

	var count uint32
	var devices *unsafe.Pointer
	if hr := procCall(procMFEnumDeviceSources, uintptr(attributes), uintptr(unsafe.Pointer(&devices)), uintptr(unsafe.Pointer(&count))); hr < 0 {
		mfRelease(&attributes)
		return fmt.Errorf("enumerating cameras failed (0x%08x)", uint32(hr))
	}
	if count == 0 || devices == nil {
		if devices != nil {
			procCoTaskMemFree.Call(uintptr(unsafe.Pointer(devices)))
		}
		mfRelease(&attributes)
		return errors.New("no camera was found on this PC")
	}
	s.devices = devices
	array := unsafe.Slice(devices, int(count))
	s.activate = array[0]
	for _, extra := range array[1:] {
		extra := extra
		mfRelease(&extra)
	}

	var source unsafe.Pointer
	if hr := mfCall(s.activate, 33, 0, 0, uintptr(unsafe.Pointer(&source))); hr < 0 { // IMFActivate::ActivateObject
		mfRelease(&attributes)
		return fmt.Errorf("the camera could not be opened (0x%08x)", uint32(hr))
	}

	var callbackAttrs unsafe.Pointer
	if hr := procCall(procMFCreateAttributes, uintptr(unsafe.Pointer(&callbackAttrs)), 1); hr < 0 {
		mfRelease(&source)
		mfRelease(&attributes)
		return fmt.Errorf("MFCreateAttributes failed (0x%08x)", uint32(hr))
	}
	setUint32(callbackAttrs, &guidEnableVideoProcessing, 1)
	s.callback = newCameraCallback(s)
	setUnknown(callbackAttrs, &guidAsyncCallback, unsafe.Pointer(s.callback))

	var reader unsafe.Pointer
	if hr := procCall(procMFCreateSourceReaderFromMediaSource, uintptr(source), uintptr(callbackAttrs), uintptr(unsafe.Pointer(&reader))); hr < 0 {
		mfRelease(&source)
		mfRelease(&callbackAttrs)
		mfRelease(&attributes)
		return fmt.Errorf("the camera source reader could not be created (0x%08x)", uint32(hr))
	}
	s.reader = reader
	mfRelease(&callbackAttrs)
	mfRelease(&source)
	s.attrs = attributes

	return s.configure()
}

func (s *mfCameraSource) configure() error {
	var media unsafe.Pointer
	if hr := procCall(procMFCreateMediaType, uintptr(unsafe.Pointer(&media))); hr < 0 {
		return fmt.Errorf("MFCreateMediaType failed (0x%08x)", uint32(hr))
	}
	setGUID(media, &guidMajorType, &guidMediaTypeVideo)
	setGUID(media, &guidSubtype, &guidVideoFormatNV12)
	procMFSetAttributeSize.Call(uintptr(media), uintptr(unsafe.Pointer(&guidFrameSize)), uintptr(cameraWidth), uintptr(cameraHeight))
	procMFSetAttributeRatio.Call(uintptr(media), uintptr(unsafe.Pointer(&guidFrameRate)), 30, 1)
	setUint32(media, &guidInterlaceMode, mfVideoInterlaceProgressive)

	if hr := mfCall(s.reader, 7, mfSourceReaderFirstVideoStream, 0, uintptr(media)); hr < 0 { // SetCurrentMediaType
		mfRelease(&media)
		return fmt.Errorf("the camera does not support %dx%d NV12 (0x%08x)", cameraWidth, cameraHeight, uint32(hr))
	}
	if hr := mfCall(s.reader, 4, mfSourceReaderFirstVideoStream, 1); hr < 0 { // SetStreamSelection(true)
		mfRelease(&media)
		return fmt.Errorf("the camera stream could not be selected (0x%08x)", uint32(hr))
	}

	var current unsafe.Pointer
	if hr := mfCall(s.reader, 6, mfSourceReaderFirstVideoStream, uintptr(unsafe.Pointer(&current))); hr >= 0 { // GetCurrentMediaType
		if stride := int(getUint32(current, &guidDefaultStride)); stride > 0 {
			s.stride = stride
		}
		mfRelease(&current)
	}
	if s.stride < cameraWidth {
		s.stride = cameraWidth
	}
	mfRelease(&media)
	s.media = media
	return nil
}

func (s *mfCameraSource) handleSample(status hresult, sample uintptr) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if sample != 0 {
		defer mfCall(unsafe.Pointer(sample), 2) // IUnknown::Release
	}
	if s.stopped || s.reader == nil {
		return
	}
	if status >= 0 && sample != 0 {
		if frame := s.copyFrame(sample); frame != nil && s.frames != nil {
			select {
			case s.frames <- frame:
			default: // drop a frame rather than stall the camera callback
			}
		}
	}
	// Re-request while still holding the lock so stop() cannot release the
	// reader between the check and the call.
	mfCall(s.reader, 9, mfSourceReaderFirstVideoStream, 0, 0, 0, 0, 0) // ReadSample
}

// copyFrame converts one camera sample to tightly packed NV12, dropping any row
// padding the device reported.
func (s *mfCameraSource) copyFrame(sample uintptr) []byte {
	var buffer unsafe.Pointer
	if hr := mfCall(unsafe.Pointer(sample), 10, 0, uintptr(unsafe.Pointer(&buffer))); hr < 0 { // GetBufferByIndex
		return nil
	}
	defer mfRelease(&buffer)

	var data unsafe.Pointer
	var maxLength, currentLength uint32
	if hr := mfCall(buffer, 3, uintptr(unsafe.Pointer(&data)), uintptr(unsafe.Pointer(&maxLength)), uintptr(unsafe.Pointer(&currentLength))); hr < 0 { // Lock
		return nil
	}
	defer mfCall(buffer, 4) // Unlock

	if data == nil || currentLength < uint32(s.stride*cameraHeight*3/2) {
		return nil
	}

	frame := make([]byte, cameraFrameBytes)
	rows := unsafe.Slice((*byte)(data), int(currentLength))
	luma := cameraWidth * cameraHeight
	for row := 0; row < cameraHeight; row++ {
		copy(frame[row*cameraWidth:(row+1)*cameraWidth], rows[row*s.stride:row*s.stride+cameraWidth])
	}
	chroma := s.stride * cameraHeight
	for row := 0; row < cameraHeight/2; row++ {
		copy(frame[luma+row*cameraWidth:luma+(row+1)*cameraWidth], rows[chroma+row*s.stride:chroma+row*s.stride+cameraWidth])
	}
	return frame
}

func (s *mfCameraSource) stop() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closeLocked()
}

// closeLocked releases Media Foundation resources. The caller holds s.mu so a
// callback in flight can never re-issue ReadSample against a released reader.
func (s *mfCameraSource) closeLocked() {
	s.stopped = true
	if s.reader != nil {
		mfCall(s.reader, 8, mfSourceReaderFirstVideoStream) // Flush
	}
	mfRelease(&s.reader)
	mfRelease(&s.media)
	mfRelease(&s.attrs)
	mfRelease(&s.activate)
	if s.devices != nil {
		procCoTaskMemFree.Call(uintptr(unsafe.Pointer(s.devices)))
		s.devices = nil
	}
	s.frames = nil
	if s.comInitialized {
		procCoUninitialize.Call()
		s.comInitialized = false
	}
}
