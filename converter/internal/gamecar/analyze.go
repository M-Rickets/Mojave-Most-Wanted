// Package gamecar finds the parts of one of New Vegas's own vehicle models (a truck, a motorcycle) that a drivable
// version needs moving on their own
package gamecar

import (
	"fmt"
	"math"
	"math/bits"
	"sort"

	"mmw/internal/fnvnif"
)

// Piece is one connected part of a mesh.
type Piece struct {
	Mesh  int
	Tris  []int
	Min   [3]float32
	Max   [3]float32
	Verts int
}

func (p *Piece) Size(i int) float32   { return p.Max[i] - p.Min[i] }
func (p *Piece) Centre(i int) float32 { return (p.Max[i] + p.Min[i]) / 2 }

// Pieces breaks every mesh into connected pieces.
func Pieces(ms []*fnvnif.Mesh) []*Piece {
	var out []*Piece
	for mi, m := range ms {
		key := func(p [3]float32) [3]int32 {
			return [3]int32{int32(math.Round(float64(p[0]) * 20)), int32(math.Round(float64(p[1]) * 20)), int32(math.Round(float64(p[2]) * 20))}
		}
		ids := map[[3]int32]int{}
		weld := make([]int, len(m.Pos))
		for i, p := range m.Pos {
			k := key(p)
			id, ok := ids[k]
			if !ok {
				id = len(ids)
				ids[k] = id
			}
			weld[i] = id
		}
		par := make([]int, len(ids))
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
		for _, t := range m.Tris {
			a, b, c := find(weld[t[0]]), find(weld[t[1]]), find(weld[t[2]])
			par[b] = a
			par[find(c)] = a
		}
		byRoot := map[int]*Piece{}
		var order []int
		for ti, t := range m.Tris {
			r := find(weld[t[0]])
			p := byRoot[r]
			if p == nil {
				p = &Piece{Mesh: mi, Min: [3]float32{1e9, 1e9, 1e9}, Max: [3]float32{-1e9, -1e9, -1e9}}
				byRoot[r] = p
				order = append(order, r)
			}
			p.Tris = append(p.Tris, ti)
			for _, v := range t {
				q := m.Pos[v]
				for i := 0; i < 3; i++ {
					if q[i] < p.Min[i] {
						p.Min[i] = q[i]
					}
					if q[i] > p.Max[i] {
						p.Max[i] = q[i]
					}
				}
			}
		}
		for _, r := range order {
			p := byRoot[r]
			seen := map[uint16]bool{}
			for _, ti := range p.Tris {
				for _, v := range m.Tris[ti] {
					seen[v] = true
				}
			}
			p.Verts = len(seen)
			out = append(out, p)
		}
	}
	return out
}

// Wheel is one wheel found: its centre, radius and width, the pieces it's made of.
type Wheel struct {
	At     [3]float32
	Radius float32
	Width  float32
	Pieces []int
	Name   string
}

// Layout is what was found in a model.
type Layout struct {
	Fwd, Side int
	FwdSign   float32
	Min, Max  [3]float32
	Wheels    []Wheel
	Pieces    []*Piece
	Notes     []string
	Owner     map[[2]int]int
}

func (l *Layout) note(f string, a ...any) { l.Notes = append(l.Notes, fmt.Sprintf(f, a...)) }

// Find works out the layout of a vehicle model. bike: a motorcycle (two wheels on the centre line). front: where the
// model's front is when it's known
func Find(ms []*fnvnif.Mesh, bike bool, front ...string) *Layout {
	l := &Layout{Min: [3]float32{1e9, 1e9, 1e9}, Max: [3]float32{-1e9, -1e9, -1e9}}
	l.Pieces = Pieces(ms)
	for _, p := range l.Pieces {
		for i := 0; i < 3; i++ {
			l.Min[i] = min(l.Min[i], p.Min[i])
			l.Max[i] = max(l.Max[i], p.Max[i])
		}
	}
	if l.Max[1]-l.Min[1] > l.Max[0]-l.Min[0] {
		l.Fwd, l.Side = 1, 0
	} else {
		l.Fwd, l.Side = 0, 1
	}
	ground := l.Min[2]
	length := l.Max[l.Fwd] - l.Min[l.Fwd]
	type cand struct {
		i    int
		d, w float32
	}
	var cs []cand
	for i, p := range l.Pieces {
		h, f, s := p.Size(2), p.Size(l.Fwd), p.Size(l.Side)
		if h < 8 || h > 0.45*length || f < 0.75*h || f > 1.3*h || s > 0.9*h {
			continue
		}
		if p.Min[2]-ground > 0.6*h+2 {
			continue
		}
		cs = append(cs, cand{i, (h + f) / 2, s})
	}
	used := make([]bool, len(cs))
	sort.Slice(cs, func(a, b int) bool { return cs[a].d > cs[b].d })
	for a := range cs {
		if used[a] {
			continue
		}
		pa := l.Pieces[cs[a].i]
		w := Wheel{Radius: cs[a].d / 2, Pieces: []int{cs[a].i}}
		used[a] = true
		mn, mx := pa.Min, pa.Max
		for b := a + 1; b < len(cs); b++ {
			if used[b] {
				continue
			}
			pb := l.Pieces[cs[b].i]
			close := true
			for k := 0; k < 3; k++ {
				if math.Abs(float64(pb.Centre(k)-pa.Centre(k))) > float64(w.Radius) {
					close = false
				}
			}
			if close {
				used[b] = true
				w.Pieces = append(w.Pieces, cs[b].i)
				for k := 0; k < 3; k++ {
					mn[k] = min(mn[k], pb.Min[k])
					mx[k] = max(mx[k], pb.Max[k])
				}
			}
		}
		for i, p := range l.Pieces {
			if containsInt(w.Pieces, i) {
				continue
			}
			in := true
			for k := 0; k < 3; k++ {
				if p.Min[k] < mn[k]-1 || p.Max[k] > mx[k]+1 {
					in = false
				}
			}
			if in {
				w.Pieces = append(w.Pieces, i)
			}
		}
		for k := 0; k < 3; k++ {
			w.At[k] = (mn[k] + mx[k]) / 2
		}
		w.Width = mx[l.Side] - mn[l.Side]
		if mn[2]-ground > 0.25*(mx[2]-mn[2])+2 && !bike {
			l.tyreAround(ms, &w)
			if w.At[2]-w.Radius-ground > 3 {
				continue
			}
		}
		l.Wheels = append(l.Wheels, w)
	}
	mid := (l.Min[l.Fwd] + l.Max[l.Fwd]) / 2
	nPlus, nMinus := 0, 0
	for _, w := range l.Wheels {
		if w.At[l.Fwd] > mid {
			nPlus++
		} else {
			nMinus++
		}
	}
	l.FwdSign = 1
	if nPlus > nMinus {
		l.FwdSign = -1
	}
	if len(front) > 0 && len(front[0]) == 2 {
		if (front[0][1] == 'x') != (l.Fwd == 0) {
			l.note("front %s is across the model's long axis - ignored", front[0])
		} else if front[0][0] == '-' {
			l.FwdSign = -1
		} else {
			l.FwdSign = 1
		}
	}
	if bike && len(l.Wheels) == 1 {
		w0 := l.Wheels[0]
		mn, mx := float32(1e9), float32(-1e9)
		for _, p := range l.Pieces {
			if p.Min[2]-ground > 0.2*w0.Radius || (p.Centre(l.Fwd)-mid)*(w0.At[l.Fwd]-mid) > 0 {
				continue
			}
			mn, mx = min(mn, p.Min[l.Fwd]), max(mx, p.Max[l.Fwd])
		}
		if mx > mn {
			w := Wheel{Radius: w0.Radius, Width: w0.Width}
			w.At = w0.At
			w.At[l.Fwd] = (mn + mx) / 2
			w.At[2] = ground + w0.Radius
			l.Wheels = append(l.Wheels, w)
			l.note("the other wheel taken from the low pieces at the far end (%.0f..%.0f)", mn, mx)
		}
	}
	sideMid := (l.Min[l.Side] + l.Max[l.Side]) / 2
	for i := range l.Wheels {
		w := &l.Wheels[i]
		front := (w.At[l.Fwd]-mid)*l.FwdSign > 0
		if bike {
			if front {
				w.Name = "F"
			} else {
				w.Name = "R"
			}
			continue
		}
		s := "L"
		if w.At[l.Side] > sideMid {
			s = "R"
		}
		if front {
			w.Name = "F" + s
		} else {
			w.Name = "R" + s
		}
	}
	l.claim(ms, bike)
	return l
}

func (l *Layout) claim(ms []*fnvnif.Mesh, bike bool) {
	l.Owner = map[[2]int]int{}
	if !bike {
		for i := range l.Wheels {
			l.tyreAround(ms, &l.Wheels[i])
		}
	}
	for wi, w := range l.Wheels {
		for _, pi := range w.Pieces {
			p := l.Pieces[pi]
			for _, t := range p.Tris {
				l.Owner[[2]int{p.Mesh, t}] = wi
			}
		}
	}
	sideMid := (l.Min[l.Side] + l.Max[l.Side]) / 2
	for mi, m := range ms {
		for ti, t := range m.Tris {
			if _, ok := l.Owner[[2]int{mi, ti}]; ok {
				continue
			}
			for wi, w := range l.Wheels {
				r := w.Radius * 1.06
				lo, hi := w.At[l.Side]-r*0.45, w.At[l.Side]+r*0.45
				if !bike {
					out := float32(1)
					if w.At[l.Side] < sideMid {
						out = -1
					}
					face := w.At[l.Side] + out*w.Width/2
					lo, hi = min(face-out*r*0.8, face+out*r*0.15), max(face-out*r*0.8, face+out*r*0.15)
				}
				in := true
				for _, v := range t {
					q := m.Pos[v]
					df, dz := q[l.Fwd]-w.At[l.Fwd], q[2]-w.At[2]
					if df*df+dz*dz > r*r || q[l.Side] < lo || q[l.Side] > hi {
						in = false
						break
					}
				}
				if in {
					l.Owner[[2]int{mi, ti}] = wi
					break
				}
			}
		}
	}
}

func containsInt(a []int, x int) bool {
	for _, v := range a {
		if v == x {
			return true
		}
	}
	return false
}

func (l *Layout) tyreAround(ms []*fnvnif.Mesh, w *Wheel) {
	sideMid := (l.Min[l.Side] + l.Max[l.Side]) / 2
	out := float32(1)
	if w.At[l.Side] < sideMid {
		out = -1
	}
	face := w.At[l.Side] + out*w.Width/2
	lo, hi := min(face-out*w.Radius*1.3, face+out*w.Radius*0.15), max(face-out*w.Radius*1.3, face+out*w.Radius*0.15)
	const sectors = 18
	n := int(w.Radius*1.2) + 2
	cover := make([]uint32, n)
	for _, m := range ms {
		for _, q := range m.Pos {
			if q[l.Side] < lo || q[l.Side] > hi {
				continue
			}
			df, dz := float64(q[l.Fwd]-w.At[l.Fwd]), float64(q[2]-w.At[2])
			d := float32(math.Sqrt(df*df + dz*dz))
			i := int(d - w.Radius)
			if i < 0 || i >= n {
				continue
			}
			a := int((math.Atan2(dz, df) + math.Pi) / (2 * math.Pi) * sectors)
			if a >= sectors {
				a = sectors - 1
			}
			cover[i] |= 1 << a
			if i > 0 {
				cover[i-1] |= 1 << a
			}
		}
	}
	best := -1
	for i := range cover {
		if bits.OnesCount32(cover[i]) >= 11 {
			best = i
		}
	}
	if best > 1 {
		r := w.Radius + float32(best) + 1
		l.note("wheel at %.0f %.0f: a tyre round it, r %.1f (found %.1f)", w.At[l.Fwd], w.At[l.Side], r, w.Radius)
		w.Radius = r
	}
}
