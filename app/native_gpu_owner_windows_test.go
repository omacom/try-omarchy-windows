//go:build windows

package main

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
)

func nativeIdentityFixture() map[string]any {
	return map[string]any{"nativeLauncherVmReady": true, "protocol": 1, "launchMode": "backend-owned",
		"bridgePid": 102, "launcherPid": 101, "launcherCreationTime100ns": "134000000000000001",
		"qemuPid": 103, "qemuCreatorPid": 102, "qemuCreationTime100ns": "134000000000000124",
		"heldProcessHandle": true, "privateJobAssigned": true, "imageFilePinned": true, "qmpReady": true,
		"guestGraphicsVerified": false, "launcherInputVerified": false, "dailyUseAccepted": false}
}

func TestNativeGPUIdentityProtocol(t *testing.T) {
	fixture, _ := json.Marshal(nativeIdentityFixture())
	if identity, err := decodeNativeVMIdentity(fixture); err != nil || identity.QEMUPID != 103 || identity.QEMUCreated != "134000000000000124" {
		t.Fatalf("exact native identity rejected: %+v %v", identity, err)
	}
	mutations := map[string]any{"protocol": 2, "launchMode": "launcher-owned", "qemuCreatorPid": 104,
		"heldProcessHandle": false, "privateJobAssigned": false, "imageFilePinned": false, "qmpReady": false,
		"guestGraphicsVerified": true, "launcherInputVerified": true, "dailyUseAccepted": true,
		"launcherCreationTime100ns": float64(134000000000000001), "qemuCreationTime100ns": "0134000000000000124",
		"qemuPid": 102, "bridgePid": 101}
	for field, value := range mutations {
		t.Run(field, func(t *testing.T) {
			message := nativeIdentityFixture()
			message[field] = value
			data, _ := json.Marshal(message)
			if _, err := decodeNativeVMIdentity(data); err == nil {
				t.Fatal("altered identity accepted")
			}
		})
	}
	for field := range nativeIdentityFixture() {
		t.Run("missing-"+field, func(t *testing.T) {
			message := nativeIdentityFixture()
			delete(message, field)
			data, _ := json.Marshal(message)
			if _, err := decodeNativeVMIdentity(data); err == nil {
				t.Fatal("missing identity field accepted")
			}
		})
		t.Run("case-"+field, func(t *testing.T) {
			message := nativeIdentityFixture()
			value := message[field]
			delete(message, field)
			message[strings.ToUpper(field)] = value
			data, _ := json.Marshal(message)
			if _, err := decodeNativeVMIdentity(data); err == nil {
				t.Fatal("case-folded identity field accepted")
			}
		})
	}
	for _, field := range []string{"guestGraphicsVerified", "launcherInputVerified", "dailyUseAccepted"} {
		message := nativeIdentityFixture()
		message[field] = nil
		data, _ := json.Marshal(message)
		if _, err := decodeNativeVMIdentity(data); err == nil {
			t.Fatal("null acceptance flag accepted")
		}
	}
	for _, data := range []string{string(fixture) + " {}", strings.Replace(string(fixture), "{", `{"protocol":1,`, 1),
		strings.Replace(string(fixture), "{", `{"future":true,`, 1)} {
		if _, err := decodeNativeVMIdentity([]byte(data)); err == nil {
			t.Fatal("ambiguous identity accepted")
		}
	}
}

func TestNativeGPUCancelledBeforeStart(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	command := exec.Command("must-not-run-native-gpu.exe")
	if owner, err := startNativeGPUOwner(ctx, command, "unused-qemu.exe"); err == nil || owner != nil || command.Process != nil {
		t.Fatal("cancelled context attempted native startup")
	}
}

func TestSupervisedGuestDirectHelper(t *testing.T) {
	if os.Getenv("OMARCHY_PRIVATE_DIRECT_PROCESS_HELPER") != "1" {
		t.Skip("hidden own-child control only")
	}
	os.Exit(0)
}

func TestSupervisedGuestDefaultProcess(t *testing.T) {
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	command := exec.Command(self, "-test.run=^TestSupervisedGuestDirectHelper$")
	command.Env = append(os.Environ(), "OMARCHY_PRIVATE_DIRECT_PROCESS_HELPER=1")
	command.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	guest, err := startSupervisedGuest(context.Background(), command, nil)
	if err != nil {
		t.Fatal(err)
	}
	if guest.owner != nil || guest.pid() != command.Process.Pid {
		t.Fatal("default process identity changed")
	}
	if err := guest.wait(); err != nil {
		t.Fatal(err)
	}
}

func TestNativeGPURejectAutomaticOwnerKill(t *testing.T) {
	command := exec.CommandContext(context.Background(), "must-not-run-native-gpu.exe")
	if _, err := startNativeGPUOwner(context.Background(), command, "unused-qemu.exe"); err == nil || command.Process != nil {
		t.Fatal("automatic native-owner kill accepted")
	}
}

// Explicit private integration entry point. Ordinary tests and CI skip it.
// The parent must provide an owned hidden QEMU command; this never runs the
// launcher UI, dispatches host input or selects a guest disk itself.
func TestNativeGPUProcessHost(t *testing.T) {
	configuration := os.Getenv("OMARCHY_NATIVE_OWNER_PRIVATE_COMMAND")
	if configuration == "" {
		t.Skip("private hidden GPU integration only")
	}
	var command []string
	if err := json.Unmarshal([]byte(configuration), &command); err != nil || len(command) < 7 || command[1] != "--run-qemu" {
		t.Fatal("invalid private native command")
	}
	output := os.Getenv("OMARCHY_NATIVE_OWNER_PRIVATE_RECEIPT")
	if output == "" {
		t.Fatal("fresh private integration receipt required")
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	original := exec.Command(command[2])
	original.Env, original.Stdout, original.Stderr = os.Environ(), os.Stdout, os.Stderr
	guest, err := startSupervisedGuest(ctx, original, func(original *exec.Cmd) (*exec.Cmd, error) {
		prepared := exec.Command(command[0], command[1:]...)
		prepared.Env = os.Environ()
		prepared.Stdout, prepared.Stderr = os.Stdout, os.Stderr
		return prepared, nil
	})
	if err != nil {
		t.Fatal(err)
	}
	publishSupervisedGuest(guest)
	// Match main.supervise closing its original log entry after start. The
	// protocol reader's independently retained output entry must keep working.
	if err := os.Stdout.Close(); err != nil {
		t.Fatal(err)
	}
	published := qemuPid.Load()
	if published != uint32(guest.pid()) || published == uint32(guest.command.Process.Pid) {
		t.Fatal("supervisor published the GPU owner instead of QEMU")
	}
	// A caller confusing bridge PID with QEMU must not kill the GPU page owner.
	if err := stopSupervisedGuest(uint32(guest.command.Process.Pid)); err == nil {
		t.Fatal("stop accepted the GPU owner PID instead of the bound guest")
	}
	requestCancel := os.Getenv("OMARCHY_NATIVE_OWNER_PRIVATE_CANCEL") == "1"
	if requestCancel {
		if err := stopSupervisedGuest(published); err != nil {
			t.Fatal(err)
		}
	}
	waitErr := guest.wait()
	owner := guest.owner
	data, err := json.MarshalIndent(map[string]any{"schema": 1, "scope": "actual Go supervisor boundary; private hidden GPU workload",
		"actualGoCreatorPid": os.Getpid(), "bridgePid": guest.command.Process.Pid, "publishedQemuPid": published,
		"actualNativeCommand": guest.command.Args, "parentOutputEntryClosedAfterStart": true,
		"wrongNativeOwnerStopPidRejected": true,
		"identity":                        owner.identity, "qemuReapedThroughHeldHandle": owner.qemuReaped, "qemuExit": owner.qemuExit,
		"nativeExit": guest.command.ProcessState.ExitCode(), "cancellationRequested": requestCancel,
		"nativeEventClosed": owner.event == 0, "normalLauncherGuestBootVerified": false,
		"officialInputModified": false, "dailyUseAccepted": false}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	file, err := os.OpenFile(output, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	_, writeErr := file.Write(append(data, '\n'))
	closeErr := file.Close()
	if writeErr != nil || closeErr != nil || !owner.qemuReaped {
		t.Fatal("failed private process receipt/exit proof")
	}
	code := guest.command.ProcessState.ExitCode()
	if !requestCancel && waitErr != nil {
		t.Fatal(waitErr)
	}
	if requestCancel && (code != 1 || owner.qemuExit != 90 || waitErr == nil) {
		t.Fatal("native cancellation did not reap the owned VM")
	}
	// Exit mirrors the native owner for the unchanged harness.
	os.Exit(code)
}
