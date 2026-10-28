// Package bnd2 reads Criterion "bnd2" bundles (Need for Speed Hot Pursuit 2010/Remastered = v3, Need for Speed Most
// Wanted 2012 = v5).
package bnd2

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"regexp"
	"strconv"
)

type Resource struct {
	ID          uint64
	Type        uint32
	USize       [4]uint32
	DSize       [4]uint32
	DOff        [4]uint32
	ImportOff   uint32
	ImportCount uint16
	Flags       uint8
	Stream      uint8
	raw         [4][]byte
	cache       [4][]byte
	compressed  bool
}

type Import struct {
	ID     uint64
	Offset uint32
}

type Bundle struct {
	Path     string
	Version  int
	Ptr64    bool
	Flags    uint32
	Res      []*Resource
	ByID     map[uint64]*Resource
	Names    map[uint64]string
	TypeName map[uint64]string
}

var le = binary.LittleEndian

func Open(path string) (*Bundle, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	bd, err := Parse(b)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	bd.Path = path
	return bd, nil
}

func Parse(b []byte) (*Bundle, error) {
	if len(b) < 0x30 || string(b[:4]) != "bnd2" {
		return nil, fmt.Errorf("not a bnd2 bundle")
	}
	bd := &Bundle{ByID: map[uint64]*Resource{}, Names: map[uint64]string{}, TypeName: map[uint64]string{}}
	var dbg, cnt, eoff uint32
	var doffs [4]uint32
	stride := 0
	switch le.Uint16(b[4:]) {
	case 3:
		bd.Version, bd.Ptr64 = 3, true
		dbg, cnt, eoff = le.Uint32(b[0xC:]), le.Uint32(b[0x10:]), le.Uint32(b[0x14:])
		for i := 0; i < 4; i++ {
			doffs[i] = le.Uint32(b[0x18+4*i:])
		}
		bd.Flags = le.Uint32(b[0x28:])
		stride = 0x50
	case 5:
		bd.Version = 5
		dbg, cnt, eoff = le.Uint32(b[0x8:]), le.Uint32(b[0xC:]), le.Uint32(b[0x10:])
		for i := 0; i < 4; i++ {
			doffs[i] = le.Uint32(b[0x14+4*i:])
		}
		bd.Flags = le.Uint32(b[0x24:])
		stride = 0x48
	default:
		return nil, fmt.Errorf("unsupported bundle version %d", le.Uint16(b[4:]))
	}
	if int(eoff)+int(cnt)*stride > len(b) {
		return nil, fmt.Errorf("truncated resource table")
	}
	for i := 0; i < int(cnt); i++ {
		o := int(eoff) + i*stride
		r := &Resource{compressed: bd.Flags&1 != 0}
		r.ID = le.Uint64(b[o:])
		o += 8
		if bd.Version == 3 {
			o += 8
		}
		for k := 0; k < 4; k++ {
			r.USize[k] = le.Uint32(b[o+4*k:])
			r.DSize[k] = le.Uint32(b[o+16+4*k:])
			r.DOff[k] = le.Uint32(b[o+32+4*k:])
		}
		o += 48
		r.ImportOff = le.Uint32(b[o:])
		r.Type = le.Uint32(b[o+4:])
		r.ImportCount = le.Uint16(b[o+8:])
		r.Flags = b[o+10]
		r.Stream = b[o+11]
		for k := 0; k < 4; k++ {
			if r.DSize[k] == 0 {
				continue
			}
			s, e := int(doffs[k])+int(r.DOff[k]), int(doffs[k])+int(r.DOff[k])+int(r.DSize[k])
			if e > len(b) {
				return nil, fmt.Errorf("resource %x chunk %d out of range", r.ID, k)
			}
			r.raw[k] = b[s:e]
		}
		bd.Res = append(bd.Res, r)
		bd.ByID[r.ID] = r
	}
	if int(dbg) < len(b) {
		re := regexp.MustCompile(`id="([0-9a-f]+)" type="([^"]*)" name="([^"]*)"`)
		for _, m := range re.FindAllSubmatch(b[dbg:], -1) {
			id, _ := strconv.ParseUint(string(m[1]), 16, 64)
			bd.Names[id] = string(m[3])
			bd.TypeName[id] = string(m[2])
		}
	}
	return bd, nil
}

// Data returns chunk k of a resource, decompressed.
func (r *Resource) Data(k int) ([]byte, error) {
	if r.cache[k] != nil || r.raw[k] == nil {
		return r.cache[k], nil
	}
	want := int(r.USize[k] & 0x0FFFFFFF)
	if !r.compressed {
		r.cache[k] = r.raw[k]
		return r.cache[k], nil
	}
	zr, err := zlib.NewReader(bytes.NewReader(r.raw[k]))
	if err != nil {
		return nil, fmt.Errorf("resource %x: %w", r.ID, err)
	}
	out := make([]byte, 0, want)
	buf := bytes.NewBuffer(out)
	if _, err := io.Copy(buf, zr); err != nil {
		return nil, fmt.Errorf("resource %x: %w", r.ID, err)
	}
	r.cache[k] = buf.Bytes()
	return r.cache[k], nil
}

// MustData is Data that returns nil on error.
func (r *Resource) MustData(k int) []byte { d, _ := r.Data(k); return d }

// Imports lists the resource's import table (16-byte records in chunk 0).
func (r *Resource) Imports() []Import {
	d := r.MustData(0)
	out := make([]Import, 0, r.ImportCount)
	for i := 0; i < int(r.ImportCount); i++ {
		o := int(r.ImportOff) + 16*i
		if o+12 > len(d) {
			break
		}
		out = append(out, Import{ID: le.Uint64(d[o:]), Offset: le.Uint32(d[o+8:])})
	}
	return out
}

// ImportAt returns the resource id imported into chunk-0 offset off.
func (r *Resource) ImportAt(off uint32) (uint64, bool) {
	for _, im := range r.Imports() {
		if im.Offset == off {
			return im.ID, true
		}
	}
	return 0, false
}
