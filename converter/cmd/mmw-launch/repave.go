package main

import (
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"
	"strings"

	"mmw/internal/bsa"
	"mmw/internal/dds"
)

const roadsVersion = 2

// repaveRoads gives New Vegas clean roads: each texture in the roads sheet is read from the player's own archives and
// rewritten as a loose file (see sheets/roads.json).
func repaveRoads(data, managed string, enabled bool, extra []roadRow) {
	rows := append(append([]roadRow{}, sheetRoads...), extra...)
	defer func() {
		if r := recover(); r != nil {
			log.Printf("clean roads: failed: %v", r)
		}
	}()
	ours := func(p string) (cacheEntry, bool) {
		cacheMu.Lock()
		defer cacheMu.Unlock()
		e, ok := cache["road:"+p]
		return e, ok
	}
	isOurs := func(full, p string) bool {
		e, ok := ours(p)
		st, err := os.Stat(full)
		return ok && err == nil && e.Sig == fmt.Sprintf("%d:%d", st.Size(), st.ModTime().Unix())
	}
	if !enabled {
		n := 0
		for _, r := range rows {
			full := filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(r.Path, `\`, "/")))
			if isOurs(full, r.Path) {
				os.Remove(full)
				n++
			}
			cacheMu.Lock()
			delete(cache, "road:"+r.Path)
			cacheMu.Unlock()
		}
		log.Printf("clean roads: off (%d repaved textures removed)", n)
		return
	}
	src := filepath.Join(data, "Fallout - Textures2.bsa")
	srcSig := sig(src) + fmt.Sprintf("|roads%d", roadsVersion)
	arcs := map[string]bsa.File{}
	var archives []*bsa.Archive
	for _, name := range []string{"Update.bsa", "Fallout - Textures2.bsa", "Fallout - Textures.bsa"} {
		a, err := bsa.Open(filepath.Join(data, name))
		if err != nil {
			continue
		}
		archives = append(archives, a)
		for _, f := range a.Files {
			if _, ok := arcs[f.Path]; !ok {
				arcs[f.Path] = f
			}
		}
	}
	defer func() {
		for _, a := range archives {
			a.Close()
		}
	}()
	read := func(p string) ([]byte, error) {
		f, ok := arcs[strings.ToLower(p)]
		if !ok {
			return nil, fmt.Errorf("not in the archives")
		}
		for _, a := range archives {
			for _, g := range a.Files {
				if g == f {
					return a.Read(f)
				}
			}
		}
		return nil, fmt.Errorf("archive lost")
	}
	made, kept, same := 0, 0, 0
	for _, r := range rows {
		full := filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(r.Path, `\`, "/")))
		if exists(full) && !isOurs(full, r.Path) {
			log.Printf("clean roads: kept your own %s", r.Path)
			kept++
			continue
		}
		key := "roadsrc:" + r.Path
		cacheMu.Lock()
		prev, had := cache[key]
		cacheMu.Unlock()
		if had && prev.Sig == srcSig && isOurs(full, r.Path) {
			same++
			continue
		}
		b, err := read(r.Path)
		if err != nil {
			log.Printf("clean roads: %s: %v", r.Path, err)
			continue
		}
		w, h, rgba, err := dds.Decode(b)
		if err != nil && (r.Kind != "normal" && r.Kind != "decal") {
			log.Printf("clean roads: %s: %v", r.Path, err)
			continue
		}
		if w <= 0 || h <= 0 || w > 4096 || h > 4096 {
			w, h = 512, 512
		}
		var out []byte
		switch r.Kind {
		case "atlas":
			out = repaveAtlas(rgba, w, h, true)
		case "ground":
			out = repaveAtlas(rgba, w, h, false)
		case "decal":
			out = make([]byte, w*h*4)
			for i := 0; i < w*h; i++ {
				out[i*4], out[i*4+1], out[i*4+2], out[i*4+3] = 50, 50, 52, 0
			}
		case "normal":
			out = flatNormal(w, h)
		default:
			continue
		}
		os.MkdirAll(filepath.Dir(full), 0o755)
		if err := os.WriteFile(full, dds.DXT5Mips(out, w, h), 0o644); err != nil {
			log.Printf("clean roads: %s: %v", r.Path, err)
			continue
		}
		st, _ := os.Stat(full)
		cacheMu.Lock()
		cache["road:"+r.Path] = cacheEntry{Sig: fmt.Sprintf("%d:%d", st.Size(), st.ModTime().Unix())}
		cache[key] = cacheEntry{Sig: srcSig}
		cacheMu.Unlock()
		made++
	}
	log.Printf("clean roads: %d repaved, %d up to date, %d of your own kept", made, same, kept)
}

// Periodic value noise in [0,1): cells per axis, wraps at the texture edge so tiling stays seamless
func periodicNoise(w, h, cells int, seed uint32) []float32 {
	grid := make([]float32, cells*cells)
	s := seed*2654435761 + 1
	for i := range grid {
		s ^= s << 13
		s ^= s >> 17
		s ^= s << 5
		grid[i] = float32(s%10000) / 10000
	}
	out := make([]float32, w*h)
	for y := 0; y < h; y++ {
		fy := float32(y) * float32(cells) / float32(h)
		y0 := int(fy)
		ty := fy - float32(y0)
		ty = ty * ty * (3 - 2*ty)
		for x := 0; x < w; x++ {
			fx := float32(x) * float32(cells) / float32(w)
			x0 := int(fx)
			tx := fx - float32(x0)
			tx = tx * tx * (3 - 2*tx)
			g := func(i, j int) float32 { return grid[((j%cells+cells)%cells)*cells+(i%cells+cells)%cells] }
			a := g(x0, y0)*(1-tx) + g(x0+1, y0)*tx
			b := g(x0, y0+1)*(1-tx) + g(x0+1, y0+1)*tx
			out[y*w+x] = a*(1-ty) + b*ty
		}
	}
	return out
}

// repaveAtlas: fresh asphalt everywhere the texture is opaque; lane markings (yellow or white paint brighter than the
// asphalt around it) found in the original and repainted crisp; alpha kept so the road shapes stay.
func repaveAtlas(src []byte, w, h int, markings bool) []byte {
	n := w * h
	lum := make([]float32, n)
	if src != nil {
		for i := 0; i < n; i++ {
			lum[i] = 0.299*float32(src[i*4]) + 0.587*float32(src[i*4+1]) + 0.114*float32(src[i*4+2])
		}
	}
	rad := w / 48
	if rad < 2 {
		rad = 2
	}
	mean := boxBlur(lum, w, h, rad)
	yellow := make([]bool, n)
	white := make([]bool, n)
	if markings && src != nil {
		for i := 0; i < n; i++ {
			r, g, b := float32(src[i*4]), float32(src[i*4+1]), float32(src[i*4+2])
			mx := math.Max(float64(r), math.Max(float64(g), float64(b)))
			mn := math.Min(float64(r), math.Min(float64(g), float64(b)))
			if r-b > 35 && g-b > 15 && lum[i] > mean[i]+8 {
				yellow[i] = true
			} else if lum[i] > mean[i]+45 && mx-mn < 28 && lum[i] > 140 {
				white[i] = true
			}
		}
		yellow = morph(morph(yellow, w, h, 2, true), w, h, 2, false)
		white = morph(morph(white, w, h, 2, true), w, h, 2, false)
		sp := w / 256
		if sp < 1 {
			sp = 1
		}
		yellow = morph(morph(yellow, w, h, sp, false), w, h, sp, true)
		white = morph(morph(white, w, h, sp+1, false), w, h, sp+1, true)
	}
	fine := periodicNoise(w, h, w/2, 7)
	mid := periodicNoise(w, h, w/16, 11)
	low := periodicNoise(w, h, 4, 13)
	out := make([]byte, n*4)
	for i := 0; i < n; i++ {
		v := 54 + (fine[i]-0.5)*16 + (mid[i]-0.5)*8 + (low[i]-0.5)*6
		r, g, b := v, v+1, v+3
		switch {
		case yellow[i]:
			r, g, b = 222+(fine[i]-0.5)*12, 180+(fine[i]-0.5)*10, 40
		case white[i]:
			c := 226 + (fine[i]-0.5)*12
			r, g, b = c, c, c-4
		}
		out[i*4] = clampByte(r)
		out[i*4+1] = clampByte(g)
		out[i*4+2] = clampByte(b)
		a := byte(255)
		if src != nil {
			a = src[i*4+3]
		}
		out[i*4+3] = a
	}
	return out
}

func clampByte(v float32) byte {
	if v < 0 {
		return 0
	}
	if v > 255 {
		return 255
	}
	return byte(v)
}

func boxBlur(v []float32, w, h, r int) []float32 {
	tmp := make([]float32, len(v))
	out := make([]float32, len(v))
	for y := 0; y < h; y++ {
		var s float32
		for k := -r; k <= r; k++ {
			s += v[y*w+((k%w)+w)%w]
		}
		for x := 0; x < w; x++ {
			tmp[y*w+x] = s / float32(2*r+1)
			s += v[y*w+((x+r+1)%w)] - v[y*w+((x-r)%w+w)%w]
		}
	}
	for x := 0; x < w; x++ {
		var s float32
		for k := -r; k <= r; k++ {
			s += tmp[(((k%h)+h)%h)*w+x]
		}
		for y := 0; y < h; y++ {
			out[y*w+x] = s / float32(2*r+1)
			s += tmp[((y+r+1)%h)*w+x] - tmp[(((y-r)%h+h)%h)*w+x]
		}
	}
	return out
}

// morph: dilate (grow=true) or erode a mask by r pixels (square), wrapping at the edges
func morph(m []bool, w, h, r int, grow bool) []bool {
	out := make([]bool, len(m))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			v := !grow
			for dy := -r; dy <= r && v == !grow; dy++ {
				for dx := -r; dx <= r; dx++ {
					p := m[(((y+dy)%h+h)%h)*w+((x+dx)%w+w)%w]
					if grow && p {
						v = true
						break
					}
					if !grow && !p {
						v = false
						break
					}
				}
			}
			out[y*w+x] = v
		}
	}
	return out
}

// Flat normal map with a hint of grain; alpha = specular strength (New Vegas reads it from the normal map)
func flatNormal(w, h int) []byte {
	fine := periodicNoise(w, h, w/2, 17)
	fine2 := periodicNoise(w, h, w/2, 19)
	out := make([]byte, w*h*4)
	for i := 0; i < w*h; i++ {
		out[i*4] = clampByte(128 + (fine[i]-0.5)*10)
		out[i*4+1] = clampByte(128 + (fine2[i]-0.5)*10)
		out[i*4+2] = 252
		out[i*4+3] = 46
	}
	return out
}
