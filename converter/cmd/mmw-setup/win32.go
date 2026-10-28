//go:build windows

package main

import (
	"runtime"
	"strconv"
	"syscall"
	"unicode/utf16"
	"unsafe"
)

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	advapi32 = syscall.NewLazyDLL("advapi32.dll")
	version  = syscall.NewLazyDLL("version.dll")
	ole32    = syscall.NewLazyDLL("ole32.dll")

	pRegisterClassExW      = user32.NewProc("RegisterClassExW")
	pCreateWindowExW       = user32.NewProc("CreateWindowExW")
	pDefWindowProcW        = user32.NewProc("DefWindowProcW")
	pGetMessageW           = user32.NewProc("GetMessageW")
	pTranslateMessage      = user32.NewProc("TranslateMessage")
	pDispatchMessageW      = user32.NewProc("DispatchMessageW")
	pPostQuitMessage       = user32.NewProc("PostQuitMessage")
	pPostMessageW          = user32.NewProc("PostMessageW")
	pSetTimer              = user32.NewProc("SetTimer")
	pInvalidateRect        = user32.NewProc("InvalidateRect")
	pBeginPaint            = user32.NewProc("BeginPaint")
	pEndPaint              = user32.NewProc("EndPaint")
	pShowWindow            = user32.NewProc("ShowWindow")
	pLoadCursorW           = user32.NewProc("LoadCursorW")
	pSetCursor             = user32.NewProc("SetCursor")
	pSendMessageW          = user32.NewProc("SendMessageW")
	pSetWindowTextW        = user32.NewProc("SetWindowTextW")
	pMoveWindow            = user32.NewProc("MoveWindow")
	pGetSystemMetrics      = user32.NewProc("GetSystemMetrics")
	pSetProcessDpiAwareCtx = user32.NewProc("SetProcessDpiAwarenessContext")
	pGetDpiForWindow       = user32.NewProc("GetDpiForWindow")
	pTrackMouseEvent       = user32.NewProc("TrackMouseEvent")
	pSetForegroundWindow   = user32.NewProc("SetForegroundWindow")
	pMessageBoxW           = user32.NewProc("MessageBoxW")
	pSetFocus              = user32.NewProc("SetFocus")
	pLoadIconW             = user32.NewProc("LoadIconW")
	pGetWindowRect         = user32.NewProc("GetWindowRect")

	pStretchDIBits     = gdi32.NewProc("StretchDIBits")
	pCreateFontW       = gdi32.NewProc("CreateFontW")
	pGetModuleHandleW  = kernel32.NewProc("GetModuleHandleW")
	pCreateMutexW      = kernel32.NewProc("CreateMutexW")
	pCreateToolhelp32  = kernel32.NewProc("CreateToolhelp32Snapshot")
	pProcess32FirstW   = kernel32.NewProc("Process32FirstW")
	pProcess32NextW    = kernel32.NewProc("Process32NextW")
	pGlobalMemoryStatu = kernel32.NewProc("GlobalMemoryStatusEx")
	pShellExecuteW     = shell32.NewProc("ShellExecuteW")
	pSHGetKnownFolder  = shell32.NewProc("SHGetKnownFolderPath")
	pCoTaskMemFree     = ole32.NewProc("CoTaskMemFree")
	pIsUserAnAdmin     = shell32.NewProc("IsUserAnAdmin")

	pGetFileVersionInfoSizeW = version.NewProc("GetFileVersionInfoSizeW")
	pGetFileVersionInfoW     = version.NewProc("GetFileVersionInfoW")
	pVerQueryValueW          = version.NewProc("VerQueryValueW")
)

const (
	wmDestroy     = 0x0002
	wmPaint       = 0x000F
	wmClose       = 0x0010
	wmEraseBkgnd  = 0x0014
	wmSetCursor   = 0x0020
	wmTimer       = 0x0113
	wmMouseMove   = 0x0200
	wmLButtonDown = 0x0201
	wmLButtonUp   = 0x0202
	wmMouseLeave  = 0x02A3
	wmKeyDown     = 0x0100
	wmNcHitTest   = 0x0084
	wmDpiChanged  = 0x02E0
	wmSetFont     = 0x0030
	wmApp         = 0x8000

	wsPopup       = 0x80000000
	wsChild       = 0x40000000
	wsVScroll     = 0x00200000
	wsMinimizeBox = 0x00020000
	wsSysMenu     = 0x00080000
	wsExAppWindow = 0x00040000
	esMultiline   = 0x0004
	esReadOnly    = 0x0800
	esAutoVScroll = 0x0040

	htCaption  = 2
	htClient   = 1
	swShow     = 5
	swHide     = 0
	swMinimize = 6
	idcArrow   = 32512
	idcHand    = 32649
	vkReturn   = 0x0D
	vkEscape   = 0x1B
	emSetSel   = 0x00B1
	emScroll   = 0x00B5
)

type wndClassEx struct {
	size       uint32
	style      uint32
	wndProc    uintptr
	clsExtra   int32
	wndExtra   int32
	instance   uintptr
	icon       uintptr
	cursor     uintptr
	background uintptr
	menuName   *uint16
	className  *uint16
	iconSm     uintptr
}

type point struct{ x, y int32 }

type winMsg struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	pt      point
}

type paintStruct struct {
	hdc         uintptr
	erase       int32
	rcPaint     [4]int32
	restore     int32
	incUpdate   int32
	rgbReserved [32]byte
}

type bitmapInfoHeader struct {
	size          uint32
	width         int32
	height        int32
	planes        uint16
	bitCount      uint16
	compression   uint32
	sizeImage     uint32
	xPelsPerMeter int32
	yPelsPerMeter int32
	clrUsed       uint32
	clrImportant  uint32
}

type trackMouseEvent struct {
	size      uint32
	flags     uint32
	hwnd      uintptr
	hoverTime uint32
}

func u16(s string) *uint16 { p, _ := syscall.UTF16PtrFromString(s); return p }

func shellExecute(verb, file, args, dir string, show int) error {
	var v, a, d *uint16
	if verb != "" {
		v = u16(verb)
	}
	if args != "" {
		a = u16(args)
	}
	if dir != "" {
		d = u16(dir)
	}
	r, _, _ := pShellExecuteW.Call(0, uintptr(unsafe.Pointer(v)), uintptr(unsafe.Pointer(u16(file))), uintptr(unsafe.Pointer(a)), uintptr(unsafe.Pointer(d)), uintptr(show))
	if r <= 32 {
		return syscall.Errno(r)
	}
	return nil
}

func openURL(url string) { shellExecute("open", url, "", "", 1) }

func isAdmin() bool { r, _, _ := pIsUserAnAdmin.Call(); return r != 0 }

func messageBox(text, caption string, flags uint32) int {
	r, _, _ := pMessageBoxW.Call(hwndMain, uintptr(unsafe.Pointer(u16(text))), uintptr(unsafe.Pointer(u16(caption))), uintptr(flags))
	return int(r)
}

// knownFolder: FOLDERID_Downloads, Documents, LocalAppData, Desktop
func knownFolder(guid [16]byte) string {
	var p *uint16
	r, _, _ := pSHGetKnownFolder.Call(uintptr(unsafe.Pointer(&guid[0])), 0, 0, uintptr(unsafe.Pointer(&p)))
	if r != 0 || p == nil {
		return ""
	}
	defer pCoTaskMemFree.Call(uintptr(unsafe.Pointer(p)))
	return syscall.UTF16ToString((*[1 << 15]uint16)(unsafe.Pointer(p))[:])
}

func guid(a uint32, b, c uint16, d [8]byte) [16]byte {
	var g [16]byte
	g[0], g[1], g[2], g[3] = byte(a), byte(a>>8), byte(a>>16), byte(a>>24)
	g[4], g[5] = byte(b), byte(b>>8)
	g[6], g[7] = byte(c), byte(c>>8)
	copy(g[8:], d[:])
	return g
}

var (
	folderDownloads    = guid(0x374DE290, 0x123F, 0x4565, [8]byte{0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B})
	folderDocuments    = guid(0xFDD39AD0, 0x238F, 0x46AF, [8]byte{0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7})
	folderLocalAppData = guid(0xF1B32785, 0x6FBA, 0x4FCF, [8]byte{0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91})
	folderDesktop      = guid(0xB4BFCC3A, 0xDB2C, 0x424C, [8]byte{0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41})
)

// processRunning reports whether a process with that exe name (case-insensitive) runs.
func processRunning(names ...string) string {
	snap, _, _ := pCreateToolhelp32.Call(2, 0)
	if snap == uintptr(syscall.InvalidHandle) {
		return ""
	}
	defer syscall.CloseHandle(syscall.Handle(snap))
	type pe32 struct {
		size         uint32
		usage        uint32
		pid          uint32
		heapID       uintptr
		moduleID     uint32
		threads      uint32
		parentPid    uint32
		priClassBase int32
		flags        uint32
		exeFile      [260]uint16
	}
	var e pe32
	e.size = uint32(unsafe.Sizeof(e))
	r, _, _ := pProcess32FirstW.Call(snap, uintptr(unsafe.Pointer(&e)))
	for r != 0 {
		n := syscall.UTF16ToString(e.exeFile[:])
		for _, want := range names {
			if equalFold(n, want) {
				return n
			}
		}
		r, _, _ = pProcess32NextW.Call(snap, uintptr(unsafe.Pointer(&e)))
	}
	return ""
}

func equalFold(a, b string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := 0; i < len(a); i++ {
		x, y := a[i], b[i]
		if 'A' <= x && x <= 'Z' {
			x += 32
		}
		if 'A' <= y && y <= 'Z' {
			y += 32
		}
		if x != y {
			return false
		}
	}
	return true
}

// fileVersion reads a PE file's version resource (FileVersion: a.b.c.d), "" if none.
func fileVersion(path string) string {
	p := u16(path)
	n, _, _ := pGetFileVersionInfoSizeW.Call(uintptr(unsafe.Pointer(p)), 0)
	if n == 0 {
		return ""
	}
	buf := make([]byte, n)
	r, _, _ := pGetFileVersionInfoW.Call(uintptr(unsafe.Pointer(p)), 0, n, uintptr(unsafe.Pointer(&buf[0])))
	if r == 0 {
		return ""
	}
	var fi unsafe.Pointer
	var l uint32
	r, _, _ = pVerQueryValueW.Call(uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(u16(`\`))), uintptr(unsafe.Pointer(&fi)), uintptr(unsafe.Pointer(&l)))
	if r == 0 || l < 52 {
		return ""
	}
	ffi := (*[13]uint32)(fi)
	ms, ls := ffi[2], ffi[3]
	return itoa(int(ms>>16)) + "." + itoa(int(ms&0xFFFF)) + "." + itoa(int(ls>>16)) + "." + itoa(int(ls&0xFFFF))
}

func itoa(i int) string { return strconv.Itoa(i) }

func memoryGB() float64 {
	type memStatusEx struct {
		length    uint32
		load      uint32
		totalPhys uint64
		availPhys uint64
		rest      [5]uint64
	}
	var m memStatusEx
	m.length = uint32(unsafe.Sizeof(m))
	pGlobalMemoryStatu.Call(uintptr(unsafe.Pointer(&m)))
	return float64(m.totalPhys) / (1 << 30)
}

// ---- The Windows file picker, comdlg32 GetOpenFileNameW
var (
	comdlg32          = syscall.NewLazyDLL("comdlg32.dll")
	pGetOpenFileNameW = comdlg32.NewProc("GetOpenFileNameW")
)

type openFileNameW struct {
	lStructSize       uint32
	hwndOwner         uintptr
	hInstance         uintptr
	lpstrFilter       *uint16
	lpstrCustomFilter *uint16
	nMaxCustFilter    uint32
	nFilterIndex      uint32
	lpstrFile         *uint16
	nMaxFile          uint32
	lpstrFileTitle    *uint16
	nMaxFileTitle     uint32
	lpstrInitialDir   *uint16
	lpstrTitle        *uint16
	Flags             uint32
	nFileOffset       uint16
	nFileExtension    uint16
	lpstrDefExt       *uint16
	lCustData         uintptr
	lpfnHook          uintptr
	lpTemplateName    *uint16
	pvReserved        uintptr
	dwReserved        uint32
	FlagsEx           uint32
}

// filter: pairs separated by NULs and ended by two (as Windows wants it); "" when cancelled
func openFileDialog(title, filter string) string {
	f := utf16.Encode([]rune(filter))
	buf := make([]uint16, 1024)
	ofn := openFileNameW{
		hwndOwner:       hwndMain,
		lpstrFilter:     &f[0],
		nFilterIndex:    1,
		lpstrFile:       &buf[0],
		nMaxFile:        uint32(len(buf)),
		lpstrInitialDir: u16(knownFolder(folderDownloads)),
		lpstrTitle:      u16(title),
		Flags:           0x00080000 | 0x00001000 | 0x00000800 | 0x00000008,
	}
	ofn.lStructSize = uint32(unsafe.Sizeof(ofn))
	r, _, _ := pGetOpenFileNameW.Call(uintptr(unsafe.Pointer(&ofn)))
	runtime.KeepAlive(f)
	if r == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}
