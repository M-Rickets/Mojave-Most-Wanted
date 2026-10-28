// Package audio converts EA audio from the player's Need for Speed installs: EA-XAS v0 engine samples (Ginsu),
// EAAudioCore.SPS streams and EALayer3 (repacked to standard MP3 frames).
package audio

import "encoding/binary"

var xaCoefs = [16][2]float32{{0, 0}, {0.9375, 0}, {1.796875, -0.8125}, {1.53125, -0.859375}}

func clamp16(v int32) int16 {
	if v > 32767 {
		return 32767
	}
	if v < -32768 {
		return -32768
	}
	return int16(v)
}

// DecodeXASv0 decodes mono EA-XAS v0: frames of 0x13 bytes (4-byte header + 15 nibble bytes) holding 32 samples.
func DecodeXASv0(data []byte, numSamples int) []int16 {
	out := make([]int16, 0, numSamples)
	for off := 0; off+0x13 <= len(data) && len(out) < numSamples; off += 0x13 {
		h := binary.LittleEndian.Uint32(data[off:])
		c1, c2 := xaCoefs[h&0x0F][0], xaCoefs[h&0x0F][1]
		hist2 := int16(h & 0xFFF0)
		hist1 := int16((h >> 16) & 0xFFF0)
		shift := (h >> 16) & 0x0F
		out = append(out, hist2, hist1)
		for i := 0; i < 30; i++ {
			nb := data[off+4+i/2]
			var n uint8
			if i&1 == 1 {
				n = nb & 0x0F
			} else {
				n = nb >> 4
			}
			s := int32(int16(uint16(n)<<12)) >> shift
			v := clamp16(int32(float32(s) + float32(hist1)*c1 + float32(hist2)*c2))
			out = append(out, v)
			hist2, hist1 = hist1, v
		}
	}
	if len(out) > numSamples {
		out = out[:numSamples]
	}
	return out
}
