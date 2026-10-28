//go:build windows

package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"

	"mmw/cmd/mmw-setup/scene"
	"mmw/internal/steam"

	"golang.org/x/sys/windows/registry"
)

// payload.zip: our own files only, made by tools/build_setup.sh
//
//go:embed payload.zip
var payload []byte

const espName = "MojaveMostWanted.esp"

// ---- The undo record: everything setup added or replaced, so "undo everything" can put the game back
type Manifest struct {
	Version    string            `json:"version"`
	Game       string            `json:"game"`
	Added      []string          `json:"added"`
	Replaced   map[string]string `json:"replaced"`
	Dirs       []string          `json:"dirs"`
	When       string            `json:"when"`
	NoCars     bool              `json:"noCars"`
	Vanilla    bool              `json:"vanilla"`
	VanillaOff []string          `json:"vanillaOff"`
	Unlocked   []string          `json:"unlocked"`
	Mods       string            `json:"mods"`
	ModsExe    string            `json:"modsExe"`
	IntroSeen  bool              `json:"introSeen"`
}

var man Manifest

func undoDir(game string) string   { return filepath.Join(game, "MojaveMostWanted-undo") }
func mmwDir(game string) string    { return filepath.Join(game, "MojaveMostWanted") }
func dataDir(game string) string   { return filepath.Join(game, "Data") }
func pluginDir(game string) string { return filepath.Join(game, "Data", "NVSE", "Plugins") }

func loadManifest(game string) bool {
	b, err := os.ReadFile(filepath.Join(undoDir(game), "undo.json"))
	if err != nil {
		man = Manifest{Game: game, Replaced: map[string]string{}}
		return false
	}
	man = Manifest{}
	if json.Unmarshal(b, &man) != nil || man.Replaced == nil {
		man.Replaced = map[string]string{}
	}
	man.Game = game
	return true
}

func saveManifest() error {
	man.Version, man.When = appVersion, time.Now().Format(time.RFC3339)
	if err := os.MkdirAll(undoDir(man.Game), 0o755); err != nil {
		return err
	}
	b, _ := json.MarshalIndent(man, "", " ")
	return os.WriteFile(filepath.Join(undoDir(man.Game), "undo.json"), b, 0o644)
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if strings.EqualFold(x, s) {
			return true
		}
	}
	return false
}

// Mkdirs makes a folder and remembers the ones setup made (removed again on undo when empty)
func mkdirs(dir string) error {
	var made []string
	for d := dir; d != "" && !exists(d); d = filepath.Dir(d) {
		made = append(made, d)
		if filepath.Dir(d) == d {
			break
		}
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	for _, d := range made {
		if !contains(man.Dirs, d) {
			man.Dirs = append(man.Dirs, d)
		}
	}
	return nil
}

// putFile writes a file the undo record knows about: a file that was there before is copied aside first
func putFile(dst string, data []byte) error {
	if old, err := os.ReadFile(dst); err == nil {
		if bytes.Equal(old, data) {
			return nil
		}
		if _, known := man.Replaced[dst]; !known && !contains(man.Added, dst) {
			rel := strings.TrimPrefix(strings.ReplaceAll(dst, ":", ""), `\`)
			bk := filepath.Join(undoDir(man.Game), "originals", rel)
			if err := os.MkdirAll(filepath.Dir(bk), 0o755); err != nil {
				return err
			}
			if err := os.WriteFile(bk, old, 0o644); err != nil {
				return err
			}
			man.Replaced[dst] = bk
		}
	} else if !contains(man.Added, dst) {
		man.Added = append(man.Added, dst)
	}
	if err := mkdirs(filepath.Dir(dst)); err != nil {
		return err
	}
	tmp := dst + ".mmwtmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	os.Remove(dst)
	return os.Rename(tmp, dst)
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

// ---- start: is it in already?
func startup() {
	app.logf("Mojave Enhanced Setup %s on %s", appVersion, windowsVersion())
	go checkUpdate()
	game := findGame(false)
	if game == "" {
		app.status("Ready when you are. Make sure Fallout: New Vegas is closed.")
		return
	}
	app.mu.Lock()
	app.game = game
	app.mu.Unlock()
	if fm := filepath.Join(knownFolder(folderLocalAppData), "MojaveMostWanted", "friends-mode.json"); exists(fm) {
		os.Remove(fm)
		app.logf("Friends mode is gone: the game stays as it is (the dev files it kept aside are in %s)", filepath.Join(mmwDir(game), "dev-backup"))
	}
	if loadManifest(game) && exists(filepath.Join(pluginDir(game), "MojaveMostWanted.dll")) && exists(filepath.Join(mmwDir(game), "mmw-launch.exe")) {
		app.mu.Lock()
		app.installed = true
		app.mu.Unlock()
		app.links()
		for i := 0; i < 5; i++ {
			app.step(i, scene.Done)
		}
		app.progress(1)
		app.button("PLAY", scene.BtnDone, func() { go play() })
		app.status("All set. Press PLAY (new here? FIRST TIME START-UP).")
		netPills()
		autoAddons(game)
		unpackInBackground(game)
		if *flagPlay {
			waitUpdate(6 * time.Second)
			if applyPendingUpdate(true) {
				return
			}
			app.status("All set. Press PLAY.")
		}
		return
	}
	if *flagResume {
		go install()
	}
}

// ---- MAKE IT WORK
func install() {
	app.mu.Lock()
	if app.busy {
		app.mu.Unlock()
		return
	}
	app.busy = true
	app.mu.Unlock()
	defer func() {
		app.mu.Lock()
		app.busy = false
		app.mu.Unlock()
		app.with(func(s *scene.State) { s.CarMoves = false })
		if r := recover(); r != nil {
			app.logf("setup stopped: %v", r)
			app.status("Something went wrong. Open details (top right), copy it all and send it.")
			app.button("TRY AGAIN", scene.BtnReady, func() { go install() })
		}
	}()
	app.button("WORKING", scene.BtnWorking, nil)
	app.with(func(s *scene.State) { s.CarMoves = true; s.DoneT = -1; s.StepSt = [5]scene.StepState{} })
	fail := func(i int, msg string) {
		app.step(i, scene.Warn)
		app.status("%s", msg)
		app.button("TRY AGAIN", scene.BtnReady, func() { go install() })
	}
	app.step(0, scene.Active)
	app.progress(0.02)
	game := findGame(true)
	if game == "" {
		game = waitForGame()
	}
	if game == "" {
		fail(0, "Couldn't find Fallout: New Vegas. Put this program in the game's folder (where FalloutNV.exe is) and run it from there.")
		return
	}
	if p := processRunning("FalloutNV.exe", "nvse_loader.exe", "mmw-launch.exe"); p != "" {
		fail(0, "Close Fallout: New Vegas first ("+p+" is running), then press the button again.")
		return
	}
	if !canWrite(game) {
		if !isAdmin() {
			app.status("The game's folder needs permission: say Yes when Windows asks.")
			exe, _ := os.Executable()
			if err := shellExecute("runas", exe, fmt.Sprintf(`--game "%s" --resume`, game), "", 1); err == nil {
				time.Sleep(500 * time.Millisecond)
				os.Exit(0)
			}
		}
		fail(0, "Needs permission for the game's folder: press the button again and click Yes when Windows asks.")
		return
	}
	loadManifest(game)
	app.mu.Lock()
	app.game = game
	app.mu.Unlock()
	app.logf("Fallout: New Vegas: %s (FalloutNV.exe %s, %s)", redact(game), fileVersion(filepath.Join(game, "FalloutNV.exe")), laaText(filepath.Join(game, "FalloutNV.exe")))
	app.step(0, scene.Done)
	app.step(1, scene.Active)
	if err := getTools(game); err != nil {
		saveManifest()
		fail(1, err.Error())
		return
	}
	app.step(1, scene.Done)
	saveManifest()
	app.step(2, scene.Active)
	app.progress(0.5)
	if err := putIn(game); err != nil {
		saveManifest()
		fail(2, "Couldn't put the mod in: "+err.Error())
		return
	}
	saveManifest()
	app.mu.Lock()
	app.installed = true
	app.mu.Unlock()
	app.links()
	app.step(2, scene.Done)
	app.step(3, scene.Active)
	mw, err := buildCars(game)
	if err != nil {
		saveManifest()
		fail(3, err.Error())
		return
	}
	_ = mw
	app.step(3, scene.Done)
	app.step(4, scene.Active)
	app.progress(0.97)
	if msg, ok := verify(game); !ok {
		fail(4, msg)
		return
	}
	saveManifest()
	app.step(4, scene.Done)
	app.progress(1)
	app.with(func(s *scene.State) { s.DoneT = time.Since(startT).Seconds(); s.CarMoves = false })
	app.button("ALL SET!", scene.BtnDone, nil)
	app.status("All set! Press PLAY (or use the Mojave Enhanced shortcut on your desktop).")
	go func() {
		time.Sleep(2500 * time.Millisecond)
		app.button("PLAY", scene.BtnDone, func() { go play() })
		netPills()
		if !man.IntroSeen {
			man.IntroSeen = true
			saveManifest()
			showInfo("first")
		}
	}()
}

func canWrite(game string) bool {
	for _, d := range []string{game, dataDir(game)} {
		p := filepath.Join(d, ".mmw-write-test")
		if err := os.WriteFile(p, []byte("x"), 0o644); err != nil {
			return false
		}
		os.Remove(p)
	}
	return true
}

// ---- Finding the game
func findGame(loud bool) string {
	ok := func(p string) bool { return p != "" && exists(filepath.Join(p, "FalloutNV.exe")) }
	exe, _ := os.Executable()
	here := filepath.Dir(exe)
	cands := []string{strings.Trim(*flagGame, `" `), here, filepath.Dir(here)}
	cands = append(cands, steam.Find("22380", "Fallout New Vegas"))
	for _, k := range []string{`SOFTWARE\WOW6432Node\GOG.com\Games\1454587428`, `SOFTWARE\GOG.com\Games\1454587428`} {
		if v := regString(registry.LOCAL_MACHINE, k, "path"); v != "" {
			cands = append(cands, v)
		}
	}
	if ms, _ := filepath.Glob(`C:\ProgramData\Epic\EpicGamesLauncher\Data\Manifests\*.item`); len(ms) > 0 {
		for _, m := range ms {
			b, _ := os.ReadFile(m)
			var it struct{ DisplayName, InstallLocation string }
			if json.Unmarshal(b, &it) == nil && strings.Contains(strings.ToLower(it.DisplayName), "new vegas") {
				cands = append(cands, it.InstallLocation)
			}
		}
	}
	for _, c := range cands {
		if ok(c) {
			return filepath.Clean(c)
		}
	}
	if loud {
		app.logf("Looked for FalloutNV.exe in: %s", strings.Join(nonEmpty(cands), "; "))
	}
	return ""
}

// No Fallout: New Vegas: GET FALLOUT: NEW VEGAS opens its store page, CHOOSE FALLOUTNV.EXE takes it from anywhere, and
// setup looks again every few seconds - it carries on by itself once the game is installed
func waitForGame() string {
	app.step(0, scene.Warn)
	app.status("Get Fallout: New Vegas: press GET, install it, setup carries on by itself.")
	app.logf("Fallout: New Vegas: not found. Installed somewhere else? Press CHOOSE FALLOUTNV.EXE and pick FalloutNV.exe in its folder.")
	wait := make(chan bool, 1)
	app.button("GET FALLOUT: NEW VEGAS", scene.BtnAction, func() {
		openURL("https://store.steampowered.com/app/22380/")
		select {
		case wait <- true:
		default:
		}
	})
	drainPicked()
	choosePill("CHOOSE FALLOUTNV.EXE", "Find FalloutNV.exe in your Fallout: New Vegas folder", "FalloutNV.exe\x00FalloutNV.exe\x00Programs\x00*.exe\x00\x00")
	defer choosePill("", "", "")
	app.mu.Lock()
	app.busy = false
	app.mu.Unlock()
	defer func() { app.mu.Lock(); app.busy = true; app.mu.Unlock() }()
	for i := 0; i < 100000; i++ {
		select {
		case <-wait:
		case p := <-picked:
			if strings.EqualFold(filepath.Base(p), "FalloutNV.exe") {
				*flagGame = filepath.Dir(p)
				app.logf("Fallout: New Vegas: you chose %s", redact(*flagGame))
			} else {
				app.status("That isn't FalloutNV.exe - pick the one in your Fallout: New Vegas folder.")
			}
		case <-time.After(4 * time.Second):
		}
		if g := findGame(false); g != "" {
			app.button("WORKING", scene.BtnWorking, nil)
			app.step(0, scene.Active)
			return g
		}
	}
	return ""
}

// The optional games: what each adds, and where to get it (logged once the cars are built)
func optionalGames() {
	for _, g := range []struct{ name, id, folder, adds string }{
		{"Need for Speed Hot Pursuit Remastered", "1328660", "Need For Speed Hot Pursuit Remastered", "its soundtrack on the car radio and its tyre smoke"},
	} {
		if steam.Find(g.id, g.folder) == "" {
			app.logf("Optional: %s isn't installed - it adds %s. Get it: https://store.steampowered.com/app/%s/ (setup and PLAY pick it up by themselves).", g.name, g.adds, g.id)
		}
	}
}

func nonEmpty(v []string) []string {
	var o []string
	for _, s := range v {
		if s != "" {
			o = append(o, redact(s))
		}
	}
	return o
}

func regString(root registry.Key, path, name string) string {
	k, err := registry.OpenKey(root, path, registry.QUERY_VALUE|registry.WOW64_64KEY)
	if err != nil {
		return ""
	}
	defer k.Close()
	v, _, err := k.GetStringValue(name)
	if err != nil {
		return ""
	}
	return v
}

func regDword(root registry.Key, path, name string) uint64 {
	k, err := registry.OpenKey(root, path, registry.QUERY_VALUE|registry.WOW64_64KEY)
	if err != nil {
		return 0
	}
	defer k.Close()
	v, _, _ := k.GetIntegerValue(name)
	return v
}

func findMW() string {
	ok := func(p string) bool { return p != "" && exists(filepath.Join(p, "NFS13.exe")) }
	cands := []string{strings.Trim(*flagMW, `" `), steam.Find("1262560", "Need for Speed(TM) Most Wanted")}
	for _, k := range []string{`SOFTWARE\WOW6432Node\EA Games\Need for Speed(TM) Most Wanted`, `SOFTWARE\WOW6432Node\Criterion Games\Need for Speed(TM) Most Wanted`, `SOFTWARE\EA Games\Need for Speed(TM) Most Wanted`} {
		for _, n := range []string{"Install Dir", "InstallDir", "Install_Dir"} {
			if v := regString(registry.LOCAL_MACHINE, k, n); v != "" {
				cands = append(cands, v)
			}
		}
	}
	for _, d := range drives() {
		for _, sub := range []string{`Program Files\EA Games\Need for Speed(TM) Most Wanted`, `Program Files (x86)\Origin Games\Need for Speed(TM) Most Wanted`,
			`Program Files\EA Games\Need for Speed Most Wanted`, `EA Games\Need for Speed(TM) Most Wanted`, `Games\Need for Speed(TM) Most Wanted`} {
			cands = append(cands, filepath.Join(d, sub))
		}
	}
	for _, c := range cands {
		if ok(c) {
			return filepath.Clean(c)
		}
	}
	return ""
}

func drives() []string {
	var out []string
	for c := 'C'; c <= 'Z'; c++ {
		d := string(c) + `:\`
		if exists(d) {
			out = append(out, d)
		}
	}
	return out
}

// The plugin build in a DLL
var rePluginNo = regexp.MustCompile(`Mojave (?:Most Wanted|Enhanced) plugin (\d+)`)

func pluginNo(b []byte) int {
	m := rePluginNo.FindSubmatch(b)
	if m == nil {
		return 0
	}
	n, _ := strconv.Atoi(string(m[1]))
	return n
}

// Which of the mod's files in the game differ from this setup's own (payload) copies - none when the game already has a
// newer plugin build than setup carries
func modFilesStale(game string) []string {
	zr, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return nil
	}
	have, err := os.ReadFile(filepath.Join(pluginDir(game), "MojaveMostWanted.dll"))
	if nb, e := os.ReadFile(filepath.Join(pluginDir(game), "MojaveMostWanted.dll.new")); e == nil && pluginNo(nb) > pluginNo(have) {
		have, err = nb, nil
	}
	if err == nil {
		for _, f := range zr.File {
			if path.Base(f.Name) != "MojaveMostWanted.dll" {
				continue
			}
			if rc, err := f.Open(); err == nil {
				b, _ := io.ReadAll(rc)
				rc.Close()
				if h, p := pluginNo(have), pluginNo(b); h > p {
					app.logf("Mod files: this game has plugin %d, newer than setup's %d - left as they are.", h, p)
					return nil
				}
			}
		}
	}
	var out []string
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			continue
		}
		b, _ := io.ReadAll(rc)
		rc.Close()
		old, err := os.ReadFile(filepath.Join(game, filepath.FromSlash(f.Name)))
		if err != nil || !bytes.Equal(old, b) {
			out = append(out, path.Base(f.Name))
		}
	}
	return out
}

// ---- 3. our files, the load order, loose files, the shortcut
func putIn(game string) error {
	zr, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return err
	}
	n := 0
	for _, f := range zr.File {
		if f.FileInfo().IsDir() {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return err
		}
		b, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return err
		}
		dst := filepath.Join(game, filepath.FromSlash(f.Name))
		if err := putFile(dst, b); err != nil {
			return fmt.Errorf("%s: %v", f.Name, err)
		}
		n++
	}
	app.logf("Mojave Enhanced: %d files in place", n)
	os.RemoveAll(filepath.Join(pluginDir(game), "MojaveMostWanted", "fonts"))
	exe, _ := os.Executable()
	self := filepath.Join(mmwDir(game), setupName)
	if old := filepath.Join(mmwDir(game), oldSetupName); !strings.EqualFold(filepath.Clean(exe), old) {
		os.Remove(old)
	}
	if !strings.EqualFold(filepath.Clean(exe), self) {
		if b, err := os.ReadFile(exe); err == nil {
			if err := putFile(self, b); err != nil {
				app.logf("setup copy: %v", err)
			}
		}
	}
	if err := loadOrder(game); err != nil {
		return fmt.Errorf("the load order (plugins.txt): %v", err)
	}
	if err := looseFiles(game); err != nil {
		app.logf("loose files setting: %v (the cars still load; repaved roads might not show)", err)
	}
	smoothAim(game)
	modManagers(game, true)
	shortcut(self, game)
	return nil
}

// The official files in the order the game loads them
var officialMasters = []string{"FalloutNV.esm", "DeadMoney.esm", "HonestHearts.esm", "OldWorldBlues.esm", "LonesomeRoad.esm",
	"GunRunnersArsenal.esm", "CaravanPack.esm", "ClassicPack.esm", "MercenaryPack.esm", "TribalPack.esm"}

func pluginsTxtPath() string {
	return filepath.Join(knownFolder(folderLocalAppData), "FalloutNV", "plugins.txt")
}

// The load order is the same on every PC
func loadOrder(game string) error {
	p := pluginsTxtPath()
	old, _ := os.ReadFile(p)
	official := map[string]bool{}
	for _, m := range officialMasters {
		official[strings.ToLower(m)] = true
	}
	var out, esms, esps []string
	for _, m := range officialMasters {
		if exists(filepath.Join(dataDir(game), m)) {
			out = append(out, m)
		}
	}
	seen := map[string]bool{}
	for _, l := range splitLines(string(old)) {
		l = strings.TrimSpace(l)
		low := strings.ToLower(l)
		if l == "" || strings.HasPrefix(l, "#") || official[low] || strings.EqualFold(l, espName) || seen[low] {
			continue
		}
		seen[low] = true
		if strings.HasSuffix(low, ".esm") {
			esms = append(esms, l)
		} else {
			esps = append(esps, l)
		}
	}
	out = append(out, esms...)
	out = append(out, esps...)
	out = append(out, espName)
	if err := putFile(p, []byte(strings.Join(out, "\r\n")+"\r\n")); err != nil {
		return err
	}
	base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	var fixed []string
	last := base
	for i, m := range officialMasters {
		f := filepath.Join(dataDir(game), m)
		st, err := os.Stat(f)
		if err != nil {
			continue
		}
		want := base.AddDate(0, 0, i)
		last = want
		if !st.ModTime().Equal(want) {
			if os.Chtimes(f, want, want) == nil {
				fixed = append(fixed, m)
			}
		}
	}
	if len(fixed) > 0 {
		app.logf("Load order: the date of %s set to Bethesda's own (the game loads by date)", strings.Join(fixed, ", "))
	}
	if st, err := os.Stat(filepath.Join(dataDir(game), espName)); err == nil && !st.ModTime().After(last) {
		t := last.AddDate(0, 0, 18)
		os.Chtimes(filepath.Join(dataDir(game), espName), t, t)
	}
	app.logf("Load order: %d files - the official ones first in their order, %s last", len(out), espName)
	return nil
}

// The smooth aim-in: lStewieAl's Tweaks blends the camera onto the sights and kNVSE eases every animation blend.
func smoothAim(game string) {
	sets := []struct {
		file    string
		section string
		kv      map[string]string
		create  bool
	}{
		{"nvse_stewie_tweaks.ini", "Tweaks", map[string]string{"bSmoothIronsightsCameraTransition": "1"}, false},
		{"nvse_stewie_tweaks.ini", "Smooth Iron Sights Camera", map[string]string{"iAimTransitionTimeMS": "450", "iEasingFunction": "3"}, false},
		{"kNVSE.ini", "Blend Fixes", map[string]string{"bBlendSmoothing": "1", "fBlendSmoothingRate": "0.12"}, true},
	}
	for _, st := range sets {
		p := filepath.Join(pluginDir(game), st.file)
		b, err := os.ReadFile(p)
		if err != nil {
			if !st.create || !exists(filepath.Join(pluginDir(game), "kNVSE.dll")) {
				continue
			}
			b = nil
		}
		nb := setIni(string(b), st.section, st.kv)
		if nb == string(b) {
			continue
		}
		if err := putFile(p, []byte(nb)); err != nil {
			app.logf("%s: %v", st.file, err)
			continue
		}
		app.logf("%s: [%s] smooth aim-in keys set", st.file, st.section)
	}
}

// New Vegas only uses loose files over its own archives with bInvalidateOlderFiles=1 ([Archive] in Fallout.ini)
func looseFiles(game string) error {
	dir := filepath.Join(knownFolder(folderDocuments), "My Games", "FalloutNV")
	ini := filepath.Join(dir, "Fallout.ini")
	if !exists(ini) {
		def := filepath.Join(game, "Fallout_default.ini")
		b, err := os.ReadFile(def)
		if err != nil {
			return errors.New("no Fallout.ini yet (start the game once from Steam, then run setup again)")
		}
		if err := putFile(ini, b); err != nil {
			return err
		}
	}
	for _, name := range []string{"Fallout.ini", "FalloutPrefs.ini"} {
		p := filepath.Join(dir, name)
		b, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		nb := setIni(string(b), "Archive", map[string]string{"bInvalidateOlderFiles": "1", "SInvalidationFile": ""})
		if nb != string(b) {
			if err := putFile(p, []byte(nb)); err != nil {
				return err
			}
			app.logf("%s: loose files allowed (bInvalidateOlderFiles=1)", name)
		}
	}
	return nil
}

// Mod Organizer 2 runs the game from its own profile's load order: our.esp goes in there too (mods.go)
func modManagers(game string, fix bool) []string {
	var notes []string
	for _, m := range mo2Instances(game) {
		notes = append(notes, fmt.Sprintf("Mod Organizer 2 manages this game (instance %q, profile %q)", m.Name, m.Profile))
		if fix {
			mo2PluginOn(m)
		}
	}
	if vortexThere() {
		notes = append(notes, "Vortex is installed (if it manages New Vegas, keep "+espName+" enabled in its plugin list)")
	}
	for _, n := range notes {
		app.logf("%s", n)
	}
	return notes
}

func iniValue(s, key string) string {
	for _, l := range splitLines(s) {
		if k, v, ok := strings.Cut(strings.TrimSpace(l), "="); ok && strings.EqualFold(strings.TrimSpace(k), key) {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func shortcut(target, game string) {
	desk := knownFolder(folderDesktop)
	if desk == "" {
		return
	}
	os.Remove(filepath.Join(desk, "Mojave Most Wanted.lnk"))
	lnk := filepath.Join(desk, "Mojave Enhanced.lnk")
	ps := fmt.Sprintf(`$s=(New-Object -COM WScript.Shell).CreateShortcut('%s');$s.TargetPath='%s';$s.Arguments='--play';$s.WorkingDirectory='%s';$s.IconLocation='%s,0';$s.Description='Mojave Enhanced';$s.Save()`,
		psq(lnk), psq(target), psq(filepath.Dir(target)), psq(target))
	cmd := exec.Command("powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", ps)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	if out, err := cmd.CombinedOutput(); err != nil {
		app.logf("desktop shortcut: %v %s", err, strings.TrimSpace(string(out)))
		return
	}
	if !contains(man.Added, lnk) {
		man.Added = append(man.Added, lnk)
	}
	app.logf("Desktop shortcut: Mojave Enhanced")
}

func psq(s string) string { return strings.ReplaceAll(s, "'", "''") }

// PLAY WITHOUT CARS as a setting: paths.ini cars=off tells the launcher to build no cars (and the game gets none); off
// again, the next PLAY builds them (quickly: the launcher's cache keeps what it built)
var waitingNFS bool

func setCarsPath(game string, off bool) { setPathsKey(game, "cars", map[bool]string{true: "off"}[off]) }

// setPathsKey sets key=value in the launcher's paths.ini (an empty value takes the key out)
func setPathsKey(game, key, value string) {
	p := filepath.Join(mmwDir(game), "paths.ini")
	var keep []string
	if b, err := os.ReadFile(p); err == nil {
		for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
			if t := strings.TrimSpace(l); t != "" && !strings.HasPrefix(strings.ToLower(t), key+"=") {
				keep = append(keep, t)
			}
		}
	}
	if len(keep) == 0 {
		keep = []string{"; written by setup"}
	}
	if value != "" {
		keep = append(keep, key+"="+value)
	}
	putFile(p, []byte(strings.Join(keep, "\r\n")+"\r\n"))
}

func toggleCars() {
	app.mu.Lock()
	game, busy := app.game, app.busy
	app.mu.Unlock()
	if game == "" || busy {
		return
	}
	man.NoCars = !man.NoCars
	saveManifest()
	setCarsPath(game, man.NoCars)
	if man.NoCars {
		app.status("No cars: PLAY starts everything but the cars (the garage stays empty). Press it again for cars.")
		app.logf("PLAY WITHOUT CARS: on")
	} else {
		app.status("Cars back on: the next PLAY builds them (quickly - they're kept from before).")
		app.logf("PLAY WITHOUT CARS: off")
	}
	netPills()
}

// ---- 4. the cars, built from the player's own Need for Speed Most Wanted by the launcher
var reCars = regexp.MustCompile(`cars: (\d+)/(\d+)`)
var reReady = regexp.MustCompile(`cars: (\d+) of (\d+) ready`)

func buildCars(game string) (string, error) {
	mw := findMW()
	enforceRollout()
	if man.Vanilla {
		app.with(func(st *scene.State) { st.Steps[3], st.Steps[4] = "Set up vanilla stable (no cars)", "Ready to play" })
	}
	skip := man.NoCars || man.Vanilla
	waitingNFS = true
	defer func() { waitingNFS = false }()
	for mw == "" && !skip {
		app.step(3, scene.Warn)
		app.status("Get Need for Speed Most Wanted (2012) with GET - or press PLAY WITHOUT CARS.")
		app.logf("Need for Speed Most Wanted: not found (Steam, EA app, Origin folders). Installed somewhere else? Press CHOOSE NFS13.EXE and pick NFS13.exe in its folder.")
		wait := make(chan bool, 1)
		app.button("GET NFS MOST WANTED", scene.BtnAction, func() {
			openURL("https://store.steampowered.com/app/1262560/")
			select {
			case wait <- true:
			default:
			}
		})
		drainPicked()
		choosePill("CHOOSE NFS13.EXE", "Find NFS13.exe in your Need for Speed Most Wanted folder", "NFS13.exe\x00NFS13.exe\x00Programs\x00*.exe\x00\x00")
		app.with(func(s *scene.State) {
			s.Pills, s.PillOn = append(s.Pills, "PLAY WITHOUT CARS"), append(s.PillOn, false)
		})
		app.logf("No Need for Speed? PLAY WITHOUT CARS sets up everything but the cars (fine for testing and playing together on foot).")
		drainSkip()
		app.mu.Lock()
		app.busy = false
		app.mu.Unlock()
		for i := 0; mw == "" && !skip && i < 100000; i++ {
			select {
			case <-skipCars:
				skip = true
				continue
			case <-wait:
			case p := <-picked:
				if strings.EqualFold(filepath.Base(p), "NFS13.exe") {
					*flagMW = filepath.Dir(p)
					app.logf("Need for Speed Most Wanted: you chose %s", redact(*flagMW))
				} else {
					app.status("That isn't NFS13.exe - pick the one in your Need for Speed Most Wanted folder.")
				}
			case <-time.After(4 * time.Second):
			}
			mw = findMW()
		}
		choosePill("", "", "")
		app.mu.Lock()
		app.busy = true
		app.mu.Unlock()
		app.button("WORKING", scene.BtnWorking, nil)
		app.step(3, scene.Active)
	}
	waitingNFS = false
	if skip {
		if !man.NoCars && !man.Vanilla {
			man.NoCars = true
			saveManifest()
		}
		setCarsPath(game, man.NoCars)
		app.logf("Need for Speed Most Wanted: skipped - no cars (PLAY WITHOUT CARS).")
		app.status("Setting up without cars...")
		cmd := exec.Command(filepath.Join(mmwDir(game), "mmw-launch.exe"), "--fnv", game, "--convert-only")
		cmd.Dir = mmwDir(game)
		cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
		out, err := cmd.CombinedOutput()
		for _, l := range splitLines(string(out)) {
			app.logf("  %s", l)
		}
		if err != nil {
			return "", fmt.Errorf("The launcher failed (%v). Open details and send it.", err)
		}
		return "", nil
	}
	if !exists(filepath.Join(mw, "VEHICLES")) {
		return "", fmt.Errorf("Need for Speed Most Wanted at %s has no VEHICLES folder: is it still downloading? Let it finish, then press the button again.", redact(mw))
	}
	app.logf("Need for Speed Most Wanted: %s", redact(mw))
	putFile(filepath.Join(mmwDir(game), "paths.ini"), []byte("; written by setup\r\nmw="+mw+"\r\n"))
	launcher := filepath.Join(mmwDir(game), "mmw-launch.exe")
	cmd := exec.Command(launcher, "--fnv", game, "--mw", mw, "--convert-only")
	cmd.Dir = mmwDir(game)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	out, err := cmd.StdoutPipe()
	if err != nil {
		return "", err
	}
	cmd.Stderr = cmd.Stdout
	app.status("Building the cars from Need for Speed (the first time takes a few minutes)...")
	if err := cmd.Start(); err != nil {
		return "", fmt.Errorf("couldn't start the car builder: %v (an antivirus may have blocked mmw-launch.exe)", err)
	}
	sc := bufio.NewScanner(out)
	ready := ""
	for sc.Scan() {
		line := sc.Text()
		app.logf("  %s", line)
		if m := reCars.FindStringSubmatch(line); m != nil {
			a, _ := strconv.Atoi(m[1])
			b, _ := strconv.Atoi(m[2])
			if b > 0 {
				app.progress(0.55 + 0.4*float64(a)/float64(b))
				app.with(func(s *scene.State) {
					s.Status = fmt.Sprintf("Building the cars from Need for Speed (%d of %d)...", a, b)
				})
			}
		}
		if m := reReady.FindStringSubmatch(line); m != nil {
			ready = m[1] + " of " + m[2]
		}
	}
	if err := cmd.Wait(); err != nil {
		return "", fmt.Errorf("Building the cars failed (%v). Open details and send it.", err)
	}
	if ready != "" {
		app.logf("Cars ready: %s", ready)
	}
	return mw, nil
}

// ---- 5. the last look
func verify(game string) (string, bool) {
	var miss []string
	need := []string{filepath.Join(pluginDir(game), "MojaveMostWanted.dll"), filepath.Join(dataDir(game), espName),
		filepath.Join(mmwDir(game), "mmw-launch.exe"), filepath.Join(game, "nvse_loader.exe"), filepath.Join(game, "nvse_1_4.dll"),
		filepath.Join(pluginDir(game), "MojaveMostWanted", "mmw.ini")}
	for _, t := range nexusTools {
		need = append(need, filepath.Join(pluginDir(game), t.DLL))
	}
	for _, p := range need {
		if !exists(p) {
			miss = append(miss, redact(p))
		}
	}
	if len(miss) > 0 {
		app.logf("Missing: %s", strings.Join(miss, ", "))
		return "Some files are missing (an antivirus may have removed them). Open details, then press the button again.", false
	}
	b, _ := os.ReadFile(filepath.Join(pluginDir(game), "MojaveMostWanted", "mmw.ini"))
	cars := 0
	for _, l := range splitLines(string(b)) {
		if strings.HasPrefix(l, "car.") && strings.Count(l, ".") == 1 && strings.HasSuffix(l, "=1") {
			cars++
		}
	}
	lo, _ := os.ReadFile(pluginsTxtPath())
	if !strings.Contains(strings.ToLower(string(lo)), strings.ToLower(espName)) {
		return "The load order lost " + espName + " (a mod manager?). Press the button again.", false
	}
	app.logf("Last look: every file in place, %d cars built, %s in the load order", cars, espName)
	optionalGames()
	if cars == 0 && !man.NoCars && !man.Vanilla {
		return "No cars were built. Open details and send it.", false
	}
	if man.NoCars {
		app.logf("Set up without cars (no Need for Speed Most Wanted): everything else works; the garage says Most Wanted is missing.")
	}
	return "", true
}

// ---- PLAY: the launcher converts anything new and starts the game through xNVSE
func play() {
	app.mu.Lock()
	game := app.game
	app.mu.Unlock()
	if p := processRunning("FalloutNV.exe"); p != "" {
		app.status("Fallout: New Vegas is already running.")
		return
	}
	if miss := toolsMissing(game); miss != "" {
		app.mu.Lock()
		app.busy = true
		app.mu.Unlock()
		app.logf("Before playing: %s is missing.", miss)
		app.step(1, scene.Active)
		err := fixTools(game)
		app.mu.Lock()
		app.busy = false
		app.mu.Unlock()
		netPills()
		if err != nil {
			app.step(1, scene.Warn)
			app.status("%s", err.Error())
			app.button("PLAY", scene.BtnDone, func() { go play() })
			return
		}
		app.step(1, scene.Done)
		saveManifest()
	}
	if stale := modFilesStale(game); len(stale) > 0 {
		app.status("Updating Mojave Enhanced (%d files)...", len(stale))
		app.logf("Mod files newer in setup %s: %s", appVersion, strings.Join(stale, ", "))
		if err := putIn(game); err != nil {
			app.status("Couldn't update the mod: %v", err)
			app.button("PLAY", scene.BtnDone, func() { go play() })
			return
		}
		saveManifest()
	}
	if playThroughMO2(game) {
		return
	}
	if man.Mods == "vortex" && !vortexDeployed(game) {
		app.logf("Vortex: no deployment in this game yet (Deploy Mods in Vortex puts its mods in)")
	}
	launcher := filepath.Join(mmwDir(game), "mmw-launch.exe")
	cmd := exec.Command(launcher, "--fnv", game)
	cmd.Dir = mmwDir(game)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	app.button("STARTING", scene.BtnWorking, nil)
	app.status("Starting Fallout: New Vegas...")
	out, _ := cmd.StdoutPipe()
	cmd.Stderr = cmd.Stdout
	if err := cmd.Start(); err != nil {
		app.status("Couldn't start the launcher: %v", err)
		app.button("PLAY", scene.BtnDone, func() { go play() })
		return
	}
	sc := bufio.NewScanner(out)
	for sc.Scan() {
		app.logf("  %s", sc.Text())
	}
	cmd.Wait()
	app.status("Have fun! (This window closes by itself.)")
	time.Sleep(4 * time.Second)
	pPostMessageW.Call(hwndMain, wmClose, 0, 0)
}

func sha(b []byte) string { h := sha256.Sum256(b); return hex.EncodeToString(h[:]) }

// Redact the author's own folder name from what's shown and reported
func redact(p string) string {
	home := os.Getenv("USERPROFILE")
	if home != "" && len(p) >= len(home) && strings.EqualFold(p[:len(home)], home) {
		return `%USERPROFILE%` + p[len(home):]
	}
	return p
}
