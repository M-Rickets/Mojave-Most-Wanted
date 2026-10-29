//go:build windows

// Mojave Most Wanted Setup: one program that puts the mod in a friend's Fallout: New Vegas and starts it.
package main

import (
	"flag"
	"fmt"
	"image"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"mmw/cmd/mmw-setup/help"
	"mmw/cmd/mmw-setup/scene"
)

const appVersion = "1.0.35"

// Setup's own copy in the game folder (the desktop shortcut and updates use it); before 1.0.15 the mod was called
// Mojave Most Wanted
const setupName, oldSetupName = "MojaveEnhanced-Setup.exe", "MojaveMostWanted-Setup.exe"

var (
	hwndMain, hwndDetails uintptr
	dpiScale              = 1.0
	app                   = &App{}
	startT                = time.Now()
	flagGame              = flag.String("game", "", "the Fallout: New Vegas folder, if setup can't find it")
	flagMW                = flag.String("mw", "", "the Need for Speed Most Wanted folder, if setup can't find it")
	flagPlay              = flag.Bool("play", false, "start the game (the desktop shortcut)")
	flagResume            = flag.Bool("resume", false, "carry on after restarting with permission")
)

// App holds what the window shows; the work runs on its own goroutine and changes it under mu.
type App struct {
	mu          sync.Mutex
	st          scene.State
	hits        []scene.Hit
	logLines    []string
	busy        bool
	action      func()
	detailsOpen bool
	installed   bool
	game        string
}

func (a *App) with(f func(s *scene.State)) { a.mu.Lock(); f(&a.st); a.mu.Unlock(); redraw() }

func (a *App) status(f string, v ...any) {
	msg := fmt.Sprintf(f, v...)
	a.with(func(s *scene.State) { s.Status = msg })
	a.logf("%s", msg)
}

func (a *App) logf(f string, v ...any) {
	line := fmt.Sprintf(f, v...)
	a.mu.Lock()
	for _, l := range strings.Split(line, "\n") {
		a.logLines = append(a.logLines, l)
	}
	if len(a.logLines) > 4000 {
		a.logLines = a.logLines[len(a.logLines)-4000:]
	}
	a.mu.Unlock()
	writeLogFile(line)
	if hwndDetails != 0 {
		pPostMessageW.Call(hwndMain, wmApp+1, 0, 0)
	}
}

func (a *App) step(i int, st scene.StepState) { a.with(func(s *scene.State) { s.StepSt[i] = st }) }
func (a *App) progress(p float64)             { a.with(func(s *scene.State) { s.Progress = p }) }
func (a *App) button(label string, look int, act func()) {
	a.mu.Lock()
	a.st.Button, a.st.BtnLook, a.action = label, look, act
	a.mu.Unlock()
	redraw()
}
func (a *App) links() {
	a.mu.Lock()
	l := []string{"details", "report"}
	if a.installed {
		l = append(l, "undo everything")
	}
	if a.detailsOpen {
		l[0] = "hide details"
	}
	a.st.Links = l
	a.mu.Unlock()
	redraw()
}

// ---- The log file next to the setup (and in details)
var logFile *os.File

func writeLogFile(line string) {
	if logFile == nil {
		return
	}
	fmt.Fprintf(logFile, "%s %s\r\n", time.Now().Format("15:04:05"), strings.ReplaceAll(line, "\n", "\r\n   "))
}

func main() {
	runtime.LockOSThread()
	flag.Parse()
	exe, _ := os.Executable()
	logDir := filepath.Join(os.Getenv("LOCALAPPDATA"), "MojaveMostWanted")
	os.MkdirAll(logDir, 0o755)
	logFile, _ = os.OpenFile(filepath.Join(logDir, "setup.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	writeLogFile(fmt.Sprintf("---- Mojave Enhanced Setup %s (%s) started: %s", appVersion, exe, strings.Join(os.Args[1:], " ")))
	if _, _, e := pCreateMutexW.Call(0, 0, uintptr(unsafe.Pointer(u16("MojaveMostWantedSetup")))); e == syscall.ERROR_ALREADY_EXISTS {
		messageBox("Mojave Enhanced Setup is already open.", "Mojave Enhanced", 0x40)
		return
	}
	pSetProcessDpiAwareCtx.Call(^uintptr(3))
	steps := [5]string{"Find Fallout: New Vegas", "Get the modding tools", "Put in Mojave Enhanced", "Set up vanilla stable (no cars)", "Ready to play"}
	app.st = scene.State{Exe: "MOJAVE_ENHANCED.EXE", Button: "MAKE IT WORK", BtnLook: scene.BtnReady, Steps: steps,
		Status: "Ready when you are. Make sure Fallout: New Vegas is closed.", DoneT: -1}
	app.st.SideL, _ = help.Columns(false, false, "")
	app.links()
	app.action = func() { go install() }
	createWindow()
	go startup()
	var m winMsg
	for {
		r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		if m.message == 0x0102 && m.hwnd != hwndDetails && nameEditing() {
			nameChar(rune(m.wParam))
			continue
		}
		if m.message == wmKeyDown && m.hwnd != hwndDetails && nameEditing() && m.wParam != 0x10 && m.wParam != 0x14 {
			if m.wParam == 0x08 || m.wParam == vkReturn || m.wParam == vkEscape {
				nameKey(m.wParam)
				continue
			}
			pTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
			continue
		}
		if m.message == wmKeyDown && m.hwnd != hwndDetails {
			switch m.wParam {
			case vkReturn:
				press()
			case vkEscape:
				app.mu.Lock()
				panel := app.st.Info || app.st.Menu
				app.mu.Unlock()
				if app.detailsOpen {
					toggleDetails()
				} else if panel {
					go pillPressed(scene.MenuClose)
				}
			}
		}
		pTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		pDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}

func press() {
	app.mu.Lock()
	act, busy := app.action, app.busy
	app.mu.Unlock()
	if act != nil && !busy {
		act()
	} else if act != nil && app.st.BtnLook == scene.BtnAction {
		act()
	}
}

func redraw() {
	if hwndMain != 0 {
		pInvalidateRect.Call(hwndMain, 0, 0)
	}
}

var frame *image.RGBA

func createWindow() {
	inst, _, _ := pGetModuleHandleW.Call(0)
	cls := u16("MMWSetupWindow")
	cur, _, _ := pLoadCursorW.Call(0, idcArrow)
	icon, _, _ := pLoadIconW.Call(inst, 1)
	if icon == 0 {
		exe, _ := os.Executable()
		icon, _, _ = shell32.NewProc("ExtractIconW").Call(inst, uintptr(unsafe.Pointer(u16(exe))), 0)
	}
	wc := wndClassEx{wndProc: syscall.NewCallback(wndProc), instance: inst, cursor: cur, className: cls, icon: icon, iconSm: icon}
	wc.size = uint32(unsafe.Sizeof(wc))
	pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))
	sw, _, _ := pGetSystemMetrics.Call(0)
	sh, _, _ := pGetSystemMetrics.Call(1)
	w, h := scene.W, scene.H
	hwndMain, _, _ = pCreateWindowExW.Call(wsExAppWindow, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16("Mojave Enhanced Setup"))),
		wsPopup|wsMinimizeBox|wsSysMenu|0x02000000, (sw-uintptr(w))/2, (sh-uintptr(h))/2, uintptr(w), uintptr(h), 0, 0, inst, 0)
	dpi, _, _ := pGetDpiForWindow.Call(hwndMain)
	if dpi >= 96 {
		dpiScale = float64(dpi) / 96
	}
	resize()
	edit := u16("EDIT")
	hwndDetails, _, _ = pCreateWindowExW.Call(0, uintptr(unsafe.Pointer(edit)), 0, wsChild|wsVScroll|esMultiline|esReadOnly|esAutoVScroll,
		0, 0, 10, 10, hwndMain, 0, inst, 0)
	layoutDetails()
	pShowWindow.Call(hwndMain, swShow)
	pSetForegroundWindow.Call(hwndMain)
	pSetTimer.Call(hwndMain, 1, 33, 0)
}

func resize() {
	w, h := int(float64(scene.W)*dpiScale+0.5), int(float64(scene.H)*dpiScale+0.5)
	var r [4]int32
	pGetWindowRect.Call(hwndMain, uintptr(unsafe.Pointer(&r)))
	pMoveWindow.Call(hwndMain, uintptr(r[0]), uintptr(r[1]), uintptr(w), uintptr(h), 1)
	frame = image.NewRGBA(image.Rect(0, 0, w, h))
}

var detailsFont uintptr

func layoutDetails() {
	s := dpiScale
	pMoveWindow.Call(hwndDetails, uintptr(14*s), uintptr(44*s), uintptr((scene.W-28)*s), uintptr((scene.H-58)*s), 1)
	detailsFont, _, _ = pCreateFontW.Call(uintptr(-int32(13*s)), 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 5, 0, uintptr(unsafe.Pointer(u16("Consolas"))))
	pSendMessageW.Call(hwndDetails, wmSetFont, detailsFont, 1)
}

func toggleDetails() {
	app.mu.Lock()
	app.detailsOpen = !app.detailsOpen
	open := app.detailsOpen
	app.mu.Unlock()
	if open {
		fillDetails()
		pShowWindow.Call(hwndDetails, swShow)
		pSetFocus.Call(hwndDetails)
	} else {
		pShowWindow.Call(hwndDetails, swHide)
		pSetFocus.Call(hwndMain)
	}
	app.links()
}

func fillDetails() {
	app.mu.Lock()
	text := strings.Join(app.logLines, "\r\n")
	app.mu.Unlock()
	pSetWindowTextW.Call(hwndDetails, uintptr(unsafe.Pointer(u16(text))))
	pSendMessageW.Call(hwndDetails, emSetSel, uintptr(len(text)), uintptr(len(text)))
	pSendMessageW.Call(hwndDetails, emScroll, 7, 0)
}

var hover string
var pressed bool
var tracking bool

func hitAt(lp uintptr) string {
	x := float64(int16(lp&0xFFFF)) / dpiScale
	y := float64(int16((lp>>16)&0xFFFF)) / dpiScale
	app.mu.Lock()
	defer app.mu.Unlock()
	for _, h := range app.hits {
		if x >= h.X && x < h.X+h.W && y >= h.Y && y < h.Y+h.H {
			return h.Name
		}
	}
	return ""
}

func wndProc(hwnd uintptr, msg uint32, wp, lp uintptr) uintptr {
	switch msg {
	case wmTimer:
		pInvalidateRect.Call(hwnd, 0, 0)
		return 0
	case wmApp + 1:
		if app.detailsOpen {
			fillDetails()
		}
		return 0
	case wmEraseBkgnd:
		return 1
	case wmPaint:
		var ps paintStruct
		hdc, _, _ := pBeginPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
		paint(hdc)
		pEndPaint.Call(hwnd, uintptr(unsafe.Pointer(&ps)))
		return 0
	case wmNcHitTest:
		r, _, _ := pDefWindowProcW.Call(hwnd, uintptr(msg), wp, lp)
		if r == htClient {
			var wr [4]int32
			pGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&wr)))
			x := int32(int16(lp&0xFFFF)) - wr[0]
			y := int32(int16((lp>>16)&0xFFFF)) - wr[1]
			name := hitAt(uintptr(uint16(x)) | uintptr(uint16(y))<<16)
			if float64(y) < 33*dpiScale && name == "" {
				return htCaption
			}
		}
		return r
	case wmMouseMove:
		h := hitAt(lp)
		if !tracking {
			tme := trackMouseEvent{flags: 2, hwnd: hwnd}
			tme.size = uint32(unsafe.Sizeof(tme))
			pTrackMouseEvent.Call(uintptr(unsafe.Pointer(&tme)))
			tracking = true
		}
		if h != hover {
			hover = h
			redraw()
		}
		return 0
	case wmMouseLeave:
		tracking = false
		hover, pressed = "", false
		redraw()
		return 0
	case wmSetCursor:
		if hover != "" {
			c, _, _ := pLoadCursorW.Call(0, idcHand)
			pSetCursor.Call(c)
			return 1
		}
	case wmLButtonDown:
		if hitAt(lp) == "button" {
			pressed = true
			redraw()
		}
		return 0
	case wmLButtonUp:
		h := hitAt(lp)
		wasPressed := pressed
		pressed = false
		switch h {
		case "button":
			if wasPressed {
				press()
			}
		case "close":
			if app.busy && messageBox("Setup is still working. Stop and close it?", "Mojave Enhanced", 0x34) != 6 {
				break
			}
			pPostQuitMessage.Call(0)
		case "minimise":
			pShowWindow.Call(hwnd, swMinimize)
		case "details", "hide details":
			toggleDetails()
		case "report":
			go makeReport()
		default:
			if h != "" {
				go pillPressed(h)
			}
		case "undo everything":
			if !app.busy && messageBox("Take Mojave Enhanced out of Fallout: New Vegas again?\n\nThis removes the mod, anything it built and the modding tools setup added, and puts back anything it replaced. Your saves stay.", "Undo everything", 0x34) == 6 {
				go undoAll()
			}
		}
		redraw()
		return 0
	case wmDpiChanged:
		dpiScale = float64(wp&0xFFFF) / 96
		resize()
		layoutDetails()
		return 0
	case wmClose:
		pPostQuitMessage.Call(0)
		return 0
	case wmDestroy:
		pPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := pDefWindowProcW.Call(hwnd, uintptr(msg), wp, lp)
	return r
}

func paint(hdc uintptr) {
	app.mu.Lock()
	app.st.T = time.Since(startT).Seconds()
	app.st.Hover, app.st.Pressed = hover, pressed
	st := app.st
	app.mu.Unlock()
	hits := scene.Render(frame, dpiScale, &st)
	app.mu.Lock()
	app.hits = hits
	app.mu.Unlock()
	p := frame.Pix
	for i := 0; i+3 < len(p); i += 4 {
		p[i], p[i+2] = p[i+2], p[i]
	}
	w, h := frame.Rect.Dx(), frame.Rect.Dy()
	bi := bitmapInfoHeader{width: int32(w), height: -int32(h), planes: 1, bitCount: 32}
	bi.size = uint32(unsafe.Sizeof(bi))
	pStretchDIBits.Call(hdc, 0, 0, uintptr(w), uintptr(h), 0, 0, uintptr(w), uintptr(h), uintptr(unsafe.Pointer(&p[0])), uintptr(unsafe.Pointer(&bi)), 0, 0x00CC0020)
}
