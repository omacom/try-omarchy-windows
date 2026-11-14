//go:build windows

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"os"
	"os/exec"
	"strconv"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// nativeGPUPrepare is supplied only by an explicitly prepared backend profile.
// No released launcher setting selects it. The profile must preserve the
// original command/environment and establish the matching guest runtime.
type nativeGPUPrepare func(original *exec.Cmd) (*exec.Cmd, error)

type supervisedGuest struct {
	command *exec.Cmd
	owner   *nativeGPUOwner
}

func startSupervisedGuest(ctx context.Context, command *exec.Cmd, prepare nativeGPUPrepare) (*supervisedGuest, error) {
	if prepare == nil {
		if err := command.Start(); err != nil {
			return nil, err
		}
		return &supervisedGuest{command: command}, nil
	}
	image, directory, input, output, errorOutput := command.Path, command.Dir, command.Stdin, command.Stdout, command.Stderr
	environment := cloneNativeEnvironment(command.Env)
	prepared, err := prepare(command)
	if err != nil {
		return nil, err
	}
	if prepared == nil || prepared == command {
		return nil, errors.New("native GPU profile did not prepare a separate owner")
	}
	// Keep the launcher's audio/keyboard/pinch environment and log ownership at
	// this boundary. A backend profile prepares graphics ownership arguments;
	// it cannot silently replace the official input environment or streams.
	prepared.Env = environment
	prepared.Dir, prepared.Stdin = directory, input
	prepared.Stdout, prepared.Stderr = output, errorOutput
	owner, err := startNativeGPUOwner(ctx, prepared, image)
	if err != nil {
		return nil, err
	}
	return &supervisedGuest{command: prepared, owner: owner}, nil
}

func cloneNativeEnvironment(environment []string) []string {
	if environment == nil {
		return nil
	}
	result := make([]string, len(environment))
	copy(result, environment)
	return result
}

func (g *supervisedGuest) pid() int {
	if g.owner != nil {
		g.owner.Lock()
		defer g.owner.Unlock()
		if g.owner.qemuReaped {
			return 0
		}
		return int(g.owner.identity.QEMUPID)
	}
	return g.command.Process.Pid
}

func (g *supervisedGuest) wait() error {
	if g.owner != nil {
		return g.owner.wait()
	}
	return g.command.Wait()
}

func (g *supervisedGuest) kill() error {
	if g.owner != nil {
		return g.owner.cancel()
	}
	return g.command.Process.Kill()
}

// Only native owners need an alternate stop operation. Default process start,
// waiting and shutdown retain the existing exec/FindProcess behavior.
var nativeGuestStop struct {
	sync.Mutex
	guest *supervisedGuest
}

func publishSupervisedGuest(g *supervisedGuest) {
	nativeGuestStop.Lock()
	defer nativeGuestStop.Unlock()
	nativeGuestStop.guest = nil
	if g.owner != nil {
		nativeGuestStop.guest = g
	}
	qemuPid.Store(uint32(g.pid()))
}

func stopSupervisedGuest(pid uint32) error {
	nativeGuestStop.Lock()
	defer nativeGuestStop.Unlock()
	if g := nativeGuestStop.guest; g != nil {
		if uint32(g.pid()) != pid {
			return errors.New("stop PID is not the bound native guest")
		}
		return g.kill()
	}
	if process, err := os.FindProcess(int(pid)); err == nil {
		return process.Kill()
	} else {
		return err
	}
}

type nativeVMIdentity struct {
	Ready           bool   `json:"nativeLauncherVmReady,omitempty"`
	Bound           bool   `json:"nativeLauncherVmBound,omitempty"`
	Protocol        uint32 `json:"protocol"`
	LaunchMode      string `json:"launchMode"`
	BridgePID       uint32 `json:"bridgePid"`
	LauncherPID     uint32 `json:"launcherPid"`
	LauncherCreated string `json:"launcherCreationTime100ns"`
	QEMUPID         uint32 `json:"qemuPid"`
	QEMUCreatorPID  uint32 `json:"qemuCreatorPid"`
	QEMUCreated     string `json:"qemuCreationTime100ns"`
	Held            bool   `json:"heldProcessHandle"`
	Job             bool   `json:"privateJobAssigned"`
	Pinned          bool   `json:"imageFilePinned"`
	QMP             bool   `json:"qmpReady"`
	Graphics        bool   `json:"guestGraphicsVerified"`
	Input           bool   `json:"launcherInputVerified"`
	Daily           bool   `json:"dailyUseAccepted"`
}

// Reject duplicate, unknown and missing fields; false acceptance flags are
// mandatory. Creation times remain canonical decimal strings beyond 2**53.
func decodeNativeVMIdentity(line []byte) (nativeVMIdentity, error) {
	return decodeNativeVMRecord(line, false)
}

func decodeNativeVMRecord(line []byte, binding bool) (nativeVMIdentity, error) {
	var identity nativeVMIdentity
	decoder := json.NewDecoder(bytes.NewReader(line))
	token, err := decoder.Token()
	if err != nil || token != json.Delim('{') {
		return identity, errors.New("native VM identity is not an object")
	}
	fields := make(map[string]bool)
	values := make(map[string]json.RawMessage)
	allowed := map[string]bool{}
	for _, key := range []string{"nativeLauncherVmReady", "protocol", "launchMode", "bridgePid", "launcherPid",
		"launcherCreationTime100ns", "qemuPid", "qemuCreatorPid", "qemuCreationTime100ns", "heldProcessHandle",
		"privateJobAssigned", "imageFilePinned", "qmpReady", "guestGraphicsVerified", "launcherInputVerified", "dailyUseAccepted"} {
		allowed[key] = true
	}
	if binding {
		delete(allowed, "nativeLauncherVmReady")
		allowed["nativeLauncherVmBound"] = true
	}
	for decoder.More() {
		token, err = decoder.Token()
		key, ok := token.(string)
		if err != nil || !ok || fields[key] || !allowed[key] {
			return identity, errors.New("duplicate or invalid native VM identity field")
		}
		fields[key] = true
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return identity, err
		}
		values[key] = value
	}
	if _, err := decoder.Token(); err != nil {
		return identity, err
	}
	if len(fields) != 16 {
		return identity, errors.New("native VM identity field count differs")
	}
	for _, key := range []string{"guestGraphicsVerified", "launcherInputVerified", "dailyUseAccepted"} {
		if !bytes.Equal(bytes.TrimSpace(values[key]), []byte("false")) {
			return identity, errors.New("native VM acceptance flags must be explicit false")
		}
	}
	if binding && !bytes.Equal(bytes.TrimSpace(values["qmpReady"]), []byte("false")) {
		return identity, errors.New("bound VM must explicitly keep QMP readiness false")
	}
	decoder = json.NewDecoder(bytes.NewReader(line))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&identity); err != nil {
		return identity, err
	}
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return identity, errors.New("trailing native VM identity data")
	}
	if (binding && (!identity.Bound || identity.Ready || identity.QMP)) || (!binding && (!identity.Ready || identity.Bound || !identity.QMP)) ||
		identity.Protocol != 1 || identity.LaunchMode != "backend-owned" ||
		!identity.Held || !identity.Job || !identity.Pinned || identity.Graphics || identity.Input || identity.Daily ||
		identity.BridgePID == 0 || identity.LauncherPID == 0 || identity.QEMUPID == 0 ||
		identity.BridgePID == identity.LauncherPID || identity.QEMUPID == identity.LauncherPID || identity.QEMUPID == identity.BridgePID ||
		identity.QEMUCreatorPID != identity.BridgePID {
		return identity, errors.New("native VM identity contract differs")
	}
	for _, text := range []string{identity.LauncherCreated, identity.QEMUCreated} {
		value, err := strconv.ParseUint(text, 10, 64)
		if err != nil || value == 0 || strconv.FormatUint(value, 10) != text {
			return identity, errors.New("invalid native VM creation timestamp")
		}
	}
	return identity, nil
}

var nativeOwnerKernel = syscall.NewLazyDLL("kernel32.dll")
var nativeOwnerCreateEvent = nativeOwnerKernel.NewProc("CreateEventW")
var nativeOwnerSetEvent = nativeOwnerKernel.NewProc("SetEvent")
var nativeOwnerQueryImage = nativeOwnerKernel.NewProc("QueryFullProcessImageNameW")

func nativeProcessCreated(process syscall.Handle) (uint64, error) {
	var created, exited, kernel, user syscall.Filetime
	if err := syscall.GetProcessTimes(process, &created, &exited, &kernel, &user); err != nil {
		return 0, err
	}
	return uint64(created.HighDateTime)<<32 | uint64(created.LowDateTime), nil
}

func nativeProcessParent(pid uint32) (uint32, error) {
	snapshot, err := syscall.CreateToolhelp32Snapshot(syscall.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return 0, err
	}
	defer syscall.CloseHandle(snapshot)
	var entry syscall.ProcessEntry32
	entry.Size = uint32(unsafe.Sizeof(entry))
	for err := syscall.Process32First(snapshot, &entry); err == nil; err = syscall.Process32Next(snapshot, &entry) {
		if entry.ProcessID == pid {
			return entry.ParentProcessID, nil
		}
	}
	return 0, errors.New("native process disappeared from snapshot")
}

func nativeProcessImage(process syscall.Handle) (string, error) {
	buffer := make([]uint16, 32768)
	size := uint32(len(buffer))
	result, _, err := nativeOwnerQueryImage.Call(uintptr(process), 0, uintptr(unsafe.Pointer(&buffer[0])), uintptr(unsafe.Pointer(&size)))
	if result == 0 {
		return "", err
	}
	return syscall.UTF16ToString(buffer[:size]), nil
}

type nativeGPUOwner struct {
	sync.Mutex
	command      *exec.Cmd
	event        syscall.Handle
	qemu         syscall.Handle
	qemuExit     uint32
	qemuReaped   bool
	identity     nativeVMIdentity
	ready        chan error
	done         chan struct{}
	result       error
	observed     bool
	bindingSeen  bool
	controlReady bool
	output       io.Writer
	outputOwned  io.Closer
	pending      []byte
	failed       error
}

func startNativeGPUOwner(ctx context.Context, command *exec.Cmd, qemuImage string) (*nativeGPUOwner, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	if command.Process != nil || command.SysProcAttr != nil || command.Cancel != nil || command.WaitDelay != 0 {
		return nil, errors.New("native owner requires a fresh private process configuration")
	}
	for _, option := range command.Args {
		if option == "--driver-launcher-cancel-handle" {
			return nil, errors.New("native owner cancellation was already configured")
		}
	}
	security := syscall.SecurityAttributes{InheritHandle: 1}
	security.Length = uint32(unsafe.Sizeof(security))
	handle, _, err := nativeOwnerCreateEvent.Call(uintptr(unsafe.Pointer(&security)), 1, 0, 0)
	if handle == 0 {
		return nil, err
	}
	owner := &nativeGPUOwner{command: command, event: syscall.Handle(handle), ready: make(chan error, 1), done: make(chan struct{}), output: command.Stdout}
	// The original supervisor closes its log entry after Start: direct QEMU
	// inherits a separate entry. Our protocol reader instead needs its own copy
	// until stdout has drained, including after native teardown.
	if file, ok := owner.output.(*os.File); ok {
		process, processErr := syscall.GetCurrentProcess()
		var duplicate syscall.Handle
		if processErr == nil {
			processErr = syscall.DuplicateHandle(process, syscall.Handle(file.Fd()), process, &duplicate, 0, false, syscall.DUPLICATE_SAME_ACCESS)
		}
		if processErr != nil {
			syscall.CloseHandle(owner.event)
			return nil, processErr
		}
		copy := os.NewFile(uintptr(duplicate), file.Name())
		owner.output, owner.outputOwned = copy, copy
	}
	command.Args = append(command.Args, "--driver-launcher-cancel-handle", strconv.FormatUint(uint64(handle), 10))
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000, AdditionalInheritedHandles: []syscall.Handle{owner.event}}
	command.Stdout = &nativeOwnerOutput{owner: owner, image: qemuImage}
	if err := command.Start(); err != nil {
		syscall.CloseHandle(owner.event)
		if owner.outputOwned != nil {
			owner.outputOwned.Close()
		}
		return nil, err
	}
	go owner.reap()
	go func() {
		select {
		case <-ctx.Done():
			_ = owner.cancel()
		case <-owner.done:
		}
	}()
	select {
	case err := <-owner.ready:
		if err == nil && ctx.Err() == nil {
			return owner, nil
		}
		if err != nil {
			return nil, owner.failedStartup(err)
		}
		return nil, owner.failedStartup(ctx.Err())
	case <-ctx.Done():
		return nil, owner.failedStartup(ctx.Err())
	case <-time.After(45 * time.Second):
		return nil, owner.failedStartup(errors.New("native GPU VM identity deadline exceeded"))
	case <-owner.done:
		if owner.identity.QEMUPID != 0 && owner.qemuReaped && owner.failed == nil {
			// The VM was genuinely started and already exited. Let supervise's
			// existing early-exit/memory/audio/runtime retry logic handle it.
			return owner, nil
		}
		return nil, errors.Join(errors.New("native GPU owner exited before VM identity publication"), owner.result)
	}
}

func (owner *nativeGPUOwner) failedStartup(cause error) error {
	cancelErr := owner.cancel()
	select {
	case <-owner.done:
		return errors.Join(cause, cancelErr, owner.result)
	case <-time.After(15 * time.Second):
		// The reaper and native owner retain their objects in the background.
		// A timeout never authorizes killing a still-live GPU page owner.
		return errors.Join(cause, cancelErr, errors.New("native cleanup still pending; owner retained until VM exit"))
	}
}

func (owner *nativeGPUOwner) cancel() error {
	owner.Lock()
	defer owner.Unlock()
	if owner.event == 0 {
		return nil
	}
	result, _, err := nativeOwnerSetEvent.Call(uintptr(owner.event))
	if result == 0 {
		return err
	}
	return nil
}

func (owner *nativeGPUOwner) wait() error {
	<-owner.done
	return owner.result
}

func (owner *nativeGPUOwner) reap() {
	result := owner.command.Wait() // Also joins stdout observation and drains logs.
	if owner.outputOwned != nil {
		result = errors.Join(result, owner.outputOwned.Close())
	}
	owner.Lock()
	defer owner.Unlock()
	if len(bytes.TrimSpace(owner.pending)) != 0 {
		owner.failed = errors.Join(owner.failed, errors.New("incomplete native owner output line at exit"))
	}
	if owner.qemu != 0 {
		// Owner-death job teardown can finish after the native process exits.
		state, err := syscall.WaitForSingleObject(owner.qemu, 10000)
		if err != nil || state != syscall.WAIT_OBJECT_0 {
			// Retain the held process object; never replace it with a bare PID.
			result = errors.Join(result, errors.New("native owner ended without proven QEMU exit"))
		} else {
			if err := syscall.GetExitCodeProcess(owner.qemu, &owner.qemuExit); err != nil {
				result = errors.Join(result, err)
			} else {
				owner.qemuReaped = true
				if owner.qemuExit != 0 && result == nil {
					result = errors.New("native owner exited successfully but QEMU did not")
				}
				syscall.CloseHandle(owner.qemu)
				owner.qemu = 0
			}
		}
	}
	if owner.event != 0 {
		syscall.CloseHandle(owner.event)
		owner.event = 0
	}
	owner.result = errors.Join(result, owner.failed)
	if !owner.observed {
		owner.ready <- errors.Join(owner.result, errors.New("native VM identity missing"))
	}
	close(owner.done)
}

type nativeOwnerOutput struct {
	owner *nativeGPUOwner
	image string
}

func (writer *nativeOwnerOutput) Write(data []byte) (int, error) {
	owner := writer.owner
	// One exec copy goroutine owns framing; cancellation/reaping share only the
	// guarded identity/event state. Keep draining after a rejected identity.
	for _, value := range data {
		if len(owner.pending) >= 65536 {
			writer.reject(errors.New("native owner output line exceeds 64 KiB"))
			owner.pending = nil
		}
		owner.pending = append(owner.pending, value)
		if value == '\n' {
			writer.line(owner.pending)
			owner.pending = nil
		}
	}
	if owner.output != nil {
		if _, err := owner.output.Write(data); err != nil {
			writer.reject(err)
		}
	}
	return len(data), nil
}

func (writer *nativeOwnerOutput) reject(err error) {
	owner := writer.owner
	owner.Lock()
	if owner.failed == nil {
		owner.failed = err
		if !owner.observed {
			owner.observed = true
			owner.ready <- err
		}
	}
	owner.Unlock()
	_ = owner.cancel()
}

func (writer *nativeOwnerOutput) line(line []byte) {
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(line, &fields); err != nil {
		writer.reject(err)
		return
	}
	_, binding := fields["nativeLauncherVmBound"]
	if _, ok := fields["nativeLauncherVmReady"]; !ok && !binding {
		return // Existing native status records are forwarded unchanged.
	}
	identity, err := decodeNativeVMRecord(line, binding)
	if err != nil {
		writer.reject(err)
		return
	}
	owner := writer.owner
	process, err := syscall.OpenProcess(0x00101000, false, identity.QEMUPID)
	if err != nil {
		writer.reject(err)
		return
	}
	keep := false
	defer func() {
		if !keep {
			syscall.CloseHandle(process)
		}
	}()
	created, err := nativeProcessCreated(process)
	if err != nil {
		writer.reject(err)
		return
	}
	parent, err := nativeProcessParent(identity.QEMUPID)
	if err != nil {
		writer.reject(err)
		return
	}
	self, err := syscall.GetCurrentProcess()
	if err != nil {
		writer.reject(err)
		return
	}
	selfCreated, err := nativeProcessCreated(self)
	if err != nil {
		writer.reject(err)
		return
	}
	image, err := nativeProcessImage(process)
	if err != nil {
		writer.reject(err)
		return
	}
	actual, actualErr := os.Stat(image)
	expected, expectedErr := os.Stat(writer.image)
	state, waitErr := syscall.WaitForSingleObject(process, 0)
	bridgeParent, bridgeErr := nativeProcessParent(uint32(owner.command.Process.Pid))
	if identity.BridgePID != uint32(owner.command.Process.Pid) || identity.LauncherPID != uint32(os.Getpid()) ||
		identity.LauncherCreated != strconv.FormatUint(selfCreated, 10) || identity.QEMUCreated != strconv.FormatUint(created, 10) ||
		created < selfCreated || parent != identity.BridgePID || bridgeErr != nil || bridgeParent != uint32(os.Getpid()) ||
		actualErr != nil || expectedErr != nil || !os.SameFile(actual, expected) || waitErr != nil || state != syscall.WAIT_TIMEOUT {
		writer.reject(errors.New("native VM identity differs from held Windows process objects"))
		return
	}
	owner.Lock()
	if owner.observed && owner.bindingSeen && !owner.controlReady && !binding && owner.failed == nil {
		before := owner.identity
		before.Ready, before.Bound, before.QMP = true, false, true
		if before != identity {
			owner.Unlock()
			writer.reject(errors.New("native control readiness changed the bound VM identity"))
			return
		}
		owner.identity, owner.controlReady = identity, true
		owner.Unlock()
		return // Keep the original process handle and single start notification.
	}
	if owner.observed || owner.failed != nil {
		owner.Unlock()
		writer.reject(errors.New("duplicate native VM identity"))
		return
	}
	owner.identity, owner.qemu, owner.observed = identity, process, true
	owner.bindingSeen, owner.controlReady = binding, !binding
	keep = true
	owner.ready <- nil
	owner.Unlock()
}
