//go:build windows

package main

import (
	"context"
	"fmt"
	"runtime"
	"syscall"
	"time"
	"unsafe"
)

type usbUIResult struct {
	devices []usbDevice
	err     error
}
type usbUIState struct {
	window, list, status uintptr
	brand                *windowBrand
	buttons              []uintptr
	devices              []usbDevice
	busy, closing, done  bool
	cancel               context.CancelFunc
	results              chan usbUIResult
	selectionMode        bool
	dir, qemu            string
	preference           usbPreferences
}

var usbUI *usbUIState
var usbUIRegistered bool
var usbUICallback = syscall.NewCallback(usbWindowProc)

var usbSelectionInventory = inventoryUSBDevices

const usbResultMessage = 0x8031

func usbSetText(handle uintptr, text string) {
	p, _ := syscall.UTF16PtrFromString(text)
	procSetWindowTextW.Call(handle, uintptr(unsafe.Pointer(p)))
}
func (s *usbUIState) start(action string) {
	if s.busy {
		return
	}
	var selected usbDevice
	if action != "refresh" && !(s.selectionMode && action == "detach") {
		index, _, _ := procSendMessageW.Call(s.list, 0x188, 0, 0)
		if index >= uintptr(len(s.devices)) {
			usbSetText(s.status, "Select a USB device first.")
			return
		}
		selected = s.devices[index]
		if action == "attach" && selected.Claimed {
			usbSetText(s.status, "This device is already attached.")
			return
		}
		if action == "detach" && !selected.Claimed {
			usbSetText(s.status, "This device is already available to Windows.")
			return
		}
		if action == "attach" && !s.selectionMode && msgBox("Attach "+selected.Name+" to Omarchy?\n\nWindows applications will lose access until you release it. Eject mounted storage before switching it.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
			return
		}
	}
	if s.selectionMode && action != "refresh" {
		updated := s.preference
		if action == "attach" {
			if msgBox("Attach "+selected.Name+" whenever Omarchy starts?\n\nWindows applications will lose access until Omarchy stops or you release the device. Eject mounted storage first. This choice stays with this USB port.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
				return
			}
			updated.Device = selectionForUSB(selected)
			updated.Enabled = true
		} else {
			updated.Enabled = false
		}
		guard, err := lockMoveStore(hostMoveStore())
		if err == nil {
			err = checkMovedSettings(s.dir)
			if err == nil {
				err = saveUSBPreferences(s.dir, updated)
			}
			guard.Close()
		}
		if err != nil {
			usbSetText(s.status, "Could not save USB choice: "+err.Error())
			return
		}
		s.preference = updated
		s.showDevices(s.devices)
		message := "Saved. The chosen device applies next time Omarchy starts."
		if !updated.Enabled {
			message = "Saved. Omarchy will not attach a USB device on its next start."
		}
		usbSetText(s.status, message)
		return
	}
	s.busy = true
	for _, button := range s.buttons {
		procEnableWindow.Call(button, 0)
	}
	usbSetText(s.status, "Reading USB devices...")
	if action == "attach" {
		usbSetText(s.status, "Attaching device...")
	} else if action == "detach" {
		usbSetText(s.status, "Releasing device to Windows...")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	s.cancel = cancel
	go func() {
		defer cancel()
		var result usbUIResult
		if s.selectionMode {
			result.devices, result.err = usbSelectionInventory(ctx, s.qemu)
			if result.err == nil {
				result.devices = usbSelectionChoices(result.devices, s.preference.Device)
			}
			s.results <- result
			procPostMessageW.Call(s.window, usbResultMessage, 0, 0)
			return
		}
		client, err := dialQMPControl(ctx, qmpToolsPort)
		if err == nil {
			defer client.Close()
			broker := usbBroker{qmp: client}
			switch action {
			case "attach":
				err = broker.Attach(ctx, selected)
			case "detach":
				err = broker.Detach(ctx, selected.ID)
			}
			if err == nil {
				result.devices, err = broker.Devices(ctx)
			}
		}
		result.err = err
		s.results <- result
		procPostMessageW.Call(s.window, usbResultMessage, 0, 0)
	}()
}
func (s *usbUIState) showDevices(devices []usbDevice) {
	s.devices = devices
	procSendMessageW.Call(s.list, 0x184, 0, 0)
	for _, device := range s.devices {
		state := "Available to Windows"
		if s.selectionMode && !device.Connected {
			state = "Not connected"
		}
		if s.selectionMode && s.preference.Enabled && s.preference.Device != nil && s.preference.Device.matches(device) {
			state += "; selected for next start"
		}
		if device.Claimed {
			state = "Attached to Omarchy"
			if !device.Connected {
				state = "Unplugged; release to clear"
			}
		}
		label := fmt.Sprintf("%s   [%s]   USB %d/%s", device.Name, state, device.Bus, device.Port)
		p, _ := syscall.UTF16PtrFromString(label)
		procSendMessageW.Call(s.list, 0x180, 0, uintptr(unsafe.Pointer(p)))
	}
	if len(s.devices) > 0 {
		index := 0
		if s.selectionMode && s.preference.Device != nil {
			for i, d := range s.devices {
				if s.preference.Device.matches(d) {
					index = i
					break
				}
			}
		}
		procSendMessageW.Call(s.list, 0x186, uintptr(index), 0)
	}
	usbSetText(s.status, fmt.Sprintf("%d devices. Refresh after connecting or unplugging a device.", len(s.devices)))
}
func usbWindowProc(hwnd, message, w, l uintptr) uintptr {
	s := usbUI
	if s == nil {
		r, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
		return r
	}
	if result, handled := s.brand.handle(hwnd, message, w, l); handled {
		return result
	}
	switch message {
	case usbResultMessage:
		result := <-s.results
		s.busy = false
		if s.closing {
			procDestroyWindow.Call(hwnd)
			return 0
		}
		for _, button := range s.buttons {
			procEnableWindow.Call(button, 1)
		}
		if result.err != nil {
			usbSetText(s.status, result.err.Error())
			return 0
		}
		s.showDevices(result.devices)
		return 0
	case wmCommand:
		switch w & 0xffff {
		case 4301:
			s.start("refresh")
		case 4302:
			s.start("attach")
		case 4303:
			s.start("detach")
		case 2:
			procPostMessageW.Call(hwnd, wmClose, 0, 0)
		}
		return 0
	case wmClose:
		if s.busy {
			s.closing = true
			s.cancel()
			usbSetText(s.status, "Finishing device operation...")
		} else {
			procDestroyWindow.Call(hwnd)
		}
		return 0
	case wmDestroy:
		s.done = true
		procPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, message, w, l)
	return r
}
func runUSBDeviceUI() error                    { return runUSBWindow("", "", false) }
func runUSBSelectionUI(dir, qemu string) error { return runUSBWindow(dir, qemu, true) }
func runUSBWindow(dir, qemu string, selection bool) error {
	runtime.LockOSThread()
	// Retire this UI thread on return, including its posted WM_QUIT.
	s := &usbUIState{results: make(chan usbUIResult, 1), brand: newWindowBrand(), dir: dir, qemu: qemu, selectionMode: selection}
	if selection {
		var err error
		err = checkMovedSettings(dir)
		if err == nil {
			s.preference, err = loadUSBPreferences(dir)
		}
		if err != nil {
			s.brand.close()
			return err
		}
	}
	defer s.brand.close()
	usbUI = s
	defer func() { usbUI = nil }()
	instance, _, _ := procGetModuleHandleW.Call(0)
	class, _ := syscall.UTF16PtrFromString("TryOmarchyUSBDevices")
	if !usbUIRegistered {
		type windowClass struct {
			size, style                   uint32
			callback                      uintptr
			classExtra, windowExtra       int32
			instance, icon, cursor, brush uintptr
			menu, class                   *uint16
			smallIcon                     uintptr
		}
		cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
		wc := windowClass{size: uint32(unsafe.Sizeof(windowClass{})), callback: usbUICallback, instance: instance, cursor: cursor, brush: colorBtnface + 1, class: class}
		if result, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); result == 0 {
			return err
		}
		usbUIRegistered = true
	}
	title, _ := syscall.UTF16PtrFromString("USB devices")
	style := uintptr(wsCaption | wsSysmenu | 0x02000000)
	frame := [4]int32{}
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&frame)), style, 0, 0)
	work := [4]int32{}
	procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work)), 0)
	width := int(min(int32(660), work[2]-work[0]-32-(frame[2]-frame[0])))
	height := int(min(int32(350), work[3]-work[1]-32-(frame[3]-frame[1])))
	rect := [4]int32{0, 0, int32(width), int32(height)}
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	var err error
	s.window, _, err = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(title)), style|wsVisible, uintptr(work[0]+16), uintptr(work[1]+16), uintptr(rect[2]-rect[0]), uintptr(rect[3]-rect[1]), 0, 0, instance, 0)
	if s.window == 0 {
		return err
	}
	s.brand.window(s.window)
	var controlErr error
	control := func(class, label string, x, y, width, height int, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		p, _ := syscall.UTF16PtrFromString(label)
		h, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(p)), wsVisible|wsChild|style, uintptr(x), uintptr(y), uintptr(width), uintptr(height), s.window, id, instance, 0)
		if h == 0 {
			controlErr = err
		}
		s.brand.control(h, class, style)
		return h
	}
	body := "Attach a device to Omarchy, then release it when you want to use it in Windows."
	if selection {
		body = "Experimental. Choose one device for the next start. Missing or busy devices are skipped. No drivers are installed."
	}
	control("STATIC", body, 16, 16, width-32, 40, ssNoprefix, 0)
	s.list = control("LISTBOX", "", 16, 64, width-32, height-168, wsTabstop|wsBorder|wsVscroll|1, 4300)
	s.status = control("STATIC", "", 16, height-94, width-32, 48, ssNoprefix, 0)
	for _, button := range []struct {
		text string
		x    int
		id   uintptr
	}{{"Refresh", 16, 4301}, {"Attach", 128, 4302}, {"Release", 240, 4303}} {
		if selection && button.id == 4302 {
			button.text = "Save choice"
		}
		if selection && button.id == 4303 {
			button.text = "Don't attach"
		}
		s.buttons = append(s.buttons, control("BUTTON", button.text, button.x, height-44, 100, 28, wsTabstop, button.id))
	}
	s.brand.primary, _, _ = user32.NewProc("GetDlgItem").Call(s.window, 4302)
	control("BUTTON", "Close", width-116, height-44, 100, 28, wsTabstop, 2)
	if controlErr != nil {
		procDestroyWindow.Call(s.window)
		return controlErr
	}
	procSetFocus.Call(s.list)
	s.start("refresh")
	var message msgStruct
	for !s.done {
		result, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&message)), 0, 0, 0)
		if result == 0 || int32(result) == -1 {
			break
		}
		if message.message == wmKeydown && message.wParam == 13 {
			focus, _, _ := procGetFocus.Call()
			id, _, _ := user32.NewProc("GetDlgCtrlID").Call(focus)
			if id == 2 || id >= 4301 && id <= 4303 {
				procSendMessageW.Call(s.window, wmCommand, id, 0)
				continue
			}
		}
		if handled, _, _ := procIsDialogMessageW.Call(s.window, uintptr(unsafe.Pointer(&message))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&message)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&message)))
	}
	return nil
}
