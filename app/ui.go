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

// The first-run splash: a borderless dark panel in the Omarchy look - pixel-art
// O, the wordmark, a live status line and a slim green progress bar. The
// download goroutine writes atomics; a WM_TIMER repaints from them. Esc or
// closing cancels the setup (nothing else is running at that point). Drag
// anywhere moves it.

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
	status    atomic.Value // string
	account   atomic.Value // string
	cur       atomic.Int64
	total     atomic.Int64
	done      atomic.Bool
	canceling atomic.Bool
	available atomic.Bool
	ready     chan struct{}
	prompts   chan setupPromptRequest
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

func newProgressUI() *progressUI {
	ui := &progressUI{ready: make(chan struct{}), prompts: make(chan setupPromptRequest)}
	ui.status.Store("Preparing...")
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
	reply := make(chan setupPromptResult, 1)
	ui.prompts <- setupPromptRequest{kind: setupPromptProvision, reply: reply}
	result := <-reply
	ui.setInstantMode(result.primary)
	return result.primary
}

func (ui *progressUI) chooseShortcuts() (bool, bool) {
	if !ui.available.Load() {
		return false, false
	}
	reply := make(chan setupPromptResult, 1)
	ui.prompts <- setupPromptRequest{kind: setupPromptShortcuts, reply: reply}
	result := <-reply
	return result.primary, result.secondary
}

func (ui *progressUI) confirmCancel(hCancel uintptr) {
	if ui.canceling.Load() {
		return
	}
	if msgBox("Cancel Try Omarchy setup?\n\nUnfinished setup files will be removed. An existing working installation will be kept.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return
	}
	if !ui.canceling.CompareAndSwap(false, true) {
		return
	}
	requestSetupCancel()
	ui.setStatus("Cancelling and cleaning up...")
	ui.setProgress(0, 0)
	t, _ := syscall.UTF16PtrFromString("CANCELLING...")
	procSendMessageW.Call(hCancel, wmSettext, 0, uintptr(unsafe.Pointer(t)))
}

// pixelO returns the cells of the chunky logo-style O: a 10x12 ring, sides 3
// cells thick, top/bottom 2, notched corners (same design as the app icon).
func pixelO() [][2]int32 {
	var cells [][2]int32
	for cx := int32(0); cx < 10; cx++ {
		for cy := int32(0); cy < 12; cy++ {
			onRing := cx < 3 || cx >= 7 || cy < 2 || cy >= 10
			notch := (cx == 0 || cx == 9) && (cy == 0 || cy == 11)
			if onRing && !notch {
				cells = append(cells, [2]int32{cx, cy})
			}
		}
	}
	return cells
}

func (ui *progressUI) run() {
	runtime.LockOSThread()
	type iccex struct{ size, icc uint32 }
	ic := iccex{8, iccProgress}
	procInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&ic)))
	hInst, _, _ := procGetModuleHandleW.Call(0)

	bgBrush, _, _ := procCreateSolidBrush.Call(colBg)
	greenBrush, _, _ := procCreateSolidBrush.Call(colGreen)
	barBgBrush, _, _ := procCreateSolidBrush.Call(colBgBar)

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
		mark := func(selected bool) string {
			if selected {
				return "●"
			}
			return "○"
		}
		if promptKind == setupPromptProvision {
			setText(hPromptOption1, mark(primary)+"  INSTANT TRIAL  (omarchy / omarchy)")
			setText(hPromptOption2, mark(!primary)+"  CHOOSE MY USERNAME AND PASSWORD")
		} else {
			setText(hPromptOption1, mark(primary)+"  START MENU")
			setText(hPromptOption2, mark(secondary)+"  DESKTOP")
		}
	}
	setPromptVisible := func(visible bool) {
		for _, h := range []uintptr{hText, hSuperInfo, hKeySpace, hKeyK, hKeyReturn, hKeyW,
			hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose, hCancel} {
			show(h, !visible)
		}
		show(hAccountInfo, !visible && ui.account.Load().(string) != "")
		for _, h := range []uintptr{hPromptTitle, hPromptBody, hPromptOption1, hPromptOption2, hPromptContinue} {
			show(h, visible)
		}
	}
	finishPrompt := func(hwnd uintptr) {
		if promptKind == setupPromptNone || promptReply == nil {
			return
		}
		if promptKind == setupPromptProvision {
			ui.setInstantMode(primary)
		}
		promptReply <- setupPromptResult{primary: primary, secondary: secondary}
		promptReply = nil
		promptKind = setupPromptNone
		setPromptVisible(false)
		procSetWindowPos.Call(hwnd, hwndNotTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	}

	// Pixel O geometry: cell 8px at (40, 64).
	const (
		oCell      = 8
		oX         = 40
		oY         = 64
		windowW    = 520
		windowH    = 374
		sideMargin = 40
	)
	oCells := pixelO()
	barRect := [4]int32{sideMargin, 204, windowW - sideMargin, 210}

	wndProc := syscall.NewCallback(func(hwnd, msg, wParam, lParam uintptr) uintptr {
		switch msg {
		case wmTimer:
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
				show(hAccountInfo, promptKind == setupPromptNone && account != "")
			}
			select {
			case request := <-ui.prompts:
				promptKind = request.kind
				promptReply = request.reply
				primary, secondary = true, false
				if promptKind == setupPromptProvision {
					setText(hPromptTitle, "CHOOSE YOUR FIRST LAUNCH")
					setText(hPromptBody, "Start instantly, or create your own Linux account.")
				} else {
					setText(hPromptTitle, "KEEP TRY OMARCHY HANDY")
					setText(hPromptBody, "Choose where you want a launcher shortcut.")
				}
				setOptionText()
				setPromptVisible(true)
				procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
				procSetForegroundWindow.Call(hwnd)
				procInvalidateRect.Call(hwnd, 0, 1)
			default:
			}
			procInvalidateRect.Call(hwnd, uintptr(unsafe.Pointer(&barRect)), 0)
			return 0
		case wmPaint:
			var ps [16]uintptr // PAINTSTRUCT is 72 bytes on x64; overshoot is fine
			hdc, _, _ := procBeginPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
			for _, c := range oCells {
				r := [4]int32{oX + c[0]*oCell, oY + c[1]*oCell, oX + (c[0]+1)*oCell, oY + (c[1]+1)*oCell}
				procFillRect.Call(hdc, uintptr(unsafe.Pointer(&r)), greenBrush)
			}
			// Self-drawn slim progress bar: no classic-theme border, our colors.
			if promptKind == setupPromptNone {
				procFillRect.Call(hdc, uintptr(unsafe.Pointer(&barRect)), barBgBrush)
				if total := ui.total.Load(); total > 0 {
					fill := barRect
					fill[2] = fill[0] + int32(int64(fill[2]-fill[0])*ui.cur.Load()/total)
					procFillRect.Call(hdc, uintptr(unsafe.Pointer(&fill)), greenBrush)
				}
			}
			procEndPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
			return 0
		case wmCtlcolorstatic:
			procSetBkMode.Call(wParam, transparentBkMode)
			switch lParam {
			case hHead, hKeySpace, hKeyK, hKeyReturn, hKeyW, hCancel,
				hPromptTitle, hPromptOption1, hPromptOption2, hPromptContinue:
				procSetTextColor.Call(wParam, colGreen)
			case hTag, uintptr(0):
				procSetTextColor.Call(wParam, colDim)
			case hText, hAccountInfo, hSuperInfo:
				procSetTextColor.Call(wParam, colText)
			default:
				procSetTextColor.Call(wParam, colDim)
			}
			return bgBrush
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
				ui.confirmCancel(hCancel)
			}
			return 0
		case wmCommand:
			switch wParam & 0xffff {
			case cancelControlID:
				ui.confirmCancel(hCancel)
			case promptOption1ID:
				if promptKind == setupPromptProvision {
					primary = true
				} else {
					primary = !primary
				}
				setOptionText()
			case promptOption2ID:
				if promptKind == setupPromptProvision {
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
			ui.confirmCancel(hCancel)
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
		brush: bgBrush, className: className,
	}
	const errClassAlreadyExists = 1410 // second UI in one run (download, then disk prep)
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		if errno, ok := err.(syscall.Errno); !ok || errno != errClassAlreadyExists {
			logf("progress UI: RegisterClassExW failed: %v", err)
			close(ui.ready)
			return
		}
	}

	sx, _, _ := procGetSystemMetrics.Call(smCxscreen)
	sy, _, _ := procGetSystemMetrics.Call(smCyscreen)
	title, _ := syscall.UTF16PtrFromString(appTitle)
	hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		wsPopup|wsVisible, (sx-windowW)/2, (sy-windowH)/2, windowW, windowH, 0, 0, hInst, 0)
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
	hHead = mk("OMARCHY", 144, 66, 300, 52, 0, 0)
	hTag = mk("Beautiful, Modern & Opinionated Linux", 146, 118, 320, 22, 0, 0)
	hText = mk("Preparing...", 40, 174, windowW-80, 22, 0, 0)
	// Starter keybindings on screen during the boot wait (the #1 field
	// complaint: an hour lost guessing tiling WM keys once the VM appears and
	// this window closes). Binds verified against Omarchy v4.0.1 defaults.
	hAccountInfo = mk("", 40, 230, windowW-80, 22, 0, 0)
	hSuperInfo = mk("On Linux, the Windows key is called SUPER", 40, 256, windowW-80, 22, 0, 0)
	hKeySpace = mk("SUPER+SPACE", 40, 286, 108, 20, 0, 0)
	hLabelMenu = mk("Menu", 150, 286, 90, 20, 0, 0)
	hKeyK = mk("SUPER+K", 280, 286, 86, 20, 0, 0)
	hLabelKeys = mk("All keybindings", 368, 286, 112, 20, 0, 0)
	hKeyReturn = mk("SUPER+RETURN", 40, 310, 108, 20, 0, 0)
	hLabelTerminal = mk("Terminal", 150, 310, 90, 20, 0, 0)
	hKeyW = mk("SUPER+W", 280, 310, 86, 20, 0, 0)
	hLabelClose = mk("Close window", 368, 310, 100, 20, 0, 0)
	hCancel = mk("CANCEL SETUP", 380, 342, 100, 20, ssNotify, cancelControlID)
	hPromptTitle = mk("", 40, 168, windowW-80, 24, 0, 0)
	hPromptBody = mk("", 40, 200, windowW-80, 22, 0, 0)
	hPromptOption1 = mk("", 40, 238, windowW-80, 24, ssNotify, promptOption1ID)
	hPromptOption2 = mk("", 40, 270, windowW-80, 24, ssNotify, promptOption2ID)
	hPromptContinue = mk("CONTINUE", 380, 342, 100, 22, ssNotify, promptContinueID)
	setPromptVisible(false)

	font := func(height, weight int, name string) uintptr {
		n, _ := syscall.UTF16PtrFromString(name)
		f, _, _ := procCreateFontW.Call(^uintptr(height-1), 0, 0, 0, uintptr(weight), 0, 0, 0, 0, 0, 0, 5, 0,
			uintptr(unsafe.Pointer(n)))
		return f
	}
	procSendMessageW.Call(hHead, wmSetfont, font(40, 800, "Segoe UI"), 1)
	procSendMessageW.Call(hTag, wmSetfont, font(15, 400, "Segoe UI"), 1)
	procSendMessageW.Call(hText, wmSetfont, font(16, 400, "Segoe UI"), 1)
	procSendMessageW.Call(hAccountInfo, wmSetfont, font(14, 600, "Segoe UI"), 1)
	procSendMessageW.Call(hSuperInfo, wmSetfont, font(15, 600, "Segoe UI"), 1)
	for _, h := range []uintptr{hKeySpace, hKeyK, hKeyReturn, hKeyW} {
		procSendMessageW.Call(h, wmSetfont, font(13, 700, "Segoe UI"), 1)
	}
	for _, h := range []uintptr{hLabelMenu, hLabelKeys, hLabelTerminal, hLabelClose} {
		procSendMessageW.Call(h, wmSetfont, font(13, 400, "Segoe UI"), 1)
	}
	procSendMessageW.Call(hCancel, wmSetfont, font(13, 600, "Segoe UI"), 1)
	procSendMessageW.Call(hPromptTitle, wmSetfont, font(17, 700, "Segoe UI"), 1)
	procSendMessageW.Call(hPromptBody, wmSetfont, font(14, 400, "Segoe UI"), 1)
	procSendMessageW.Call(hPromptOption1, wmSetfont, font(14, 700, "Segoe UI"), 1)
	procSendMessageW.Call(hPromptOption2, wmSetfont, font(14, 700, "Segoe UI"), 1)
	procSendMessageW.Call(hPromptContinue, wmSetfont, font(13, 700, "Segoe UI"), 1)

	procSetTimer.Call(hwnd, 1, 100, 0)
	procShowWindow.Call(hwnd, swShow)
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
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
