package gamecar

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"mmw/internal/fnvnif"
	"mmw/internal/nif"
)

// Steer is a steering wheel taken from another game model: its shapes about its centre, turned so its column (the way
// its face looks) is +z, and its rim's radius.
type Steer struct {
	Shapes []*nif.Shape
	Radius float32
	From   string
}

type sel struct {
	m    *fnvnif.Mesh
	tris []int
}

// SteeringWheel finds the steering wheel in a model
func SteeringWheel(ms []*fnvnif.Mesh, whole bool, round ...bool) (*Steer, error) {
	var pick []sel
	var names []string
	for _, m := range ms {
		hit := strings.Contains(strings.ToLower(m.Name), "steer")
		for _, p := range m.Path {
			hit = hit || strings.Contains(strings.ToLower(p), "steer")
		}
		if hit && !m.Hidden {
			pick = append(pick, sel{m, nil})
			names = append(names, m.Name)
		}
	}
	if len(pick) == 0 && len(round) > 0 && round[0] {
		var note string
		pick, note = roundPart(ms)
		if len(pick) == 0 {
			return nil, fmt.Errorf("no round part (%s)", note)
		}
		names = append(names, note)
	}
	if len(pick) == 0 && !whole {
		return nil, fmt.Errorf("no part named steer...")
	}
	if len(pick) == 0 {
		var mn, mx [3]float32
		first := true
		for _, m := range ms {
			for _, p := range m.Pos {
				for k := 0; k < 3; k++ {
					if first || p[k] < mn[k] {
						mn[k] = p[k]
					}
					if first || p[k] > mx[k] {
						mx[k] = p[k]
					}
				}
				first = false
			}
		}
		if first || mx[0]-mn[0] > 80 || mx[1]-mn[1] > 80 || mx[2]-mn[2] > 80 {
			return nil, fmt.Errorf("no steering wheel in it")
		}
		for _, m := range ms {
			if !m.Hidden {
				pick = append(pick, sel{m, nil})
				names = append(names, m.Name)
			}
		}
	}
	return steerOf(pick, strings.Join(names, ", "))
}

// Verts of a selection (each once)
func (s sel) verts() []int {
	if s.tris == nil {
		v := make([]int, len(s.m.Pos))
		for i := range v {
			v[i] = i
		}
		return v
	}
	seen := map[int]bool{}
	var v []int
	for _, t := range s.tris {
		for _, k := range s.m.Tris[t] {
			if !seen[int(k)] {
				seen[int(k)] = true
				v = append(v, int(k))
			}
		}
	}
	return v
}

// eig3: a symmetric 3x3's eigenvalues (largest first) and their vectors (Jacobi)
func eig3(a [3][3]float64) ([3]float64, [3][3]float64) {
	v := [3][3]float64{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}
	for sweep := 0; sweep < 50; sweep++ {
		for p := 0; p < 2; p++ {
			for q := p + 1; q < 3; q++ {
				if math.Abs(a[p][q]) < 1e-12 {
					continue
				}
				th := 0.5 * math.Atan2(2*a[p][q], a[q][q]-a[p][p])
				c, s := math.Cos(th), math.Sin(th)
				for k := 0; k < 3; k++ {
					akp, akq := a[k][p], a[k][q]
					a[k][p], a[k][q] = c*akp-s*akq, s*akp+c*akq
				}
				for k := 0; k < 3; k++ {
					apk, aqk := a[p][k], a[q][k]
					a[p][k], a[q][k] = c*apk-s*aqk, s*apk+c*aqk
				}
				for k := 0; k < 3; k++ {
					vkp, vkq := v[k][p], v[k][q]
					v[k][p], v[k][q] = c*vkp-s*vkq, s*vkp+c*vkq
				}
			}
		}
	}
	ev := [3]float64{a[0][0], a[1][1], a[2][2]}
	idx := []int{0, 1, 2}
	sort.Slice(idx, func(i, j int) bool { return ev[idx[i]] > ev[idx[j]] })
	var out [3]float64
	var vec [3][3]float64
	for r, i := range idx {
		out[r] = ev[i]
		vec[r] = [3]float64{v[0][i], v[1][i], v[2][i]}
	}
	return out, vec
}

func roundPart(ms []*fnvnif.Mesh) ([]sel, string) {
	ps := Pieces(ms)
	var lo, hi float32 = 1e9, -1e9
	for _, p := range ps {
		lo, hi = min(lo, p.Min[2]), max(hi, p.Max[2])
	}
	best, bestScore := -1, 1e9
	var bestR float64
	var cands []string
	type round struct {
		i         int
		r         float64
		c         [3]float64
		flat, rnd float64
	}
	var rs []round
	for i, p := range ps {
		if ms[p.Mesh].Hidden || p.Centre(2) < lo+0.5*(hi-lo) {
			continue
		}
		s := sel{ms[p.Mesh], p.Tris}
		vs := s.verts()
		if len(vs) < 12 {
			continue
		}
		var c [3]float64
		for _, k := range vs {
			for j := 0; j < 3; j++ {
				c[j] += float64(s.m.Pos[k][j]) / float64(len(vs))
			}
		}
		var cv [3][3]float64
		for _, k := range vs {
			d := [3]float64{float64(s.m.Pos[k][0]) - c[0], float64(s.m.Pos[k][1]) - c[1], float64(s.m.Pos[k][2]) - c[2]}
			for a := 0; a < 3; a++ {
				for b := 0; b < 3; b++ {
					cv[a][b] += d[a] * d[b] / float64(len(vs))
				}
			}
		}
		ev, _ := eig3(cv)
		if ev[0] <= 0 {
			continue
		}
		flat, rnd := ev[2]/ev[0], ev[1]/ev[0]
		r := math.Sqrt(2 * ev[0])
		if flat > 0.2 || rnd < 0.7 || r < 7 || r > 25 {
			continue
		}
		rs = append(rs, round{i, r, c, flat, rnd})
	}
	for a, x := range rs {
		why := ""
		if x.flat < 0.002 {
			why = " - a face, no rim"
		}
		for b, y := range rs {
			d := math.Hypot(math.Hypot(x.c[0]-y.c[0], x.c[1]-y.c[1]), x.c[2]-y.c[2])
			if a != b && math.Abs(x.c[2]-y.c[2]) < 3 && math.Abs(x.r-y.r) < 0.15*math.Max(x.r, y.r) && d > 1.5*math.Max(x.r, y.r) {
				why += fmt.Sprintf(" - one of a pair (with piece %d)", y.i)
				break
			}
		}
		cands = append(cands, fmt.Sprintf("piece %d (%s) r %.1f at %.0f %.0f %.0f flat %.3f round %.2f%s", x.i, ms[ps[x.i].Mesh].Name, x.r, x.c[0], x.c[1], x.c[2], x.flat, x.rnd, why))
		if why != "" {
			continue
		}
		if score := x.flat + (1 - x.rnd); score < bestScore {
			best, bestScore, bestR = x.i, score, x.r
		}
	}
	if best < 0 {
		return nil, "none round and flat enough: " + strings.Join(cands, "; ")
	}
	bp := ps[best]
	out := []sel{{ms[bp.Mesh], bp.Tris}}
	for i, p := range ps {
		if i == best {
			continue
		}
		in := true
		for k := 0; k < 3; k++ {
			if p.Min[k] < bp.Min[k]-0.5 || p.Max[k] > bp.Max[k]+0.5 {
				in = false
			}
		}
		if in {
			out = append(out, sel{ms[p.Mesh], p.Tris})
		}
	}
	return out, fmt.Sprintf("piece %d, r %.1f, with %d inside it; round flat parts: %s", best, bestR, len(out)-1, strings.Join(cands, "; "))
}

// steerOf: the selection about its centre, laid flat (its face +z)
func steerOf(pick []sel, from string) (*Steer, error) {
	var c [3]float64
	n := 0
	for _, s := range pick {
		for _, k := range s.verts() {
			for j := 0; j < 3; j++ {
				c[j] += float64(s.m.Pos[k][j])
			}
			n++
		}
	}
	if n < 8 {
		return nil, fmt.Errorf("too small")
	}
	for k := range c {
		c[k] /= float64(n)
	}
	var cv [3][3]float64
	for _, s := range pick {
		for _, k := range s.verts() {
			p := s.m.Pos[k]
			d := [3]float64{float64(p[0]) - c[0], float64(p[1]) - c[1], float64(p[2]) - c[2]}
			for i := 0; i < 3; i++ {
				for j := 0; j < 3; j++ {
					cv[i][j] += d[i] * d[j] / float64(n)
				}
			}
		}
	}
	nrm := leastAxis(cv)
	ref := [3]float64{1, 0, 0}
	if math.Abs(nrm[0]) > 0.9 {
		ref = [3]float64{0, 1, 0}
	}
	ux := norm3(cross3(ref, nrm))
	uy := cross3(nrm, ux)
	st := &Steer{From: from}
	var rmax float64
	for _, s := range pick {
		m := s.m
		sh := &nif.Shape{Name: "MMWSteer_" + m.Name, Diffuse: m.Diffuse, NormalTx: m.NormalMap, Alpha: 1, Specular: [3]float32{0.25, 0.25, 0.25}, Gloss: 10}
		idx := map[int]uint16{}
		for _, k := range s.verts() {
			p := m.Pos[k]
			d := [3]float64{float64(p[0]) - c[0], float64(p[1]) - c[1], float64(p[2]) - c[2]}
			q := [3]float32{float32(dot3(d, ux)), float32(dot3(d, uy)), float32(dot3(d, nrm))}
			idx[k] = uint16(len(sh.Pos))
			sh.Pos = append(sh.Pos, q)
			rmax = math.Max(rmax, math.Hypot(float64(q[0]), float64(q[1])))
			nv := [3]float64{0, 0, 1}
			if m.Normal != nil {
				nv = [3]float64{float64(m.Normal[k][0]), float64(m.Normal[k][1]), float64(m.Normal[k][2])}
			}
			sh.Normal = append(sh.Normal, [3]float32{float32(dot3(nv, ux)), float32(dot3(nv, uy)), float32(dot3(nv, nrm))})
			if m.UV != nil {
				sh.UV = append(sh.UV, m.UV[k])
			} else {
				sh.UV = append(sh.UV, [2]float32{})
			}
		}
		tris := s.tris
		if tris == nil {
			tris = make([]int, len(m.Tris))
			for i := range tris {
				tris[i] = i
			}
		}
		for _, t := range tris {
			tr := m.Tris[t]
			sh.Tris = append(sh.Tris, [3]uint16{idx[int(tr[0])], idx[int(tr[1])], idx[int(tr[2])]})
		}
		st.Shapes = append(st.Shapes, sh)
	}
	st.Radius = float32(rmax)
	if st.Radius < 3 {
		return nil, fmt.Errorf("too small (r %.1f)", st.Radius)
	}
	return st, nil
}

// placed: the wheel at `at` (car frame), its column along `axis` (towards the driver), its rim `radius` - shapes about
// `at`, for the MMWCockpitWheel node
func (s *Steer) placed(axis [3]float64, radius float32) []*nif.Shape {
	z := norm3(axis)
	ref := [3]float64{1, 0, 0}
	if math.Abs(z[0]) > 0.9 {
		ref = [3]float64{0, 1, 0}
	}
	x := norm3(cross3(ref, z))
	y := cross3(z, x)
	k := float64(radius / s.Radius)
	var out []*nif.Shape
	for _, sh := range s.Shapes {
		c := *sh
		c.Pos = make([][3]float32, len(sh.Pos))
		c.Normal = make([][3]float32, len(sh.Normal))
		for i, p := range sh.Pos {
			for j := 0; j < 3; j++ {
				c.Pos[i][j] = float32(k * (float64(p[0])*x[j] + float64(p[1])*y[j] + float64(p[2])*z[j]))
			}
		}
		for i, p := range sh.Normal {
			for j := 0; j < 3; j++ {
				c.Normal[i][j] = float32(float64(p[0])*x[j] + float64(p[1])*y[j] + float64(p[2])*z[j])
			}
		}
		c.Tangent, c.Bitan = nil, nil
		out = append(out, &c)
	}
	return out
}

func leastAxis(m [3][3]float64) [3]float64 {
	tr := m[0][0] + m[1][1] + m[2][2]
	v := [3]float64{0.3, 0.5, 0.8}
	for it := 0; it < 200; it++ {
		var w [3]float64
		for i := 0; i < 3; i++ {
			for j := 0; j < 3; j++ {
				a := -m[i][j]
				if i == j {
					a += tr
				}
				w[i] += a * v[j]
			}
		}
		v = norm3(w)
	}
	return v
}

func cross3(a, b [3]float64) [3]float64 {
	return [3]float64{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}
func dot3(a, b [3]float64) float64 { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }
func norm3(a [3]float64) [3]float64 {
	l := math.Sqrt(dot3(a, a))
	if l == 0 {
		return [3]float64{0, 0, 1}
	}
	return [3]float64{a[0] / l, a[1] / l, a[2] / l}
}

// MadeSteeringWheel: a plain steering wheel made here when no game model has one - a round rim, three spokes and a hub,
// flat in x-y (its column +z), radius 1 - in the given texture (a tyre's dark rubber suits it)
func MadeSteeringWheel(diffuse, normalTx string) *Steer {
	s := &nif.Shape{Name: "MMWSteerMade", Diffuse: diffuse, NormalTx: normalTx, Alpha: 1, Specular: [3]float32{0.15, 0.15, 0.15}, Gloss: 8}
	add := func(p, n [3]float64, u, v float32) uint16 {
		s.Pos = append(s.Pos, [3]float32{float32(p[0]), float32(p[1]), float32(p[2])})
		s.Normal = append(s.Normal, [3]float32{float32(n[0]), float32(n[1]), float32(n[2])})
		s.UV = append(s.UV, [2]float32{u, v})
		return uint16(len(s.Pos) - 1)
	}
	const nr, nt = 32, 8
	base := len(s.Pos)
	for i := 0; i < nr; i++ {
		a := float64(i) * 2 * math.Pi / nr
		c := [3]float64{math.Cos(a), math.Sin(a), 0}
		for j := 0; j < nt; j++ {
			b := float64(j) * 2 * math.Pi / nt
			n := [3]float64{c[0] * math.Cos(b), c[1] * math.Cos(b), math.Sin(b)}
			add([3]float64{c[0] + 0.09*n[0], c[1] + 0.09*n[1], 0.09 * n[2]}, n, float32(i)/nr, float32(j)/nt)
		}
	}
	for i := 0; i < nr; i++ {
		for j := 0; j < nt; j++ {
			a := uint16(base + i*nt + j)
			b := uint16(base + ((i+1)%nr)*nt + j)
			c := uint16(base + ((i+1)%nr)*nt + (j+1)%nt)
			d := uint16(base + i*nt + (j+1)%nt)
			s.Tris = append(s.Tris, [3]uint16{a, b, c}, [3]uint16{a, c, d})
		}
	}
	box := func(p, q [3]float64, w, h float64) {
		f := norm3([3]float64{q[0] - p[0], q[1] - p[1], q[2] - p[2]})
		sd := norm3(cross3(f, [3]float64{0, 0, 1}))
		up := cross3(sd, f)
		var v [8]uint16
		for k := 0; k < 8; k++ {
			e := p
			if k&1 != 0 {
				e = q
			}
			ws, hs := w/2, h/2
			if k&2 == 0 {
				ws = -ws
			}
			if k&4 == 0 {
				hs = -hs
			}
			pt := [3]float64{e[0] + sd[0]*ws + up[0]*hs, e[1] + sd[1]*ws + up[1]*hs, e[2] + sd[2]*ws + up[2]*hs}
			v[k] = add(pt, norm3([3]float64{sd[0]*ws + up[0]*hs, sd[1]*ws + up[1]*hs, sd[2]*ws + up[2]*hs}), float32(k&1), float32(k>>1&1))
		}
		for _, q := range [][4]int{{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}} {
			s.Tris = append(s.Tris, [3]uint16{v[q[0]], v[q[1]], v[q[2]]}, [3]uint16{v[q[0]], v[q[2]], v[q[3]]})
		}
	}
	for _, a := range []float64{0, math.Pi, 1.5 * math.Pi} {
		box([3]float64{0.15 * math.Cos(a), 0.15 * math.Sin(a), -0.08}, [3]float64{0.93 * math.Cos(a), 0.93 * math.Sin(a), 0}, 0.12, 0.05)
	}
	box([3]float64{0, -0.12, -0.1}, [3]float64{0, 0.12, -0.1}, 0.28, 0.14)
	return &Steer{Shapes: []*nif.Shape{s}, Radius: 1.09, From: "made here (no game model has one)"}
}
