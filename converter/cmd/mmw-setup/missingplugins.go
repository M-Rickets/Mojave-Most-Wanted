package main

// The report lists each plugin the load order names that Data doesn't have, and where on this PC a copy of it is

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

var reVortexInstall = regexp.MustCompile(`(?s)installPath.{0,40}?"((?:[A-Za-z]:|\{USERDATA\})[^"\x00]{1,240})"`)

// The folders mod managers keep their mods in (each holds one folder per mod)
func modStagingRoots(game string) []string {
	var roots []string
	seen := map[string]bool{}
	add := func(p string) {
		if p == "" {
			return
		}
		k := strings.ToLower(filepath.Clean(p))
		if seen[k] {
			return
		}
		if st, err := os.Stat(p); err == nil && st.IsDir() {
			seen[k] = true
			roots = append(roots, p)
		}
	}
	appdata := os.Getenv("APPDATA")
	for _, g := range []string{"falloutnv", "newvegas"} {
		add(filepath.Join(appdata, "Vortex", g, "mods"))
		for _, d := range drives() {
			add(filepath.Join(d, "Vortex Mods", g))
			add(filepath.Join(d, "Vortex", g, "mods"))
			add(filepath.Join(d, "Modding", "Vortex Mods", g))
			add(filepath.Join(d, "Games", "Vortex Mods", g))
			add(filepath.Join(d, "Mods", "Vortex Mods", g))
		}
	}
	if fs, _ := filepath.Glob(filepath.Join(appdata, "Vortex", "state.v2", "*")); len(fs) > 0 {
		for _, f := range fs {
			if st, err := os.Stat(f); err != nil || st.IsDir() || st.Size() > 64<<20 {
				continue
			}
			bs, err := os.ReadFile(f)
			if err != nil {
				continue
			}
			for _, m := range reVortexInstall.FindAllSubmatch(bs, -1) {
				v := strings.ReplaceAll(string(m[1]), `\\`, `\`)
				for _, g := range []string{"falloutnv", "newvegas"} {
					q := strings.NewReplacer("{GAME}", g, "{game}", g, "{USERDATA}", filepath.Join(appdata, "Vortex"), "{userdata}", filepath.Join(appdata, "Vortex")).Replace(v)
					add(q)
				}
			}
		}
	}
	var inis []string
	if m, _ := filepath.Glob(filepath.Join(os.Getenv("LOCALAPPDATA"), "ModOrganizer", "*", "ModOrganizer.ini")); len(m) > 0 {
		inis = append(inis, m...)
	}
	for _, d := range drives() {
		for _, sub := range []string{"Modding", "MO2", "Mod Organizer 2", "Games"} {
			if m, _ := filepath.Glob(filepath.Join(d, sub, "*", "ModOrganizer.ini")); len(m) > 0 {
				inis = append(inis, m...)
			}
		}
	}
	for _, ini := range inis {
		b, err := os.ReadFile(ini)
		if err != nil {
			continue
		}
		s := string(b)
		base := filepath.Dir(ini)
		if bd := iniValue(s, "base_directory"); bd != "" {
			base = strings.ReplaceAll(bd, `\\`, `\`)
		}
		add(filepath.Join(base, "mods"))
	}
	return roots
}

// Where each plugin file (lower-case name) is in the mods folders: the mod's folder, its root or one level down
func findInStaging(roots []string, want map[string]bool) map[string]string {
	found := map[string]string{}
	look := func(dir string) {
		es, err := os.ReadDir(dir)
		if err != nil {
			return
		}
		for _, e := range es {
			n := strings.ToLower(e.Name())
			if !e.IsDir() && want[n] && found[n] == "" {
				found[n] = dir
			}
		}
	}
	for _, r := range roots {
		mods, err := os.ReadDir(r)
		if err != nil {
			continue
		}
		for _, m := range mods {
			if !m.IsDir() {
				continue
			}
			md := filepath.Join(r, m.Name())
			look(md)
			subs, _ := os.ReadDir(md)
			for _, s := range subs {
				if s.IsDir() {
					look(filepath.Join(md, s.Name()))
				}
			}
		}
	}
	return found
}

// The report's lines: the load order's plugins that Data doesn't have, and where each is
func missingPlugins(game string) []string {
	b, err := os.ReadFile(pluginsTxtPath())
	if err != nil {
		return []string{"(no plugins.txt)"}
	}
	var names []string
	for _, l := range splitLines(string(b)) {
		l = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(l), "*"))
		if l == "" || strings.HasPrefix(l, "#") {
			continue
		}
		low := strings.ToLower(l)
		if strings.HasSuffix(low, ".esm") || strings.HasSuffix(low, ".esp") {
			names = append(names, l)
		}
	}
	have := map[string]bool{}
	if es, err := os.ReadDir(dataDir(game)); err == nil {
		for _, e := range es {
			have[strings.ToLower(e.Name())] = true
		}
	}
	var missing []string
	want := map[string]bool{}
	for _, n := range names {
		if !have[strings.ToLower(n)] {
			missing = append(missing, n)
			want[strings.ToLower(n)] = true
		}
	}
	if len(missing) == 0 {
		return []string{fmt.Sprintf("none - all %d plugins the load order names are in Data", len(names))}
	}
	roots := modStagingRoots(game)
	found := findInStaging(roots, want)
	out := []string{fmt.Sprintf("%d of the %d plugins in the load order are NOT in Data (the game can't load them; a save made with them may crash):", len(missing), len(names))}
	nFound := 0
	for _, n := range missing {
		if d := found[strings.ToLower(n)]; d != "" {
			nFound++
			out = append(out, fmt.Sprintf("  %s - in a mod manager's folder, not deployed: %s", n, redact(d)))
		} else {
			out = append(out, fmt.Sprintf("  %s - not found on this PC", n))
		}
	}
	if nFound > 0 {
		out = append(out, fmt.Sprintf("%d of them are installed in a mod manager but not in the game: in Vortex, press Deploy Mods (Mod Organizer 2: start the game from it)", nFound))
	}
	sort.Strings(roots)
	if len(roots) == 0 {
		out = append(out, "Looked for them in mod managers' folders: none found (Vortex's staging folder may be somewhere else - Vortex > Settings > Mods shows it)")
	} else {
		var rs []string
		for _, r := range roots {
			rs = append(rs, redact(r))
		}
		out = append(out, "Looked in: "+strings.Join(rs, "; "))
	}
	return out
}
