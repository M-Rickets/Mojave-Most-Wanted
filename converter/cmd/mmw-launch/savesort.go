package main

// Saves that sort themselves.

import (
	"bytes"
	"io"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// saveHasTTW: the save's plugin list (near its start, after the screenshot) names Tale of Two Wastelands' master
func saveHasTTW(path string) (bool, bool) {
	f, err := os.Open(path)
	if err != nil {
		return false, false
	}
	defer f.Close()
	b := make([]byte, 4<<20)
	n, _ := io.ReadFull(f, b)
	b = b[:n]
	if !bytes.HasPrefix(b, []byte("FO3SAVEGAME")) {
		return false, false
	}
	return bytes.Contains(bytes.ToLower(b), []byte("taleoftwowastelands.esm")), true
}

// sortSaves: root is My Games\FalloutNV, base the New Vegas save folder (the player's own, default Saves\).
func sortSaves(root, base string) {
	const ttwRel = `Saves\TTW\`
	dir := func(rel string) string {
		return filepath.Join(root, filepath.FromSlash(strings.ReplaceAll(strings.TrimSuffix(rel, `\`), `\`, "/")))
	}
	type folder struct {
		rel    string
		ttw    bool
		legacy bool
	}
	folders := []folder{{base, false, false}, {ttwRel, true, false}, {`Saves\Vanilla\`, false, true}, {`Saves\TTW Vanilla\`, true, true}}
	for _, from := range folders {
		ents, err := os.ReadDir(dir(from.rel))
		if err != nil {
			continue
		}
		for _, e := range ents {
			if e.IsDir() || !strings.EqualFold(filepath.Ext(e.Name()), ".fos") {
				continue
			}
			ttw, ok := saveHasTTW(filepath.Join(dir(from.rel), e.Name()))
			if !ok || (ttw == from.ttw && !from.legacy) {
				continue
			}
			to := base
			if ttw {
				to = ttwRel
			}
			os.MkdirAll(dir(to), 0o755)
			stem := strings.TrimSuffix(e.Name(), filepath.Ext(e.Name()))
			var group []string
			for _, g := range ents {
				if !g.IsDir() && strings.HasPrefix(strings.ToLower(g.Name()), strings.ToLower(stem)+".") {
					group = append(group, g.Name())
				}
			}
			newStem := stem
			for n := 1; ; n++ {
				taken := false
				for _, g := range group {
					taken = taken || exists(filepath.Join(dir(to), newStem+g[len(stem):]))
				}
				if !taken {
					break
				}
				newStem = stem + " (moved)"
				if n > 1 {
					newStem = stem + " (moved " + strconv.Itoa(n) + ")"
				}
			}
			for _, name := range group {
				src, dst := filepath.Join(dir(from.rel), name), filepath.Join(dir(to), newStem+name[len(stem):])
				if err := os.Rename(src, dst); err != nil {
					log.Printf("saves: %s couldn't be moved to %s: %v", name, to, err)
					continue
				}
				log.Printf("saves: %s%s was made %s - moved to %s%s", from.rel, name, map[bool]string{true: "with TTW", false: "without TTW"}[ttw], to, filepath.Base(dst))
			}
		}
		if from.legacy && os.Remove(dir(from.rel)) == nil {
			log.Printf("saves: %s emptied and removed", from.rel)
		}
	}
}
