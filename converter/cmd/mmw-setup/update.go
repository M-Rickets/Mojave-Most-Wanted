//go:build windows

package main

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"mmw/cmd/mmw-setup/scene"
)

// Updates come from the project's GitHub repository: update.json names the newest setup with its size and SHA-256, and
// update.json.sig is its Ed25519 signature by the key on the author's PC.
const updateBase = "https://raw.githubusercontent.com/M-Rickets/Mojave-Most-Wanted/main/updates/"

type updateInfo struct {
	Version  string   `json:"version"`
	Date     string   `json:"date"`
	Notes    string   `json:"notes"`
	Unlocked []string `json:"unlocked"`
	Setup    struct {
		File   string `json:"file"`
		Size   int64  `json:"size"`
		SHA256 string `json:"sha256"`
	} `json:"setup"`
}

var (
	updMu      sync.Mutex
	updDone    = make(chan struct{})
	updPending string
	updInfo    updateInfo
)

func checkUpdate() {
	defer close(updDone)
	if os.Getenv("MMW_NO_UPDATE") != "" {
		return
	}
	j, err := download(updateBase + "update.json")
	if err != nil {
		app.logf("Updates: couldn't check (%v)", err)
		return
	}
	sig, err := download(updateBase + "update.json.sig")
	if err != nil {
		app.logf("Updates: no signature next to update.json - ignored")
		return
	}
	sb, err := base64.StdEncoding.DecodeString(strings.TrimSpace(string(sig)))
	pk, _ := hex.DecodeString(updatePublicKey)
	if err != nil || len(pk) != ed25519.PublicKeySize || !ed25519.Verify(pk, j, sb) {
		app.logf("Updates: update.json's signature doesn't match - ignored")
		return
	}
	var u updateInfo
	if json.Unmarshal(j, &u) != nil || u.Version == "" {
		return
	}
	rolloutFrom(u.Unlocked)
	if !versionLess(appVersion, u.Version) {
		app.logf("Updates: this is the newest (%s)", appVersion)
		return
	}
	app.logf("Updates: %s is out (%s)\n%s", u.Version, u.Date, u.Notes)
	b, err := download(updateBase + u.Setup.File)
	if err != nil || int64(len(b)) != u.Setup.Size || sha(b) != strings.ToLower(u.Setup.SHA256) {
		app.logf("Updates: the download of %s didn't match its record (%v) - not used", u.Setup.File, err)
		return
	}
	exe, _ := os.Executable()
	p := filepath.Join(filepath.Dir(exe), "MojaveMostWanted-Setup.new.exe")
	if err := os.WriteFile(p, b, 0o755); err != nil {
		app.logf("Updates: %v", err)
		return
	}
	updMu.Lock()
	updPending, updInfo = p, u
	updMu.Unlock()
	app.mu.Lock()
	busy := app.busy
	app.mu.Unlock()
	if !busy {
		app.button("INSTALL UPDATE", scene.BtnUpdate, func() { applyPendingUpdate(false) })
		app.status("Mojave Enhanced %s is ready: press INSTALL UPDATE.", u.Version)
	}
}

func waitUpdate(d time.Duration) {
	select {
	case <-updDone:
	case <-time.After(d):
	}
}

// applyPendingUpdate swaps the new setup in for this one and starts it (with the same intent: play or not)
func applyPendingUpdate(play bool) bool {
	updMu.Lock()
	p := updPending
	updMu.Unlock()
	if p == "" {
		return false
	}
	exe, _ := os.Executable()
	old := exe + ".old"
	os.Remove(old)
	if err := os.Rename(exe, old); err != nil {
		app.logf("Updates: couldn't swap in the new setup: %v", err)
		return false
	}
	if err := os.Rename(p, exe); err != nil {
		os.Rename(old, exe)
		app.logf("Updates: couldn't swap in the new setup: %v", err)
		return false
	}
	args := ""
	if play {
		args = "--play"
	}
	app.logf("Updates: now %s - restarting", updInfo.Version)
	if err := shellExecute("open", exe, args, filepath.Dir(exe), 1); err != nil {
		app.logf("Updates: restart failed: %v", err)
		return false
	}
	go func() { time.Sleep(300 * time.Millisecond); os.Exit(0) }()
	return true
}

func init() {
	exe, _ := os.Executable()
	go func() { time.Sleep(3 * time.Second); os.Remove(exe + ".old") }()
	_ = fmt.Sprint
}
