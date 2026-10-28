package audio

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"

	mp3 "github.com/hajimehoshi/go-mp3"

	"mmw/internal/bnd2"
)

// WaveResource decodes a short in-memory wave (bundle resource type 0x81: header, then an EAAudioCore.SPS stream with
// EALayer3) to 16-bit mono PCM: repacked to MP3 frames, then decoded with go-mp3 (Apache 2.0).
func WaveResource(b *bnd2.Bundle, id uint64) ([]int16, uint32, error) {
	r := b.ByID[id]
	if r == nil {
		return nil, 0, fmt.Errorf("wave %x not in %s", id, b.Path)
	}
	d, err := r.Data(0)
	if err != nil {
		return nil, 0, err
	}
	k := bytes.Index(d, []byte{0x48, 0, 0})
	if k < 0 || k > 0x200 {
		return nil, 0, fmt.Errorf("wave %x: no stream header", id)
	}
	info, payload, err := ReadSPS(d[k:])
	if err != nil {
		return nil, 0, err
	}
	if info.Codec != 5 {
		return nil, 0, fmt.Errorf("wave %x: codec %d", id, info.Codec)
	}
	m, _, err := EALayer3ToMP3(payload)
	if err != nil {
		return nil, 0, err
	}
	dec, err := mp3.NewDecoder(bytes.NewReader(m))
	if err != nil {
		return nil, 0, err
	}
	pcm, err := io.ReadAll(dec)
	if err != nil && len(pcm) == 0 {
		return nil, 0, err
	}
	n := len(pcm) / 4
	out := make([]int16, 0, n)
	for i := 0; i < n; i++ {
		l := int32(int16(binary.LittleEndian.Uint16(pcm[i*4:])))
		rr := int32(int16(binary.LittleEndian.Uint16(pcm[i*4+2:])))
		out = append(out, int16((l+rr)/2))
	}
	skip := 1152 + 529
	if len(out) > skip+info.Samples {
		out = out[skip : skip+info.Samples]
	} else if len(out) > skip {
		out = out[skip:]
	}
	return out, uint32(dec.SampleRate()), nil
}
