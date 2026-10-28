//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
)

// A slow roll-out.
var defaultUnlocked = []string{"vanilla"}

func localUnlocked() []string {
	b, err := os.ReadFile(filepath.Join(knownFolder(folderLocalAppData), "MojaveMostWanted", "unlocked.txt"))
	if err != nil {
		return nil
	}
	return strings.Fields(strings.ToLower(string(b)))
}

// pathsValue: key's value in the launcher's paths.ini ("" if it isn't there)
func pathsValue(game, key string) string {
	b, err := os.ReadFile(filepath.Join(mmwDir(game), "paths.ini"))
	if err != nil {
		return ""
	}
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(l), "="); ok && strings.EqualFold(k, key) {
			return v
		}
	}
	return ""
}

// unlockedByName: f itself is in the signed or the local list ("all" doesn't count)
func unlockedByName(f string) bool {
	app.mu.Lock()
	list := man.Unlocked
	app.mu.Unlock()
	for _, x := range append(append([]string{}, list...), localUnlocked()...) {
		if x == f {
			return true
		}
	}
	return false
}

func unlocked(f string) bool {
	app.mu.Lock()
	list := man.Unlocked
	app.mu.Unlock()
	if list == nil {
		list = defaultUnlocked
	}
	for _, x := range append(append([]string{}, list...), localUnlocked()...) {
		if x == f || x == "all" {
			return true
		}
	}
	return false
}

// rolloutFrom keeps the signed list (only when it says something) and applies it
func rolloutFrom(list []string) {
	if list == nil {
		return
	}
	for i := range list {
		list[i] = strings.ToLower(strings.TrimSpace(list[i]))
	}
	app.mu.Lock()
	changed := strings.Join(man.Unlocked, ",") != strings.Join(list, ",") || man.Unlocked == nil
	man.Unlocked = list
	game := app.game
	app.mu.Unlock()
	if changed && game != "" {
		saveManifest()
		app.logf("Updates: on offer: %s", strings.Join(list, ", "))
	}
	enforceRollout()
	netPills()
}

// enforceRollout: without Mojave Most Wanted on offer, PLAY starts vanilla stable; the weather (seasons, snow, rain)
// only where "weather" itself is on offer - "all" doesn't open it
func enforceRollout() {
	app.mu.Lock()
	game, vanilla := app.game, man.Vanilla
	app.mu.Unlock()
	if game != "" {
		w := "0"
		if unlockedByName("weather") {
			w = "1"
		}
		if pathsValue(game, "weather") != w {
			setPathsKey(game, "weather", w)
			app.logf("Weather: %s", map[string]string{"1": "on offer (seasons, snow, rain)", "0": "not on offer yet"}[w])
		}
	}
	if game == "" || vanilla || unlocked("mmw") {
		return
	}
	app.mu.Lock()
	man.Vanilla = true
	app.mu.Unlock()
	saveManifest()
	setPathsKey(game, "version", "vanilla")
	app.logf("Game: vanilla stable (what's on offer for now)")
}
