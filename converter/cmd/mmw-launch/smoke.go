package main

import (
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"log"
	"math"
	"path/filepath"

	"mmw/internal/bnd2"
	"mmw/internal/crit"
	"mmw/internal/dds"
)

// makeSmoke builds the burnout smoke on the player's PC
func makeSmoke(data string, read func(path string) []byte) []byte {
	b := read(smokeRecipe.Template)
	if b == nil {
		log.Printf("tyre smoke: %s not in the archives", smokeRecipe.Template)
		return nil
	}
	sum := sha1.Sum(b)
	if len(b) != smokeRecipe.Size || hex.EncodeToString(sum[:]) != smokeRecipe.Sha1 {
		log.Printf("tyre smoke: %s isn't the one the recipe was made from (%d bytes)", smokeRecipe.Template, len(b))
		return nil
	}
	n, err := parseNif(b)
	if err != nil {
		log.Printf("tyre smoke: %v", err)
		return nil
	}
	out := append([]byte(nil), b...)
	for _, p := range smokeRecipe.Patches {
		v, _ := hex.DecodeString(p.Bytes)
		at := n.blockStart[p.Block] + p.Off
		copy(out[at:at+len(v)], v)
	}
	out, err = n.replaceTexture(out, smokeRecipe.OldTexture, smokeRecipe.Texture, smokeRecipe.ShaderBlock)
	if err != nil {
		log.Printf("tyre smoke: %v", err)
		return nil
	}
	return out
}

// nifLayout: where the parts of a NIF 20.2.0.7 (Fallout 3 / New Vegas, BS version 34) file are.
type nifLayout struct {
	numBlocks             int
	sizesAt               int
	stringsAt, stringsEnd int
	maxLenAt              int
	blockStart            []int
	blockSize             []int
}

func parseNif(b []byte) (*nifLayout, error) {
	le := binary.LittleEndian
	nl := bytes.IndexByte(b, '\n')
	if nl < 0 || !bytes.HasPrefix(b, []byte("Gamebryo File Format, Version 20.2.0.7")) {
		return nil, fmt.Errorf("not a NIF 20.2.0.7")
	}
	p := nl + 1
	need := func(n int) error {
		if p+n > len(b) {
			return fmt.Errorf("NIF header cut short")
		}
		return nil
	}
	if err := need(17); err != nil {
		return nil, err
	}
	p += 4 + 1 + 4
	l := &nifLayout{numBlocks: int(le.Uint32(b[p:]))}
	p += 4
	p += 4
	for i := 0; i < 3; i++ {
		if err := need(1); err != nil {
			return nil, err
		}
		p += 1 + int(b[p])
	}
	if err := need(2); err != nil {
		return nil, err
	}
	nt := int(le.Uint16(b[p:]))
	p += 2
	for i := 0; i < nt; i++ {
		if err := need(4); err != nil {
			return nil, err
		}
		p += 4 + int(le.Uint32(b[p:]))
	}
	p += 2 * l.numBlocks
	l.sizesAt = p
	if err := need(4 * l.numBlocks); err != nil {
		return nil, err
	}
	for i := 0; i < l.numBlocks; i++ {
		l.blockSize = append(l.blockSize, int(le.Uint32(b[p+4*i:])))
	}
	p += 4 * l.numBlocks
	l.stringsAt = p
	if err := need(8); err != nil {
		return nil, err
	}
	ns := int(le.Uint32(b[p:]))
	l.maxLenAt = p + 4
	p += 8
	for i := 0; i < ns; i++ {
		if err := need(4); err != nil {
			return nil, err
		}
		p += 4 + int(le.Uint32(b[p:]))
	}
	l.stringsEnd = p
	if err := need(4); err != nil {
		return nil, err
	}
	p += 4 + 4*int(le.Uint32(b[p:]))
	for _, z := range l.blockSize {
		l.blockStart = append(l.blockStart, p)
		p += z
	}
	if p > len(b) {
		return nil, fmt.Errorf("NIF blocks run past the end")
	}
	return l, nil
}

// replaceTexture swaps a texture path in the header string table and in one block's own sized string (the no-lighting
// shader keeps a copy), fixing that block's size and the longest-string field.
func (l *nifLayout) replaceTexture(b []byte, old, nw string, shaderBlock int) ([]byte, error) {
	le := binary.LittleEndian
	sized := func(s string) []byte {
		o := make([]byte, 4+len(s))
		le.PutUint32(o, uint32(len(s)))
		copy(o[4:], s)
		return o
	}
	if shaderBlock >= 0 && shaderBlock < l.numBlocks {
		st, z := l.blockStart[shaderBlock], l.blockSize[shaderBlock]
		blk := b[st : st+z]
		if k := bytes.Index(blk, sized(old)); k >= 0 {
			nb := append(append(append([]byte(nil), blk[:k]...), sized(nw)...), blk[k+len(sized(old)):]...)
			b = append(append(append([]byte(nil), b[:st]...), nb...), b[st+z:]...)
			le.PutUint32(b[l.sizesAt+4*shaderBlock:], uint32(len(nb)))
		} else {
			return nil, fmt.Errorf("shader texture path not found")
		}
	}
	k := bytes.Index(b[l.stringsAt:l.stringsEnd], sized(old))
	if k < 0 {
		return nil, fmt.Errorf("texture path not in the string table")
	}
	k += l.stringsAt
	b = append(append(append([]byte(nil), b[:k]...), sized(nw)...), b[k+len(sized(old)):]...)
	if int(le.Uint32(b[l.maxLenAt:])) < len(nw) {
		le.PutUint32(b[l.maxLenAt:], uint32(len(nw)))
	}
	return b, nil
}

// smokeTexture: a soft round puff (white, alpha falling off to the edge, broken up by noise) as DXT5 with mips, used
// until Hot Pursuit's own tyre smoke texture is picked (smokeRecipe.HPTexture).
func smokeTexture() []byte {
	const n = 128
	noise := periodicNoise(n, n, 6, 7)
	fine := periodicNoise(n, n, 16, 11)
	rgba := make([]byte, n*n*4)
	for y := 0; y < n; y++ {
		for x := 0; x < n; x++ {
			dx, dy := (float64(x)+0.5)/n*2-1, (float64(y)+0.5)/n*2-1
			r := math.Sqrt(dx*dx + dy*dy)
			fall := math.Max(0, 1-r*r)
			fall = math.Pow(fall, 1.4)
			a := fall * (0.55 + 0.35*float64(noise[y*n+x]) + 0.15*float64(fine[y*n+x]))
			v := 200 + 45*float64(noise[y*n+x])
			o := (y*n + x) * 4
			rgba[o], rgba[o+1], rgba[o+2] = byte(v), byte(v), byte(v)
			rgba[o+3] = byte(math.Min(255, a*255))
		}
	}
	return dds.DXT5Mips(rgba, n, n)
}

// hpSmokeTexture: Hot Pursuit Remastered's tyre smoke texture from GLOBALEFFECTS.BNDL, once its id is known.
func hpSmokeTexture(hp string, id uint64) []byte {
	if hp == "" || id == 0 {
		return nil
	}
	b, err := bnd2.Open(filepath.Join(hp, "GLOBALEFFECTS.BNDL"))
	if err != nil {
		return nil
	}
	for _, r := range b.Res {
		if r.ID != id {
			continue
		}
		ra, err := crit.ReadRaster(r, b.Ptr64)
		if err != nil {
			return nil
		}
		d, err := dds.FromDXGI(ra.Format, ra.Width, ra.Height, ra.Mips, ra.Pixels, 512)
		if err != nil {
			return nil
		}
		return d
	}
	return nil
}
