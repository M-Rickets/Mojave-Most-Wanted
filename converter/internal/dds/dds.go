// Package dds writes DirectX 9 style DDS files that Fallout: New Vegas loads (DXT1/DXT3/DXT5, 32-bit BGRA).
package dds

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
)

const (
	DXGI_BC1    = 71
	DXGI_BC1s   = 72
	DXGI_BC2    = 74
	DXGI_BC2s   = 75
	DXGI_BC3    = 77
	DXGI_BC3s   = 78
	DXGI_RGBA8  = 28
	DXGI_RGBA8s = 29
	DXGI_BGRA8  = 87
	DXGI_BGRA8s = 91
)

func blockBytes(fourcc string) int {
	if fourcc == "DXT1" {
		return 8
	}
	return 16
}

func mipSize(w, h int, fourcc string) int {
	if fourcc == "" {
		return w * h * 4
	}
	bw, bh := (w+3)/4, (h+3)/4
	if bw < 1 {
		bw = 1
	}
	if bh < 1 {
		bh = 1
	}
	return bw * bh * blockBytes(fourcc)
}

func header(w, h, mips int, fourcc string) []byte {
	b := &bytes.Buffer{}
	le := binary.LittleEndian
	b.WriteString("DDS ")
	flags := uint32(0x1 | 0x2 | 0x4 | 0x1000)
	if mips > 1 {
		flags |= 0x20000
	}
	pitch := uint32(mipSize(w, h, fourcc))
	if fourcc != "" {
		flags |= 0x80000
	} else {
		flags |= 0x8
		pitch = uint32(w * 4)
	}
	for _, v := range []uint32{124, flags, uint32(h), uint32(w), pitch, 0, uint32(mips)} {
		binary.Write(b, le, v)
	}
	for i := 0; i < 11; i++ {
		binary.Write(b, le, uint32(0))
	}
	binary.Write(b, le, uint32(32))
	if fourcc != "" {
		binary.Write(b, le, uint32(0x4))
		b.WriteString(fourcc)
		for i := 0; i < 5; i++ {
			binary.Write(b, le, uint32(0))
		}
	} else {
		for _, v := range []uint32{0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000} {
			binary.Write(b, le, v)
		}
	}
	caps := uint32(0x1000)
	if mips > 1 {
		caps |= 0x8 | 0x400000
	}
	for _, v := range []uint32{caps, 0, 0, 0, 0} {
		binary.Write(b, le, v)
	}
	return b.Bytes()
}

// FromDXGI converts raw D3D11 texture data (mips tightly packed) to a DDS file. maxSize > 0 drops leading mips until
// the top level fits (keeps files small).
func FromDXGI(format uint32, w, h, mips int, px []byte, maxSize int) ([]byte, error) {
	fourcc := ""
	switch format {
	case DXGI_BC1, DXGI_BC1s:
		fourcc = "DXT1"
	case DXGI_BC2, DXGI_BC2s:
		fourcc = "DXT3"
	case DXGI_BC3, DXGI_BC3s:
		fourcc = "DXT5"
	case DXGI_BGRA8, DXGI_BGRA8s, DXGI_RGBA8, DXGI_RGBA8s:
		fourcc = ""
	default:
		return nil, fmt.Errorf("texture format %d not supported", format)
	}
	off := 0
	for maxSize > 0 && mips > 1 && (w > maxSize || h > maxSize) {
		off += mipSize(w, h, fourcc)
		w, h, mips = max(1, w/2), max(1, h/2), mips-1
	}
	total := 0
	ww, hh := w, h
	for i := 0; i < mips; i++ {
		total += mipSize(ww, hh, fourcc)
		ww, hh = max(1, ww/2), max(1, hh/2)
	}
	if off+total > len(px) {
		return nil, fmt.Errorf("texture data short: need %d, have %d", off+total, len(px))
	}
	data := px[off : off+total]
	if format == DXGI_RGBA8 || format == DXGI_RGBA8s {
		data = append([]byte(nil), data...)
		for i := 0; i+3 < len(data); i += 4 {
			data[i], data[i+2] = data[i+2], data[i]
		}
	}
	return append(header(w, h, mips, fourcc), data...), nil
}

// Solid returns a small uncompressed DDS of one colour (with mips).
func Solid(r, g, b, a uint8) []byte {
	w, mips := 8, 4
	var px []byte
	for m, s := 0, w; m < mips; m, s = m+1, s/2 {
		for i := 0; i < s*s; i++ {
			px = append(px, b, g, r, a)
		}
	}
	return append(header(w, w, mips, ""), px...)
}

// Image returns an uncompressed 32-bit DDS of one level from BGRA pixels (w*h*4 bytes): the plugin redraws such a
// texture in place (the car's dash screen), so it has no mips to go stale.
func Image(w, h int, bgra []byte) []byte { return append(header(w, h, 1, ""), bgra...) }

// ImageMips: an uncompressed BGRA image with its full mip chain (each level the 2x2 average of the one above) - for a
// picture the plugin redraws that must stay readable small
func ImageMips(w, h int, bgra []byte) []byte {
	mips := 1
	for a, b := w, h; a > 1 || b > 1; a, b = max(1, a/2), max(1, b/2) {
		mips++
	}
	out := append(header(w, h, mips, ""), bgra...)
	cur, cw, ch := bgra, w, h
	for m := 1; m < mips; m++ {
		nw, nh := max(1, cw/2), max(1, ch/2)
		nx := make([]byte, nw*nh*4)
		for y := 0; y < nh; y++ {
			for x := 0; x < nw; x++ {
				for c := 0; c < 4; c++ {
					sum := 0
					for dy := 0; dy < 2; dy++ {
						for dx := 0; dx < 2; dx++ {
							sx, sy := min(2*x+dx, cw-1), min(2*y+dy, ch-1)
							sum += int(cur[4*(sy*cw+sx)+c])
						}
					}
					nx[4*(y*nw+x)+c] = byte(sum / 4)
				}
			}
		}
		out = append(out, nx...)
		cur, cw, ch = nx, nw, nh
	}
	return out
}

// FlatNormal is a neutral normal map (normal up, low specular in alpha).
func FlatNormal() []byte { return Solid(128, 128, 255, 40) }

// Cube returns an uncompressed cube map (faces +X -X +Y -Y +Z -Z, each with its mips) whose texel colours come from
// f(direction).
func Cube(size int, f func(x, y, z float64) [3]uint8) []byte {
	mips := 1
	for s := size; s > 1; s /= 2 {
		mips++
	}
	var px []byte
	for face := 0; face < 6; face++ {
		for m, s := 0, size; m < mips; m, s = m+1, max(1, s/2) {
			for j := 0; j < s; j++ {
				for i := 0; i < s; i++ {
					u := (float64(i)+0.5)/float64(s)*2 - 1
					v := (float64(j)+0.5)/float64(s)*2 - 1
					var x, y, z float64
					switch face {
					case 0:
						x, y, z = 1, -v, -u
					case 1:
						x, y, z = -1, -v, u
					case 2:
						x, y, z = u, 1, v
					case 3:
						x, y, z = u, -1, -v
					case 4:
						x, y, z = u, -v, 1
					default:
						x, y, z = -u, -v, -1
					}
					l := math.Sqrt(x*x + y*y + z*z)
					c := f(x/l, y/l, z/l)
					px = append(px, c[2], c[1], c[0], 255)
				}
			}
		}
	}
	h := header(size, size, mips, "")
	binary.LittleEndian.PutUint32(h[112:], 0x200|0x400|0x800|0x1000|0x2000|0x4000|0x8000)
	binary.LittleEndian.PutUint32(h[108:], 0x1000|0x8|0x400000)
	return append(h, px...)
}
