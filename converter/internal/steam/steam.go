// Package steam finds installed Steam games: Steam's folder from the registry (or the usual places), every library from
// steamapps/libraryfolders.vdf, then steamapps/appmanifest_<appid>.acf for the game's folder.
package steam

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// Roots returns Steam install folders that exist.
func Roots() []string {
	var out []string
	seen := map[string]bool{}
	add := func(p string) {
		if p == "" {
			return
		}
		p = filepath.Clean(strings.ReplaceAll(p, "/", string(filepath.Separator)))
		k := strings.ToLower(p)
		if seen[k] {
			return
		}
		if st, err := os.Stat(filepath.Join(p, "steamapps")); err == nil && st.IsDir() {
			seen[k] = true
			out = append(out, p)
		}
	}
	for _, p := range registryRoots() {
		add(p)
	}
	for _, p := range []string{`C:\Program Files (x86)\Steam`, `C:\Program Files\Steam`, `C:\Steam`, `D:\Steam`, `D:\SteamLibrary`, `C:\Games\Steam`} {
		add(p)
	}
	return out
}

var reVDFPath = regexp.MustCompile(`"path"\s+"([^"]+)"`)
var reInstallDir = regexp.MustCompile(`"installdir"\s+"([^"]+)"`)

// Libraries lists every Steam library folder (each holds steamapps/).
func Libraries() []string {
	var out []string
	seen := map[string]bool{}
	for _, root := range Roots() {
		for _, lib := range append([]string{root}, libsFromVDF(filepath.Join(root, "steamapps", "libraryfolders.vdf"))...) {
			k := strings.ToLower(filepath.Clean(lib))
			if !seen[k] {
				seen[k] = true
				out = append(out, filepath.Clean(lib))
			}
		}
	}
	return out
}

func libsFromVDF(path string) []string {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var out []string
	for _, m := range reVDFPath.FindAllStringSubmatch(string(b), -1) {
		out = append(out, strings.ReplaceAll(m[1], `\\`, `\`))
	}
	return out
}

// Find returns the install folder of a Steam app: from its appmanifest, or the expected folder name in any library.
func Find(appID string, dirName string) string {
	for _, lib := range Libraries() {
		man := filepath.Join(lib, "steamapps", "appmanifest_"+appID+".acf")
		if b, err := os.ReadFile(man); err == nil {
			if m := reInstallDir.FindStringSubmatch(string(b)); m != nil {
				p := filepath.Join(lib, "steamapps", "common", m[1])
				if st, err := os.Stat(p); err == nil && st.IsDir() {
					return p
				}
			}
		}
		p := filepath.Join(lib, "steamapps", "common", dirName)
		if st, err := os.Stat(p); err == nil && st.IsDir() {
			return p
		}
	}
	return ""
}
