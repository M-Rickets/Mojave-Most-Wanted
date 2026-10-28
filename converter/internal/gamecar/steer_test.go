package gamecar

import (
	"math"
	"testing"

	"mmw/internal/fnvnif"
)

// A ring (a steering wheel) tilted in a vehicle model, named "SteeringWheel": found, its face found, laid flat
func TestSteeringWheel(t *testing.T) {
	m := &fnvnif.Mesh{Name: "SteeringWheel01"}
	nrm := [3]float64{0, -0.6, 0.8}
	u := [3]float64{1, 0, 0}
	v := cross3(nrm, u)
	const n = 24
	for i := 0; i < n; i++ {
		a := float64(i) * 2 * math.Pi / n
		for _, r := range []float64{11, 13} {
			p := [3]float32{}
			for k := 0; k < 3; k++ {
				p[k] = float32([]float64{5, 40, 60}[k] + r*math.Cos(a)*u[k] + r*math.Sin(a)*v[k])
			}
			m.Pos = append(m.Pos, p)
			m.UV = append(m.UV, [2]float32{})
		}
	}
	for i := 0; i < n; i++ {
		j := (i + 1) % n
		m.Tris = append(m.Tris, [3]uint16{uint16(2 * i), uint16(2 * j), uint16(2*j + 1)}, [3]uint16{uint16(2 * i), uint16(2*j + 1), uint16(2*i + 1)})
	}
	body := &fnvnif.Mesh{Name: "Body", Pos: [][3]float32{{-100, -200, 0}, {100, 200, 150}, {0, 0, 0}}, Tris: [][3]uint16{{0, 1, 2}}}
	s, err := SteeringWheel([]*fnvnif.Mesh{body, m}, false)
	if err != nil {
		t.Fatal(err)
	}
	if s.Radius < 12.5 || s.Radius > 13.5 {
		t.Errorf("radius %.1f, want 13", s.Radius)
	}
	for _, sh := range s.Shapes {
		for _, p := range sh.Pos {
			if math.Abs(float64(p[2])) > 0.01 {
				t.Fatalf("not laid flat: z %.3f", p[2])
			}
		}
	}
	out := s.placed([3]float64{0, -1, 0}, 16)
	for _, p := range out[0].Pos {
		if math.Abs(float64(p[1])) > 0.01 {
			t.Fatalf("not upright: y %.3f", p[1])
		}
	}
}

// The made one: round, flat about z, radius ~1
func TestMadeSteeringWheel(t *testing.T) {
	s := MadeSteeringWheel(`textures\x.dds`, "")
	var rmax float64
	for _, p := range s.Shapes[0].Pos {
		rmax = math.Max(rmax, math.Hypot(float64(p[0]), float64(p[1])))
		if math.Abs(float64(p[2])) > 0.2 {
			t.Fatalf("not flat: %v", p)
		}
	}
	if rmax < 1.05 || rmax > 1.1 {
		t.Errorf("radius %.2f", rmax)
	}
	if len(s.Shapes[0].Tris) < 500 {
		t.Errorf("%d triangles", len(s.Shapes[0].Tris))
	}
}

// A front loader's: the wheel isn't named - the round part high up with a rim's thickness is found, not the flat round
// faces (a gauge) or a round pair either side (lamps: gc11 took one of those, "a flat grey wheel")
func TestSteeringWheelRound(t *testing.T) {
	ring := func(name string, c [3]float64, nrm [3]float64, r, tube float64) *fnvnif.Mesh {
		m := &fnvnif.Mesh{Name: name}
		u := norm3(cross3(nrm, [3]float64{0, 0, 1}))
		if dot3(u, u) < 0.5 {
			u = [3]float64{1, 0, 0}
		}
		v := cross3(nrm, u)
		const n, nt = 24, 6
		for i := 0; i < n; i++ {
			a := float64(i) * 2 * math.Pi / n
			for j := 0; j < nt; j++ {
				b := float64(j) * 2 * math.Pi / nt
				p := [3]float32{}
				for k := 0; k < 3; k++ {
					out := math.Cos(a)*u[k] + math.Sin(a)*v[k]
					p[k] = float32(c[k] + (r+tube*math.Cos(b))*out + tube*math.Sin(b)*nrm[k])
				}
				m.Pos = append(m.Pos, p)
			}
		}
		for i := 0; i < n; i++ {
			for j := 0; j < nt; j++ {
				a0, a1 := uint16(i*nt+j), uint16(((i+1)%n)*nt+j)
				b0, b1 := uint16(i*nt+(j+1)%nt), uint16(((i+1)%n)*nt+(j+1)%nt)
				m.Tris = append(m.Tris, [3]uint16{a0, a1, b1}, [3]uint16{a0, b1, b0})
			}
		}
		return m
	}
	disc := func(name string, c [3]float64, r float64) *fnvnif.Mesh {
		m := &fnvnif.Mesh{Name: name}
		const n = 24
		for i := 0; i < n; i++ {
			a := float64(i) * 2 * math.Pi / n
			m.Pos = append(m.Pos, [3]float32{float32(c[0] + r*math.Cos(a)), float32(c[1]), float32(c[2] + r*math.Sin(a))})
		}
		for i := 1; i+1 < n; i++ {
			m.Tris = append(m.Tris, [3]uint16{0, uint16(i), uint16(i + 1)})
		}
		return m
	}
	wheel := ring("FrontLoader:3", [3]float64{5, 40, 120}, norm3([3]float64{0, -0.6, 0.8}), 12, 1)
	gauge := disc("FrontLoader:2", [3]float64{0, 60, 130}, 12)
	lampL := ring("FrontLoader:1", [3]float64{-30, 100, 125}, [3]float64{0, 1, 0}, 11, 0.8)
	lampR := ring("FrontLoader:1", [3]float64{30, 100, 125}, [3]float64{0, 1, 0}, 11, 0.8)
	b := &fnvnif.Mesh{Name: "FrontLoader:0", Pos: [][3]float32{{-100, -200, 0}, {100, -200, 0}, {100, 200, 150}, {-100, 200, 150}}, Tris: [][3]uint16{{0, 1, 2}, {0, 2, 3}}}
	ms := []*fnvnif.Mesh{b, gauge, lampL, lampR, wheel}
	if _, err := SteeringWheel(ms, false); err == nil {
		t.Fatal("found one without being told to look for a round part")
	}
	s, err := SteeringWheel(ms, false, true)
	if err != nil {
		t.Fatal(err)
	}
	t.Log(s.From)
	if s.Shapes[0].Name != "MMWSteer_FrontLoader:3" {
		t.Errorf("took %s", s.Shapes[0].Name)
	}
	if s.Radius < 12.5 || s.Radius > 13.5 {
		t.Errorf("radius %.1f", s.Radius)
	}
}
