package car

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"mmw/internal/dds"
	"mmw/internal/nif"
)

// Extra holds the per-car settings that only some cars need (cars.json: seats, steer, lamps, shiftLights, suspension),
// passed by the launcher as one JSON object.
type Extra struct {
	Seats      int          `json:"seats,omitempty"`
	SteerMin   float32      `json:"steerMin,omitempty"`
	SteerDepth float32      `json:"steerDepth,omitempty"`
	Lamps      *LampSet     `json:"lamps,omitempty"`
	Shift      *ShiftLEDs   `json:"shiftLights,omitempty"`
	Links      bool         `json:"suspensionLinks,omitempty"`
	ArchGap    float32      `json:"archGap,omitempty"`
	Gadgets    *DashGadgets `json:"dashGadgets,omitempty"`
	Snow       *SnowSpec    `json:"snow,omitempty"`
	Gauge      *GaugeSpec   `json:"gauge,omitempty"`
}

// LampSet: a car whose lights aren't Most Wanted's four-channel light material (the Ariel Atom's are plain lenses, one
// material for every lamp): the lens material and where each lamp is.
type LampSet struct {
	Material string  `json:"material"`
	Reach    float32 `json:"reach,omitempty"`
	Mirror   bool    `json:"mirror,omitempty"`
	Units    []Lamp  `json:"units"`
}

type Lamp struct {
	At   [3]float32 `json:"at"`
	Role string     `json:"role"`
}

// ShiftLEDs: the dash's shift lights, painted on its texture (unlit) at these texture coordinates.
type ShiftLEDs struct {
	Texture string       `json:"texture"`
	LEDs    [][2]float32 `json:"leds"`
	Colors  [][3]uint8   `json:"colors"`
	Radius  float32      `json:"radius,omitempty"`
}

var roleByName = map[string]int{"head": LightHead, "tail": LightTail, "brake": LightBrake, "reverse": LightReverse, "none": LightNone}

// applyLamps gives the lens triangles of a LampSet their light roles.
func applyLamps(parts []*part, ls *LampSet) []*part {
	reach := ls.Reach
	if reach <= 0 {
		reach = 3
	}
	type at struct {
		p    [3]float32
		role int
	}
	var lamps []at
	for _, u := range ls.Units {
		r, ok := roleByName[strings.ToLower(u.Role)]
		if !ok {
			continue
		}
		lamps = append(lamps, at{u.At, r})
		if ls.Mirror && u.At[0] != 0 {
			lamps = append(lamps, at{[3]float32{-u.At[0], u.At[1], u.At[2]}, r})
		}
	}
	want := strings.ToLower(ls.Material)
	counts := map[int]int{}
	var out []*part
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || !strings.HasSuffix(fmt.Sprintf("%x", p.mat.ID), want) {
			out = append(out, p)
			continue
		}
		by := map[int][][3]uint32{}
		var rest [][3]uint32
		for _, t := range p.tris {
			c := triCentre(p, t)
			best, bd := -2, reach*reach
			for _, l := range lamps {
				if d := dist2(c, l.p); d < bd {
					best, bd = l.role, d
				}
			}
			if best < 0 {
				rest = append(rest, t)
				continue
			}
			by[best] = append(by[best], t)
		}
		if len(rest) > 0 {
			q := *p
			q.tris = rest
			out = append(out, &q)
		}
		for r := 0; r < 4; r++ {
			if len(by[r]) == 0 {
				continue
			}
			q := *p
			q.tris, q.light = by[r], r
			out = append(out, &q)
			counts[r] += len(by[r])
		}
	}
	Logf("lamps (%s): head %d, tail %d, brake %d, reverse %d triangles", ls.Material, counts[LightHead], counts[LightTail], counts[LightBrake], counts[LightReverse])
	return out
}

func triCentre(p *part, t [3]uint32) [3]float32 {
	a, b, c := p.pos[t[0]], p.pos[t[1]], p.pos[t[2]]
	return [3]float32{(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3}
}

func dist2(a, b [3]float32) float32 {
	d := sub(a, b)
	return dot(d, d)
}

// shiftLEDs places the shift lights: for each LED's texture point, the dash triangle drawn there (barycentric in UV)
// gives its position and facing.
func shiftLEDs(parts []*part, sh *ShiftLEDs, glowTx, flatN string) []nif.NamedNode {
	want := strings.ToLower(sh.Texture)
	rad := sh.Radius
	if rad <= 0 {
		rad = 0.16
	}
	var nodes []nif.NamedNode
	for i, uv := range sh.LEDs {
		found := false
		var pos, n [3]float32
		for _, p := range parts {
			if found || p.mat == nil || p.uv == nil || p.group >= 0 {
				continue
			}
			id, ok := p.mat.Tex("diffuse")
			if !ok || !strings.HasSuffix(fmt.Sprintf("%x", id), want) {
				continue
			}
			for _, t := range p.tris {
				a, b, c := p.uv[t[0]], p.uv[t[1]], p.uv[t[2]]
				d := (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1])
				if math.Abs(float64(d)) < 1e-12 {
					continue
				}
				l1 := ((b[1]-c[1])*(uv[0]-c[0]) + (c[0]-b[0])*(uv[1]-c[1])) / d
				l2 := ((c[1]-a[1])*(uv[0]-c[0]) + (a[0]-c[0])*(uv[1]-c[1])) / d
				l3 := 1 - l1 - l2
				if l1 < -1e-5 || l2 < -1e-5 || l3 < -1e-5 {
					continue
				}
				pa, pb, pc := p.pos[t[0]], p.pos[t[1]], p.pos[t[2]]
				for k := 0; k < 3; k++ {
					pos[k] = l1*pa[k] + l2*pb[k] + l3*pc[k]
				}
				n = norm(cross(sub(pb, pa), sub(pc, pa)))
				found = true
				break
			}
		}
		if !found {
			Logf("shift light %d: texture point (%.4f %.4f) not found on the dash", i, uv[0], uv[1])
			continue
		}
		if dot(n, [3]float32{0, -1, 0.3}) < 0 {
			n = scale(n, -1)
		}
		col := [3]uint8{255, 30, 20}
		if i < len(sh.Colors) {
			col = sh.Colors[i]
		}
		e1 := [3]float32{1, 0, 0}
		if cz := cross(n, [3]float32{0, 0, 1}); dot(cz, cz) > 1e-4 {
			e1 = norm(cz)
		}
		e2 := cross(e1, n)
		const seg = 12
		s := &nif.Shape{Name: fmt.Sprintf("MMWShiftLEDGlow:%d", i), Alpha: 1, Diffuse: glowTx, NormalTx: flatN, Specular: [3]float32{0.2, 0.2, 0.2}, Gloss: 10,
			EmissiveColor: [3]float32{float32(col[0]) / 255, float32(col[1]) / 255, float32(col[2]) / 255}, Emissive: 2.2}
		lift := scale(n, 0.05)
		for side := 0; side < 2; side++ {
			base := uint16(len(s.Pos))
			nn := n
			if side == 1 {
				nn = scale(n, -1)
			}
			s.Pos = append(s.Pos, lift)
			s.Normal = append(s.Normal, nn)
			s.UV = append(s.UV, [2]float32{0.5, 0.5})
			for k := 0; k < seg; k++ {
				a := 2 * math.Pi * float64(k) / seg
				q := add(lift, add(scale(e1, rad*float32(math.Cos(a))), scale(e2, rad*float32(math.Sin(a)))))
				s.Pos = append(s.Pos, q)
				s.Normal = append(s.Normal, nn)
				s.UV = append(s.UV, [2]float32{0.5 + 0.5*float32(math.Cos(a)), 0.5 + 0.5*float32(math.Sin(a))})
			}
			for k := 0; k < seg; k++ {
				a, b := base+1+uint16(k), base+1+uint16((k+1)%seg)
				if side == 0 {
					s.Tris = append(s.Tris, [3]uint16{base, a, b})
				} else {
					s.Tris = append(s.Tris, [3]uint16{base, b, a})
				}
			}
		}
		nodes = append(nodes, nif.NamedNode{Name: fmt.Sprintf("MMWShiftLED%d", i), At: pos, Shapes: []*nif.Shape{s}, Hidden: true})
		Logf("shift light %d: (%.2f %.2f %.2f) facing (%.2f %.2f %.2f), colour %v", i, pos[0], pos[1], pos[2], n[0], n[1], n[2], col)
	}
	return nodes
}

func add(a, b [3]float32) [3]float32 { return [3]float32{a[0] + b[0], a[1] + b[1], a[2] + b[2]} }

// ---- suspension links

type wheelInfo struct {
	name string
	c    [3]float32
	r    float32
	hw   float32
}

type suspLink struct {
	corner string
	kind   byte
	k      float32
	a, b   [3]float32
	h      [3]float32
	tris   map[*part][]int
}

// suspensionLinks marks the carriers (group -10-wheel) and links and returns the links and kingpins.
func suspensionLinks(parts []*part, wheels []wheelInfo) ([]*part, []suspLink, map[string][3]float32) {
	type comp struct {
		c          *swComp
		mn, mx, ce [3]float32
	}
	carrierOf := map[*swComp]int{}
	var links []suspLink
	linkComps := map[*swComp]int{}
	var all []comp
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" {
			continue
		}
		for _, c := range components(p) {
			mn := [3]float32{math.MaxFloat32, math.MaxFloat32, math.MaxFloat32}
			mx := [3]float32{-math.MaxFloat32, -math.MaxFloat32, -math.MaxFloat32}
			var ce [3]float32
			for _, vi := range c.verts {
				q := p.pos[vi]
				for k := 0; k < 3; k++ {
					mn[k], mx[k] = min(mn[k], q[k]), max(mx[k], q[k])
					ce[k] += q[k] / float32(len(c.verts))
				}
			}
			all = append(all, comp{c, mn, mx, ce})
		}
	}
	pins := map[string][3]float32{}
	for wi, w := range wheels {
		sx := float32(1)
		if w.c[0] < 0 {
			sx = -1
		}
		ax := abs32(w.c[0])
		xi := ax - w.hw
		front := w.name[0] == 'F'
		var hinges []float32
		var joints []float32
		type cand struct {
			cp       comp
			a, b     [3]float32
			h        [3]float32
			wishbone bool
		}
		var cands []cand
		for _, cp := range all {
			if _, used := carrierOf[cp.c]; used {
				continue
			}
			if _, used := linkComps[cp.c]; used {
				continue
			}
			if cp.ce[0]*sx <= 0 || abs32(cp.ce[1]-w.c[1]) > 1.3*w.r || cp.ce[2] > w.c[2]+w.r+20 {
				continue
			}
			inner, outer := abs32(cp.mn[0]), abs32(cp.mx[0])
			if inner > outer {
				inner, outer = outer, inner
			}
			if cp.mn[0] < 0 && cp.mx[0] > 0 {
				continue
			}
			ext := sub(cp.mx, cp.mn)
			maxE := max(ext[0], max(ext[1], ext[2]))
			minE := min(ext[0], min(ext[1], ext[2]))
			if inner > xi-9 && outer < ax+w.hw+3 && maxE < 2.4*w.r && abs32(cp.ce[1]-w.c[1]) < 0.75*w.r &&
				cp.mn[1] > w.c[1]-1.3*w.r && cp.mx[1] < w.c[1]+1.3*w.r && ext[0] < 2*w.hw+6 && cp.mx[2] < w.c[2]+w.r+8 {
				carrierOf[cp.c] = wi
				continue
			}
			if !(inner < xi-9 && outer > xi-7 && maxE < 3.2*w.r && minE < 0.4*maxE) {
				continue
			}
			p := cp.c.part
			var a, b, band [3]float32
			var na, nb, nband int
			var bandPts [][3]float32
			span := outer - inner
			for _, vi := range cp.c.verts {
				q := p.pos[vi]
				x := abs32(q[0])
				if x <= inner+2.5 {
					a = add(a, q)
					na++
				}
				if x >= outer-2.5 {
					b = add(b, q)
					nb++
				}
				if x <= inner+0.35*span {
					band = add(band, q)
					nband++
					bandPts = append(bandPts, q)
				}
			}
			if na == 0 || nb == 0 {
				continue
			}
			a, b = scale(a, 1/float32(na)), scale(b, 1/float32(nb))
			if abs32(b[1]-w.c[1]) > 0.8*w.r || abs32(b[2]-w.c[2]) > w.r || float32(math.Sqrt(float64(dist2(a, b)))) < 14 {
				continue
			}
			var ylo, yhi float32 = math.MaxFloat32, -math.MaxFloat32
			for _, q := range bandPts {
				ylo, yhi = min(ylo, q[1]), max(yhi, q[1])
			}
			cd := cand{cp: cp, a: a, b: b}
			if yhi-ylo > 8 {
				cd.wishbone = true
				bc := scale(band, 1/float32(nband))
				h := largestAxis(bandPts, bc)
				if h[1] < 0 {
					h = scale(h, -1)
				}
				cd.h, cd.a = h, bc
				hinges = append(hinges, abs32(bc[0]))
				joints = append(joints, abs32(b[0]))
			}
			cands = append(cands, cd)
		}
		hx := xi - 25
		if len(hinges) > 0 {
			hx = mean(hinges)
		}
		var pin [3]float32
		hasPin := false
		if len(joints) == 0 {
			for _, cd := range cands {
				if abs32(cd.b[0]) >= xi-4 {
					joints = append(joints, abs32(cd.b[0]))
				}
			}
		}
		if front && len(joints) > 0 {
			pin = [3]float32{sx * mean(joints), w.c[1], w.c[2]}
			hasPin = true
			pins[w.name] = pin
		}
		for _, cd := range cands {
			l := suspLink{corner: w.name, a: cd.a, b: cd.b, h: cd.h, tris: map[*part][]int{cd.cp.c.part: cd.cp.c.tris}}
			if cd.wishbone {
				l.kind, l.k = 'H', 1
			} else {
				l.kind = 'L'
				l.k = float32(math.Max(0.25, math.Min(1, float64((abs32(cd.b[0])-hx)/(xi-hx)))))
				if hasPin && abs32(cd.b[1]-w.c[1]) > 3.5 {
					l.kind = 'P'
				}
			}
			linkComps[cd.cp.c] = len(links)
			links = append(links, l)
		}
	}
	var out []*part
	for _, p := range parts {
		if p.group != -1 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" {
			out = append(out, p)
			continue
		}
		owner := make([]int, len(p.tris))
		any := false
		for _, cp := range all {
			if cp.c.part != p {
				continue
			}
			g := 0
			if wi, ok := carrierOf[cp.c]; ok {
				g = -10 - wi
			} else if li, ok := linkComps[cp.c]; ok {
				g = -100 - li
			}
			if g != 0 {
				any = true
				for _, ti := range cp.c.tris {
					owner[ti] = g
				}
			}
		}
		if !any {
			out = append(out, p)
			continue
		}
		by := map[int][][3]uint32{}
		var keys []int
		for ti, t := range p.tris {
			if _, ok := by[owner[ti]]; !ok {
				keys = append(keys, owner[ti])
			}
			by[owner[ti]] = append(by[owner[ti]], t)
		}
		sort.Ints(keys)
		for _, g := range keys {
			q := *p
			q.tris = by[g]
			if g != 0 {
				q.group = g
			}
			out = append(out, &q)
		}
	}
	nc := map[int]int{}
	for _, wi := range carrierOf {
		nc[wi]++
	}
	for wi, w := range wheels {
		nh, nl, np := 0, 0, 0
		for _, l := range links {
			if l.corner != w.name {
				continue
			}
			switch l.kind {
			case 'H':
				nh++
			case 'L':
				nl++
			case 'P':
				np++
			}
		}
		_, hp := pins[w.name]
		Logf("suspension %s: %d pieces ride with the wheel, %d wishbones, %d rods, %d track rods, kingpin %v", w.name, nc[wi], nh, nl, np, hp)
	}
	return out, links, pins
}

func mean(v []float32) float32 {
	var s float32
	for _, x := range v {
		s += x
	}
	return s / float32(max(1, len(v)))
}

// largestAxis: the direction the points spread most in (by power iteration on the covariance)
func largestAxis(pts [][3]float32, ce [3]float32) [3]float32 {
	var a [3][3]float64
	for _, q := range pts {
		d := [3]float64{float64(q[0] - ce[0]), float64(q[1] - ce[1]), float64(q[2] - ce[2])}
		for i := 0; i < 3; i++ {
			for j := 0; j < 3; j++ {
				a[i][j] += d[i] * d[j]
			}
		}
	}
	v := [3]float64{0.1, 1, 0.1}
	for it := 0; it < 60; it++ {
		var w [3]float64
		for i := 0; i < 3; i++ {
			w[i] = a[i][0]*v[0] + a[i][1]*v[1] + a[i][2]*v[2]
		}
		l := math.Sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2])
		if l < 1e-12 {
			break
		}
		v = [3]float64{w[0] / l, w[1] / l, w[2] / l}
	}
	return [3]float32{float32(v[0]), float32(v[1]), float32(v[2])}
}

// linkName encodes a link for the plugin (see the comment above suspLink).
func linkName(l suspLink) (string, [3]float32) {
	if l.kind == 'H' {
		o := sub(l.b, l.a)
		return fmt.Sprintf("MMWSusp %s H %.3f %.3f %.3f %.3f %.4f %.4f %.4f %.3f %.3f %.3f", l.corner, l.k, l.a[0], l.a[1], l.a[2], l.h[0], l.h[1], l.h[2], o[0], o[1], o[2]), l.a
	}
	v := sub(l.a, l.b)
	return fmt.Sprintf("MMWSusp %s %c %.3f %.3f %.3f %.3f %.3f %.3f %.3f", l.corner, l.kind, l.k, l.b[0], l.b[1], l.b[2], v[0], v[1], v[2]), l.b
}

// ---- dash gadgets

type DashGadgets struct {
	PipBoy *struct {
		At    [3]float32 `json:"at"`
		Rot   [9]float32 `json:"rot"`
		Scale float32    `json:"scale"`
	} `json:"pipboy,omitempty"`
	Speaker *SpeakerSpec `json:"speaker,omitempty"`
	Cable   *struct {
		Points [][3]float32 `json:"points"`
		Radius float32      `json:"radius"`
	} `json:"cable,omitempty"`
	Props []Prop     `json:"props,omitempty"`
	Shelf *ShelfSpec `json:"shelf,omitempty"`
	Stand *PipStand  `json:"stand,omitempty"`
}

// PipStand: a small dark-aluminium stand: a round foot on Base, a post up to Top, and a plate there (Plate across,
// tilted to Normal) the Pip-Boy rests on
type PipStand struct {
	Base   [3]float32 `json:"base"`
	Top    [3]float32 `json:"top"`
	Normal [3]float32 `json:"normal"`
	Plate  float32    `json:"plate"`
}

// Prop: one of the game's own models (its path under meshes\) on the dash, loaded by the plugin from the player's
// archives under "MMWProp r00.. r22 scale <model>" - nothing of it is in our files.
type Prop struct {
	Model string     `json:"model"`
	At    [3]float32 `json:"at"`
	Rot   [9]float32 `json:"rot"`
	Scale float32    `json:"scale"`
}

// ShelfSpec: a plain dash made here, a box from Min to Max in worn dark-painted metal (the game's wreck pickup has no
// dashboard: its cab opens straight from the firewall to the roof)
type ShelfSpec struct {
	Min [3]float32 `json:"min"`
	Max [3]float32 `json:"max"`
}

type SpeakerSpec struct {
	Centre [3]float32 `json:"centre"`
	Axis   [3]float32 `json:"axis"`
	Up     [3]float32 `json:"up,omitempty"`
	Radius float32    `json:"radius"`
	Length float32    `json:"length"`
	Socket int        `json:"socket,omitempty"`
	Stand  *struct {
		Gap float32 `json:"gap"`
	} `json:"stand,omitempty"`
}

func (sp *SpeakerSpec) frame() (ax, up, side [3]float32, sock float32) {
	ax = norm(sp.Axis)
	u := sp.Up
	if dot(u, u) < 1e-6 {
		u = [3]float32{0, 0, 1}
	}
	up = norm(sub(u, scale(ax, dot(u, ax))))
	side = cross(ax, up)
	sock = -1
	if sp.Socket > 0 {
		sock = 1
	}
	return
}

// Mesh is a shape under construction (positions relative to the node it goes in)
type mesh struct {
	pos, nrm [][3]float32
	uv       [][2]float32
	tris     [][3]uint16
}

func (m *mesh) vert(p, n [3]float32, uv [2]float32) uint16 {
	m.pos = append(m.pos, p)
	m.nrm = append(m.nrm, n)
	m.uv = append(m.uv, uv)
	return uint16(len(m.pos) - 1)
}

// tube: a ring of `sides` vertices at each point of the path, radius r (both ends left open)
func (m *mesh) tube(path [][3]float32, r float32, sides int) {
	if len(path) < 2 {
		return
	}
	var prevN [3]float32
	base := uint16(len(m.pos))
	for i, p := range path {
		var t [3]float32
		switch {
		case i == 0:
			t = sub(path[1], path[0])
		case i == len(path)-1:
			t = sub(path[i], path[i-1])
		default:
			t = sub(path[i+1], path[i-1])
		}
		t = norm(t)
		n := prevN
		if i == 0 || dot(n, n) < 0.5 {
			n = cross(t, [3]float32{0, 0, 1})
			if dot(n, n) < 1e-4 {
				n = cross(t, [3]float32{1, 0, 0})
			}
		}
		n = norm(sub(n, scale(t, dot(n, t))))
		prevN = n
		b := cross(t, n)
		for k := 0; k < sides; k++ {
			a := 2 * math.Pi * float64(k) / float64(sides)
			d := add(scale(n, float32(math.Cos(a))), scale(b, float32(math.Sin(a))))
			m.vert(add(p, scale(d, r)), d, [2]float32{float32(k) / float32(sides), float32(i) / float32(len(path)-1)})
		}
	}
	for i := 0; i+1 < len(path); i++ {
		for k := 0; k < sides; k++ {
			a := base + uint16(i*sides+k)
			b := base + uint16(i*sides+(k+1)%sides)
			c := a + uint16(sides)
			d := b + uint16(sides)
			m.tris = append(m.tris, [3]uint16{a, c, b}, [3]uint16{b, c, d})
		}
	}
}

// disc: a flat round cap at c facing n
func (m *mesh) disc(c, n [3]float32, r float32, sides int) {
	n = norm(n)
	e1 := cross(n, [3]float32{0, 0, 1})
	if dot(e1, e1) < 1e-4 {
		e1 = cross(n, [3]float32{1, 0, 0})
	}
	e1 = norm(e1)
	e2 := cross(n, e1)
	ci := m.vert(c, n, [2]float32{0.5, 0.5})
	first := uint16(len(m.pos))
	for k := 0; k < sides; k++ {
		a := 2 * math.Pi * float64(k) / float64(sides)
		d := add(scale(e1, float32(math.Cos(a))), scale(e2, float32(math.Sin(a))))
		m.vert(add(c, scale(d, r)), n, [2]float32{0.5 + 0.5*float32(math.Cos(a)), 0.5 + 0.5*float32(math.Sin(a))})
	}
	for k := 0; k < sides; k++ {
		m.tris = append(m.tris, [3]uint16{ci, first + uint16(k), first + uint16((k+1)%sides)})
	}
}

// shape: the mesh as a shape seen from both sides (each triangle again, turned round, with its normals flipped: the
// gadgets don't depend on which winding the game draws)
func (m *mesh) shape(name, tex, flatN string, spec [3]float32, gloss float32) *nif.Shape {
	s := &nif.Shape{Name: name, Alpha: 1, Diffuse: tex, NormalTx: flatN, Specular: spec, Gloss: gloss}
	n := uint16(len(m.pos))
	s.Pos = append(append(s.Pos, m.pos...), m.pos...)
	s.UV = append(append(s.UV, m.uv...), m.uv...)
	s.Normal = append(s.Normal, m.nrm...)
	for _, v := range m.nrm {
		s.Normal = append(s.Normal, scale(v, -1))
	}
	for _, t := range m.tris {
		s.Tris = append(s.Tris, t, [3]uint16{t[0] + n, t[2] + n, t[1] + n})
	}
	return s
}

// gadgetNodes builds the dash gadgets: the Pip-Boy's mount node and a node with the speaker and the cable.
func gadgetNodes(g *DashGadgets, tx map[string]string) []nif.NamedNode {
	var out []nif.NamedNode
	if pb := g.PipBoy; pb != nil {
		r := pb.Rot
		out = append(out, nif.NamedNode{Name: fmt.Sprintf("MMWPipBoy %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.4f",
			r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], pb.Scale), At: pb.At})
	}
	for _, pr := range g.Props {
		r := pr.Rot
		out = append(out, nif.NamedNode{Name: fmt.Sprintf("MMWProp %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.4f %s",
			r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], pr.Scale, strings.TrimPrefix(strings.ToLower(pr.Model), `meshes\`)), At: pr.At})
	}
	var shapes []*nif.Shape
	if sh := g.Shelf; sh != nil {
		shapes = append(shapes, shelfShape(sh, tx))
	}
	if st := g.Stand; st != nil {
		m := &mesh{}
		up := [3]float32{0, 0, 1}
		m.lathe(st.Base, up, [][2]float32{{0, 0}, {2.2, 0}, {2.2, 0.35}, {1.6, 0.6}, {0, 0.6}}, 20)
		m.tube([][3]float32{st.Base, st.Top}, 0.55, 12)
		n := norm(st.Normal)
		m.lathe(add(st.Top, scale(n, -0.45)), n, [][2]float32{{0, 0}, {st.Plate, 0}, {st.Plate, 0.45}, {0, 0.45}}, 24)
		shapes = append(shapes, m.shape("MMWPipStand", tx["stand"], tx["flatN"], [3]float32{0.55, 0.55, 0.6}, 40))
	}
	if sp := g.Speaker; sp != nil {
		shapes = append(shapes, speakerShapes(sp, tx)...)
	}
	if cb := g.Cable; cb != nil && len(cb.Points) >= 2 {
		wire, plugs := &mesh{}, &mesh{}
		p := cb.Points
		wire.tube(p, cb.Radius, 6)
		for _, end := range [][2][3]float32{{p[0], p[1]}, {p[len(p)-1], p[len(p)-2]}} {
			d := norm(sub(end[1], end[0]))
			plugs.tube([][3]float32{add(end[0], scale(d, -0.25)), add(end[0], scale(d, 0.55))}, cb.Radius*2, 10)
			plugs.disc(add(end[0], scale(d, -0.25)), scale(d, -1), cb.Radius*2, 10)
			plugs.tube([][3]float32{add(end[0], scale(d, 0.55)), add(end[0], scale(d, 1.1))}, cb.Radius*1.5, 8)
		}
		shapes = append(shapes, wire.shape("MMWAuxCable", tx["rubber"], tx["flatN"], [3]float32{0.25, 0.25, 0.25}, 30))
		shapes = append(shapes, plugs.shape("MMWAuxPlugs", tx["chrome"], tx["flatN"], [3]float32{0.9, 0.9, 0.9}, 80))
	}
	if len(shapes) > 0 {
		out = append(out, nif.NamedNode{Name: "MMWDashGadgets", Shapes: shapes})
	}
	return out
}

// lathe: a profile of (radius, offset along n) points turned round the axis through c pointing along n
func (m *mesh) lathe(c, n [3]float32, prof [][2]float32, sides int) {
	n = norm(n)
	e1 := cross(n, [3]float32{0, 0, 1})
	if dot(e1, e1) < 1e-4 {
		e1 = cross(n, [3]float32{1, 0, 0})
	}
	e1 = norm(e1)
	e2 := cross(n, e1)
	base := uint16(len(m.pos))
	for i, p := range prof {
		var t [2]float32
		switch {
		case i == 0:
			t = [2]float32{prof[1][0] - p[0], prof[1][1] - p[1]}
		case i == len(prof)-1:
			t = [2]float32{p[0] - prof[i-1][0], p[1] - prof[i-1][1]}
		default:
			t = [2]float32{prof[i+1][0] - prof[i-1][0], prof[i+1][1] - prof[i-1][1]}
		}
		nr, nz := t[1], -t[0]
		if nz < 0 {
			nr, nz = -nr, -nz
		}
		l := float32(math.Sqrt(float64(nr*nr + nz*nz)))
		if l > 0 {
			nr, nz = nr/l, nz/l
		}
		for k := 0; k < sides; k++ {
			a := 2 * math.Pi * float64(k) / float64(sides)
			d := add(scale(e1, float32(math.Cos(a))), scale(e2, float32(math.Sin(a))))
			pos := add(add(c, scale(d, p[0])), scale(n, p[1]))
			m.vert(pos, norm(add(scale(d, nr), scale(n, nz))), [2]float32{0.5 + 0.5*float32(math.Cos(a))*p[0]/prof[0][0], 0.5 + 0.5*float32(math.Sin(a))*p[0]/prof[0][0]})
		}
	}
	for i := 0; i+1 < len(prof); i++ {
		for k := 0; k < sides; k++ {
			a := base + uint16(i*sides+k)
			b := base + uint16(i*sides+(k+1)%sides)
			m.tris = append(m.tris, [3]uint16{a, a + uint16(sides), b}, [3]uint16{b, a + uint16(sides), b + uint16(sides)})
		}
	}
}

func speakerShapes(sp *SpeakerSpec, tx map[string]string) []*nif.Shape {
	c, r, length := sp.Centre, sp.Radius, sp.Length
	ax, up, side, sock := sp.frame()
	half := length / 2
	capW := float32(0.55)
	at := func(x, ang, rad float32) [3]float32 {
		d := add(scale(up, float32(math.Cos(float64(ang)))), scale(side, float32(math.Sin(float64(ang)))))
		return add(add(c, scale(ax, x)), scale(d, rad))
	}
	fabric, rubber, strip, radiator, metal, led := &mesh{}, &mesh{}, &mesh{}, &mesh{}, &mesh{}, &mesh{}
	var standShape *nif.Shape
	{
		const seg = 36
		x0, x1 := -half+capW, half-capW
		tilesRound := float32(8)
		tileLen := 2 * math.Pi * float64(r) / float64(tilesRound)
		base := uint16(len(fabric.pos))
		for i := 0; i <= 1; i++ {
			x := x0 + float32(i)*(x1-x0)
			for k := 0; k <= seg; k++ {
				ang := float32(2*math.Pi) * float32(k) / seg
				d := add(scale(up, float32(math.Cos(float64(ang)))), scale(side, float32(math.Sin(float64(ang)))))
				fabric.vert(add(add(c, scale(ax, x)), scale(d, r)), d, [2]float32{tilesRound * float32(k) / seg, (x - x0) / float32(tileLen)})
			}
		}
		for k := 0; k < seg; k++ {
			a, b := base+uint16(k), base+uint16(k+1)
			fabric.tris = append(fabric.tris, [3]uint16{a, a + seg + 1, b}, [3]uint16{b, a + seg + 1, b + seg + 1})
		}
	}
	for _, sd := range []float32{-1, 1} {
		n := scale(ax, sd)
		inner := add(c, scale(ax, sd*(half-capW)))
		rubber.lathe(inner, n, [][2]float32{{r * 0.99, 0}, {r * 1.05, 0.08}, {r * 1.05, capW - 0.2}, {r * 1.02, capW - 0.08}, {r * 0.95, capW - 0.01}, {r * 0.9, capW}}, 36)
		rubber.lathe(inner, n, [][2]float32{{r * 0.9, capW}, {r * 0.76, capW}, {r * 0.745, capW - 0.12}}, 36)
		radiator.lathe(inner, n, [][2]float32{{r * 0.745, capW - 0.12}, {r * 0.7, capW - 0.05}, {r * 0.64, capW - 0.1}, {r * 0.45, capW - 0.12}, {r * 0.22, capW - 0.1}, {r * 0.001, capW - 0.09}}, 36)
	}
	{
		const n = 10
		x0, x1 := -half*0.62, half*0.62
		a0, a1 := float32(-0.42), float32(0.42)
		top, bot := r+0.09, r-0.02
		base := uint16(len(strip.pos))
		for i := 0; i <= 1; i++ {
			x := x0 + float32(i)*(x1-x0)
			for k := 0; k <= n; k++ {
				ang := a0 + (a1-a0)*float32(k)/n
				d := norm(sub(at(x, ang, 1), add(c, scale(ax, x))))
				strip.vert(at(x, ang, top), d, [2]float32{float32(k) / n, 1 - float32(i)})
			}
		}
		for k := 0; k < n; k++ {
			a, b := base+uint16(k), base+uint16(k+1)
			strip.tris = append(strip.tris, [3]uint16{a, b, a + n + 1}, [3]uint16{b, b + n + 1, a + n + 1})
		}
		for _, ang := range []float32{a0, a1} {
			d := norm(sub(at(0, ang, 1), c))
			sideN := scale(cross(ax, d), float32(math.Copysign(1, float64(ang))))
			p := []uint16{strip.vert(at(x0, ang, top), sideN, [2]float32{0, 0}), strip.vert(at(x1, ang, top), sideN, [2]float32{0, 0}),
				strip.vert(at(x1, ang, bot), sideN, [2]float32{0, 0}), strip.vert(at(x0, ang, bot), sideN, [2]float32{0, 0})}
			strip.tris = append(strip.tris, [3]uint16{p[0], p[1], p[2]}, [3]uint16{p[0], p[2], p[3]})
		}
		for _, x := range []float32{x0, x1} {
			en := scale(ax, float32(math.Copysign(1, float64(x))))
			var ring []uint16
			for k := 0; k <= n; k++ {
				ang := a0 + (a1-a0)*float32(k)/n
				ring = append(ring, strip.vert(at(x, ang, top), en, [2]float32{0, 0}), strip.vert(at(x, ang, bot), en, [2]float32{0, 0}))
			}
			for k := 0; k < n; k++ {
				p := ring[2*k:]
				strip.tris = append(strip.tris, [3]uint16{p[0], p[2], p[1]}, [3]uint16{p[1], p[2], p[3]})
			}
		}
		led.disc(at(x0+0.35*(x1-x0)*0.12, 0, top+0.012), up, 0.07, 10)
	}
	{
		fx0, fx1 := -half*0.7, half*0.7
		w, h := float32(0.6), float32(0.12)
		bz := -r - h
		corner := func(x, y, z float32) [3]float32 { return add(add(add(c, scale(ax, x)), scale(side, y)), scale(up, z)) }
		box := [8][3]float32{corner(fx0, -w, bz), corner(fx1, -w, bz), corner(fx1, w, bz), corner(fx0, w, bz),
			corner(fx0, -w, -r+0.35), corner(fx1, -w, -r+0.35), corner(fx1, w, -r+0.35), corner(fx0, w, -r+0.35)}
		faces := [][4]int{{0, 1, 2, 3}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}}
		for _, f := range faces {
			nn := norm(cross(sub(box[f[1]], box[f[0]]), sub(box[f[2]], box[f[0]])))
			var q [4]uint16
			for k := range f {
				q[k] = rubber.vert(box[f[k]], nn, [2]float32{0, 0})
			}
			rubber.tris = append(rubber.tris, [3]uint16{q[0], q[1], q[2]}, [3]uint16{q[0], q[2], q[3]})
		}
	}
	for _, off := range []float32{-0.11, 0.11} {
		var path [][3]float32
		for k := 0; k <= 12; k++ {
			a := math.Pi/2 - math.Pi*float64(k)/12
			path = append(path, add(add(at(-sock*(half-0.12), 0, r*0.74), scale(side, off)), add(scale(ax, -sock*0.36*float32(math.Cos(a))), scale(up, 0.3*float32(math.Sin(a))))))
		}
		fabric.tube(path, 0.075, 6)
	}
	{
		p := sp.auxPort()
		metal.lathe(p, scale(ax, sock), [][2]float32{{0.17, 0}, {0.12, 0.01}}, 14)
		rubber.disc(add(p, scale(ax, -sock*0.02)), scale(ax, sock), 0.12, 14)
	}
	if st := sp.Stand; st != nil {
		gap := st.Gap
		if gap <= 0 {
			gap = 0.5
		}
		const thick = 0.14
		box := func(m *mesh, x0, x1, y0, y1, z0, z1 float32) {
			pt := func(x, y, z float32) [3]float32 { return add(add(add(c, scale(ax, x)), scale(side, y)), scale(up, z)) }
			v := [8][3]float32{pt(x0, y0, z0), pt(x1, y0, z0), pt(x1, y1, z0), pt(x0, y1, z0), pt(x0, y0, z1), pt(x1, y0, z1), pt(x1, y1, z1), pt(x0, y1, z1)}
			for _, f := range [][4]int{{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}} {
				nn := norm(cross(sub(v[f[1]], v[f[0]]), sub(v[f[2]], v[f[0]])))
				var q [4]uint16
				for k := range f {
					q[k] = m.vert(v[f[k]], nn, [2]float32{0, 0})
				}
				m.tris = append(m.tris, [3]uint16{q[0], q[1], q[2]}, [3]uint16{q[0], q[2], q[3]})
			}
		}
		stand := &mesh{}
		baseTop := -r - gap
		box(stand, -half*0.62, half*0.62, -r*0.8, r*0.8, baseTop-thick, baseTop)
		for _, x := range []float32{-half * 0.42, half * 0.42} {
			const n = 10
			w := float32(0.28)
			ri, ro := r+0.03, r+0.24
			ring := func(ang, rad, xx float32) [3]float32 { return at(xx, ang, rad) }
			base := uint16(len(stand.pos))
			for k := 0; k <= n; k++ {
				ang := float32(math.Pi) - float32(math.Pi/3) + float32(2*math.Pi/3)*float32(k)/n
				d := norm(sub(ring(ang, 1, 0), c))
				for _, rad := range []float32{ri, ro} {
					for _, xx := range []float32{x - w, x + w} {
						nn := d
						if rad == ri {
							nn = scale(d, -1)
						}
						stand.vert(ring(ang, rad, xx), nn, [2]float32{0, 0})
					}
				}
			}
			for k := 0; k < n; k++ {
				a := base + uint16(4*k)
				b := a + 4
				stand.tris = append(stand.tris,
					[3]uint16{a, a + 1, b + 1}, [3]uint16{a, b + 1, b},
					[3]uint16{a + 2, b + 2, b + 3}, [3]uint16{a + 2, b + 3, a + 3},
					[3]uint16{a, b, b + 2}, [3]uint16{a, b + 2, a + 2},
					[3]uint16{a + 1, a + 3, b + 3}, [3]uint16{a + 1, b + 3, b + 1})
			}
			for _, e := range []uint16{base, base + uint16(4*n)} {
				stand.tris = append(stand.tris, [3]uint16{e, e + 2, e + 3}, [3]uint16{e, e + 3, e + 1})
			}
			box(stand, x-w*0.8, x+w*0.8, -0.45, 0.45, baseTop, -ro+0.05)
		}
		standTx := tx["stand"]
		if standTx == "" {
			standTx = tx["rubber"]
		}
		standShape = stand.shape("MMWSpeakerStand", standTx, tx["flatN"], [3]float32{0.5, 0.5, 0.55}, 35)
	}
	shapes := []*nif.Shape{
		fabric.shape("MMWSpeakerGrille", tx["fabric"], tx["flatN"], [3]float32{0.06, 0.06, 0.07}, 6),
		rubber.shape("MMWSpeakerRubber", tx["rubber"], tx["flatN"], [3]float32{0.18, 0.18, 0.2}, 22),
		radiator.shape("MMWSpeakerRadiator", tx["radiator"], tx["flatN"], [3]float32{0.35, 0.35, 0.38}, 45),
		strip.shape("MMWSpeakerButtons", tx["buttons"], tx["flatN"], [3]float32{0.2, 0.2, 0.22}, 25),
		metal.shape("MMWSpeakerSocket", tx["chrome"], tx["flatN"], [3]float32{0.9, 0.9, 0.9}, 80),
	}
	l := led.shape("MMWSpeakerLight", tx["light"], tx["flatN"], [3]float32{0.2, 0.2, 0.2}, 10)
	l.EmissiveColor, l.Emissive = [3]float32{0.2, 0.55, 1}, 2.2
	shapes = append(shapes, l)
	if standShape != nil {
		shapes = append(shapes, standShape)
	}
	return shapes
}

// auxPort: where the speaker's aux socket is (the cable's end), as speakerShapes puts it
func (sp *SpeakerSpec) auxPort() [3]float32 {
	ax, up, side, sock := sp.frame()
	dir := norm(add(scale(side, 0.7), scale(up, -0.7)))
	return add(add(sp.Centre, scale(ax, sock*(sp.Length/2+0.005))), scale(dir, sp.Radius*0.83))
}

// speakerTextures: the grille's weave (tiles), the rubber, the radiators and the button strip's moulded icons, as RGBA
// for dds.DXT5Mips
func speakerTextures() map[string][]byte {
	out := map[string][]byte{}
	noise := uint32(0x9E3779B9)
	rnd := func() int {
		noise = noise*1664525 + 1013904223
		return int(noise>>24) - 128
	}
	clamp8 := func(v int) byte {
		if v < 0 {
			return 0
		}
		if v > 255 {
			return 255
		}
		return byte(v)
	}
	{
		const n = 128
		px := make([]byte, n*n*4)
		for y := 0; y < n; y++ {
			for x := 0; x < n; x++ {
				cx, cy := x%8, y%8
				v := 44
				over := ((x/8)+(y/8))%2 == 0
				if over {
					v += 8 - 2*abs(cx-3)
				} else {
					v += 8 - 2*abs(cy-3)
				}
				if cx == 7 || cy == 7 {
					v = 22
				}
				v += rnd() / 32
				i := 4 * (y*n + x)
				px[i], px[i+1], px[i+2], px[i+3] = clamp8(v-2), clamp8(v), clamp8(v+4), 255
			}
		}
		out["fabric"] = px
	}
	{
		const n = 32
		px := make([]byte, n*n*4)
		for i := 0; i < n*n; i++ {
			v := 26 + rnd()/48
			px[4*i], px[4*i+1], px[4*i+2], px[4*i+3] = clamp8(v), clamp8(v), clamp8(v+2), 255
		}
		out["rubber"] = px
		rad := make([]byte, n*n*4)
		for y := 0; y < n; y++ {
			for x := 0; x < n; x++ {
				dx, dy := float64(x)-15.5, float64(y)-15.5
				d := math.Sqrt(dx*dx + dy*dy)
				v := 20 + int(6*math.Cos(d*1.3)) + rnd()/64
				i := 4 * (y*n + x)
				rad[i], rad[i+1], rad[i+2], rad[i+3] = clamp8(v), clamp8(v), clamp8(v+2), 255
			}
		}
		out["radiator"] = rad
	}
	{
		const w, h = 128, 512
		px := make([]byte, w*h*4)
		set := func(x, y, v int) {
			if x < 0 || y < 0 || x >= w || y >= h {
				return
			}
			i := 4 * (y*w + x)
			px[i], px[i+1], px[i+2], px[i+3] = clamp8(v), clamp8(v), clamp8(v+3), 255
		}
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				set(x, y, 30+rnd()/48)
			}
		}
		icon := func(cy int, kind string) {
			cx := w / 2
			for y := cy - 32; y <= cy+32; y++ {
				for x := cx - 32; x <= cx+32; x++ {
					d := math.Hypot(float64(x-cx), float64(y-cy))
					if d <= 26 {
						set(x, y, 38+int(5*(1-d/26)))
					} else if d <= 30 {
						set(x, y, 18)
					}
				}
			}
			ink := 92
			line := func(x0, y0, x1, y1 float64) {
				x0, y0, x1, y1 = 2*x0, 2*y0, 2*x1, 2*y1
				steps := int(math.Max(math.Abs(x1-x0), math.Abs(y1-y0))*2) + 1
				for s := 0; s <= steps; s++ {
					t := float64(s) / float64(steps)
					x, y := x0+(x1-x0)*t, y0+(y1-y0)*t
					for oy := -1; oy <= 1; oy++ {
						for ox := -1; ox <= 1; ox++ {
							set(cx+int(math.Round(y))+ox, cy-int(math.Round(x))+oy, ink)
						}
					}
				}
			}
			arc := func(rad, from, to float64) {
				for a := from; a <= to; a += 0.05 {
					line(rad*math.Cos(a), rad*math.Sin(a), rad*math.Cos(a+0.05), rad*math.Sin(a+0.05))
				}
			}
			switch kind {
			case "power":
				arc(6.5, -math.Pi/2+0.9, 3*math.Pi/2-0.9)
				line(0, -8, 0, -1)
			case "pair":
				arc(3.5, 0, 2*math.Pi-0.05)
				for a := 0.0; a < 2*math.Pi; a += 0.05 {
					line(5+3.5*math.Cos(a)-2.5, 3.5*math.Sin(a)-2.5, 5+3.5*math.Cos(a+0.05)-2.5, 3.5*math.Sin(a+0.05)-2.5)
				}
			case "minus":
				line(-6, 0, 6, 0)
			case "plus":
				line(-6, 0, 6, 0)
				line(0, -6, 0, 6)
			case "play":
				line(-5, -6, -5, 6)
				line(-5, -6, 4, 0)
				line(-5, 6, 4, 0)
			}
		}
		icon(h-56, "power")
		icon(h-56-100, "pair")
		icon(h-56-200, "minus")
		icon(h-56-300, "play")
		icon(h-56-400, "plus")
		out["buttons"] = px
	}
	return out
}

// shelfShape: the made dash, a box with its texture laid on each face at 8 units to the repeat
func shelfShape(sh *ShelfSpec, tx map[string]string) *nif.Shape {
	m := &mesh{}
	a, b := sh.Min, sh.Max
	face := func(o, u, v [3]float32, n [3]float32) {
		lu, lv := float32(math.Sqrt(float64(dot(u, u)))), float32(math.Sqrt(float64(dot(v, v))))
		i0 := m.vert(o, n, [2]float32{0, 0})
		i1 := m.vert(add(o, u), n, [2]float32{lu / 8, 0})
		i2 := m.vert(add(add(o, u), v), n, [2]float32{lu / 8, lv / 8})
		i3 := m.vert(add(o, v), n, [2]float32{0, lv / 8})
		m.tris = append(m.tris, [3]uint16{i0, i1, i2}, [3]uint16{i0, i2, i3})
	}
	dx, dy, dz := [3]float32{b[0] - a[0], 0, 0}, [3]float32{0, b[1] - a[1], 0}, [3]float32{0, 0, b[2] - a[2]}
	face([3]float32{a[0], a[1], b[2]}, dx, dy, [3]float32{0, 0, 1})
	face(a, dx, dz, [3]float32{0, -1, 0})
	face([3]float32{a[0], b[1], a[2]}, dx, dz, [3]float32{0, 1, 0})
	face(a, dx, dy, [3]float32{0, 0, -1})
	face(a, dy, dz, [3]float32{-1, 0, 0})
	face([3]float32{b[0], a[1], a[2]}, dy, dz, [3]float32{1, 0, 0})
	return m.shape("MMWDashShelf", tx["dash"], tx["flatN"], [3]float32{0.2, 0.2, 0.2}, 18)
}

// dashMetal: worn dark olive paint on steel - a faint mottle, fine horizontal brush streaks, scratches through to bare
// metal and rust freckles at random (made here, no game's texture)
func dashMetal(n int) []byte {
	clamp8 := func(v int) byte { return byte(min(max(v, 0), 255)) }
	seed := uint32(0x6D2B79F5)
	rnd := func() int { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return int(seed % 256) }
	px := make([]byte, n*n*4)
	for y := 0; y < n; y++ {
		row := rnd()/40 - 3
		for x := 0; x < n; x++ {
			mott := 4 * math.Sin(float64(x)*0.09+float64(y)*0.05) * math.Cos(float64(y)*0.11-float64(x)*0.03)
			v := 46 + int(mott) + row + rnd()/48
			i := 4 * (y*n + x)
			px[i], px[i+1], px[i+2], px[i+3] = clamp8(v-6), clamp8(v+2), clamp8(v-1), 255
		}
	}
	for k := 0; k < n/6; k++ {
		x0, y0, l := rnd()*n/256, rnd()*n/256, 4+rnd()/24
		for t := 0; t < l; t++ {
			x, y := (x0+t)%n, (y0+t/3)%n
			i := 4 * (y*n + x)
			px[i], px[i+1], px[i+2] = 120, 122, 124
		}
	}
	for k := 0; k < n/3; k++ {
		x, y := rnd()*n/256, rnd()*n/256
		i := 4 * (y*n + x)
		px[i], px[i+1], px[i+2] = 24, 48, 92
	}
	return px
}

// DashNodes: the dash gadgets for a car made outside this package (the launcher's game vehicles): their textures
// written under dataDir\textures\mmw\gadgets\ (and the Pip-Boy's screen), their nodes returned
func DashNodes(g *DashGadgets, dataDir string) ([]nif.NamedNode, error) {
	rel := filepath.Join("textures", "mmw", "gadgets")
	if err := os.MkdirAll(filepath.Join(dataDir, rel), 0o755); err != nil {
		return nil, err
	}
	writeTex := func(name string, data []byte) string {
		os.WriteFile(filepath.Join(dataDir, rel, name), data, 0o644)
		return strings.ReplaceAll(filepath.Join(rel, name), "/", `\`)
	}
	st := speakerTextures()
	tx := map[string]string{"chrome": writeTex("chrome.dds", dds.Solid(170, 170, 175, 255)), "flatN": writeTex("flat_n.dds", dds.FlatNormal())}
	tx["rubber"] = writeTex("rubber.dds", dds.DXT5Mips(st["rubber"], 32, 32))
	tx["dash"] = writeTex("dash_metal.dds", dds.DXT5Mips(dashMetal(128), 128, 128))
	tx["stand"] = writeTex("speaker_stand.dds", dds.Solid(62, 64, 68, 255))
	if g.Speaker != nil {
		for name, size := range map[string]int{"fabric": 128, "radiator": 32} {
			tx[name] = writeTex("speaker_"+name+".dds", dds.DXT5Mips(st[name], size, size))
		}
		tx["buttons"] = writeTex("speaker_buttons.dds", dds.DXT5Mips(st["buttons"], 128, 512))
	}
	if g.PipBoy != nil {
		writePipScreen(dataDir)
	}
	return gadgetNodes(g, tx), nil
}

// writePipScreen: the dash Pip-Boy's screen texture the plugin draws on, dark, with its full mip chain
func writePipScreen(dataDir string) {
	pipPath := filepath.Join(dataDir, filepath.FromSlash(strings.ReplaceAll(PipPath, `\`, "/")))
	px := make([]byte, PipW*PipW*4)
	for i := 0; i < len(px); i += 4 {
		px[i], px[i+1], px[i+2], px[i+3] = 4, 10, 6, 255
	}
	os.MkdirAll(filepath.Dir(pipPath), 0o755)
	os.WriteFile(pipPath, dds.ImageMips(PipW, PipW, px), 0o644)
}
