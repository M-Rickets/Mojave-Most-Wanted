package audio

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
)

// Ginsu is an EA granular engine sample ("Gnsu" wave resource, type 0x80 in Criterion bundles): one recorded RPM sweep
// plus tables that map RPM to sample positions and mark grain boundaries.
type Ginsu struct {
	MinRPM, MaxRPM float32
	RPMPos         []uint32
	Grains         []uint32
	Rate           uint32
	Samples        []int16
}

// ParseGinsu reads a wave resource holding "Gnsu" (HP Remastered: at +4, MW 2012: at +8).
func ParseGinsu(res []byte) (*Ginsu, error) {
	g := bytes.Index(res, []byte("Gnsu"))
	if g < 0 || g > 16 || len(res) < g+0x20 {
		return nil, fmt.Errorf("not a Ginsu wave")
	}
	d := res[g:]
	le := binary.LittleEndian
	gs := &Ginsu{
		MinRPM: math.Float32frombits(le.Uint32(d[8:])),
		MaxRPM: math.Float32frombits(le.Uint32(d[12:])),
		Rate:   le.Uint32(d[0x1C:]),
	}
	n1, n2 := int(le.Uint32(d[0x10:])), int(le.Uint32(d[0x14:]))
	num := int(le.Uint32(d[0x18:]))
	extra := 0
	if d[4] == '3' {
		extra = 4
	}
	t2 := 0x20 + (n1+1)*4 + extra
	start := t2 + (n2+1)*4
	if n1 > 4096 || n2 > 65536 || start > len(d) || gs.Rate == 0 || gs.Rate > 192000 {
		return nil, fmt.Errorf("bad Ginsu header")
	}
	for i := 0; i <= n1; i++ {
		gs.RPMPos = append(gs.RPMPos, le.Uint32(d[0x20+i*4:]))
	}
	for i := 0; i <= n2; i++ {
		gs.Grains = append(gs.Grains, le.Uint32(d[t2+i*4:]))
	}
	gs.Samples = DecodeXASv0(d[start:], num)
	return gs, nil
}

// RMS of the samples.
func (g *Ginsu) RMS() float64 {
	var s float64
	for _, v := range g.Samples {
		s += float64(v) * float64(v)
	}
	if len(g.Samples) == 0 {
		return 0
	}
	return math.Sqrt(s / float64(len(g.Samples)))
}

// WriteEngine writes the plugin's engine file: "MMWE" u32 version, rate, minRPM f32, maxRPM f32, nRPM, nGrains,
// numSamples, then the tables and 16-bit mono samples.
func (g *Ginsu) WriteEngine() []byte {
	var b bytes.Buffer
	le := binary.LittleEndian
	b.WriteString("MMWE")
	for _, v := range []uint32{1, g.Rate, math.Float32bits(g.MinRPM), math.Float32bits(g.MaxRPM), uint32(len(g.RPMPos)), uint32(len(g.Grains)), uint32(len(g.Samples))} {
		binary.Write(&b, le, v)
	}
	binary.Write(&b, le, g.RPMPos)
	binary.Write(&b, le, g.Grains)
	binary.Write(&b, le, g.Samples)
	return b.Bytes()
}

// WAV returns a 16-bit mono WAV of samples (developer checks and simple sounds such as sirens).
func WAV(samples []int16, rate uint32, channels int) []byte {
	var b bytes.Buffer
	le := binary.LittleEndian
	n := uint32(len(samples) * 2)
	b.WriteString("RIFF")
	binary.Write(&b, le, 36+n)
	b.WriteString("WAVEfmt ")
	binary.Write(&b, le, uint32(16))
	binary.Write(&b, le, uint16(1))
	binary.Write(&b, le, uint16(channels))
	binary.Write(&b, le, rate)
	binary.Write(&b, le, rate*uint32(channels)*2)
	binary.Write(&b, le, uint16(channels*2))
	binary.Write(&b, le, uint16(16))
	b.WriteString("data")
	binary.Write(&b, le, n)
	binary.Write(&b, le, samples)
	return b.Bytes()
}
