package car

import (
	"fmt"
	"math"
	"os"
	"strings"

	"mmw/internal/nif"
)

// Snow on the car (cars.json snow

type SnowSpec struct {
	WindshieldY [2]float32  `json:"windshieldY"`
	Wipers      []WiperSpec `json:"wipers,omitempty"`
	AutoWipers  bool        `json:"autoWipers,omitempty"`
}

type WiperSpec struct {
	Pivot [3]float32    `json:"pivot"`
	Arm   [][3]float32  `json:"arm"`
	Blade [2][3]float32 `json:"blade"`
	Tube  float32       `json:"tube"`
	tris  map[*part]map[int]bool
}

const (
	SnowBodyW          = 256
	SnowGlassW         = 128
	SnowGlassH         = 64
	SnowDepthPath      = `textures\mmw\snow_depth.dds`
	SnowGlassDrivePath = `textures\mmw\snow_glass_drive.dds`
)

type snowBuild struct {
	body   []*nif.Shape
	glass  []*nif.Shape
	name   string
	wipers []wiperBuild
}

type wiperBuild struct {
	name  string
	pivot [3]float32
	group int
}

// prepareSnow moves each wiper's triangles to its own group (1000 - k) and builds the snow layers.
func prepareSnow(parts []*part, sp *SnowSpec, flatN, bodyTex, glassTex string) ([]*part, *snowBuild) {
	sb := &snowBuild{}
	if sp.AutoWipers && len(sp.Wipers) == 0 {
		sp.Wipers = findWipers(parts)
	}
	near := func(q [3]float32, w *WiperSpec) bool {
		segs := [][2][3]float32{{w.Blade[0], w.Blade[1]}}
		for i := 0; i+1 < len(w.Arm); i++ {
			segs = append(segs, [2][3]float32{w.Arm[i], w.Arm[i+1]})
		}
		tube := w.Tube
		if tube <= 0 {
			tube = 2.5
		}
		for _, s := range segs {
			d := sub(s[1], s[0])
			l2 := dot(d, d)
			t := float32(0)
			if l2 > 0 {
				t = min(1, max(0, dot(sub(q, s[0]), d)/l2))
			}
			e := sub(q, add(s[0], scale(d, t)))
			if dot(e, e) <= tube*tube {
				return true
			}
		}
		return false
	}
	var out []*part
	counts := make([]int, len(sp.Wipers))
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" || p.mat.Kind == "paint" || len(sp.Wipers) == 0 {
			out = append(out, p)
			continue
		}
		take := make([]int, len(p.tris))
		any := false
		for k := range sp.Wipers {
			for ti := range sp.Wipers[k].tris[p] {
				take[ti] = k + 1
				counts[k]++
				any = true
			}
		}
		for _, c := range components(p) {
			if any {
				break
			}
			for k := range sp.Wipers {
				in := 0
				for _, vi := range c.verts {
					if near(p.pos[vi], &sp.Wipers[k]) {
						in++
					}
				}
				if len(c.verts) > 0 && float32(in) >= 0.9*float32(len(c.verts)) {
					for _, ti := range c.tris {
						take[ti] = k + 1
					}
					counts[k] += len(c.tris)
					any = true
					break
				}
			}
		}
		if !any {
			out = append(out, p)
			continue
		}
		byK := map[int][][3]uint32{}
		for ti, t := range p.tris {
			byK[take[ti]] = append(byK[take[ti]], t)
		}
		for k, tris := range byK {
			q := *p
			q.tris = tris
			if k > 0 {
				q.group = -1000 - (k - 1)
			}
			out = append(out, &q)
		}
	}
	parts = out
	for k := range sp.Wipers {
		if counts[k] > 0 {
			continue
		}
		out = nil
		for _, p := range parts {
			if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" || p.mat.Kind == "paint" {
				out = append(out, p)
				continue
			}
			var keep, take [][3]uint32
			for _, t := range p.tris {
				if near(p.pos[t[0]], &sp.Wipers[k]) && near(p.pos[t[1]], &sp.Wipers[k]) && near(p.pos[t[2]], &sp.Wipers[k]) {
					take = append(take, t)
				} else {
					keep = append(keep, t)
				}
			}
			if len(take) == 0 {
				out = append(out, p)
				continue
			}
			q, w := *p, *p
			q.tris, w.tris, w.group = keep, take, -1000-k
			if len(keep) > 0 {
				out = append(out, &q)
			}
			out = append(out, &w)
			counts[k] += len(take)
		}
		parts = out
		if counts[k] > 0 {
			Logf("snow: wiper %d taken triangle by triangle (no piece of its own)", k)
		}
	}
	for k, n := range counts {
		Logf("snow: wiper %d: %d triangles", k, n)
	}

	type tri struct {
		p     *part
		t     [3]uint32
		n     [3]float32
		c     [3]float32
		glass bool
	}
	var all []tri
	mn := [3]float32{math.MaxFloat32, math.MaxFloat32, math.MaxFloat32}
	mx := [3]float32{-math.MaxFloat32, -math.MaxFloat32, -math.MaxFloat32}
	for _, p := range parts {
		if p.group != -1 || p.mat == nil {
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
			all = append(all, tri{p, t, n, triCentre(p, t), p.mat.Kind == "glass"})
			for _, q := range [][3]float32{a, b, c} {
				for k := 0; k < 3; k++ {
					mn[k], mx[k] = min(mn[k], q[k]), max(mx[k], q[k])
				}
			}
		}
	}
	if len(all) == 0 {
		return parts, nil
	}
	gw, gh := int(mx[0]-mn[0])+3, int(mx[1]-mn[1])+3
	top := make([]float32, gw*gh)
	topT := make([]int32, gw*gh)
	for i := range top {
		top[i], topT[i] = -math.MaxFloat32, -1
	}
	for ti, t := range all {
		a, b, c := t.p.pos[t.t[0]], t.p.pos[t.t[1]], t.p.pos[t.t[2]]
		x0, x1 := int(min(a[0], b[0], c[0])-mn[0]), int(max(a[0], b[0], c[0])-mn[0])+1
		y0, y1 := int(min(a[1], b[1], c[1])-mn[1]), int(max(a[1], b[1], c[1])-mn[1])+1
		d := (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1])
		if abs32(d) < 1e-9 {
			continue
		}
		for gy := max(y0, 0); gy <= y1 && gy < gh; gy++ {
			for gx := max(x0, 0); gx <= x1 && gx < gw; gx++ {
				px, py := mn[0]+float32(gx)+0.5, mn[1]+float32(gy)+0.5
				l1 := ((b[1]-c[1])*(px-c[0]) + (c[0]-b[0])*(py-c[1])) / d
				l2 := ((c[1]-a[1])*(px-c[0]) + (a[0]-c[0])*(py-c[1])) / d
				l3 := 1 - l1 - l2
				if l1 < -0.05 || l2 < -0.05 || l3 < -0.05 {
					continue
				}
				if z := l1*a[2] + l2*b[2] + l3*c[2]; z > top[gy*gw+gx] {
					top[gy*gw+gx], topT[gy*gw+gx] = z, int32(ti)
				}
			}
		}
	}
	isWindshield := func(t tri) bool {
		if !t.glass {
			return false
		}
		if sp.WindshieldY == [2]float32{} {
			return t.c[1] > 0 && t.n[1] > 0.25
		}
		return t.c[1] >= sp.WindshieldY[0] && t.c[1] <= sp.WindshieldY[1]
	}
	var cand []tri
	seenT := map[int32]bool{}
	for _, ti := range topT {
		if ti >= 0 && !seenT[ti] {
			seenT[ti] = true
			if t := all[ti]; t.n[2] >= 0.6 && isWindshield(t) {
				cand = append(cand, t)
			}
		}
	}
	glassT := highestPiece(cand, func(t tri) [3][3]float32 {
		return [3][3]float32{t.p.pos[t.t[0]], t.p.pos[t.t[1]], t.p.pos[t.t[2]]}
	})
	wsSet := map[[3]float32]bool{}
	for _, t := range glassT {
		wsSet[t.c] = true
	}
	isWindshield = func(t tri) bool { return wsSet[t.c] }
	var N, e1, e2 [3]float32
	var s0, t0, W, H float32
	plane := func(q [3]float32) (float32, float32) { return dot(q, e1) - s0, dot(q, e2) - t0 }
	if len(glassT) > 0 {
		for _, t := range glassT {
			a, b, c := t.p.pos[t.t[0]], t.p.pos[t.t[1]], t.p.pos[t.t[2]]
			n := cross(sub(b, a), sub(c, a))
			if n[2] < 0 {
				n = scale(n, -1)
			}
			N = add(N, n)
		}
		N = norm(N)
		e1 = norm(sub([3]float32{1, 0, 0}, scale(N, N[0])))
		e2 = cross(N, e1)
		var s1, t1 float32 = -math.MaxFloat32, -math.MaxFloat32
		s0, t0 = math.MaxFloat32, math.MaxFloat32
		for _, t := range glassT {
			for _, v := range t.t {
				q := t.p.pos[v]
				s, u := dot(q, e1), dot(q, e2)
				s0, s1, t0, t1 = min(s0, s), max(s1, s), min(t0, u), max(t1, u)
			}
		}
		W, H = s1-s0, t1-t0
		if W < 40 || H < 20 {
			glassT, W, H = nil, 0, 0
			isWindshield = func(tri) bool { return false }
		}
	}
	sb.piles(all[0].p, pilesIn{
		mn: mn, mx: mx, gw: gw, gh: gh, top: top,
		kind: func(i int) (uint8, float32) {
			ti := topT[i]
			if ti < 0 {
				return 0, 0
			}
			t := all[ti]
			if len(glassT) > 0 && isWindshield(t) {
				return 2, t.n[2]
			}
			return 1, t.n[2]
		},
		plane: plane, W: W, H: H, flatN: flatN, bodyTex: bodyTex, glassTex: glassTex,
	})
	if len(glassT) == 0 {
		Logf("snow: no windshield found")
		return parts, sb
	}
	const cell = 0.5
	cw, ch := int(W/cell)+2, int(H/cell)+2
	on := make([]bool, cw*ch)
	for _, t := range glassT {
		var P [3][2]float32
		for k, v := range t.t {
			P[k][0], P[k][1] = plane(t.p.pos[v])
		}
		x0, x1 := int(min(P[0][0], P[1][0], P[2][0])/cell), int(max(P[0][0], P[1][0], P[2][0])/cell)+1
		y0, y1 := int(min(P[0][1], P[1][1], P[2][1])/cell), int(max(P[0][1], P[1][1], P[2][1])/cell)+1
		a, b, c := P[0], P[1], P[2]
		d := (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1])
		if abs32(d) < 1e-9 {
			continue
		}
		for y := max(y0, 0); y <= y1 && y < ch; y++ {
			for x := max(x0, 0); x <= x1 && x < cw; x++ {
				px, py := (float32(x)+0.5)*cell, (float32(y)+0.5)*cell
				l1 := ((b[1]-c[1])*(px-c[0]) + (c[0]-b[0])*(py-c[1])) / d
				l2 := ((c[1]-a[1])*(px-c[0]) + (a[0]-c[0])*(py-c[1])) / d
				if l1 >= 0 && l2 >= 0 && l1+l2 <= 1 {
					on[y*cw+x] = true
				}
			}
		}
	}
	inside := func(x, y float32) bool {
		for _, o := range [][2]float32{{0, 0}, {1.5, 0}, {-1.5, 0}, {0, 1.5}, {0, -1.5}} {
			gx, gy := int((x+o[0])/cell), int((y+o[1])/cell)
			if gx < 0 || gy < 0 || gx >= cw || gy >= ch || !on[gy*cw+gx] {
				return false
			}
		}
		return true
	}
	up := norm(sub([3]float32{0, 0, 1}, scale(N, N[2])))
	var desc []string
	for k, w := range sp.Wipers {
		if counts[k] == 0 {
			continue
		}
		px, py := plane(w.Pivot)
		ax, ay := plane(w.Blade[0])
		bx, by := plane(w.Blade[1])
		ra := float32(math.Hypot(float64(ax-px), float64(ay-py)))
		rb := float32(math.Hypot(float64(bx-px), float64(by-py)))
		mid := scale(add(w.Blade[0], w.Blade[1]), 0.5)
		mx2, my2 := plane(mid)
		a0 := math.Atan2(float64(my2-py), float64(mx2-px))
		d := sub(mid, w.Pivot)
		d = sub(d, scale(N, dot(d, N)))
		sign := float32(1)
		if dot(cross(N, d), up) < 0 {
			sign = -1
		}
		if os.Getenv("MMW_WIPERDEBUG") != "" {
			for deg := 0.0; deg <= 135; deg += 9 {
				var fl []string
				for _, e := range [][2]float32{{ax, ay}, {bx, by}, {mx2, my2}} {
					th := float64(sign) * deg * math.Pi / 180
					c, s := math.Cos(th), math.Sin(th)
					x := px + float32(c)*(e[0]-px) - float32(s)*(e[1]-py)
					y := py + float32(s)*(e[0]-px) + float32(c)*(e[1]-py)
					fl = append(fl, fmt.Sprintf("(%.0f %.0f %v)", x, y, inside(x, y)))
				}
				Logf("wiperdebug: wiper %d at %.0f: %s", k, deg, strings.Join(fl, " "))
			}
		}
		sweep, miss := 0.0, 0
		for deg := 1.0; deg <= 112; deg++ {
			ok := true
			for _, e := range [][2]float32{{ax, ay}, {bx, by}, {mx2, my2}} {
				th := float64(sign) * deg * math.Pi / 180
				c, s := math.Cos(th), math.Sin(th)
				x := px + float32(c)*(e[0]-px) - float32(s)*(e[1]-py)
				y := py + float32(s)*(e[0]-px) + float32(c)*(e[1]-py)
				if !inside(x, y) {
					ok = false
				}
			}
			if !ok {
				if deg <= 20 {
					continue
				}
				if miss++; miss > 8 {
					break
				}
				continue
			}
			miss = 0
			sweep = deg
		}
		sweep = math.Max(0, sweep-2) * math.Pi / 180
		if f := os.Getenv("MMW_WIPERDEBUG"); f != "" {
			if g, err := os.OpenFile(f, os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
				for _, deg := range []float64{0, sweep * 180 / math.Pi} {
					th := float64(sign) * deg * math.Pi / 180
					c, sn := math.Cos(th), math.Sin(th)
					for _, e := range [][3]float32{w.Blade[0], w.Blade[1]} {
						r := sub(e, w.Pivot)
						r = add(add(scale(r, float32(c)), scale(cross(N, r), float32(sn))), scale(N, dot(N, r)*float32(1-c)))
						fmt.Fprintf(g, "S %d %.0f %.3f %.3f %.3f\n", k, deg, w.Pivot[0]+r[0], w.Pivot[1]+r[1], w.Pivot[2]+r[2])
					}
				}
				g.Close()
			}
		}
		axis := scale(N, sign)
		sb.wipers = append(sb.wipers, wiperBuild{
			name:  fmt.Sprintf("MMWWiper%d %.5f %.5f %.5f %.4f", k, axis[0], axis[1], axis[2], sweep),
			pivot: w.Pivot, group: -1000 - k})
		desc = append(desc, fmt.Sprintf("%.2f %.2f %.2f %.2f %.4f %d", px, py, min(ra, rb), max(ra, rb), a0, int(sign)))
		Logf("snow: wiper %d pivot (%.1f %.1f %.1f) blade %.1f..%.1f from it, sweeps %.0f degrees", k, w.Pivot[0], w.Pivot[1], w.Pivot[2], min(ra, rb), max(ra, rb), sweep*180/math.Pi)
	}
	sb.name = fmt.Sprintf("MMWSnowGlass %.2f %.2f %d", W, H, len(desc))
	if len(desc) > 0 {
		sb.name += " " + strings.Join(desc, " ")
	}
	Logf("snow: windshield %d triangles, %.1f x %.1f units, normal (%.2f %.2f %.2f)", len(glassT), W, H, N[0], N[1], N[2])
	return parts, sb
}

// nodes: the snow and wiper nodes for the driving model (build: the converter's part -> shape builder)
func (sb *snowBuild) nodes(wiperParts map[int][]*part, build func([]*part, [3]float32) []*nif.Shape) []nif.NamedNode {
	var out []nif.NamedNode
	if len(sb.body) > 0 {
		out = append(out, nif.NamedNode{Name: "MMWSnowBody", Shapes: sb.body})
	}
	if len(sb.glass) > 0 {
		name := sb.name
		if name == "" {
			name = fmt.Sprintf("MMWSnowGlass 1 1 0")
		}
		out = append(out, nif.NamedNode{Name: name, Shapes: sb.glass})
	}
	for _, w := range sb.wipers {
		if ps := wiperParts[w.group]; len(ps) > 0 {
			out = append(out, nif.NamedNode{Name: w.name, At: w.pivot, Shapes: build(ps, w.pivot)})
		}
	}
	return out
}

// highestPiece: of the triangles, the connected piece (corners welded by position) whose highest point is highest
func highestPiece[T any](ts []T, corners func(T) [3][3]float32) []T {
	if len(ts) == 0 {
		return nil
	}
	par := make([]int, len(ts))
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
	seen := map[[3]int32]int{}
	for i, t := range ts {
		for _, q := range corners(t) {
			k := [3]int32{int32(math.Round(float64(q[0]) * 10)), int32(math.Round(float64(q[1]) * 10)), int32(math.Round(float64(q[2]) * 10))}
			if j, ok := seen[k]; ok {
				par[find(i)] = find(j)
			} else {
				seen[k] = i
			}
		}
	}
	topZ := map[int]float32{}
	area := map[int]float32{}
	var most float32
	for i, t := range ts {
		r := find(i)
		c := corners(t)
		for _, q := range c {
			if z, ok := topZ[r]; !ok || q[2] > z {
				topZ[r] = q[2]
			}
		}
		n := cross(sub(c[1], c[0]), sub(c[2], c[0]))
		area[r] += float32(math.Sqrt(float64(dot(n, n)))) / 2
		most = max(most, area[r])
	}
	best, bz := -1, float32(-math.MaxFloat32)
	for r, z := range topZ {
		if z > bz && area[r] >= 0.4*most {
			best, bz = r, z
		}
	}
	var out []T
	for i, t := range ts {
		if find(i) == best {
			out = append(out, t)
		}
	}
	return out
}
