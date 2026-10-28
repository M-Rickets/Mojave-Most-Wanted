package car

import (
	"image"
	"image/color"
	"math"
	"sort"
)

// The steering wheel.

// SteerWheelDebug logs the pieces near the wheel that were left out.
var SteerWheelDebug = false

// SteerMaxAcross: the widest rim taken (metres); 0.56 since test 61
var SteerMaxAcross float32 = 0.56

type steerWheel struct {
	Centre, Axis [3]float32
	Radius       float32
	Tris         int
	Pieces       int
}

type swComp struct {
	part  *part
	tris  []int
	verts []int
}

func findSteeringWheel(parts []*part, unitsPerMetre, minAcross float32, centre bool, spokeDepth float32) (map[*part][]bool, steerWheel, bool) {
	m := unitsPerMetre
	if spokeDepth <= 0 {
		spokeDepth = 0.12
	}
	if minAcross <= 0 {
		minAcross = 0.26
	}
	minSide := float32(0.12)
	if centre {
		minSide = 0
	}
	var comps []*swComp
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" || p.mat.Kind == "paint" || p.mat.Kind == "light" {
			continue
		}
		comps = append(comps, components(p)...)
	}
	stats := func(c *swComp) (mn, mx, ce [3]float32) {
		mn = [3]float32{math.MaxFloat32, math.MaxFloat32, math.MaxFloat32}
		mx = [3]float32{-math.MaxFloat32, -math.MaxFloat32, -math.MaxFloat32}
		for _, vi := range c.verts {
			q := c.part.pos[vi]
			for k := 0; k < 3; k++ {
				mn[k] = min(mn[k], q[k])
				mx[k] = max(mx[k], q[k])
				ce[k] += q[k] / float32(len(c.verts))
			}
		}
		return
	}
	var best *swComp
	var bestScore float32
	var bestCentre, bestAxis [3]float32
	var bestR float32
	consider := func(c *swComp, slab bool) {
		if len(c.tris) < 24 {
			return
		}
		mn, mx, ce := stats(c)
		d := []float32{(mx[0] - mn[0]) / m, (mx[1] - mn[1]) / m, (mx[2] - mn[2]) / m}
		sort.Slice(d, func(i, j int) bool { return d[i] < d[j] })
		if !slab && SteerWheelDebug && ce[0] <= 0.01 && d[2] > 0.15 && d[2] < 0.8 {
			Logf("steer candidate: %d tris centre (%.2f %.2f %.2f) m size %.2f %.2f %.2f", len(c.tris), ce[0]/m, ce[1]/m, ce[2]/m, d[0], d[1], d[2])
		}
		if d[2] < minAcross || d[2] > SteerMaxAcross || d[2] > 1.4*d[1] || d[0] > 0.65*d[1] {
			return
		}
		ax := abs32(ce[0]) / m
		if ax < minSide || ax > 0.75 || ce[2]/m < 0.45 || ce[2]/m > 1.75 || ce[1]/m < -0.9 || ce[1]/m > 1.2 {
			return
		}
		pts := make([][3]float32, len(c.verts))
		for i, vi := range c.verts {
			pts[i] = c.part.pos[vi]
		}
		axis := smallestAxis(pts, ce)
		if axis[1] > 0 {
			axis = [3]float32{-axis[0], -axis[1], -axis[2]}
		}
		var rmax float32
		rad := make([]float32, len(pts))
		for i, q := range pts {
			v := sub(q, ce)
			a := dot(v, axis)
			r := sub(v, scale(axis, a))
			rad[i] = float32(math.Sqrt(float64(dot(r, r))))
			rmax = max(rmax, rad[i])
		}
		far := 0
		for _, r := range rad {
			if r > 0.6*rmax {
				far++
			}
		}
		ring := float32(far) / float32(len(rad))
		if ring < 0.35 {
			return
		}
		if abs32(axis[0]) > 0.35 || axis[1] > -0.64 || axis[2] < 0.08 {
			return
		}
		score := ring - abs32(d[2]-0.38)
		if slab {
			e1 := cross(axis, [3]float32{1, 0, 0})
			e1 = scale(e1, 1/float32(math.Sqrt(float64(dot(e1, e1)))))
			e2 := cross(axis, e1)
			outer := 0
			var sectors [12]bool
			for i, q := range pts {
				if rad[i] > 0.75*rmax {
					outer++
				}
				if rad[i] > 0.85*rmax {
					v := sub(q, ce)
					k := int((math.Atan2(float64(dot(v, e2)), float64(dot(v, e1))) + math.Pi) / (2 * math.Pi) * 12)
					sectors[min(max(k, 0), 11)] = true
				}
			}
			n := 0
			for _, on := range sectors {
				if on {
					n++
				}
			}
			if n < 11 {
				return
			}
			score = float32(outer)/float32(len(pts)) - abs32(d[2]-0.38)
		}
		if best == nil || score > bestScore {
			best, bestScore, bestCentre, bestAxis, bestR = c, score, ce, axis, rmax
		}
	}
	for _, c := range comps {
		consider(c, false)
	}
	if best == nil {
		var whole *swComp
		for _, c := range comps {
			if len(c.tris) < 24 {
				continue
			}
			mn, mx, ce := stats(c)
			across := max(mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2]) / m
			if across < minAcross || across > 1.0 || abs32(ce[0])/m > 0.8 || ce[2]/m < 0.4 || ce[2]/m > 1.8 {
				continue
			}
			was := best
			for deg := 5; deg <= 50; deg++ {
				t := float64(deg) * math.Pi / 180
				axis := [3]float32{0, -float32(math.Cos(t)), float32(math.Sin(t))}
				lo := make([]float32, len(c.tris))
				hi := make([]float32, len(c.tris))
				amin, amax := float32(math.MaxFloat32), float32(-math.MaxFloat32)
				for i, ti := range c.tris {
					lo[i], hi[i] = math.MaxFloat32, -math.MaxFloat32
					for _, v := range c.part.tris[ti] {
						a := dot(c.part.pos[v], axis)
						lo[i], hi[i] = min(lo[i], a), max(hi[i], a)
					}
					amin, amax = min(amin, lo[i]), max(amax, hi[i])
				}
				for a0 := amin; a0 < amax; a0 += 0.005 * m {
					slab := &swComp{part: c.part}
					seen := map[int]bool{}
					for i, ti := range c.tris {
						if lo[i] >= a0 && hi[i] <= a0+0.07*m {
							slab.tris = append(slab.tris, ti)
							for _, v := range c.part.tris[ti] {
								if w := int(v); !seen[w] {
									seen[w] = true
									slab.verts = append(slab.verts, w)
								}
							}
						}
					}
					consider(slab, true)
				}
			}
			if best != was {
				whole = c
			}
		}
		if best == nil {
			return nil, steerWheel{}, false
		}
		in := &swComp{part: whole.part}
		rest := &swComp{part: whole.part}
		seenIn, seenRest := map[int]bool{}, map[int]bool{}
		for _, ti := range whole.tris {
			ok := true
			for _, v := range whole.part.tris[ti] {
				q := sub(whole.part.pos[v], bestCentre)
				a := dot(q, bestAxis)
				rv := sub(q, scale(bestAxis, a))
				if dot(rv, rv) > (1.08*bestR)*(1.08*bestR) || a < -spokeDepth*m || a > 0.12*m {
					ok = false
				}
			}
			c, seen := rest, seenRest
			if ok {
				c, seen = in, seenIn
			}
			c.tris = append(c.tris, ti)
			for _, v := range whole.part.tris[ti] {
				if w := int(v); !seen[w] {
					seen[w] = true
					c.verts = append(c.verts, w)
				}
			}
		}
		Logf("steering wheel: rim found inside a %d-triangle piece (with its column); %d of them turn", len(whole.tris), len(in.tris))
		var kept []*swComp
		for _, c := range comps {
			if c != whole {
				kept = append(kept, c)
			}
		}
		best = in
		comps = append(kept, in)
		if len(rest.tris) > 0 {
			comps = append(comps, rest)
		}
	}
	sel := map[*part][]bool{}
	sw := steerWheel{Centre: bestCentre, Axis: bestAxis, Radius: bestR}
	for _, c := range comps {
		ok := c == best
		if !ok {
			_, _, ce := stats(c)
			v := sub(ce, bestCentre)
			a := dot(v, bestAxis)
			rr := sub(v, scale(bestAxis, a))
			r := float32(math.Sqrt(float64(dot(rr, rr))))
			spoke := r <= 0.8*bestR && a >= -spokeDepth*m && a <= 0.12*m
			column := r <= 0.25*bestR && a >= -0.3*m && a <= 0.12*m
			if spoke {
				ok = true
				for _, vi := range c.verts {
					q := sub(c.part.pos[vi], bestCentre)
					a := dot(q, bestAxis)
					rv := sub(q, scale(bestAxis, a))
					if dot(rv, rv) > (1.08*bestR)*(1.08*bestR) || a < -0.3*m || a > 0.15*m {
						ok = false
						break
					}
				}
			}
			if !ok && column {
				near := func(vi uint32) bool {
					q := sub(c.part.pos[vi], bestCentre)
					a := dot(q, bestAxis)
					rv := sub(q, scale(bestAxis, a))
					return dot(rv, rv) <= (0.75*bestR)*(0.75*bestR) && a >= -0.1*m && a <= 0.15*m
				}
				var hub []int
				for _, ti := range c.tris {
					t := c.part.tris[ti]
					if near(t[0]) && near(t[1]) && near(t[2]) {
						hub = append(hub, ti)
					}
				}
				if len(hub) > 0 {
					mask := sel[c.part]
					if mask == nil {
						mask = make([]bool, len(c.part.tris))
						sel[c.part] = mask
					}
					for _, ti := range hub {
						mask[ti] = true
					}
					sw.Tris += len(hub)
					sw.Pieces++
					continue
				}
			}
		}
		if !ok {
			if SteerWheelDebug {
				_, _, ce := stats(c)
				v := sub(ce, bestCentre)
				a := dot(v, bestAxis)
				rr := sub(v, scale(bestAxis, a))
				if r := float32(math.Sqrt(float64(dot(rr, rr)))); r < 1.5*bestR && a > -0.4*m && a < 0.3*m {
					var amin, amax, rmx float32 = 1e9, -1e9, 0
					for _, vi := range c.verts {
						q := sub(c.part.pos[vi], bestCentre)
						a := dot(q, bestAxis)
						r := sub(q, scale(bestAxis, a))
						amin, amax, rmx = min(amin, a), max(amax, a), max(rmx, float32(math.Sqrt(float64(dot(r, r)))))
					}
					Logf("steering wheel: left out %d tris (%s): centre %.2f R off-axis, %.2f m along; spans %.2f..%.2f m along, out to %.2f R",
						len(c.tris), c.part.mat.Kind, r/bestR, a/m, amin/m, amax/m, rmx/bestR)
				}
			}
			continue
		}
		mask := sel[c.part]
		if mask == nil {
			mask = make([]bool, len(c.part.tris))
			sel[c.part] = mask
		}
		for _, ti := range c.tris {
			mask[ti] = true
		}
		sw.Tris += len(c.tris)
		sw.Pieces++
	}
	return sel, sw, true
}

// Components splits a part's triangles into connected pieces (vertices welded by position, so seams don't split)
func components(p *part) []*swComp {
	key := map[[3]int32]int{}
	weld := make([]int, len(p.pos))
	for i, q := range p.pos {
		k := [3]int32{int32(math.Round(float64(q[0]) * 20)), int32(math.Round(float64(q[1]) * 20)), int32(math.Round(float64(q[2]) * 20))}
		if j, ok := key[k]; ok {
			weld[i] = j
		} else {
			key[k] = i
			weld[i] = i
		}
	}
	par := make([]int, len(p.pos))
	for i := range par {
		par[i] = i
	}
	var find func(int) int
	find = func(x int) int {
		for par[x] != x {
			par[x] = par[par[x]]
			x = par[x]
		}
		return x
	}
	for _, t := range p.tris {
		a := find(weld[t[0]])
		par[find(weld[t[1]])] = a
		par[find(weld[t[2]])] = a
	}
	byRoot := map[int]*swComp{}
	var out []*swComp
	seen := map[int]bool{}
	for ti, t := range p.tris {
		r := find(weld[t[0]])
		c := byRoot[r]
		if c == nil {
			c = &swComp{part: p}
			byRoot[r] = c
			out = append(out, c)
		}
		c.tris = append(c.tris, ti)
		for _, v := range t {
			w := weld[v]
			if !seen[w] {
				seen[w] = true
				c.verts = append(c.verts, w)
			}
		}
	}
	return out
}

// smallestAxis: the direction the points spread least in (the normal of a flat piece), by Jacobi on the covariance
func smallestAxis(pts [][3]float32, ce [3]float32) [3]float32 {
	var a [3][3]float64
	for _, q := range pts {
		d := [3]float64{float64(q[0] - ce[0]), float64(q[1] - ce[1]), float64(q[2] - ce[2])}
		for i := 0; i < 3; i++ {
			for j := 0; j < 3; j++ {
				a[i][j] += d[i] * d[j]
			}
		}
	}
	v := [3][3]float64{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}
	for sweep := 0; sweep < 50; sweep++ {
		off := math.Abs(a[0][1]) + math.Abs(a[0][2]) + math.Abs(a[1][2])
		if off < 1e-12 {
			break
		}
		for p := 0; p < 2; p++ {
			for q := p + 1; q < 3; q++ {
				if math.Abs(a[p][q]) < 1e-15 {
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
	k := 0
	for i := 1; i < 3; i++ {
		if a[i][i] < a[k][k] {
			k = i
		}
	}
	n := [3]float32{float32(v[0][k]), float32(v[1][k]), float32(v[2][k])}
	l := float32(math.Sqrt(float64(dot(n, n))))
	return [3]float32{n[0] / l, n[1] / l, n[2] / l}
}

func dot(a, b [3]float32) float32              { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] }
func scale(a [3]float32, s float32) [3]float32 { return [3]float32{a[0] * s, a[1] * s, a[2] * s} }
func abs32(x float32) float32 {
	if x < 0 {
		return -x
	}
	return x
}

// steerTile draws the cabin around the steering wheel for a developer check: left, looking down the column; right, from
// the driver's side.
func steerTile(parts []*part, sw steerWheel, found bool, unitsPerMetre float32) *image.RGBA {
	const S = 150
	img := image.NewRGBA(image.Rect(0, 0, 2*S, S))
	for i := range img.Pix {
		img.Pix[i] = 22
		if i%4 == 3 {
			img.Pix[i] = 255
		}
	}
	c, n := sw.Centre, sw.Axis
	if !found {
		c = [3]float32{-0.37 * unitsPerMetre, 0.3 * unitsPerMetre, 0.85 * unitsPerMetre}
		n = [3]float32{0, -0.96, 0.28}
	}
	up := [3]float32{0, 0, 1}
	e1 := cross(up, n)
	e1 = scale(e1, 1/float32(math.Sqrt(float64(dot(e1, e1)))))
	e2 := cross(n, e1)
	half := 0.45 * unitsPerMetre
	k := float32(S/2) / half
	line := func(x0, y0, x1, y1 int, col color.RGBA) {
		dx, dy := x1-x0, y1-y0
		steps := max(abs(dx), abs(dy))
		if steps == 0 {
			steps = 1
		}
		for i := 0; i <= steps; i++ {
			x, y := x0+dx*i/steps, y0+dy*i/steps
			if x >= 0 && y >= 0 && x < 2*S && y < S {
				img.SetRGBA(x, y, col)
			}
		}
	}
	for pass := 0; pass < 2; pass++ {
		for _, p := range parts {
			if p.group >= 0 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" {
				continue
			}
			wheel := p.group == -2
			if (pass == 1) != wheel {
				continue
			}
			col := color.RGBA{95, 95, 105, 255}
			if wheel {
				col = color.RGBA{235, 60, 50, 255}
			}
			for _, t := range p.tris {
				var pt [3][2][2]int
				ok := true
				for j, vi := range t {
					v := sub(p.pos[vi], c)
					a, x, y := dot(v, n), dot(v, e1), dot(v, e2)
					if !wheel && (abs32(x) > half || abs32(y) > half || a < -half || a > half*0.6) {
						ok = false
						break
					}
					pt[j][0] = [2]int{int(float32(S/2) + x*k), int(float32(S/2) - y*k)}
					pt[j][1] = [2]int{S + int(float32(S/2)-a*k), int(float32(S/2) - y*k)}
				}
				if !ok {
					continue
				}
				for view := 0; view < 2; view++ {
					for j := 0; j < 3; j++ {
						a, b := pt[j][view], pt[(j+1)%3][view]
						line(a[0], a[1], b[0], b[1], col)
					}
				}
			}
		}
	}
	if !found {
		for i := 0; i < S; i++ {
			img.SetRGBA(i, i, color.RGBA{250, 200, 40, 255})
			img.SetRGBA(i, S-1-i, color.RGBA{250, 200, 40, 255})
		}
	}
	return img
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}
