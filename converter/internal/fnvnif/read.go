// Package fnvnif reads the Fallout 3 / New Vegas models the launcher builds drivable vehicles from
package fnvnif

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"strings"
)

type File struct {
	Version, User, BS uint32
	Types             []string
	Strings           []string
	blocks            [][]byte
	Roots             []int
}

type rd struct {
	b   []byte
	at  int
	err error
}

func (r *rd) need(n int) bool {
	if r.err != nil {
		return false
	}
	if n < 0 || r.at+n > len(r.b) {
		r.err = fmt.Errorf("short read at %d (+%d of %d)", r.at, n, len(r.b))
		return false
	}
	return true
}
func (r *rd) u8() uint8 {
	if !r.need(1) {
		return 0
	}
	v := r.b[r.at]
	r.at++
	return v
}
func (r *rd) u16() uint16 {
	if !r.need(2) {
		return 0
	}
	v := binary.LittleEndian.Uint16(r.b[r.at:])
	r.at += 2
	return v
}
func (r *rd) u32() uint32 {
	if !r.need(4) {
		return 0
	}
	v := binary.LittleEndian.Uint32(r.b[r.at:])
	r.at += 4
	return v
}
func (r *rd) i32() int32   { return int32(r.u32()) }
func (r *rd) f32() float32 { return math.Float32frombits(r.u32()) }
func (r *rd) v3() [3]float32 {
	return [3]float32{r.f32(), r.f32(), r.f32()}
}
func (r *rd) skip(n int) {
	if r.need(n) {
		r.at += n
	}
}
func (r *rd) sized() string {
	n := int(r.u32())
	if n > 1<<20 || !r.need(n) {
		r.err = errors.New("bad string")
		return ""
	}
	s := string(r.b[r.at : r.at+n])
	r.at += n
	return s
}
func (r *rd) short() string {
	n := int(r.u8())
	if !r.need(n) {
		return ""
	}
	s := string(r.b[r.at : r.at+n])
	r.at += n
	return strings.TrimRight(s, "\x00")
}

// Parse reads a model's header and splits it into blocks.
func Parse(b []byte) (*File, error) {
	nl := bytes.IndexByte(b, '\n')
	if nl < 0 || !bytes.HasPrefix(b, []byte("Gamebryo File Format")) {
		return nil, errors.New("not a NIF")
	}
	r := &rd{b: b, at: nl + 1}
	f := &File{}
	f.Version = r.u32()
	r.u8()
	f.User = r.u32()
	nb := int(r.u32())
	if f.Version != 0x14020007 || f.User != 11 {
		return nil, fmt.Errorf("version %x user %d: not a New Vegas model", f.Version, f.User)
	}
	f.BS = r.u32()
	r.short()
	r.short()
	r.short()
	nt := int(r.u16())
	types := make([]string, nt)
	for i := range types {
		types[i] = r.sized()
	}
	f.Types = make([]string, nb)
	for i := 0; i < nb; i++ {
		t := int(r.u16() & 0x7FFF)
		if t < nt {
			f.Types[i] = types[t]
		}
	}
	sizes := make([]int, nb)
	for i := range sizes {
		sizes[i] = int(r.u32())
	}
	ns := int(r.u32())
	r.u32()
	if ns > 1<<16 {
		return nil, errors.New("bad string table")
	}
	f.Strings = make([]string, ns)
	for i := range f.Strings {
		f.Strings[i] = r.sized()
	}
	ng := int(r.u32())
	r.skip(4 * ng)
	if r.err != nil {
		return nil, r.err
	}
	f.blocks = make([][]byte, nb)
	for i := 0; i < nb; i++ {
		if !r.need(sizes[i]) {
			return nil, r.err
		}
		f.blocks[i] = b[r.at : r.at+sizes[i]]
		r.at += sizes[i]
	}
	nr := int(r.u32())
	for i := 0; i < nr && r.err == nil; i++ {
		f.Roots = append(f.Roots, int(r.i32()))
	}
	if len(f.Roots) == 0 && nb > 0 {
		f.Roots = []int{0}
	}
	return f, nil
}

func (f *File) str(i int32) string {
	if i < 0 || int(i) >= len(f.Strings) {
		return ""
	}
	return f.Strings[i]
}

// Xf is a transform: rotation (row major), translation, uniform scale.
type Xf struct {
	R [9]float32
	T [3]float32
	S float32
}

func Identity() Xf { return Xf{R: [9]float32{1, 0, 0, 0, 1, 0, 0, 0, 1}, S: 1} }
func (x Xf) Apply(p [3]float32) [3]float32 {
	return [3]float32{
		(x.R[0]*p[0]+x.R[1]*p[1]+x.R[2]*p[2])*x.S + x.T[0],
		(x.R[3]*p[0]+x.R[4]*p[1]+x.R[5]*p[2])*x.S + x.T[1],
		(x.R[6]*p[0]+x.R[7]*p[1]+x.R[8]*p[2])*x.S + x.T[2],
	}
}
func (x Xf) Turn(p [3]float32) [3]float32 {
	return [3]float32{x.R[0]*p[0] + x.R[1]*p[1] + x.R[2]*p[2], x.R[3]*p[0] + x.R[4]*p[1] + x.R[5]*p[2], x.R[6]*p[0] + x.R[7]*p[1] + x.R[8]*p[2]}
}

// Mul: the parent's transform then the child's (child local in the parent's space)
func Mul(p, c Xf) Xf {
	var o Xf
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			o.R[i*3+j] = p.R[i*3]*c.R[j] + p.R[i*3+1]*c.R[3+j] + p.R[i*3+2]*c.R[6+j]
		}
	}
	o.T = p.Apply(c.T)
	o.S = p.S * c.S
	return o
}

// av: a NiAVObject's start (NiObjectNET, then flags, transform, properties, collision)
type av struct {
	name     string
	flags    uint32
	xf       Xf
	props    []int
	children []int
}

func (f *File) readAV(r *rd) av {
	var a av
	a.name = f.str(r.i32())
	ne := int(r.u32())
	r.skip(4 * ne)
	r.i32()
	a.flags = uint32(r.u16())
	if f.BS > 26 {
		a.flags |= uint32(r.u16()) << 16
	}
	a.xf.T = r.v3()
	for i := 0; i < 9; i++ {
		a.xf.R[i] = r.f32()
	}
	a.xf.S = r.f32()
	np := int(r.u32())
	if np > 64 {
		r.err = errors.New("bad property count")
		return a
	}
	for i := 0; i < np; i++ {
		a.props = append(a.props, int(r.i32()))
	}
	r.i32()
	return a
}

// Mesh is one triangle mesh of a model, in the model's space.
type Mesh struct {
	Name                                      string
	Path                                      []string
	Hidden                                    bool
	Pos                                       [][3]float32
	Normal                                    [][3]float32
	Tangent                                   [][3]float32
	Bitan                                     [][3]float32
	UV                                        [][2]float32
	Color                                     [][4]float32
	Tris                                      [][3]uint16
	Diffuse, NormalMap, Glow, EnvMap, EnvMask string
	ShaderFlags, ShaderFlags2                 uint32
	Alpha                                     bool
	AlphaFlags                                uint16
	AlphaRef                                  uint8
	EmissiveColor                             [4]float32
}

// Meshes walks the model from its roots and returns every triangle mesh in model space (damage stages and blast nodes'
// meshes included: the caller picks).
func (f *File) Meshes() ([]*Mesh, error) {
	var out []*Mesh
	seen := map[int]bool{}
	var walk func(i int, parent Xf, path []string, hidden bool) error
	walk = func(i int, parent Xf, path []string, hidden bool) error {
		if i < 0 || i >= len(f.blocks) || seen[i] {
			return nil
		}
		seen[i] = true
		t := f.Types[i]
		r := &rd{b: f.blocks[i]}
		switch {
		case t == "NiTriStrips" || t == "NiTriShape" || t == "BSSegmentedTriShape":
			a := f.readAV(r)
			data := int(r.i32())
			if r.err != nil {
				return fmt.Errorf("block %d %s: %v", i, t, r.err)
			}
			m, err := f.mesh(data)
			if err != nil {
				return fmt.Errorf("block %d %s (%s): %v", i, t, a.name, err)
			}
			if m == nil {
				return nil
			}
			x := Mul(parent, a.xf)
			for k := range m.Pos {
				m.Pos[k] = x.Apply(m.Pos[k])
			}
			for _, arr := range [][][3]float32{m.Normal, m.Tangent, m.Bitan} {
				for k := range arr {
					arr[k] = x.Turn(arr[k])
				}
			}
			m.Name = a.name
			m.Path = append([]string{}, path...)
			m.Hidden = hidden || a.flags&1 != 0
			f.material(m, a.props)
			out = append(out, m)
		case strings.HasSuffix(t, "Node") || t == "BSFadeNode" || t == "BSBlastNode" || t == "BSDamageStage" || t == "BSMultiBoundNode" || t == "BSOrderedNode" || t == "NiSwitchNode" || t == "NiBillboardNode" || t == "BSValueNode":
			a := f.readAV(r)
			nc := int(r.u32())
			if nc > 4096 || r.err != nil {
				return fmt.Errorf("block %d %s: bad children (%v)", i, t, r.err)
			}
			kids := make([]int, nc)
			for k := range kids {
				kids[k] = int(r.i32())
			}
			x := Mul(parent, a.xf)
			p2 := append(append([]string{}, path...), a.name)
			for _, k := range kids {
				if err := walk(k, x, p2, hidden || a.flags&1 != 0); err != nil {
					return err
				}
			}
		}
		return nil
	}
	for _, r := range f.Roots {
		if err := walk(r, Identity(), nil, false); err != nil {
			return out, err
		}
	}
	return out, nil
}

func (f *File) mesh(i int) (*Mesh, error) {
	if i < 0 || i >= len(f.blocks) {
		return nil, nil
	}
	t := f.Types[i]
	if t != "NiTriStripsData" && t != "NiTriShapeData" {
		return nil, nil
	}
	r := &rd{b: f.blocks[i]}
	m := &Mesh{}
	r.i32()
	nv := int(r.u16())
	r.u8()
	r.u8()
	if r.u8() != 0 {
		m.Pos = make([][3]float32, nv)
		for k := range m.Pos {
			m.Pos[k] = r.v3()
		}
	}
	nuv := int(r.u8())
	ev := r.u8()
	if r.u8() != 0 {
		m.Normal = make([][3]float32, nv)
		for k := range m.Normal {
			m.Normal[k] = r.v3()
		}
		if ev&16 != 0 {
			m.Tangent = make([][3]float32, nv)
			for k := range m.Tangent {
				m.Tangent[k] = r.v3()
			}
			m.Bitan = make([][3]float32, nv)
			for k := range m.Bitan {
				m.Bitan[k] = r.v3()
			}
		}
	}
	r.v3()
	r.f32()
	if r.u8() != 0 {
		m.Color = make([][4]float32, nv)
		for k := range m.Color {
			m.Color[k] = [4]float32{r.f32(), r.f32(), r.f32(), r.f32()}
		}
	}
	for s := 0; s < nuv&63; s++ {
		uv := make([][2]float32, nv)
		for k := range uv {
			uv[k] = [2]float32{r.f32(), r.f32()}
		}
		if s == 0 {
			m.UV = uv
		}
	}
	r.u16()
	r.i32()
	ntri := int(r.u16())
	if t == "NiTriStripsData" {
		ns := int(r.u16())
		lens := make([]int, ns)
		for k := range lens {
			lens[k] = int(r.u16())
		}
		if r.u8() != 0 {
			for _, l := range lens {
				pts := make([]uint16, l)
				for k := range pts {
					pts[k] = r.u16()
				}
				for k := 0; k+2 < l; k++ {
					a, b, c := pts[k], pts[k+1], pts[k+2]
					if a == b || b == c || a == c {
						continue
					}
					if k&1 == 0 {
						m.Tris = append(m.Tris, [3]uint16{a, b, c})
					} else {
						m.Tris = append(m.Tris, [3]uint16{a, c, b})
					}
				}
			}
		}
	} else {
		r.u32()
		if r.u8() != 0 {
			m.Tris = make([][3]uint16, ntri)
			for k := range m.Tris {
				m.Tris[k] = [3]uint16{r.u16(), r.u16(), r.u16()}
			}
		}
	}
	if r.err != nil {
		return nil, r.err
	}
	for _, t := range m.Tris {
		for _, v := range t {
			if int(v) >= nv {
				return nil, fmt.Errorf("a triangle points past the %d vertices", nv)
			}
		}
	}
	return m, nil
}

func (f *File) material(m *Mesh, props []int) {
	for _, p := range props {
		if p < 0 || p >= len(f.blocks) {
			continue
		}
		r := &rd{b: f.blocks[p]}
		switch f.Types[p] {
		case "BSShaderPPLightingProperty":
			r.i32()
			ne := int(r.u32())
			r.skip(4 * ne)
			r.i32()
			r.u16()
			r.u32()
			m.ShaderFlags = r.u32()
			m.ShaderFlags2 = r.u32()
			r.f32()
			r.u32()
			ts := int(r.i32())
			if f.BS > 14 {
				r.f32()
				r.i32()
			}
			if f.BS > 24 {
				r.f32()
				r.f32()
			}
			m.EmissiveColor = [4]float32{r.f32(), r.f32(), r.f32(), r.f32()}
			if ts >= 0 && ts < len(f.blocks) && f.Types[ts] == "BSShaderTextureSet" {
				tr := &rd{b: f.blocks[ts]}
				n := int(tr.i32())
				for k := 0; k < n && k < 8 && tr.err == nil; k++ {
					s := tr.sized()
					switch k {
					case 0:
						m.Diffuse = s
					case 1:
						m.NormalMap = s
					case 2:
						m.Glow = s
					case 4:
						m.EnvMap = s
					case 5:
						m.EnvMask = s
					}
				}
			}
		case "NiAlphaProperty":
			r.i32()
			ne := int(r.u32())
			r.skip(4 * ne)
			r.i32()
			m.Alpha = true
			m.AlphaFlags = r.u16()
			m.AlphaRef = r.u8()
		}
	}
}
