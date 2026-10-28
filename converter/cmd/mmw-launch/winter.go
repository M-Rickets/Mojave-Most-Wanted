package main

import (
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"

	"mmw/internal/bsa"
	"mmw/internal/dds"
)

// Winter ground in stages.

const winterVersion = 2
const winterStages = 4

// The stage the plugin's seasons last asked for (season.ini stage=; seasons off in the game just melt it to stage 0,
// the bare ground, so the stages stay in either way); paths.ini winter=0 turns the winter ground off
func winterState(pluginDir string, paths map[string]string) (on bool, stage int) {
	if paths["winter"] == "0" {
		return false, 0
	}
	on = true
	b, err := os.ReadFile(filepath.Join(pluginDir, "season.ini"))
	if err != nil {
		return on, 0
	}
	for _, l := range strings.Split(string(b), "\n") {
		l = strings.TrimSpace(l)
		if v, ok := strings.CutPrefix(l, "stage="); ok {
			stage, _ = strconv.Atoi(v)
		}
	}
	if stage < 0 || stage >= winterStages {
		stage = 0
	}
	return on, stage
}

// The archives holding landscape textures, highest first (the first that has a file wins)
func winterArchives(ttw bool) []string {
	var a []string
	if ttw {
		a = append(a, "TaleOfTwoWastelands - Textures.bsa", "TaleOfTwoWastelands - Main.bsa", "Zeta - Main.bsa", "PointLookout - Main.bsa",
			"BrokenSteel - Main.bsa", "ThePitt - Main.bsa", "Anchorage - Fallout - Textures2.bsa", "Anchorage - Fallout - Textures.bsa",
			"Anchorage - Main.bsa", "Fallout3 - Textures.bsa")
	}
	return append(a, "LonesomeRoad - Main.bsa", "OldWorldBlues - Main.bsa", "HonestHearts - Main.bsa", "DeadMoney - Main.bsa",
		"Update.bsa", "Fallout - Textures2.bsa", "Fallout - Textures.bsa")
}

func winterize(data, managed string, enabled bool, stage int, ttw bool, roads map[string]bool) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("winter: failed: %v", r)
		}
	}()
	wdir := filepath.Join(managed, "winter")
	stageFile := func(k int, p string) string {
		return filepath.Join(wdir, fmt.Sprintf("s%d", k), filepath.FromSlash(strings.ReplaceAll(p, `\`, "/")))
	}
	dataFile := func(p string) string { return filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(p, `\`, "/"))) }
	fsig := func(full string) string {
		st, err := os.Stat(full)
		if err != nil {
			return ""
		}
		return fmt.Sprintf("%d:%d", st.Size(), st.ModTime().Unix())
	}
	isOurs := func(p string) bool {
		s := fsig(dataFile(p))
		if s == "" {
			return false
		}
		cacheMu.Lock()
		defer cacheMu.Unlock()
		if e, ok := cache["winter:"+p]; ok && e.Sig == s {
			return true
		}
		for k := 0; k < winterStages; k++ {
			if e, ok := cache[fmt.Sprintf("winters%d:%s", k, p)]; ok && e.Sig == s {
				return true
			}
		}
		return false
	}
	var archives []*bsa.Archive
	var archiveSigs []string
	type src struct {
		a *bsa.Archive
		f bsa.File
	}
	files := map[string]src{}
	for _, name := range winterArchives(ttw) {
		a, err := bsa.Open(filepath.Join(data, name))
		if err != nil {
			continue
		}
		archives = append(archives, a)
		archiveSigs = append(archiveSigs, name+"="+sig(filepath.Join(data, name)))
		for _, f := range a.Files {
			p := strings.ToLower(f.Path)
			if !strings.HasPrefix(p, `textures\landscape\`) || !strings.HasSuffix(p, ".dds") {
				continue
			}
			if strings.HasSuffix(p, "_n.dds") || strings.Contains(p, `\normals\`) || roads[p] {
				continue
			}
			if _, ok := files[p]; !ok {
				files[p] = src{a, f}
			}
		}
	}
	defer func() {
		for _, a := range archives {
			a.Close()
		}
	}()
	var paths []string
	for p := range files {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	removeOurs := func(p string) bool {
		if isOurs(p) {
			os.Remove(dataFile(p))
			return true
		}
		return false
	}
	if !enabled {
		n := 0
		cacheMu.Lock()
		var keys []string
		for k := range cache {
			if strings.HasPrefix(k, "winter") && strings.Contains(k, `:textures\landscape\`) {
				keys = append(keys, k[strings.Index(k, ":")+1:])
			}
		}
		cacheMu.Unlock()
		seen := map[string]bool{}
		for _, p := range append(paths, keys...) {
			if seen[p] {
				continue
			}
			seen[p] = true
			if removeOurs(p) {
				n++
			}
		}
		os.Remove(filepath.Join(wdir, "list.txt"))
		log.Printf("winter: off (%d snowed textures out of the game; the stages kept in winter\\)", n)
		return
	}
	srcSig := fmt.Sprintf("winter%d/%d|%s", winterVersion, winterStages, strings.Join(archiveSigs, "|"))
	var mu sync.Mutex
	lowDisk := false
	made, same, kept, bad := 0, 0, 0, 0
	var list []string
	jobs := make(chan string)
	var wg sync.WaitGroup
	workers := runtime.NumCPU() - 1
	if workers < 2 {
		workers = 2
	}
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for p := range jobs {
				df := dataFile(p)
				if exists(df) && !isOurs(p) {
					mu.Lock()
					kept++
					mu.Unlock()
					continue
				}
				cacheMu.Lock()
				prev, had := cache["wintersrc:"+p]
				info, hadInfo := cache["winterinfo:"+p]
				cacheMu.Unlock()
				fresh := had && prev.Sig == srcSig && hadInfo
				for k := 0; fresh && k < winterStages; k++ {
					cacheMu.Lock()
					e, ok := cache[fmt.Sprintf("winters%d:%s", k, p)]
					cacheMu.Unlock()
					if !ok || e.Sig != fsig(stageFile(k, p)) {
						fresh = false
					}
				}
				if fresh {
					mu.Lock()
					same++
					list = append(list, p+"\t"+info.Sig)
					mu.Unlock()
					continue
				}
				mu.Lock()
				if !lowDisk && (made%50 == 0) {
					if f := diskFree(data); f != 0 && f < 5<<30 {
						lowDisk = true
						log.Printf("winter: under 5 GB free on the game's drive (%d MB) - no more snowed textures made", f>>20)
					}
				}
				stop := lowDisk
				mu.Unlock()
				if stop {
					mu.Lock()
					bad++
					mu.Unlock()
					continue
				}
				s := files[p]
				b, err := s.a.Read(s.f)
				var w, h int
				var rgba []byte
				if err == nil {
					w, h, rgba, err = dds.Decode(b)
				}
				if err != nil || w <= 0 || h <= 0 || w > 4096 || h > 4096 {
					mu.Lock()
					bad++
					mu.Unlock()
					continue
				}
				lod := strings.Contains(p, `\lod\`)
				max := 1024
				if lod {
					max = 512
				}
				if w > max || h > max {
					rgba, w, h = shrink(rgba, w, h, max)
				}
				ok := true
				var size int
				for k := 0; k < winterStages && ok; k++ {
					out := rgba
					if k > 0 {
						out = snowStage(rgba, w, h, p, lod, float32(k)/float32(winterStages-1))
					}
					enc := dds.DXT5Mips(out, w, h)
					size = len(enc)
					sf := stageFile(k, p)
					os.MkdirAll(filepath.Dir(sf), 0o755)
					os.Remove(sf)
					if err := os.WriteFile(sf, enc, 0o644); err != nil {
						ok = false
						break
					}
					cacheMu.Lock()
					cache[fmt.Sprintf("winters%d:%s", k, p)] = cacheEntry{Sig: fsig(sf)}
					cacheMu.Unlock()
				}
				if !ok {
					mu.Lock()
					bad++
					mu.Unlock()
					continue
				}
				mips := 1
				for cw, ch := w, h; cw > 1 || ch > 1; mips++ {
					cw, ch = cw/2, ch/2
					if cw < 1 {
						cw = 1
					}
					if ch < 1 {
						ch = 1
					}
				}
				i := fmt.Sprintf("%d\t%d\t%d\t%d", w, h, mips, size)
				cacheMu.Lock()
				cache["wintersrc:"+p] = cacheEntry{Sig: srcSig}
				cache["winterinfo:"+p] = cacheEntry{Sig: i}
				delete(cache, "winter:"+p)
				cacheMu.Unlock()
				mu.Lock()
				made++
				list = append(list, p+"\t"+i)
				mu.Unlock()
			}
		}()
	}
	for _, p := range paths {
		jobs <- p
	}
	close(jobs)
	wg.Wait()
	linked := 0
	for _, l := range list {
		p := l[:strings.Index(l, "\t")]
		df, sf := dataFile(p), stageFile(stage, p)
		if a, err := os.Stat(df); err == nil {
			if b, err := os.Stat(sf); err == nil && os.SameFile(a, b) {
				linked++
				continue
			}
		}
		if _, err := os.Stat(sf); err != nil {
			continue
		}
		os.MkdirAll(filepath.Dir(df), 0o755)
		tmp := df + ".mmwnew"
		os.Remove(tmp)
		if err := os.Link(sf, tmp); err != nil {
			continue
		}
		if err := os.Rename(tmp, df); err != nil {
			os.Remove(tmp)
			continue
		}
		linked++
	}
	sort.Strings(list)
	head := fmt.Sprintf("# winter stages %d, linked at stage %d, made from %s; path, width, height, mip levels, bytes (DXT5)\r\n", winterStages, stage, strings.Join(winterArchives(ttw), ", "))
	os.MkdirAll(wdir, 0o755)
	os.WriteFile(filepath.Join(wdir, "list.txt"), []byte(head+strings.Join(list, "\r\n")+"\r\n"), 0o644)
	log.Printf("winter: %d landscape textures in %d stages (%d made, %d up to date), %d of your own kept, %d unreadable; the game's at stage %d (%d linked)",
		len(list), winterStages, made, same, kept, bad, stage, linked)
}

// snowStage: the texture under snow at share f (0 bare.. 1 full).
func snowStage(src []byte, w, h int, p string, lod bool, f float32) []byte {
	n := w * h
	var mean float64
	for i := 0; i < n; i++ {
		mean += 0.3*float64(src[i*4]) + 0.59*float64(src[i*4+1]) + 0.11*float64(src[i*4+2])
	}
	mean = math.Max(mean/float64(n), 8)
	base := float32(0.86)
	if strings.Contains(p, "rock") || strings.Contains(p, "cliff") || strings.Contains(p, "boulder") {
		base = 0.6
	}
	if lod {
		base = 0.9
	}
	noise := periodicNoise(w, h, 6, 23)
	fine := periodicNoise(w, h, 24, 29)
	early := periodicNoise(w, h, 9, 31)
	var hist [256]int
	for _, v := range early {
		hist[int(math.Min(255, math.Max(0, float64(v)*255)))]++
	}
	var cdf [256]float32
	acc := 0
	for i := range hist {
		acc += hist[i]
		cdf[i] = float32(acc) / float32(n)
	}
	out := make([]byte, n*4)
	for i := 0; i < n; i++ {
		r, g, b := float32(src[i*4]), float32(src[i*4+1]), float32(src[i*4+2])
		lum := 0.3*r + 0.59*g + 0.11*b
		det := float32(math.Min(1.45, math.Max(0.55, float64(lum)/mean)))
		cover := base + 0.22*(noise[i]-0.5) + 0.08*(fine[i]-0.5) - 0.35*float32(math.Max(0, float64(0.8-det)))
		if lod {
			cover = base + 0.05*(noise[i]-0.5)
		}
		u := cdf[int(math.Min(255, math.Max(0, float64(early[i])*255)))]
		th := u - 0.15*(det-1)
		grow := (f*1.05 - th) / 0.12
		if f >= 1 {
			grow = 1
		}
		grow = float32(math.Min(1, math.Max(0, float64(grow))))
		cover = float32(math.Min(1, math.Max(0, float64(cover)))) * grow
		shade := 0.84 + 0.16*det
		sr, sg, sb := 232*shade, 237*shade, 244*shade
		out[i*4] = clampByte(r*(1-cover) + sr*cover)
		out[i*4+1] = clampByte(g*(1-cover) + sg*cover)
		out[i*4+2] = clampByte(b*(1-cover) + sb*cover)
		out[i*4+3] = src[i*4+3]
	}
	return out
}

// Shrink halves an image until it fits max (box filter)
func shrink(src []byte, w, h, max int) ([]byte, int, int) {
	for w > max || h > max {
		nw, nh := w/2, h/2
		if nw < 1 || nh < 1 {
			break
		}
		out := make([]byte, nw*nh*4)
		for y := 0; y < nh; y++ {
			for x := 0; x < nw; x++ {
				for c := 0; c < 4; c++ {
					s := int(src[((2*y)*w+2*x)*4+c]) + int(src[((2*y)*w+2*x+1)*4+c]) + int(src[((2*y+1)*w+2*x)*4+c]) + int(src[((2*y+1)*w+2*x+1)*4+c])
					out[(y*nw+x)*4+c] = byte(s / 4)
				}
			}
		}
		src, w, h = out, nw, nh
	}
	return src, w, h
}
