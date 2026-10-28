package dds

import (
	"encoding/binary"
	"fmt"

	"mmw/internal/bc"
)

// Decode returns the top mip of a DDS file as RGBA (DXT1, DXT5 or 32-bit uncompressed).
func Decode(b []byte) (w, h int, rgba []byte, err error) {
	if len(b) < 128 || string(b[0:4]) != "DDS " {
		return 0, 0, nil, fmt.Errorf("not a DDS")
	}
	le := binary.LittleEndian
	h, w = int(le.Uint32(b[12:])), int(le.Uint32(b[16:]))
	fourcc := string(b[84:88])
	px := b[128:]
	switch fourcc {
	case "DXT1":
		if out, ok := bc.Decode(71, w, h, px); ok {
			return w, h, out, nil
		}
	case "DXT5":
		if out, ok := bc.Decode(77, w, h, px); ok {
			return w, h, out, nil
		}
	default:
		if le.Uint32(b[88:]) == 32 && len(px) >= w*h*4 {
			out := make([]byte, w*h*4)
			for i := 0; i < w*h; i++ {
				out[i*4], out[i*4+1], out[i*4+2], out[i*4+3] = px[i*4+2], px[i*4+1], px[i*4], px[i*4+3]
			}
			return w, h, out, nil
		}
	}
	return w, h, nil, fmt.Errorf("unsupported DDS format %q", fourcc)
}

// DXT5Mips writes RGBA as a DXT5 DDS with a full mip chain (box-filtered).
func DXT5Mips(rgba []byte, w, h int) []byte {
	var levels [][]byte
	cw, ch, cur := w, h, rgba
	for {
		levels = append(levels, bc.EncodeBC3(cur, cw, ch))
		if cw == 1 && ch == 1 {
			break
		}
		nw, nh := cw/2, ch/2
		if nw < 1 {
			nw = 1
		}
		if nh < 1 {
			nh = 1
		}
		next := make([]byte, nw*nh*4)
		for y := 0; y < nh; y++ {
			for x := 0; x < nw; x++ {
				for c := 0; c < 4; c++ {
					s, n := 0, 0
					for dy := 0; dy < 2; dy++ {
						for dx := 0; dx < 2; dx++ {
							sx, sy := x*2+dx, y*2+dy
							if sx < cw && sy < ch {
								s += int(cur[(sy*cw+sx)*4+c])
								n++
							}
						}
					}
					next[(y*nw+x)*4+c] = byte(s / n)
				}
			}
		}
		cw, ch, cur = nw, nh, next
	}
	out := header(w, h, len(levels), "DXT5")
	for _, l := range levels {
		out = append(out, l...)
	}
	return out
}
