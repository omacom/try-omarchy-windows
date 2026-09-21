//go:build windows

package main

import (
	"os"
	"os/exec"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// Run in an interactive Windows session with TRYOMARCHY_UI_TEST=1. Reopening
// checks class teardown as well as real button routing and cancellation.
func TestChoiceDialogNative(t *testing.T) {
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop and TRYOMARCHY_UI_TEST=1")
	}
	for _, want := range []int{2, 1, 0, 3} {
		type result struct {
			action int
			err    error
		}
		finished := make(chan result, 1)
		go func() {
			action, err := chooseAction("Try Omarchy choice test", "This test changes no installation files.", "First action", "Second action", "Close")
			finished <- result{action, err}
		}()
		class, _ := syscall.UTF16PtrFromString("TryOmarchyChoice")
		title, _ := syscall.UTF16PtrFromString("Try Omarchy choice test")
		var window, button uintptr
		deadline := time.Now().Add(5 * time.Second)
		for time.Now().Before(deadline) {
			window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)))
			if window != 0 {
				button, _, _ = user32.NewProc("GetDlgItem").Call(window, uintptr(3000+max(want, 1)))
				visible, _, _ := user32.NewProc("IsWindowVisible").Call(window)
				if button != 0 && visible != 0 {
					break
				}
			}
			select {
			case result := <-finished:
				t.Fatalf("dialog exited before selection: %+v", result)
			default:
			}
			time.Sleep(20 * time.Millisecond)
		}
		if button == 0 {
			if window != 0 {
				procPostMessageW.Call(window, wmClose, 0, 0)
			}
			t.Fatal("choice window did not create its buttons")
		}
		if want == 0 {
			procPostMessageW.Call(button, wmKeydown, vkEscape, 0)
		} else {
			// BM_CLICK routes through the native BUTTON window procedure.
			procPostMessageW.Call(button, 0x00F5, 0, 0)
		}
		select {
		case result := <-finished:
			if result.err != nil || result.action != want {
				t.Fatalf("got %+v, want action %d", result, want)
			}
		case <-time.After(5 * time.Second):
			procPostMessageW.Call(window, wmClose, 0, 0)
			t.Fatal("choice dialog did not finish")
		}
	}
}

func TestChoiceDialogHiddenStartup(t *testing.T) {
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop and TRYOMARCHY_UI_TEST=1")
	}
	const titleText = "Try Omarchy hidden-startup test"
	if os.Getenv("TRYOMARCHY_CHOICE_CHILD") == "1" {
		action, err := chooseAction(titleText, "A native dialog must remain visible when its process suppresses a console.", "Close")
		if err != nil || action != 0 {
			t.Fatalf("unexpected result: %d, %v", action, err)
		}
		return
	}
	self, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(self, "-test.run=^TestChoiceDialogHiddenStartup$")
	cmd.Env = append(os.Environ(), "TRYOMARCHY_CHOICE_CHILD=1")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	defer func() { _ = cmd.Process.Kill() }()
	title, _ := syscall.UTF16PtrFromString(titleText)
	var window uintptr
	visible := false
	for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); {
		window, _, _ = user32.NewProc("FindWindowW").Call(0, uintptr(unsafe.Pointer(title)))
		if window != 0 {
			shown, _, _ := user32.NewProc("IsWindowVisible").Call(window)
			if shown != 0 {
				visible = true
				break
			}
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !visible {
		t.Fatal("hidden-startup child failed to show its native dialog")
	}
	procPostMessageW.Call(window, wmClose, 0, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("child did not exit after closing its dialog")
	}
}

func TestFirstRunLocationNative(t *testing.T) {
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop and TRYOMARCHY_UI_TEST=1")
	}
	for _, action := range []int{1, 3} {
		dir := t.TempDir()
		type selection struct {
			path    string
			proceed bool
			err     error
		}
		done := make(chan selection, 1)
		go func() {
			path, proceed, err := chooseFirstRunDataDirectory(dir)
			done <- selection{path, proceed, err}
		}()
		title, _ := syscall.UTF16PtrFromString("Choose where to keep Omarchy")
		var window, button uintptr
		for deadline := time.Now().Add(5 * time.Second); time.Now().Before(deadline); {
			window, _, _ = user32.NewProc("FindWindowW").Call(0, uintptr(unsafe.Pointer(title)))
			if window != 0 {
				button, _, _ = user32.NewProc("GetDlgItem").Call(window, uintptr(3000+action))
				if button != 0 {
					break
				}
			}
			time.Sleep(20 * time.Millisecond)
		}
		if button == 0 {
			t.Fatal("location dialog did not appear")
		}
		procPostMessageW.Call(button, 0x00F5, 0, 0)
		select {
		case got := <-done:
			if got.err != nil || got.proceed != (action == 1) || got.proceed && got.path != dir {
				t.Fatalf("action %d: unexpected selection %+v", action, got)
			}
		case <-time.After(5 * time.Second):
			procPostMessageW.Call(window, wmClose, 0, 0)
			t.Fatal("location selection did not finish")
		}
	}
}
