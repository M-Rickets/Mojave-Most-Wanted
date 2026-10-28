// Package bc decodes block-compressed textures (BC1 and BC3) to RGBA, for reading masks the converter needs (the
// light-group channels of an NFS car's emissive texture).
package bc

import "encoding/binary"

func c565(v uint16) [3]int {
	r := int(v>>11) & 31
	g := int(v>>5) & 63
	b := int(v) & 31
	return [3]int{r<<3 | r>>2, g<<2 | g>>4, b<<3 | b>>2}
}

func colorBlock(blk []byte, out []byte, stride, x0, y0, w, h int, bc1 bool) {
	c0, c1 := binary.LittleEndian.Uint16(blk[0:]), binary.LittleEndian.Uint16(blk[2:])
	a, b := c565(c0), c565(c1)
	var pal [4][4]int
	pal[0] = [4]int{a[0], a[1], a[2], 255}
	pal[1] = [4]int{b[0], b[1], b[2], 255}
	if c0 > c1 || !bc1 {
		for k := 0; k < 3; k++ {
			pal[2][k] = (2*a[k] + b[k]) / 3
			pal[3][k] = (a[k] + 2*b[k]) / 3
		}
		pal[2][3], pal[3][3] = 255, 255
	} else {
		for k := 0; k < 3; k++ {
			pal[2][k] = (a[k] + b[k]) / 2
		}
		pal[2][3] = 255
		pal[3] = [4]int{0, 0, 0, 0}
	}
	bits := binary.LittleEndian.Uint32(blk[4:])
	for py := 0; py < 4; py++ {
		for px := 0; px < 4; px++ {
			x, y := x0+px, y0+py
			if x >= w || y >= h {
				continue
			}
			c := pal[(bits>>(2*(py*4+px)))&3]
			o := y*stride + x*4
			out[o], out[o+1], out[o+2] = byte(c[0]), byte(c[1]), byte(c[2])
			if bc1 {
				out[o+3] = byte(c[3])
			}
		}
	}
}

func alphaBlock(blk []byte, out []byte, stride, x0, y0, w, h int) {
	a0, a1 := int(blk[0]), int(blk[1])
	var pal [8]int
	pal[0], pal[1] = a0, a1
	if a0 > a1 {
		for i := 1; i < 7; i++ {
			pal[i+1] = ((7-i)*a0 + i*a1) / 7
		}
	} else {
		for i := 1; i < 5; i++ {
			pal[i+1] = ((5-i)*a0 + i*a1) / 5
		}
		pal[6], pal[7] = 0, 255
	}
	var bits uint64
	for i := 0; i < 6; i++ {
		bits |= uint64(blk[2+i]) << (8 * i)
	}
	for py := 0; py < 4; py++ {
		for px := 0; px < 4; px++ {
			x, y := x0+px, y0+py
			if x >= w || y >= h {
				continue
			}
			out[y*stride+x*4+3] = byte(pal[(bits>>(3*(py*4+px)))&7])
		}
	}
}

// Decode returns the top mip as RGBA (w*h*4) for DXGI BC1 (71) or BC3 (77); ok is false for other formats.
func Decode(format uint32, w, h int, px []byte) ([]byte, bool) {
	bs := 0
	switch format {
	case 71, 72:
		bs = 8
	case 77, 78:
		bs = 16
	default:
		return nil, false
	}
	bw, bh := (w+3)/4, (h+3)/4
	if len(px) < bw*bh*bs {
		return nil, false
	}
	out := make([]byte, w*h*4)
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			blk := px[(by*bw+bx)*bs:]
			if bs == 8 {
				colorBlock(blk, out, w*4, bx*4, by*4, w, h, true)
			} else {
				alphaBlock(blk, out, w*4, bx*4, by*4, w, h)
				colorBlock(blk[8:], out, w*4, bx*4, by*4, w, h, false)
			}
		}
	}
	return out, true
}
