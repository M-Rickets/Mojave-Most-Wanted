//go:build windows

package main

import (
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"

	"mmw/internal/p2p"
)

var (
	gUser32              = windows.NewLazySystemDLL("user32.dll")
	pRegisterHotKey      = gUser32.NewProc("RegisterHotKey")
	pGetMessageW         = gUser32.NewProc("GetMessageW")
	pPostThreadMessageW  = gUser32.NewProc("PostThreadMessageW")
	pClipCursor          = gUser32.NewProc("ClipCursor")
	pGetForegroundWindow = gUser32.NewProc("GetForegroundWindow")
	pGetWindowThreadPID  = gUser32.NewProc("GetWindowThreadProcessId")
	pShowWindowAsync     = gUser32.NewProc("ShowWindowAsync")
	pMessageBeep         = gUser32.NewProc("MessageBeep")
	pMessageBoxW         = gUser32.NewProc("MessageBoxW")
	pReleaseCapture      = gUser32.NewProc("ReleaseCapture")
	pSetForegroundWindow = gUser32.NewProc("SetForegroundWindow")
	pGetShellWindow      = gUser32.NewProc("GetShellWindow")
)

// spawnGuard: the guard started beside the game, detached (it outlives this launcher) - from its own copy,
// mmw-guard.exe, so mmw-launch.exe can still be replaced while the game runs (a running exe is locked)
func spawnGuard(fnv, exe string) {
	gexe := filepath.Join(filepath.Dir(exe), "mmw-guard.exe")
	if b, err := os.ReadFile(exe); err == nil {
		if old, err := os.ReadFile(gexe); err != nil || len(old) != len(b) || string(old) != string(b) {
			if err := os.WriteFile(gexe, b, 0o755); err != nil {
				log.Printf("guard: couldn't update mmw-guard.exe (%v) - one may be running; it carries on", err)
			}
		}
	}
	if !exists(gexe) {
		gexe = exe
	}
	cmd := exec.Command(gexe, "--guard", "--fnv", fnv)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x00000008 | 0x00000200}
	if err := cmd.Start(); err != nil {
		log.Printf("guard: couldn't start (%v) - no panic key this time", err)
		return
	}
	log.Print("guard started: Ctrl+Alt+End is the panic key (frees the mouse, playing together off, a frozen game ended)")
	cmd.Process.Release()
}

// The game's process started after `after` (0: none) - not a game from before still closing
func gamePID(after time.Time) uint32 {
	snap, err := windows.CreateToolhelp32Snapshot(windows.TH32CS_SNAPPROCESS, 0)
	if err != nil {
		return 0
	}
	defer windows.CloseHandle(snap)
	var e windows.ProcessEntry32
	e.Size = uint32(unsafe.Sizeof(e))
	for err = windows.Process32First(snap, &e); err == nil; err = windows.Process32Next(snap, &e) {
		if !strings.EqualFold(windows.UTF16ToString(e.ExeFile[:]), "FalloutNV.exe") {
			continue
		}
		h, err := windows.OpenProcess(windows.PROCESS_QUERY_LIMITED_INFORMATION, false, e.ProcessID)
		if err != nil {
			continue
		}
		var c, x, k, u windows.Filetime
		err = windows.GetProcessTimes(h, &c, &x, &k, &u)
		windows.CloseHandle(h)
		if err == nil && time.Unix(0, c.Nanoseconds()).After(after) {
			return e.ProcessID
		}
	}
	return 0
}

func runGuard(fnv, managed string) {
	if f, err := os.Create(filepath.Join(managed, "mmw-guard.log")); err == nil {
		log.SetOutput(io.MultiWriter(os.Stdout, f))
		defer f.Close()
	}
	log.SetFlags(log.Ltime)
	name, _ := windows.UTF16PtrFromString(`Local\MojaveEnhancedGuard`)
	mx, err := windows.CreateMutex(nil, false, name)
	if err == windows.ERROR_ALREADY_EXISTS {
		log.Print("guard: one is already running")
		return
	}
	defer windows.CloseHandle(mx)
	pluginDir := filepath.Join(fnv, "Data", "NVSE", "Plugins", "MojaveMostWanted")
	os.Remove(filepath.Join(pluginDir, "net.stop"))
	os.Remove(filepath.Join(pluginDir, "net.stopped"))
	var pid uint32
	since := time.Now().Add(-20 * time.Second)
	for t := 0; t < 180 && pid == 0; t++ {
		if pid = gamePID(since); pid == 0 {
			time.Sleep(time.Second)
		}
	}
	if pid == 0 {
		log.Print("guard: New Vegas didn't start within 3 minutes - leaving")
		return
	}
	h, err := windows.OpenProcess(windows.SYNCHRONIZE|windows.PROCESS_TERMINATE|windows.PROCESS_QUERY_LIMITED_INFORMATION, false, pid)
	if err != nil {
		log.Printf("guard: can't watch New Vegas (%v) - leaving", err)
		return
	}
	defer windows.CloseHandle(h)
	log.Printf("guard: watching New Vegas (process %d); Ctrl+Alt+End is the panic key", pid)
	var router *p2p.Router
	var routerT time.Time
	alive := func() bool { r, _ := windows.WaitForSingleObject(h, 0); return r == uint32(windows.WAIT_TIMEOUT) }
	g := &guard{pluginDir: pluginDir,
		router: func() *p2p.Router {
			if router == nil && time.Since(routerT) > 5*time.Second {
				routerT = time.Now()
				router, _ = p2p.FindRouter(2 * time.Second)
			}
			return router
		},
		freeMouse: func() {
			pClipCursor.Call(0)
			pReleaseCapture.Call()
			fg, _, _ := pGetForegroundWindow.Call()
			var p uint32
			pGetWindowThreadPID.Call(fg, uintptr(unsafe.Pointer(&p)))
			if fg != 0 && p == pid {
				pShowWindowAsync.Call(fg, 6)
				if sh, _, _ := pGetShellWindow.Call(); sh != 0 {
					pSetForegroundWindow.Call(sh)
				}
			}
			log.Print("guard: mouse freed")
		},
		gameAlive: alive,
		endGame:   func() error { return windows.TerminateProcess(h, 1) },
		tell: func(msg string) {
			pMessageBeep.Call(0x30)
			go func() {
				t, _ := windows.UTF16PtrFromString(msg)
				c, _ := windows.UTF16PtrFromString("Mojave Enhanced - panic key")
				pMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), 0x30|0x40000|0x10000)
			}()
		},
	}
	g.forward()
	var mu sync.Mutex
	var lastPress time.Time
	tidCh := make(chan uint32, 1)
	go func() {
		runtime.LockOSThread()
		tidCh <- windows.GetCurrentThreadId()
		if r, _, err := pRegisterHotKey.Call(0, 1, 0x0001|0x0002|0x4000, 0x23); r == 0 {
			log.Printf("guard: Ctrl+Alt+End is taken by another program (%v) - no panic key", err)
		}
		var msg struct {
			hwnd           uintptr
			message        uint32
			wParam, lParam uintptr
			time           uint32
			x, y           int32
		}
		for {
			r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&msg)), 0, 0, 0)
			if int32(r) <= 0 {
				return
			}
			if msg.message == 0x0312 {
				now := time.Now()
				twice := !lastPress.IsZero() && now.Sub(lastPress) < 5*time.Second
				lastPress = now
				if twice {
					go g.panic(true)
				} else {
					go func() { mu.Lock(); defer mu.Unlock(); g.panic(false) }()
				}
			}
		}
	}()
	tid := <-tidCh
	renew := time.Now()
	for alive() {
		windows.WaitForSingleObject(h, 1000)
		if time.Since(renew) > 30*time.Minute {
			renew = time.Now()
			g.forward()
		}
	}
	mu.Lock()
	g.closeForward()
	mu.Unlock()
	pPostThreadMessageW.Call(uintptr(tid), 0x0012, 0, 0)
	log.Print("guard: New Vegas closed - leaving")
}
