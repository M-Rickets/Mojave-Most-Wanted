package bc

import "encoding/binary"

// EncodeBC3 compresses RGBA (w*h*4) to BC3/DXT5 blocks (bounding-box colour endpoints, 8-step alpha).
func EncodeBC3(rgba []byte, w, h int) []byte {
	bw, bh := (w+3)/4, (h+3)/4
	out := make([]byte, bw*bh*16)
	var px [16][4]int
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			for i := 0; i < 16; i++ {
				x, y := bx*4+i%4, by*4+i/4
				if x >= w {
					x = w - 1
				}
				if y >= h {
					y = h - 1
				}
				o := (y*w + x) * 4
				px[i] = [4]int{int(rgba[o]), int(rgba[o+1]), int(rgba[o+2]), int(rgba[o+3])}
			}
			blk := out[(by*bw+bx)*16:]
			encodeAlpha(px[:], blk[:8])
			encodeColor(px[:], blk[8:16])
		}
	}
	return out
}

func encodeAlpha(px [][4]int, b []byte) {
	lo, hi := 255, 0
	for _, p := range px {
		if p[3] < lo {
			lo = p[3]
		}
		if p[3] > hi {
			hi = p[3]
		}
	}
	b[0], b[1] = byte(hi), byte(lo)
	var pal [8]int
	pal[0], pal[1] = hi, lo
	for i := 1; i < 7; i++ {
		pal[i+1] = ((7-i)*hi + i*lo) / 7
	}
	if hi == lo {
		for i := 2; i < 8; i++ {
			pal[i] = hi
		}
	}
	var bits uint64
	for i, p := range px {
		best, bd := 0, 1<<30
		for k := 0; k < 8; k++ {
			d := p[3] - pal[k]
			if d < 0 {
				d = -d
			}
			if d < bd {
				best, bd = k, d
			}
		}
		bits |= uint64(best) << (3 * i)
	}
	for i := 0; i < 6; i++ {
		b[2+i] = byte(bits >> (8 * i))
	}
}

func to565(c [3]int) uint16 {
	return uint16((c[0]*31+127)/255)<<11 | uint16((c[1]*63+127)/255)<<5 | uint16((c[2]*31+127)/255)
}

func encodeColor(px [][4]int, b []byte) {
	mn, mx := [3]int{255, 255, 255}, [3]int{0, 0, 0}
	for _, p := range px {
		for k := 0; k < 3; k++ {
			if p[k] < mn[k] {
				mn[k] = p[k]
			}
			if p[k] > mx[k] {
				mx[k] = p[k]
			}
		}
	}
	for k := 0; k < 3; k++ {
		in := (mx[k] - mn[k]) / 16
		mn[k] += in
		mx[k] -= in
	}
	c0, c1 := to565(mx), to565(mn)
	if c0 < c1 {
		c0, c1 = c1, c0
	}
	binary.LittleEndian.PutUint16(b[0:], c0)
	binary.LittleEndian.PutUint16(b[2:], c1)
	a, z := c565(c0), c565(c1)
	var pal [4][3]int
	pal[0], pal[1] = a, z
	for k := 0; k < 3; k++ {
		pal[2][k] = (2*a[k] + z[k]) / 3
		pal[3][k] = (a[k] + 2*z[k]) / 3
	}
	var bits uint32
	if c0 != c1 {
		for i, p := range px {
			best, bd := 0, 1<<30
			for k := 0; k < 4; k++ {
				d := (p[0]-pal[k][0])*(p[0]-pal[k][0]) + (p[1]-pal[k][1])*(p[1]-pal[k][1]) + (p[2]-pal[k][2])*(p[2]-pal[k][2])
				if d < bd {
					best, bd = k, d
				}
			}
			bits |= uint32(best) << (2 * i)
		}
	}
	binary.LittleEndian.PutUint32(b[4:], bits)
}
