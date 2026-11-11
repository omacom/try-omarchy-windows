//go:build windows

package main

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// The first-run splash: a borderless dark panel in the Omarchy look - the app
// icon, the wordmark, a live status line and a slim green progress bar. The
// download goroutine writes atomics; a WM_TIMER repaints from them. Esc or
// closing cancels the setup (nothing else is running at that point). Drag
// the empty background moves it. Native buttons support keyboard navigation.

var (
	comctl32                 = syscall.NewLazyDLL("comctl32.dll")
	procInitCommonControlsEx = comctl32.NewProc("InitCommonControlsEx")
	procCreateWindowExW      = user32.NewProc("CreateWindowExW")
	procDefWindowProcW       = user32.NewProc("DefWindowProcW")
	procRegisterClassExW     = user32.NewProc("RegisterClassExW")
	procShowWindow           = user32.NewProc("ShowWindow")
	procGetMessageW          = user32.NewProc("GetMessageW")
	procTranslateMessage     = user32.NewProc("TranslateMessage")
	procDispatchMessageW     = user32.NewProc("DispatchMessageW")
	procSetTimer             = user32.NewProc("SetTimer")
	procSendMessageW         = user32.NewProc("SendMessageW")
	procSetWindowPos         = user32.NewProc("SetWindowPos")
	procPostQuitMessage      = user32.NewProc("PostQuitMessage")
	procDestroyWindow        = user32.NewProc("DestroyWindow")
	procGetSystemMetrics     = user32.NewProc("GetSystemMetrics")
	procSetForegroundWindow  = user32.NewProc("SetForegroundWindow")
	procBeginPaint           = user32.NewProc("BeginPaint")
	procEndPaint             = user32.NewProc("EndPaint")
	procFillRect             = user32.NewProc("FillRect")
	procLoadIconW            = user32.NewProc("LoadIconW")
	procLoadImageW           = user32.NewProc("LoadImageW")
	procDrawIconEx           = user32.NewProc("DrawIconEx")
	procDestroyIcon          = user32.NewProc("DestroyIcon")
	procCreateFontW          = syscall.NewLazyDLL("gdi32.dll").NewProc("CreateFontW")
	procCreateSolidBrush     = syscall.NewLazyDLL("gdi32.dll").NewProc("CreateSolidBrush")
	procSetTextColor         = syscall.NewLazyDLL("gdi32.dll").NewProc("SetTextColor")
	procSetBkMode            = syscall.NewLazyDLL("gdi32.dll").NewProc("SetBkMode")
	procInvalidateRect       = user32.NewProc("InvalidateRect")
	procDwmSetWindowAttr     = syscall.NewLazyDLL("dwmapi.dll").NewProc("DwmSetWindowAttribute")
	procGetModuleHandleW     = kernel32.NewProc("GetModuleHandleW")
)

const (
	wsPopup           = 0x80000000
	wsVisible         = 0x10000000
	wsChild           = 0x40000000
	wmDestroy         = 0x0002
	wmClose           = 0x0010
	wmCommand         = 0x0111
	wmKeydownMsg      = 0x0100
	wmTimer           = 0x0113
	wmSetfont         = 0x0030
	wmSettext         = 0x000C
	wmSeticon         = 0x0080
	wmPaint           = 0x000F
	wmNchittest       = 0x0084
	wmCtlcolorstatic  = 0x0138
	ssNoprefix        = 0x80
	ssNotify          = 0x100
	htClient          = 1
	htCaption         = 2
	vkEscape          = 0x1B
	swShow            = 5
	hwndTopmost       = ^uintptr(0)
	hwndNotTopmost    = ^uintptr(1)
	swpNoSize         = 0x0001
	swpNoMove         = 0x0002
	swpShowWindow     = 0x0040
	smCxscreen        = 0
	smCyscreen        = 1
	iccProgress       = 0x20
	transparentBkMode = 1
	cancelControlID   = 1001
	promptOption1ID   = 1002
	promptOption2ID   = 1003
	promptContinueID  = 1004
	imageIcon         = 1
	diNormal          = 0x0003

	// The Omarchy look (Tokyo Night-ish, matching the boot splash). COLORREF
	// is 0x00BBGGRR.
	colBg    = 0x00261B1A // RGB(26,27,38)
	colBgBar = 0x003C2A28 // RGB(40,42,60)
	colGreen = 0x006ACE9E // RGB(158,206,106)
	colText  = 0x00F5CAC0 // RGB(192,202,245)
	colDim   = 0x00A27A73 // RGB(115,122,162)
)

type setupPromptKind uint8

const (
	setupPromptNone setupPromptKind = iota
	setupPromptProvision
	setupPromptSharedFolder
	setupPromptShortcuts
)

type setupPromptRequest struct {
	kind  setupPromptKind
	reply chan setupPromptResult
}

type setupPromptResult struct {
	primary   bool
	secondary bool
}

type progressUI struct {
	status        atomic.Value // string
	cancelMessage atomic.Value // string
	cancelStatus  atomic.Value // string
	account       atomic.Value // string
	cur           atomic.Int64
	total         atomic.Int64
	done          atomic.Bool
	canceling     atomic.Bool
	finishOnly    atomic.Bool
	available     atomic.Bool
	ready         chan struct{}
	prompts       chan setupPromptRequest
}

// THE app has ONE splash (launch-UX requirement: it appears at launch and
// stays visible until the Omarchy window itself is on screen - setup must
// never look like nothing is happening). The singleton is also what makes the
// Win32 side correct: the window class registers once with one wndproc; with
// per-phase windows, every window after the first ran the FIRST window's
// wndproc closure, saw its done flag already set, and destroyed itself
// invisibly - exactly the "splash vanished, nothing on screen" failure.
var (
	uiOnce      sync.Once
	uiSingleton *progressUI
)

func getUI() *progressUI {
	uiOnce.Do(func() { uiSingleton = newProgressUI() })
	return uiSingleton
}

// uiDone closes the splash if one exists; safe to call repeatedly and from
// any goroutine (the title enforcer fires it when the VM window appears).
func uiDone() {
	if uiSingleton != nil {
		uiSingleton.finish()
	}
}

// uiStatus updates the splash's status line if there is one.
func uiStatus(format string, a ...any) {
	if uiSingleton != nil {
		uiSingleton.setStatus(format, a...)
	}
}

func newProgressUI() *progressUI {
	ui := &progressUI{ready: make(chan struct{}), prompts: make(chan setupPromptRequest)}
	ui.status.Store(uiText("status.preparing"))
	ui.account.Store("")
	go ui.run()
	<-ui.ready
	return ui
}

func (ui *progressUI) setStatus(format string, a ...any) { ui.status.Store(fmt.Sprintf(format, a...)) }
func (ui *progressUI) setProgress(cur, total int64)      { ui.cur.Store(cur); ui.total.Store(total) }
func (ui *progressUI) finish()                           { ui.done.Store(true) }
func (ui *progressUI) setInstantMode(instant bool)       { ui.account.Store(provisionAccountHint(instant)) }

func (ui *progressUI) chooseInstantMode() bool {
	if !ui.available.Load() {
		return false
	}
	// The first option is the user's own account, so quick start is the second.
	instant := !ui.ask(setupPromptProvision).primary
	ui.setInstantMode(instant)
	return instant
}

func (ui *progressUI) chooseShortcuts() (bool, bool) {
	if !ui.available.Load() {
		return false, false
	}
	result := ui.ask(setupPromptShortcuts)
	return result.primary, result.secondary
}

func (ui *progressUI) chooseSharedFolder() bool {
	if !ui.available.Load() {
		return false
	}
	return ui.ask(setupPromptSharedFolder).primary
}

func setupPromptName(kind setupPromptKind) string {
	switch kind {
	case setupPromptProvision:
		return "account"
	case setupPromptSharedFolder:
		return "shared folder"
	case setupPromptShortcuts:
		return "shortcut"
	}
	return "setup"
}

// ask shows a setup question and waits for the answer. Setup stands still
// until someone answers, so the log says how long that took.
func (ui *progressUI) ask(kind setupPromptKind) setupPromptResult {
	phase := startSetupPhase(setupPromptName(kind) + " question")
	reply := make(chan setupPromptResult, 1)
	ui.prompts <- setupPromptRequest{kind: kind, reply: reply}
	result := <-reply
	phase.end(nil)
	return result
}

func (ui *progressUI) confirmCancel(hCancel uintptr) bool {
	if ui.finishOnly.Load() {
		infoBox(uiText("status.finishing"))
		return false
	}
	if ui.canceling.Load() {
		return true
	}
	message := uiText("setup.cancel.confirm")
	if custom, ok := ui.cancelMessage.Load().(string); ok {
		message = custom
	}
	if msgBox(message, mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return false
	}
	if !ui.canceling.CompareAndSwap(false, true) {
		return true
	}
	requestSetupCancel()
	status := uiText("status.cancelling")
	if custom, ok := ui.cancelStatus.Load().(string); ok {
		status = custom
	}
	ui.setStatus("%s", status)
	ui.setProgress(0, 0)
	procEnableWindow.Call(hCancel, 0)
	t, _ := syscall.UTF16PtrFromString(uiText("setup.cancelling_button"))
	procSendMessageW.Call(hCancel, wmSettext, 0, uintptr(unsafe.Pointer(t)))
	return true
}

// Leave a gap between the final option and the always-visible actions.
func setupPromptPositions(height int32) (title, body, first, second int32) {
	second = min(int32(294), height-88)
	first = min(int32(254), second-40)
	title = min(int32(168), first-86)
	return title, title + 32, first, second
}

func (ui *progressUI) run() {
	runtime.LockOSThread()
	type iccex struct{ size, icc uint32 }
	ic := iccex{8, iccProgress}
	procInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&ic)))
	hInst, _, _ := procGetModuleHandleW.Call(0)
	brandIcon, _, _ := procLoadImageW.Call(hInst, 1, imageIcon, 96, 96, 0)
	if brandIcon != 0 {
		defer procDestroyIcon.Call(brandIcon)
	}

	brand := newWindowBrand()
	defer brand.close()
	var progressStep int32

	className, _ := syscall.UTF16PtrFromString("TryOmarchySetup")
	var hHead, hTag, hText, hAccountInfo, hSuperInfo uintptr
	var hKeySpace, hKeyK, hKeyReturn, hKeyW uintptr
	var hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose, hCancel uintptr
	var hPromptTitle, hPromptBody, hPromptOption1, hPromptOption2, hPromptContinue uintptr
	lastStatus := ""
	lastAccount := ""
	promptKind := setupPromptNone
	var promptReply chan setupPromptResult
	primary, secondary := true, false
	compact := false
	short := false
	iconY, iconSize := int32(30), int32(64)

	setText := func(handle uintptr, value string) {
		t, _ := syscall.UTF16PtrFromString(value)
		procSendMessageW.Call(handle, wmSettext, 0, uintptr(unsafe.Pointer(t)))
	}
	show := func(handle uintptr, visible bool) {
		cmd := uintptr(0)
		if visible {
			cmd = swShow
		}
		procShowWindow.Call(handle, cmd)
	}
	setOptionText := func() {

		switch promptKind {
		case setupPromptProvision:
			setText(hPromptOption1, uiText("setup.account.personal"))
			setText(hPromptOption2, uiText("setup.account.quick"))
		case setupPromptSharedFolder:
			setText(hPromptOption1, uiText("setup.share.yes"))
			setText(hPromptOption2, uiText("setup.share.no"))
		case setupPromptShortcuts:
			setText(hPromptOption1, uiText("setup.shortcut.start"))
			setText(hPromptOption2, uiText("setup.shortcut.desktop"))
		}
		style := uintptr(bsAutoradiobutton)
		if promptKind == setupPromptShortcuts {
			style = bsAutocheckbox
		}
		for i, handle := range []uintptr{hPromptOption1, hPromptOption2} {
			old, _, _ := user32.NewProc("GetWindowLongW").Call(handle, ^uintptr(15))
			user32.NewProc("SetWindowLongW").Call(handle, ^uintptr(15), old&^uintptr(0xf)|style)
			selected := primary
			if i == 1 {
				selected = !primary
				if promptKind == setupPromptShortcuts {
					selected = secondary
				}
			}
			check := uintptr(0)
			if selected {
				check = bstChecked
			}
			procSendMessageW.Call(handle, bmSetcheck, check, 0)
		}

	}
	setPromptVisible := func(visible bool) {
		for _, h := range []uintptr{hText, hSuperInfo, hKeySpace, hKeyK, hKeyReturn, hKeyW,
			hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose} {
			show(h, !visible)
		}
		if compact {
			for _, h := range []uintptr{hKeySpace, hKeyK, hKeyReturn, hKeyW, hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose} {
				show(h, false)
			}
		}
		show(hAccountInfo, !visible && !short && ui.account.Load().(string) != "")
		for _, h := range []uintptr{hPromptTitle, hPromptBody, hPromptOption1, hPromptOption2, hPromptContinue} {
			show(h, visible)
		}
	}
	var finishPrompt func(hwnd uintptr)
	// Cancelling never picks the quick-start account or creates a shared
	// folder: the first option means the user's own account on the account
	// prompt, and "not now" is the second option on the shared folder prompt.
	cancelPrompt := func(hwnd uintptr) {
		primary, secondary = promptKind == setupPromptProvision, false
		finishPrompt(hwnd)
	}
	finishPrompt = func(hwnd uintptr) {
		if promptKind == setupPromptNone || promptReply == nil {
			return
		}
		if promptKind == setupPromptProvision {
			ui.setInstantMode(!primary)
		}
		promptReply <- setupPromptResult{primary: primary, secondary: secondary}
		promptReply = nil
		promptKind = setupPromptNone
		setPromptVisible(false)
		procSetWindowPos.Call(hwnd, hwndNotTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
		procSetFocus.Call(hCancel)
	}

	const (
		iconX      = 32
		windowW    = 560
		windowH    = 420
		sideMargin = 32
	)
	barRect := [4]int32{sideMargin, 204, windowW - sideMargin, 210}

	wndProc := syscall.NewCallback(func(hwnd, msg, wParam, lParam uintptr) uintptr {
		if result, handled := brand.handle(hwnd, msg, wParam, lParam); handled {
			return result
		}
		switch msg {
		case wmTimer:
			progressStep += 12
			if ui.done.Load() {
				procDestroyWindow.Call(hwnd)
				return 0
			}
			if s := ui.status.Load().(string); s != lastStatus {
				lastStatus = s
				t, _ := syscall.UTF16PtrFromString(s)
				procSendMessageW.Call(hText, wmSettext, 0, uintptr(unsafe.Pointer(t)))
			}
			if account := ui.account.Load().(string); account != lastAccount {
				lastAccount = account
				setText(hAccountInfo, account)
				show(hAccountInfo, !short && promptKind == setupPromptNone && account != "")
			}
			select {
			case request := <-ui.prompts:
				promptKind = request.kind
				promptReply = request.reply
				primary, secondary = true, false
				switch promptKind {
				case setupPromptProvision:
					setText(hPromptTitle, uiText("setup.account.title"))
					setText(hPromptBody, uiText("setup.account.body"))
				case setupPromptSharedFolder:
					setText(hPromptTitle, uiText("setup.share.title"))
					setText(hPromptBody, uiText("setup.share.body"))
				case setupPromptShortcuts:
					setText(hPromptTitle, uiText("setup.shortcut.title"))
					setText(hPromptBody, uiText("setup.shortcut.body"))
				}
				setOptionText()
				setPromptVisible(true)
				procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
				procSetForegroundWindow.Call(hwnd)
				procSetFocus.Call(hPromptOption1)
				procInvalidateRect.Call(hwnd, 0, 1)
			default:
			}
			procInvalidateRect.Call(hwnd, uintptr(unsafe.Pointer(&barRect)), 0)
			return 0
		case wmPaint:
			var ps [16]uintptr // PAINTSTRUCT is 72 bytes on x64; overshoot is fine
			hdc, _, _ := procBeginPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
			if brandIcon != 0 {
				procDrawIconEx.Call(hdc, iconX, uintptr(iconY), brandIcon, uintptr(iconSize), uintptr(iconSize), 0, 0, diNormal)
			}
			// Self-drawn slim progress bar: no classic-theme border, our colors.
			if promptKind == setupPromptNone {
				procFillRect.Call(hdc, uintptr(unsafe.Pointer(&barRect)), brand.brushes[1])
				if total := ui.total.Load(); total > 0 {
					fill := barRect
					fill[2] = fill[0] + int32(int64(fill[2]-fill[0])*max(int64(0), min(total, ui.cur.Load()))/total)
					procFillRect.Call(hdc, uintptr(unsafe.Pointer(&fill)), brand.brushes[2])
				} else {
					fill := barRect
					start := barRect[0] + progressStep%(barRect[2]-barRect[0]+80) - 80
					fill[0], fill[2] = max(barRect[0], start), min(barRect[2], start+80)
					procFillRect.Call(hdc, uintptr(unsafe.Pointer(&fill)), brand.brushes[2])
				}
			}
			procEndPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
			return 0
		case wmNchittest:
			// Borderless: dragging anywhere moves the window.
			r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
			if r == htClient {
				return htCaption
			}
			return r
		case wmKeydownMsg:
			if promptKind != setupPromptNone && wParam == 0x0D {
				finishPrompt(hwnd)
				procInvalidateRect.Call(hwnd, 0, 1)
				return 0
			}
			if wParam == vkEscape {
				if ui.confirmCancel(hCancel) && promptKind != setupPromptNone {
					cancelPrompt(hwnd)
				}
			}
			return 0
		case wmCommand:
			switch wParam & 0xffff {
			case cancelControlID, idCancel:
				if ui.confirmCancel(hCancel) && promptKind != setupPromptNone {
					cancelPrompt(hwnd)
				}
			case promptOption1ID:
				if promptKind == setupPromptProvision || promptKind == setupPromptSharedFolder {
					primary = true
				} else {
					primary = !primary
				}
				setOptionText()
			case promptOption2ID:
				if promptKind == setupPromptProvision || promptKind == setupPromptSharedFolder {
					primary = false
				} else {
					secondary = !secondary
				}
				setOptionText()
			case promptContinueID:
				finishPrompt(hwnd)
				procInvalidateRect.Call(hwnd, 0, 1)
			}
			return 0
		case wmClose:
			if ui.confirmCancel(hCancel) && promptKind != setupPromptNone {
				cancelPrompt(hwnd)
			}
			return 0
		case wmDestroy:
			procPostQuitMessage.Call(0)
			return 0
		}
		r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
		return r
	})

	// Field types must mirror WNDCLASSEXW exactly: cbClsExtra/cbWndExtra are C
	// ints (4 bytes), NOT pointer-sized. Getting this wrong inflates cbSize and
	// RegisterClassExW rejects the struct - silently, if nobody checks (it
	// shipped that way once: no progress window, no error, download running
	// blind. Check every return value here).
	type wndclassex struct {
		size, style         uint32
		wndProc             uintptr
		clsExtra, wndExtra  int32
		inst                uintptr
		icon, cursor, brush uintptr
		menuName, className *uint16
		iconSm              uintptr
	}
	wc := wndclassex{
		size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst,
		brush: brand.brushes[0], className: className,
	}
	const errClassAlreadyExists = 1410 // second UI in one run (download, then disk prep)
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		if errno, ok := err.(syscall.Errno); !ok || errno != errClassAlreadyExists {
			logf("progress UI: RegisterClassExW failed: %v", err)
			close(ui.ready)
			return
		}
	}

	work := [4]int32{}
	procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work)), 0)
	height := min(int32(windowH), work[3]-work[1]-16)
	width := min(int32(windowW), work[2]-work[0]-16)
	compact = height < windowH || width < windowW
	short = height < 320
	if short {
		iconY, iconSize = 12, 48
	}
	barRect[2] = width - sideMargin
	title, _ := syscall.UTF16PtrFromString(appTitle)
	hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		wsPopup|wsVisible|0x02000000, uintptr(work[0]+(work[2]-work[0]-width)/2), uintptr(work[1]+(work[3]-work[1]-height)/2), uintptr(width), uintptr(height), 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("progress UI: CreateWindowExW failed: %v", err)
		close(ui.ready)
		return
	}
	// Win11 rounded corners on the borderless panel; harmless no-op elsewhere.
	corner := int32(2) // DWMWCP_ROUND
	procDwmSetWindowAttr.Call(hwnd, 33, uintptr(unsafe.Pointer(&corner)), 4)
	// Taskbar icon (the .ico embedded via rsrc; id 1).
	if icon, _, _ := procLoadIconW.Call(hInst, 1); icon != 0 {
		procSendMessageW.Call(hwnd, wmSeticon, 1, icon)
		procSendMessageW.Call(hwnd, wmSeticon, 0, icon)
	}

	staticClass, _ := syscall.UTF16PtrFromString("STATIC")
	mk := func(text string, x, y, cx, cy int32, extraStyle, id uintptr) uintptr {
		t, _ := syscall.UTF16PtrFromString(text)
		// SS_NOPREFIX: without it a & in the text renders as an underline.
		hw, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(staticClass)), uintptr(unsafe.Pointer(t)),
			wsChild|wsVisible|ssNoprefix|extraStyle, uintptr(x), uintptr(y), uintptr(cx), uintptr(cy), hwnd, id, hInst, 0)
		return hw
	}
	button := func(label string, x, y, width, height int32, style, id uintptr) uintptr {
		class, _ := syscall.UTF16PtrFromString("BUTTON")
		text, _ := syscall.UTF16PtrFromString(label)
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(text)), wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(width), uintptr(height), hwnd, id, hInst, 0)
		brand.control(h, "BUTTON", style)
		return h
	}
	hHead = mk(uiText("brand.name"), 120, 30, width-152, 34, 0, 0)
	hTag = mk("WINDOWS  ·  "+currentVersion, 122, 67, width-154, 22, 0, 0)
	hText = mk(uiText("status.preparing"), 32, 148, width-64, 44, 0, 0)
	// Starter keybindings on screen during the boot wait (the #1 field
	// complaint: an hour lost guessing tiling WM keys once the VM appears and
	// this window closes). Binds verified against Omarchy v4.0.1 defaults.
	hAccountInfo = mk("", 40, 230, width-80, 22, 0, 0)
	hSuperInfo = mk(uiText("setup.keys.super"), 40, 256, width-80, 22, 0, 0)
	// Two key columns; each label column widens for its longer translation and
	// pushes the second column right, keeping the English layout as the minimum.
	menuLabel, terminalLabel := uiText("setup.keys.menu"), uiText("setup.keys.terminal")
	keysLabel, closeLabel := uiText("setup.keys.all"), uiText("setup.keys.close")
	labelWidth := func(minimum int32, labels ...string) int32 {
		for _, label := range labels {
			w, _ := measureText(hwnd, brand.font, label, 0)
			minimum = max(minimum, w+4)
		}
		return minimum
	}
	firstLabelWidth := labelWidth(90, menuLabel, terminalLabel)
	secondKeyX := max(int32(280), 164+firstLabelWidth+16)
	secondLabelX := secondKeyX + 88
	secondLabelWidth := min(labelWidth(112, keysLabel, closeLabel), width-40-secondLabelX)
	hKeySpace = mk("SUPER+SPACE", 40, 286, 118, 20, 0, 0)
	hLabelMenu = mk(menuLabel, 164, 286, firstLabelWidth, 20, 0, 0)
	hKeyK = mk("SUPER+K", secondKeyX, 286, 86, 20, 0, 0)
	hLabelKeys = mk(keysLabel, secondLabelX, 286, secondLabelWidth, 20, 0, 0)
	hKeyReturn = mk("SUPER+RETURN", 40, 310, 118, 20, 0, 0)
	hLabelTerminal = mk(terminalLabel, 164, 310, firstLabelWidth, 20, 0, 0)
	hKeyW = mk("SUPER+W", secondKeyX, 310, 86, 20, 0, 0)
	hLabelClose = mk(closeLabel, secondLabelX, 310, secondLabelWidth, 20, 0, 0)
	cancelWidth := buttonWidthFor(brand.font, 100, uiText("setup.cancel"), uiText("setup.cancelling_button"))
	hCancel = button(uiText("setup.cancel"), 32, height-50, cancelWidth, 36, wsTabstop, cancelControlID)
	hPromptTitle = mk("", 40, 168, width-80, 24, 0, 0)
	hPromptBody = mk("", 32, 200, width-64, 44, 0, 0)
	hPromptOption1 = button("", 32, 254, width-64, 28, bsAutocheckbox|wsGroup|wsTabstop, promptOption1ID)
	hPromptOption2 = button("", 32, 294, width-64, 28, bsAutocheckbox|wsTabstop, promptOption2ID)
	continueWidth := buttonWidthFor(brand.font, 154, uiText("setup.continue"))
	hPromptContinue = button(uiText("setup.continue"), width-32-continueWidth, height-50, continueWidth, 36, bsDefpushbutton|wsTabstop, promptContinueID)
	brand.primary = hPromptContinue
	setPromptVisible(false)

	for _, h := range []uintptr{hHead, hTag, hText, hAccountInfo, hSuperInfo, hKeySpace, hKeyK, hKeyReturn, hKeyW, hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose, hPromptTitle, hPromptBody} {
		procSendMessageW.Call(h, wmSetfont, brand.font, 1)
	}
	procSendMessageW.Call(hHead, wmSetfont, brand.heading, 1)
	for _, h := range []uintptr{hKeySpace, hKeyK, hKeyReturn, hKeyW, hPromptTitle} {
		brand.sectionControls[h] = true
	}
	brand.window(hwnd)

	if compact {
		title, body, first, second := setupPromptPositions(height)
		for i, h := range []uintptr{hPromptTitle, hPromptBody, hPromptOption1, hPromptOption2} {
			positions := []int32{title, body, first, second}
			procSetWindowPos.Call(h, 0, 32, uintptr(positions[i]), 0, 0, swpNoSize|0x0004|0x0010)
		}
		setText(hSuperInfo, uiText("setup.keys.compact"))
		if short {
			procSetWindowPos.Call(hHead, 0, 96, 10, 0, 0, swpNoSize|0x0004|0x0010)
			procSetWindowPos.Call(hTag, 0, 98, 40, 0, 0, swpNoSize|0x0004|0x0010)
			procSetWindowPos.Call(hText, 0, 32, 100, 0, 0, swpNoSize|0x0004|0x0010)
			procSetWindowPos.Call(hSuperInfo, 0, 32, 178, uintptr(width-64), 40, 0x0004|0x0010)
			barRect[1], barRect[3] = 154, 160
		}
		setPromptVisible(false)
	}
	procSetTimer.Call(hwnd, 1, 100, 0)
	procSetWindowPos.Call(hwnd, 0, 0, 0, 0, 0, swpNoMove|swpNoSize|swpShowWindow|0x0004)
	// Launched without foreground rights (shortcut helpers, background shells)
	// the window opens buried; ask for the front anyway - best effort.
	procSetForegroundWindow.Call(hwnd)
	ui.available.Store(true)
	close(ui.ready)

	var m msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			return
		}
		if m.message == wmKeydown && m.wParam == vkEscape {
			procSendMessageW.Call(hwnd, wmKeydownMsg, vkEscape, 0)
			continue
		}
		if m.message == wmKeydown && m.wParam == 13 {
			if m.hwnd == hCancel || m.hwnd == hPromptContinue {
				procSendMessageW.Call(m.hwnd, 0x00f5, 0, 0)
				continue
			}
			if promptKind != setupPromptNone {
				finishPrompt(hwnd)
				procInvalidateRect.Call(hwnd, 0, 1)
				continue
			}
		}
		if handled, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&m))); handled != 0 {
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
