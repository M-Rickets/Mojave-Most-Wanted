// Package crit decodes the Criterion Chameleon resources the mashup needs from Need for Speed Most Wanted (2012,
// 32-bit) and Need for Speed Hot Pursuit Remastered (64-bit)
package crit

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"regexp"
	"strings"

	"mmw/internal/bnd2"
)

var le = binary.LittleEndian

const (
	TypeRaster      = 0x1
	TypeMaterial    = 0x2
	TypeVDesc       = 0x3
	TypeRenderable  = 0x5
	TypeSampler     = 0x7
	TypeShader      = 0x53
	TypeModel       = 0x51
	TypeWave80      = 0x80
	TypeWave81      = 0x81
	TypeVehicleList = 0x105
	TypeVGS         = 0x106
	TypeVehicleSnd  = 0x207
)

// Lookup finds a resource in the car bundle(s) first, then in shared bundles (shaders, shared textures).
type Lookup struct{ Bundles []*bnd2.Bundle }

func (l Lookup) Get(id uint64) (*bnd2.Resource, *bnd2.Bundle) {
	for _, b := range l.Bundles {
		if r, ok := b.ByID[id]; ok {
			return r, b
		}
	}
	return nil, nil
}

func (l Lookup) Name(id uint64) string {
	for _, b := range l.Bundles {
		if n, ok := b.Names[id]; ok {
			return n
		}
	}
	return ""
}

// ---- vertex descriptors ----------

type VElem struct {
	Semantic, Format, Offset, Stream, Stride uint32
}

func VDesc(r *bnd2.Resource) ([]VElem, error) {
	d, err := r.Data(0)
	if err != nil || len(d) < 16 {
		return nil, fmt.Errorf("vdesc %x unreadable", r.ID)
	}
	n := int(le.Uint32(d[12:]))
	var out []VElem
	for i := 0; i < n && 0x10+i*20+20 <= len(d); i++ {
		o := 0x10 + i*20
		out = append(out, VElem{le.Uint32(d[o:]), le.Uint32(d[o+4:]), le.Uint32(d[o+8:]), le.Uint32(d[o+12:]), le.Uint32(d[o+16:])})
	}
	return out, nil
}

// ---- shaders ----------

type Shader struct {
	ID        uint64
	Technique string
	Strings   []string
	Samplers  []string
	VDesc     []VElem
}

var reIdent = regexp.MustCompile(`[A-Za-z_][A-Za-z0-9_]{3,}\x00`)

func ReadShader(l Lookup, id uint64, ptr64 bool) (*Shader, error) {
	r, _ := l.Get(id)
	if r == nil {
		return nil, fmt.Errorf("shader %x not found", id)
	}
	d, err := r.Data(0)
	if err != nil {
		return nil, err
	}
	s := &Shader{ID: id}
	for _, m := range reIdent.FindAll(d, -1) {
		str := string(m[:len(m)-1])
		s.Strings = append(s.Strings, str)
		if strings.HasPrefix(str, "Vehicle") && strings.Count(str, "_") >= 1 && s.Technique == "" {
			s.Technique = str
		}
	}
	for _, str := range s.Strings {
		if strings.HasPrefix(str, "VehicleNFS") || strings.HasPrefix(str, "Vehicle_") {
			s.Technique = str
		}
	}
	ims := r.Imports()
	for _, im := range ims {
		if vr, _ := l.Get(im.ID); vr != nil && vr.Type == TypeVDesc {
			s.VDesc, _ = VDesc(vr)
			break
		}
	}
	if !ptr64 && len(d) > 0x70 {
		n := int(le.Uint32(d[0x5c:]))
		p := int(le.Uint32(d[0x68:]))
		if n > 0 && n < 32 && p > 0 && p+4*n <= len(d) {
			for i := 0; i < n; i++ {
				so := int(le.Uint32(d[p+4*i:]))
				if so <= 0 || so >= len(d) {
					s.Samplers = nil
					break
				}
				e := bytes.IndexByte(d[so:], 0)
				if e < 0 {
					s.Samplers = nil
					break
				}
				s.Samplers = append(s.Samplers, string(d[so:so+e]))
			}
		}
	}
	return s, nil
}

func (s *Shader) Has(sub string) bool {
	for _, x := range s.Strings {
		if strings.Contains(x, sub) {
			return true
		}
	}
	return false
}

// ---- materials ----------

type TexRef struct {
	ID   uint64
	Role string
}

type Material struct {
	ID       uint64
	Name     string
	Shader   *Shader
	Textures []TexRef
	Kind     string
	Params   map[uint32][4]float32
}

// Shader constant name hashes (MW 2012 materials).
const (
	ParamDiffuseColour  = 0x067923b3
	ParamSpecularColour = 0x58ed4287
)

// readParams reads a MW 2012 material's constants
func readParams(d []byte) map[uint32][4]float32 {
	if len(d) < 0x20 {
		return nil
	}
	permOff, sizeOff, hashOff, valOff := int(le.Uint32(d[0x0c:])), int(le.Uint32(d[0x10:])), int(le.Uint32(d[0x14:])), int(le.Uint32(d[0x18:]))
	n := int(d[0x1c])
	if n == 0 || n > 32 || permOff+n > len(d) || sizeOff+n > len(d) || hashOff+4*n > len(d) {
		return nil
	}
	start := make([]int, n)
	acc := 0
	for k := 0; k < n; k++ {
		start[k] = valOff + 16*acc
		sz := int(d[sizeOff+k])
		if sz == 0 {
			sz = 1
		}
		acc += sz
	}
	out := map[uint32][4]float32{}
	for i := 0; i < n; i++ {
		slot := int(d[permOff+i])
		if slot >= n {
			return nil
		}
		o := start[slot]
		if o+16 > len(d) {
			return nil
		}
		var v [4]float32
		for c := 0; c < 4; c++ {
			v[c] = math.Float32frombits(le.Uint32(d[o+4*c:]))
		}
		out[le.Uint32(d[hashOff+4*i:])] = v
	}
	return out
}

func roleFromName(n string) string {
	n = strings.ToLower(n)
	switch {
	case strings.Contains(n, "blur"), strings.Contains(n, "crumple"), strings.Contains(n, "crack"):
		return "other"
	case strings.Contains(n, "diffuse"), strings.Contains(n, "colour"), strings.Contains(n, "color"):
		return "diffuse"
	case strings.Contains(n, "normal"):
		return "normal"
	case strings.Contains(n, "specao"), strings.Contains(n, "specular"):
		return "specular"
	case strings.Contains(n, ".ao"), strings.Contains(n, "ambientocclusion"):
		return "ao"
	case strings.Contains(n, "emissive"), strings.Contains(n, "lightmap"):
		return "emissive"
	case strings.Contains(n, "effects"):
		return "effects"
	}
	return "other"
}

func ReadMaterial(l Lookup, id uint64, ptr64 bool) (*Material, error) {
	r, b := l.Get(id)
	if r == nil {
		return nil, fmt.Errorf("material %x not found", id)
	}
	m := &Material{ID: id, Name: b.Names[id]}
	if !ptr64 {
		if d0, err := r.Data(0); err == nil {
			m.Params = readParams(d0)
		}
	}
	ims := r.Imports()
	if len(ims) == 0 {
		return nil, fmt.Errorf("material %x has no imports", id)
	}
	var texIDs []uint64
	for _, im := range ims {
		if im.Offset == 8 {
			sh, err := ReadShader(l, im.ID, ptr64)
			if err == nil {
				m.Shader = sh
			}
			continue
		}
		tr, _ := l.Get(im.ID)
		if tr != nil && tr.Type != TypeRaster {
			continue
		}
		if tr == nil && strings.HasPrefix(l.Name(im.ID), "SAMPLERSTATE") {
			continue
		}
		if tr == nil && (im.ID>>32) == 0 && l.Name(im.ID) == "" {
			continue
		}
		texIDs = append(texIDs, im.ID)
	}
	for i, tid := range texIDs {
		role := "other"
		if n := l.Name(tid); n != "" {
			role = roleFromName(n)
		} else if m.Shader != nil && i < len(m.Shader.Samplers) {
			role = roleFromName(m.Shader.Samplers[i])
		}
		m.Textures = append(m.Textures, TexRef{ID: tid, Role: role})
	}
	tech := ""
	if m.Shader != nil {
		tech = strings.ToLower(m.Shader.Technique)
	}
	lname := strings.ToLower(m.Name)
	switch {
	case strings.Contains(tech, "glass") || strings.Contains(lname, "window") || strings.Contains(lname, "glass"):
		m.Kind = "glass"
	case strings.Contains(tech, "bodypaint") || strings.Contains(lname, "paint_colour") || (m.Shader != nil && m.Shader.Has("PbrPaintClearcoat")):
		m.Kind = "paint"
	case strings.Contains(lname, "light") || strings.Contains(tech, "light"):
		m.Kind = "light"
	default:
		m.Kind = "plain"
	}
	if m.Kind != "glass" && m.Kind != "paint" {
		for _, t := range m.Textures {
			if t.Role == "diffuse" {
				m.Kind = "textured"
			}
		}
	}
	return m, nil
}

func (m *Material) Tex(role string) (uint64, bool) {
	for _, t := range m.Textures {
		if t.Role == role {
			return t.ID, true
		}
	}
	return 0, false
}

// ---- renderables ----------

type Mesh struct {
	Prim, BaseVertex, StartIndex, NumIndices uint32
	MaterialID                               uint64
	IBOff, IBSize, IBStride                  uint32
	VBOff, VBSize                            uint32
}

type Renderable struct {
	Center [3]float32
	Radius float32
	Meshes []Mesh
	Gfx    []byte
}

func ReadRenderable(r *bnd2.Resource, ptr64 bool) (*Renderable, error) {
	d0, err := r.Data(0)
	if err != nil {
		return nil, err
	}
	d1, err := r.Data(1)
	if err != nil {
		return nil, err
	}
	R := &Renderable{Gfx: d1}
	for i := 0; i < 3; i++ {
		R.Center[i] = math.Float32frombits(le.Uint32(d0[4*i:]))
	}
	R.Radius = math.Float32frombits(le.Uint32(d0[12:]))
	n := int(le.Uint16(d0[0x12:]))
	ptr := func(o int) int {
		if ptr64 {
			return int(le.Uint64(d0[o:]))
		}
		return int(le.Uint32(d0[o:]))
	}
	P := 4
	marr := ptr(0x14)
	if ptr64 {
		P = 8
		marr = ptr(0x18)
	}
	imps := map[uint32]uint64{}
	for _, im := range r.Imports() {
		imps[im.Offset] = im.ID
	}
	for i := 0; i < n; i++ {
		mo := ptr(marr + i*P)
		if mo <= 0 || mo+0x40 > len(d0) {
			return nil, fmt.Errorf("renderable %x: bad mesh pointer", r.ID)
		}
		m := Mesh{Prim: le.Uint32(d0[mo+0x10:]), BaseVertex: le.Uint32(d0[mo+0x14:]), StartIndex: le.Uint32(d0[mo+0x18:]), NumIndices: le.Uint32(d0[mo+0x1C:])}
		m.MaterialID = imps[uint32(mo+0x20)]
		if ptr64 {
			ib, vb := ptr(mo+0x30), ptr(mo+0x38)
			m.IBOff, m.IBSize, m.IBStride = uint32(le.Uint64(d0[ib+0x10:])), le.Uint32(d0[ib+0x18:]), le.Uint32(d0[ib+0x1C:])
			m.VBOff, m.VBSize = uint32(le.Uint64(d0[vb+0x10:])), le.Uint32(d0[vb+0x18:])
		} else {
			ib, vb := int(le.Uint32(d0[mo+0x28:])), int(le.Uint32(d0[mo+0x2C:]))
			m.IBOff, m.IBSize, m.IBStride = le.Uint32(d0[ib+0xC:]), le.Uint32(d0[ib+0x10:]), le.Uint32(d0[ib+0x14:])
			m.VBOff, m.VBSize = le.Uint32(d0[vb+0xC:]), le.Uint32(d0[vb+0x10:])
		}
		if int(m.IBOff+m.IBSize) > len(d1) || int(m.VBOff+m.VBSize) > len(d1) || m.IBStride == 0 {
			return nil, fmt.Errorf("renderable %x: buffers out of range", r.ID)
		}
		R.Meshes = append(R.Meshes, m)
	}
	return R, nil
}

// Triangles converts a mesh's index buffer (strip with primitive restart, or list) to triangles.
func (R *Renderable) Triangles(m Mesh) [][3]uint32 {
	n := int(m.IBSize / m.IBStride)
	get := func(i int) uint32 {
		o := int(m.IBOff) + i*int(m.IBStride)
		if m.IBStride == 2 {
			return uint32(le.Uint16(R.Gfx[o:]))
		}
		return le.Uint32(R.Gfx[o:])
	}
	s, e := int(m.StartIndex), int(m.StartIndex+m.NumIndices)
	if e > n {
		e = n
	}
	restart := uint32(0xFFFF)
	if m.IBStride == 4 {
		restart = 0xFFFFFFFF
	}
	var tris [][3]uint32
	if m.Prim == 4 {
		for i := s; i+2 < e; i += 3 {
			tris = append(tris, [3]uint32{get(i) + m.BaseVertex, get(i+1) + m.BaseVertex, get(i+2) + m.BaseVertex})
		}
		return tris
	}
	var cur []uint32
	flush := func() {
		for i := 0; i+2 < len(cur); i++ {
			a, b, c := cur[i], cur[i+1], cur[i+2]
			if a == b || b == c || a == c {
				continue
			}
			if i%2 == 0 {
				tris = append(tris, [3]uint32{a, b, c})
			} else {
				tris = append(tris, [3]uint32{b, a, c})
			}
		}
		cur = cur[:0]
	}
	for i := s; i < e; i++ {
		v := get(i)
		if v == restart {
			flush()
			continue
		}
		cur = append(cur, v+m.BaseVertex)
	}
	flush()
	return tris
}

// ---- models ----------

// ModelLOD0 returns the most detailed renderable of a Model (the import at the lowest offset).
func ModelLOD0(l Lookup, model *bnd2.Resource) (*bnd2.Resource, error) {
	ims := model.Imports()
	var best *bnd2.Import
	for i := range ims {
		r, _ := l.Get(ims[i].ID)
		if r == nil || r.Type != TypeRenderable {
			continue
		}
		if best == nil || ims[i].Offset < best.Offset {
			best = &ims[i]
		}
	}
	if best == nil {
		return nil, fmt.Errorf("model %x has no renderable", model.ID)
	}
	r, _ := l.Get(best.ID)
	return r, nil
}

// ---- Vehicle graphics spec

type Wheel struct {
	Name   string
	Pos    [3]float32
	Models []*bnd2.Resource
}

type VehicleSpec struct {
	Body   *bnd2.Resource
	Wheels []Wheel
}

func ReadVehicleSpec(l Lookup, b *bnd2.Bundle) (*VehicleSpec, error) {
	var spec *bnd2.Resource
	for _, r := range b.Res {
		if r.Type == TypeVGS {
			spec = r
			break
		}
	}
	if spec == nil {
		return nil, fmt.Errorf("no VehicleGraphicsSpec in %s", b.Path)
	}
	d, err := spec.Data(0)
	if err != nil {
		return nil, err
	}
	ims := spec.Imports()
	impAt := map[uint32]uint64{}
	for _, im := range ims {
		impAt[im.Offset] = im.ID
	}
	vs := &VehicleSpec{}
	var bodyOff uint32 = math.MaxUint32
	for _, im := range ims {
		if im.Offset&0x80000000 != 0 {
			continue
		}
		if r, _ := l.Get(im.ID); r != nil && r.Type == TypeModel && im.Offset < bodyOff {
			bodyOff, vs.Body = im.Offset, r
		}
	}
	if vs.Body == nil {
		return nil, fmt.Errorf("no body model in spec")
	}
	for _, name := range []string{"FrontLeft", "FrontRight", "RearLeft", "RearRight"} {
		i := bytes.Index(d, []byte(name+"\x00"))
		if i < 0 {
			continue
		}
		w := Wheel{Name: name}
		var mptr uint32
		cnt := 1
		if b.Ptr64 {
			line := i - i%16
			for k := 0; k < 3; k++ {
				w.Pos[k] = math.Float32frombits(le.Uint32(d[line+0x20+4*k:]))
			}
			mptr = uint32(le.Uint64(d[line-0x10:]))
			cnt = int(le.Uint32(d[line-0x8:]))
			if cnt < 1 || cnt > 16 {
				cnt = 1
			}
		} else {
			for k := 0; k < 3; k++ {
				w.Pos[k] = math.Float32frombits(le.Uint32(d[i-2-0x80+4*k:]))
			}
			mptr = le.Uint32(d[i-2-8:])
			cnt = int(le.Uint16(d[i-2:]))
		}
		step := uint32(4)
		if b.Ptr64 {
			step = 8
		}
		for k := 0; k < cnt; k++ {
			if id, ok := impAt[mptr+uint32(k)*step]; ok {
				if r, _ := l.Get(id); r != nil && r.Type == TypeModel {
					w.Models = append(w.Models, r)
				}
			}
		}
		vs.Wheels = append(vs.Wheels, w)
	}
	return vs, nil
}

// ---- rasters ----------

type Raster struct {
	Format        uint32
	Width, Height int
	Mips          int
	Pixels        []byte
}

func ReadRaster(r *bnd2.Resource, ptr64 bool) (*Raster, error) {
	h, err := r.Data(0)
	if err != nil {
		return nil, err
	}
	px, err := r.Data(1)
	if err != nil {
		return nil, err
	}
	ra := &Raster{Pixels: px}
	if ptr64 {
		if len(h) < 0x40 {
			return nil, fmt.Errorf("raster header too short")
		}
		ra.Format = le.Uint32(h[0x2C:])
		ra.Width, ra.Height = int(le.Uint16(h[0x34:])), int(le.Uint16(h[0x36:]))
		ra.Mips = int(h[0x3D])
	} else {
		if len(h) < 0x2E {
			return nil, fmt.Errorf("raster header too short")
		}
		ra.Format = le.Uint32(h[0x1C:])
		ra.Width, ra.Height = int(le.Uint16(h[0x24:])), int(le.Uint16(h[0x26:]))
		ra.Mips = int(h[0x2D])
	}
	if ra.Width == 0 || ra.Height == 0 {
		return nil, fmt.Errorf("raster %x: empty", r.ID)
	}
	if ra.Mips == 0 {
		ra.Mips = 1
	}
	return ra, nil
}
