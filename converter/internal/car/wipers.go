package car

import (
	"fmt"
	"math"
	"os"
	"sort"
)

// findWipers finds a car's own wipers: long thin pieces lying on the foot of the windshield.
func findWipers(parts []*part) []WiperSpec {
	type gt struct {
		q    [3][3]float32
		n, c [3]float32
	}
	var glass []gt
	for _, p := range parts {
		if p.group != -1 || p.mat == nil || p.mat.Kind != "glass" {
			continue
		}
		for _, t := range p.tris {
			a, b, c := p.pos[t[0]], p.pos[t[1]], p.pos[t[2]]
			n := cross(sub(b, a), sub(c, a))
			if dot(n, n) < 1e-8 {
				continue
			}
			n = norm(n)
			if n[2] < 0 {
				n = scale(n, -1)
			}
			ce := triCentre(p, t)
			if ce[1] > 0 && n[1] > 0.25 && n[2] >= 0.6 {
				glass = append(glass, gt{[3][3]float32{a, b, c}, n, ce})
			}
		}
	}
	ws := highestPiece(glass, func(t gt) [3][3]float32 { return t.q })
	if len(ws) < 4 {
		Logf("snow: wipers: no windshield to find them on")
		return nil
	}
	var N, P0 [3]float32
	var area float32
	for _, t := range ws {
		n := cross(sub(t.q[1], t.q[0]), sub(t.q[2], t.q[0]))
		if n[2] < 0 {
			n = scale(n, -1)
		}
		N = add(N, n)
		a := float32(math.Sqrt(float64(dot(n, n))))
		P0 = add(P0, scale(t.c, a))
		area += a
	}
	N = norm(N)
	P0 = scale(P0, 1/area)
	e1 := norm(sub([3]float32{1, 0, 0}, scale(N, N[0])))
	e2 := cross(N, e1)
	if e2[2] < 0 {
		e2 = scale(e2, -1)
	}
	s0, s1, t0, t1 := float32(math.MaxFloat32), float32(-math.MaxFloat32), float32(math.MaxFloat32), float32(-math.MaxFloat32)
	for _, t := range ws {
		for _, q := range t.q {
			s, u := dot(sub(q, P0), e1), dot(sub(q, P0), e2)
			s0, s1, t0, t1 = min(s0, s), max(s1, s), min(t0, u), max(t1, u)
		}
	}
	H := t1 - t0
	type wsT struct{ a, b, c [3]float32 }
	var wsP []wsT
	for _, t := range ws {
		var v [3][3]float32
		for k, q := range t.q {
			q = sub(q, P0)
			v[k] = [3]float32{dot(q, e1), dot(q, e2), dot(q, N)}
		}
		wsP = append(wsP, wsT{v[0], v[1], v[2]})
	}
	glassH := func(x, y float32) (float32, bool) {
		for _, t := range wsP {
			d := (t.b[1]-t.c[1])*(t.a[0]-t.c[0]) + (t.c[0]-t.b[0])*(t.a[1]-t.c[1])
			if d == 0 {
				continue
			}
			l1 := ((t.b[1]-t.c[1])*(x-t.c[0]) + (t.c[0]-t.b[0])*(y-t.c[1])) / d
			l2 := ((t.c[1]-t.a[1])*(x-t.c[0]) + (t.a[0]-t.c[0])*(y-t.c[1])) / d
			if l1 >= -1e-4 && l2 >= -1e-4 && l1+l2 <= 1+1e-4 {
				return l1*t.a[2] + l2*t.b[2] + (1-l1-l2)*t.c[2], true
			}
		}
		return 0, false
	}
	footH := map[int][2]float32{}
	for _, t := range wsP {
		for _, v := range [][3]float32{t.a, t.b, t.c} {
			k := int(math.Floor(float64(v[0] / 2)))
			if f, ok := footH[k]; !ok || v[1] < f[0] {
				footH[k] = [2]float32{v[1], v[2]}
			}
		}
	}
	footAt := func(x float32) float32 {
		k := int(math.Floor(float64(x / 2)))
		for d := 0; d < 60; d++ {
			if f, ok := footH[k-d]; ok {
				return f[1]
			}
			if f, ok := footH[k+d]; ok {
				return f[1]
			}
		}
		return 0
	}
	type piece struct {
		p      *part
		c      *swComp
		a, b   [3]float32
		length float32
		wide   float32
	}
	var pieces []piece
	var dbgF *os.File
	defer func() {
		if dbgF != nil {
			dbgF.Close()
		}
	}()
	if os.Getenv("MMW_WIPERDEBUG") != "" {
		Logf("wiperdebug: glass s %.1f..%.1f t %.1f..%.1f N (%.2f %.2f %.2f) P0 (%.1f %.1f %.1f)", s0, s1, t0, t1, N[0], N[1], N[2], P0[0], P0[1], P0[2])
		dbgF, _ = os.Create(os.Getenv("MMW_WIPERDEBUG"))
		fmt.Fprintf(dbgF, "F %g %g %g %g %g %g %g %g %g %g %g %g\n", P0[0], P0[1], P0[2], e1[0], e1[1], e1[2], e2[0], e2[1], e2[2], N[0], N[1], N[2])
		for pi, p := range parts {
			kind := ""
			if p.mat != nil {
				kind = p.mat.Kind + "/" + p.mat.Name
			}
			for _, c := range components(p) {
				var ce [3]float32
				for _, vi := range c.verts {
					ce = add(ce, p.pos[vi])
				}
				ce = scale(ce, 1/float32(len(c.verts)))
				q := sub(ce, P0)
				s, u, h := dot(q, e1), dot(q, e2), dot(q, N)
				if s < s0-10 || s > s1+10 || u < t0-20 || u > t0+0.6*H || h < -15 || h > 15 {
					continue
				}
				var lo, hi [3]float32
				for i, vi := range c.verts {
					q := sub(p.pos[vi], P0)
					v := [3]float32{dot(q, e1), dot(q, e2), dot(q, N)}
					if i == 0 {
						lo, hi = v, v
					}
					for k := 0; k < 3; k++ {
						lo[k], hi[k] = min(lo[k], v[k]), max(hi[k], v[k])
					}
				}
				Logf("wiperdebug: part %d %s g%d l%d: %d verts, s %.1f..%.1f t %.1f..%.1f h %.1f..%.1f", pi, kind, p.group, p.light, len(c.verts), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])
			}
			n := 0
			var lo, hi [3]float32
			for _, t := range p.tris {
				in := true
				for _, vi := range t {
					q := sub(p.pos[vi], P0)
					s, u, h := dot(q, e1), dot(q, e2), dot(q, N)
					if s < s0-4 || s > s1+4 || u < t0-12 || u > t0+0.5*H || h < -6 || h > 7 {
						in = false
					}
				}
				if in {
					if dbgF != nil {
						for _, vi := range t {
							q := sub(p.pos[vi], P0)
							fmt.Fprintf(dbgF, "%d %s %.2f %.2f %.2f ", pi, p.mat.Kind, dot(q, e1), dot(q, e2), dot(q, N))
						}
						fmt.Fprintln(dbgF)
					}
					for _, vi := range t {
						q := sub(p.pos[vi], P0)
						v := [3]float32{dot(q, e1), dot(q, e2), dot(q, N)}
						if n == 0 {
							lo, hi = v, v
						}
						for k := 0; k < 3; k++ {
							lo[k], hi[k] = min(lo[k], v[k]), max(hi[k], v[k])
						}
					}
					n++
				}
			}
			if n > 0 {
				Logf("wiperdebug: part %d %s g%d l%d: %d of %d triangles in the foot region, s %.1f..%.1f t %.1f..%.1f h %.1f..%.1f", pi, kind, p.group, p.light, n, len(p.tris), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2])
			}
		}
	}
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" || p.mat.Kind == "paint" {
			continue
		}
		for _, c := range components(p) {
			if len(c.verts) < 6 {
				continue
			}
			ok := true
			var ce [3]float32
			for _, vi := range c.verts {
				q := sub(p.pos[vi], P0)
				s, u, h := dot(q, e1), dot(q, e2), dot(q, N)
				off := h - footAt(s)
				lo, hi := float32(-8), float32(7)
				if g, ok := glassH(s, u); ok {
					off, lo, hi = h-g, -2, 6
				}
				if s < s0-4 || s > s1+4 || u < t0-12 || u > t0+0.5*H || off < lo || off > hi {
					ok = false
					break
				}
				ce = add(ce, p.pos[vi])
			}
			if !ok {
				continue
			}
			ce = scale(ce, 1/float32(len(c.verts)))
			pts := make([][3]float32, len(c.verts))
			for i, vi := range c.verts {
				pts[i] = p.pos[vi]
			}
			ax := largestAxis(pts, ce)
			lo, hi := float32(math.MaxFloat32), float32(-math.MaxFloat32)
			var a, b [3]float32
			var wide float32
			for _, q := range pts {
				d := dot(sub(q, ce), ax)
				if d < lo {
					lo, a = d, q
				}
				if d > hi {
					hi, b = d, q
				}
			}
			for _, q := range pts {
				r := sub(sub(q, ce), scale(ax, dot(sub(q, ce), ax)))
				wide = max(wide, float32(math.Sqrt(float64(dot(r, r)))))
			}
			if os.Getenv("MMW_WIPERDEBUG") != "" && hi-lo >= 8 {
				pa, pb := sub(a, P0), sub(b, P0)
				Logf("wiperdebug: candidate %s: %d verts, %.1f long, %.1f wide, ends (%.1f %.1f %.1f) (%.1f %.1f %.1f) [s t h]", p.mat.Kind, len(c.verts), hi-lo, wide,
					dot(pa, e1), dot(pa, e2), dot(pa, N), dot(pb, e1), dot(pb, e2), dot(pb, N))
			}
			if hi-lo < 10 || wide > max(2.25, min(4.5, 0.1*(hi-lo))) {
				continue
			}
			pieces = append(pieces, piece{p, c, a, b, hi - lo, wide})
			if os.Getenv("MMW_WIPERDEBUG") != "" {
				pa, pb := sub(a, P0), sub(b, P0)
				Logf("wiperdebug: piece %d: %d verts, %.1f long, %.1f wide, ends (%.1f %.1f %.1f) (%.1f %.1f %.1f) [s t h]", len(pieces)-1, len(c.verts), hi-lo, wide,
					dot(pa, e1), dot(pa, e2), dot(pa, N), dot(pb, e1), dot(pb, e2), dot(pb, N))
			}
		}
	}
	clearance := func(i int) (float32, float32) {
		var sum float32
		n := 0
		for _, vi := range pieces[i].c.verts {
			q := sub(pieces[i].p.pos[vi], P0)
			if h, ok := glassH(dot(q, e1), dot(q, e2)); ok {
				sum += dot(q, N) - h
				n++
			}
		}
		if n == 0 {
			return 0, 0
		}
		return sum / float32(n), float32(n) / float32(len(pieces[i].c.verts))
	}
	if os.Getenv("MMW_WIPERDEBUG") != "" {
		for i := range pieces {
			c, over := clearance(i)
			Logf("wiperdebug: piece %d stands %.1f off the glass, %.0f%% over it", i, c, over*100)
		}
	}
	st := func(q [3]float32) [2]float32 { q = sub(q, P0); return [2]float32{dot(q, e1), dot(q, e2)} }
	dir := func(i int) [2]float32 {
		a, b := st(pieces[i].a), st(pieces[i].b)
		d := [2]float32{b[0] - a[0], b[1] - a[1]}
		l := float32(math.Hypot(float64(d[0]), float64(d[1])))
		return [2]float32{d[0] / l, d[1] / l}
	}
	touch := func(i, j int) bool {
		for _, ab := range [][2]int{{i, j}, {j, i}} {
			a, b := pieces[ab[0]], pieces[ab[1]]
			for _, vi := range a.c.verts {
				q := a.p.pos[vi]
				for _, ti := range b.c.tris {
					t := b.p.tris[ti]
					if d := sub(q, closestOnTri(q, b.p.pos[t[0]], b.p.pos[t[1]], b.p.pos[t[2]])); dot(d, d) < 1.5*1.5 {
						return true
					}
				}
			}
		}
		return false
	}
	type link struct {
		i, j int
		ang  float64
	}
	var links []link
	for i := range pieces {
		for j := i + 1; j < len(pieces); j++ {
			if !touch(i, j) {
				continue
			}
			di, dj := dir(i), dir(j)
			c := math.Abs(float64(di[0]*dj[0] + di[1]*dj[1]))
			ang := math.Acos(math.Min(1, c)) * 180 / math.Pi
			if os.Getenv("MMW_WIPERDEBUG") != "" {
				Logf("wiperdebug: pieces %d and %d touch, %.0f degrees apart", i, j, ang)
			}
			if ang <= 20 {
				links = append(links, link{i, j, ang})
			}
		}
	}
	sort.SliceStable(links, func(a, b int) bool { return links[a].ang < links[b].ang })
	shape := func(g []int) (span, stray float32) {
		var ends [][2]float32
		for _, i := range g {
			ends = append(ends, st(pieces[i].a), st(pieces[i].b))
		}
		var ea, eb [2]float32
		for x := range ends {
			for y := x + 1; y < len(ends); y++ {
				d := float32(math.Hypot(float64(ends[x][0]-ends[y][0]), float64(ends[x][1]-ends[y][1])))
				if d > span {
					span, ea, eb = d, ends[x], ends[y]
				}
			}
		}
		if span == 0 {
			return
		}
		ux, uy := (eb[0]-ea[0])/span, (eb[1]-ea[1])/span
		for _, q := range ends {
			stray = max(stray, float32(math.Abs(float64((q[0]-ea[0])*uy-(q[1]-ea[1])*ux))))
		}
		return
	}
	groupOf := make([]int, len(pieces))
	members := map[int][]int{}
	for i := range pieces {
		groupOf[i] = i
		members[i] = []int{i}
	}
	for _, l := range links {
		gi, gj := groupOf[l.i], groupOf[l.j]
		if gi == gj {
			continue
		}
		g := append(append([]int{}, members[gi]...), members[gj]...)
		span, stray := shape(g)
		spi, _ := shape(members[gi])
		spj, _ := shape(members[gj])
		both := len(members[gi]) >= 2 && len(members[gj]) >= 2 && spi >= 0.35*(s1-s0) && spj >= 0.35*(s1-s0)
		if both || span > 0.75*(s1-s0) || stray > max(4, 0.12*span) {
			if os.Getenv("MMW_WIPERDEBUG") != "" {
				Logf("wiperdebug: not joining %v and %v: %.0f long, strays %.1f", members[gi], members[gj], span, stray)
			}
			continue
		}
		for _, i := range members[gj] {
			groupOf[i] = gi
		}
		members[gi] = g
		delete(members, gj)
	}
	var keys []int
	for k := range members {
		keys = append(keys, k)
	}
	sort.Ints(keys)
	var out []WiperSpec
	for _, k := range keys {
		g := members[k]
		span, _ := shape(g)
		if span < 25 {
			continue
		}
		maxLen := float32(0)
		for _, i := range g {
			maxLen = max(maxLen, pieces[i].length)
		}
		longest := -1
		for _, i := range g {
			if pieces[i].length < 0.6*maxLen {
				continue
			}
			if longest < 0 || pieces[i].wide < pieces[longest].wide-0.15 ||
				(pieces[i].wide <= pieces[longest].wide+0.15 && pieces[i].length > pieces[longest].length) {
				longest = i
			}
		}
		lowest, lowC := -1, float32(0)
		for _, i := range g {
			if pieces[i].length < 0.5*maxLen {
				continue
			}
			c, over := clearance(i)
			if over >= 0.6 && (lowest < 0 || c < lowC-0.3) {
				lowest, lowC = i, c
			}
		}
		if lowest >= 0 {
			longest = lowest
		}
		bm := scale(add(pieces[longest].a, pieces[longest].b), 0.5)
		var pivot [3]float32
		var far float32 = -1
		for _, i := range g {
			for _, e := range [][3]float32{pieces[i].a, pieces[i].b} {
				d := sub(e, bm)
				if l := dot(d, d); l > far {
					far, pivot = l, e
				}
			}
		}
		tip := pieces[longest].a
		if d1, d2 := sub(pieces[longest].a, pivot), sub(pieces[longest].b, pivot); dot(d2, d2) > dot(d1, d1) {
			tip = pieces[longest].b
		}
		blade := [2][3]float32{pieces[longest].a, pieces[longest].b}
		if len(g) == 1 {
			blade = [2][3]float32{add(pivot, scale(sub(tip, pivot), 0.45)), tip}
		}
		w := WiperSpec{Pivot: pivot, Arm: [][3]float32{pivot, tip}, Blade: blade, Tube: 3, tris: map[*part]map[int]bool{}}
		for _, i := range g {
			m := w.tris[pieces[i].p]
			if m == nil {
				m = map[int]bool{}
				w.tris[pieces[i].p] = m
			}
			for _, ti := range pieces[i].c.tris {
				m[ti] = true
			}
		}
		out = append(out, w)
		if dbgF != nil {
			for p, m := range w.tris {
				for ti := range m {
					for _, vi := range p.tris[ti] {
						q := sub(p.pos[vi], P0)
						fmt.Fprintf(dbgF, "W %s %.2f %.2f %.2f ", fmt.Sprint(len(out)-1), dot(q, e1), dot(q, e2), dot(q, N))
					}
					fmt.Fprintln(dbgF)
				}
			}
			q := sub(pivot, P0)
			fmt.Fprintf(dbgF, "P %d %.2f %.2f %.2f\n", len(out)-1, dot(q, e1), dot(q, e2), dot(q, N))
		}
		ps := st(pivot)
		Logf("snow: wipers: found one, %d pieces, %.0f units, pivot (%.1f %.1f %.1f) %.0f across the glass", len(g), span, pivot[0], pivot[1], pivot[2], ps[0])
	}
	for k := range out {
		w := &out[k]
		type wt struct{ a, b, c [3]float32 }
		var own []wt
		lo, hi := [3]float32{math.MaxFloat32, math.MaxFloat32, math.MaxFloat32}, [3]float32{-math.MaxFloat32, -math.MaxFloat32, -math.MaxFloat32}
		for p, m := range w.tris {
			for ti := range m {
				t := p.tris[ti]
				own = append(own, wt{p.pos[t[0]], p.pos[t[1]], p.pos[t[2]]})
				for _, vi := range t {
					for j := 0; j < 3; j++ {
						lo[j], hi[j] = min(lo[j], p.pos[vi][j]-1.5), max(hi[j], p.pos[vi][j]+1.5)
					}
				}
			}
		}
		added := 0
		for _, p := range parts {
			if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" || p.mat.Kind == "paint" {
				continue
			}
			for _, c := range components(p) {
				if w.tris[p] != nil && w.tris[p][c.tris[0]] {
					continue
				}
				taken := false
				for k2 := range out {
					if out[k2].tris[p] != nil && out[k2].tris[p][c.tris[0]] {
						taken = true
					}
				}
				if taken || len(c.verts) > 400 {
					continue
				}
				in := true
				for _, vi := range c.verts {
					q := p.pos[vi]
					if q[0] < lo[0] || q[0] > hi[0] || q[1] < lo[1] || q[1] > hi[1] || q[2] < lo[2] || q[2] > hi[2] {
						in = false
						break
					}
					near := false
					for _, t := range own {
						if d := sub(q, closestOnTri(q, t.a, t.b, t.c)); dot(d, d) <= 1.5*1.5 {
							near = true
							break
						}
					}
					if !near {
						in = false
						break
					}
				}
				if !in {
					continue
				}
				m := w.tris[p]
				if m == nil {
					m = map[int]bool{}
					w.tris[p] = m
				}
				for _, ti := range c.tris {
					m[ti] = true
				}
				added += len(c.tris)
			}
		}
		if added > 0 {
			Logf("snow: wipers: %d more triangles along wiper %d (its small pieces)", added, k)
		}
	}
	if len(out) > 3 {
		out = out[:3]
	}
	if len(out) == 0 {
		Logf("snow: wipers: none found at the foot of the windshield (%d long thin pieces there)", len(pieces))
	}
	return out
}

// closestOnTri: the point of triangle abc nearest q
func closestOnTri(q, a, b, c [3]float32) [3]float32 {
	ab, ac, ap := sub(b, a), sub(c, a), sub(q, a)
	d1, d2 := dot(ab, ap), dot(ac, ap)
	if d1 <= 0 && d2 <= 0 {
		return a
	}
	bp := sub(q, b)
	d3, d4 := dot(ab, bp), dot(ac, bp)
	if d3 >= 0 && d4 <= d3 {
		return b
	}
	vc := d1*d4 - d3*d2
	if vc <= 0 && d1 >= 0 && d3 <= 0 {
		return add(a, scale(ab, d1/(d1-d3)))
	}
	cp := sub(q, c)
	d5, d6 := dot(ab, cp), dot(ac, cp)
	if d6 >= 0 && d5 <= d6 {
		return c
	}
	vb := d5*d2 - d1*d6
	if vb <= 0 && d2 >= 0 && d6 <= 0 {
		return add(a, scale(ac, d2/(d2-d6)))
	}
	va := d3*d6 - d5*d4
	if va <= 0 && d4-d3 >= 0 && d5-d6 >= 0 {
		return add(b, scale(sub(c, b), (d4-d3)/((d4-d3)+(d5-d6))))
	}
	den := 1 / (va + vb + vc)
	return add(a, add(scale(ab, vb*den), scale(ac, vc*den)))
}
