package car

import (
	"fmt"
	"math"

	"mmw/internal/nif"
)

const pileStep = 2.0

var pileLevels = []float32{0.35, 1.4, 3.0, 5.0, 7.5, 10.5}
var pileRefs = []uint8{18, 55, 91, 128, 164, 200}

const glassPileLevels = 4

type pilesIn struct {
	mn, mx                   [3]float32
	gw, gh                   int
	top                      []float32
	kind                     func(cell int) (uint8, float32)
	plane                    func([3]float32) (float32, float32)
	W, H                     float32
	flatN, bodyTex, glassTex string
}

func smooth01(x float64) float64 {
	x = math.Min(1, math.Max(0, x))
	return x * x * (3 - 2*x)
}

func (sb *snowBuild) piles(_ *part, in pilesIn) {
	nx := int((in.mx[0]-in.mn[0])/pileStep) + 2
	ny := int((in.mx[1]-in.mn[1])/pileStep) + 2
	n := nx * ny
	z := make([]float32, n)
	kind := make([]uint8, n)
	nz := make([]float32, n)
	cell := func(x, y float32) int {
		gx, gy := int(x-in.mn[0]), int(y-in.mn[1])
		if gx < 0 || gy < 0 || gx >= in.gw || gy >= in.gh {
			return -1
		}
		return gy*in.gw + gx
	}
	for j := 0; j < ny; j++ {
		for i := 0; i < nx; i++ {
			x, y := in.mn[0]+float32(i)*pileStep, in.mn[1]+float32(j)*pileStep
			c := cell(x, y)
			if c < 0 {
				continue
			}
			k, nzv := in.kind(c)
			if k == 0 || nzv < 0.72 {
				continue
			}
			zz := in.top[c]
			fx, fy := x-in.mn[0]-0.5, y-in.mn[1]-0.5
			gx, gy := int(fx), int(fy)
			if gx >= 0 && gy >= 0 && gx+1 < in.gw && gy+1 < in.gh {
				a, b, cc, d := in.top[gy*in.gw+gx], in.top[gy*in.gw+gx+1], in.top[(gy+1)*in.gw+gx], in.top[(gy+1)*in.gw+gx+1]
				if max(a, b, cc, d)-min(a, b, cc, d) < 2 {
					tx, ty := fx-float32(gx), fy-float32(gy)
					zz = (a*(1-tx)+b*tx)*(1-ty) + (cc*(1-tx)+d*tx)*ty
				}
			}
			z[j*nx+i], kind[j*nx+i], nz[j*nx+i] = zz, k, nzv
		}
	}
	for pass := 0; pass < 2; pass++ {
		keep := make([]bool, n)
		for j := 0; j < ny; j++ {
			for i := 0; i < nx; i++ {
				a := j*nx + i
				if kind[a] == 0 {
					continue
				}
				cnt := 0
				for dj := -1; dj <= 1; dj++ {
					for di := -1; di <= 1; di++ {
						ii, jj := i+di, j+dj
						if (di != 0 || dj != 0) && ii >= 0 && jj >= 0 && ii < nx && jj < ny && kind[jj*nx+ii] != 0 && abs32(z[jj*nx+ii]-z[a]) <= 2.6 {
							cnt++
						}
					}
				}
				keep[a] = cnt >= 5
			}
		}
		for a := range kind {
			if !keep[a] {
				kind[a] = 0
			}
		}
	}
	drop := make([]bool, n)
	for j := 0; j < ny; j++ {
		for i := 0; i < nx; i++ {
			a := j*nx + i
			if kind[a] == 0 {
				continue
			}
			for _, o := range [][2]int{{1, 0}, {0, 1}, {-1, 0}, {0, -1}} {
				ii, jj := i+o[0], j+o[1]
				if ii < 0 || jj < 0 || ii >= nx || jj >= ny {
					drop[a] = true
					continue
				}
				b := jj*nx + ii
				if kind[b] == 0 || abs32(z[b]-z[a]) > 2.6 {
					drop[a] = true
				}
			}
		}
	}
	const far = 1 << 20
	dist := make([]int, n)
	queue := make([]int, 0, n)
	for a := range dist {
		if kind[a] == 0 {
			dist[a] = -1
		} else if drop[a] {
			dist[a] = 0
			queue = append(queue, a)
		} else {
			dist[a] = far
		}
	}
	for q := 0; q < len(queue); q++ {
		a := queue[q]
		i, j := a%nx, a/nx
		for _, o := range [][2]int{{1, 0}, {0, 1}, {-1, 0}, {0, -1}} {
			ii, jj := i+o[0], j+o[1]
			if ii < 0 || jj < 0 || ii >= nx || jj >= ny {
				continue
			}
			b := jj*nx + ii
			if dist[b] > dist[a]+1 && abs32(z[b]-z[a]) <= 2.6 {
				dist[b] = dist[a] + 1
				queue = append(queue, b)
			}
		}
	}
	frac := make([]float32, n)
	edge := make([]float32, n)
	for a := range frac {
		if kind[a] == 0 {
			continue
		}
		x, y := float64(a%nx)*pileStep, float64(a/nx)*pileStep
		lump := 0.85 + 0.3*vnoise2(x/11, y/11, 7)
		slope := smooth01((float64(nz[a]) - 0.72) / (0.92 - 0.72))
		e := smooth01(float64(dist[a]) / 6)
		frac[a] = float32(slope * e * lump)
		edge[a] = float32(smooth01(float64(dist[a]) / 3))
	}
	for pass := 0; pass < 2; pass++ {
		f2 := make([]float32, n)
		for j := 0; j < ny; j++ {
			for i := 0; i < nx; i++ {
				a := j*nx + i
				if kind[a] == 0 {
					continue
				}
				sum, w := frac[a], float32(1)
				for _, o := range [][2]int{{1, 0}, {0, 1}, {-1, 0}, {0, -1}} {
					ii, jj := i+o[0], j+o[1]
					if ii >= 0 && jj >= 0 && ii < nx && jj < ny {
						b := jj*nx + ii
						if kind[b] != 0 && abs32(z[b]-z[a]) <= 2.6 {
							sum += frac[b]
							w++
						} else {
							w++
						}
					}
				}
				f2[a] = sum / w
			}
		}
		frac = f2
	}
	type vk struct{ a, region int }
	var bodyM, glassM []*mesh
	for range pileLevels {
		bodyM = append(bodyM, &mesh{})
	}
	for range pileLevels[:glassPileLevels] {
		glassM = append(glassM, &mesh{})
	}
	idx := make([]map[vk]uint16, len(pileLevels))
	for l := range idx {
		idx[l] = map[vk]uint16{}
	}
	var alphaB, alphaG [][]float32
	alphaB = make([][]float32, len(pileLevels))
	alphaG = make([][]float32, glassPileLevels)
	level := func(a, l int) float32 {
		return z[a] + max(0.25, pileLevels[l]*frac[a])
	}
	normal := func(a, l int) [3]float32 {
		i, j := a%nx, a/nx
		h := func(ii, jj int) (float32, bool) {
			if ii < 0 || jj < 0 || ii >= nx || jj >= ny {
				return 0, false
			}
			b := jj*nx + ii
			if kind[b] == 0 || abs32(z[b]-z[a]) > 2.6 {
				return 0, false
			}
			return level(b, l), true
		}
		c := level(a, l)
		dx, dy := float32(0), float32(0)
		if r, ok := h(i+1, j); ok {
			dx += r - c
		}
		if lft, ok := h(i-1, j); ok {
			dx += c - lft
		}
		if u, ok := h(i, j+1); ok {
			dy += u - c
		}
		if d, ok := h(i, j-1); ok {
			dy += c - d
		}
		return norm([3]float32{-dx / (2 * pileStep), -dy / (2 * pileStep), 1})
	}
	vert := func(m *mesh, l, a, region int) uint16 {
		key := vk{a, region}
		if v, ok := idx[l][key]; ok {
			return v
		}
		x, y := in.mn[0]+float32(a%nx)*pileStep, in.mn[1]+float32(a/nx)*pileStep
		p := [3]float32{x, y, level(a, l)}
		var uv [2]float32
		if region == 2 {
			ps, pt := in.plane([3]float32{x, y, z[a]})
			uv = [2]float32{ps / in.W, 1 - pt/in.H}
		} else {
			uv = [2]float32{(x - in.mn[0]) / (in.mx[0] - in.mn[0]), (in.mx[1] - y) / (in.mx[1] - in.mn[1])}
		}
		v := m.vert(p, normal(a, l), uv)
		idx[l][key] = v
		va := 0.6 + 0.4*edge[a]
		if region == 2 {
			alphaG[l] = append(alphaG[l], va)
		} else {
			alphaB[l] = append(alphaB[l], va)
		}
		return v
	}
	tris := 0
	addTri := func(c [3]int) {
		w := 0
		for _, a := range c {
			if kind[a] == 0 {
				return
			}
			if kind[a] == 2 {
				w++
			}
		}
		for p := 0; p < 3; p++ {
			if abs32(z[c[p]]-z[c[(p+1)%3]]) > 2.6 {
				return
			}
		}
		region := 1
		if w >= 2 {
			region = 2
		}
		tris++
		for l := range pileLevels {
			var m *mesh
			if region == 2 {
				if l >= glassPileLevels {
					continue
				}
				m = glassM[l]
			} else {
				m = bodyM[l]
			}
			if len(m.pos) > 65000 {
				continue
			}
			f := [3]uint16{vert(m, l, c[0], region), vert(m, l, c[1], region), vert(m, l, c[2], region)}
			a, b, cc := m.pos[f[0]], m.pos[f[1]], m.pos[f[2]]
			if cross(sub(b, a), sub(cc, a))[2] < 0 {
				f[1], f[2] = f[2], f[1]
			}
			m.tris = append(m.tris, f)
		}
	}
	for j := 0; j+1 < ny; j++ {
		for i := 0; i+1 < nx; i++ {
			a, b, c, d := j*nx+i, j*nx+i+1, (j+1)*nx+i, (j+1)*nx+i+1
			full := kind[a] != 0 && kind[b] != 0 && kind[c] != 0 && kind[d] != 0
			if full {
				addTri([3]int{a, b, d})
				addTri([3]int{a, d, c})
				continue
			}
			for _, t := range [][3]int{{a, b, d}, {a, d, c}, {a, b, c}, {b, d, c}} {
				ok := true
				for _, q := range t {
					ok = ok && kind[q] != 0
				}
				if ok {
					addTri(t)
					break
				}
			}
		}
	}
	for l, m := range bodyM {
		if len(m.tris) == 0 {
			continue
		}
		s := &nif.Shape{Name: fmt.Sprintf("MMWSnowLayer:%d", l), Alpha: 1, AlphaTest: true, AlphaRef: pileRefs[l],
			Diffuse: in.bodyTex, NormalTx: in.flatN, Specular: [3]float32{0.22, 0.22, 0.25}, Gloss: 10,
			Pos: m.pos, Normal: m.nrm, UV: m.uv, Tris: m.tris, VertexAlpha: alphaB[l]}
		if FlipWinding {
			for i := range s.Tris {
				s.Tris[i][1], s.Tris[i][2] = s.Tris[i][2], s.Tris[i][1]
			}
		}
		sb.body = append(sb.body, s)
	}
	for l, m := range glassM {
		if len(m.tris) == 0 {
			continue
		}
		s := m.shape(fmt.Sprintf("MMWSnowGlassLayer:%d", l), in.glassTex, in.flatN, [3]float32{0.22, 0.22, 0.25}, 10)
		s.AlphaTest, s.AlphaRef = true, pileRefs[l]
		s.VertexAlpha = append(append([]float32{}, alphaG[l]...), alphaG[l]...)
		sb.glass = append(sb.glass, s)
	}
	nb, ng := 0, 0
	if len(bodyM) > 0 {
		nb = len(bodyM[0].tris)
	}
	if len(glassM) > 0 {
		ng = len(glassM[0].tris)
	}
	Logf("snow: piles on a %.0f-unit grid: %d triangles a level on the body (%d levels, to %.1f units), %d on the windshield (%d levels)",
		pileStep, nb, len(pileLevels), pileLevels[len(pileLevels)-1], ng, glassPileLevels)
	_ = tris
}

// vnoise2: smooth value noise in [0,1)
func vnoise2(x, y float64, seed uint32) float64 {
	ix, iy := math.Floor(x), math.Floor(y)
	fx, fy := x-ix, y-iy
	fx, fy = fx*fx*(3-2*fx), fy*fy*(3-2*fy)
	h := func(i, j float64) float64 {
		u := uint32(int32(i))*374761393 + uint32(int32(j))*668265263 + seed*2246822519
		u = (u ^ (u >> 13)) * 1274126177
		return float64((u^(u>>16))&0xFFFF) / 65536
	}
	a, b, c, d := h(ix, iy), h(ix+1, iy), h(ix, iy+1), h(ix+1, iy+1)
	return (a*(1-fx)+b*fx)*(1-fy) + (c*(1-fx)+d*fx)*fy
}
