// Package nif writes the small subset of Gamebryo 20.2.0.7 (Fallout: New Vegas, user 11, BS version 34) NIF files the
// mashup needs: nodes, triangle shapes, lighting-shader properties and single-bone skinning.
package nif

import (
	"bytes"
	"encoding/binary"
	"math"
	"os"
)

type Writer struct {
	types   []string
	blocks  []block
	strings []string
	strIdx  map[string]int
}

type block struct {
	typ int
	buf *bytes.Buffer
}

func New() *Writer { return &Writer{strIdx: map[string]int{}} }

func (w *Writer) typeIndex(t string) int {
	for i, x := range w.types {
		if x == t {
			return i
		}
	}
	w.types = append(w.types, t)
	return len(w.types) - 1
}

// Str returns the header string index for s ("" = none).
func (w *Writer) Str(s string) uint32 {
	if s == "" {
		return 0xFFFFFFFF
	}
	if i, ok := w.strIdx[s]; ok {
		return uint32(i)
	}
	w.strings = append(w.strings, s)
	w.strIdx[s] = len(w.strings) - 1
	return uint32(len(w.strings) - 1)
}

// Reserve adds an empty block of type t and returns its index (filled later with Set).
func (w *Writer) Reserve(t string) int {
	w.blocks = append(w.blocks, block{typ: w.typeIndex(t), buf: &bytes.Buffer{}})
	return len(w.blocks) - 1
}

func (w *Writer) Set(i int, b *B) { w.blocks[i].buf = &b.Buffer }

// B is a little-endian block builder.
type B struct{ bytes.Buffer }

func (b *B) U8(v uint8)   { b.WriteByte(v) }
func (b *B) U16(v uint16) { binary.Write(b, binary.LittleEndian, v) }
func (b *B) U32(v uint32) { binary.Write(b, binary.LittleEndian, v) }
func (b *B) I32(v int32)  { binary.Write(b, binary.LittleEndian, v) }
func (b *B) F32(v float32) {
	if v != v || v > math.MaxFloat32 || v < -math.MaxFloat32 {
		v = 0
	}
	binary.Write(b, binary.LittleEndian, math.Float32bits(v))
}
func (b *B) Ref(i int) { b.I32(int32(i)) }
func (b *B) Vec(v [3]float32) {
	b.F32(v[0])
	b.F32(v[1])
	b.F32(v[2])
}
func (b *B) Identity33() {
	for r := 0; r < 3; r++ {
		for c := 0; c < 3; c++ {
			if r == c {
				b.F32(1)
			} else {
				b.F32(0)
			}
		}
	}
}
func (b *B) Sized(s string) { b.U32(uint32(len(s))); b.WriteString(s) }

// ObjectNET writes NiObjectNET (name, extra data refs, controller).
func (w *Writer) ObjectNET(b *B, name string, extra []int) {
	b.U32(w.Str(name))
	b.U32(uint32(len(extra)))
	for _, e := range extra {
		b.Ref(e)
	}
	b.Ref(-1)
}

// AVObject writes NiAVObject fields (identity rotation).
func (w *Writer) AVObject(b *B, flags uint32, tr [3]float32, props []int) {
	w.AVObjectC(b, flags, tr, props, -1)
}

func (w *Writer) AVObjectC(b *B, flags uint32, tr [3]float32, props []int, collision int) {
	b.U32(flags)
	b.Vec(tr)
	b.Identity33()
	b.F32(1)
	b.U32(uint32(len(props)))
	for _, p := range props {
		b.Ref(p)
	}
	b.Ref(collision)
}

func (w *Writer) Node(idx int, typ, name string, extra, children []int) {
	w.NodeC(idx, name, extra, children, -1)
}

func (w *Writer) NodeC(idx int, name string, extra, children []int, collision int) {
	b := &B{}
	w.ObjectNET(b, name, extra)
	w.AVObjectC(b, 0x8000E, [3]float32{}, nil, collision)
	b.U32(uint32(len(children)))
	for _, c := range children {
		b.Ref(c)
	}
	b.U32(0)

	w.Set(idx, b)
}

// NodeAt writes a plain NiNode with a translation.
func (w *Writer) NodeAt(idx int, name string, tr [3]float32, children []int) {
	w.NodeFlags(idx, name, tr, children, 0x8000E)
}

// NodeFlags writes a plain NiNode with a translation and NiAVObject flags (bit 0 = hidden).
func (w *Writer) NodeFlags(idx int, name string, tr [3]float32, children []int, flags uint32) {
	b := &B{}
	w.ObjectNET(b, name, nil)
	w.AVObjectC(b, flags, tr, nil, -1)
	b.U32(uint32(len(children)))
	for _, c := range children {
		b.Ref(c)
	}
	b.U32(0)
	w.Set(idx, b)
}

func exportString(buf *bytes.Buffer, s string) {
	buf.WriteByte(byte(len(s) + 1))
	buf.WriteString(s)
	buf.WriteByte(0)
}

// Bytes serialises the file.
func (w *Writer) Bytes() []byte {
	out := &bytes.Buffer{}
	out.WriteString("Gamebryo File Format, Version 20.2.0.7\n")
	le := binary.LittleEndian
	binary.Write(out, le, uint32(0x14020007))
	out.WriteByte(1)
	binary.Write(out, le, uint32(11))
	binary.Write(out, le, uint32(len(w.blocks)))
	binary.Write(out, le, uint32(34))
	exportString(out, "Mojave Most Wanted")
	exportString(out, "")
	exportString(out, "")
	binary.Write(out, le, uint16(len(w.types)))
	for _, t := range w.types {
		binary.Write(out, le, uint32(len(t)))
		out.WriteString(t)
	}
	for _, b := range w.blocks {
		binary.Write(out, le, uint16(b.typ))
	}
	for _, b := range w.blocks {
		binary.Write(out, le, uint32(b.buf.Len()))
	}
	binary.Write(out, le, uint32(len(w.strings)))
	maxl := 0
	for _, s := range w.strings {
		if len(s) > maxl {
			maxl = len(s)
		}
	}
	binary.Write(out, le, uint32(maxl))
	for _, s := range w.strings {
		binary.Write(out, le, uint32(len(s)))
		out.WriteString(s)
	}
	binary.Write(out, le, uint32(0))
	for _, b := range w.blocks {
		out.Write(b.buf.Bytes())
	}
	binary.Write(out, le, uint32(1))
	binary.Write(out, le, int32(0))
	return out.Bytes()
}

func (w *Writer) Save(path string) error { return os.WriteFile(path, w.Bytes(), 0o644) }
