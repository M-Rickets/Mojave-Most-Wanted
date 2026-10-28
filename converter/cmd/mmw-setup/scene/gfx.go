// Package scene draws the setup window.
package scene

import (
	"image"
	"image/color"
	"image/draw"
	"math"
	"sync"

	"golang.org/x/image/font"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
	"golang.org/x/image/vector"
)

// Canvas draws at a scale into an RGBA image.
type Canvas struct {
	Img    *image.RGBA
	S      float64
	ox, oy float64
	rast   vector.Rasterizer
}

// Font faces are kept between frames (making one, and its glyph cache, costs more than a frame's drawing)
var (
	facesMu sync.Mutex
	faces   = map[faceKey]font.Face{}
)

type faceKey struct {
	f    *opentype.Font
	size float64
}

func NewCanvas(img *image.RGBA, scale float64) *Canvas {
	return &Canvas{Img: img, S: scale}
}

func rgba(hex uint32, a uint8) color.RGBA {
	r, g, b := uint8(hex>>16), uint8(hex>>8), uint8(hex)
	return color.RGBA{uint8(uint32(r) * uint32(a) / 255), uint8(uint32(g) * uint32(a) / 255), uint8(uint32(b) * uint32(a) / 255), a}
}

// Path is a polygon outline in design units.
type Path [][2]float64

func (c *Canvas) Fill(paths []Path, col color.RGBA) {
	minX, minY, maxX, maxY := math.Inf(1), math.Inf(1), math.Inf(-1), math.Inf(-1)
	for _, p := range paths {
		for _, q := range p {
			minX, minY = math.Min(minX, q[0]*c.S), math.Min(minY, q[1]*c.S)
			maxX, maxY = math.Max(maxX, q[0]*c.S), math.Max(maxY, q[1]*c.S)
		}
	}
	minX, maxX, minY, maxY = minX-c.ox, maxX-c.ox, minY-c.oy, maxY-c.oy
	r := image.Rect(int(math.Floor(minX)), int(math.Floor(minY)), int(math.Ceil(maxX))+1, int(math.Ceil(maxY))+1).Intersect(c.Img.Bounds())
	if r.Empty() {
		return
	}
	ox, oy := float64(r.Min.X)+c.ox, float64(r.Min.Y)+c.oy
	c.rast.Reset(r.Dx(), r.Dy())
	for _, p := range paths {
		if len(p) < 3 {
			continue
		}
		c.rast.MoveTo(float32(p[0][0]*c.S-ox), float32(p[0][1]*c.S-oy))
		for _, q := range p[1:] {
			c.rast.LineTo(float32(q[0]*c.S-ox), float32(q[1]*c.S-oy))
		}
		c.rast.ClosePath()
	}
	c.rast.Draw(c.Img, r, image.NewUniform(col), image.Point{})
}

func (c *Canvas) FillPath(p Path, col color.RGBA) { c.Fill([]Path{p}, col) }

func Rect(x, y, w, h float64) Path { return Path{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}} }

func Ellipse(cx, cy, rx, ry float64) Path {
	n := int(math.Max(rx, ry) * 3)
	if n < 10 {
		n = 10
	}
	if n > 64 {
		n = 64
	}
	p := make(Path, n)
	for i := 0; i < n; i++ {
		a := 2 * math.Pi * float64(i) / float64(n)
		p[i] = [2]float64{cx + rx*math.Cos(a), cy + ry*math.Sin(a)}
	}
	return p
}

func Circle(cx, cy, r float64) Path { return Ellipse(cx, cy, r, r) }

// RoundRect: a rectangle with rounded corners (radius r).
func RoundRect(x, y, w, h, r float64) Path {
	if r > h/2 {
		r = h / 2
	}
	if r > w/2 {
		r = w / 2
	}
	var p Path
	arc := func(cx, cy, a0 float64) {
		for i := 0; i <= 8; i++ {
			a := a0 + math.Pi/2*float64(i)/8
			p = append(p, [2]float64{cx + r*math.Cos(a), cy + r*math.Sin(a)})
		}
	}
	arc(x+w-r, y+r, -math.Pi/2)
	arc(x+w-r, y+h-r, 0)
	arc(x+r, y+h-r, math.Pi/2)
	arc(x+r, y+r, math.Pi)
	return p
}

// Stroke a polyline of width w as quads (with round joins as small circles).
func (c *Canvas) Stroke(pts [][2]float64, w float64, col color.RGBA, closed bool) {
	var paths []Path
	n := len(pts)
	segs := n - 1
	if closed {
		segs = n
	}
	for i := 0; i < segs; i++ {
		a, b := pts[i], pts[(i+1)%n]
		dx, dy := b[0]-a[0], b[1]-a[1]
		l := math.Hypot(dx, dy)
		if l < 1e-6 {
			continue
		}
		nx, ny := -dy/l*w/2, dx/l*w/2
		paths = append(paths, Path{{a[0] + nx, a[1] + ny}, {b[0] + nx, b[1] + ny}, {b[0] - nx, b[1] - ny}, {a[0] - nx, a[1] - ny}})
	}
	for _, p := range pts {
		paths = append(paths, Circle(p[0], p[1], w/2))
	}
	for _, p := range paths {
		c.FillPath(ensureCCW(p), col)
	}
}

func ensureCCW(p Path) Path {
	s := 0.0
	for i := range p {
		a, b := p[i], p[(i+1)%len(p)]
		s += a[0]*b[1] - b[0]*a[1]
	}
	if s < 0 {
		q := make(Path, len(p))
		for i := range p {
			q[i] = p[len(p)-1-i]
		}
		return q
	}
	return p
}

// Ring: a circle outline.
func (c *Canvas) Ring(cx, cy, r, w float64, col color.RGBA) {
	outer := Circle(cx, cy, r+w/2)
	inner := Circle(cx, cy, r-w/2)
	for i, j := 0, len(inner)-1; i < j; i, j = i+1, j-1 {
		inner[i], inner[j] = inner[j], inner[i]
	}
	c.Fill([]Path{outer, inner}, col)
}

// Arc stroke from a0 to a1 (radians).
func (c *Canvas) Arc(cx, cy, r, a0, a1, w float64, col color.RGBA) {
	var pts [][2]float64
	n := int(math.Max(4, math.Abs(a1-a0)/(math.Pi/24)))
	for i := 0; i <= n; i++ {
		a := a0 + (a1-a0)*float64(i)/float64(n)
		pts = append(pts, [2]float64{cx + r*math.Cos(a), cy + r*math.Sin(a)})
	}
	c.Stroke(pts, w, col, false)
}

func (c *Canvas) face(f *opentype.Font, size float64) font.Face {
	k := faceKey{f, size * c.S}
	facesMu.Lock()
	defer facesMu.Unlock()
	if fc, ok := faces[k]; ok {
		return fc
	}
	fc, err := opentype.NewFace(f, &opentype.FaceOptions{Size: size * c.S, DPI: 72, Hinting: font.HintingNone})
	if err != nil {
		return nil
	}
	faces[k] = fc
	return fc
}

// TextWidth in design units.
func (c *Canvas) TextWidth(f *opentype.Font, size float64, s string) float64 {
	fc := c.face(f, size)
	if fc == nil {
		return 0
	}
	return float64(font.MeasureString(fc, s)) / 64 / c.S
}

// Text draws s with its baseline at (x, y) in design units.
func (c *Canvas) Text(f *opentype.Font, size float64, x, y float64, s string, col color.RGBA) {
	fc := c.face(f, size)
	if fc == nil {
		return
	}
	d := font.Drawer{Dst: c.Img, Src: image.NewUniform(col), Face: fc}
	d.Dot = fixed.Point26_6{X: fixed.Int26_6((x*c.S - c.ox) * 64), Y: fixed.Int26_6((y*c.S - c.oy) * 64)}
	d.DrawString(s)
}

// TextCentered at cx.
func (c *Canvas) TextCentered(f *opentype.Font, size, cx, y float64, s string, col color.RGBA) {
	c.Text(f, size, cx-c.TextWidth(f, size, s)/2, y, s, col)
}

// Clear the whole image.
func (c *Canvas) Clear(col color.RGBA) {
	draw.Draw(c.Img, c.Img.Bounds(), image.NewUniform(col), image.Point{}, draw.Src)
}

// Clip: f draws into a separate image the size of the rect (design units), which then goes over this one
func (c *Canvas) Clip(x, y, w, h float64, f func(c *Canvas)) {
	r := image.Rect(int(x*c.S-c.ox), int(y*c.S-c.oy), int(math.Ceil((x+w)*c.S-c.ox)), int(math.Ceil((y+h)*c.S-c.oy))).Intersect(c.Img.Bounds())
	if r.Empty() {
		return
	}
	tmp := image.NewRGBA(image.Rect(0, 0, r.Dx(), r.Dy()))
	sub := &Canvas{Img: tmp, S: c.S, ox: c.ox + float64(r.Min.X), oy: c.oy + float64(r.Min.Y)}
	f(sub)
	draw.Draw(c.Img, r, tmp, image.Point{}, draw.Over)
}
