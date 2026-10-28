package audio

import (
	"encoding/binary"
	"fmt"
)

// SPSInfo is the EAAudioCore header of an.SPS stream.
type SPSInfo struct {
	Codec, Channels, Rate, Samples int
}

// ReadSPS parses an EAAudioCore V1 stream (.SPS: 'H' header block, 'D' data blocks, 'E' end) and returns the header and
// the codec data with the block headers removed.
func ReadSPS(b []byte) (*SPSInfo, []byte, error) {
	be := binary.BigEndian
	if len(b) < 12 || b[0] != 0x48 {
		return nil, nil, fmt.Errorf("not an SPS stream")
	}
	hsize := int(be.Uint32(b) & 0xFFFFFF)
	h1, h2 := be.Uint32(b[4:]), be.Uint32(b[8:])
	info := &SPSInfo{Codec: int(h1>>24) & 0x0F, Channels: int(h1>>18)&0x3F + 1, Rate: int(h1 & 0x3FFFF), Samples: int(h2 & 0x1FFFFFFF)}
	if h1>>28 != 1 {
		return nil, nil, fmt.Errorf("SPS version %d", h1>>28)
	}
	var data []byte
	for off := hsize; off+8 <= len(b); {
		id, size := b[off], int(be.Uint32(b[off:])&0xFFFFFF)
		if size < 8 || off+size > len(b) {
			break
		}
		if id == 0x44 {
			data = append(data, b[off+8:off+size]...)
		}
		off += size
		if id == 0x45 {
			break
		}
	}
	return info, data, nil
}

type bitReader struct {
	b   []byte
	pos int
}

func (r *bitReader) get(n int) (uint32, bool) {
	if r.pos+n > len(r.b)*8 {
		return 0, false
	}
	var v uint32
	for i := 0; i < n; i++ {
		bit := (r.b[r.pos>>3] >> (7 - uint(r.pos&7))) & 1
		v = v<<1 | uint32(bit)
		r.pos++
	}
	return v, true
}

type bitWriter struct {
	b   []byte
	pos int
}

func (w *bitWriter) put(n int, v uint32) {
	for i := n - 1; i >= 0; i-- {
		if w.pos>>3 >= len(w.b) {
			w.b = append(w.b, 0)
		}
		if (v>>uint(i))&1 == 1 {
			w.b[w.pos>>3] |= 1 << (7 - uint(w.pos&7))
		}
		w.pos++
	}
}

// eaGranule is one parsed EALayer3 frame (one MPEG granule).
type eaGranule struct {
	versionIdx, rateIdx, chanMode, modeExt uint32
	granule                                uint32
	channels                               int
	scfsi                                  [2]uint32
	size                                   [2]uint32
	o1, o2                                 [2]uint32
	data                                   []byte
	dataBits                               int
}

// parseEAL3v1b reads EALayer3 v1 frames as stored in SNS/SPS streams.
func parseEAL3v1b(b []byte) ([]eaGranule, error) {
	var out []eaGranule
	r := &bitReader{b: b}
	channels := 0
	for r.pos/8 < len(b) {
		start := r.pos
		flag, ok := r.get(8)
		if !ok {
			break
		}
		if flag != 0x00 && flag != 0xEE {
			if len(out) == 0 {
				return nil, fmt.Errorf("EALayer3: bad frame flag %#x", flag)
			}
			break
		}
		var g eaGranule
		cbase := r.pos
		v, _ := r.get(8)
		g.versionIdx, g.rateIdx, g.chanMode, g.modeExt = v>>6, (v>>4)&3, (v>>2)&3, v&3
		if v == 0 {
			break
		}
		if g.versionIdx != 3 {
			return nil, fmt.Errorf("EALayer3: only MPEG-1 streams are supported")
		}
		g.channels = 2
		if g.chanMode == 3 {
			g.channels = 1
		}
		g.granule, _ = r.get(1)
		if g.granule == 1 {
			for c := 0; c < g.channels; c++ {
				g.scfsi[c], _ = r.get(4)
			}
		}
		for c := 0; c < g.channels; c++ {
			g.size[c], _ = r.get(12)
			g.o1[c], _ = r.get(32)
			g.o2[c], ok = r.get(15)
		}
		if !ok {
			break
		}
		total := 0
		for c := 0; c < g.channels; c++ {
			total += int(g.size[c])
		}
		w := &bitWriter{}
		for i := 0; i < total; i++ {
			bit, ok := r.get(1)
			if !ok {
				return out, nil
			}
			w.put(1, bit)
		}
		g.data, g.dataBits = w.b, total
		if (r.pos-cbase)%8 != 0 {
			r.pos += 8 - (r.pos-cbase)%8
		}
		if flag == 0xEE {
			off, _ := r.get(16)
			pcm, _ := r.get(16)
			r.get(32)
			_ = off
			r.pos += int(pcm) * g.channels * 16
		}
		channels = g.channels
		_ = start
		out = append(out, g)
	}
	_ = channels
	return out, nil
}

var mpeg1Rates = [3]int{44100, 48000, 32000}
var mpeg1Bitrates = [15]int{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}

// EALayer3ToMP3 repacks EALayer3 v1 granules into a standard MPEG-1 Layer III stream.
func EALayer3ToMP3(payload []byte) ([]byte, int, error) {
	gr, err := parseEAL3v1b(payload)
	if err != nil {
		return nil, 0, err
	}
	var g0s, g1s []*eaGranule
	for i := 0; i+1 < len(gr); i++ {
		if gr[i].granule == 0 && gr[i+1].granule == 1 && gr[i].channels == gr[i+1].channels {
			g0s, g1s = append(g0s, &gr[i]), append(g1s, &gr[i+1])
			i++
		}
	}
	if len(g0s) == 0 {
		return nil, 0, fmt.Errorf("EALayer3: no frames")
	}
	first := g0s[0]
	rate := mpeg1Rates[first.rateIdx]
	side := 32
	if first.channels == 1 {
		side = 17
	}
	type frameRec struct{ off, area, cap int }
	var frames []frameRec
	out := make([]byte, 0, len(payload)+len(payload)/4)
	header := func(bitrateIdx int, g0 *eaGranule, g1 *eaGranule, mdBegin int) []byte {
		w := &bitWriter{}
		w.put(11, 0x7FF)
		w.put(2, 3)
		w.put(2, 1)
		w.put(1, 1)
		w.put(4, uint32(bitrateIdx))
		w.put(2, first.rateIdx)
		w.put(1, 0)
		w.put(1, 0)
		w.put(2, first.chanMode)
		if g0 != nil {
			w.put(2, g0.modeExt)
		} else {
			w.put(2, 0)
		}
		w.put(1, 0)
		w.put(1, 1)
		w.put(2, 0)
		w.put(9, uint32(mdBegin))
		if first.channels == 1 {
			w.put(5, 0)
		} else {
			w.put(3, 0)
		}
		if g0 == nil {
			for w.pos < (4+side)*8 {
				w.put(1, 0)
			}
			return w.b
		}
		for c := 0; c < g1.channels; c++ {
			w.put(4, g1.scfsi[c])
		}
		for _, g := range []*eaGranule{g0, g1} {
			for c := 0; c < g.channels; c++ {
				w.put(12, g.size[c])
				w.put(32, g.o1[c])
				w.put(15, g.o2[c])
			}
		}
		return w.b
	}
	frameSize := func(bi int) int { return 144 * mpeg1Bitrates[bi] * 1000 / rate }
	xingBI := 9
	out = append(out, make([]byte, frameSize(xingBI))...)
	copy(out, header(xingBI, nil, nil, 0))
	n := len(g0s)
	lens := make([]int, n)
	cmax := frameSize(len(mpeg1Bitrates)-1) - 4 - side
	silence := func(k int) {
		for _, g := range []*eaGranule{g0s[k], g1s[k]} {
			g.dataBits, g.data = 0, nil
			for c := range g.size {
				g.size[c] = 0
			}
		}
		lens[k] = 0
	}
	for k := 0; k < n; k++ {
		lens[k] = (g0s[k].dataBits + g1s[k].dataBits + 7) / 8
		if lens[k] > cmax+511 {
			silence(k)
		}
	}
	need := make([]int, n+1)
	for k := n - 1; k >= 0; k-- {
		need[k] = lens[k] + need[k+1] - cmax
		if need[k] > 511 {
			silence(k)
			need[k] = need[k+1] - cmax
		}
		if need[k] < 0 {
			need[k] = 0
		}
	}
	pos, area := 0, 0
	for k := range g0s {
		w := &bitWriter{}
		for _, g := range []*eaGranule{g0s[k], g1s[k]} {
			r := &bitReader{b: g.data}
			for i := 0; i < g.dataBits; i++ {
				bit, _ := r.get(1)
				w.put(1, bit)
			}
		}
		md := w.b
		avail := area - pos
		if avail > 511 {
			avail = 511
		}
		start := area - avail
		bi := 1
		for ; bi < len(mpeg1Bitrates); bi++ {
			c := frameSize(bi) - 4 - side
			left := avail + c - len(md)
			if left > 511 {
				left = 511
			}
			if len(md) <= avail+c && left >= need[k+1] {
				break
			}
		}
		if bi == len(mpeg1Bitrates) {
			return nil, 0, fmt.Errorf("EALayer3: frame %d too big", k)
		}
		size := frameSize(bi)
		f := frameRec{off: len(out), area: area, cap: size - 4 - side}
		out = append(out, make([]byte, size)...)
		copy(out[f.off:], header(bi, g0s[k], g1s[k], area-start))
		frames = append(frames, f)
		j := len(frames) - 1
		for i := 0; i < len(md); i++ {
			v := start + i
			for frames[j].area > v {
				j--
			}
			for j+1 < len(frames) && frames[j+1].area <= v {
				j++
			}
			out[frames[j].off+4+side+(v-frames[j].area)] = md[i]
		}
		pos = start + len(md)
		area += f.cap
	}
	x := 4 + side
	copy(out[x:], "Xing")
	binary.BigEndian.PutUint32(out[x+4:], 3)
	binary.BigEndian.PutUint32(out[x+8:], uint32(len(frames)+1))
	binary.BigEndian.PutUint32(out[x+12:], uint32(len(out)))
	secs := float64(len(frames)+1) * 1152 / float64(rate)
	return out, int(float64(len(out)) * 8 / secs / 1000), nil
}
