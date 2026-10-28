package main

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"mmw/internal/bsa"
)

func dumpSkyFiles(data string) {
	dir := filepath.Join(filepath.Dir(data), "MojaveMostWanted", "debug")
	if !exists(dir) {
		return
	}
	out := filepath.Join(dir, "sky.txt")
	names, _ := filepath.Glob(filepath.Join(data, "*.bsa"))
	sort.Strings(names)
	key := ""
	for _, n := range names {
		if fi, err := os.Stat(n); err == nil {
			key += fmt.Sprintf("%s %d;", filepath.Base(n), fi.Size())
		}
	}
	if b, err := os.ReadFile(out); err == nil && strings.HasPrefix(string(b), "# "+key+"\n") {
		return
	}
	want := func(p string) bool {
		if !strings.HasPrefix(p, `textures\`) && !strings.HasPrefix(p, `meshes\`) {
			return false
		}
		if strings.Contains(p, `\sky\`) {
			return true
		}
		for _, w := range []string{"rain", "precip", "snowflake", "raindrop", "drizzle"} {
			if strings.Contains(p, w) && !strings.Contains(p, "terrain") && !strings.Contains(p, "brain") && !strings.Contains(p, "train") {
				return true
			}
		}
		return false
	}
	var b strings.Builder
	fmt.Fprintf(&b, "# %s\n", key)
	n := 0
	for _, name := range names {
		a, err := bsa.Open(name)
		if err != nil {
			continue
		}
		for _, f := range a.Files {
			if !want(f.Path) {
				continue
			}
			fmt.Fprintf(&b, "%s\t%s\n", filepath.Base(name), f.Path)
			n++
			cp := filepath.Join(dir, "sky", filepath.Base(name)+"__"+strings.ReplaceAll(f.Path, `\`, "__"))
			if exists(cp) {
				continue
			}
			if body, err := a.Read(f); err == nil && len(body) < 4<<20 {
				os.MkdirAll(filepath.Dir(cp), 0o755)
				os.WriteFile(cp, body, 0o644)
			}
		}
		a.Close()
	}
	if err := os.WriteFile(out, []byte(b.String()), 0o644); err != nil {
		log.Printf("sky files: %v", err)
		return
	}
	log.Printf("debug: %d sky and precipitation files listed in debug\\sky.txt", n)
}
