package main

// Our in-game screens are lettered in the game's own fonts, the ones its menus use: font 1 (the HUD's, with its glow),
// font 2 (the menus' text and buttons) and font 6 (the menus' titles).

import (
	"bytes"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"mmw/internal/bsa"
)

var gameFontDefaults = map[int]string{
	1: `Textures\Fonts\Glow_Monofonto_Large.fnt`,
	2: `Textures\Fonts\Monofonto_Large.fnt`,
	6: `Textures\Fonts\Glow_Monofonto_VL_dialogs.fnt`,
}

func gameFonts(data, pluginDir string) int {
	dir := filepath.Join(pluginDir, "gamefont")
	var archives []*bsa.Archive
	bsas, _ := filepath.Glob(filepath.Join(data, "*.bsa"))
	sort.Strings(bsas)
	for _, p := range bsas {
		if a, err := bsa.Open(p); err == nil {
			archives = append(archives, a)
		}
	}
	defer func() {
		for _, a := range archives {
			a.Close()
		}
	}()
	read := func(rel string) []byte {
		rel = strings.ReplaceAll(rel, "/", `\`)
		if b, err := os.ReadFile(filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(rel, `\`, "/")))); err == nil {
			return b
		}
		want := strings.ToLower(rel)
		var got []byte
		for _, a := range archives {
			for _, f := range a.Files {
				if f.Path == want {
					if b, err := a.Read(f); err == nil {
						got = b
					}
				}
			}
		}
		return got
	}
	n := 0
	for _, i := range []int{1, 2, 6} {
		rel, _ := gameIniValue("Fonts", "sFontFile_"+strconv.Itoa(i))
		if rel == "" {
			rel = gameFontDefaults[i]
		}
		fnt := read(rel)
		if len(fnt) < 296+256*56 {
			log.Printf("game font %d (%s): not found - our own lettering for it", i, rel)
			continue
		}
		name := fnt[12:296]
		if k := bytes.IndexByte(name, 0); k >= 0 {
			name = name[:k]
		}
		texRel := filepath.ToSlash(filepath.Dir(filepath.FromSlash(strings.ReplaceAll(rel, `\`, "/")))) + "/" + string(name) + ".tex"
		tex := read(texRel)
		if len(tex) < 8 {
			log.Printf("game font %d (%s): its letters (%s) not found - our own lettering for it", i, rel, texRel)
			continue
		}
		os.MkdirAll(dir, 0o755)
		for _, f := range []struct {
			ext string
			b   []byte
		}{{".fnt", fnt}, {".tex", tex}} {
			p := filepath.Join(dir, "font"+strconv.Itoa(i)+f.ext)
			if old, err := os.ReadFile(p); err != nil || !bytes.Equal(old, f.b) {
				os.WriteFile(p, f.b, 0o644)
			}
		}
		log.Printf("game font %d: %s (letters %s)", i, rel, texRel)
		n++
	}
	return n
}
