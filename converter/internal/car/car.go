// Package car turns a Need for Speed car (its own bundle plus the game's shared shader/texture bundles) into Fallout
package car

import (
	"fmt"
	"image"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"mmw/internal/bc"
	"mmw/internal/bnd2"
	"mmw/internal/crit"
	"mmw/internal/dds"
	"mmw/internal/nif"
)

const UnitsPerMetre = 70.0

type Options struct {
	ID                       string
	Bundles                  []string
	Shared                   []string
	Paint                    [3]uint8
	DataDir                  string
	MaxTex                   int
	Bone                     string
	Scale                    float32
	ScreenTex                string
	ScreenUV                 [4]float32
	ScreenKind               string
	ScreenRotate, ScreenSkew float32
	ArchFit                  bool
	Extra                    Extra
}

// ScreenW, ScreenH: the dash screen texture's size; a single-DIN display strip uses DinW x DinH.
const ScreenW, ScreenH = 256, 128
const ScreenPath = `textures\mmw\screen.dds`
const DinW, DinH = 256, 64
const DinPath = `textures\mmw\screen_din.dds`

// PipW, PipH: the dash Pip-Boy's screen texture (DashGadgets), drawn on by the plugin like the screens above
const PipW, PipH = 128, 96
const PipPath = `textures\mmw\screen_pip.dds`

type Result struct {
	WornNIF, StaticNIF, DriveNIF    string
	Shapes, Triangles               int
	Textures                        int
	Length, Width                   float32
	HalfTrack, HalfBase, HalfLength float32
	Clearance, Front, Rear          float32
	WheelRadius                     float32
	WheelRadiusF, WheelRadiusR      float32
	Lamps                           [4]int
	LampY                           [4]float32
	Gauge                           bool
	SteerWheel                      bool
	SteerAxis                       [3]float32
	SteerTris                       int
	SteerRadius                     float32
	SteerTile                       *image.RGBA
}

const (
	LightNone    = -1
	LightHead    = 0
	LightTail    = 1
	LightBrake   = 2
	LightReverse = 3
)

var lightNames = [4]string{"Head", "Tail", "Brake", "Reverse"}

type part struct {
	group int
	chan_ int
	light int
	mat   *crit.Material
	pos   [][3]float32
	uv    [][2]float32
	tris  [][3]uint32
}

// toGame converts NFS model coordinates (metres, y up) to game units (z up), times the cars' scale.
func toGame(p [3]float32, sc float32) [3]float32 {
	u := UnitsPerMetre * sc
	return [3]float32{-p[0] * u, p[2] * u, p[1] * u}
}

func Convert(o Options) (*Result, error) {
	sc := o.Scale
	if sc <= 0 {
		sc = 1
	}
	var bundles []*bnd2.Bundle
	for _, p := range o.Bundles {
		b, err := bnd2.Open(p)
		if err != nil {
			return nil, err
		}
		bundles = append(bundles, b)
	}
	if len(bundles) == 0 {
		return nil, fmt.Errorf("no car bundles")
	}
	ptr64 := bundles[0].Ptr64
	posScale := float32(10)
	if ptr64 {
		posScale = 8
	}
	all := append([]*bnd2.Bundle{}, bundles...)
	for _, p := range o.Shared {
		if b, err := bnd2.Open(p); err == nil {
			all = append(all, b)
		}
	}
	L := crit.Lookup{Bundles: all}
	spec, err := crit.ReadVehicleSpec(L, bundles[0])
	if err != nil {
		return nil, err
	}
	mats := map[uint64]*crit.Material{}
	var parts []*part
	type mask struct {
		w, h int
		px   []byte
	}
	masks := map[uint64]*mask{}
	lightMask := func(mat *crit.Material) *mask {
		if mat.Shader == nil || !strings.Contains(strings.ToLower(mat.Shader.Technique), "fourchannel") {
			return nil
		}
		if mk, ok := masks[mat.ID]; ok {
			return mk
		}
		var mk *mask
		if id, ok := mat.Tex("emissive"); ok {
			if r, b := L.Get(id); r != nil && r.Type == crit.TypeRaster {
				if ra, err := crit.ReadRaster(r, b.Ptr64); err == nil {
					if px, ok := bc.Decode(ra.Format, ra.Width, ra.Height, ra.Pixels); ok {
						mk = &mask{ra.Width, ra.Height, px}
					}
				}
			}
		}
		masks[mat.ID] = mk
		return mk
	}
	triChan := func(mk *mask, uv [][2]float32, t [3]uint32) int {
		var best [4]int
		sample := func(u, v float32) {
			x := int(u*float32(mk.w)) % mk.w
			y := int(v*float32(mk.h)) % mk.h
			if x < 0 {
				x += mk.w
			}
			if y < 0 {
				y += mk.h
			}
			o := (y*mk.w + x) * 4
			for c := 0; c < 4; c++ {
				if int(mk.px[o+c]) > best[c] {
					best[c] = int(mk.px[o+c])
				}
			}
		}
		var cu, cv float32
		for _, i := range t {
			sample(uv[i][0], uv[i][1])
			cu += uv[i][0] / 3
			cv += uv[i][1] / 3
		}
		sample(cu, cv)
		ch, bv := -1, 60
		for c := 0; c < 4; c++ {
			if best[c] > bv {
				ch, bv = c, best[c]
			}
		}
		return ch
	}
	addModel := func(model *bnd2.Resource, group int, xf func([3]float32) [3]float32) error {
		rr, err := crit.ModelLOD0(L, model)
		if err != nil {
			return err
		}
		R, err := crit.ReadRenderable(rr, ptr64)
		if err != nil {
			return err
		}
		for _, m := range R.Meshes {
			mat, ok := mats[m.MaterialID]
			if !ok {
				mat, err = crit.ReadMaterial(L, m.MaterialID, ptr64)
				if err != nil {
					Logf("skip mesh: %v", err)
					continue
				}
				mats[m.MaterialID] = mat
			}
			tris := R.Triangles(m)
			if len(tris) == 0 {
				continue
			}
			var maxIdx uint32
			for _, t := range tris {
				for _, v := range t {
					if v > maxIdx {
						maxIdx = v
					}
				}
			}
			layout := []crit.VElem(nil)
			if mat.Shader != nil {
				layout = mat.Shader.VDesc
			}
			if layout == nil {
				stride := uint32(0)
				for s := uint32(64); s >= 16; s -= 4 {
					if int(m.VBSize)/int(s) >= int(maxIdx)+1 {
						stride = s
						break
					}
				}
				layout = crit.FallbackLayout(stride)
			}
			v, err := crit.DecodeVertices(R, m, layout, int(maxIdx)+1, posScale)
			if err != nil {
				Logf("skip mesh (material %x): %v", m.MaterialID, err)
				continue
			}
			pos := make([][3]float32, len(v.Pos))
			for i, q := range v.Pos {
				pos[i] = toGame(xf(q), sc)
			}
			if mk := lightMask(mat); mk != nil && v.UV != nil && group < 0 {
				var byChan [5][][3]uint32
				for _, t := range tris {
					c := triChan(mk, v.UV, t)
					byChan[c+1] = append(byChan[c+1], t)
				}
				for c := -1; c < 4; c++ {
					if len(byChan[c+1]) > 0 {
						parts = append(parts, &part{group: group, chan_: c, light: LightNone, mat: mat, tris: byChan[c+1], uv: v.UV, pos: pos})
					}
				}
				continue
			}
			if id, ok := mat.Tex("diffuse"); ok && o.ScreenTex != "" && group < 0 && v.UV != nil && strings.Contains(","+o.ScreenTex+",", fmt.Sprintf(",%x,", id)) {
				u0, v0, u1, v1 := o.ScreenUV[0], o.ScreenUV[1], o.ScreenUV[2], o.ScreenUV[3]
				var sp [][3]float32
				var su [][2]float32
				var st, rest [][3]uint32
				rpos := append([][3]float32(nil), pos...)
				ruv := append([][2]float32(nil), v.UV...)
				aspect := float64(ScreenW) / float64(ScreenH)
				if o.ScreenKind == "din" {
					aspect = float64(DinW) / float64(DinH)
				}
				rs, rc := math.Sincos(float64(o.ScreenRotate) * math.Pi / 180)
				sk := math.Tan(float64(o.ScreenSkew) * math.Pi / 180)
				unturn := func(uu, vv float32) (float32, float32) {
					if o.ScreenRotate == 0 && o.ScreenSkew == 0 {
						return uu, vv
					}
					x, y := (float64(uu)-0.5)*aspect, float64(vv)-0.5
					x, y = x*rc-y*rs, x*rs+y*rc
					x += sk * y
					return float32(x/aspect + 0.5), float32(y + 0.5)
				}
				addScreen := func(poly []clipV) {
					b := uint32(len(sp))
					for _, q := range poly {
						sp = append(sp, q.p)
						uu := (q.uv[0] - u0) / (u1 - u0)
						vv := (q.uv[1] - v0) / (v1 - v0)
						uu, vv = unturn(uu, vv)
						su = append(su, [2]float32{float32(math.Min(1, math.Max(0, float64(uu)))), float32(math.Min(1, math.Max(0, float64(vv))))})
					}
					for k := 1; k+1 < len(poly); k++ {
						st = append(st, [3]uint32{b, b + uint32(k), b + uint32(k) + 1})
					}
				}
				addRest := func(poly []clipV) {
					b := uint32(len(rpos))
					for _, q := range poly {
						rpos = append(rpos, q.p)
						ruv = append(ruv, q.uv)
					}
					for k := 1; k+1 < len(poly); k++ {
						rest = append(rest, [3]uint32{b, b + uint32(k), b + uint32(k) + 1})
					}
				}
				const eps = 0.004
				bounds := func(t [3]uint32) (lo, hi [2]float32) {
					lo, hi = [2]float32{9, 9}, [2]float32{-9, -9}
					for _, k := range t {
						for c := 0; c < 2; c++ {
							lo[c] = min(lo[c], v.UV[k][c])
							hi[c] = max(hi[c], v.UV[k][c])
						}
					}
					return
				}
				own := false
				for _, t := range tris {
					lo, hi := bounds(t)
					if lo[0] >= u0-eps && hi[0] <= u1+eps && lo[1] >= v0-eps && hi[1] <= v1+eps {
						own = true
						break
					}
				}
				for _, t := range tris {
					lo, hi := bounds(t)
					tri := []clipV{{pos[t[0]], v.UV[t[0]]}, {pos[t[1]], v.UV[t[1]]}, {pos[t[2]], v.UV[t[2]]}}
					cu := (v.UV[t[0]][0] + v.UV[t[1]][0] + v.UV[t[2]][0]) / 3
					cv := (v.UV[t[0]][1] + v.UV[t[1]][1] + v.UV[t[2]][1]) / 3
					switch {
					case own && (cu < u0 || cu > u1 || cv < v0 || cv > v1):
						rest = append(rest, t)
					case own:
						addScreen(tri)
					case hi[0] <= u0 || lo[0] >= u1 || hi[1] <= v0 || lo[1] >= v1:
						rest = append(rest, t)
					default:
						in := tri
						for _, hp := range [4]struct {
							c    int
							v    float32
							keep float32
						}{{0, u0, 1}, {0, u1, -1}, {1, v0, 1}, {1, v1, -1}} {
							outPiece := clipPoly(in, hp.c, hp.v, -hp.keep)
							if len(outPiece) >= 3 {
								addRest(outPiece)
							}
							in = clipPoly(in, hp.c, hp.v, hp.keep)
							if len(in) < 3 {
								break
							}
						}
						if len(in) >= 3 {
							addScreen(in)
						}
					}
				}
				if len(st) > 0 {
					parts = append(parts, &part{group: -3, chan_: -1, light: LightNone, mat: mat, tris: st, uv: su, pos: sp})
					tris, pos = rest, rpos
					v.UV = ruv
					Logf("screen: %d triangles", len(st))
				}
			}
			parts = append(parts, &part{group: group, chan_: -1, light: LightNone, mat: mat, tris: tris, uv: v.UV, pos: pos})
		}
		return nil
	}
	if err := addModel(spec.Body, -1, func(p [3]float32) [3]float32 { return p }); err != nil {
		return nil, fmt.Errorf("body: %w", err)
	}
	bodyParts := len(parts)
	for wi, w := range spec.Wheels {
		pos := w.Pos
		right := pos[0] < 0
		for _, wm := range w.Models {
			_ = addModel(wm, wi, func(p [3]float32) [3]float32 {
				if right {
					p = [3]float32{-p[0], p[1], -p[2]}
				}
				return [3]float32{p[0] + pos[0], p[1] + pos[1], p[2] + pos[2]}
			})
		}
	}
	if len(parts) == 0 {
		return nil, fmt.Errorf("no meshes decoded")
	}
	wheelDrop := make([]float32, len(spec.Wheels))
	if o.ArchFit {
		const archGapMin, archGapTarget = 1.5, 4.0
		type wInfo struct {
			c      [3]float32
			r, gap float32
			ok     bool
		}
		info := make([]wInfo, len(spec.Wheels))
		var sumY float32
		for wi, w := range spec.Wheels {
			c := toGame(w.Pos, sc)
			sumY += c[1]
			var r float32
			for _, p := range parts {
				if p.group != wi {
					continue
				}
				for _, q := range p.pos {
					dy, dz := q[1]-c[1], q[2]-c[2]
					if d := float32(math.Sqrt(float64(dy*dy + dz*dz))); d > r {
						r = d
					}
				}
			}
			if r <= 0 {
				continue
			}
			arch := float32(math.MaxFloat32)
			for _, p := range parts {
				if p.group != -1 || p.chan_ >= 0 || p.light != LightNone {
					continue
				}
				for _, t := range p.tris {
					x := (p.pos[t[0]][0] + p.pos[t[1]][0] + p.pos[t[2]][0]) / 3
					y := (p.pos[t[0]][1] + p.pos[t[1]][1] + p.pos[t[2]][1]) / 3
					z := (p.pos[t[0]][2] + p.pos[t[1]][2] + p.pos[t[2]][2]) / 3
					if float32(math.Abs(float64(x-c[0]))) < 0.35*r && float32(math.Abs(float64(y-c[1]))) < 0.5*r && z > c[2]+0.6*r && z < arch {
						arch = z
					}
				}
			}
			if arch == float32(math.MaxFloat32) {
				continue
			}
			info[wi] = wInfo{c: c, r: r, gap: arch - (c[2] + r), ok: true}
		}
		midY := sumY / float32(max(1, len(spec.Wheels)))
		for _, front := range []bool{true, false} {
			gap, any := float32(math.MaxFloat32), false
			for _, in := range info {
				if in.ok && (in.c[1] > midY) == front {
					gap = float32(math.Min(float64(gap), float64(in.gap)))
					any = true
				}
			}
			if !any || gap >= archGapMin*sc {
				continue
			}
			drop := archGapTarget*sc - gap
			for wi, in := range info {
				if !in.ok || (in.c[1] > midY) != front {
					continue
				}
				wheelDrop[wi] = drop
				for _, p := range parts {
					if p.group == wi {
						for i := range p.pos {
							p.pos[i][2] -= drop
						}
					}
				}
			}
			axle := "rear"
			if front {
				axle = "front"
			}
			Logf("wheels: %s axle lowered %.1f (its tyres were %.1f from the arches)", axle, drop, gap)
		}
	}
	if g := o.Extra.ArchGap; g > 0 && !o.ArchFit {
		type wg struct {
			c   [3]float32
			gap float32
			ok  bool
		}
		info := make([]wg, len(spec.Wheels))
		var sumY float32
		for wi, w := range spec.Wheels {
			c := toGame(w.Pos, sc)
			sumY += c[1]
			var r float32
			for _, p := range parts {
				if p.group != wi {
					continue
				}
				for _, q := range p.pos {
					dy, dz := q[1]-c[1], q[2]-c[2]
					r = max(r, float32(math.Sqrt(float64(dy*dy+dz*dz))))
				}
			}
			if r <= 0 {
				continue
			}
			best := float32(math.MaxFloat32)
			for _, p := range parts {
				if p.group != -1 || p.chan_ >= 0 || p.light != LightNone || p.mat == nil || p.mat.Kind == "glass" {
					continue
				}
				for _, t := range p.tris {
					for _, vi := range t {
						q := p.pos[vi]
						if abs32(q[0]-c[0]) >= 0.35*r || q[2] <= c[2]+0.3*r || abs32(q[1]-c[1]) >= 0.7*r {
							continue
						}
						dy, dz := q[1]-c[1], q[2]-c[2]
						d := float32(math.Sqrt(float64(dy*dy + dz*dz)))
						if d < 0.75*r {
							continue
						}
						best = min(best, d-r)
					}
				}
			}
			if best < math.MaxFloat32 {
				info[wi] = wg{c: c, gap: best, ok: true}
			}
		}
		midY := sumY / float32(max(1, len(spec.Wheels)))
		for _, front := range []bool{true, false} {
			gap, any := float32(math.MaxFloat32), false
			for _, in := range info {
				if in.ok && (in.c[1] > midY) == front {
					gap, any = min(gap, in.gap), true
				}
			}
			axle := "rear"
			if front {
				axle = "front"
			}
			if !any {
				continue
			}
			if gap >= 0.3*sc && gap <= 6*sc {
				Logf("wheels: %s tyres %.1f from the arches (left as they are)", axle, gap)
				continue
			}
			drop := g*sc - gap
			moved := map[*[3]float32]bool{}
			for wi, in := range info {
				if !in.ok || (in.c[1] > midY) != front {
					continue
				}
				wheelDrop[wi] += drop
				for _, p := range parts {
					if p.group != wi || len(p.pos) == 0 || moved[&p.pos[0]] {
						continue
					}
					moved[&p.pos[0]] = true
					for i := range p.pos {
						p.pos[i][2] -= drop
					}
				}
			}
			if drop > 0 {
				Logf("wheels: %s axle lowered %.1f (its tyres were %.1f into the arches)", axle, drop, -gap)
			} else {
				Logf("wheels: %s axle raised %.1f (the body sat %.1f over its tyres)", axle, -drop, gap)
			}
		}
	}
	minZ := float32(math.MaxFloat32)
	for _, p := range parts {
		for _, q := range p.pos {
			if q[2] < minZ {
				minZ = q[2]
			}
		}
	}
	var cx, wMinY, wMaxY, track float32
	wMinY, wMaxY = math.MaxFloat32, -math.MaxFloat32
	for _, w := range spec.Wheels {
		g := toGame(w.Pos, sc)
		cx += g[0] / float32(len(spec.Wheels))
		wMinY = float32(math.Min(float64(wMinY), float64(g[1])))
		wMaxY = float32(math.Max(float64(wMaxY), float64(g[1])))
	}
	cy := (wMinY + wMaxY) / 2
	if len(spec.Wheels) == 0 {
		cx, cy, wMinY, wMaxY = 0, 0, -87, 87
	}
	for _, w := range spec.Wheels {
		track += float32(math.Abs(float64(toGame(w.Pos, sc)[0]-cx))) / float32(len(spec.Wheels))
	}
	if track == 0 {
		track = 54
	}
	shifted := map[*[3]float32]bool{}
	for _, p := range parts {
		if len(p.pos) == 0 || shifted[&p.pos[0]] {
			continue
		}
		shifted[&p.pos[0]] = true
		for i := range p.pos {
			p.pos[i] = [3]float32{p.pos[i][0] - cx, p.pos[i][1] - cy, p.pos[i][2] - minZ}
		}
	}
	{
		var out []*part
		for _, p := range parts {
			if p.chan_ < 0 {
				out = append(out, p)
				continue
			}
			var byRole [4][][3]uint32
			for _, t := range p.tris {
				y := (p.pos[t[0]][1] + p.pos[t[1]][1] + p.pos[t[2]][1]) / 3
				role := LightTail
				switch {
				case y > 0:
					role = LightHead
				case p.chan_ == 0:
					role = LightBrake
				case p.chan_ == 2:
					role = LightReverse
				}
				byRole[role] = append(byRole[role], t)
			}
			for r := 0; r < 4; r++ {
				if len(byRole[r]) > 0 {
					q := *p
					q.tris, q.light = byRole[r], r
					out = append(out, &q)
				}
			}
		}
		parts = out
	}
	var swFound steerWheel
	if sel, sw, ok := findSteeringWheel(parts, UnitsPerMetre*sc, o.Extra.SteerMin, o.Extra.Seats == 1, o.Extra.SteerDepth); ok {
		var out []*part
		for _, p := range parts {
			mask := sel[p]
			if mask == nil {
				out = append(out, p)
				continue
			}
			var keep, take [][3]uint32
			for ti, t := range p.tris {
				if mask[ti] {
					take = append(take, t)
				} else {
					keep = append(keep, t)
				}
			}
			if len(keep) > 0 {
				q := *p
				q.tris = keep
				out = append(out, &q)
			}
			q := *p
			q.tris, q.group = take, -2
			out = append(out, &q)
		}
		parts = out
		swFound = sw
		Logf("steering wheel: centre (%.1f %.1f %.1f) column (%.2f %.2f %.2f) radius %.2f m, %d triangles in %d pieces",
			sw.Centre[0], sw.Centre[1], sw.Centre[2], sw.Axis[0], sw.Axis[1], sw.Axis[2], sw.Radius/(UnitsPerMetre*sc), sw.Tris, sw.Pieces)
	} else {
		Logf("steering wheel: not found")
	}
	res0 := &Result{HalfTrack: track, HalfBase: (wMaxY - wMinY) / 2}
	res0.SteerTile = steerTile(parts, swFound, swFound.Tris > 0, UnitsPerMetre*sc)
	if swFound.Tris > 0 {
		res0.SteerWheel, res0.SteerAxis, res0.SteerTris = true, swFound.Axis, swFound.Tris
		res0.SteerRadius = swFound.Radius / (UnitsPerMetre * sc)
	}
	res0.Clearance, res0.Front, res0.Rear = math.MaxFloat32, 0, 0
	for i, p := range parts {
		for _, q := range p.pos {
			res0.Front = float32(math.Max(float64(res0.Front), float64(q[1])))
			res0.Rear = float32(math.Max(float64(res0.Rear), float64(-q[1])))
			if i < bodyParts && p.mat.Kind != "glass" {
				res0.Clearance = float32(math.Min(float64(res0.Clearance), float64(q[2])))
			}
		}
	}
	Logf("recentred by (%.1f, %.1f, %.1f) game units; half track %.1f, half wheelbase %.1f", -cx, -cy, -minZ, res0.HalfTrack, res0.HalfBase)
	if o.Extra.Lamps != nil && o.Extra.Lamps.Material != "" {
		parts = applyLamps(parts, o.Extra.Lamps)
	}
	var links []suspLink
	var pins map[string][3]float32
	if o.Extra.Links {
		var wi []wheelInfo
		for i, w := range spec.Wheels {
			g := toGame(w.Pos, sc)
			c := [3]float32{g[0] - cx, g[1] - cy, g[2] - minZ - wheelDrop[i]}
			var r, xlo, xhi float32 = 0, math.MaxFloat32, -math.MaxFloat32
			for _, p := range parts {
				if p.group != i {
					continue
				}
				for _, t := range p.tris {
					for _, vi := range t {
						q := p.pos[vi]
						dy, dz := q[1]-c[1], q[2]-c[2]
						r = max(r, float32(math.Sqrt(float64(dy*dy+dz*dz))))
						xlo, xhi = min(xlo, q[0]), max(xhi, q[0])
					}
				}
			}
			if r <= 0 {
				continue
			}
			name := "R"
			if c[1] > 0 {
				name = "F"
			}
			if c[0] > 0 {
				name += "R"
			} else {
				name += "L"
			}
			wi = append(wi, wheelInfo{name: name, c: c, r: r, hw: (xhi - xlo) / 2})
		}
		parts, links, pins = suspensionLinks(parts, wi)
	}
	var snow *snowBuild
	if o.Extra.Snow == nil && SnowAllCars {
		o.Extra.Snow = &SnowSpec{AutoWipers: true}
	}
	if o.Extra.Snow != nil {
		dir := `textures\mmw\cars\` + o.ID + `\`
		parts, snow = prepareSnow(parts, o.Extra.Snow, dir+"flat_n.dds", SnowDepthPath, SnowDepthPath)
	}
	texRel := filepath.Join("mmw", "cars", o.ID)
	texDir := filepath.Join(o.DataDir, "textures", texRel)
	if err := os.MkdirAll(texDir, 0o755); err != nil {
		return nil, err
	}
	written := map[string]bool{}
	writeTex := func(name string, data []byte) (string, error) {
		rel := filepath.Join("textures", texRel, name)
		if !written[name] {
			if err := os.WriteFile(filepath.Join(o.DataDir, rel), data, 0o644); err != nil {
				return "", err
			}
			written[name] = true
		}
		return strings.ReplaceAll(rel, "/", `\`), nil
	}
	paint, _ := writeTex("paint.dds", dds.Solid(o.Paint[0], o.Paint[1], o.Paint[2], 255))
	flatN, _ := writeTex("flat_n.dds", dds.FlatNormal())
	paintN, _ := writeTex("paint_n.dds", dds.Solid(128, 128, 255, 255))
	glass, _ := writeTex("glass.dds", dds.Solid(20, 24, 28, 110))
	dark, _ := writeTex("dark.dds", dds.Solid(28, 28, 30, 255))
	chrome, _ := writeTex("chrome.dds", dds.Solid(170, 170, 175, 255))
	lightTx, _ := writeTex("light.dds", dds.Solid(235, 232, 220, 255))
	redLens, _ := writeTex("lens_red.dds", dds.Solid(150, 14, 12, 255))
	rasterTex := func(id uint64, suffix string) (string, bool) {
		hd := filepath.Join("textures", "mmw", "hd", fmt.Sprintf("%s_%x%s.dds", o.ID, id, suffix))
		if _, err := os.Stat(filepath.Join(o.DataDir, hd)); err == nil {
			Logf("texture %x%s: higher-detail version %s", id, suffix, hd)
			return strings.ReplaceAll(hd, "/", `\`), true
		}
		r, b := L.Get(id)
		if r == nil || r.Type != crit.TypeRaster {
			return "", false
		}
		ra, err := crit.ReadRaster(r, b.Ptr64)
		if err != nil {
			return "", false
		}
		data, err := dds.FromDXGI(ra.Format, ra.Width, ra.Height, ra.Mips, ra.Pixels, o.MaxTex)
		if err != nil {
			return "", false
		}
		p, err := writeTex(fmt.Sprintf("%x%s.dds", id, suffix), data)
		return p, err == nil
	}
	res := res0
	glow := [4]struct {
		c [3]float32
		m float32
	}{{[3]float32{1, 0.96, 0.85}, 1.6}, {[3]float32{0.9, 0.05, 0.03}, 0.8}, {[3]float32{1, 0.06, 0.04}, 2.5}, {[3]float32{1, 1, 1}, 1.4}}
	lit := false
	build := func(ps0 []*part, off [3]float32, count bool) []*nif.Shape {
		byMat := map[uint64][]*part{}
		var order []uint64
		for _, p := range ps0 {
			key := p.mat.ID ^ uint64(p.light+1)<<60
			if _, ok := byMat[key]; !ok {
				order = append(order, key)
			}
			byMat[key] = append(byMat[key], p)
		}
		sort.Slice(order, func(i, j int) bool { return order[i] < order[j] })
		var shapes []*nif.Shape
		for _, mid := range order {
			ps := byMat[mid]
			mat := ps[0].mat
			base := nif.Shape{Alpha: 1, Specular: [3]float32{0.3, 0.3, 0.3}, Gloss: 20, NormalTx: flatN}
			hasUV := true
			for _, p := range ps {
				if p.uv == nil {
					hasUV = false
				}
			}
			switch mat.Kind {
			case "paint":
				base.Diffuse, base.Specular, base.Gloss = paint, [3]float32{0.9, 0.9, 0.9}, 80
				base.NormalTx, base.EnvMap, base.EnvMask = paintN, PaintEnvMap, PaintEnvMaskGloss
			case "glass":
				base.Diffuse, base.Blend, base.Alpha, base.Specular, base.Gloss = glass, true, 0.55, [3]float32{1, 1, 1}, 90
			default:
				base.Diffuse = dark
				if id, ok := mat.Tex("diffuse"); ok && hasUV {
					if p, ok := rasterTex(id, ""); ok {
						base.Diffuse = p
						if nid, ok := mat.Tex("normal"); ok {
							if np, ok := rasterTex(nid, "_n"); ok {
								base.NormalTx = np
							}
						}
					}
				} else if mat.Kind == "light" {
					base.Diffuse, base.Emissive = lightTx, 0.6
				} else if strings.Contains(strings.ToLower(mat.Name), "chrome") || strings.Contains(strings.ToLower(mat.Name), "alumin") {
					base.Diffuse = chrome
				}
			}
			if base.Diffuse == dark || base.Diffuse == paint || base.Diffuse == glass || base.Diffuse == chrome || base.Diffuse == lightTx {
				hasUV = hasUV && true
			}
			if ps[0].light != LightNone {
				if ps[0].light == LightTail || ps[0].light == LightBrake {
					base.Diffuse = redLens
				} else {
					base.Diffuse = lightTx
				}
				base.NormalTx, base.EnvMap, base.EnvMask = flatN, "", ""
				base.Specular, base.Gloss, base.Emissive = [3]float32{1, 1, 1}, 90, 0
				if lit {
					g := glow[ps[0].light]
					base.EmissiveColor, base.Emissive = g.c, g.m
				}
			}
			cur := &nif.Shape{}
			*cur = base
			flush := func() {
				if len(cur.Tris) == 0 {
					return
				}
				computeNormals(cur)
				if cur.UV != nil {
					computeTangents(cur)
				}
				cur.Name = fmt.Sprintf("%s:%d", shortName(mat), len(shapes))
				if cur.EnvMap == PaintEnvMap {
					cur.Name = fmt.Sprintf("MMWPaint:%d", len(shapes))
				}
				shapes = append(shapes, cur)
				if count {
					res.Triangles += len(cur.Tris)
				}
				n := &nif.Shape{}
				*n = base
				cur = n
			}
			remap := map[uint32]uint16{}
			for _, p := range ps {
				clear(remap)
				for _, t := range p.tris {
					need := 0
					for _, v := range t {
						if _, ok := remap[v]; !ok {
							need++
						}
					}
					if len(cur.Pos)+need > 65000 {
						flush()
						clear(remap)
					}
					var idx [3]uint16
					for k, v := range t {
						i, ok := remap[v]
						if !ok {
							q := p.pos[v]
							i = uint16(len(cur.Pos))
							cur.Pos = append(cur.Pos, [3]float32{q[0] - off[0], q[1] - off[1], q[2] - off[2]})
							if hasUV {
								cur.UV = append(cur.UV, p.uv[v])
							}
							remap[v] = i
						}
						idx[k] = i
					}
					if FlipWinding {
						idx[1], idx[2] = idx[2], idx[1]
					}
					cur.Tris = append(cur.Tris, idx)
				}
			}
			flush()
		}
		return shapes
	}
	shapes := build(parts, [3]float32{}, true)
	var bodyPs, steerPs, screenPs []*part
	var lightPs [4][]*part
	wiperPs := map[int][]*part{}
	wheelPs := make([][]*part, len(spec.Wheels))
	carriedPs := make([][]*part, len(spec.Wheels))
	linkPs := make([][]*part, len(links))
	for _, p := range parts {
		switch {
		case p.group <= -1000:
			wiperPs[p.group] = append(wiperPs[p.group], p)
		case p.group <= -100:
			linkPs[-100-p.group] = append(linkPs[-100-p.group], p)
		case p.group <= -10:
			carriedPs[-10-p.group] = append(carriedPs[-10-p.group], p)
		case p.group == -2:
			steerPs = append(steerPs, p)
		case p.group == -3:
			screenPs = append(screenPs, p)
		case p.group >= 0:
			wheelPs[p.group] = append(wheelPs[p.group], p)
		case p.light != LightNone:
			lightPs[p.light] = append(lightPs[p.light], p)
		default:
			bodyPs = append(bodyPs, p)
		}
	}
	driveBody := build(bodyPs, [3]float32{}, false)
	if len(screenPs) > 0 {
		texPath, tw, th := ScreenPath, ScreenW, ScreenH
		if o.ScreenKind == "din" {
			texPath, tw, th = DinPath, DinW, DinH
		}
		scrPath := filepath.Join(o.DataDir, filepath.FromSlash(strings.ReplaceAll(texPath, `\`, "/")))
		if _, err := os.Stat(scrPath); err != nil {
			px := make([]byte, tw*th*4)
			for i := 0; i < len(px); i += 4 {
				px[i], px[i+1], px[i+2], px[i+3] = 12, 8, 6, 255
			}
			os.MkdirAll(filepath.Dir(scrPath), 0o755)
			os.WriteFile(scrPath, dds.Image(tw, th, px), 0o644)
		}
		for _, sh := range build(screenPs, [3]float32{}, false) {
			sh.Name = "MMWScreen"
			sh.Diffuse, sh.NormalTx, sh.EnvMap, sh.EnvMask = texPath, flatN, "", ""
			sh.Specular, sh.Gloss = [3]float32{0.25, 0.25, 0.25}, 60
			sh.EmissiveColor, sh.Emissive = [3]float32{1, 1, 1}, 1
			sh.Blend, sh.Alpha = false, 1
			driveBody = append(driveBody, sh)
		}
	}
	if swFound.Tris > 0 && (o.Extra.Gauge == nil || !o.Extra.Gauge.Off) {
		gaugeTexture(o.DataDir)
		driveBody = append(driveBody, gaugeShape(swFound, o.Extra.Gauge, flatN))
		res0.Gauge = true
	}
	var lightNodes []nif.LightNode
	for r := 0; r < 4; r++ {
		if len(lightPs[r]) == 0 {
			continue
		}
		offShapes := build(lightPs[r], [3]float32{}, false)
		lit = true
		onShapes := build(lightPs[r], [3]float32{}, false)
		lit = false
		lightNodes = append(lightNodes, nif.LightNode{Name: lightNames[r], Off: offShapes, On: onShapes})
		n := 0
		for _, p := range lightPs[r] {
			n += len(p.tris)
		}
		Logf("lights: %s %d triangles", lightNames[r], n)
		var y float32
		var k int
		for _, p := range lightPs[r] {
			for _, t := range p.tris {
				y += triCentre(p, t)[1]
				k++
			}
		}
		res0.Lamps[r], res0.LampY[r] = n, y/float32(max(k, 1))/(UnitsPerMetre*sc)
	}
	var wheelNodes []nif.WheelNode
	var radSum float32
	var radN int
	for i, w := range spec.Wheels {
		if len(wheelPs[i]) == 0 {
			continue
		}
		g := toGame(w.Pos, sc)
		c := [3]float32{g[0] - cx, g[1] - cy, g[2] - minZ - wheelDrop[i]}
		name := "R"
		if c[1] > 0 {
			name = "F"
		}
		if c[0] > 0 {
			name += "R"
		} else {
			name += "L"
		}
		{
			mn := [3]float32{math.MaxFloat32, math.MaxFloat32, math.MaxFloat32}
			mx := [3]float32{-math.MaxFloat32, -math.MaxFloat32, -math.MaxFloat32}
			for _, p := range wheelPs[i] {
				for _, t := range p.tris {
					for _, vi := range t {
						q := p.pos[vi]
						for k := 0; k < 3; k++ {
							mn[k] = float32(math.Min(float64(mn[k]), float64(q[k])))
							mx[k] = float32(math.Max(float64(mx[k]), float64(q[k])))
						}
					}
				}
			}
			Logf("wheelcheck %s spec (%.1f %.1f %.1f) geom centre (%.1f %.1f %.1f) size (%.1f %.1f %.1f)", name, c[0], c[1], c[2],
				(mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2, mx[0]-mn[0], mx[1]-mn[1], mx[2]-mn[2])
		}
		var rmax float32
		for _, p := range wheelPs[i] {
			for _, q := range p.pos {
				dy, dz := q[1]-c[1], q[2]-c[2]
				if r := float32(math.Sqrt(float64(dy*dy + dz*dz))); r > rmax {
					rmax = r
				}
			}
		}
		if rmax > 0 {
			radSum += rmax / UnitsPerMetre
			radN++
		}
		wn := nif.WheelNode{Name: name, At: c, Shapes: build(wheelPs[i], c, false), Carried: build(carriedPs[i], c, false)}
		wheelNodes = append(wheelNodes, wn)
		if name == "FL" || name == "RL" {
			axle := "f"
			if name == "RL" {
				axle = "r"
				res.WheelRadiusR = rmax / UnitsPerMetre
			} else {
				res.WheelRadiusF = rmax / UnitsPerMetre
			}
			wdir := filepath.Join(o.DataDir, "meshes", "mmw", "wheels")
			if err := os.MkdirAll(wdir, 0o755); err == nil {
				if err := nif.Group("MMWRim", wn.Shapes).Save(filepath.Join(wdir, o.ID+"_"+axle+".nif")); err != nil {
					Logf("wheel model: %v", err)
				}
			}
		}
	}
	if radN > 0 {
		res.WheelRadius = radSum / float32(radN)
	}
	res.Shapes, res.Textures = len(shapes), len(written)
	meshDir := filepath.Join(o.DataDir, "meshes", "mmw", "cars", o.ID)
	if err := os.MkdirAll(meshDir, 0o755); err != nil {
		return nil, err
	}
	os.Remove(filepath.Join(meshDir, "worn.nif"))
	os.Remove(filepath.Join(meshDir, "static.nif"))
	var mn0, mx0 [3]float32
	for i := range mn0 {
		mn0[i], mx0[i] = math.MaxFloat32, -math.MaxFloat32
	}
	for _, s := range shapes {
		for _, p := range s.Pos {
			for k := 0; k < 3; k++ {
				mn0[k] = float32(math.Min(float64(mn0[k]), float64(p[k])))
				mx0[k] = float32(math.Max(float64(mx0[k]), float64(p[k])))
			}
		}
	}
	box := &nif.Box{Min: mn0, Max: mx0}
	box.Min[2] += 0.12 * UnitsPerMetre
	if !ParkedCollision {
		box = nil
	}
	_ = box
	var named []nif.NamedNode
	if len(steerPs) > 0 {
		named = append(named, nif.NamedNode{Name: "MMWCockpitWheel", At: swFound.Centre, Shapes: build(steerPs, swFound.Centre, false)})
	}
	for li, l := range links {
		if len(linkPs[li]) == 0 {
			continue
		}
		name, at := linkName(l)
		named = append(named, nif.NamedNode{Name: name, At: at, Shapes: build(linkPs[li], at, false)})
	}
	for _, corner := range []string{"FL", "FR"} {
		if k, ok := pins[corner]; ok {
			named = append(named, nif.NamedNode{Name: "MMWPin" + corner, At: k})
		}
	}
	if o.Extra.Shift != nil && len(o.Extra.Shift.LEDs) > 0 {
		named = append(named, shiftLEDs(parts, o.Extra.Shift, lightTx, flatN)...)
	}
	if g := o.Extra.Gadgets; g != nil {
		st := speakerTextures()
		tx := map[string]string{"chrome": chrome, "light": lightTx, "flatN": flatN}
		tx["stand"], _ = writeTex("speaker_stand.dds", dds.Solid(62, 64, 68, 255))
		for name, size := range map[string]int{"fabric": 128, "rubber": 32, "radiator": 32} {
			tx[name], _ = writeTex("speaker_"+name+".dds", dds.DXT5Mips(st[name], size, size))
		}
		tx["buttons"], _ = writeTex("speaker_buttons.dds", dds.DXT5Mips(st["buttons"], 128, 512))
		named = append(named, gadgetNodes(g, tx)...)
		if g.PipBoy != nil {
			writePipScreen(o.DataDir)
		}
	}
	if snow != nil {
		named = append(named, snow.nodes(wiperPs, func(ps []*part, at [3]float32) []*nif.Shape { return build(ps, at, false) })...)
		for _, t := range []struct {
			path string
			w, h int
		}{{SnowDepthPath, SnowBodyW, SnowBodyW}, {SnowGlassDrivePath, SnowGlassW, SnowGlassH}} {
			full := filepath.Join(o.DataDir, filepath.FromSlash(strings.ReplaceAll(t.path, `\`, "/")))
			if _, err := os.Stat(full); err == nil {
				continue
			}
			px := make([]byte, t.w*t.h*4)
			for i := 0; i < len(px); i += 4 {
				px[i], px[i+1], px[i+2], px[i+3] = 250, 246, 242, 0
			}
			os.MkdirAll(filepath.Dir(full), 0o755)
			os.WriteFile(full, dds.Image(t.w, t.h, px), 0o644)
		}
		os.Remove(filepath.Join(meshDir, "snow_cap.raw"))
	}
	if err := nif.MovingWheels(driveBody, wheelNodes, lightNodes, named...).Save(filepath.Join(meshDir, "drive.nif")); err != nil {
		return nil, err
	}
	res.StaticNIF = `mmw\cars\` + o.ID + `\drive.nif`
	var mn, mx [3]float32
	for i := range mn {
		mn[i], mx[i] = math.MaxFloat32, -math.MaxFloat32
	}
	for _, s := range shapes {
		for _, p := range s.Pos {
			for k := 0; k < 3; k++ {
				mn[k] = float32(math.Min(float64(mn[k]), float64(p[k])))
				mx[k] = float32(math.Max(float64(mx[k]), float64(p[k])))
			}
		}
	}
	res.Length, res.Width = (mx[1]-mn[1])/UnitsPerMetre, (mx[0]-mn[0])/UnitsPerMetre
	res.HalfLength = float32(math.Max(float64(mx[1]), float64(-mn[1])))
	res.DriveNIF = `mmw\cars\` + o.ID + `\drive.nif`
	return res, nil
}

// Logf reports conversion details (set by the caller).
var Logf = func(string, ...any) {}

// ParkedCollision gives static.nif a Havok box.
var ParkedCollision = false

// SnowAllCars gives every car snow on its tops (cars.json snow adds the windshield and its wipers).
var SnowAllCars = true

// FlipWinding reverses triangle winding (set from the constants sheet).
var FlipWinding = false

func shortName(m *crit.Material) string {
	n := m.Name
	if i := strings.LastIndex(n, "/"); i >= 0 {
		n = n[i+1:]
	}
	if i := strings.Index(n, "."); i >= 0 {
		n = n[:i]
	}
	if n == "" {
		n = fmt.Sprintf("%s_%x", m.Kind, m.ID&0xffffff)
	}
	return n
}

func sub(a, b [3]float32) [3]float32 { return [3]float32{a[0] - b[0], a[1] - b[1], a[2] - b[2]} }
func cross(a, b [3]float32) [3]float32 {
	return [3]float32{a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]}
}
func norm(a [3]float32) [3]float32 {
	l := float32(math.Sqrt(float64(a[0]*a[0] + a[1]*a[1] + a[2]*a[2])))
	if l == 0 || l != l || l > 1e30 {
		return [3]float32{0, 0, 1}
	}
	return [3]float32{a[0] / l, a[1] / l, a[2] / l}
}

func computeNormals(s *nif.Shape) {
	n := make([][3]float32, len(s.Pos))
	for _, t := range s.Tris {
		a, b, c := s.Pos[t[0]], s.Pos[t[1]], s.Pos[t[2]]
		f := cross(sub(b, a), sub(c, a))
		for _, v := range t {
			n[v][0] += f[0]
			n[v][1] += f[1]
			n[v][2] += f[2]
		}
	}
	for i := range n {
		n[i] = norm(n[i])
	}
	s.Normal = n
}

func computeTangents(s *nif.Shape) {
	t := make([][3]float32, len(s.Pos))
	bt := make([][3]float32, len(s.Pos))
	for _, tr := range s.Tris {
		p0, p1, p2 := s.Pos[tr[0]], s.Pos[tr[1]], s.Pos[tr[2]]
		u0, u1, u2 := s.UV[tr[0]], s.UV[tr[1]], s.UV[tr[2]]
		e1, e2 := sub(p1, p0), sub(p2, p0)
		du1, dv1, du2, dv2 := u1[0]-u0[0], u1[1]-u0[1], u2[0]-u0[0], u2[1]-u0[1]
		r := du1*dv2 - du2*dv1
		if r > -1e-12 && r < 1e-12 {
			continue
		}
		f := 1 / r
		tv := [3]float32{f * (dv2*e1[0] - dv1*e2[0]), f * (dv2*e1[1] - dv1*e2[1]), f * (dv2*e1[2] - dv1*e2[2])}
		bv := [3]float32{f * (du1*e2[0] - du2*e1[0]), f * (du1*e2[1] - du2*e1[1]), f * (du1*e2[2] - du2*e1[2])}
		for _, v := range tr {
			for k := 0; k < 3; k++ {
				t[v][k] += tv[k]
				bt[v][k] += bv[k]
			}
		}
	}
	for i := range t {
		t[i], bt[i] = norm(t[i]), norm(bt[i])
	}
	s.Tangent, s.Bitan = t, bt
}

// clipV is a vertex being cut: position and UV (interpolated together).
type clipV struct {
	p  [3]float32
	uv [2]float32
}

// clipPoly keeps the part of a convex polygon where keep*(uv[c] - v) >= 0 (Sutherland-Hodgman, one plane in UV).
func clipPoly(in []clipV, c int, v, keep float32) []clipV {
	var out []clipV
	n := len(in)
	for i := 0; i < n; i++ {
		a, b := in[i], in[(i+1)%n]
		da, db := keep*(a.uv[c]-v), keep*(b.uv[c]-v)
		if da >= 0 {
			out = append(out, a)
		}
		if (da >= 0) != (db >= 0) {
			t := da / (da - db)
			var q clipV
			for k := 0; k < 3; k++ {
				q.p[k] = a.p[k] + (b.p[k]-a.p[k])*t
			}
			for k := 0; k < 2; k++ {
				q.uv[k] = a.uv[k] + (b.uv[k]-a.uv[k])*t
			}
			q.uv[c] = v
			out = append(out, q)
		}
	}
	return out
}
