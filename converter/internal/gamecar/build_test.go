package gamecar

import (
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"mmw/internal/fnvnif"
)

// A made-up truck: a box body over four tyres (cylinders across y... the model's forward is x, as some of the game's
// are), each tyre its own piece in one mesh with the body
func fakeTruck() []*fnvnif.Mesh {
	m := &fnvnif.Mesh{Name: "Truck:0", Diffuse: `textures\x.dds`}
	add := func(p [3]float32) uint16 {
		m.Pos = append(m.Pos, p)
		m.Normal = append(m.Normal, [3]float32{0, 0, 1})
		m.UV = append(m.UV, [2]float32{})
		return uint16(len(m.Pos) - 1)
	}
	box := func(mn, mx [3]float32) {
		var v [8]uint16
		for i := 0; i < 8; i++ {
			v[i] = add([3]float32{[]float32{mn[0], mx[0]}[i&1], []float32{mn[1], mx[1]}[i>>1&1], []float32{mn[2], mx[2]}[i>>2&1]})
		}
		for _, q := range [][4]int{{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}} {
			m.Tris = append(m.Tris, [3]uint16{v[q[0]], v[q[1]], v[q[2]]}, [3]uint16{v[q[0]], v[q[2]], v[q[3]]})
		}
	}
	tyre := func(cx, cy, r, w float32) {
		const n = 16
		var a, b [n]uint16
		for i := 0; i < n; i++ {
			t := float64(i) * 2 * math.Pi / n
			x, z := cx+r*float32(math.Cos(t)), r+r*float32(math.Sin(t))
			a[i] = add([3]float32{x, cy - w/2, z})
			b[i] = add([3]float32{x, cy + w/2, z})
		}
		for i := 0; i < n; i++ {
			j := (i + 1) % n
			m.Tris = append(m.Tris, [3]uint16{a[i], a[j], b[j]}, [3]uint16{a[i], b[j], b[i]})
		}
	}
	box([3]float32{-200, -80, 30}, [3]float32{200, 80, 140})
	tyre(130, -75, 22, 18)
	tyre(130, 75, 22, 18)
	tyre(-130, -75, 22, 18)
	tyre(-130, 75, 22, 18)
	tyre(-175, -75, 22, 18)
	tyre(-175, 75, 22, 18)
	return []*fnvnif.Mesh{m}
}

func TestFakeTruck(t *testing.T) {
	ms := fakeTruck()
	l := Find(ms, false)
	if len(l.Wheels) != 6 {
		t.Fatalf("%d wheels found, want 6", len(l.Wheels))
	}
	w, b, err := Build(ms, l, false)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("cab %v eye %v hood %v wheels %v half track %.1f half base %.1f front %.1f rear %.1f clearance %.1f radius F %.2f m R %.2f m", b.Cab, b.Eye, b.Hood, b.Wheels, b.HalfTrack, b.HalfBase, b.Front, b.Rear, b.Clearance, b.WheelRadiusF, b.WheelRadiusR)
	if b.HalfTrack < 70 || b.HalfTrack > 80 || b.Clearance < 25 || b.Clearance > 35 {
		t.Errorf("odd measures: %+v", b)
	}
	out := filepath.Join(t.TempDir(), "drive.nif")
	if err := w.Save(out); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(out)
	f, err := fnvnif.Parse(data)
	if err != nil {
		t.Fatal(err)
	}
	back, err := f.Meshes()
	if err != nil || len(back) != 7 {
		t.Fatalf("read back %d meshes (%v)", len(back), err)
	}
	if p := os.Getenv("MMW_KEEP"); p != "" {
		os.WriteFile(p, data, 0o644)
	}
}

// As the game's NV_Truck: the tyres are part of the body's piece (here joined to it by a triangle each) and only a thin
// hub disc per wheel is a piece of its own - the tyres still come off with their wheels
func TestWeldedTyres(t *testing.T) {
	m := &fnvnif.Mesh{Name: "Truck07:0"}
	add := func(p [3]float32) uint16 {
		m.Pos = append(m.Pos, p)
		m.Normal = append(m.Normal, [3]float32{0, 0, 1})
		m.UV = append(m.UV, [2]float32{})
		return uint16(len(m.Pos) - 1)
	}
	b0, b1, b2, b3 := add([3]float32{-230, -90, 10}), add([3]float32{160, -90, 10}), add([3]float32{160, 90, 10}), add([3]float32{-230, 90, 10})
	m.Tris = append(m.Tris, [3]uint16{b0, b1, b2}, [3]uint16{b0, b2, b3})
	bodyTris := 2
	for _, c := range [][2]float32{{90, -82}, {90, 82}, {-178, -82}, {-178, 82}} {
		const n = 16
		r, w := float32(20.5), float32(14)
		out := float32(1)
		if c[1] < 0 {
			out = -1
		}
		var a, b [n]uint16
		for i := 0; i < n; i++ {
			th := float64(i) * 2 * math.Pi / n
			x, z := c[0]+r*float32(math.Cos(th)), -29+r*float32(math.Sin(th))
			a[i] = add([3]float32{x, c[1] - out*w, z})
			b[i] = add([3]float32{x, c[1], z})
		}
		for i := 0; i < n; i++ {
			j := (i + 1) % n
			m.Tris = append(m.Tris, [3]uint16{a[i], a[j], b[j]}, [3]uint16{a[i], b[j], b[i]})
		}
		m.Tris = append(m.Tris, [3]uint16{a[0], b0, b1})
		bodyTris++
		ctr := add([3]float32{c[0], c[1] + out*0.5, -29})
		var d [n]uint16
		for i := 0; i < n; i++ {
			th := float64(i) * 2 * math.Pi / n
			d[i] = add([3]float32{c[0] + r*float32(math.Cos(th)), c[1] + out*0.5, -29 + r*float32(math.Sin(th))})
		}
		for i := 0; i < n; i++ {
			m.Tris = append(m.Tris, [3]uint16{ctr, d[i], d[(i+1)%n]})
		}
	}
	ms := []*fnvnif.Mesh{m}
	l := Find(ms, false)
	if len(l.Wheels) != 4 {
		t.Fatalf("%d wheels, want 4", len(l.Wheels))
	}
	_, b, err := Build(ms, l, false)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("wheels %v body tris %d wheel tris %d", b.Wheels, b.BodyTris, b.WheelTris)
	if b.BodyTris != bodyTris {
		t.Errorf("body kept %d triangles, want %d (the tyres should all have come off)", b.BodyTris, bodyTris)
	}
}

// A hub disc smaller than the tyre welded round it: the wheel grows to the tyre
func TestTyreRoundHub(t *testing.T) {
	m := &fnvnif.Mesh{Name: "Truck07:0"}
	add := func(p [3]float32) uint16 {
		m.Pos = append(m.Pos, p)
		m.Normal = append(m.Normal, [3]float32{0, 0, 1})
		m.UV = append(m.UV, [2]float32{})
		return uint16(len(m.Pos) - 1)
	}
	b0, b1, b2, b3 := add([3]float32{-230, -90, 10}), add([3]float32{160, -90, 10}), add([3]float32{160, 90, 10}), add([3]float32{-230, 90, 10})
	m.Tris = append(m.Tris, [3]uint16{b0, b1, b2}, [3]uint16{b0, b2, b3})
	bodyTris := 2
	for _, c := range [][2]float32{{90, -82}, {90, 82}, {-178, -82}, {-178, 82}} {
		const n = 16
		r, w := float32(20.5), float32(14)
		out := float32(1)
		if c[1] < 0 {
			out = -1
		}
		var a, b [n]uint16
		for i := 0; i < n; i++ {
			th := float64(i) * 2 * math.Pi / n
			x, z := c[0]+r*float32(math.Cos(th)), -29+r*float32(math.Sin(th))
			a[i] = add([3]float32{x, c[1] - out*w, z})
			b[i] = add([3]float32{x, c[1], z})
		}
		for i := 0; i < n; i++ {
			j := (i + 1) % n
			m.Tris = append(m.Tris, [3]uint16{a[i], a[j], b[j]}, [3]uint16{a[i], b[j], b[i]})
		}
		m.Tris = append(m.Tris, [3]uint16{a[0], b0, b1})
		bodyTris++
		ctr := add([3]float32{c[0], c[1] + out*0.5, -29})
		var d [n]uint16
		for i := 0; i < n; i++ {
			th := float64(i) * 2 * math.Pi / n
			d[i] = add([3]float32{c[0] + 12*float32(math.Cos(th)), c[1] + out*0.5, -29 + 12*float32(math.Sin(th))})
		}
		for i := 0; i < n; i++ {
			m.Tris = append(m.Tris, [3]uint16{ctr, d[i], d[(i+1)%n]})
		}
	}
	ms := []*fnvnif.Mesh{m}
	l := Find(ms, false)
	if len(l.Wheels) != 4 {
		t.Fatalf("%d wheels, want 4", len(l.Wheels))
	}
	_, b, err := Build(ms, l, false)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("wheels %v body tris %d wheel tris %d", b.Wheels, b.BodyTris, b.WheelTris)
	if b.BodyTris != bodyTris {
		t.Errorf("body kept %d triangles, want %d (the tyres should all have come off)", b.BodyTris, bodyTris)
	}
	if r := b.WheelRadiusF * UnitsPerMetre; r < 20 || r > 23 {
		t.Errorf("wheel radius %.1f, want the tyre's 20.5 (the hub is 12)", r)
	}
}

// A wreck's wheels swapped for another model's, sized to its own; and the front turned round on request
func TestDonor(t *testing.T) {
	ms := fakeTruck()
	dl := Find(ms, false)
	d, err := DonorFrom(ms, dl)
	if err != nil {
		t.Fatal(err)
	}
	if len(d.Left) == 0 || len(d.Right) == 0 || d.Radius < 15 {
		t.Fatalf("odd donor: %d %d r %.1f", len(d.Left), len(d.Right), d.Radius)
	}
	_, own, err := Build(ms, Find(ms, false), false)
	if err != nil {
		t.Fatal(err)
	}
	_, got, err := Build(ms, Find(ms, false), false, d)
	if err != nil {
		t.Fatal(err)
	}
	for _, n := range got.WheelNotes {
		t.Log(n)
	}
	lift := got.WheelRadiusF*UnitsPerMetre - 22
	if r := got.WheelRadiusF * UnitsPerMetre; r < 24 || r > 24.2 || math.Abs(float64(got.Clearance-own.Clearance-lift)) > 0.01 || math.Abs(float64(got.Origin[2]-own.Origin[2]+lift)) > 0.01 {
		t.Errorf("not sized to the well: r %.1f clearance %.1f (own %.1f) origin z %.1f (own %.1f)", r, got.Clearance, own.Clearance, got.Origin[2], own.Origin[2])
	}
	if got.BodyTris != own.BodyTris || got.WheelTris != own.WheelTris || got.Wheels == nil {
		t.Errorf("same wheels, other counts: own %d/%d donor %d/%d", own.BodyTris, own.WheelTris, got.BodyTris, got.WheelTris)
	}
	a, b := Find(ms, false), Find(ms, false, "-x")
	c := Find(ms, false, "+x")
	if a.Fwd != 0 || b.FwdSign != -1 || c.FwdSign != 1 {
		t.Errorf("front: auto %d/%v, -x %v, +x %v", a.Fwd, a.FwdSign, b.FwdSign, c.FwdSign)
	}
	if n := Find(ms, false, "+y"); len(n.Notes) == 0 {
		t.Errorf("+y on an x-forward model went by quietly")
	}
}

// A pickup: a cab standing over the hood and the bed - the driver's eye goes under its roof, behind its middle
func TestCab(t *testing.T) {
	ms := fakeTruck()
	c := &fnvnif.Mesh{Name: "Cab:0"}
	for i := 0; i < 8; i++ {
		c.Pos = append(c.Pos, [3]float32{[]float32{0, 90}[i&1], []float32{-70, 70}[i>>1&1], []float32{140, 200}[i>>2&1]})
		c.Normal = append(c.Normal, [3]float32{0, 0, 1})
		c.UV = append(c.UV, [2]float32{})
	}
	for _, q := range [][4]uint16{{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}} {
		c.Tris = append(c.Tris, [3]uint16{q[0], q[1], q[2]}, [3]uint16{q[0], q[2], q[3]})
	}
	ms = append(ms, c)
	_, b, err := Build(ms, Find(ms, false), false)
	if err != nil {
		t.Fatal(err)
	}
	t.Logf("cab %v eye %v hood %v", b.Cab, b.Eye, b.Hood)
	if !b.Cab || b.Eye[2] < 180 || b.Eye[2] > 195 || b.Eye[1] < 20 || b.Eye[1] > 70 || b.Eye[0] > -20 || b.Hood[0] < 95 || b.Hood[0] > 120 {
		t.Errorf("cab misplaced")
	}
}

// The donor's best pair: the front tyres squashed, a rear pair is taken
func TestDonorTrueWheel(t *testing.T) {
	ms := fakeTruck()
	m := ms[0]
	for i, p := range m.Pos {
		if p[0] > 100 && p[0] < 160 && math.Abs(float64(p[1])) > 60 {
			m.Pos[i][2] = 22 + (p[2]-22)*0.8
		}
	}
	d, err := DonorFrom(ms, Find(ms, false))
	if err != nil {
		t.Fatal(err)
	}
	for _, n := range d.Notes {
		t.Log(n)
	}
	if last := d.Notes[len(d.Notes)-1]; !strings.Contains(last, "RL") {
		t.Errorf("took %s", last)
	}
}

// A trailer gives its two wheels
func TestDonorTrailer(t *testing.T) {
	ms := fakeTruck()
	m := ms[0]
	var keep [][3]uint16
	for _, tr := range m.Tris {
		x := m.Pos[tr[0]][0]
		if m.Pos[tr[0]][2] >= 30 || (x > 100 && x < 160) {
			keep = append(keep, tr)
		}
	}
	m.Tris = keep
	l := Find(ms, false)
	if len(l.Wheels) != 2 {
		t.Fatalf("%d wheels", len(l.Wheels))
	}
	d, err := DonorFrom(ms, l)
	if err != nil {
		t.Fatal(err)
	}
	t.Log(d.Notes)
	if len(d.Left) == 0 || len(d.Right) == 0 || d.Radius < 20 || d.Radius > 24 {
		t.Errorf("odd: %d %d r %.1f", len(d.Left), len(d.Right), d.Radius)
	}
}
