//go:build windows

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"golang.org/x/sys/windows/registry"

	"mmw/cmd/mmw-setup/help"
	"mmw/cmd/mmw-setup/scene"
	"mmw/internal/plugins"
)

const (
	modsFolder = "GAME FOLDER"
	modsVortex = "VORTEX"
	modsMO2    = "MOD ORGANIZER 2"
	modsPickMO = "CHOOSE MODORGANIZER.EXE"
)

// A Mod Organizer 2 instance that manages this game
type mo2 struct {
	Name, Ini, Base, Profile, ModsDir, Exe string
	ProfilesDir, OverwriteDir              string
	Portable                               bool
}

func (m mo2) pluginsTxt() string { return filepath.Join(m.ProfilesDir, m.Profile, "plugins.txt") }

// The pill's label: one instance is MOD ORGANIZER 2, several are told apart by name
func (m mo2) label(n int) string {
	if n <= 1 {
		return modsMO2
	}
	return "MO2: " + strings.ToUpper(m.Name)
}

func mo2Value(s, key string) string {
	v := iniValue(s, key)
	v = strings.Trim(strings.TrimSuffix(strings.TrimPrefix(v, "@ByteArray("), ")"), `"`)
	return strings.ReplaceAll(v, `\\`, `\`)
}

// mo2Instances: the Mod Organizer 2 instances whose game is this one (global ones in %LOCALAPPDATA%\ModOrganizer,
// portable ones in the usual folders)
func mo2Instances(game string) []mo2 {
	var inis []string
	if m, _ := filepath.Glob(filepath.Join(os.Getenv("LOCALAPPDATA"), "ModOrganizer", "*", "ModOrganizer.ini")); len(m) > 0 {
		inis = append(inis, m...)
	}
	for _, d := range drives() {
		for _, sub := range []string{"Modding", "MO2", "Mod Organizer 2", "Games", `Program Files\Mod Organizer 2`, `Program Files\Mod Organizer`} {
			if m, _ := filepath.Glob(filepath.Join(d, sub, "ModOrganizer.ini")); len(m) > 0 {
				inis = append(inis, m...)
			}
			if m, _ := filepath.Glob(filepath.Join(d, sub, "*", "ModOrganizer.ini")); len(m) > 0 {
				inis = append(inis, m...)
			}
		}
	}
	var out []mo2
	seen := map[string]bool{}
	for _, ini := range inis {
		if seen[strings.ToLower(ini)] {
			continue
		}
		seen[strings.ToLower(ini)] = true
		b, err := os.ReadFile(ini)
		if err != nil {
			continue
		}
		s := string(b)
		gp := mo2Value(s, "gamePath")
		if gp == "" || !strings.EqualFold(filepath.Clean(gp), filepath.Clean(game)) {
			continue
		}
		dir := filepath.Dir(ini)
		m := mo2{Name: filepath.Base(dir), Ini: ini, Base: dir, Profile: mo2Value(s, "selected_profile")}
		if bd := mo2Value(s, "base_directory"); bd != "" {
			m.Base = bd
		}
		dirOf := func(key, def string) string {
			if v := mo2Value(s, key); v != "" {
				return filepath.Clean(strings.ReplaceAll(v, "%BASE_DIR%", m.Base))
			}
			return filepath.Join(m.Base, def)
		}
		m.ModsDir, m.ProfilesDir, m.OverwriteDir = dirOf("mod_directory", "mods"), dirOf("profiles_directory", "profiles"), dirOf("overwrite_directory", "overwrite")
		if m.Profile == "" {
			m.Profile = "Default"
		}
		m.Portable = exists(filepath.Join(dir, "ModOrganizer.exe"))
		m.Exe = mo2Exe(dir)
		out = append(out, m)
	}
	return out
}

// mo2Exe: ModOrganizer.exe - beside a portable instance's ini, else where Mod Organizer handles Nexus links from, else
// where the player showed it (YOUR MODS: CHOOSE MODORGANIZER.EXE)
func mo2Exe(dir string) string {
	if p := filepath.Join(dir, "ModOrganizer.exe"); exists(p) {
		return p
	}
	cmd := regString(registry.CURRENT_USER, `Software\Classes\nxm\shell\open\command`, "")
	if cmd != "" {
		exe := strings.Trim(strings.SplitN(strings.TrimPrefix(cmd, `"`), `"`, 2)[0], " ")
		if p := filepath.Join(filepath.Dir(exe), "ModOrganizer.exe"); exists(p) {
			return p
		}
	}
	app.mu.Lock()
	chosen := man.ModsExe
	app.mu.Unlock()
	if exists(chosen) {
		return chosen
	}
	return ""
}

// The instance picked (nil: none)
func pickedMO2(game string) *mo2 {
	app.mu.Lock()
	pick := man.Mods
	app.mu.Unlock()
	ini, ok := strings.CutPrefix(pick, "mo2:")
	if !ok {
		return nil
	}
	for _, m := range mo2Instances(game) {
		if strings.EqualFold(m.Ini, ini) {
			m := m
			return &m
		}
	}
	return nil
}

func vortexThere() bool { return exists(filepath.Join(os.Getenv("APPDATA"), "Vortex")) }

// Vortex has deployed into this game (it keeps a record of what it put in Data)
func vortexDeployed(game string) bool {
	m, _ := filepath.Glob(filepath.Join(dataDir(game), "vortex.deployment*.json"))
	return len(m) > 0
}

// modsLabel: the right column's YOUR MODS pill
func modsLabel(game string) string {
	app.mu.Lock()
	pick := man.Mods
	app.mu.Unlock()
	switch {
	case pick == "vortex":
		return modsVortex
	case strings.HasPrefix(pick, "mo2:") && pickedMO2(game) != nil:
		return modsMO2
	}
	return modsFolder
}

// loadOrderFile: the plugins.txt the game will start with - Mod Organizer's profile's when PLAY goes through it
func loadOrderFile(game string) string {
	if m := pickedMO2(game); m != nil {
		return m.pluginsTxt()
	}
	return pluginsTxtPath()
}

// findPluginFile: where a plugin's file is - in Data, or (Mod Organizer picked) in one of its profile's mods that are
// on
func findPluginFile(game, name string) string {
	if p := filepath.Join(dataDir(game), name); exists(p) {
		return p
	}
	m := pickedMO2(game)
	if m == nil {
		return ""
	}
	if p := filepath.Join(m.OverwriteDir, name); exists(p) {
		return p
	}
	b, err := os.ReadFile(filepath.Join(m.ProfilesDir, m.Profile, "modlist.txt"))
	if err != nil {
		return ""
	}
	for _, l := range splitLines(string(b)) {
		if mod, ok := strings.CutPrefix(strings.TrimSpace(l), "+"); ok {
			if p := filepath.Join(m.ModsDir, mod, name); exists(p) {
				return p
			}
		}
	}
	return ""
}

// The YOUR MODS panel, as things stand
func modsLines(game string) []scene.Line {
	app.mu.Lock()
	pick := man.Mods
	app.mu.Unlock()
	ms := mo2Instances(game)
	v := help.ModsView{Pills: []string{modsFolder}, On: []bool{pick == ""}}
	for _, m := range ms {
		v.Pills, v.On = append(v.Pills, m.label(len(ms))), append(v.On, strings.EqualFold(pick, "mo2:"+m.Ini))
	}
	if vortexThere() {
		v.Pills, v.On = append(v.Pills, modsVortex), append(v.On, pick == "vortex")
	}
	switch m := pickedMO2(game); {
	case m != nil:
		v.Say = "Mod Organizer 2 (" + m.Name + ", profile " + m.Profile + "): PLAY starts the game through it, with every mod that profile has on. Mojave Enhanced is switched on in its load order."
		if m.Exe == "" {
			v.Say += " ModOrganizer.exe wasn't found: show setup where it is."
			v.PickExe = modsPickMO
		}
	case pick == "vortex" && vortexDeployed(game):
		v.Say = "Vortex has deployed its mods into the game folder, so PLAY has them all. After changing mods in Vortex, press Deploy Mods there before playing."
	case pick == "vortex":
		v.Say = "Vortex hasn't deployed any mods into this game yet: press Deploy Mods in Vortex, then PLAY."
	default:
		v.Say = help.ModsFolderSay
	}
	v.NoneFound = len(ms) == 0 && !vortexThere()
	return help.ModsLines(v)
}

// A YOUR MODS pill was pressed
func modsPressed(name string) bool {
	app.mu.Lock()
	game := app.game
	app.mu.Unlock()
	ms := mo2Instances(game)
	pick := ""
	switch name {
	case strings.ToLower(modsFolder):
	case strings.ToLower(modsVortex):
		pick = "vortex"
	case strings.ToLower(modsPickMO):
		if p := openFileDialog("Where is ModOrganizer.exe?", "ModOrganizer.exe\x00ModOrganizer.exe\x00\x00"); p != "" {
			app.mu.Lock()
			man.ModsExe = p
			app.mu.Unlock()
			saveManifest()
		}
		showInfo("mods")
		return true
	default:
		found := false
		for _, m := range ms {
			if name == strings.ToLower(m.label(len(ms))) {
				pick, found = "mo2:"+m.Ini, true
			}
		}
		if !found {
			return false
		}
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then pick where your mods are.", p)
		return true
	}
	app.mu.Lock()
	man.Mods = pick
	app.mu.Unlock()
	modManagers(game, true)
	saveManifest()
	switch {
	case strings.HasPrefix(pick, "mo2:"):
		app.status("Your mods: Mod Organizer 2 - PLAY starts the game through it.")
	case pick == "vortex":
		app.status("Your mods: Vortex's, deployed into the game folder.")
	default:
		app.status("Your mods: the ones in the game folder.")
	}
	app.logf("Your mods: %s", map[bool]string{true: pick, false: "the game folder"}[pick != ""])
	netPills()
	showInfo("mods")
	return true
}

// playThroughMO2: the launcher started by Mod Organizer 2 (its profile's mods in the game); false when it couldn't be
func playThroughMO2(game string) bool {
	m := pickedMO2(game)
	if m == nil {
		return false
	}
	if m.Exe == "" {
		app.status("ModOrganizer.exe wasn't found: show setup where it is (YOUR MODS).")
		return true
	}
	launcher := filepath.Join(mmwDir(game), "mmw-launch.exe")
	args := []string{"-i", m.Name}
	if m.Portable {
		args = []string{"-i", ""}
	}
	args = append(args, "-p", m.Profile, "run", "-a", `--fnv "`+game+`"`, "-c", mmwDir(game), launcher)
	cmd := exec.Command(m.Exe, args...)
	cmd.Dir = filepath.Dir(m.Exe)
	cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x08000000}
	app.button("STARTING", scene.BtnWorking, nil)
	app.status("Starting Fallout: New Vegas through Mod Organizer 2 (profile %s)...", m.Profile)
	app.logf("Mod Organizer 2: %s %s", m.Exe, strings.Join(args, " "))
	if err := cmd.Start(); err != nil {
		app.status("Couldn't start Mod Organizer 2: %v", err)
		app.button("PLAY", scene.BtnDone, func() { go play() })
		return true
	}
	go cmd.Wait()
	for i := 0; i < 180; i++ {
		time.Sleep(time.Second)
		if processRunning("FalloutNV.exe") != "" {
			app.status("Have fun! (This window closes by itself.)")
			time.Sleep(4 * time.Second)
			pPostMessageW.Call(hwndMain, wmClose, 0, 0)
			return true
		}
	}
	app.status("The game didn't start through Mod Organizer 2: see its window (or pick GAME FOLDER in YOUR MODS).")
	app.button("PLAY", scene.BtnDone, func() { go play() })
	return true
}

// mo2PluginOn: our plugin on in an instance's profile.
func mo2PluginOn(m mo2) {
	pt := m.pluginsTxt()
	b, err := os.ReadFile(pt)
	if err != nil {
		return
	}
	var out []string
	had := false
	for _, l := range splitLines(string(b)) {
		t := strings.TrimSpace(l)
		if strings.EqualFold(t, "*"+espName) || strings.EqualFold(t, espName) && had {
			continue
		}
		if strings.EqualFold(t, espName) {
			had = true
		}
		if t != "" {
			out = append(out, t)
		}
	}
	if !had {
		out = append(out, espName)
	}
	nb := strings.Join(out, "\r\n") + "\r\n"
	if nb != string(b) && putFile(pt, []byte(nb)) == nil {
		app.logf("Mod Organizer 2: %s on in profile %q", espName, m.Profile)
	}
	lo := filepath.Join(filepath.Dir(pt), "loadorder.txt")
	if l := plugins.Read(lo); l != nil && !plugins.Has(l, espName) {
		putFile(lo, []byte(strings.Join(append(l, espName), "\r\n")+"\r\n"))
	}
}
