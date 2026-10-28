package audio

// PairSizes returns the main-data bytes of each MPEG frame (granule pair) in an EALayer3 payload (developer stats).
func PairSizes(payload []byte) []int {
	gr, _ := parseEAL3v1b(payload)
	var out []int
	for i := 0; i+1 < len(gr); i++ {
		if gr[i].granule == 0 && gr[i+1].granule == 1 {
			out = append(out, (gr[i].dataBits+gr[i+1].dataBits+7)/8)
			i++
		}
	}
	return out
}

// SimulateReservoir returns the first frame that does not fit at a bitrate (or -1) (developer stats).
func SimulateReservoir(sizes []int, kbps, rate, side int) (int, int) {
	c := 144*kbps*1000/rate - 4 - side
	pos, area := 0, 0
	minSlack := 1 << 30
	for k, l := range sizes {
		start := pos
		if area-start > 511 {
			start = area - 511
		}
		if start+l > area+c {
			return k, minSlack
		}
		if s := area + c - (start + l); s < minSlack {
			minSlack = s
		}
		pos = start + l
		area += c
	}
	return -1, minSlack
}
