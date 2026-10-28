//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"

	"mmw/cmd/mmw-setup/help"
	"mmw/internal/plugins"
)

// WORLD: New Vegas or Tale of Two Wastelands, for anyone who has TTW (setup never gets or ships it).

func ttwOrderPath(game string) string { return filepath.Join(mmwDir(game), "ttw-loadorder.txt") }

// What FNV took out, kept for TTW: the load order it was taken from, TTW's plugins that came out, the whole order they
// stood in
type ttwKept struct {
	From       string
	Out, Order []string
}

func readKept(game string) ttwKept {
	var k ttwKept
	b, err := os.ReadFile(ttwOrderPath(game))
	if err != nil {
		return k
	}
	for _, l := range splitLines(string(b)) {
		l = strings.TrimSpace(l)
		switch {
		case strings.HasPrefix(l, "from="):
			k.From = strings.TrimPrefix(l, "from=")
		case strings.HasPrefix(l, "out="):
			for _, p := range strings.Split(strings.TrimPrefix(l, "out="), "|") {
				if p != "" {
					k.Out = append(k.Out, p)
				}
			}
		case l != "" && !strings.HasPrefix(l, "#"):
			k.Order = append(k.Order, l)
		}
	}
	return k
}

func writeKept(game string, k ttwKept) error {
	txt := "# setup's WORLD: what FNV took out of the load order, for TTW to put back\r\nfrom=" + k.From + "\r\nout=" + strings.Join(k.Out, "|") + "\r\n" + strings.Join(k.Order, "\r\n") + "\r\n"
	return putFile(ttwOrderPath(game), []byte(txt))
}

// ttwOn: TTW is in the game PLAY starts
func ttwOn(game string) bool {
	if findPluginFile(game, plugins.TTW) == "" {
		return false
	}
	lo := loadOrderFile(game)
	return !exists(lo) || plugins.Has(plugins.Read(lo), plugins.TTW)
}

// A WORLD pill was pressed
func worldPressed(name string) bool {
	var wantTTW bool
	switch name {
	case strings.ToLower(help.WorldFNV):
	case strings.ToLower(help.WorldTTW):
		wantTTW = true
	default:
		return false
	}
	app.mu.Lock()
	game, busy := app.game, app.busy
	app.mu.Unlock()
	if game == "" || busy || ttwOn(game) == wantTTW {
		return true
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then switch the world.", p)
		return true
	}
	if processRunning("ModOrganizer.exe") != "" && pickedMO2(game) != nil {
		app.status("Close Mod Organizer 2 first (it writes its load order back when it closes), then switch the world.")
		return true
	}
	lo := loadOrderFile(game)
	find := func(p string) string { return findPluginFile(game, p) }
	list := plugins.Read(lo)
	kept := readKept(game)
	if kept.From != "" && !strings.EqualFold(kept.From, lo) {
		kept = ttwKept{}
	}
	if !wantTTW {
		if !exists(lo) {
			loadOrder(game)
			list = plugins.Read(lo)
		}
		if len(list) == 0 {
			app.status("No load order found (%s): start the game once, then switch the world.", redact(lo))
			return true
		}
		set := plugins.TTWSet(list, find, officialMasters)
		q := "Play New Vegas without Tale of Two Wastelands?\n\nTTW's plugins (" + strings.Join(set, ", ") + ") come out of the load order. Nothing is deleted: TTW puts them back. Each world keeps its own saves.\n\nFriends playing with you pick FNV too."
		if messageBox(q, "World: FNV", 0x24) != 6 {
			return true
		}
		order := list
		if len(kept.Order) > 0 {
			order = plugins.Restore(kept.Order, list, func(string) bool { return true })
		}
		out := append(append([]string{}, kept.Out...), plugins.Without(set, kept.Out)...)
		if err := writeKept(game, ttwKept{From: lo, Out: out, Order: order}); err != nil {
			app.status("Couldn't keep TTW's load order: %v", err)
			return true
		}
		if err := putFile(lo, []byte(strings.Join(plugins.Without(list, set), "\r\n")+"\r\n")); err != nil {
			app.status("Couldn't change the load order: %v", err)
			return true
		}
		saveManifest()
		app.logf("World: New Vegas (%s out of %s)", strings.Join(set, ", "), redact(lo))
		app.status("World: New Vegas. PLAY starts FNV with its own saves.")
		netPills()
		return true
	}
	if find(plugins.TTW) == "" {
		for _, a := range loadAddons() {
			if !a.in(game) && addonProvides(a, plugins.TTW, game) {
				if messageBox("Put Tale of Two Wastelands in ("+a.Name+")?", "World: TTW", 0x24) == 6 {
					go func() {
						switchAddon(a, game, true)
						autoAddons(game)
						if find(plugins.TTW) != "" {
							worldPressed(name)
						}
						netPills()
					}()
				}
				return true
			}
		}
		messageBox("Tale of Two Wastelands isn't installed.\n\nTTW is free, made by its own team, and needs Fallout 3 as well as New Vegas: install it with its own installer (taleoftwowastelands.com) or your mod manager, then press TTW here.\n\nEveryone playing together needs the same world.", "World: TTW", 0x40)
		return true
	}
	order, out := kept.Order, kept.Out
	if !plugins.Has(out, plugins.TTW) {
		order = append(append([]string{}, officialMasters...), plugins.Masters(find(plugins.TTW))...)
		order = append(order, plugins.TTW)
		out = plugins.Without(order, officialMasters)
	}
	var back, missing []string
	for _, p := range out {
		if find(p) != "" {
			back = append(back, p)
		} else {
			missing = append(missing, p)
		}
	}
	newList := plugins.Restore(list, order, func(p string) bool { return plugins.Has(back, p) })
	if err := putFile(lo, []byte(strings.Join(newList, "\r\n")+"\r\n")); err != nil {
		app.status("Couldn't change the load order: %v", err)
		return true
	}
	if lo == pluginsTxtPath() {
		loadOrder(game)
	}
	if len(missing) > 0 {
		writeKept(game, ttwKept{From: lo, Out: missing, Order: order})
		app.logf("World: %s not found - kept to put back later", strings.Join(missing, ", "))
	} else {
		os.Remove(ttwOrderPath(game))
	}
	saveManifest()
	app.logf("World: Tale of Two Wastelands (%s back in %s)", strings.Join(back, ", "), redact(lo))
	app.status("World: Tale of Two Wastelands. PLAY starts TTW with its own saves.")
	netPills()
	return true
}
