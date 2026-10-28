//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"mmw/cmd/mmw-setup/scene"
	"mmw/internal/p2p"
)

// Undo everything: the cars and roads the launcher built, then every file setup added or replaced, the folders it made,
// the load order and settings it changed - back as they were.
func undoAll() {
	app.mu.Lock()
	if app.busy {
		app.mu.Unlock()
		return
	}
	app.busy = true
	game := app.game
	app.mu.Unlock()
	defer func() { app.mu.Lock(); app.busy = false; app.mu.Unlock() }()
	if game == "" || !loadManifest(game) {
		app.status("Nothing to undo: setup hasn't put anything in this game.")
		return
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then undo again.", p)
		return
	}
	app.button("UNDOING", scene.BtnWorking, nil)
	addonsOffAll(game)
	app.status("Taking Mojave Enhanced out...")
	if l := filepath.Join(mmwDir(game), "mmw-launch.exe"); exists(l) {
		out, err := hiddenCmd(l, "--fnv", game, "--uninstall").CombinedOutput()
		app.logf("%s", strings.TrimSpace(string(out)))
		if err != nil {
			app.logf("launcher clean-up: %v", err)
		}
	}
	if exists(netIniPath(game)) {
		shellExecute("runas", "netsh.exe", `advfirewall firewall delete rule name="Mojave Most Wanted"`, "", 0)
		shellExecute("runas", "netsh.exe", `advfirewall firewall delete rule name="Mojave Enhanced"`, "", 0)
		if st := readNet(game); st.UPnP && st.Port > 0 {
			if u, err := p2p.FindRouter(2 * time.Second); err == nil {
				u.UnmapPort(st.Port)
			}
		}
	}
	removed, restored := 0, 0
	for f, bk := range man.Replaced {
		b, err := os.ReadFile(bk)
		if err == nil && os.WriteFile(f, b, 0o644) == nil {
			restored++
		} else {
			app.logf("couldn't put back %s: %v", redact(f), err)
		}
	}
	exe, _ := os.Executable()
	selfLeft := ""
	for i := len(man.Added) - 1; i >= 0; i-- {
		f := man.Added[i]
		if _, rep := man.Replaced[f]; rep {
			continue
		}
		if strings.EqualFold(filepath.Clean(f), filepath.Clean(exe)) {
			selfLeft = f
			continue
		}
		if err := os.Remove(f); err == nil {
			removed++
		} else if !os.IsNotExist(err) {
			app.logf("couldn't remove %s: %v", redact(f), err)
		}
	}
	if desk := knownFolder(folderDesktop); desk != "" {
		os.Remove(filepath.Join(desk, "Mojave Most Wanted.lnk"))
		os.Remove(filepath.Join(desk, "Mojave Enhanced.lnk"))
	}
	for _, f := range []string{"mmw-launch.exe", "mmw-launch.log", "mmw-cache.json", "paths.ini"} {
		os.Remove(filepath.Join(mmwDir(game), f))
	}
	for _, d := range []string{"heat", "radio", "dev", "workbench", filepath.Join("packs", ".unpacked")} {
		os.RemoveAll(filepath.Join(mmwDir(game), d))
	}
	for _, f := range []string{"MojaveMostWanted.log", "MojaveMostWanted-previous.log", "MojaveMostWanted-crash.log", "MojaveMostWanted.prev.log", "MojaveMostWanted.dll.new"} {
		os.Remove(filepath.Join(pluginDir(game), f))
	}
	os.RemoveAll(filepath.Join(pluginDir(game), "MojaveMostWanted", "gamefont"))
	dirs := append([]string{}, man.Dirs...)
	sort.Slice(dirs, func(i, j int) bool { return len(dirs[i]) > len(dirs[j]) })
	for _, d := range dirs {
		os.Remove(d)
	}
	os.RemoveAll(undoDir(game))
	app.logf("Undo: %d files removed, %d put back as they were.", removed, restored)
	{
		logDir := filepath.Join(os.Getenv("LOCALAPPDATA"), "MojaveMostWanted")
		self := selfLeft
		if self == "" {
			self = filepath.Join(mmwDir(game), setupName)
			os.Remove(filepath.Join(mmwDir(game), oldSetupName))
		}
		script := fmt.Sprintf(`for /l %%i in (1,1,30) do @(ping -n 2 127.0.0.1 >nul & del /f /q "%s" 2>nul & del /f /q "%s" 2>nul & if not exist "%s" (rmdir "%s" 2>nul & rmdir "%s" 2>nul & exit /b))`,
			self, filepath.Join(logDir, "setup.log"), self, filepath.Dir(self), logDir)
		hiddenCmd("cmd.exe", "/c", script).Start()
	}
	app.mu.Lock()
	app.installed = false
	app.mu.Unlock()
	app.links()
	app.with(func(s *scene.State) { s.StepSt = [5]scene.StepState{}; s.Progress = 0; s.Pills = nil })
	app.button("MAKE IT WORK", scene.BtnReady, func() { go install() })
	app.status("Done: Mojave Enhanced is out and the game is as it was.")
	if selfLeft != "" {
		go func() { time.Sleep(2 * time.Second); pPostMessageW.Call(hwndMain, wmClose, 0, 0) }()
	}
}
