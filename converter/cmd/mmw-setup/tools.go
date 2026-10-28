//go:build windows

package main

import (
	"debug/pe"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"syscall"
	"time"

	"mmw/cmd/mmw-setup/scene"

	"golang.org/x/sys/windows/registry"
)

// The New Vegas tools the mod is set up with. xNVSE comes from its GitHub releases by itself
type nexusTool struct {
	Name, DLL string
	ID        int
	Need      string
	NeedFile  string
	Root      bool
}

// The 4GB Patcher lets New Vegas use 4 GB of memory and makes it load xNVSE when started from Steam.
var patcher4GB = nexusTool{Name: "4GB Patcher", DLL: "FalloutNVpatch.exe", ID: 62552, Root: true}

func is4GB(game string) bool {
	f, err := pe.Open(filepath.Join(game, "FalloutNV.exe"))
	if err != nil {
		return false
	}
	defer f.Close()
	return f.FileHeader.Characteristics&0x20 != 0
}

func get4GB(game string) error {
	exe := filepath.Join(game, "FalloutNV.exe")
	if is4GB(game) {
		app.logf("4GB Patcher: FalloutNV.exe already uses 4 GB.")
		return nil
	}
	patch := filepath.Join(game, patcher4GB.DLL)
	if !exists(patch) {
		if err := waitNexus(game, patcher4GB, false); err != nil {
			return err
		}
	}
	if _, known := man.Replaced[exe]; !known {
		b, err := os.ReadFile(exe)
		if err != nil {
			return err
		}
		bk := filepath.Join(undoDir(game), "originals", "FalloutNV.exe")
		if err := os.MkdirAll(filepath.Dir(bk), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(bk, b, 0o644); err != nil {
			return err
		}
		man.Replaced[exe] = bk
	}
	hadBackup := exists(filepath.Join(game, "FalloutNV_backup.exe"))
	app.status("Letting Fallout: New Vegas use 4 GB of memory (the 4GB Patcher)...")
	cmd := hiddenCmd(patch)
	cmd.Dir = game
	type res struct {
		out []byte
		err error
	}
	done := make(chan res, 1)
	go func() { o, e := cmd.CombinedOutput(); done <- res{o, e} }()
	var r res
	select {
	case r = <-done:
	case <-time.After(60 * time.Second):
		if cmd.Process != nil {
			cmd.Process.Kill()
		}
		r.err = errors.New("it didn't finish in a minute")
	}
	said := strings.TrimSpace(string(r.out))
	app.logf("4GB Patcher said: %s", said)
	if !hadBackup && exists(filepath.Join(game, "FalloutNV_backup.exe")) && !contains(man.Added, filepath.Join(game, "FalloutNV_backup.exe")) {
		man.Added = append(man.Added, filepath.Join(game, "FalloutNV_backup.exe"))
	}
	saveManifest()
	if !is4GB(game) {
		if strings.Contains(said, "INVALID EXE VERSION") {
			return fmt.Errorf("The 4GB Patcher doesn't know this FalloutNV.exe (Epic, Xbox and Bethesda.net copies aren't supported). Open details and send it.")
		}
		return fmt.Errorf("The 4GB Patcher didn't patch FalloutNV.exe (%v). Open details and send it.", r.err)
	}
	app.logf("4GB Patcher: FalloutNV.exe now uses 4 GB (%s)", laaText(exe))
	return nil
}

var nexusTools = []nexusTool{
	{"JIP LN NVSE", "jip_nvse.dll", 58277, "", "", false},
	{"kNVSE", "kNVSE.dll", 71336, "", "", false},
	{"lStewieAl's Tweaks", "nvse_stewie_tweaks.dll", 66347, "", "", false},
	{"New Vegas Tick Fix", "NVTF.dll", 66537, "NVTF.ini", "NVTF - INI", false},
}

// toolsMissing: what a Nexus tool still lacks (its DLL or a settings file it needs); none: ""
func toolsMissing(game string) string {
	for _, t := range nexusTools {
		if !exists(filepath.Join(pluginDir(game), t.DLL)) {
			return t.Name
		}
		if t.Need != "" && !exists(filepath.Join(pluginDir(game), t.Need)) {
			return t.Name + " (" + t.Need + ")"
		}
	}
	if !is4GB(game) {
		return "the 4GB Patcher"
	}
	return ""
}

// fixTools: before PLAY, put in what's missing
func fixTools(game string) error {
	for i, t := range nexusTools {
		if err := getNexusTool(game, t, i); err != nil {
			return err
		}
	}
	return get4GB(game)
}

// ---- A file the player points at (CHOOSE THE FILE, under the checklist while setup waits for a Nexus download)
var picked = make(chan string, 1)
var pickTitle, pickFilter = "Choose the file you downloaded from Nexus", "Mod files (*.7z, *.zip, *.dll, *.ini)\x00*.7z;*.zip;*.dll;*.ini\x00All files\x00*.*\x00\x00"

func chooseFile() {
	app.mu.Lock()
	title, filter := pickTitle, pickFilter
	app.mu.Unlock()
	p := openFileDialog(title, filter)
	if p == "" {
		return
	}
	select {
	case picked <- p:
	default:
	}
}

// PLAY WITHOUT CARS (the Need for Speed step)
var skipCars = make(chan bool, 1)

func drainSkip() {
	for {
		select {
		case <-skipCars:
		default:
			return
		}
	}
}

// The pill under the checklist while setup waits: label "" takes it away; title/filter for the file picker
func choosePill(label, title, filter string) {
	app.mu.Lock()
	pickTitle, pickFilter = title, filter
	app.mu.Unlock()
	app.with(func(s *scene.State) {
		if label != "" {
			s.Pills, s.PillOn = []string{label}, []bool{false}
		} else {
			s.Pills, s.PillOn = nil, nil
		}
	})
}
func waitPills(on bool) {
	if on {
		choosePill("CHOOSE THE FILE", "Choose the file you downloaded from Nexus", "Mod files (*.7z, *.zip, *.dll, *.ini)\x00*.7z;*.zip;*.dll;*.ini\x00All files\x00*.*\x00\x00")
	} else {
		choosePill("", "", "")
	}
}
func drainPicked() {
	for {
		select {
		case <-picked:
		default:
			return
		}
	}
}

func getTools(game string) error {
	app.progress(0.1)
	vcRuntime()
	if err := getXNVSE(game); err != nil {
		return err
	}
	for i, t := range nexusTools {
		app.progress(0.2 + 0.25*float64(i+1)/float64(len(nexusTools)))
		if err := getNexusTool(game, t, i); err != nil {
			return err
		}
	}
	return get4GB(game)
}

var httpc = &http.Client{Timeout: 5 * time.Minute}

func download(url string) ([]byte, error) {
	req, _ := http.NewRequest("GET", url, nil)
	req.Header.Set("User-Agent", "MojaveMostWantedSetup/"+appVersion)
	resp, err := httpc.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("%s: %s", url, resp.Status)
	}
	return io.ReadAll(resp.Body)
}

// The Visual C++ 2015-2022 runtime
func vcRuntime() {
	if regDword(registry.LOCAL_MACHINE, `SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x86`, "Installed") == 1 ||
		regDword(registry.LOCAL_MACHINE, `SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86`, "Installed") == 1 {
		app.logf("Microsoft Visual C++ runtime (x86): fine.")
		return
	}
	app.status("Getting the Microsoft Visual C++ runtime (Windows asks for permission)...")
	b, err := download("https://aka.ms/vs/17/release/vc_redist.x86.exe")
	if err != nil {
		app.logf("Visual C++ runtime download failed: %v (xNVSE may not start without it)", err)
		return
	}
	p := filepath.Join(os.TempDir(), "vc_redist.x86.exe")
	os.WriteFile(p, b, 0o755)
	if err := shellExecute("runas", p, "/install /quiet /norestart", "", 0); err != nil {
		app.logf("Visual C++ runtime: %v", err)
		return
	}
	for i := 0; i < 120 && processRunning("vc_redist.x86.exe") != ""; i++ {
		time.Sleep(time.Second)
	}
	app.logf("Microsoft Visual C++ runtime (x86): installed.")
}

// ---- xNVSE from github.com/xNVSE/NVSE/releases (the newest; kept if what's there is as new)
func getXNVSE(game string) error {
	app.status("Getting xNVSE...")
	tag, err := latestTag("https://github.com/xNVSE/NVSE/releases/latest")
	have := fileVersion(filepath.Join(game, "nvse_1_4.dll"))
	if err != nil {
		if have != "" {
			app.logf("xNVSE %s already in (couldn't check for a newer one: %v)", have, err)
			return nil
		}
		return fmt.Errorf("Couldn't reach GitHub for xNVSE (%v). Check the internet connection and press the button again.", err)
	}
	if have != "" && !versionLess(have, tag) {
		app.logf("xNVSE %s: already installed.", have)
		return nil
	}
	url := fmt.Sprintf("https://github.com/xNVSE/NVSE/releases/download/%s/nvse_%s.7z", tag, strings.ReplaceAll(tag, ".", "_"))
	app.logf("xNVSE %s: downloading %s", tag, url)
	b, err := download(url)
	if err != nil {
		return fmt.Errorf("xNVSE download failed (%v). Press the button again.", err)
	}
	files, err := readArchive(b, url)
	if err != nil {
		return fmt.Errorf("xNVSE archive: %v", err)
	}
	n := 0
	for name, data := range files {
		lower := strings.ToLower(name)
		if strings.HasSuffix(lower, ".pdb") || strings.HasPrefix(lower, "src/") {
			continue
		}
		dst := ""
		switch {
		case strings.HasPrefix(lower, "data/"):
			dst = filepath.Join(game, filepath.FromSlash(name))
		case !strings.Contains(name, "/"):
			dst = filepath.Join(game, name)
		default:
			continue
		}
		if err := putFile(dst, data); err != nil {
			return fmt.Errorf("xNVSE: %s: %v", name, err)
		}
		n++
	}
	if !exists(filepath.Join(game, "nvse_1_4.dll")) {
		return errors.New("xNVSE's archive didn't have nvse_1_4.dll: open details and send it")
	}
	app.logf("xNVSE %s: installed (%d files).", tag, n)
	return nil
}

// latestTag follows GitHub's /releases/latest redirect to.../tag/<tag>
func latestTag(url string) (string, error) {
	c := &http.Client{Timeout: 30 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
	resp, err := c.Get(url)
	if err != nil {
		return "", err
	}
	resp.Body.Close()
	loc := resp.Header.Get("Location")
	if i := strings.LastIndex(loc, "/tag/"); i >= 0 {
		return loc[i+5:], nil
	}
	return "", fmt.Errorf("no release found (%s)", resp.Status)
}

// ---- The Nexus tools: in already, downloaded already (Downloads, the desktop, setup's own folder, Vortex's or Mod
func getNexusTool(game string, t nexusTool, idx int) error {
	dll := filepath.Join(pluginDir(game), t.DLL)
	if exists(dll) {
		if t.Need == "" || exists(filepath.Join(pluginDir(game), t.Need)) {
			app.logf("%s %s: already installed.", t.Name, fileVersion(dll))
			return nil
		}
	} else if err := waitNexus(game, t, false); err != nil {
		return err
	}
	if t.Need != "" && !exists(filepath.Join(pluginDir(game), t.Need)) {
		app.logf("%s: %s is missing - the game closes without it.", t.Name, t.Need)
		return waitNexus(game, t, true)
	}
	return nil
}

// What to click on a tool's Nexus page
func nexusHint(t nexusTool, settings bool) string {
	file, also := "", ""
	switch {
	case settings:
		file = "\"" + t.NeedFile + "\"  (the small settings file - the game closes at once without it)"
	case t.ID == 58277:
		file = "\"JIP LN NVSE Plugin\"  (the plugin itself; the optional \"Settings INI\" isn't needed)"
	case t.ID == 71336:
		file = "\"kNVSE\"  (the newest one)"
	case t.ID == 66347:
		file = "the main file of lStewieAl's Tweaks  (the newest version, 10 or later)"
	case t.ID == 66537:
		file = "\"NVTF\"  (the Tick Fix itself)"
		also = "\r\nALSO download \"NVTF - INI\" from the same list (a tiny settings file).\r\nThe game closes at once without it.\r\n"
	case t.ID == 62552:
		file = "\"4GB Patcher\"  (version 1.5, about 100 KB: \"does not require the VC++ redist\")"
		also = "\r\nNOT \"FNV4GB for Linux\" and NOT the older \"FNV 4GB Patch\" 1.4.\r\nIt lets New Vegas use 4 GB of memory (without it the game can crash when it loads Goodsprings).\r\n"
	default:
		file = "the newest file under Main files"
	}
	return "Next: " + t.Name + " from Nexus Mods (a free account is needed).\r\n\r\n" +
		"On the page that opens, under MAIN FILES, download:\r\n\r\n    > " + file + "\r\n" + also +
		"\r\nClick \"Manual download\" (then \"Slow download\" on a free account) - not \"Mod manager download\".\r\n\r\n" +
		"Leave the file in your Downloads folder: setup finds it and puts it in by itself. No need to open or unzip it.\r\n" +
		"(Saved somewhere else? Press CHOOSE THE FILE under the checklist.)\r\n\r\nPress OK to open the page."
}

// One download of a tool (the mod itself, or its settings file): found, or the page opened and waited for
func waitNexus(game string, t nexusTool, settings bool) error {
	what := t.Name
	if settings {
		what = t.Name + "'s settings file (" + t.NeedFile + ")"
	}
	if f := findDownloadKind(t, settings); f != "" {
		return installFrom(game, t, f, settings)
	}
	page := fmt.Sprintf("https://www.nexusmods.com/newvegas/mods/%d?tab=files", t.ID)
	app.step(1, scene.Warn)
	if settings {
		app.status("%s also needs \"%s\": click Manual download on it (same page, Main files).", t.Name, t.NeedFile)
	} else {
		if t.Root {
			app.status("%s: click Manual download on \"4GB Patcher\" (Main files) on its Nexus page. Setup does the rest.", t.Name)
		} else {
			app.status("%s: click Manual download on its Nexus page (Main files). Setup puts it in by itself.", t.Name)
		}
	}
	app.logf("%s: not downloaded yet - opening %s (needs a free Nexus Mods account; use Manual download, not Mod manager download). Setup watches %s, or press CHOOSE THE FILE.", what, page, redact(knownFolder(folderDownloads)))
	messageBox(nexusHint(t, settings), "Mojave Enhanced - get "+t.Name, 0x40)
	openURL(page)
	app.button("GET "+strings.ToUpper(t.Name), scene.BtnAction, func() { openURL(page) })
	drainPicked()
	waitPills(true)
	defer waitPills(false)
	start := time.Now()
	for {
		select {
		case p := <-picked:
			app.logf("%s: you chose %s", what, redact(p))
			if err := installFrom(game, t, p, settings); err != nil {
				app.status("%s", err.Error())
				continue
			}
			app.button("WORKING", scene.BtnWorking, nil)
			app.step(1, scene.Active)
			return nil
		case <-time.After(1500 * time.Millisecond):
		}
		if f := findDownloadKind(t, settings); f != "" {
			app.button("WORKING", scene.BtnWorking, nil)
			app.step(1, scene.Active)
			return installFrom(game, t, f, settings)
		}
		if time.Since(start) > 2*time.Hour {
			return fmt.Errorf("Still waiting for %s from Nexus: download it, then press the button again.", what)
		}
	}
}

// Where downloads can be: the Downloads folder first, then the desktop, setup's own folder, the mod managers'
func downloadDirs() []string {
	var ds []string
	add := func(d string) {
		if d == "" {
			return
		}
		for _, x := range ds {
			if strings.EqualFold(x, d) {
				return
			}
		}
		if st, err := os.Stat(d); err == nil && st.IsDir() {
			ds = append(ds, d)
		}
	}
	add(knownFolder(folderDownloads))
	add(knownFolder(folderDesktop))
	if exe, err := os.Executable(); err == nil {
		add(filepath.Dir(exe))
	}
	add(filepath.Join(os.Getenv("APPDATA"), "Vortex", "downloads", "falloutnv"))
	add(filepath.Join(os.Getenv("APPDATA"), "Vortex", "downloads", "newvegas"))
	if ms, _ := filepath.Glob(filepath.Join(os.Getenv("LOCALAPPDATA"), "ModOrganizer", "*", "downloads")); len(ms) > 0 {
		for _, m := range ms {
			add(m)
		}
	}
	return ds
}

func findDownloadKind(t nexusTool, wantINI bool) string {
	re := regexp.MustCompile(fmt.Sprintf(`(^|[^0-9])%d([^0-9]|$)`, t.ID))
	iniRe := regexp.MustCompile(`(?i)(^|[^a-z])ini([^a-z]|$)`)
	type cand struct {
		p   string
		t   time.Time
		dir bool
	}
	var cs []cand
	for _, dl := range downloadDirs() {
		ents, _ := os.ReadDir(dl)
		for _, e := range ents {
			n := e.Name()
			low := strings.ToLower(n)
			if !re.MatchString(n) {
				continue
			}
			base := n
			if !e.IsDir() {
				if !(strings.HasSuffix(low, ".7z") || strings.HasSuffix(low, ".zip")) {
					continue
				}
				base = strings.TrimSuffix(n, filepath.Ext(n))
			}
			if iniRe.MatchString(base) != wantINI {
				continue
			}
			info, err := e.Info()
			if err != nil || (!e.IsDir() && info.Size() < 300) {
				continue
			}
			if e.IsDir() && !dirHas(filepath.Join(dl, n), t, wantINI) {
				continue
			}
			cs = append(cs, cand{filepath.Join(dl, n), info.ModTime(), e.IsDir()})
		}
	}
	sort.Slice(cs, func(i, j int) bool { return cs[i].t.After(cs[j].t) })
	for _, c := range cs {
		if c.dir {
			return c.p
		}
		a, _ := os.Stat(c.p)
		time.Sleep(700 * time.Millisecond)
		b, _ := os.Stat(c.p)
		if a != nil && b != nil && a.Size() == b.Size() {
			return c.p
		}
	}
	return ""
}

// An unpacked download: does it hold the file we're after?
func dirHas(dir string, t nexusTool, wantINI bool) bool {
	want := t.DLL
	if wantINI {
		want = t.Need
	}
	if want == "" {
		return false
	}
	found := false
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() && strings.EqualFold(d.Name(), want) {
			found = true
			return filepath.SkipAll
		}
		return nil
	})
	return found
}

// The files of a download: an archive, an unpacked folder, or the single.dll /.ini itself
func readDownload(p string) (map[string][]byte, error) {
	st, err := os.Stat(p)
	if err != nil {
		return nil, err
	}
	if st.IsDir() {
		out := map[string][]byte{}
		total := int64(0)
		err := filepath.WalkDir(p, func(f string, d os.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return err
			}
			info, _ := d.Info()
			if info != nil {
				total += info.Size()
			}
			if total > 200<<20 {
				return errors.New("that folder is too big to be the download")
			}
			b, err := os.ReadFile(f)
			if err != nil {
				return err
			}
			rel, _ := filepath.Rel(p, f)
			out[filepath.ToSlash(rel)] = b
			return nil
		})
		return out, err
	}
	b, err := os.ReadFile(p)
	if err != nil {
		return nil, err
	}
	low := strings.ToLower(p)
	if strings.HasSuffix(low, ".dll") || strings.HasSuffix(low, ".ini") {
		return map[string][]byte{filepath.Base(p): b}, nil
	}
	return readArchive(b, p)
}

// Puts a download in (settings: the settings file, never over one that's there already)
func installFrom(game string, t nexusTool, file string, settings bool) error {
	if !settings {
		return installNexus(game, t, file)
	}
	files, err := readDownload(file)
	if err != nil {
		return fmt.Errorf("%s: couldn't open %s (%v). Download it again.", t.Name, filepath.Base(file), err)
	}
	n := 0
	for name, data := range files {
		rel := nexusRel(files, name)
		if rel == "" && strings.EqualFold(path.Base(name), t.Need) {
			rel = "NVSE/Plugins/" + t.Need
		}
		if rel == "" {
			continue
		}
		dst := filepath.Join(dataDir(game), filepath.FromSlash(rel))
		if exists(dst) {
			continue
		}
		if err := putFile(dst, data); err != nil {
			return fmt.Errorf("%s: %s: %v", t.Name, rel, err)
		}
		n++
	}
	if !exists(filepath.Join(pluginDir(game), t.Need)) {
		return fmt.Errorf("%s: %s didn't have %s - is it the \"%s\" file?", t.Name, filepath.Base(file), t.Need, t.NeedFile)
	}
	app.logf("%s: its settings file %s put in from %s (%d files).", t.Name, t.Need, filepath.Base(file), n)
	return nil
}

func installNexus(game string, t nexusTool, file string) error {
	app.status("Putting in %s...", t.Name)
	files, err := readDownload(file)
	if err != nil {
		return fmt.Errorf("%s: couldn't open %s (%v). Download it again.", t.Name, filepath.Base(file), err)
	}
	if t.Root {
		pick := ""
		for name := range files {
			b := strings.ToLower(path.Base(name))
			if strings.EqualFold(b, t.DLL) {
				pick = name
				break
			}
			if strings.HasSuffix(b, ".exe") && strings.Contains(b, "patch") && pick == "" {
				pick = name
			}
		}
		for name, data := range files {
			if name == pick {
				if err := putFile(filepath.Join(game, t.DLL), data); err != nil {
					return fmt.Errorf("%s: %v", t.Name, err)
				}
				app.logf("%s: %s put in from %s", t.Name, t.DLL, filepath.Base(file))
				return nil
			}
		}
		return fmt.Errorf("%s: %s didn't have %s - is it the right file (\"4GB Patcher\", Main files)?", t.Name, filepath.Base(file), t.DLL)
	}
	n := 0
	for name, data := range files {
		rel := nexusRel(files, name)
		if rel == "" {
			continue
		}
		if err := putFile(filepath.Join(dataDir(game), filepath.FromSlash(rel)), data); err != nil {
			return fmt.Errorf("%s: %s: %v", t.Name, rel, err)
		}
		n++
	}
	if !exists(filepath.Join(pluginDir(game), t.DLL)) {
		return fmt.Errorf("%s: %s didn't have %s - is it the right file? Open details and send it.", t.Name, filepath.Base(file), t.DLL)
	}
	app.logf("%s %s: installed from %s (%d files).", t.Name, fileVersion(filepath.Join(pluginDir(game), t.DLL)), filepath.Base(file), n)
	if ini := findDownloadKind(t, true); ini != "" {
		if fs, err := readDownload(ini); err == nil {
			for name, data := range fs {
				if rel := nexusRel(fs, name); rel != "" && !exists(filepath.Join(dataDir(game), filepath.FromSlash(rel))) {
					putFile(filepath.Join(dataDir(game), filepath.FromSlash(rel)), data)
				}
			}
			app.logf("%s: its settings file from %s too", t.Name, filepath.Base(ini))
		}
	}
	return nil
}

func hiddenCmd(name string, args ...string) *exec.Cmd {
	c := exec.Command(name, args...)
	c.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000}
	return c
}
