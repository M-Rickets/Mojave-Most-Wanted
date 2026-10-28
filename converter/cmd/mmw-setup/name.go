//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// The name friends see: the NAME pill under the checklist.

var nameEdit struct {
	sync.Mutex
	on  bool
	buf string
}

const nameMax = 20

func namePill() string {
	nameEdit.Lock()
	defer nameEdit.Unlock()
	if nameEdit.on {
		return "NAME: " + strings.ToUpper(nameEdit.buf) + "_"
	}
	return "NAME: " + strings.ToUpper(playerName())
}

func nameStart() {
	n := playerName()
	nameEdit.Lock()
	nameEdit.on, nameEdit.buf = true, n
	nameEdit.Unlock()
	app.status("Type the name your friends see, then press Enter (Esc keeps %s).", n)
	netPills()
}

func nameEditing() bool { nameEdit.Lock(); defer nameEdit.Unlock(); return nameEdit.on }

// A typed character (WM_CHAR)
func nameChar(c rune) {
	nameEdit.Lock()
	ok := nameEdit.on && len(nameEdit.buf) < nameMax && (c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || strings.ContainsRune(" -_.", c))
	if ok {
		nameEdit.buf += string(c)
	}
	nameEdit.Unlock()
	if ok {
		netPills()
	}
}

// Backspace, Enter, Esc (WM_KEYDOWN); true if it was taken
func nameKey(vk uintptr) bool {
	nameEdit.Lock()
	if !nameEdit.on {
		nameEdit.Unlock()
		return false
	}
	switch vk {
	case 0x08:
		if len(nameEdit.buf) > 0 {
			nameEdit.buf = nameEdit.buf[:len(nameEdit.buf)-1]
		}
		nameEdit.Unlock()
		netPills()
		return true
	case vkEscape:
		nameEdit.on = false
		nameEdit.Unlock()
		app.status("Your name stays %s.", playerName())
		netPills()
		return true
	case vkReturn:
		n := strings.TrimSpace(nameEdit.buf)
		nameEdit.on = false
		nameEdit.Unlock()
		if n == "" {
			app.status("A name can't be empty: it stays %s.", playerName())
		} else if err := saveName(n); err != nil {
			app.status("Couldn't save your name: %v", err)
		} else {
			app.status("Friends will see you as %s.", n)
		}
		netPills()
		return true
	}
	nameEdit.Unlock()
	return true
}

func saveName(n string) error {
	app.mu.Lock()
	game := app.game
	app.mu.Unlock()
	if game == "" {
		return os.ErrNotExist
	}
	dir := filepath.Join(pluginDir(game), "MojaveMostWanted")
	os.MkdirAll(dir, 0o755)
	if err := setKeyLine(filepath.Join(dir, "settings.ini"), "playerName", n); err != nil {
		return err
	}
	if _, err := os.Stat(netIniPath(game)); err == nil {
		setKeyLine(netIniPath(game), "name", n)
	}
	app.logf("Your name for friends: %s", n)
	return nil
}

// Key=value in a plain ini file, every other line kept
func setKeyLine(path, key, val string) error {
	b, _ := os.ReadFile(path)
	lines := splitLines(string(b))
	done := false
	for i, l := range lines {
		if k, _, ok := strings.Cut(strings.TrimSpace(l), "="); ok && strings.EqualFold(k, key) {
			lines[i] = key + "=" + val
			done = true
		}
	}
	if !done {
		lines = append(lines, key+"="+val)
	}
	return os.WriteFile(path, []byte(strings.Join(lines, "\r\n")+"\r\n"), 0o644)
}
