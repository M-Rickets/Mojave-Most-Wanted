package crit

import (
	"fmt"
	"math"
)

// Vertices holds decoded vertex data for one mesh, in NFS model space (metres, X left, Y up, Z forward).
type Vertices struct {
	Pos [][3]float32
	UV  [][2]float32
}

func half(h uint16) float32 {
	s := uint32(h>>15) & 1
	e := uint32(h>>10) & 0x1f
	f := uint32(h) & 0x3ff
	var bits uint32
	switch {
	case e == 0 && f == 0:
		bits = s << 31
	case e == 0:
		for f&0x400 == 0 {
			f <<= 1
			e--
		}
		e++
		f &^= 0x400
		bits = s<<31 | (e+112)<<23 | f<<13
	case e == 31:
		bits = s<<31 | 0xff<<23 | f<<13
	default:
		bits = s<<31 | (e+112)<<23 | f<<13
	}
	return math.Float32frombits(bits)
}

// FallbackLayout guesses a layout from the stride when the shader's descriptor is unavailable (HP Remastered layouts
// measured from the player's files: position int16x4 at 0, uv half2 at the end).
func FallbackLayout(stride uint32) []VElem {
	l := []VElem{{Semantic: 1, Format: 13, Offset: 0, Stride: stride}}
	if stride >= 24 {
		l = append(l, VElem{Semantic: 5, Format: 34, Offset: stride - 4, Stride: stride})
	}
	return l
}

// DecodeVertices reads count vertices from a renderable's graphics buffer.
func DecodeVertices(R *Renderable, m Mesh, layout []VElem, count int, posScale float32) (*Vertices, error) {
	var pos, uv *VElem
	for i := range layout {
		e := &layout[i]
		switch {
		case e.Semantic == 1 && pos == nil:
			pos = e
		case (e.Semantic == 5 || e.Semantic == 0x106) && uv == nil:
			uv = e
		}
	}
	if pos == nil {
		return nil, fmt.Errorf("layout has no position")
	}
	stride := int(pos.Stride)
	if stride == 0 {
		return nil, fmt.Errorf("layout stride 0")
	}
	if max := int(m.VBSize) / stride; count > max {
		return nil, fmt.Errorf("mesh needs %d vertices, buffer holds %d", count, max)
	}
	v := &Vertices{Pos: make([][3]float32, count)}
	if uv != nil {
		v.UV = make([][2]float32, count)
	}
	g := R.Gfx
	for i := 0; i < count; i++ {
		base := int(m.VBOff) + i*stride
		o := base + int(pos.Offset)
		switch pos.Format {
		case 13:
			for k := 0; k < 3; k++ {
				v.Pos[i][k] = float32(int16(le.Uint16(g[o+2*k:]))) * posScale / 32767
			}
		case 6, 2:
			for k := 0; k < 3; k++ {
				v.Pos[i][k] = math.Float32frombits(le.Uint32(g[o+4*k:]))
			}
		case 10:
			for k := 0; k < 3; k++ {
				v.Pos[i][k] = half(le.Uint16(g[o+2*k:]))
			}
		default:
			return nil, fmt.Errorf("position format %d not supported", pos.Format)
		}
		if uv != nil {
			o := base + int(uv.Offset)
			switch uv.Format {
			case 34:
				v.UV[i] = [2]float32{half(le.Uint16(g[o:])), half(le.Uint16(g[o+2:]))}
			case 16:
				v.UV[i] = [2]float32{math.Float32frombits(le.Uint32(g[o:])), math.Float32frombits(le.Uint32(g[o+4:]))}
			case 37:
				v.UV[i] = [2]float32{float32(int16(le.Uint16(g[o:]))) / 32767, float32(int16(le.Uint16(g[o+2:]))) / 32767}
			default:
				v.UV = nil
				uv = nil
			}
		}
	}
	return v, nil
}
