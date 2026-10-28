package main

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"mmw/internal/plugins"
)

// The game the player picked in setup (paths.ini version=mmw|vanilla) and the world that's in.
type profile struct {
	vanilla bool
	ttw     bool
}

func readProfile(data string, paths map[string]string) profile {
	return profile{
		vanilla: strings.EqualFold(paths["version"], "vanilla"),
		ttw:     ttwIn(data),
	}
}

// TTW is in while its plugin is there and in the load order: setup's FNV takes it out of plugins.txt and leaves the
// files.
func ttwIn(data string) bool {
	if !exists(filepath.Join(data, plugins.TTW)) {
		return false
	}
	lo := filepath.Join(os.Getenv("LOCALAPPDATA"), "FalloutNV", "plugins.txt")
	return !exists(lo) || plugins.Has(plugins.Read(lo), plugins.TTW)
}

func (p profile) String() string {
	s := "Mojave Enhanced"
	if p.vanilla {
		s = "vanilla stable"
	}
	if p.ttw {
		s += " + Tale of Two Wastelands"
	}
	return s
}

// The save folder under My Games\FalloutNV (the game's SLocalSavePath)
func (p profile) saves() string {
	if p.ttw {
		return `Saves\TTW\`
	}
	return `Saves\`
}

// The tuning a vanilla stable game runs with (mmw.ini overrides of sheets/tuning.json): our performance and reliability
// work, gun inertia, contextual aiming and recoil stay; everything else of ours is off.
var vanillaTuning = []string{
	"reloadFeel=0", "chamberRound=0", "dropMagLosesRounds=0",
	"pistolFireAnimSpeed=1",
	"barrelFollow=0", "spreadWalkDeg=0", "spreadSprintDeg=0", "spreadAirDeg=0",
	"vibeOn=0",
}

var ourSaves = map[string]bool{`saves\ttw vanilla\`: true, `saves\ttw\`: true, `saves\vanilla\`: true}

// iniValue reads [sec] key from an ini file ("" when it isn't there)
func iniValue(path, sec, key string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	in := sec == ""
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			in = strings.EqualFold(t[1:len(t)-1], sec)
			continue
		}
		if k, v, ok := strings.Cut(t, "="); in && ok && strings.EqualFold(strings.TrimSpace(k), key) {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

// customIni: My Games\FalloutNV\FalloutCustom.ini - JIP LN NVSE reads it after Fallout.ini and FalloutPrefs.ini, so its
// keys win over both ("" when the folder isn't there).
func customIni() string {
	if d := documentsDir(); d != "" {
		dir := filepath.Join(d, "My Games", "FalloutNV")
		if fi, err := os.Stat(dir); err == nil && fi.IsDir() {
			return filepath.Join(dir, "FalloutCustom.ini")
		}
	}
	return ""
}

// gameIniValue: [sec] key as the game ends up with it - FalloutCustom.ini over FalloutPrefs.ini over Fallout.ini
func gameIniValue(sec, key string) (string, bool) {
	c := customIni()
	if c == "" {
		return "", false
	}
	dir := filepath.Dir(c)
	for _, f := range []string{"FalloutCustom.ini", "FalloutPrefs.ini", "Fallout.ini"} {
		if p := filepath.Join(dir, f); iniHas(p, sec, key) {
			return iniValue(p, sec, key), true
		}
	}
	return "", false
}

// gameIniPut sets [sec] key=val where the game takes it from
func gameIniPut(sec, key, val string) bool {
	c := customIni()
	if c == "" {
		return false
	}
	if !exists(c) {
		os.WriteFile(c, []byte("; Mojave Enhanced and its add-ons keep their game settings here (read by JIP LN NVSE after Fallout.ini and FalloutPrefs.ini)\r\n"), 0o644)
	}
	if !iniPut(c, sec, key, val) {
		return false
	}
	for _, f := range []string{"FalloutPrefs.ini", "Fallout.ini"} {
		if p := filepath.Join(filepath.Dir(c), f); iniHas(p, sec, key) && iniValue(p, sec, key) != val {
			if iniSet(p, sec, key, val) != nil {
				log.Printf("%s is read-only or locked: [%s] %s left there (FalloutCustom.ini's wins)", f, sec, key)
			}
		}
	}
	return true
}

// iniPut sets [sec] key=val in an ini file, keeping every other line; false if the file can't be read or written
func iniPut(path, sec, key, val string) bool {
	err := iniSet(path, sec, key, val)
	if err != nil {
		log.Printf("%s: %v", path, err)
	}
	return err == nil
}

func iniSet(path, sec, key, val string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	text := strings.ReplaceAll(string(b), "\r\n", "\n")
	lines := strings.Split(strings.TrimSuffix(text, "\n"), "\n")
	secAt, keyAt, end := -1, -1, len(lines)
	for i, l := range lines {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			if secAt >= 0 {
				end = i
				break
			}
			if strings.EqualFold(t[1:len(t)-1], sec) {
				secAt = i
			}
			continue
		}
		if secAt >= 0 {
			if k, _, ok := strings.Cut(t, "="); ok && strings.EqualFold(strings.TrimSpace(k), key) {
				keyAt = i
			}
		}
	}
	switch {
	case keyAt >= 0:
		if strings.TrimSpace(lines[keyAt]) == key+"="+val {
			return nil
		}
		lines[keyAt] = key + "=" + val
	case secAt >= 0:
		lines = append(lines[:end], append([]string{key + "=" + val}, lines[end:]...)...)
	default:
		lines = append(lines, "["+sec+"]", key+"="+val)
	}
	nb := []byte(strings.Join(lines, "\r\n") + "\r\n")
	if err := writeKeepingReadOnly(path, nb); err != nil {
		return err
	}
	if got, err := os.ReadFile(path); err != nil || string(got) != string(nb) {
		return fmt.Errorf("the change didn't land (read back different)")
	}
	return nil
}

// writeKeepingReadOnly writes a file the player may have made read-only (a common New Vegas tip for Fallout.ini, so the
// game's own launcher can't reset it)
func writeKeepingReadOnly(path string, b []byte) error {
	err := os.WriteFile(path, b, 0o644)
	if err == nil {
		return nil
	}
	fi, serr := os.Stat(path)
	if serr != nil || fi.Mode().Perm()&0o200 != 0 {
		return err
	}
	if err := os.Chmod(path, 0o666); err != nil {
		return err
	}
	err = os.WriteFile(path, b, 0o644)
	os.Chmod(path, fi.Mode().Perm())
	return err
}

// applyProfile: the save folder, our plugin in or out of the load order, our animation sets in or out
func applyProfile(p profile, data, managed string) error {
	saves := p.saves()
	ini := customIni()
	if ini == "" && p.ttw {
		return fmt.Errorf("Documents\\My Games\\FalloutNV isn't there, so %s can't get its own saves: start New Vegas once from Steam, then press Play again", p)
	}
	if ini != "" {
		basePath := filepath.Join(managed, "saves-base.txt")
		base := `Saves\`
		if b, err := os.ReadFile(basePath); err == nil && strings.TrimSpace(string(b)) != "" {
			base = strings.TrimSpace(string(b))
		} else if cur, _ := gameIniValue("General", "SLocalSavePath"); cur != "" && !ourSaves[strings.ToLower(cur)] {
			base = cur
			os.WriteFile(basePath, []byte(base+"\r\n"), 0o644)
		}
		if !p.ttw {
			saves = base
		}
		ok := gameIniPut("General", "SLocalSavePath", saves)
		for try := 0; !ok && try < 5; try++ {
			time.Sleep(400 * time.Millisecond)
			ok = gameIniPut("General", "SLocalSavePath", saves)
		}
		switch {
		case !ok && p.ttw:
			return fmt.Errorf("couldn't set the save folder in %s, so %s can't get its own saves", ini, p)
		case !ok:
			log.Printf("saves: couldn't set the save folder in %s - this time %s uses the folder already set", ini, p)
			saves, _ = gameIniValue("General", "SLocalSavePath")
		}
		os.MkdirAll(filepath.Join(filepath.Dir(ini), filepath.FromSlash(strings.ReplaceAll(saves, `\`, "/"))), 0o755)
		sortSaves(filepath.Dir(ini), base)
	}
	log.Printf("game: %s (saves in My Games\\FalloutNV\\%s)", p, saves)
	ago := filepath.Join(data, "meshes", "AnimGroupOverride")
	on, off := filepath.Join(ago, "MojaveMostWanted.json"), filepath.Join(ago, "MojaveMostWanted.json.off")
	if p.vanilla && exists(on) {
		os.Remove(off)
		if os.Rename(on, off) == nil {
			log.Print("animations: our sets parked (vanilla stable)")
		}
	} else if !p.vanilla && exists(off) && !exists(on) {
		if os.Rename(off, on) == nil {
			log.Print("animations: our sets back")
		}
	}
	return nil
}

// restoreSaves (undo everything): the save folder back to the player's own
func restoreSaves(managed string) {
	c := customIni()
	if c == "" {
		return
	}
	base := `Saves\`
	if b, err := os.ReadFile(filepath.Join(managed, "saves-base.txt")); err == nil && strings.TrimSpace(string(b)) != "" {
		base = strings.TrimSpace(string(b))
	}
	if cur, _ := gameIniValue("General", "SLocalSavePath"); cur != base && gameIniPut("General", "SLocalSavePath", base) {
		log.Printf("saves: back in My Games\\FalloutNV\\%s", base)
	}
	os.Remove(filepath.Join(managed, "saves-base.txt"))
}

// Smooth ADS (the camera easing onto the sights
func smoothAds(p profile, pluginDir, data string) (bool, float64) {
	key := "mmw"
	if p.vanilla {
		key = "vanilla"
	}
	on := !p.vanilla
	if b, err := os.ReadFile(filepath.Join(pluginDir, "smoothads.ini")); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if k, v, ok := strings.Cut(strings.TrimSpace(l), "="); ok && strings.EqualFold(k, key) {
				on = strings.TrimSpace(v) == "1"
			}
		}
	}
	sec := 0.45
	st := filepath.Join(data, "NVSE", "Plugins", "nvse_stewie_tweaks.ini")
	if exists(st) {
		iniPut(st, "Tweaks", "bSmoothIronsightsCameraTransition", "1")
		if ms, err := strconv.Atoi(iniValue(st, "Smooth Iron Sights Camera", "iAimTransitionTimeMS")); err == nil && ms > 0 {
			sec = float64(ms) / 1000
		}
	}
	log.Printf("smooth ADS: %v (camera easing %.2f s when on)", on, sec)
	return on, sec
}

// alwaysActive: while playing together (setup's net.ini: hosting or joining) the game keeps running when its window
// isn't in front (Fallout.ini [General] bAlwaysActive=1) -.
func alwaysActive(pluginDir, managed string) {
	if customIni() == "" {
		return
	}
	mode := strings.ToLower(iniValue(filepath.Join(pluginDir, "net.ini"), "", "mode"))
	keep := filepath.Join(managed, "alwaysactive-before.txt")
	together := mode == "host" || mode == "join"
	cur, _ := gameIniValue("General", "bAlwaysActive")
	switch {
	case together && cur != "1":
		if !exists(keep) {
			os.WriteFile(keep, []byte(cur+"\r\n"), 0o644)
		}
		if gameIniPut("General", "bAlwaysActive", "1") {
			log.Print("playing together: the game keeps running behind other windows (bAlwaysActive=1)")
		}
	case !together && exists(keep):
		b, _ := os.ReadFile(keep)
		before := strings.TrimSpace(string(b))
		if before == "" {
			before = "0"
		}
		if gameIniPut("General", "bAlwaysActive", before) {
			os.Remove(keep)
			log.Printf("playing alone: bAlwaysActive back to %s", before)
		}
	}
}
