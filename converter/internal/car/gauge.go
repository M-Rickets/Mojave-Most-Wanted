package car

import (
	"os"
	"path/filepath"
	"strings"

	"mmw/internal/dds"
	"mmw/internal/nif"
)

// The dash gauges.

const GaugeW, GaugeH = 256, 128
const GaugePath = `textures\mmw\gauge.dds`

type GaugeSpec struct {
	Off   bool    `json:"off,omitempty"`
	Depth float32 `json:"depth,omitempty"`
	Up    float32 `json:"up,omitempty"`
	Side  float32 `json:"side,omitempty"`
	Width float32 `json:"width,omitempty"`
}

// gaugeShape: the face, both sides (the back is never seen; it keeps the face from vanishing whichever winding the game
// culls), its normal towards the driver.
func gaugeShape(sw steerWheel, g *GaugeSpec, flatN string) *nif.Shape {
	var s GaugeSpec
	if g != nil {
		s = *g
	}
	if s.Depth == 0 {
		s.Depth = 0.85
	}
	if s.Up == 0 {
		s.Up = 0.45
	}
	if s.Width == 0 {
		s.Width = 1.25
	}
	R := sw.Radius
	A := norm(sw.Axis)
	U := sub([3]float32{0, 0, 1}, scale(A, A[2]))
	if dot(U, U) < 1e-4 {
		U = [3]float32{0, 1, 0}
	}
	U = norm(U)
	Rt := norm(cross(scale(A, -1), U))
	c := add(add(add(sw.Centre, scale(A, -s.Depth*R)), scale(U, s.Up*R)), scale(Rt, s.Side*R))
	hw, hh := s.Width*R/2, s.Width*R/4
	corner := func(x, y float32) [3]float32 { return add(add(c, scale(Rt, x*hw)), scale(U, y*hh)) }
	sh := &nif.Shape{Name: "MMWGauge", Alpha: 1, Diffuse: GaugePath, NormalTx: flatN,
		Specular: [3]float32{0.2, 0.2, 0.2}, Gloss: 60, EmissiveColor: [3]float32{1, 1, 1}, Emissive: 1}
	ps := [4][3]float32{corner(-1, 1), corner(1, 1), corner(1, -1), corner(-1, -1)}
	uv := [4][2]float32{{0, 0}, {1, 0}, {1, 1}, {0, 1}}
	for side := 0; side < 2; side++ {
		n := A
		if side == 1 {
			n = scale(A, -1)
		}
		for i := 0; i < 4; i++ {
			sh.Pos = append(sh.Pos, ps[i])
			sh.Normal = append(sh.Normal, n)
			sh.UV = append(sh.UV, uv[i])
		}
	}
	sh.Tris = [][3]uint16{{0, 3, 2}, {0, 2, 1}, {4, 5, 6}, {4, 6, 7}}
	if dot(cross(sub(ps[3], ps[0]), sub(ps[2], ps[0])), A) < 0 {
		sh.Tris = [][3]uint16{{0, 2, 3}, {0, 1, 2}, {4, 6, 5}, {4, 7, 6}}
	}
	Logf("gauge: centre (%.1f %.1f %.1f), %.2f m across", c[0], c[1], c[2], s.Width*R/UnitsPerMetre)
	return sh
}

// gaugeTexture writes the shared face texture once.
func gaugeTexture(dataDir string) {
	p := filepath.Join(dataDir, filepath.FromSlash(strings.ReplaceAll(GaugePath, `\`, "/")))
	if _, err := os.Stat(p); err == nil {
		return
	}
	px := make([]byte, GaugeW*GaugeH*4)
	for i := 0; i < len(px); i += 4 {
		px[i], px[i+1], px[i+2], px[i+3] = 10, 9, 8, 255
	}
	os.MkdirAll(filepath.Dir(p), 0o755)
	os.WriteFile(p, dds.Image(GaugeW, GaugeH, px), 0o644)
}
