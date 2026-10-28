//go:build windows

package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/bodgit/sevenzip"

	"mmw/cmd/mmw-setup/scene"
	"mmw/internal/packkey"
)

// Local add-ons: optional packs that live only on this PC.
type addon struct {
	Name          string   `json:"name"`
	On            []string `json:"on"`
	Off           []string `json:"off"`
	InWhen        string   `json:"installedWhen"`
	Group         string   `json:"group"`
	KeepInVanilla bool     `json:"keepInVanilla"`
	Hidden        bool     `json:"hidden"`
	ComesWith     []string `json:"comesWith"`
}

func addonsDir() string {
	return filepath.Join(knownFolder(folderLocalAppData), "MojaveMostWanted", "addons")
}

func loadAddons() []addon {
	files, _ := filepath.Glob(filepath.Join(addonsDir(), "*.json"))
	var out []addon
	for _, f := range files {
		b, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		b = bytes.TrimPrefix(b, []byte("\xef\xbb\xbf"))
		var a addon
		if err := json.Unmarshal(b, &a); err != nil || strings.TrimSpace(a.Name) == "" || len(a.On) == 0 || len(a.Off) == 0 {
			app.logf("Local add-on %s skipped: not a valid add-on file", filepath.Base(f))
			continue
		}
		out = append(out, a)
	}
	sort.Slice(out, func(i, j int) bool { return strings.ToLower(out[i].Name) < strings.ToLower(out[j].Name) })
	return out
}

func addonExpand(s, game string) string { return strings.ReplaceAll(s, "{game}", game) }

func (a addon) in(game string) bool {
	return game != "" && a.InWhen != "" && exists(addonExpand(a.InWhen, game))
}

// The add-ons for the panel (by heading, then name), and how many are in
func addonItems(game string) (labels []string, on []bool, groups []string, nOn int) {
	if game == "" {
		return
	}
	as := loadAddons()
	sort.SliceStable(as, func(i, j int) bool { return strings.ToLower(as[i].Group) < strings.ToLower(as[j].Group) })
	for _, a := range as {
		if a.Hidden {
			continue
		}
		in := a.in(game)
		labels, on, groups = append(labels, strings.ToUpper(a.Name)), append(on, in), append(groups, a.Group)
		if in {
			nOn++
		}
	}
	return
}

// A pill was pressed: true if it was an add-on's
func addonPressed(name string) bool {
	var hit *addon
	for _, a := range loadAddons() {
		if strings.ToLower(a.Name) == name && !a.Hidden {
			a := a
			hit = &a
			break
		}
	}
	if hit == nil {
		return false
	}
	app.mu.Lock()
	game, busy := app.game, app.busy
	app.mu.Unlock()
	if game == "" || busy {
		return true
	}
	if p := processRunning("FalloutNV.exe", "mmw-launch.exe"); p != "" {
		app.status("Close Fallout: New Vegas first (%s is running), then switch %s.", p, hit.Name)
		return true
	}
	turnOn := !hit.in(game)
	q := fmt.Sprintf("Turn %s on?\n\nThis puts the %s pack into Fallout: New Vegas and backs up anything it replaces. It can take a few minutes.", hit.Name, hit.Name)
	var deps []addon
	if !turnOn {
		q = fmt.Sprintf("Turn %s off?\n\nThis takes the %s pack out of Fallout: New Vegas and puts back anything it replaced.", hit.Name, hit.Name)
		deps = addonDependents(*hit, game, loadAddons())
		var names []string
		for _, d := range deps {
			if !d.Hidden {
				names = append(names, d.Name)
			}
		}
		if len(names) > 0 {
			q = fmt.Sprintf("%s needs %s.\n\nTurn %s off too? (No leaves everything as it is.)", strings.Join(names, " and "), hit.Name, strings.Join(append(names, hit.Name), ", "))
		}
	}
	if messageBox(q, hit.Name, 0x34) != 6 {
		return true
	}
	go func() {
		for _, d := range deps {
			switchAddon(d, game, false)
		}
		switchAddon(*hit, game, turnOn)
		autoAddons(game)
	}()
	return true
}

func switchAddon(a addon, game string, turnOn bool) {
	app.mu.Lock()
	app.busy = true
	app.mu.Unlock()
	word, args := "on", a.On
	if !turnOn {
		word, args = "off", a.Off
	}
	app.button("WORKING", scene.BtnWorking, nil)
	app.progress(-1)
	ok := true
	if turnOn {
		for _, p := range addonProviders(a, game, loadAddons()) {
			app.status("Switching %s on first (%s needs it)...", p.Name, a.Name)
			if !runAddon(p, game, p.On) || !p.in(game) {
				app.logf("%s needs %s, which didn't switch on", a.Name, p.Name)
				ok = false
				break
			}
		}
	}
	app.status("Switching %s %s...", a.Name, word)
	ok = ok && runAddon(a, game, args)
	app.mu.Lock()
	app.busy = false
	app.mu.Unlock()
	app.progress(1)
	app.button("PLAY", scene.BtnDone, func() { go play() })
	switch {
	case ok && a.in(game) == turnOn:
		app.status("%s is %s.", a.Name, word)
	case ok:
		app.status("%s finished, but it doesn't look switched %s: see details.", a.Name, word)
	default:
		app.status("%s couldn't be switched %s: see details.", a.Name, word)
	}
	netPills()
}

// autoAddons: the hidden add-ons in while all they come with are in, out otherwise.
var autoRefused = map[string]bool{}

// autoPlan: which hidden add-ons go in (all they come with are in) and which come out (one of them isn't)
func autoPlan(all []addon, game string, refused map[string]bool) (in, out []addon) {
	on := map[string]bool{}
	for _, a := range all {
		if !a.Hidden && a.in(game) {
			on[strings.ToLower(a.Name)] = true
		}
	}
	for _, h := range all {
		if !h.Hidden || len(h.ComesWith) == 0 {
			continue
		}
		want := true
		for _, w := range h.ComesWith {
			want = want && on[strings.ToLower(strings.TrimSpace(w))]
		}
		if want && !h.in(game) && !refused[strings.ToLower(h.Name)] {
			in = append(in, h)
		} else if !want && h.in(game) {
			out = append(out, h)
		}
	}
	return
}

func autoAddons(game string) {
	type job struct {
		a  addon
		on bool
	}
	var jobs []job
	ins, outs := autoPlan(loadAddons(), game, autoRefused)
	for _, a := range outs {
		jobs = append(jobs, job{a, false})
	}
	for _, a := range ins {
		jobs = append(jobs, job{a, true})
	}
	if len(jobs) == 0 {
		return
	}
	app.mu.Lock()
	if app.busy {
		app.mu.Unlock()
		return
	}
	app.busy = true
	app.mu.Unlock()
	app.button("WORKING", scene.BtnWorking, nil)
	for _, j := range jobs {
		with := strings.Join(j.a.ComesWith, " and ")
		if j.on {
			app.status("Putting %s in (it comes with %s)...", j.a.Name, with)
			if !runAddon(j.a, game, j.a.On) || !j.a.in(game) {
				autoRefused[strings.ToLower(j.a.Name)] = true
				app.logf("%s stays out (it would come with %s): see the lines above", j.a.Name, with)
			} else {
				app.logf("%s is in (it comes with %s)", j.a.Name, with)
			}
		} else {
			app.status("Taking %s out (it comes with %s)...", j.a.Name, with)
			runAddon(j.a, game, j.a.Off)
		}
	}
	app.mu.Lock()
	app.busy = false
	app.mu.Unlock()
	app.button("PLAY", scene.BtnDone, func() { go play() })
	app.status("All set. Press PLAY to drive.")
	netPills()
}

// Runs one of an add-on's commands hidden, its output into details and the log
func runAddon(a addon, game string, args []string) bool {
	cmdArgs := make([]string, len(args))
	for i, s := range args {
		cmdArgs[i] = addonExpand(s, game)
	}
	c := hiddenCmd(cmdArgs[0], cmdArgs[1:]...)
	out, err := c.StdoutPipe()
	if err != nil {
		app.logf("%s: %v", a.Name, err)
		return false
	}
	c.Stderr = c.Stdout
	if err := c.Start(); err != nil {
		app.logf("%s: %v", a.Name, err)
		return false
	}
	sc := bufio.NewScanner(out)
	sc.Buffer(make([]byte, 64*1024), 1024*1024)
	for sc.Scan() {
		if l := strings.TrimRight(sc.Text(), " \r"); l != "" {
			app.logf("[%s] %s", a.Name, l)
		}
	}
	if err := c.Wait(); err != nil {
		app.logf("%s: %v", a.Name, err)
		return false
	}
	return true
}

// Undo everything: add-ons come out first, while the tools they rely on are still there
func addonsOffAll(game string) {
	for _, a := range loadAddons() {
		if a.in(game) {
			app.status("Switching %s off first...", a.Name)
			runAddon(a, game, a.Off)
		}
	}
}

// addonNeeds: the files an add-on's switch-on command says must be in Data first (mmw-pack --needs)
func addonNeeds(a addon) []string {
	var out []string
	for i, s := range a.On {
		switch {
		case (s == "--needs" || s == "-needs") && i+1 < len(a.On):
			out = append(out, a.On[i+1])
		case strings.HasPrefix(s, "--needs="):
			out = append(out, strings.TrimPrefix(s, "--needs="))
		case strings.HasPrefix(s, "-needs="):
			out = append(out, strings.TrimPrefix(s, "-needs="))
		}
	}
	return out
}

// addonProviders: what a needs that isn't in the game's Data yet, and the add-ons that put it in and aren't in - in the
// order to switch them on.
func addonProviders(a addon, game string, all []addon) []addon {
	var out []addon
	seen := map[string]bool{strings.ToLower(a.Name): true}
	var walk func(x addon)
	walk = func(x addon) {
		for _, n := range addonNeeds(x) {
			if exists(filepath.Join(game, "Data", filepath.FromSlash(strings.ReplaceAll(n, `\`, "/")))) {
				continue
			}
			for _, p := range all {
				if seen[strings.ToLower(p.Name)] || p.in(game) || !addonProvides(p, n, game) {
					continue
				}
				seen[strings.ToLower(p.Name)] = true
				walk(p)
				out = append(out, p)
				break
			}
		}
	}
	walk(a)
	return out
}

// addonDependents: the add-ons that are in and need a file p puts in (those that need them first), for switching p off
func addonDependents(p addon, game string, all []addon) []addon {
	var out []addon
	seen := map[string]bool{strings.ToLower(p.Name): true}
	var walk func(x addon)
	walk = func(x addon) {
		for _, a := range all {
			if seen[strings.ToLower(a.Name)] || !a.in(game) {
				continue
			}
			for _, n := range addonNeeds(a) {
				if addonProvides(x, n, game) {
					seen[strings.ToLower(a.Name)] = true
					walk(a)
					out = append(out, a)
					break
				}
			}
		}
	}
	walk(p)
	return out
}

func normRel(p string) string {
	return strings.Trim(strings.ToLower(strings.ReplaceAll(filepath.ToSlash(filepath.Clean(p)), `\`, "/")), "/")
}

// addonProvides: p puts the Data-relative file need into the game
func addonProvides(p addon, need, game string) bool {
	want := normRel(need)
	if p.InWhen != "" && normRel(addonExpand(p.InWhen, game)) == normRel(filepath.Join(game, "Data", need)) {
		return true
	}
	for i, s := range p.On {
		var src string
		switch {
		case (s == "--src" || s == "-src") && i+1 < len(p.On):
			src = p.On[i+1]
		case strings.HasPrefix(s, "--src="):
			src = strings.TrimPrefix(s, "--src=")
		default:
			continue
		}
		src = addonExpand(src, game)
		if d := packkey.Ready(game, src); d != "" {
			src = d
		}
		if srcHolds(src, want) {
			return true
		}
	}
	return false
}

// srcHolds: a pack source (folder,.zip or.7z) has the file want (lower-case, / separated) at its root, under a Data
// folder, or under any one folder (an archive's top folder)
func srcHolds(src, want string) bool {
	match := func(rel string) bool {
		r := normRel(rel)
		return r == want || strings.HasSuffix(r, "/"+want)
	}
	st, err := os.Stat(src)
	if err != nil {
		return false
	}
	if st.IsDir() {
		depth := strings.Count(want, "/") + 2
		found := false
		filepath.WalkDir(src, func(p string, d os.DirEntry, err error) error {
			if err != nil || found {
				return filepath.SkipDir
			}
			rel, _ := filepath.Rel(src, p)
			n := strings.Count(normRel(rel), "/") + 1
			if d.IsDir() {
				if rel != "." && n > depth {
					return filepath.SkipDir
				}
				return nil
			}
			found = match(rel)
			return nil
		})
		return found
	}
	switch strings.ToLower(filepath.Ext(src)) {
	case ".zip":
		r, err := zip.OpenReader(src)
		if err != nil {
			return false
		}
		defer r.Close()
		for _, f := range r.File {
			if match(f.Name) {
				return true
			}
		}
	case ".7z":
		r, err := sevenzip.OpenReader(src)
		if err != nil {
			return false
		}
		defer r.Close()
		for _, f := range r.File {
			if match(f.Name) {
				return true
			}
		}
	}
	return false
}
