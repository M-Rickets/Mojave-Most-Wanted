package audio

import (
	"math"
	"math/cmplx"
	"sort"

	"mmw/internal/bnd2"
)

// SirenCandidate is a wave that sounds like a siren: one strong tone between 300 and 3000 Hz that sweeps.
type SirenCandidate struct {
	ID           uint64
	Score        float64
	Tonality     float64
	MinHz, MaxHz float64
	Seconds      float64
	Samples      []int16
	Rate         uint32
}

func fft(a []complex128) {
	n := len(a)
	for i, j := 1, 0; i < n; i++ {
		bit := n >> 1
		for ; j&bit != 0; bit >>= 1 {
			j ^= bit
		}
		j ^= bit
		if i < j {
			a[i], a[j] = a[j], a[i]
		}
	}
	for l := 2; l <= n; l <<= 1 {
		w := cmplx.Exp(complex(0, -2*math.Pi/float64(l)))
		for i := 0; i < n; i += l {
			wk := complex(1, 0)
			for k := 0; k < l/2; k++ {
				u, v := a[i+k], a[i+k+l/2]*wk
				a[i+k], a[i+k+l/2] = u+v, u-v
				wk *= w
			}
		}
	}
}

// FindSirens scores every short mono wave in a bundle (developer aid: candidates are logged and saved so the siren's
// resource id can be confirmed by ear/spectrogram and written into the sounds sheet).
func FindSirens(b *bnd2.Bundle, max int) []SirenCandidate {
	var out []SirenCandidate
	for _, r := range b.Res {
		if r.Type != 0x81 {
			continue
		}
		pcm, rate, err := WaveResource(b, r.ID)
		if err != nil || rate == 0 || len(pcm) < int(rate)/3 || len(pcm) > int(rate)*8 {
			continue
		}
		const w = 2048
		var freqs, tons []float64
		buf := make([]complex128, w)
		for i := 0; i+w <= len(pcm); i += w / 2 {
			for k := 0; k < w; k++ {
				win := 0.5 - 0.5*math.Cos(2*math.Pi*float64(k)/float64(w-1))
				buf[k] = complex(float64(pcm[i+k])*win, 0)
			}
			fft(buf)
			lo, hi := int(300*w/float64(rate)), int(3000*w/float64(rate))
			best, sum := 0.0, 0.0
			bk := lo
			for k := lo; k < hi; k++ {
				m := cmplx.Abs(buf[k])
				sum += m
				if m > best {
					best, bk = m, k
				}
			}
			freqs = append(freqs, float64(bk)*float64(rate)/w)
			tons = append(tons, best/(sum/float64(hi-lo)+1e-9))
		}
		if len(freqs) < 4 {
			continue
		}
		t := append([]float64{}, tons...)
		sort.Float64s(t)
		f := append([]float64{}, freqs...)
		sort.Float64s(f)
		lo, hi := f[len(f)/10], f[len(f)*9/10]
		jump := 0.0
		for i := 1; i < len(freqs); i++ {
			jump += math.Abs(freqs[i] - freqs[i-1])
		}
		jump /= float64(len(freqs) - 1)
		ton := t[len(t)/2]
		sweep := hi - lo
		score := ton * math.Min(sweep, 1200) / (100 + jump)
		out = append(out, SirenCandidate{ID: r.ID, Score: score, Tonality: ton, MinHz: lo, MaxHz: hi, Seconds: float64(len(pcm)) / float64(rate), Samples: pcm, Rate: rate})
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Score > out[j].Score })
	if len(out) > max {
		out = out[:max]
	}
	return out
}
