//go:build windows

package main

import (
	"errors"
	"fmt"
	"path/filepath"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

func runCheckpointUI(dir string) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	const listID, nameID, createID, restoreID, deleteID, closeID, rollbackID = 4100, 4101, 4102, 4103, 4104, 4105, 4106
	const operationDone, progressChanged = 0x8020, 0x8021
	store := checkpointStore{installation: dir}
	if err := store.Recover(); err != nil {
		return err
	}
	var hwnd, list, name, status, closeButton uintptr
	var buttons []uintptr
	var entries []vmCheckpoint
	busy := false
	type outcome struct {
		message string
		err     error
	}
	results := make(chan outcome, 1)
	var progress atomic.Value
	progress.Store("")
	setText := func(handle uintptr, text string) {
		value, _ := syscall.UTF16PtrFromString(text)
		procSetWindowTextW.Call(handle, uintptr(unsafe.Pointer(value)))
	}
	refresh := func() {
		var err error
		entries, err = store.List()
		procSendMessageW.Call(list, 0x184, 0, 0) // LB_RESETCONTENT
		if err != nil {
			setText(status, err.Error())
			return
		}
		for _, entry := range entries {
			label := fmt.Sprintf("%s   %s   (%.1f MiB)", entry.Created.Local().Format("2006-01-02 15:04"), entry.Name, float64(entry.ArchiveBytes)/(1<<20))
			if entry.Problem != "" {
				label = entry.Name + "   " + entry.ID
			}
			text, _ := syscall.UTF16PtrFromString(label)
			procSendMessageW.Call(list, 0x180, 0, uintptr(unsafe.Pointer(text))) // LB_ADDSTRING
		}
		if len(entries) > 0 {
			procSendMessageW.Call(list, 0x186, 0, 0)
		}
		setText(status, fmt.Sprintf("%d snapshots. Restore a copy or roll back this installation.", len(entries)))
	}
	selected := func() (vmCheckpoint, bool) {
		index, _, _ := procSendMessageW.Call(list, 0x188, 0, 0)
		if int(index) < 0 || int(index) >= len(entries) {
			return vmCheckpoint{}, false
		}
		return entries[int(index)], true
	}
	start := func(label string, operation func(backupProgress) outcome) {
		busy = true
		configureSetupCancellation(false)
		for _, button := range buttons {
			procEnableWindow.Call(button, 0)
		}
		setText(closeButton, "Cancel operation")
		setText(status, label)
		go func() {
			last := time.Time{}
			report := func(current, total int64, file string) {
				if time.Since(last) < 150*time.Millisecond && current != total {
					return
				}
				last = time.Now()
				progress.Store(fmt.Sprintf("%s %.1f / %.1f MiB", label, float64(current)/(1<<20), float64(total)/(1<<20)))
				procPostMessageW.Call(hwnd, progressChanged, 0, 0)
			}
			results <- operation(report)
			procPostMessageW.Call(hwnd, operationDone, 0, 0)
		}()
	}
	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchySnapshots")
	wndProc := syscall.NewCallback(func(h, message, wParam, lParam uintptr) uintptr {
		switch message {
		case progressChanged:
			if busy {
				setText(status, progress.Load().(string))
			}
			return 0
		case operationDone:
			result := <-results
			busy = false
			for _, button := range buttons {
				procEnableWindow.Call(button, 1)
			}
			setText(closeButton, "Close")
			refresh()
			if errors.Is(result.err, errSetupCancelled) {
				setText(status, "Cancelled. Your current installation and completed snapshots were kept.")
			} else if result.err != nil {
				errorBox(result.err.Error())
			} else if result.message != "" {
				infoBox(result.message)
			}
			return 0
		case wmCommand:
			id := wParam & 0xffff
			if id == closeID || id == 2 { // IDCANCEL is sent by Escape.
				procPostMessageW.Call(h, wmClose, 0, 0)
				return 0
			}
			if busy {
				return 0
			}
			if id == listID {
				if entry, ok := selected(); ok && entry.Problem != "" {
					setText(status, entry.Problem)
				}
				return 0
			}
			switch id {
			case createID:
				buffer := make([]uint16, 161)
				procGetWindowTextW.Call(name, uintptr(unsafe.Pointer(&buffer[0])), uintptr(len(buffer)))
				label := syscall.UTF16ToString(buffer)
				start("Creating snapshot.", func(report backupProgress) outcome { _, err := store.Create(label, report); return outcome{err: err} })
			case restoreID:
				entry, ok := selected()
				if !ok {
					infoBox("Select a snapshot first.")
					return 0
				}
				if entry.Problem != "" {
					errorBox(entry.Problem)
					return 0
				}
				parent, ok, err := chooseRecoveryPath(hwnd, "Choose where to restore the snapshot", "", false, true)
				if err != nil {
					errorBox(err.Error())
					return 0
				}
				if !ok {
					return 0
				}
				destination := filepath.Join(parent, "OmarchySnapshot-"+time.Now().Format("20060102-150405"))
				start("Restoring snapshot.", func(report backupProgress) outcome {
					if err := store.Restore(entry.ID, destination, report); err != nil {
						return outcome{err: err}
					}
					message := "Snapshot restored to:\n\n" + destination
					if err := createRestoredLaunchers(destination); err != nil {
						message += "\n\nStartup shortcuts could not be created: " + err.Error()
					} else {
						message += "\n\nOpen Start Omarchy in that folder to use it."
					}
					return outcome{message: message}
				})
			case rollbackID:
				entry, ok := selected()
				if !ok {
					infoBox("Select a snapshot first.")
					return 0
				}
				if entry.Problem != "" {
					errorBox(entry.Problem)
					return 0
				}
				if msgBox("Roll back to snapshot \""+entry.Name+"\"?\n\nThis replaces the active guest and settings. Your current state will be retained in a recovery folder.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
					return 0
				}
				start("Rolling back snapshot.", func(report backupProgress) outcome {
					retained, err := store.Rollback(entry.ID, report)
					if err != nil {
						return outcome{err: err}
					}
					recoveryFolder := retained
					if disk, err := inspectInstallationDisk(retained); err == nil && disk.Format == "qcow2" {
						recoveryFolder = filepath.Dir(retained)
					}
					message := "Snapshot restored. Open Try Omarchy normally to use it.\n\nYour previous state is retained at:\n\n" + recoveryFolder
					if err := createRollbackRecoveryLaunchers(retained, dir); err != nil {
						message += "\n\nCould not create recovery shortcuts: " + err.Error()
					}
					return outcome{message: message}
				})
			case deleteID:
				entry, ok := selected()
				if !ok {
					infoBox("Select a snapshot first.")
					return 0
				}
				if msgBox("Delete snapshot \""+entry.Name+"\"?\n\nYour current installation will be kept.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
					return 0
				}
				start("Deleting snapshot.", func(backupProgress) outcome { return outcome{err: store.Delete(entry.ID)} })
			}
			return 0
		case wmClose:
			if busy {
				requestSetupCancel()
				setText(status, "Cancelling the operation...")
				return 0
			}
			procDestroyWindow.Call(h)
			return 0
		case wmDestroy:
			procPostQuitMessage.Call(0)
			return 0
		}
		r, _, _ := procDefWindowProcW.Call(h, message, wParam, lParam)
		return r
	})
	type windowClass struct {
		size, style                   uint32
		wndProc                       uintptr
		classExtra, windowExtra       int32
		instance, icon, cursor, brush uintptr
		menu, class                   *uint16
		smallIcon                     uintptr
	}
	cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
	icon, _, _ := procLoadIconW.Call(hInst, 1)
	wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), wndProc: wndProc, instance: hInst, icon: icon, cursor: cursor, brush: colorBtnface + 1, class: className, smallIcon: icon}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		return fmt.Errorf("cannot create Snapshots window: %v", err)
	}
	const width, height = 640, 376
	rect := [4]int32{0, 0, width, height}
	style := uintptr(wsCaption | wsSysmenu)
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	w, h := rect[2]-rect[0], rect[3]-rect[1]
	work := [4]int32{0, 0, 1024, 768}
	procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work[0])), 0)
	x, y := work[0]+(work[2]-work[0]-w)/2, work[1]+(work[3]-work[1]-h)/2
	if y < work[1] {
		y = work[1]
	}
	title, _ := syscall.UTF16PtrFromString("Try Omarchy snapshots")
	var createErr error
	hwnd, _, createErr = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)), style|wsVisible, uintptr(x), uintptr(y), uintptr(w), uintptr(h), 0, 0, hInst, 0)
	if hwnd == 0 {
		return fmt.Errorf("cannot open Snapshots: %v", createErr)
	}
	font, _, _ := procGetStockObject.Call(defaultGuiFont)
	var controlErr error
	control := func(class, label string, x, y, w, h int32, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		value, _ := syscall.UTF16PtrFromString(label)
		handle, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(value)), wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(w), uintptr(h), hwnd, id, hInst, 0)
		if handle == 0 && controlErr == nil {
			controlErr = fmt.Errorf("cannot create snapshot control %d: %v", id, err)
		}
		procSendMessageW.Call(handle, wmSetfont, font, 1)
		return handle
	}
	control("STATIC", "Close Omarchy before taking or restoring snapshots. Rollback retains your current state in a recovery folder.", 16, 12, 608, 34, ssNoprefix, 0)
	list = control("LISTBOX", "", 16, 52, 608, 178, wsBorder|wsVscroll|wsTabstop|1, listID)
	control("STATIC", "Snapshot name", 16, 244, 120, 24, ssNoprefix, 0)
	name = control("EDIT", "Before changes", 140, 240, 330, 26, wsBorder|wsTabstop|esAutohscroll, nameID)
	procSendMessageW.Call(name, 0xC5, 160, 0) // EM_SETLIMITTEXT
	create := control("BUTTON", "Create snapshot", 484, 240, 140, 26, wsTabstop, createID)
	restore := control("BUTTON", "Restore as copy...", 16, 282, 152, 28, wsTabstop, restoreID)
	rollback := control("BUTTON", "Roll back...", 178, 282, 130, 28, wsTabstop, rollbackID)
	remove := control("BUTTON", "Delete...", 318, 282, 130, 28, wsTabstop, deleteID)
	closeButton = control("BUTTON", "Close", 484, 282, 140, 28, wsTabstop, closeID)
	status = control("STATIC", "", 16, 324, 608, 40, ssNoprefix, 0)
	if controlErr != nil {
		procDestroyWindow.Call(hwnd)
		return controlErr
	}
	buttons = []uintptr{list, name, create, restore, rollback, remove}
	refresh()
	// Recovery is launched without a console. Explicitly show the window so
	// inherited SW_HIDE startup flags cannot leave the parent disabled behind
	// an invisible snapshots window.
	procSetWindowPos.Call(hwnd, 0, 0, 0, 0, 0, swpNoMove|swpNoSize|swpShowWindow|0x0004)
	procSetForegroundWindow.Call(hwnd)
	procSetFocus.Call(name)
	var message msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if handled, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&message))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
	return nil
}
