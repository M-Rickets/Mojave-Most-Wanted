package main

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"log"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"mmw/internal/dds"
)

var fxTemplate []byte

func makeSnowfall(data string) bool {
	b := fxTemplate
	outOf := func(i int) string { return filepath.Join(data, "meshes", "mmw", "fx", fmt.Sprintf("snow%d.nif", i)) }
	clear := func() {
		for i := range snowRecipe.Variants {
			os.Remove(outOf(i))
		}
	}
	if b == nil {
		log.Printf("snow: %s not in the archives", snowRecipe.Template)
		clear()
		return false
	}
	sum := sha1.Sum(b)
	if len(b) != snowRecipe.Size || hex.EncodeToString(sum[:]) != snowRecipe.Sha1 {
		log.Printf("snow: %s isn't the one the recipe was made from (%d bytes)", snowRecipe.Template, len(b))
		clear()
		return false
	}
	n, err := parseNif(b)
	if err != nil {
		log.Printf("snow: %v", err)
		return false
	}
	for i, patches := range snowRecipe.Variants {
		m := append([]byte(nil), b...)
		for _, p := range patches {
			v, _ := hex.DecodeString(p.Bytes)
			at := n.blockStart[p.Block] + p.Off
			copy(m[at:at+len(v)], v)
		}
		m, err = n.replaceTexture(m, snowRecipe.OldTexture, snowRecipe.Texture, snowRecipe.ShaderBlock)
		if err != nil {
			log.Printf("snow: %v", err)
			return false
		}
		os.MkdirAll(filepath.Dir(outOf(i)), 0o755)
		if err := os.WriteFile(outOf(i), m, 0o644); err != nil {
			log.Printf("snow: %v", err)
			return false
		}
	}
	tex := filepath.Join(data, filepath.FromSlash(strings.ReplaceAll(snowRecipe.Texture, `\`, "/")))
	os.MkdirAll(filepath.Dir(tex), 0o755)
	os.WriteFile(tex, flakeTexture(), 0o644)
	log.Printf("snow: falling snow made (light, heavy, blizzard: meshes\\mmw\\fx\\snow0-2.nif)")
	return true
}

// flakeTexture: a soft white flake - a bright core with a few short arms, fading out (64 x 64, DXT5 with mips)
func flakeTexture() []byte {
	const n = 64
	rgba := make([]byte, n*n*4)
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			dx, dy := (float64(x)+0.5)/n*2-1, (float64(y)+0.5)/n*2-1
			r := math.Sqrt(dx*dx + dy*dy)
			ang := math.Atan2(dy, dx)
			core := math.Max(0, 1-r/0.55)
			arms := math.Max(0, 1-r/0.95) * math.Pow(math.Abs(math.Cos(3*ang)), 6) * 0.55
			a := math.Min(1, math.Pow(core, 1.3)+arms)
			o := (y*n + x) * 4
			rgba[o], rgba[o+1], rgba[o+2] = 246, 248, 255
			rgba[o+3] = byte(a * 255)
		}
	}
	return dds.DXT5Mips(rgba, n, n)
}

// snowWeather picks the sky for snow among FalloutNV.esm's weathers (all listed in dev\weathers.txt): an overcast one
// first, then a cloudy one; never rain, storms, dust or radiation (their own particles and colours).
func snowWeather(esm, managed string) (uint32, string) {
	all, err := listRecords(esm, "WTHR")
	if err != nil {
		log.Printf("snow sky: %v", err)
		return 0, ""
	}
	var names []string
	for n := range all {
		names = append(names, n)
	}
	sort.Strings(names)
	var lines []string
	for _, n := range names {
		lines = append(lines, fmt.Sprintf("%08X %s", all[n], n))
	}
	score := func(n string) int {
		l := strings.ToLower(n)
		for _, bad := range []string{"rain", "storm", "dust", "rad", "sand", "fire", "smoke", "ash", "test", "dlc", "zion", "sierra", "divide"} {
			if strings.Contains(l, bad) {
				return 0
			}
		}
		switch {
		case strings.Contains(l, "overcast"):
			return 3
		case strings.Contains(l, "cloudy"):
			return 2
		case strings.Contains(l, "cloud"):
			return 1
		}
		return 0
	}
	for _, want := range []string{"gWastelandCloudy", "SuburbanCloudy", "NVWastelandHazy"} {
		if id, ok := all[want]; ok {
			return id, want
		}
	}
	best, bestScore := "", 0
	for _, n := range names {
		if s := score(n); s > bestScore {
			best, bestScore = n, s
		}
	}
	if best == "" {
		return 0, ""
	}
	return all[best], best
}

// listRecords: every record of a type in an esm, editor id -> form id
func listRecords(esm, recType string) (map[string]uint32, error) {
	f, err := os.Open(esm)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	r := bufio.NewReaderSize(f, 1<<20)
	hdr := make([]byte, 24)
	le := binary.LittleEndian
	out := map[string]uint32{}
	inType := false
	for {
		if _, err := io.ReadFull(r, hdr); err != nil {
			return out, nil
		}
		sig, size := string(hdr[:4]), le.Uint32(hdr[4:])
		if sig == "GRUP" {
			label, gtype := string(hdr[8:12]), le.Uint32(hdr[12:])
			if gtype == 0 {
				inType = label == recType
				if !inType {
					if _, err := r.Discard(int(size) - 24); err != nil {
						return out, nil
					}
				}
			}
			continue
		}
		body := make([]byte, size)
		if _, err := io.ReadFull(r, body); err != nil {
			return out, nil
		}
		if !inType || sig != recType {
			continue
		}
		flags, id := le.Uint32(hdr[8:]), le.Uint32(hdr[12:])
		if flags&0x40000 != 0 && len(body) > 4 {
			zr, err := zlib.NewReader(bytes.NewReader(body[4:]))
			if err != nil {
				continue
			}
			if body, err = io.ReadAll(zr); err != nil {
				continue
			}
		}
		for o := 0; o+6 <= len(body); {
			st, ss := string(body[o:o+4]), int(le.Uint16(body[o+4:]))
			if o+6+ss > len(body) {
				break
			}
			if st == "EDID" {
				out[string(bytes.TrimRight(body[o+6:o+6+ss], "\x00"))] = id
				break
			}
			o += 6 + ss
		}
	}
}
