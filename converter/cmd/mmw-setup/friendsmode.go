//go:build windows

package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"

	"mmw/cmd/mmw-setup/scene"
)

const friendsPill = "friends mode"

type friendsState struct {
	On      bool     `json:"on"`
	Addons  []string `json:"addons"`
	Vanilla bool     `json:"vanilla"`
}

func friendsPath() string {
	return filepath.Join(knownFolder(folderLocalAppData), "MojaveMostWanted", "friends-mode.json")
}

func readFriends() friendsState {
	var s friendsState
	if b, err := os.ReadFile(friendsPath()); err == nil {
		json.Unmarshal(bytes.TrimPrefix(b, []byte("\xef\xbb\xbf")), &s)
	}
	return s
}

func writeFriends(s friendsState) {
	b, _ := json.MarshalIndent(s, "", " ")
	os.MkdirAll(filepath.Dir(friendsPath()), 0o755)
	os.WriteFile(friendsPath(), b, 0o644)
}

func friendsMode() bool { return readFriends().On }

func devBackupDir(game string) string { return filepath.Join(mmwDir(game), "dev-backup") }

// The mod's own files (setup's payload) as they are in the game now go aside, so this setup's can go in
func backupDevFiles(game string) (int, error) {
	zr, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return 0, err
	}
	names := []string{}
	for _, f := range zr.File {
		if !f.FileInfo().IsDir() {
			names = append(names, filepath.FromSlash(f.Name))
		}
	}
	names = append(names, filepath.Join("Data", "NVSE", "Plugins", "MojaveMostWanted.dll.new"))
	n := 0
	for _, rel := range names {
		src := filepath.Join(game, rel)
		b, err := os.ReadFile(src)
		if err != nil {
			continue
		}
		dst := filepath.Join(devBackupDir(game), rel)
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return n, err
		}
		if err := os.WriteFile(dst, b, 0o644); err != nil {
			return n, err
		}
		n++
	}
	os.Remove(filepath.Join(pluginDir(game), "MojaveMostWanted.dll.new"))
	return n, nil
}

// And back: every file kept aside goes where it was
func restoreDevFiles(game string) int {
	root := devBackupDir(game)
	n := 0
	filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		rel, err := filepath.Rel(root, p)
		if err != nil {
			return nil
		}
		in, err := os.Open(p)
		if err != nil {
			return nil
		}
		defer in.Close()
		dst := filepath.Join(game, rel)
		os.MkdirAll(filepath.Dir(dst), 0o755)
		out, err := os.Create(dst + ".mmwtmp")
		if err != nil {
			return nil
		}
		_, cerr := io.Copy(out, in)
		out.Close()
		if cerr == nil {
			os.Remove(dst)
			if os.Rename(dst+".mmwtmp", dst) == nil {
				n++
			}
		}
		return nil
	})
	os.RemoveAll(root)
	return n
}

// The pill was pressed: Friends mode on or off
func friendsPressed() {
	app.mu.Lock()
	game, busy := app.game, app.busy
	app.mu.Unlock()
	if game == "" || busy {
		return
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then switch Friends mode.", p)
		return
	}
	st := readFriends()
	q := "Switch Friends mode on?\n\nThis game is then set up exactly as your friends' setup leaves theirs: your local add-ons come out and the mod's files are this setup's own. Your dev files and add-ons are kept and come back when you switch it off. It can take a few minutes."
	if st.On {
		q = "Switch Friends mode off?\n\nYour dev files, the add-ons you had in and the game you had picked come back. It can take a few minutes."
	}
	if messageBox(q, "Friends mode", 0x24) != 6 {
		return
	}
	go func() {
		app.mu.Lock()
		app.busy = true
		app.mu.Unlock()
		app.button("WORKING", scene.BtnWorking, nil)
		app.progress(-1)
		defer func() {
			app.mu.Lock()
			app.busy = false
			app.mu.Unlock()
			app.progress(1)
			app.button("PLAY", scene.BtnDone, func() { go play() })
			netPills()
		}()
		if !st.On {
			var was []string
			for _, a := range loadAddons() {
				if !a.Hidden && a.in(game) {
					was = append(was, a.Name)
				}
			}
			app.mu.Lock()
			vanilla := man.Vanilla
			app.mu.Unlock()
			writeFriends(friendsState{On: true, Addons: was, Vanilla: vanilla})
			addonsOffAll(game)
			n, err := backupDevFiles(game)
			if err != nil {
				app.logf("Friends mode: keeping your dev files aside failed: %v", err)
			}
			if err := putIn(game); err != nil {
				app.logf("Friends mode: putting this setup's files in failed: %v", err)
			}
			saveManifest()
			enforceRollout()
			app.logf("Friends mode: on - %d add-on(s) out (%s), %d dev file(s) kept aside", len(was), strings.Join(was, ", "), n)
			app.status("Friends mode is on: this game is set up as your friends'. Switch it off for your dev side.")
			return
		}
		n := restoreDevFiles(game)
		app.mu.Lock()
		man.Vanilla = st.Vanilla
		app.mu.Unlock()
		if st.Vanilla {
			setPathsKey(game, "version", "vanilla")
		} else {
			setPathsKey(game, "version", "")
		}
		saveManifest()
		writeFriends(friendsState{})
		back := 0
		all := loadAddons()
		for _, name := range st.Addons {
			for _, a := range all {
				if strings.EqualFold(a.Name, name) && !a.in(game) {
					for _, p := range addonProviders(a, game, all) {
						runAddon(p, game, p.On)
					}
					app.status("Switching %s back on...", a.Name)
					if runAddon(a, game, a.On) && a.in(game) {
						back++
					}
				}
			}
		}
		app.mu.Lock()
		app.busy = false
		app.mu.Unlock()
		autoAddons(game)
		app.logf("Friends mode: off - %d dev file(s) back, %d of %d add-on(s) back in", n, back, len(st.Addons))
		app.status("Friends mode is off: your dev side is back.")
	}()
}
