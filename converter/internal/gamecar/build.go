package gamecar

import (
	"errors"
	"fmt"
	"math"
	"sort"

	"mmw/internal/fnvnif"
	"mmw/internal/nif"
)

// UnitsPerMetre as the car converter has it (1 unit = 0.0142875 m)
const UnitsPerMetre = 70.0

// Built is what the plugin needs of a driving model (game units unless said; origin on the ground midway between the
// axles and the sides, x right, y forward, z up - as the car converter's)
type Built struct {
	HalfTrack, HalfBase, HalfLength, HalfWidth float32
	Front, Rear, Clearance                     float32
	WheelRadiusF, WheelRadiusR                 float32
	Wheels                                     []string
	BodyTris, WheelTris                        int
	Origin                                     [3]float32
	ModelYaw                                   float32
	Cab                                        bool
	Eye                                        [3]float32
	Hood                                       [2]float32
	WheelNotes                                 []string
	Steer                                      bool
	SteerAxis                                  [3]float32
	SteerAt                                    [3]float32

	wheelShapes [][]*nif.Shape
}

// frame: the model's axes into the car's
type frame struct {
	o      [3]float32
	F, R   [3]float32
	ground float32
}

func (f *frame) pt(p [3]float32) [3]float32 {
	d := [3]float32{p[0] - f.o[0], p[1] - f.o[1], 0}
	return [3]float32{d[0]*f.R[0] + d[1]*f.R[1], d[0]*f.F[0] + d[1]*f.F[1], p[2] - f.ground}
}
func (f *frame) dir(p [3]float32) [3]float32 {
	return [3]float32{p[0]*f.R[0] + p[1]*f.R[1], p[0]*f.F[0] + p[1]*f.F[1], p[2]}
}

// Donor is another model's wheels put on this one (the pickups outside Goodsprings stand on bare hubs): a left and a
// right wheel's shapes about their centres (the car's frame), and their radius
type Donor struct {
	Left, Right []*nif.Shape
	Radius      float32
	Notes       []string
	Want        float32
	Steer       *Steer
	Raise       float32
	Extra       []nif.NamedNode
	ColumnTx    string
}

// DonorFrom takes a model's best pair of wheels as a donor: of each axle's left and right, the pair whose tyres are the
// roundest, the best centred on their hubs and the least tilted (wreck models bend and flatten some).
func DonorFrom(ms []*fnvnif.Mesh, l *Layout) (*Donor, error) {
	_, b, err := build(ms, l, false, true)
	if err != nil {
		return nil, err
	}
	d := &Donor{}
	score := map[string]float64{}
	type true_ struct{ r, cy, cz float64 }
	shape := map[string]true_{}
	for i, w := range b.Wheels {
		if i < len(b.wheelShapes) {
			f, r, cy, cz := wheelFault(b.wheelShapes[i])
			score[w], shape[w] = f, true_{r, cy, cz}
		}
	}
	best := ""
	for _, w := range b.Wheels {
		if w[1] != 'L' {
			continue
		}
		rw := string(w[0]) + "R" + w[2:]
		sl, okl := score[w]
		sr, okr := score[rw]
		if !okl || !okr {
			continue
		}
		if best == "" || sl+sr < score[best]+score[string(best[0])+"R"+best[2:]] {
			best = w
		}
	}
	for _, w := range b.Wheels {
		d.Notes = append(d.Notes, fmt.Sprintf("its %s: out of true %.3f", w, score[w]))
	}
	if best == "" {
		return nil, fmt.Errorf("no pair of wheels to give (%v)", b.Wheels)
	}
	right := string(best[0]) + "R" + best[2:]
	centred := func(ss []*nif.Shape, t true_) []*nif.Shape {
		var out []*nif.Shape
		for _, s := range ss {
			c := *s
			c.Pos = make([][3]float32, len(s.Pos))
			for j, p := range s.Pos {
				c.Pos[j] = [3]float32{p[0], p[1] - float32(t.cy), p[2] - float32(t.cz)}
			}
			out = append(out, &c)
		}
		return out
	}
	for i, w := range b.Wheels {
		if w == best {
			d.Left = centred(b.wheelShapes[i], shape[w])
		}
		if w == right {
			d.Right = centred(b.wheelShapes[i], shape[w])
		}
	}
	d.Radius = float32((shape[best].r + shape[right].r) / 2)
	d.Notes = append(d.Notes, fmt.Sprintf("its %s and %s taken (radius %.1f)", best, right, d.Radius))
	if d.Left == nil || d.Right == nil || d.Radius < 1 {
		return nil, fmt.Errorf("no wheels to give (%v)", b.Wheels)
	}
	return d, nil
}

func wheelFault(shapes []*nif.Shape) (fault, radius, cy, cz float64) {
	const n = 18
	var rr [n]float64
	var xx [n]float64
	mark := func(p [3]float32) {
		r := math.Hypot(float64(p[1]), float64(p[2]))
		a := int((math.Atan2(float64(p[2]), float64(p[1])) + math.Pi) / (2 * math.Pi) * n)
		if a >= n {
			a = n - 1
		}
		if r > rr[a] {
			rr[a], xx[a] = r, float64(p[0])
		}
	}
	for _, s := range shapes {
		for _, t := range s.Tris {
			for e := 0; e < 3; e++ {
				p, q := s.Pos[t[e]], s.Pos[t[(e+1)%3]]
				for j := 0; j <= 8; j++ {
					f := float32(j) / 8
					mark([3]float32{p[0] + (q[0]-p[0])*f, p[1] + (q[1]-p[1])*f, p[2] + (q[2]-p[2])*f})
				}
			}
		}
	}
	var mx float64
	for a := 0; a < n; a++ {
		th := (float64(a)+0.5)/n*2*math.Pi - math.Pi
		radius += rr[a] / n
		cy += 2 * rr[a] * math.Cos(th) / n
		cz += 2 * rr[a] * math.Sin(th) / n
		mx += xx[a] / n
	}
	mr := radius
	if mr == 0 {
		return 9, 0, 0, 0
	}
	var vr, vx float64
	for a := 0; a < n; a++ {
		vr += (rr[a] - mr) * (rr[a] - mr) / n
		vx += (xx[a] - mx) * (xx[a] - mx) / n
	}
	return (math.Sqrt(vr) + math.Hypot(cy, cz) + math.Sqrt(vx)) / mr, mr, cy, cz
}

// Build makes the driving model: the body, and each wheel under MMWSteer<name>/MMWWheel<name> at its centre - or a
// donor's wheels in their place, sized to each.
func Build(ms []*fnvnif.Mesh, l *Layout, bike bool, donor ...*Donor) (*nif.Writer, Built, error) {
	return build(ms, l, bike, false, donor...)
}

// build: forDonor - only its wheels are wanted: two will do, on one axle (a trailer's)
func build(ms []*fnvnif.Mesh, l *Layout, bike, forDonor bool, donor ...*Donor) (*nif.Writer, Built, error) {
	var b Built
	if len(l.Wheels) < 2 || (!bike && !forDonor && len(l.Wheels) < 4) {
		return nil, b, fmt.Errorf("%d wheels found", len(l.Wheels))
	}
	var fr frame
	fr.F[l.Fwd] = l.FwdSign
	fr.R = [3]float32{fr.F[1], -fr.F[0], 0}
	fr.ground = l.Min[2]
	var front, rear []float32
	var sideSum float32
	for _, w := range l.Wheels {
		if w.Name[0] == 'F' {
			front = append(front, w.At[l.Fwd])
		} else {
			rear = append(rear, w.At[l.Fwd])
		}
		sideSum += w.At[l.Side]
	}
	if forDonor && len(front) == 0 {
		front = rear
	}
	if forDonor && len(rear) == 0 {
		rear = front
	}
	if len(front) == 0 || len(rear) == 0 {
		return nil, b, errors.New("no front or no rear wheels")
	}
	mean := func(a []float32) float32 {
		var s float32
		for _, v := range a {
			s += v
		}
		return s / float32(len(a))
	}
	fy, ry := mean(front), mean(rear)
	fr.o[l.Fwd] = (fy + ry) / 2
	fr.o[l.Side] = sideSum / float32(len(l.Wheels))
	if bike {
		fr.o[l.Side] = (l.Min[l.Side] + l.Max[l.Side]) / 2
	}
	b.HalfBase = float32(math.Abs(float64(fy-ry))) / 2
	b.Origin = [3]float32{fr.o[0], fr.o[1], fr.ground}
	switch {
	case l.Fwd == 0 && l.FwdSign > 0:
		b.ModelYaw = 90
	case l.Fwd == 0:
		b.ModelYaw = -90
	case l.FwdSign > 0:
		b.ModelYaw = 0
	default:
		b.ModelYaw = 180
	}
	owner := l.Owner
	var wheels []wh
	for _, w := range l.Wheels {
		at := fr.pt(w.At)
		at[2] = w.At[2] - fr.ground
		wheels = append(wheels, wh{w.Name, at, w.Radius})
	}
	if !bike {
		var rearYs []float32
		for i := range wheels {
			if wheels[i].at[1] < 0 {
				rearYs = append(rearYs, wheels[i].at[1])
			}
		}
		sort.Slice(rearYs, func(a, c int) bool { return rearYs[a] > rearYs[c] })
		for i := range wheels {
			w := &wheels[i]
			fb := "F"
			if w.at[1] < 0 {
				fb = "R"
			}
			lr := "L"
			if w.at[0] > 0 {
				lr = "R"
			}
			w.name = fb + lr
			if fb == "R" && len(rearYs) > 0 && w.at[1] < rearYs[0]-w.r {
				w.name += "2"
			}
		}
	}
	for _, w := range wheels {
		b.Wheels = append(b.Wheels, w.name)
		if !bike {
			b.HalfTrack += float32(math.Abs(float64(w.at[0]))) / float32(len(wheels))
		}
		if w.name[0] == 'F' {
			b.WheelRadiusF = max(b.WheelRadiusF, w.r/UnitsPerMetre)
		} else {
			b.WheelRadiusR = max(b.WheelRadiusR, w.r/UnitsPerMetre)
		}
	}
	shape := func(m *fnvnif.Mesh, name string, tris []int, at [3]float32) *nif.Shape {
		s := &nif.Shape{Name: name, Diffuse: m.Diffuse, NormalTx: m.NormalMap, Alpha: 1, Specular: [3]float32{0.25, 0.25, 0.25}, Gloss: 10}
		if m.Alpha {
			if m.AlphaFlags&1 != 0 {
				s.Blend = true
			} else {
				s.AlphaTest, s.AlphaRef = true, m.AlphaRef
			}
		}
		idx := map[uint16]uint16{}
		for _, ti := range tris {
			var t [3]uint16
			for k, v := range m.Tris[ti] {
				n, ok := idx[v]
				if !ok {
					n = uint16(len(s.Pos))
					idx[v] = n
					p := fr.pt(m.Pos[v])
					s.Pos = append(s.Pos, [3]float32{p[0] - at[0], p[1] - at[1], p[2] - at[2]})
					if m.Normal != nil {
						s.Normal = append(s.Normal, fr.dir(m.Normal[v]))
					} else {
						s.Normal = append(s.Normal, [3]float32{0, 0, 1})
					}
					if m.Tangent != nil {
						s.Tangent = append(s.Tangent, fr.dir(m.Tangent[v]))
						s.Bitan = append(s.Bitan, fr.dir(m.Bitan[v]))
					}
					if m.UV != nil {
						s.UV = append(s.UV, m.UV[v])
					} else {
						s.UV = append(s.UV, [2]float32{})
					}
				}
				t[k] = n
			}
			s.Tris = append(s.Tris, t)
		}
		return s
	}
	var body []*nif.Shape
	perWheel := make([][]*nif.Shape, len(wheels))
	b.Clearance = math.MaxFloat32
	for mi, m := range ms {
		var bt []int
		wt := make([][]int, len(wheels))
		for ti := range m.Tris {
			if w, ok := owner[[2]int{mi, ti}]; ok {
				wt[w] = append(wt[w], ti)
			} else {
				bt = append(bt, ti)
			}
		}
		if len(bt) > 0 {
			s := shape(m, m.Name, bt, [3]float32{})
			body = append(body, s)
			b.BodyTris += len(bt)
			for _, p := range s.Pos {
				b.Clearance = min(b.Clearance, p[2])
			}
		}
		for w, tl := range wt {
			if len(tl) > 0 {
				perWheel[w] = append(perWheel[w], shape(m, m.Name+"_"+wheels[w].name, tl, wheels[w].at))
				b.WheelTris += len(tl)
			}
		}
	}
	for _, m := range ms {
		for _, p := range m.Pos {
			q := fr.pt(p)
			b.Front = max(b.Front, q[1])
			b.Rear = max(b.Rear, -q[1])
			b.HalfWidth = max(b.HalfWidth, float32(math.Abs(float64(q[0]))))
		}
	}
	b.HalfLength = (b.Front + b.Rear) / 2
	if !bike {
		cab(&b, body)
	}
	if b.Clearance == math.MaxFloat32 {
		b.Clearance = 0
	}
	b.wheelShapes = perWheel
	if len(donor) > 0 && donor[0] != nil && len(donor[0].Left) > 0 {
		putOn(&b, donor[0], body, wheels, perWheel)
	}
	var named []nif.NamedNode
	if len(donor) > 0 && donor[0] != nil && donor[0].Steer != nil && b.Cab {
		el := 40 * math.Pi / 180
		axis := [3]float64{0, -math.Cos(el), math.Sin(el)}
		at := [3]float32{b.Eye[0], b.Eye[1] + 28, b.Eye[2] - 30}
		named = append(named, nif.NamedNode{Name: "MMWCockpitWheel", At: at, Shapes: donor[0].Steer.placed(axis, 16)})
		b.Steer = true
		b.SteerAxis = [3]float32{float32(axis[0]), float32(axis[1]), float32(axis[2])}
		b.SteerAt = at
		b.WheelNotes = append(b.WheelNotes, fmt.Sprintf("a steering wheel in the cab at %.0f %.0f %.0f (from %s, r %.1f -> 16)", at[0], at[1], at[2], donor[0].Steer.From, donor[0].Steer.Radius))
		tx := donor[0].ColumnTx
		if tx == "" && len(donor[0].Steer.Shapes) > 0 {
			tx = donor[0].Steer.Shapes[0].Diffuse
		}
		down := [3]float64{-axis[0], -axis[1], -axis[2]}
		named = append(named, nif.NamedNode{Name: "MMWColumn", At: at, Shapes: []*nif.Shape{column(down, tx)}})
		b.WheelNotes = append(b.WheelNotes, "its column: 34 units down its line into the dash")
	}
	if len(donor) > 0 && donor[0] != nil {
		named = append(named, donor[0].Extra...)
	}
	var wn []nif.WheelNode
	for i, w := range wheels {
		wn = append(wn, nif.WheelNode{Name: w.name, At: w.at, Shapes: perWheel[i]})
	}
	return nif.MovingWheels(body, wn, nil, named...), b, nil
}

type wh struct {
	name string
	at   [3]float32
	r    float32
}

func putOn(b *Built, d *Donor, body []*nif.Shape, wheels []wh, perWheel [][]*nif.Shape) {
	var hubR float32
	R := float32(math.MaxFloat32)
	for _, w := range wheels {
		hubR = max(hubR, w.r)
		a := archOver(body, w)
		if a > 0 {
			R = min(R, a-w.at[2]-2)
		}
		b.WheelNotes = append(b.WheelNotes, fmt.Sprintf("%s: hub r %.1f at %.0f %.0f %.0f, %d own triangles; the well's arch %.1f over the ground", w.name, w.r, w.at[0], w.at[1], w.at[2], tris(perWheel, w.name, wheels), a))
	}
	if R == math.MaxFloat32 {
		R = hubR * 1.35
		b.WheelNotes = append(b.WheelNotes, "no wheel well found over any hub: tyres 1.35 times the hub")
	}
	R = min(max(R, hubR), hubR*2.2)
	if d.Want > 0 {
		R = max(d.Want, hubR)
		b.WheelNotes = append(b.WheelNotes, fmt.Sprintf("tyres r %.1f as cars.json says", R))
	}
	lift := R - hubR
	for i := range wheels {
		w := &wheels[i]
		src := d.Left
		if w.at[0] > 0 {
			src = d.Right
		}
		k := R / d.Radius
		for _, s := range perWheel[i] {
			b.WheelTris -= len(s.Tris)
		}
		perWheel[i] = nil
		n := 0
		for _, s := range src {
			c := *s
			c.Pos = make([][3]float32, len(s.Pos))
			for j, p := range s.Pos {
				c.Pos[j] = [3]float32{p[0] * k, p[1] * k, p[2] * k}
			}
			c.Name = s.Name + "_on_" + w.name
			perWheel[i] = append(perWheel[i], &c)
			b.WheelTris += len(c.Tris)
			n += len(c.Tris)
		}
		w.at[2] += lift
		w.r = R
		b.WheelNotes = append(b.WheelNotes, fmt.Sprintf("%s: the donor's wheel on (%d shapes, %d triangles, scaled %.2f to r %.1f)", w.name, len(src), n, k, R))
	}
	up := lift + d.Raise
	for _, s := range body {
		for j := range s.Pos {
			s.Pos[j][2] += up
		}
	}
	b.Clearance += up
	b.Origin[2] -= up
	b.Eye[2] += up
	b.Hood[1] += up
	b.WheelRadiusF, b.WheelRadiusR = R/UnitsPerMetre, R/UnitsPerMetre
	b.WheelNotes = append(b.WheelNotes, fmt.Sprintf("tyres r %.1f (hubs %.1f): the body %.1f higher (%.1f of it the suspension raised)", R, hubR, up, d.Raise))
}

func tris(perWheel [][]*nif.Shape, name string, wheels []wh) int {
	n := 0
	for i, w := range wheels {
		if w.name == name {
			for _, s := range perWheel[i] {
				n += len(s.Tris)
			}
		}
	}
	return n
}

func archOver(body []*nif.Shape, w wh) float32 {
	best := float32(math.MaxFloat32)
	try := func(p [3]float32) {
		if p[0]*w.at[0] > 0 && math.Abs(float64(p[0])) >= math.Abs(float64(w.at[0])) && math.Abs(float64(p[1]-w.at[1])) < 6 && p[2] > w.at[2]+0.9*w.r && p[2] < best {
			best = p[2]
		}
	}
	for _, s := range body {
		for _, t := range s.Tris {
			a, b, c := s.Pos[t[0]], s.Pos[t[1]], s.Pos[t[2]]
			u := [3]float64{float64(b[0] - a[0]), float64(b[1] - a[1]), float64(b[2] - a[2])}
			v := [3]float64{float64(c[0] - a[0]), float64(c[1] - a[1]), float64(c[2] - a[2])}
			n := [3]float64{u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0]}
			l := math.Sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2])
			if l == 0 || math.Abs(n[0])/l < 0.6 {
				continue
			}
			for e := 0; e < 3; e++ {
				p, q := s.Pos[t[e]], s.Pos[t[(e+1)%3]]
				d := math.Max(math.Abs(float64(q[0]-p[0])), math.Max(math.Abs(float64(q[1]-p[1])), math.Abs(float64(q[2]-p[2]))))
				k := int(d/3) + 1
				for j := 0; j <= k; j++ {
					f := float32(j) / float32(k)
					try([3]float32{p[0] + (q[0]-p[0])*f, p[1] + (q[1]-p[1])*f, p[2] + (q[2]-p[2])*f})
				}
			}
		}
	}
	if best == math.MaxFloat32 {
		return 0
	}
	return best
}

func cab(b *Built, body []*nif.Shape) {
	const step = 8
	n := int((b.Front+b.Rear)/step) + 1
	if n < 4 {
		return
	}
	top := make([]float32, n)
	for i := range top {
		top[i] = -math.MaxFloat32
	}
	mark := func(p [3]float32) {
		if i := int((p[1] + b.Rear) / step); i >= 0 && i < n && p[2] > top[i] {
			top[i] = p[2]
		}
	}
	for _, s := range body {
		for _, t := range s.Tris {
			for e := 0; e < 3; e++ {
				p, q := s.Pos[t[e]], s.Pos[t[(e+1)%3]]
				k := int(math.Abs(float64(q[1]-p[1]))/(step/2)) + 1
				for j := 0; j <= k; j++ {
					f := float32(j) / float32(k)
					mark([3]float32{p[0] + (q[0]-p[0])*f, p[1] + (q[1]-p[1])*f, p[2] + (q[2]-p[2])*f})
				}
			}
		}
	}
	hi := 0
	for i := range top {
		if top[i] > top[hi] {
			hi = i
		}
	}
	roof := top[hi]
	if roof < b.Clearance+40 {
		return
	}
	lo, up := hi, hi
	for lo > 0 && top[lo-1] >= roof-10 {
		lo--
	}
	for up < n-1 && top[up+1] >= roof-10 {
		up++
	}
	back, front := float32(lo)*step-b.Rear, float32(up+1)*step-b.Rear
	if front-back < 40 {
		return
	}
	b.Cab = true
	b.Eye = [3]float32{-b.HalfTrack * 0.45, back + (front-back)*0.4, roof - 13}
	b.Hood = [2]float32{front + 8, roof + 3}
}

// column: the steering column about the wheel's centre, along dir - a hub boss (r 2.6) behind the wheel, then the tube
// (r 1.6) 34 units on, closed at both ends; flat-shaded round in the given texture
func column(dir [3]float64, tx string) *nif.Shape {
	z := norm3(dir)
	ref := [3]float64{1, 0, 0}
	if math.Abs(z[0]) > 0.9 {
		ref = [3]float64{0, 1, 0}
	}
	x := norm3(cross3(ref, z))
	y := cross3(z, x)
	sh := &nif.Shape{Name: "MMWColumn", Diffuse: tx, Alpha: 1, Specular: [3]float32{0.3, 0.3, 0.3}, Gloss: 20}
	const sides = 14
	ring := func(t, r float64) uint16 {
		base := uint16(len(sh.Pos))
		for i := 0; i < sides; i++ {
			a := float64(i) * 2 * math.Pi / sides
			n := [3]float64{math.Cos(a)*x[0] + math.Sin(a)*y[0], math.Cos(a)*x[1] + math.Sin(a)*y[1], math.Cos(a)*x[2] + math.Sin(a)*y[2]}
			sh.Pos = append(sh.Pos, [3]float32{float32(z[0]*t + r*n[0]), float32(z[1]*t + r*n[1]), float32(z[2]*t + r*n[2])})
			sh.Normal = append(sh.Normal, [3]float32{float32(n[0]), float32(n[1]), float32(n[2])})
			sh.UV = append(sh.UV, [2]float32{float32(i) / sides, float32(t / 8)})
		}
		return base
	}
	band := func(a, b uint16) {
		for i := uint16(0); i < sides; i++ {
			j := (i + 1) % sides
			sh.Tris = append(sh.Tris, [3]uint16{a + i, b + i, b + j}, [3]uint16{a + i, b + j, a + j})
		}
	}
	cap := func(t float64, r float64, out float64) {
		c := uint16(len(sh.Pos))
		sh.Pos = append(sh.Pos, [3]float32{float32(z[0] * t), float32(z[1] * t), float32(z[2] * t)})
		sh.Normal = append(sh.Normal, [3]float32{float32(z[0] * out), float32(z[1] * out), float32(z[2] * out)})
		sh.UV = append(sh.UV, [2]float32{0.5, 0.5})
		r0 := ring(t, r)
		for i := uint16(0); i < sides; i++ {
			sh.Normal[int(r0+i)] = [3]float32{float32(z[0] * out), float32(z[1] * out), float32(z[2] * out)}
			sh.Tris = append(sh.Tris, [3]uint16{c, r0 + i, r0 + (i+1)%sides})
		}
	}
	cap(-1.5, 2.6, -1)
	band(ring(-1.5, 2.6), ring(3.5, 2.6))
	cap(3.5, 2.6, 1)
	band(ring(3.5, 1.6), ring(34, 1.6))
	cap(34, 1.6, 1)
	n := len(sh.Tris)
	for i := 0; i < n; i++ {
		t := sh.Tris[i]
		sh.Tris = append(sh.Tris, [3]uint16{t[0], t[2], t[1]})
	}
	return sh
}
