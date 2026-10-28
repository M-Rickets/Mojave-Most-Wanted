package audio

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"

	"mmw/internal/bnd2"
)

// PickEngine chooses a car's engine recordings from its bundle
func PickEngine(b *bnd2.Bundle) (on, off *Ginsu, err error) {
	if on, off = pickByGraph(b); on != nil {
		return on, off, nil
	}
	var all []*Ginsu
	maxLen := 0
	for _, r := range b.Res {
		if r.Type != 0x80 {
			continue
		}
		d, err := r.Data(0)
		if err != nil {
			continue
		}
		g, err := ParseGinsu(d)
		if err != nil || len(g.Samples) == 0 {
			continue
		}
		all = append(all, g)
		if len(g.Samples) > maxLen {
			maxLen = len(g.Samples)
		}
	}
	var full []*Ginsu
	for _, g := range all {
		if len(g.Samples)*10 >= maxLen*6 {
			full = append(full, g)
		}
	}
	if len(full) == 0 {
		return nil, nil, fmt.Errorf("no engine recording in %s", b.Path)
	}
	bestScore := -1
	for i := range full {
		for j := range full {
			if i == j {
				continue
			}
			a, c := full[i], full[j]
			if math.Abs(float64(a.MinRPM-c.MinRPM)) > 0.05*float64(a.MinRPM) || math.Abs(float64(a.MaxRPM-c.MaxRPM)) > 0.05*float64(a.MaxRPM) {
				continue
			}
			score := len(a.Grains) + len(c.Grains)
			if score > bestScore && a.RMS() >= c.RMS() {
				bestScore, on, off = score, a, c
			}
		}
	}
	if on == nil {
		for _, g := range full {
			if on == nil || len(g.Grains) > len(on.Grains) {
				on = g
			}
		}
	}
	return on, off, nil
}

// AudibleRPM reads the range of revs a car's engine is heard over in Most Wanted, from its sound graph
func AudibleRPM(b *bnd2.Bundle) (lo, hi float32, ok bool) {
	for _, r := range b.Res {
		if r.Type != 0x15 {
			continue
		}
		d, err := r.Data(0)
		if err != nil || len(d) < 0x20 || !bytes.Contains(d, []byte("GetPlayerAudibleRPM")) {
			continue
		}
		top := math.Float32frombits(binary.LittleEndian.Uint32(d[0x18:]))
		bot := math.Float32frombits(binary.LittleEndian.Uint32(d[0x1C:]))
		if top >= 2000 && top <= 15000 && bot >= 300 && bot < top-500 {
			return bot, top, true
		}
	}
	return 0, 0, false
}

// rising: a sweep recorded with the revs going up (its RPM table moves forward through the samples); a falling one is
// the engine winding down off the throttle.
func (g *Ginsu) rising() bool {
	back, fwd := 0, 0
	for i := 2; i < len(g.RPMPos); i++ {
		if g.RPMPos[i] < g.RPMPos[i-1] {
			back++
		} else if g.RPMPos[i] > g.RPMPos[i-1] {
			fwd++
		}
	}
	return fwd >= back
}

// pickByGraph: a Most Wanted car's sound graph plays its engine from two objects driven by "GetPlayerAudibleRPM" (two
// microphones), each holding two rising sweeps (on the throttle) and one falling (off it).
func pickByGraph(b *bnd2.Bundle) (on, off *Ginsu) {
	bestScore := -1.0
	for _, r := range b.Res {
		if r.Type != 0x15 {
			continue
		}
		d, err := r.Data(0)
		if err != nil || !bytes.Contains(d, []byte("GetPlayerAudibleRPM")) {
			continue
		}
		var up, down *Ginsu
		for _, im := range r.Imports() {
			gr := b.ByID[im.ID]
			if gr == nil || gr.Type != 0x80 {
				continue
			}
			gd, err := gr.Data(0)
			if err != nil {
				continue
			}
			g, err := ParseGinsu(gd)
			if err != nil || len(g.Samples) == 0 || len(g.RPMPos) < 4 {
				continue
			}
			if g.rising() {
				if up == nil || len(g.Grains) > len(up.Grains) {
					up = g
				}
			} else if down == nil || len(g.Grains) > len(down.Grains) {
				down = g
			}
		}
		if up == nil {
			continue
		}
		score := float64(len(up.Grains))*100000 + up.RMS()
		if score > bestScore {
			bestScore, on, off = score, up, down
		}
	}
	return on, off
}
