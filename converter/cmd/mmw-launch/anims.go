package main

import (
	"log"
	"os"
	"path/filepath"
	"strings"
)

// pruneAnims keeps Data\meshes\AnimGroupOverride\MojaveMostWanted to the files listed in MojaveMostWanted.files
// (written with them by tools/gen_anims.py)
func pruneAnims(data, managed string) {
	agod := filepath.Join(data, "meshes", "AnimGroupOverride")
	b, err := os.ReadFile(filepath.Join(agod, "MojaveMostWanted.files"))
	if err != nil {
		return
	}
	keep := map[string]bool{}
	for _, l := range strings.Split(string(b), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			keep[strings.ToLower(filepath.FromSlash(strings.ReplaceAll(l, `\`, "/")))] = true
		}
	}
	if len(keep) == 0 {
		return
	}
	root := filepath.Join(agod, "MojaveMostWanted")
	var dirs []string
	n := 0
	filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		if fi.IsDir() {
			dirs = append(dirs, p)
			return nil
		}
		rel, _ := filepath.Rel(root, p)
		if !keep[strings.ToLower(rel)] && os.Remove(p) == nil {
			n++
		}
		return nil
	})
	for i := len(dirs) - 1; i > 0; i-- {
		os.Remove(dirs[i])
	}
	if n > 0 {
		log.Printf("animations: removed %d files no longer in the set", n)
	}
	pruneAnimSounds(data, managed, agod)
}

// pruneAnimSounds removes the sounds an earlier build shipped for the animations (Data\Sound\..., listed in
// MojaveMostWanted\anim-sounds.txt at the last Play) that the current MojaveMostWanted.sounds no longer lists.
func pruneAnimSounds(data, managed, agod string) {
	cur, err := os.ReadFile(filepath.Join(agod, "MojaveMostWanted.sounds"))
	if err != nil {
		return
	}
	now := map[string]bool{}
	for _, l := range strings.Split(string(cur), "\n") {
		if l = strings.TrimSpace(l); l != "" {
			now[strings.ToLower(l)] = true
		}
	}
	prevPath := filepath.Join(managed, "anim-sounds.txt")
	if prev, err := os.ReadFile(prevPath); err == nil {
		n := 0
		for _, l := range strings.Split(string(prev), "\n") {
			if l = strings.TrimSpace(l); l != "" && !now[strings.ToLower(l)] {
				p := filepath.Join(data, "Sound", filepath.FromSlash(strings.ReplaceAll(l, `\`, "/")))
				if os.Remove(p) == nil {
					n++
				} else if _, err := os.Stat(p); err == nil {
					cur = append(cur, []byte(l+"\r\n")...)
				}
			}
		}
		if n > 0 {
			log.Printf("animations: removed %d sounds no longer used", n)
		}
	}
	os.WriteFile(prevPath, cur, 0o644)
}
