package nif

import "math"

// Shape is one textured triangle mesh in game units.
type Shape struct {
	Name          string
	Pos           [][3]float32
	Normal        [][3]float32
	Tangent       [][3]float32
	Bitan         [][3]float32
	UV            [][2]float32
	Tris          [][3]uint16
	Diffuse       string
	NormalTx      string
	Alpha         float32
	Blend         bool
	AlphaTest     bool
	AlphaRef      uint8
	VertexAlpha   []float32
	Specular      [3]float32
	Gloss         float32
	Emissive      float32
	EmissiveColor [3]float32
	EnvMap        string
	EnvMask       string
}

func bound(p [][3]float32) ([3]float32, float32) {
	var mn, mx [3]float32
	for i := range mn {
		mn[i], mx[i] = math.MaxFloat32, -math.MaxFloat32
	}
	for _, v := range p {
		for k := 0; k < 3; k++ {
			mn[k] = float32(math.Min(float64(mn[k]), float64(v[k])))
			mx[k] = float32(math.Max(float64(mx[k]), float64(v[k])))
		}
	}
	c := [3]float32{(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2, (mn[2] + mx[2]) / 2}
	var r float64
	for _, v := range p {
		d := math.Sqrt(float64((v[0]-c[0])*(v[0]-c[0]) + (v[1]-c[1])*(v[1]-c[1]) + (v[2]-c[2])*(v[2]-c[2])))
		r = math.Max(r, d)
	}
	return c, float32(r)
}

// writeShape adds NiTriShape + data + properties; skin is the skin instance block index or -1.
func (w *Writer) writeShape(shapeIdx int, s *Shape, skinned bool, skin int) {
	dataIdx := w.Reserve("NiTriShapeData")
	ppIdx := w.Reserve("BSShaderPPLightingProperty")
	tsIdx := w.Reserve("BSShaderTextureSet")
	matIdx := w.Reserve("NiMaterialProperty")
	props := []int{ppIdx, matIdx}
	if s.Blend || s.Alpha < 1 || s.AlphaTest {
		aIdx := w.Reserve("NiAlphaProperty")
		props = append(props, aIdx)
		b := &B{}
		w.ObjectNET(b, "", nil)
		ref := uint8(128)
		if s.Blend {
			b.U16(0x00ED)
		} else {
			b.U16(0x12EC)
			if s.AlphaTest {
				ref = s.AlphaRef
			}
		}
		b.U8(ref)
		w.Set(aIdx, b)
	}
	b := &B{}
	w.ObjectNET(b, s.Name, nil)
	w.AVObject(b, 0x8000E, [3]float32{}, props)
	b.Ref(dataIdx)
	b.Ref(skin)
	b.U32(0)
	b.I32(-1)
	b.U8(0)
	w.Set(shapeIdx, b)
	d := &B{}
	d.I32(0)
	d.U16(uint16(len(s.Pos)))
	d.U8(0)
	d.U8(0)
	d.U8(1)
	for _, v := range s.Pos {
		d.Vec(v)
	}
	flags := uint16(0)
	if s.UV != nil {
		flags |= 1
	}
	if s.Tangent != nil {
		flags |= 0x1000
	}
	d.U16(flags)
	d.U8(1)
	for _, v := range s.Normal {
		d.Vec(v)
	}
	if s.Tangent != nil {
		for _, v := range s.Tangent {
			d.Vec(v)
		}
		for _, v := range s.Bitan {
			d.Vec(v)
		}
	}
	c, r := bound(s.Pos)
	d.Vec(c)
	d.F32(r)
	if len(s.VertexAlpha) == len(s.Pos) && len(s.Pos) > 0 {
		d.U8(1)
		for _, a := range s.VertexAlpha {
			d.F32(1)
			d.F32(1)
			d.F32(1)
			d.F32(a)
		}
	} else {
		d.U8(0)
	}
	if s.UV != nil {
		for _, t := range s.UV {
			d.F32(t[0])
			d.F32(t[1])
		}
	}
	d.U16(0x4000)
	d.Ref(-1)
	d.U16(uint16(len(s.Tris)))
	d.U32(uint32(len(s.Tris) * 3))
	d.U8(1)
	for _, t := range s.Tris {
		d.U16(t[0])
		d.U16(t[1])
		d.U16(t[2])
	}
	d.U16(0)
	w.Set(dataIdx, d)
	p := &B{}
	w.ObjectNET(p, "", nil)
	p.U16(1)
	p.U32(1)
	sf := uint32(0x82000001)
	if skinned {
		sf |= 2
	}
	sf2 := uint32(0x1)
	if len(s.VertexAlpha) == len(s.Pos) && len(s.Pos) > 0 {
		sf |= 0x8
		sf2 |= 0x20
	}
	if s.EnvMap != "" {
		sf |= 0x80
		sf2 |= 0x8000
	}
	p.U32(sf)
	p.U32(sf2)
	p.F32(1)
	p.U32(3)
	p.Ref(tsIdx)
	p.F32(0)
	p.I32(0)
	p.F32(4)
	p.F32(1)
	w.Set(ppIdx, p)
	ts := &B{}
	ts.U32(6)
	ts.Sized(s.Diffuse)
	ts.Sized(s.NormalTx)
	ts.Sized("")
	ts.Sized("")
	ts.Sized(s.EnvMap)
	ts.Sized(s.EnvMask)
	w.Set(tsIdx, ts)
	m := &B{}
	w.ObjectNET(m, "", nil)
	m.Vec(s.Specular)
	m.Vec(s.EmissiveColor)
	m.F32(s.Gloss)
	m.F32(s.Alpha)
	m.F32(s.Emissive)
	w.Set(matIdx, m)
}

// Box is an axis-aligned collision box in game units.
type Box struct{ Min, Max [3]float32 }

const havokScale = 1.0 / 7.0

// Collision adds bhkCollisionObject -> bhkRigidBody (fixed, layer STATIC) -> bhkConvexTransformShape -> bhkBoxShape.
func (w *Writer) collision(target int, box Box) int {
	co := w.Reserve("bhkCollisionObject")
	rb := w.Reserve("bhkRigidBody")
	ts := w.Reserve("bhkConvexTransformShape")
	bs := w.Reserve("bhkBoxShape")
	b := &B{}
	b.Ref(target)
	b.U16(1)
	b.Ref(rb)
	w.Set(co, b)
	r := &B{}
	r.Ref(ts)
	r.U8(1)
	r.U8(0)
	r.U16(0)
	r.U32(0)
	r.U8(1)
	r.U8(0)
	r.U8(0)
	r.U8(0)
	r.U32(0)
	r.U32(0)
	r.U32(0x80000000)
	r.U8(1)
	r.U8(0)
	r.U16(0xFFFF)
	r.U32(0)
	r.U8(1)
	r.U8(0)
	r.U16(0)
	r.U32(0)
	r.U8(1)
	r.U8(0)
	r.U16(0xFFFF)
	r.U32(0)
	for i := 0; i < 4; i++ {
		r.F32(0)
	}
	r.F32(0)
	r.F32(0)
	r.F32(0)
	r.F32(1)
	for i := 0; i < 8; i++ {
		r.F32(0)
	}
	for i := 0; i < 12; i++ {
		r.F32(0)
	}
	for i := 0; i < 4; i++ {
		r.F32(0)
	}
	r.F32(0)
	r.F32(0.1)
	r.F32(0.05)
	r.F32(0.5)
	r.F32(0.4)
	r.F32(104.4)
	r.F32(31.57)
	r.F32(0.15)
	r.U8(7)
	r.U8(1)
	r.U8(1)
	r.U8(1)
	for i := 0; i < 12; i++ {
		r.U8(0)
	}
	r.U32(0)
	r.U32(0)
	w.Set(rb, r)
	c := [3]float32{(box.Min[0] + box.Max[0]) / 2 * havokScale, (box.Min[1] + box.Max[1]) / 2 * havokScale, (box.Min[2] + box.Max[2]) / 2 * havokScale}
	t := &B{}
	t.Ref(bs)
	t.U32(5)
	t.F32(0.1)
	for i := 0; i < 8; i++ {
		t.U8(0)
	}
	m := [16]float32{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, c[0], c[1], c[2], 1}
	for _, v := range m {
		t.F32(v)
	}
	w.Set(ts, t)
	x := &B{}
	x.U32(5)
	x.F32(0.1)
	for i := 0; i < 8; i++ {
		x.U8(0)
	}
	x.F32((box.Max[0] - box.Min[0]) / 2 * havokScale)
	x.F32((box.Max[1] - box.Min[1]) / 2 * havokScale)
	x.F32((box.Max[2] - box.Min[2]) / 2 * havokScale)
	x.F32(0)
	w.Set(bs, x)
	return co
}

// WheelNode is one wheel of a driving model: shapes relative to the wheel centre At (game units).
type WheelNode struct {
	Name    string
	At      [3]float32
	Shapes  []*Shape
	Carried []*Shape
}

// LightNode is one light role of a driving model: the lens shapes unlit (shown) and lit (hidden).
type LightNode struct {
	Name    string
	Off, On []*Shape
}

// MovingWheels is the driving model with turnable wheels
type NamedNode struct {
	Name   string
	At     [3]float32
	Shapes []*Shape
	Hidden bool
}

func MovingWheels(body []*Shape, wheels []WheelNode, lights []LightNode, named ...NamedNode) *Writer {
	w := New()
	root := w.Reserve("NiNode")
	bsx := w.Reserve("BSXFlags")
	var kids []int
	for _, s := range body {
		i := w.Reserve("NiTriShape")
		w.writeShape(i, s, false, -1)
		kids = append(kids, i)
	}
	for _, wh := range wheels {
		steer := w.Reserve("NiNode")
		spin := w.Reserve("NiNode")
		var sk []int
		for _, s := range wh.Shapes {
			i := w.Reserve("NiTriShape")
			w.writeShape(i, s, false, -1)
			sk = append(sk, i)
		}
		w.NodeAt(spin, "MMWWheel"+wh.Name, [3]float32{}, sk)
		kids2 := []int{spin}
		for _, s := range wh.Carried {
			i := w.Reserve("NiTriShape")
			w.writeShape(i, s, false, -1)
			kids2 = append(kids2, i)
		}
		w.NodeAt(steer, "MMWSteer"+wh.Name, wh.At, kids2)
		kids = append(kids, steer)
	}
	for _, l := range lights {
		for k, set := range [][]*Shape{l.Off, l.On} {
			n := w.Reserve("NiNode")
			var sk []int
			for _, s := range set {
				i := w.Reserve("NiTriShape")
				w.writeShape(i, s, false, -1)
				sk = append(sk, i)
			}
			if k == 0 {
				w.NodeFlags(n, "MMWLightOff"+l.Name, [3]float32{}, sk, 0x8000E)
			} else {
				w.NodeFlags(n, "MMWLightOn"+l.Name, [3]float32{}, sk, 0x8000F)
			}
			kids = append(kids, n)
		}
	}
	for _, nn := range named {
		n := w.Reserve("NiNode")
		var sk []int
		for _, s := range nn.Shapes {
			i := w.Reserve("NiTriShape")
			w.writeShape(i, s, false, -1)
			sk = append(sk, i)
		}
		if nn.Hidden {
			w.NodeFlags(n, nn.Name, nn.At, sk, 0x8000F)
		} else {
			w.NodeAt(n, nn.Name, nn.At, sk)
		}
		kids = append(kids, n)
	}
	w.NodeC(root, "Scene Root", []int{bsx}, kids, -1)
	b := &B{}
	b.U32(w.Str("BSX"))
	b.U32(0)
	w.Set(bsx, b)
	return w
}

// Group is a model of shapes under a NiNode root with this name.
func Group(name string, shapes []*Shape) *Writer {
	w := New()
	root := w.Reserve("NiNode")
	bsx := w.Reserve("BSXFlags")
	var kids []int
	for _, s := range shapes {
		i := w.Reserve("NiTriShape")
		w.writeShape(i, s, false, -1)
		kids = append(kids, i)
	}
	w.NodeC(root, name, []int{bsx}, kids, -1)
	b := &B{}
	b.U32(w.Str("BSX"))
	b.U32(0)
	w.Set(bsx, b)
	return w
}
