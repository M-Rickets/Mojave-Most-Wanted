package main

import (
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"mmw/internal/audio"
	"mmw/internal/bnd2"
	"mmw/internal/bsa"
)

// Tyre skid loops from Most Wanted's global sound bank (sheets/sounds.json, kind "skid"), picked by spectrogram:
// 0xf357a steady squeal bands at ~2 and ~4.3 kHz, 0xf357e the same an octave lower.
var skidWaves = []struct {
	id   uint64
	rate float64
	out  string
}{{0x1000000000f357e, 0.62, "skid_body.wav"}, {0x1000000000f357e, 1, "skid_mid.wav"}, {0x1000000000f357a, 1, "skid_hi.wav"}}

// Resample plays p at rate (linear interpolation): below 1 it is longer and lower.
func resample(p []int16, rate float64) []int16 {
	if rate == 1 || len(p) < 2 {
		return p
	}
	n := int(float64(len(p)-1) / rate)
	out := make([]int16, n)
	for i := range out {
		x := float64(i) * rate
		k := int(x)
		t := x - float64(k)
		out[i] = int16(float64(p[k])*(1-t) + float64(p[k+1])*t)
	}
	return out
}

// Highpass removes rumble below hz.
func highpass(p []int16, rate uint32, hz float64) []int16 {
	w := 2 * math.Pi * hz / float64(rate)
	al := math.Sin(w) / (2 * 0.7071)
	c := math.Cos(w)
	a0 := 1 + al
	b0, b1, b2 := (1+c)/2/a0, -(1+c)/a0, (1+c)/2/a0
	a1, a2 := -2*c/a0, (1-al)/a0
	x := make([]float64, len(p))
	for i, v := range p {
		x[i] = float64(v)
	}
	for pass := 0; pass < 2; pass++ {
		var x1, x2, y1, y2 float64
		for i, v := range x {
			y := b0*v + b1*x1 + b2*x2 - a1*y1 - a2*y2
			x2, x1, y2, y1 = x1, v, y1, y
			x[i] = y
		}
	}
	out := make([]int16, len(p))
	for i, v := range x {
		out[i] = int16(math.Max(-32768, math.Min(32767, v)))
	}
	return out
}

// convertSkids writes sound\skid_*.wav as seamless loops (the last 60 ms crossfaded into the start).
func convertSkids(pluginDir, mw string) bool {
	if mw == "" {
		return false
	}
	src := filepath.Join(mw, "SOUND", "GLOBAL.BNDL")
	if !exists(src) {
		log.Print("skid sounds: Most Wanted's SOUND\\GLOBAL.BNDL not found")
		return false
	}
	s := sig(src) + "|skid2"
	all := true
	for _, w := range skidWaves {
		all = all && exists(filepath.Join(pluginDir, "sound", w.out))
	}
	if e, ok := cache["skids"]; ok && e.Sig == s && all {
		return true
	}
	b, err := bnd2.Open(src)
	if err != nil {
		log.Printf("skid sounds: %v", err)
		return false
	}
	n := 0
	for _, w := range skidWaves {
		pcm, rate, err := audio.WaveResource(b, w.id)
		if err != nil || len(pcm) < int(rate)/4 {
			log.Printf("skid sounds: wave %x: %v", w.id, err)
			continue
		}
		if w.rate != 1 {
			pcm = highpass(resample(pcm, w.rate), rate, 160)
		}
		pcm = seamless(pcm, int(rate)*60/1000)
		out := filepath.Join(pluginDir, "sound", w.out)
		os.MkdirAll(filepath.Dir(out), 0o755)
		if os.WriteFile(out, audio.WAV(pcm, rate, 1), 0o644) == nil {
			n++
		}
	}
	os.Remove(filepath.Join(pluginDir, "sound", "skid_scrub.wav"))
	log.Printf("skid sounds: %d of %d from %s", n, len(skidWaves), src)
	if n == len(skidWaves) {
		cache["skids"] = cacheEntry{Sig: s}
	}
	return n > 0
}

// Seamless crossfades the last fade samples into the first ones and drops them, so the loop has no seam.
func seamless(p []int16, fade int) []int16 {
	if fade <= 0 || len(p) < 3*fade {
		return p
	}
	out := append([]int16(nil), p[:len(p)-fade]...)
	for i := 0; i < fade; i++ {
		t := float64(i) / float64(fade)
		v := float64(p[len(p)-fade+i])*(1-t) + float64(p[i])*t
		out[i] = int16(v)
	}
	return out
}

// tyreSmoke copies the tyre smoke candidates (sheets/fx.json, New Vegas's own particle smoke models) from the game's
// archives into meshes\mmw\fx\tiresmoke<i>.nif
func tyreSmoke(data, managed, hp string, set func(string, any)) int {
	want := map[string]int{}
	for i, c := range sheetSmokeCandidates {
		want[strings.ToLower(c)] = i
	}
	tmpl := strings.ToLower(smokeRecipe.Template)
	var template []byte
	found := make([][]byte, len(sheetSmokeCandidates))
	bsas, _ := filepath.Glob(filepath.Join(data, "*.bsa"))
	sort.Strings(bsas)
	for _, p := range bsas {
		a, err := bsa.Open(p)
		if err != nil {
			continue
		}
		for _, f := range a.Files {
			if f.Path == tmpl && template == nil {
				template, _ = a.Read(f)
				fxTemplate = template
			}
			i, ok := want[f.Path]
			if !ok || found[i] != nil {
				continue
			}
			if b, err := a.Read(f); err == nil {
				found[i] = b
			}
		}
		a.Close()
	}
	if i, ok := want["mmw"]; ok {
		found[i] = makeSmoke(data, func(string) []byte { return template })
		if found[i] != nil {
			tex := hpSmokeTexture(hp, smokeRecipe.HPTexture)
			src := "Hot Pursuit"
			if tex == nil {
				tex, src = smokeTexture(), "made here"
			}
			out := filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(smokeRecipe.Texture, `\`, "/")))
			os.MkdirAll(filepath.Dir(out), 0o755)
			os.WriteFile(out, tex, 0o644)
			log.Printf("tyre smoke texture: %s", src)
		}
	}
	n := 0
	for i, b := range found {
		out := filepath.Join(data, "meshes", "mmw", "fx", fmt.Sprintf("tiresmoke%d.nif", i))
		if b == nil {
			os.Remove(out)
			set(fmt.Sprintf("smoke%d", i), 0)
			log.Printf("tyre smoke %d: %s not available", i, sheetSmokeCandidates[i])
			continue
		}
		os.MkdirAll(filepath.Dir(out), 0o755)
		os.WriteFile(out, b, 0o644)
		set(fmt.Sprintf("smoke%d", i), 1)
		n++
		log.Printf("tyre smoke %d: %s", i, sheetSmokeCandidates[i])
	}
	return n
}

// Crash sounds from Most Wanted's car impact bank (SOUND/METALCAR.BNDL), picked from the 118 waves by their shape
var crashWaves = []struct {
	id  uint64
	out string
}{{0x1000000000b3927, "crash_light.wav"}, {0x1000000000b3915, "crash_mid.wav"}, {0x10000000014e0b1, "crash_heavy.wav"}}

func convertCrashes(pluginDir, mw string) bool {
	if mw == "" {
		return false
	}
	src := filepath.Join(mw, "SOUND", "METALCAR.BNDL")
	if !exists(src) {
		log.Print("crash sounds: Most Wanted's SOUND\\METALCAR.BNDL not found")
		return false
	}
	s := sig(src) + "|crash1"
	all := true
	for _, w := range crashWaves {
		all = all && exists(filepath.Join(pluginDir, "sound", w.out))
	}
	if e, ok := cache["crashes"]; ok && e.Sig == s && all {
		return true
	}
	b, err := bnd2.Open(src)
	if err != nil {
		log.Printf("crash sounds: %v", err)
		return false
	}
	n := 0
	for _, w := range crashWaves {
		pcm, rate, err := audio.WaveResource(b, w.id)
		if err != nil || len(pcm) < int(rate)/10 {
			log.Printf("crash sounds: wave %x: %v", w.id, err)
			continue
		}
		out := filepath.Join(pluginDir, "sound", w.out)
		os.MkdirAll(filepath.Dir(out), 0o755)
		if os.WriteFile(out, audio.WAV(pcm, rate, 1), 0o644) == nil {
			n++
		}
	}
	log.Printf("crash sounds: %d of %d from %s", n, len(crashWaves), src)
	if n == len(crashWaves) {
		cache["crashes"] = cacheEntry{Sig: s}
	}
	return n > 0
}
