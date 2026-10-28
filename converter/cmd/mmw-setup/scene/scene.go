package scene

import (
	_ "embed"
	"image"
	"image/color"
	"math"
	"strings"
	"sync"

	"golang.org/x/image/font/opentype"
)

//go:embed ArchivoBlack-Regular.ttf
var ttfTitle []byte

//go:embed SpaceMono-Regular.ttf
var ttfMono []byte

//go:embed SpaceMono-Bold.ttf
var ttfMonoBold []byte

//go:embed Poppins-Medium.ttf
var ttfBody []byte

//go:embed Poppins-SemiBold.ttf
var ttfBodyBold []byte

var fTitle, fMono, fMonoBold, fBody, fBodyBold *opentype.Font

func init() {
	p := func(b []byte) *opentype.Font { f, _ := opentype.Parse(b); return f }
	fTitle, fMono, fMonoBold, fBody, fBodyBold = p(ttfTitle), p(ttfMono), p(ttfMonoBold), p(ttfBody), p(ttfBodyBold)
}

// The design size.
const W, H = 560, 578

// Palette (Night City Shred's).
const (
	cPaper  = 0xF6EFE3
	cInk    = 0x16141A
	cPink   = 0xFF4FA3
	cPinkLt = 0xF9B6D6
	cTeal   = 0x2EC4B6
	cYellow = 0xFFD23F
	cOrange = 0xFF9F1C
	cPurple = 0x9B5DE5
	cGray   = 0x8C8790
	cLilac  = 0xF2D3EE
	cGrid   = 0xF08FCB
)

type StepState int

const (
	Pending StepState = iota
	Active
	Done
	Warn
	Skipped
)

const (
	BtnReady = iota
	BtnWorking
	BtnDone
	BtnAction
	BtnUpdate
)

type State struct {
	Exe          string
	Button       string
	BtnLook      int
	Steps        [5]string
	StepSt       [5]StepState
	Progress     float64
	Status       string
	Links        []string
	Hover        string
	Pressed      bool
	T            float64
	DoneT        float64
	CarMoves     bool
	Walker       bool
	Pills        []string
	PillOn       []bool
	Menu         bool
	MenuTitle    string
	MenuItems    []string
	MenuOn       []bool
	MenuGroups   []string
	SideL, SideR []Side
	Info         bool
	InfoTitle    string
	InfoLines    []Line
	InfoCols     int
}

type Hit struct {
	Name       string
	X, Y, W, H float64
}

// Render draws the window and returns the clickable areas (design units).
func Render(img *image.RGBA, scale float64, s *State) []Hit {
	c := NewCanvas(img, scale)
	var hits []Hit
	bgMu.Lock()
	if bg == nil || bg.Bounds() != img.Bounds() || bgScale != scale {
		bg = image.NewRGBA(img.Bounds())
		bgScale = scale
		b := NewCanvas(bg, scale)
		b.Clear(rgba(cPaper, 255))
		for y := 46.0; y < 430; y += 18 {
			for x := 12.0; x < W; x += 18 {
				b.FillPath(Circle(x, y, 0.8), rgba(0xD9CFC0, 255))
			}
		}
		floorGrid(b)
		title(b)
		b.TextCentered(fMono, 10.5, W/2, 172, "Fallout: New Vegas, enhanced  ·  one-button setup", rgba(cGray, 255))
	}
	copy(img.Pix, bg.Pix)
	bgMu.Unlock()
	floor(c, s)
	memphis(c, s.T)
	hits = append(hits, button(c, s))
	steps(c, s)
	hits = append(hits, pills(c, s)...)
	progress(c, s)
	st := "> " + s.Status
	if len(st) > 64 {
		st = st[:61] + "..."
	}
	c.Text(fMono, 11, 53, 459, st, rgba(cInk, 255))
	if math.Mod(s.T, 1.0) < 0.55 {
		x := 53 + c.TextWidth(fMono, 11, st) + 2
		c.FillPath(Rect(x, 449, 6.5, 12), rgba(cInk, 255))
	}
	if s.DoneT >= 0 && s.T-s.DoneT < 3.2 {
		confetti(c, s.T-s.DoneT)
	}
	hits = append(hits, side(c, s.SideL, 18, s.Hover)...)
	hits = append(hits, side(c, s.SideR, W-18-sideW, s.Hover)...)
	if s.Menu {
		hits = menu(c, s)
	}
	if s.Info {
		hits = info(c, s)
	}
	hits = append(hits, titleBar(c, s)...)
	c.Stroke([][2]float64{{0.75, 0.75}, {W - 0.75, 0.75}, {W - 0.75, H - 0.75}, {0.75, H - 0.75}}, 1.5, rgba(cInk, 255), true)
	return hits
}

func titleBar(c *Canvas, s *State) []Hit {
	var hits []Hit
	c.FillPath(Rect(0, 0, W, 33), rgba(cPaper, 255))
	c.FillPath(Rect(0, 33, W, 1.5), rgba(cInk, 255))
	c.Text(fMono, 10.5, 16, 21, s.Exe, rgba(cInk, 255))
	x := 16 + c.TextWidth(fMono, 10.5, s.Exe) + 10
	for i, col := range []uint32{cPink, cTeal, cYellow} {
		c.FillPath(Rect(x+float64(i)*9, 13, 6, 6), rgba(col, 255))
	}
	lx := 480.0
	for i := len(s.Links) - 1; i >= 0; i-- {
		l := s.Links[i]
		w := c.TextWidth(fMono, 10.5, l)
		lx -= w
		col := rgba(cGray, 255)
		if s.Hover == l {
			col = rgba(cPink, 255)
			c.FillPath(Rect(lx, 24, w, 1.2), col)
		}
		c.Text(fMono, 10.5, lx, 21, l, col)
		hits = append(hits, Hit{l, lx - 4, 4, w + 8, 26})
		lx -= 14
	}
	mc, cc := rgba(cInk, 255), rgba(cInk, 255)
	if s.Hover == "minimise" {
		c.FillPath(Rect(486, 4, 30, 26), rgba(cPinkLt, 255))
	}
	if s.Hover == "close" {
		c.FillPath(Rect(524, 4, 30, 26), rgba(cPink, 255))
		cc = rgba(cPaper, 255)
	}
	c.FillPath(Rect(495, 17, 12, 1.6), mc)
	c.Stroke([][2]float64{{534, 12}, {544, 22}}, 1.6, cc, false)
	c.Stroke([][2]float64{{544, 12}, {534, 22}}, 1.6, cc, false)
	hits = append(hits, Hit{"minimise", 486, 4, 30, 26}, Hit{"close", 524, 4, 30, 26})
	return hits
}

func title(c *Canvas) {
	cx, cy, r := 280.0, 98.0, 58.0
	c.FillPath(Circle(cx, cy, r), rgba(cPinkLt, 255))
	for i, y := 0, cy+6; y < cy+r; i, y = i+1, y+7 {
		c.FillPath(Rect(cx-r, y, 2*r, 1.5+float64(i)*0.9), rgba(cPaper, 255))
	}
	t1 := "MOJAVE"
	w1 := c.TextWidth(fTitle, 34, t1)
	c.Text(fTitle, 34, W/2-w1/2+2.5, 82+2.5, t1, rgba(cPink, 255))
	c.Text(fTitle, 34, W/2-w1/2, 82, t1, rgba(cInk, 255))
	t2 := "ENHANCED"
	size := 50.0
	for c.TextWidth(fTitle, size, t2) > 380 && size > 30 {
		size -= 1
	}
	w2 := c.TextWidth(fTitle, size, t2)
	c.Text(fTitle, size, W/2-w2/2+4, 140+4, t2, rgba(cPink, 255))
	c.Text(fTitle, size, W/2-w2/2, 140, t2, rgba(cInk, 255))
}

func memphis(c *Canvas, t float64) {
	bob := func(ph float64) float64 { return 2 * math.Sin(t*1.3+ph) }
	var sq [][2]float64
	for i := 0; i <= 24; i++ {
		x := 28 + float64(i)*1.9
		sq = append(sq, [2]float64{x, 84 + bob(0) + 4*math.Sin(float64(i)/24*2*math.Pi*1.5)})
	}
	c.Stroke(sq, 3, rgba(cTeal, 255), false)
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			c.FillPath(Circle(34+float64(i)*7, 122+float64(j)*7+bob(1), 1.7), rgba(cPurple, 255))
		}
	}
	c.FillPath(halfDisc(40, 214+bob(2), 17), rgba(cYellow, 255))
	c.FillPath(Rect(20, 214+bob(2), 40, 2), rgba(cInk, 255))
	plus(c, 34, 404+bob(3), 7, 2.4, cTeal)
	c.Stroke([][2]float64{{505, 62 + bob(4)}, {517, 84 + bob(4)}, {493, 84 + bob(4)}}, 2, rgba(cInk, 255), true)
	plus(c, 512, 136+bob(5), 8, 2.8, cYellow)
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			c.FillPath(Circle(510+float64(i)*7, 186+float64(j)*7+bob(6), 1.7), rgba(cPink, 255))
		}
	}
	var zz [][2]float64
	for i := 0; i <= 5; i++ {
		y := 248.0
		if i%2 == 1 {
			y = 240
		}
		zz = append(zz, [2]float64{485 + float64(i)*7, y + bob(7)})
	}
	c.Stroke(zz, 2.6, rgba(cPurple, 255), false)
	c.Ring(530, 393+bob(8), 9, 2.2, rgba(cPink, 255))
}

func halfDisc(cx, cy, r float64) Path {
	var p Path
	for i := 0; i <= 24; i++ {
		a := math.Pi + math.Pi*float64(i)/24
		p = append(p, [2]float64{cx + r*math.Cos(a), cy + r*math.Sin(a)})
	}
	return p
}

func plus(c *Canvas, x, y, r, w float64, col uint32) {
	c.FillPath(Rect(x-r, y-w/2, 2*r, w), rgba(col, 255))
	c.FillPath(Rect(x-w/2, y-r, w, 2*r), rgba(col, 255))
}

func button(c *Canvas, s *State) Hit {
	x, y, w, h := 148.0, 195.0, 264.0, 60.0
	off := 6.0
	if s.Pressed {
		off = 2
		x += 4
		y += 4
	} else if s.Hover == "button" {
		off = 7.5
		x -= 1
		y -= 1
	}
	var fill uint32 = cYellow
	switch s.BtnLook {
	case BtnDone:
		fill = cTeal
	case BtnAction:
		fill = cOrange
	case BtnUpdate:
		fill = cPurple
	}
	c.FillPath(RoundRect(x+off, y+off, w, h, 9), rgba(cInk, 255))
	c.FillPath(RoundRect(x, y, w, h, 9), rgba(cInk, 255))
	c.FillPath(RoundRect(x+3, y+3, w-6, h-6, 7), rgba(fill, 255))
	if s.BtnLook == BtnWorking {
		c.Clip(x+3, y+3, w-6, h-6, func(c *Canvas) {
			sh := math.Mod(s.T*40, 24)
			for k := -4.0; k < 16; k++ {
				x0 := x + k*24 + sh
				c.FillPath(Path{{x0, y + h}, {x0 + 12, y + h}, {x0 + 12 + h, y}, {x0 + h, y}}, rgba(0xF2B705, 255))
			}
		})
	}
	label := s.Button
	if s.BtnLook == BtnWorking {
		n := int(s.T*3) % 3
		label = strings.TrimRight(label, ".") + strings.Repeat(".", n)
	}
	size := 22.0
	for c.TextWidth(fTitle, size, label) > w-30 && size > 12 {
		size -= 1
	}
	tc := rgba(cInk, 255)
	if s.BtnLook == BtnUpdate {
		tc = rgba(cPaper, 255)
	}
	c.TextCentered(fTitle, size, x+w/2, y+h/2+size*0.36, label, tc)
	if s.BtnLook == BtnAction {
		c.FillPath(Circle(x+w-4, y+4, 11), rgba(cInk, 255))
		c.FillPath(Circle(x+w-4, y+4, 9), rgba(cOrange, 255))
		c.TextCentered(fTitle, 13, x+w-4, y+9, "!", rgba(cInk, 255))
	}
	return Hit{"button", 148, 195, w + 6, h + 6}
}

func steps(c *Canvas, s *State) {
	for i := 0; i < 5; i++ {
		y := 278.0 + float64(i)*26.3
		cx := 150.0
		txt := rgba(cInk, 255)
		f := fBody
		switch s.StepSt[i] {
		case Pending:
			c.Ring(cx, y, 7.5, 1.6, rgba(0xB9B2BB, 255))
			txt = rgba(cGray, 255)
		case Skipped:
			c.Ring(cx, y, 7.5, 1.6, rgba(0xB9B2BB, 255))
			c.FillPath(Rect(cx-4, y-0.8, 8, 1.6), rgba(0xB9B2BB, 255))
			txt = rgba(cGray, 255)
		case Active:
			a := s.T * 6
			c.Arc(cx, y, 7, a, a+math.Pi*1.4, 2.2, rgba(cPink, 255))
			f = fBodyBold
		case Done:
			c.FillPath(Circle(cx, y, 8.5), rgba(cTeal, 255))
			c.Stroke([][2]float64{{cx - 4, y + 0.2}, {cx - 1.2, y + 3}, {cx + 4.2, y - 3}}, 2, rgba(cPaper, 255), false)
		case Warn:
			c.FillPath(Circle(cx, y, 8.5), rgba(cOrange, 255))
			c.TextCentered(fTitle, 11, cx, y+4, "!", rgba(cInk, 255))
			f = fBodyBold
		}
		c.Text(f, 13.5, 170, y+5, s.Steps[i], txt)
	}
}

func progress(c *Canvas, s *State) {
	x, y, w, h := 52.0, 426.0, 456.0, 14.0
	c.FillPath(RoundRect(x, y, w, h, 7), rgba(cInk, 255))
	c.FillPath(RoundRect(x+2, y+2, w-4, h-4, 5), rgba(cPaper, 255))
	fx, fw := x+2, 0.0
	if s.Progress < 0 {
		fw = (w - 4) * 0.28
		ph := math.Mod(s.T*0.6, 2)
		if ph > 1 {
			ph = 2 - ph
		}
		fx = x + 2 + (w-4-fw)*ph
	} else {
		fw = (w - 4) * math.Min(1, s.Progress)
	}
	if fw > 1 {
		c.Clip(fx, y+2, fw, h-4, func(c *Canvas) {
			c.FillPath(RoundRect(fx, y+2, fw, h-4, 5), rgba(cTeal, 255))
			sh := math.Mod(s.T*20, 14)
			for k := -2.0; k < w/14+2; k++ {
				x0 := x + k*14 + sh
				c.FillPath(Path{{x0, y + h}, {x0 + 6, y + h}, {x0 + 6 + h, y}, {x0 + h, y}}, rgba(0x25A99D, 255))
			}
		})
	}
}

var (
	bgMu    sync.Mutex
	bg      *image.RGBA
	bgScale float64
)

// The floor: a lilac band with a pink perspective grid (drawn once), and the car
func floorGrid(c *Canvas) {
	top := 430.0
	for i := 0; i < 30; i++ {
		y := top + float64(i)*(H-top)/30
		a := uint8(40 + i*5)
		c.FillPath(Rect(0, y, W, (H-top)/30+1), rgba(cLilac, a))
	}
	hz := 486.0
	for i := -14; i <= 14; i++ {
		x0 := W/2 + float64(i)*22
		x1 := W/2 + float64(i)*95
		c.Stroke([][2]float64{{x0, hz}, {x1, H}}, 0.8, rgba(cGrid, 150), false)
	}
	for k := 0.0; k < 9; k++ {
		y := hz + math.Pow(k/8, 1.8)*(H-hz)
		c.FillPath(Rect(0, y, W, 0.8), rgba(cGrid, 150))
	}
}

func floor(c *Canvas, s *State) {
	cx := W / 2.0
	if s.Walker {
		if s.CarMoves {
			cx = math.Mod(s.T*50, W+120) - 60
		}
		walker(c, cx, 538, s.T, s.CarMoves)
		return
	}
	if s.CarMoves {
		cx = math.Mod(s.T*90, W+260) - 130
	}
	car(c, cx, 538, s.T, s.CarMoves)
}

// An original low two-door shape (no real car's): pink body, dark glass, yellow wheels like the board's.
func car(c *Canvas, cx, base, t float64, moving bool) {
	b := func(x, y float64) [2]float64 { return [2]float64{cx + x, base + y} }
	c.FillPath(Ellipse(cx, base+2, 62, 5), rgba(0xC9A8C6, 160))
	body := Path{b(-58, -6), b(-60, -16), b(-52, -20), b(-24, -22), b(-8, -34), b(22, -34), b(36, -23), b(56, -19), b(60, -11), b(58, -6)}
	c.FillPath(body, rgba(cInk, 255))
	inner := Path{b(-55.5, -8.5), b(-57, -15), b(-51, -18), b(-23, -20), b(-7, -31.5), b(21, -31.5), b(34, -21), b(54, -17), b(57.5, -11), b(55.5, -8.5)}
	c.FillPath(inner, rgba(cPink, 255))
	c.FillPath(Path{b(-19, -22), b(-6, -30), b(5, -30), b(5, -22)}, rgba(0x2B2B3A, 255))
	c.FillPath(Path{b(8, -22), b(8, -30), b(20, -30), b(30, -22)}, rgba(0x2B2B3A, 255))
	c.FillPath(Rect(cx-55, base-13, 8, 2.4), rgba(cYellow, 255))
	c.FillPath(Rect(cx+50, base-15, 7, 3), rgba(0xFFF3B0, 255))
	c.FillPath(Rect(cx-30, base-14, 60, 1.4), rgba(0xD13F87, 255))
	for _, wx := range []float64{-36, 38} {
		c.FillPath(Circle(cx+wx, base-5, 9.5), rgba(cInk, 255))
		c.FillPath(Circle(cx+wx, base-5, 7), rgba(cYellow, 255))
		a := 0.0
		if moving {
			a = -t * 9
		}
		c.Stroke([][2]float64{{cx + wx + 4*math.Cos(a), base - 5 + 4*math.Sin(a)}, {cx + wx - 4*math.Cos(a), base - 5 - 4*math.Sin(a)}}, 1.4, rgba(cInk, 255), false)
	}
}

func confetti(c *Canvas, dt float64) {
	cols := []uint32{cPink, cTeal, cYellow, cPurple, cOrange}
	for i := 0; i < 34; i++ {
		seed := float64(i)*12.9898 + 7
		r1 := frac(math.Sin(seed) * 43758.5453)
		r2 := frac(math.Sin(seed*1.7) * 24634.6345)
		x := 150 + r1*270 + 40*math.Sin(dt*2+r2*6)
		y := 200 + (r2*0.4+dt*0.55)*260 - 60*math.Exp(-dt*3)
		a := dt*4 + r1*6
		l := 4.0
		c.Stroke([][2]float64{{x - l*math.Cos(a), y - l*math.Sin(a)}, {x + l*math.Cos(a), y + l*math.Sin(a)}}, 2.2, rgba(cols[i%len(cols)], 255), false)
	}
}

func frac(x float64) float64 { return x - math.Floor(x) }

// Icon draws the program's icon (a size x size image): the striped sun and the car on a paper tile.
func Icon(img *image.RGBA, size int) {
	s := float64(size) / 128
	c := NewCanvas(img, s)
	c.FillPath(RoundRect(4, 4, 120, 120, 22), rgba(cInk, 255))
	c.FillPath(RoundRect(9, 9, 110, 110, 18), rgba(cPaper, 255))
	cx, cy, r := 64.0, 58.0, 36.0
	c.FillPath(Circle(cx, cy, r), rgba(cPink, 255))
	for i, y := 0, cy+4; y < cy+r; i, y = i+1, y+7 {
		c.FillPath(Rect(cx-r, y, 2*r, 2+float64(i)*1.2), rgba(cPaper, 255))
	}
	c.FillPath(Rect(14, 96, 100, 3), rgba(cGrid, 255))
	car(c, 64, 100, 0, false)
}

// The small pills under the checklist (names double as their hit names, lower case)
func pills(c *Canvas, s *State) []Hit {
	var hits []Hit
	if len(s.Pills) == 0 {
		return nil
	}
	gap := 14.0
	ws := make([]float64, len(s.Pills))
	for i, p := range s.Pills {
		ws[i] = c.TextWidth(fMonoBold, 10.5, p) + 26
	}
	var rows [][]int
	cur, curW := []int{}, -gap
	for i := range s.Pills {
		if len(cur) > 0 && curW+gap+ws[i] > W-40 {
			rows, cur, curW = append(rows, cur), []int{}, -gap
		}
		cur, curW = append(cur, i), curW+gap+ws[i]
	}
	rows = append(rows, cur)
	y := 396.0 - 15*float64(len(rows)-1)
	for _, row := range rows {
		total := -gap
		for _, i := range row {
			total += ws[i] + gap
		}
		x := W/2 - total/2
		for _, i := range row {
			p := s.Pills[i]
			on := i < len(s.PillOn) && s.PillOn[i]
			name := strings.ToLower(p)
			hov := s.Hover == name
			c.FillPath(RoundRect(x+3, y+3, ws[i], 22, 11), rgba(cInk, 255))
			c.FillPath(RoundRect(x, y, ws[i], 22, 11), rgba(cInk, 255))
			var fill uint32 = cPaper
			if on {
				fill = cTeal
			} else if hov {
				fill = cPinkLt
			}
			c.FillPath(RoundRect(x+2, y+2, ws[i]-4, 18, 9), rgba(fill, 255))
			c.TextCentered(fMonoBold, 10.5, x+ws[i]/2, y+15, p, rgba(cInk, 255))
			hits = append(hits, Hit{name, x, y, ws[i] + 3, 25})
			x += ws[i] + gap
		}
		y += 30
	}
	return hits
}

// MenuClose is the panel's close pill's hit name.
const MenuClose = "close panel"

// The panel: a title, a DONE pill, then the items as pills wrapping in rows, under their headings
func menu(c *Canvas, s *State) []Hit {
	var hits []Hit
	x0, y0, w := 26.0, 44.0, float64(W-52)
	minH, maxH := 400.0, float64(H)-y0-14
	type spot struct{ x, y, w float64 }
	col := 118.0
	layout := func(size float64) ([]spot, []float64, float64) {
		left, right := x0+18+col, x0+w-18
		x, y := left, y0+50
		var at []spot
		var heads []float64
		prev := "\x00"
		for i, it := range s.MenuItems {
			g := ""
			if i < len(s.MenuGroups) {
				g = s.MenuGroups[i]
			}
			hy := -1.0
			if g != prev {
				if i > 0 {
					x, y = left, y+31
				}
				hy = y
				prev = g
			}
			pw := c.TextWidth(fMonoBold, size, it) + 22
			if x+pw > right && x > left {
				x, y = left, y+28
			}
			at = append(at, spot{x, y, pw})
			heads = append(heads, hy)
			x += pw + 10
		}
		return at, heads, y + 25
	}
	size := 10.5
	at, heads, bottom := layout(size)
	for bottom+12 > y0+maxH && size > 7.5 {
		size -= 0.5
		at, heads, bottom = layout(size)
	}
	h := math.Min(maxH, math.Max(minH, bottom+16-y0))
	c.FillPath(RoundRect(x0+5, y0+5, w, h, 14), rgba(cInk, 255))
	c.FillPath(RoundRect(x0, y0, w, h, 14), rgba(cInk, 255))
	c.FillPath(RoundRect(x0+2, y0+2, w-4, h-4, 12), rgba(cPaper, 255))
	c.Text(fTitle, 17, x0+18, y0+30, s.MenuTitle, rgba(cInk, 255))
	pill := func(x, y, pw float64, label, name string, on bool, size float64) {
		c.FillPath(RoundRect(x+3, y+3, pw, 22, 11), rgba(cInk, 255))
		c.FillPath(RoundRect(x, y, pw, 22, 11), rgba(cInk, 255))
		var fill uint32 = cPaper
		if on {
			fill = cTeal
		} else if s.Hover == name {
			fill = cPinkLt
		}
		c.FillPath(RoundRect(x+2, y+2, pw-4, 18, 9), rgba(fill, 255))
		c.TextCentered(fMonoBold, size, x+pw/2, y+15, label, rgba(cInk, 255))
		hits = append(hits, Hit{name, x, y, pw + 3, 25})
	}
	dw := c.TextWidth(fMonoBold, 10.5, "DONE") + 30
	pill(x0+w-18-dw, y0+12, dw, "DONE", MenuClose, false, 10.5)
	for i, it := range s.MenuItems {
		if heads[i] >= 0 && i < len(s.MenuGroups) && s.MenuGroups[i] != "" {
			words := strings.Fields(strings.ToUpper(s.MenuGroups[i]))
			var lines []string
			cur := ""
			for _, wd := range words {
				if cur != "" && c.TextWidth(fMono, 9, cur+" "+wd) > col-10 {
					lines, cur = append(lines, cur), wd
				} else if cur == "" {
					cur = wd
				} else {
					cur += " " + wd
				}
			}
			lines = append(lines, cur)
			ly := heads[i] + 15 - float64(len(lines)-1)*5.5
			for _, l := range lines {
				c.Text(fMono, 9, x0+18, ly, l, rgba(cGray, 255))
				ly += 11
			}
		}
		on := i < len(s.MenuOn) && s.MenuOn[i]
		pill(at[i].x, at[i].y, at[i].w, it, strings.ToLower(it), on, size)
	}
	hits = append(hits, Hit{"", x0, y0, w, h})
	return hits
}

func walker(c *Canvas, cx, base, t float64, moving bool) {
	ph := 0.0
	if moving {
		ph = t * 2 * math.Pi * 1.0
	}
	bob := -0.5 * math.Sin(t*1.6)
	if moving {
		bob = -1.5 * math.Abs(math.Sin(ph))
	}
	c.FillPath(Ellipse(cx+2, base+2, 22, 3.5), rgba(0xC9A8C6, 160))
	hipY := base - 30 + bob
	if !moving {
		hipY = base - 32.6 + bob*0.3
	}
	wind := math.Sin(t*3.1) + 0.5*math.Sin(t*5.3+1)
	draw := func(ox, oy float64, col color.RGBA) {
		X := func(x float64) float64 { return x + ox }
		Y := func(y float64) float64 { return y + oy }
		sy := hipY - 26
		for side := 0.0; side < 2; side++ {
			p := ph + side*math.Pi
			fx := cx + 11*math.Sin(p)
			if !moving {
				fx = cx + (side*2-1)*4
			}
			lift := 0.0
			if moving && math.Cos(p) > 0 {
				lift = 5 * math.Cos(p)
			}
			fy := base - 2 - lift
			hx, hy := cx+side*1.5, hipY
			d := math.Min(math.Hypot(fx-hx, fy-hy), 30.9)
			a := math.Atan2(fy-hy, fx-hx)
			k := math.Acos(math.Max(-1, math.Min(1, d/31)))
			kx, ky := hx+15.5*math.Cos(a-k), hy+15.5*math.Sin(a-k)
			c.Stroke([][2]float64{{X(hx), Y(hy)}, {X(kx), Y(ky)}, {X(fx), Y(fy)}}, 4.4, col, false)
			c.FillPath(RoundRect(X(fx-3), Y(fy-2.4), 8.5, 4.2, 2), col)
		}
		sw := 0.0
		if moving {
			sw = 2.5*math.Sin(ph*2) + 1.2*wind
		}
		robe := Path{{X(cx - 7), Y(sy)}, {X(cx + 7), Y(sy)}, {X(cx + 10), Y(hipY - 4)}, {X(cx + 12 + sw*0.3), Y(hipY + 19)},
			{X(cx + 3), Y(hipY + 21)}, {X(cx - 6), Y(hipY + 21)}, {X(cx - 14 - sw), Y(hipY + 22)}, {X(cx - 10), Y(hipY - 4)}}
		c.FillPath(robe, col)
		{
			bx, by := cx-27, hipY+1
			mx, my := cx-3, sy-34
			L := math.Hypot(mx-bx, my-by)
			ux, uy := (mx-bx)/L, (my-by)/L
			nx, ny := -uy, ux
			at := func(u, v float64) [2]float64 { return [2]float64{X(bx + ux*u + nx*v), Y(by + uy*u + ny*v)} }
			c.FillPath(Path{at(0, -4.4), at(0, 4.4), at(5, 3.6), at(19, 2.0), at(20, -1.8)}, col)
			c.FillPath(Path{at(19, -2.2), at(19, 2.8), at(27, 2.8), at(27, -2.2)}, col)
			c.Stroke([][2]float64{at(20, 2.8), at(20, 6.5), at(27, 5.6), at(25.5, 2.8)}, 1.3, col, false)
			c.Stroke([][2]float64{at(27, -0.2), at(42, -0.2)}, 2.4, col, false)
			c.Stroke([][2]float64{at(27, -0.6), at(L, -0.6)}, 1.4, col, false)
		}
		c.Stroke([][2]float64{{X(cx - 6), Y(sy + 1)}, {X(cx + 8), Y(hipY - 6)}}, 1.2, col, false)
		sp := 0.0
		if moving {
			sp = math.Sin(ph)
		}
		hx, hy := cx+12+5*sp, sy+16
		c.Stroke([][2]float64{{X(cx + 3), Y(sy + 2)}, {X(cx + 9 + 2*sp), Y(sy + 10)}, {X(hx), Y(hy)}}, 5, col, false)
		tipX := cx + 16 + 9*sp
		if !moving {
			tipX = cx + 15
		}
		topX, topY := hx-0.5, hy-5
		c.Stroke([][2]float64{{X(topX), Y(topY)}, {X(tipX), Y(base)}}, 2.2, col, false)
		c.Arc(X(topX+3), Y(topY), 3, math.Pi, 2*math.Pi, 2.2, col)
		hy0 := sy - 7
		c.FillPath(Circle(X(cx+1.5), Y(hy0), 5.4), col)
		c.FillPath(Rect(X(cx-0.5), Y(hy0+3), 4, 4), col)
		c.FillPath(Ellipse(X(cx+1), Y(hy0-5), 8.4, 6.2), col)
		c.FillPath(Ellipse(X(cx+0.5), Y(hy0-9.5), 6.4, 4.2), col)
		tw := 0.0
		if moving {
			tw = 1.5
		}
		tail := Path{{X(cx - 4), Y(hy0 - 8)}, {X(cx - 7.5), Y(hy0 - 4)}, {X(cx - 12 - tw*2 - wind), Y(hy0 + 10)}, {X(cx - 7.5 - tw - wind*0.5), Y(hy0 + 11)}}
		c.FillPath(tail, col)
	}
	draw(2.5, 2.5, rgba(cPink, 255))
	draw(0, 0, rgba(cInk, 255))
}
