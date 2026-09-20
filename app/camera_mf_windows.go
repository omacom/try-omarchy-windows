//go:build windows

package main

import (
	"errors"
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
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
	coInitMultithreaded                    = 0x0
	mfVersion                              = 0x00020070
	mfStartupLite                          = 0x1
	mfSourceReaderFirstVideoStream         = 0xfffffffc
	mfVideoInterlaceProgressive            = 2
)

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
	guidEnableVideoProcessing  = newGUID(0x0f81da2c, 0xb537, 0x4672, 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3) // advanced video processing
)

// guidIMFMediaSource is what IMFActivate::ActivateObject is asked for here: the
// result is handed straight to MFCreateSourceReaderFromMediaSource.
var guidIMFMediaSource = newGUID(0x279a808d, 0xaec7, 0x40c8, 0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66)
var guidIUnknown = newGUID(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46)
var guidSourceReaderCallback = newGUID(0xdeec8d99, 0xfa1d, 0x4d82, 0x84, 0xc2, 0x2c, 0x89, 0x69, 0x94, 0x48, 0x67)

func newGUID(data1 uint32, data2, data3 uint16, rest ...byte) comGUID {
	var value comGUID
	value.d1 = data1
	value.d2 = data2
	value.d3 = data3
	copy(value.d4[:], rest)
	return value
}

var (
	modMfreadwrite = syscall.NewLazyDLL("mfreadwrite.dll")

	// mfModules is the search order for the Media Foundation platform entry
	// points. MFEnumDeviceSources is documented as an mf.dll export rather than
	// an mfplat.dll one, and Windows builds shuffle some of these between
	// mfplat.dll, mf.dll and mfcore.dll, so resolve each name across all of them
	// instead of trusting one module.
	mfModules = []*syscall.LazyDLL{
		syscall.NewLazyDLL("mfplat.dll"),
		syscall.NewLazyDLL("mf.dll"),
		syscall.NewLazyDLL("mfcore.dll"),
	}
	procCoUninitialize = ole32.NewProc("CoUninitialize")

	mfProcsOnce sync.Once
	mfProcsErr  error
	mfProcs     mfFunctions

	mfStartupOnce sync.Once
	mfStartupErr  hresult
)

// mfFunctions holds the resolved Media Foundation entry points. Resolving them
// explicitly (LazyProc.Find) keeps a missing export an error the launcher can
// report: LazyProc.Call panics instead, and a machine whose mfplat.dll did not
// carry MFEnumDeviceSources took the whole app down that way.
//
// MFSetAttributeSize and MFSetAttributeRatio are deliberately absent: they are
// inline helpers in mfapi.h that pack two UINT32s and call SetUINT64, not
// exported functions, so there is nothing to resolve for them.
type mfFunctions struct {
	startup            *syscall.LazyProc
	createAttributes   *syscall.LazyProc
	createMediaType    *syscall.LazyProc
	enumDeviceSources  *syscall.LazyProc
	createSourceReader *syscall.LazyProc
}

func findMFProc(name string) (*syscall.LazyProc, error) {
	for _, module := range mfModules {
		proc := module.NewProc(name)
		if err := proc.Find(); err == nil {
			return proc, nil
		}
	}
	return nil, fmt.Errorf("Media Foundation is missing %s", name)
}

func mediaFoundation() (*mfFunctions, error) {
	mfProcsOnce.Do(func() {
		for _, entry := range []struct {
			target **syscall.LazyProc
			name   string
		}{
			{&mfProcs.startup, "MFStartup"},
			{&mfProcs.createAttributes, "MFCreateAttributes"},
			{&mfProcs.createMediaType, "MFCreateMediaType"},
			{&mfProcs.enumDeviceSources, "MFEnumDeviceSources"},
		} {
			proc, err := findMFProc(entry.name)
			if err != nil {
				mfProcsErr = err
				return
			}
			*entry.target = proc
		}
		reader := modMfreadwrite.NewProc("MFCreateSourceReaderFromMediaSource")
		if err := reader.Find(); err != nil {
			mfProcsErr = errors.New("Media Foundation is missing MFCreateSourceReaderFromMediaSource")
			return
		}
		mfProcs.createSourceReader = reader
	})
	if mfProcsErr != nil {
		return nil, mfProcsErr
	}
	return &mfProcs, nil
}

// mfCall invokes a COM method through the object's vtable.
//
//go:uintptrescapes
func mfCall(obj unsafe.Pointer, method int, args ...uintptr) hresult {
	if obj == nil {
		return -1
	}
	result := hresult(int32(uint32(comCall(uintptr(obj), method, args...))))
	runtime.KeepAlive(obj)
	return result
}

func mfRelease(obj *unsafe.Pointer) {
	if obj != nil && *obj != nil {
		mfCall(*obj, 2) // IUnknown::Release
		*obj = nil
	}
}

//go:uintptrescapes
func procCall(proc *syscall.LazyProc, args ...uintptr) hresult {
	result, _, _ := proc.Call(args...)
	return hresult(int32(uint32(result)))
}

func setGUID(obj unsafe.Pointer, key *comGUID, value *comGUID) hresult {
	return mfCall(obj, 24, uintptr(unsafe.Pointer(key)), uintptr(unsafe.Pointer(value)))
}

func setUint32(obj unsafe.Pointer, key *comGUID, value uint32) hresult {
	return mfCall(obj, 21, uintptr(unsafe.Pointer(key)), uintptr(value))
}

func setUint64(obj unsafe.Pointer, key *comGUID, value uint64) hresult {
	return mfCall(obj, 22, uintptr(unsafe.Pointer(key)), uintptr(value)) // IMFAttributes::SetUINT64
}

// packUint32Pair packs two UINT32s the way mfapi.h's Pack2UINT32AsUINT64 does:
// the first value in the high 32 bits, the second in the low 32.
func packUint32Pair(high, low uint32) uint64 {
	return uint64(high)<<32 | uint64(low)
}

func setUnknown(obj unsafe.Pointer, key *comGUID, value unsafe.Pointer) hresult {
	return mfCall(obj, 27, uintptr(unsafe.Pointer(key)), uintptr(value))
}

func getUint32(obj unsafe.Pointer, key *comGUID) uint32 {
	var value uint32
	mfCall(obj, 7, uintptr(unsafe.Pointer(key)), uintptr(unsafe.Pointer(&value))) // IMFAttributes::GetUINT32
	return value
}

// mfCameraSource -------------------------------------------------------------

type mfCameraSource struct {
	deviceID       string
	frames         chan []byte
	reader         unsafe.Pointer
	attrs          unsafe.Pointer
	activate       unsafe.Pointer
	devices        *unsafe.Pointer
	callback       *cameraCallback
	stride         int
	stopped        bool
	comInitialized bool
	ended          bool
	stopReq        chan struct{}
	done           chan struct{}
	mu             sync.Mutex
}

// Native COM references are invisible to Go's GC. Retain and pin each callback
// until its last COM reference is released, including late callbacks after stop.
var cameraCallbacks sync.Map

type cameraCallback struct {
	vtable *[6]uintptr
	source *mfCameraSource
	refs   atomic.Int32
	pin    runtime.Pinner
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
	callback.refs.Store(1)
	callback.pin.Pin(callback)
	callback.pin.Pin(callback.vtable)
	cameraCallbacks.Store(callback, struct{}{})
	return callback
}

func callbackQueryInterface(this, riid, ppv uintptr) uintptr {
	if ppv == 0 || riid == 0 {
		return 0x80004003 // E_POINTER
	}
	*(*uintptr)(unsafe.Pointer(ppv)) = 0
	iid := *(*comGUID)(unsafe.Pointer(riid))
	if iid != guidIUnknown && iid != guidSourceReaderCallback {
		return 0x80004002 // E_NOINTERFACE
	}
	callbackAddRef(this)
	*(*uintptr)(unsafe.Pointer(ppv)) = this
	return uintptr(sOK)
}

func callbackAddRef(this uintptr) uintptr {
	return uintptr((*cameraCallback)(unsafe.Pointer(this)).refs.Add(1))
}

func callbackRelease(this uintptr) uintptr {
	callback := (*cameraCallback)(unsafe.Pointer(this))
	refs := callback.refs.Add(-1)
	if refs == 0 {
		cameraCallbacks.Delete(callback)
		callback.pin.Unpin()
	}
	return uintptr(refs)
}

func callbackOnFlush(uintptr, uintptr) uintptr { return uintptr(sOK) }

func callbackOnEvent(uintptr, uintptr, uintptr) uintptr { return uintptr(sOK) }

func callbackOnReadSample(this, hrStatus, streamIndex, streamFlags, timestamp, sample uintptr) uintptr {
	callback := (*cameraCallback)(unsafe.Pointer(this))
	callback.source.handleSample(callback, hresult(int32(uint32(hrStatus))), uint32(streamFlags), sample)
	return uintptr(sOK)
}

func startMediaFoundation() error {
	api, err := mediaFoundation()
	if err != nil {
		return err
	}
	mfStartupOnce.Do(func() {
		mfStartupErr = procCall(api.startup, mfVersion, mfStartupLite)
	})
	if mfStartupErr < 0 {
		return fmt.Errorf("Media Foundation startup failed (0x%08x)", uint32(mfStartupErr))
	}
	return nil
}

// start and stop are serialized by serveCamera. A dedicated OS thread owns
// COM initialization and cleanup; Go may move the network goroutine at any time.
func (s *mfCameraSource) start() (<-chan []byte, error) {
	if s.stopReq != nil {
		return s.frames, nil
	}
	ready := make(chan error, 1)
	stopReq, done := make(chan struct{}), make(chan struct{})
	s.stopReq, s.done = stopReq, done
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		defer close(done)
		defer s.close()
		if hr := procCall(procCoInitializeEx, 0, coInitMultithreaded); hr < 0 {
			ready <- fmt.Errorf("COM initialization failed (0x%08x)", uint32(hr))
			return
		}
		s.comInitialized = true
		if err := startMediaFoundation(); err != nil {
			ready <- err
			return
		}
		if err := s.open(); err != nil {
			ready <- err
			return
		}
		s.mu.Lock()
		s.frames = make(chan []byte, 4)
		s.stopped, s.ended = false, false
		hr := mfCall(s.reader, 9, mfSourceReaderFirstVideoStream, 0, 0, 0, 0, 0)
		s.mu.Unlock()
		if hr < 0 {
			ready <- fmt.Errorf("camera capture failed to start (0x%08x)", uint32(hr))
			return
		}
		ready <- nil
		<-stopReq
	}()
	if err := <-ready; err != nil {
		<-done
		s.stopReq, s.done = nil, nil
		return nil, err
	}
	return s.frames, nil
}

func (s *mfCameraSource) open() error {
	api, err := mediaFoundation()
	if err != nil {
		return err
	}
	var attributes unsafe.Pointer
	if hr := procCall(api.createAttributes, uintptr(unsafe.Pointer(&attributes)), 1); hr < 0 {
		return fmt.Errorf("MFCreateAttributes failed (0x%08x)", uint32(hr))
	}
	if hr := setGUID(attributes, &guidDeviceSourceType, &guidDeviceSourceTypeVidcap); hr < 0 {
		mfRelease(&attributes)
		return fmt.Errorf("selecting camera devices failed (0x%08x)", uint32(hr))
	}

	var count uint32
	var devices *unsafe.Pointer
	if hr := procCall(api.enumDeviceSources, uintptr(attributes), uintptr(unsafe.Pointer(&devices)), uintptr(unsafe.Pointer(&count))); hr < 0 {
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
	for _, device := range array {
		if s.activate == nil && (s.deviceID == "" || cameraAttribute(device, &guidCameraLink) == s.deviceID) {
			s.activate = device
		} else {
			mfRelease(&device)
		}
	}
	if s.activate == nil {
		mfRelease(&attributes)
		return errors.New("The selected camera is disconnected. Reconnect it or choose another camera in Try Omarchy Settings.")
	}

	var source unsafe.Pointer
	if hr := mfCall(s.activate, 33, uintptr(unsafe.Pointer(&guidIMFMediaSource)), uintptr(unsafe.Pointer(&source))); hr < 0 { // IMFActivate::ActivateObject
		mfRelease(&attributes)
		return fmt.Errorf("The camera could not be opened. Close other camera apps and check Windows camera privacy settings (0x%08x)", uint32(hr))
	}

	var callbackAttrs unsafe.Pointer
	if hr := procCall(api.createAttributes, uintptr(unsafe.Pointer(&callbackAttrs)), 1); hr < 0 {
		mfRelease(&source)
		mfRelease(&attributes)
		return fmt.Errorf("MFCreateAttributes failed (0x%08x)", uint32(hr))
	}
	// The basic processing flag only converts YUV to RGB. The advanced
	// processor can negotiate our fixed NV12 size/rate from other camera modes.
	if hr := setUint32(callbackAttrs, &guidEnableVideoProcessing, 1); hr < 0 {
		mfRelease(&source)
		mfRelease(&callbackAttrs)
		mfRelease(&attributes)
		return fmt.Errorf("enabling camera format conversion failed (0x%08x)", uint32(hr))
	}
	s.mu.Lock()
	s.callback = newCameraCallback(s)
	s.mu.Unlock()
	if hr := setUnknown(callbackAttrs, &guidAsyncCallback, unsafe.Pointer(s.callback)); hr < 0 {
		mfRelease(&source)
		mfRelease(&callbackAttrs)
		mfRelease(&attributes)
		return fmt.Errorf("installing the camera callback failed (0x%08x)", uint32(hr))
	}

	var reader unsafe.Pointer
	if hr := procCall(api.createSourceReader, uintptr(source), uintptr(callbackAttrs), uintptr(unsafe.Pointer(&reader))); hr < 0 {
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
	api, err := mediaFoundation()
	if err != nil {
		return err
	}
	var media unsafe.Pointer
	if hr := procCall(api.createMediaType, uintptr(unsafe.Pointer(&media))); hr < 0 {
		return fmt.Errorf("MFCreateMediaType failed (0x%08x)", uint32(hr))
	}
	for _, hr := range []hresult{
		setGUID(media, &guidMajorType, &guidMediaTypeVideo),
		setGUID(media, &guidSubtype, &guidVideoFormatNV12),
		setUint64(media, &guidFrameSize, packUint32Pair(cameraWidth, cameraHeight)),
		setUint64(media, &guidFrameRate, packUint32Pair(30, 1)),
		setUint32(media, &guidInterlaceMode, mfVideoInterlaceProgressive),
	} {
		if hr < 0 {
			mfRelease(&media)
			return fmt.Errorf("setting camera format attributes failed (0x%08x)", uint32(hr))
		}
	}

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
	return nil
}

func (s *mfCameraSource) handleSample(callback *cameraCallback, status hresult, flags uint32, sample uintptr) {
	s.mu.Lock()
	defer s.mu.Unlock()
	// OnReadSample lends us the sample for this callback. It does not transfer
	// a reference; only the buffer acquired below belongs to us.
	if s.callback != callback || s.stopped || s.ended || s.reader == nil {
		return
	}
	if status < 0 || flags&(1|2|32) != 0 {
		logf("camera: capture ended or format changed (status=0x%08x flags=0x%x)", uint32(status), flags)
		s.endFramesLocked()
		return
	}
	if sample != 0 {
		if frame := s.copyFrame(sample); frame != nil && s.frames != nil {
			select {
			case s.frames <- frame:
			default: // drop a frame rather than stall the camera callback
			}
		}
	}
	// Re-request while still holding the lock so stop() cannot release the
	// reader between the check and the call.
	if hr := mfCall(s.reader, 9, mfSourceReaderFirstVideoStream, 0, 0, 0, 0, 0); hr < 0 {
		logf("camera: requesting sample failed (0x%08x)", uint32(hr))
		s.endFramesLocked()
	}
}

// copyFrame converts one camera sample to tightly packed NV12, dropping any row
// padding the device reported.
func (s *mfCameraSource) copyFrame(sample uintptr) []byte {
	var buffer unsafe.Pointer
	if hr := mfCall(unsafe.Pointer(sample), 41, uintptr(unsafe.Pointer(&buffer))); hr < 0 { // IMFSample::ConvertToContiguousBuffer
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

func (s *mfCameraSource) endFramesLocked() {
	if s.frames != nil && !s.ended {
		close(s.frames)
		s.ended = true
	}
}

func (s *mfCameraSource) stop() {
	if s.stopReq != nil {
		close(s.stopReq)
		<-s.done
		s.stopReq, s.done = nil, nil
	}
}

func (s *mfCameraSource) close() {
	// Never release the reader while holding the callback mutex: native
	// shutdown may wait for an in-flight callback to finish.
	s.mu.Lock()
	s.stopped = true
	s.endFramesLocked()
	reader, callback := s.reader, s.callback
	s.reader, s.callback = nil, nil
	s.mu.Unlock()
	if reader != nil {
		mfCall(reader, 10, mfSourceReaderFirstVideoStream) // Flush
	}
	if s.activate != nil {
		mfCall(s.activate, 34) // ShutdownObject releases the camera device
	}
	mfRelease(&reader)
	mfRelease(&s.attrs)
	mfRelease(&s.activate)
	if callback != nil {
		callbackRelease(uintptr(unsafe.Pointer(callback)))
	}
	if s.devices != nil {
		procCoTaskMemFree.Call(uintptr(unsafe.Pointer(s.devices)))
		s.devices = nil
	}
	if s.comInitialized {
		procCoUninitialize.Call()
		s.comInitialized = false
	}
}
