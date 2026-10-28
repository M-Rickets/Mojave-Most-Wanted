//go:build windows

package main

import (
	"strings"

	"mmw/cmd/mmw-setup/scene"
)

// Which game PLAY starts, picked at the top of the add-ons panel: Mojave Most Wanted (everything) or vanilla stable.

const (
	verGroup   = "Game"
	verMMW     = "MOJAVE MOST WANTED"
	verVanilla = "VANILLA STABLE"
)

// The version pills, first in the panel
func versionItems() (labels []string, on []bool, groups []string) {
	return []string{verMMW, verVanilla}, []bool{!man.Vanilla, man.Vanilla}, []string{verGroup, verGroup}
}

// A version pill was pressed: true if it was one
func versionPressed(name string) bool {
	var vanilla bool
	switch name {
	case strings.ToLower(verMMW):
	case strings.ToLower(verVanilla):
		vanilla = true
	default:
		return false
	}
	app.mu.Lock()
	game, busy := app.game, app.busy
	isVanilla := man.Vanilla
	app.mu.Unlock()
	if game == "" || busy || vanilla == isVanilla {
		return true
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then switch the game.", p)
		return true
	}
	var out []addon
	if vanilla {
		for _, a := range loadAddons() {
			if a.in(game) && !a.KeepInVanilla && !a.Hidden {
				out = append(out, a)
			}
		}
	}
	q := "Play vanilla stable?\n\nNew Vegas as it shipped, with the engine fixes and our gun handling (inertia, contextual aiming, recoil). Smooth ADS is off until you press O in game. No cars, snow, dark nights, radio or animation sets."
	if len(out) > 0 {
		q += "\n\nThe add-ons that are in come out (" + addonNames(out) + "); they go back in when you pick Mojave Enhanced again. That can take a few minutes."
	}
	q += "\n\nYour saves stay: both games use the same ones."
	if !vanilla {
		q = "Play Mojave Enhanced's full game again?\n\nEverything comes back: cars, snow, nights, radio, the animation sets"
		if len(man.VanillaOff) > 0 {
			q += " and the add-ons vanilla stable took out (" + strings.Join(man.VanillaOff, ", ") + ")"
		}
		q += ". Your saves are where you left them."
	}
	if messageBox(q, "Mojave Enhanced", 0x34) != 6 {
		return true
	}
	go switchVersion(game, vanilla, out)
	return true
}

func addonNames(as []addon) string {
	var n []string
	for _, a := range as {
		n = append(n, a.Name)
	}
	return strings.Join(n, ", ")
}

func switchVersion(game string, vanilla bool, out []addon) {
	app.mu.Lock()
	app.busy = true
	app.mu.Unlock()
	app.button("WORKING", scene.BtnWorking, nil)
	app.progress(-1)
	failed := 0
	app.mu.Lock()
	offList := append([]string{}, man.VanillaOff...)
	app.mu.Unlock()
	if vanilla {
		for _, a := range out {
			app.status("Taking %s out...", a.Name)
			if runAddon(a, game, a.Off) && !a.in(game) {
				offList = append(offList, a.Name)
			} else {
				failed++
			}
		}
	} else {
		var still []string
		for _, n := range offList {
			back := false
			for _, a := range loadAddons() {
				if a.Name != n {
					continue
				}
				if a.in(game) {
					back = true
					break
				}
				app.status("Putting %s back...", a.Name)
				back = runAddon(a, game, a.On) && a.in(game)
				break
			}
			if !back {
				still = append(still, n)
				failed++
			}
		}
		offList = still
	}
	app.mu.Lock()
	man.VanillaOff, man.Vanilla = offList, vanilla
	app.mu.Unlock()
	saveManifest()
	setPathsKey(game, "version", map[bool]string{true: "vanilla"}[vanilla])
	app.mu.Lock()
	app.busy = false
	app.mu.Unlock()
	autoAddons(game)
	app.mu.Lock()
	app.busy = true
	app.mu.Unlock()
	app.logf("Game: %s", map[bool]string{true: "vanilla stable", false: "Mojave Enhanced (full)"}[vanilla])
	app.mu.Lock()
	app.busy = false
	app.mu.Unlock()
	app.progress(1)
	app.button("PLAY", scene.BtnDone, func() { go play() })
	switch {
	case failed > 0:
		app.status("Switched, but %d add-on(s) didn't switch: see details.", failed)
	case vanilla:
		app.status("Vanilla stable: PLAY starts New Vegas with the fixes and our gun handling only.")
	default:
		app.status("Mojave Enhanced: PLAY starts everything again.")
	}
	netPills()
}
