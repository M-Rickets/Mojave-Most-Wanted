package main

import (
	"bytes"
	"encoding/binary"
	"log"
	"math"
	"os"
	"path/filepath"

	"mmw/internal/bsa"
	"mmw/internal/dds"
)

const (
	rainTexOld = `textures\nvdlc02\sky\NVDLC02Rain.dds`
	rainTexNew = `textures\mmw\fx\blizzard_streak1.dds`
)

func makeRainfall(data string) bool {
	fx := filepath.Join(data, "meshes", "mmw", "fx")
	tex := filepath.Join(data, "textures", "mmw", "fx", "blizzard_streak1.dds")
	outs := []string{filepath.Join(fx, "rain0.nif"), filepath.Join(fx, "rain1.nif"), filepath.Join(fx, "rain2.nif")}
	clear := func() {
		for _, o := range outs {
			os.Remove(o)
		}
		os.Remove(tex)
	}
	var rain, rain2, rtex []byte
	for _, name := range []string{"HonestHearts - Main.bsa", "OldWorldBlues - Main.bsa"} {
		a, err := bsa.Open(filepath.Join(data, name))
		if err != nil {
			continue
		}
		for _, f := range a.Files {
			switch f.Path {
			case `meshes\nvdlc02\sky\nvdlc02rain.nif`:
				rain, _ = a.Read(f)
			case `meshes\nvdlc02\sky\nvdlc02rain2.nif`:
				rain2, _ = a.Read(f)
			case `textures\nvdlc02\sky\nvdlc02rain.dds`:
				rtex, _ = a.Read(f)
			}
		}
		a.Close()
		if rain != nil && rain2 != nil && rtex != nil {
			break
		}
	}
	if rain == nil || rain2 == nil || rtex == nil {
		log.Printf("rain: Honest Hearts' rain models aren't in your archives - no rain falls (the sky still rains)")
		clear()
		return false
	}
	os.MkdirAll(fx, 0o755)
	os.MkdirAll(filepath.Dir(tex), 0o755)
	write := func(p string, b []byte) bool {
		if old, err := os.ReadFile(p); err == nil && bytes.Equal(old, b) {
			return true
		}
		return os.WriteFile(p, b, 0o644) == nil
	}
	ok := write(outs[0], rain) && write(outs[1], rain2)
	bl := bytes.ReplaceAll(rain, []byte(rainTexOld), []byte(rainTexNew))
	var f600, f1500 [4]byte
	binary.LittleEndian.PutUint32(f600[:], math.Float32bits(600))
	binary.LittleEndian.PutUint32(f1500[:], math.Float32bits(1500))
	if bytes.Count(bl, f600[:]) == 1 {
		bl = bytes.Replace(bl, f600[:], f1500[:], 1)
	} else {
		log.Printf("rain: the rain's birth rate not found once (%d) - the blizzard's streaks as thick as rain", bytes.Count(bl, f600[:]))
	}
	if bytes.Count(bl, []byte(rainTexNew)) == 0 {
		log.Printf("rain: the rain model's texture name not found - no blizzard streaks")
		os.Remove(outs[2])
	} else {
		ok = write(outs[2], bl) && ok
	}
	w, h, rgba, err := dds.Decode(rtex)
	if err != nil {
		log.Printf("rain: the rain texture: %v", err)
		os.Remove(outs[2])
		return ok
	}
	for i := 0; i < w*h; i++ {
		rgba[i*4], rgba[i*4+1], rgba[i*4+2] = 236, 240, 248
		rgba[i*4+3] = clampByte(float32(rgba[i*4+3]) * 2.2)
	}
	ok = write(tex, dds.DXT5Mips(rgba, w, h)) && ok
	log.Printf("rain: falling rain made from Honest Hearts' (meshes\\mmw\\fx\\rain0-1.nif) and the blizzard's white streaks (rain2.nif, %dx%d)", w, h)
	return ok
}

// removeRainfall: the rain models and the blizzard's streak texture taken out of Data (no weather)
func removeRainfall(data string) {
	n := 0
	for _, p := range []string{
		filepath.Join(data, "meshes", "mmw", "fx", "rain0.nif"), filepath.Join(data, "meshes", "mmw", "fx", "rain1.nif"),
		filepath.Join(data, "meshes", "mmw", "fx", "rain2.nif"), filepath.Join(data, "textures", "mmw", "fx", "blizzard_streak1.dds"),
	} {
		if os.Remove(p) == nil {
			n++
		}
	}
	if n > 0 {
		log.Printf("rain: %d rain files removed (no weather)", n)
	}
}
