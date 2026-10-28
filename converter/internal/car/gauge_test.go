package car

import "testing"

// The face sits behind the wheel (towards the dash), above the column, faces the driver, and reads left to right
func TestGaugeShape(t *testing.T) {
	A := norm([3]float32{0, -0.9, -0.3})
	sw := steerWheel{Centre: [3]float32{-25, 20, 60}, Axis: A, Radius: 13}
	s := gaugeShape(sw, nil, "n.dds")
	if s.Name != "MMWGauge" || len(s.Pos) != 8 || len(s.Tris) != 4 {
		t.Fatal("shape", s.Name, len(s.Pos), len(s.Tris))
	}
	c := scale(add(s.Pos[0], s.Pos[2]), 0.5)
	if c[1] <= sw.Centre[1] {
		t.Error("not behind the wheel", c)
	}
	if c[2] <= sw.Centre[2] {
		t.Error("not above the column", c)
	}
	if !(s.Pos[1][0] > s.Pos[0][0]) {
		t.Error("u runs the wrong way", s.Pos[0], s.Pos[1])
	}
	if !(s.Pos[3][2] < s.Pos[0][2]) {
		t.Error("v runs the wrong way")
	}
	tr := s.Tris[0]
	n := cross(sub(s.Pos[tr[1]], s.Pos[tr[0]]), sub(s.Pos[tr[2]], s.Pos[tr[0]]))
	if dot(n, A) <= 0 {
		t.Error("front face points away from the driver")
	}
	tr = s.Tris[2]
	n = cross(sub(s.Pos[tr[1]], s.Pos[tr[0]]), sub(s.Pos[tr[2]], s.Pos[tr[0]]))
	if dot(n, A) >= 0 {
		t.Error("back face points at the driver")
	}
}
