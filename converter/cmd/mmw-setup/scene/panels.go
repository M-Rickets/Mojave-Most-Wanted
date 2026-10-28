package scene

import (
	"math"
	"strings"
	"sync"

	"golang.org/x/image/font/opentype"
)

// The columns beside the checklist and the text panel their pills open.

// Side is a heading and its pills in a column beside the checklist.
type Side struct {
	Head  string
	Pills []string
	On    []bool
	Row   bool
}

const sideW = 114.0

// HitName is the name a pill's click comes as.
func HitName(label string) string { return strings.ToLower(strings.ReplaceAll(label, "\n", " ")) }

func side(c *Canvas, col []Side, x float64, hover string) []Hit {
	var hits []Hit
	y := 268.0
	for _, sd := range col {
		if sd.Head != "" {
			c.Text(fMono, 9, x+2, y+9, strings.ToUpper(sd.Head), rgba(cGray, 255))
			y += 15
		}
		n := len(sd.Pills)
		for i, p := range sd.Pills {
			lines := strings.Split(p, "\n")
			h := 22.0 + 11*float64(len(lines)-1)
			px, pw := x, sideW
			if sd.Row && n > 1 {
				pw = (sideW - 8*float64(n-1)) / float64(n)
				px = x + float64(i)*(pw+8)
			}
			on := i < len(sd.On) && sd.On[i]
			name := HitName(p)
			c.FillPath(RoundRect(px+3, y+3, pw, h, 11), rgba(cInk, 255))
			c.FillPath(RoundRect(px, y, pw, h, 11), rgba(cInk, 255))
			var fill uint32 = cPaper
			if on {
				fill = cTeal
			} else if hover == name {
				fill = cPinkLt
			}
			c.FillPath(RoundRect(px+2, y+2, pw-4, h-4, 9), rgba(fill, 255))
			size := 9.5
			for _, l := range lines {
				for c.TextWidth(fMonoBold, size, l) > pw-12 && size > 7 {
					size -= 0.5
				}
			}
			ty := y + 15
			for _, l := range lines {
				c.TextCentered(fMonoBold, size, px+pw/2, ty, l, rgba(cInk, 255))
				ty += 11
			}
			hits = append(hits, Hit{name, px, y, pw + 3, h + 3})
			if !sd.Row || i == n-1 {
				y += h + 9
			}
		}
		y += 4
	}
	return hits
}

// Line kinds of the text panel.
const (
	LText = iota
	LHead
	LStep
	LKey
	LWarn
	LPill
	LGap
)

// Line is one line of the text panel.
type Line struct {
	Kind      int
	Key, Text string
	On        bool
}

// Wrap breaks s into lines no wider than w
func wrap(c *Canvas, f *opentype.Font, size, w float64, s string) []string {
	var out []string
	for _, para := range strings.Split(s, "\n") {
		cur := ""
		for _, wd := range strings.Fields(para) {
			if cur != "" && c.TextWidth(f, size, cur+" "+wd) > w {
				out, cur = append(out, cur), wd
			} else if cur == "" {
				cur = wd
			} else {
				cur += " " + wd
			}
		}
		out = append(out, cur)
	}
	return out
}

var (
	infoMu   sync.Mutex
	infoFits bool
)

// InfoFitted: the last text panel drawn had room for all its lines
func InfoFitted() bool { infoMu.Lock(); defer infoMu.Unlock(); return infoFits }

// The text panel: a title, a DONE pill, the lines (in one or two columns, at the largest size that fits)
func info(c *Canvas, s *State) []Hit {
	var hits []Hit
	x0, y0, w := 26.0, 44.0, float64(W-52)
	maxH := float64(H) - y0 - 14
	cols := s.InfoCols
	if cols < 1 {
		cols = 1
	}
	gapX := 22.0
	colW := (w - 36 - gapX*float64(cols-1)) / float64(cols)
	top := y0 + 50
	together := true
	pass := func(sz float64, draw bool) float64 {
		lh := sz * 1.42
		col, x, y, bottom := 0, x0+18, top, top
		keyW := 0.0
		for _, l := range s.InfoLines {
			if l.Kind == LKey {
				keyW = math.Max(keyW, math.Min(colW*0.25, c.TextWidth(fMonoBold, sz-1, l.Key)+14))
			}
		}
		keyAt := func(k string) float64 { return math.Max(keyW, c.TextWidth(fMonoBold, sz-1, k)+14) + 10 }
		lineH := func(l Line) (float64, []string) {
			switch l.Kind {
			case LText:
				t := wrap(c, fBody, sz, colW, l.Text)
				return float64(len(t))*lh + 4, t
			case LHead:
				return sz + 20, nil
			case LStep:
				t := wrap(c, fBody, sz, colW-28, l.Text)
				return math.Max(22, float64(len(t))*lh+6), t
			case LKey:
				t := wrap(c, fBody, sz, colW-keyAt(l.Key), l.Text)
				return math.Max(19, float64(len(t))*lh+3), t
			case LWarn:
				t := wrap(c, fBodyBold, sz, colW-44, l.Text)
				return float64(len(t))*lh + 20, t
			case LPill:
				return 32, nil
			}
			return 6, nil
		}
		limit := y0 + maxH - 14
		pillX := -1.0
		for i, l := range s.InfoLines {
			h, txt := lineH(l)
			if l.Kind == LHead && y == top {
				h -= 6
			}
			if l.Kind == LPill {
				pw := c.TextWidth(fMonoBold, sz-0.5, l.Text) + 24
				if pillX >= 0 && pillX+pw <= x+colW {
					h = 0
				} else {
					pillX = x
				}
			} else {
				pillX = -1
			}
			need := h
			if l.Kind == LHead && cols > 1 && together {
				for _, m := range s.InfoLines[i+1:] {
					if m.Kind == LHead {
						break
					}
					mh, _ := lineH(m)
					need += mh
				}
			}
			if (y+h > limit || l.Kind == LHead && y+need > limit && top+need <= limit) && col < cols-1 && y > top {
				col, x, y = col+1, x0+18+float64(col+1)*(colW+gapX), top
				if l.Kind == LPill {
					pillX = x
				}
			}
			if l.Kind == LHead && y == top {
				h = sz + 14
			}
			if draw {
				switch l.Kind {
				case LText:
					ty := y + sz
					for _, t := range txt {
						c.Text(fBody, sz, x, ty, t, rgba(cInk, 255))
						ty += lh
					}
				case LHead:
					hy := y + h - 8
					c.Text(fMonoBold, sz, x, hy, strings.ToUpper(l.Text), rgba(cInk, 255))
					c.FillPath(Rect(x, hy+3, math.Min(colW, c.TextWidth(fMonoBold, sz, strings.ToUpper(l.Text))), 2), rgba(cPink, 255))
				case LStep:
					c.FillPath(Circle(x+9, y+9, 9), rgba(cInk, 255))
					c.FillPath(Circle(x+9, y+9, 7.5), rgba(cTeal, 255))
					c.TextCentered(fMonoBold, 9.5, x+9, y+12.5, l.Key, rgba(cInk, 255))
					ty := y + sz + 1
					for _, t := range txt {
						c.Text(fBody, sz, x+28, ty, t, rgba(cInk, 255))
						ty += lh
					}
				case LKey:
					kw := c.TextWidth(fMonoBold, sz-1, l.Key) + 14
					c.FillPath(RoundRect(x+1.5, y+1.5, kw, 16, 4), rgba(cInk, 255))
					c.FillPath(RoundRect(x, y, kw, 16, 4), rgba(cInk, 255))
					c.FillPath(RoundRect(x+1.3, y+1.3, kw-2.6, 13.4, 3), rgba(cYellow, 255))
					c.Text(fMonoBold, sz-1, x+7, y+11.8, l.Key, rgba(cInk, 255))
					ty := y + sz + 1
					for _, t := range txt {
						c.Text(fBody, sz, x+keyAt(l.Key), ty, t, rgba(cInk, 255))
						ty += lh
					}
				case LWarn:
					c.FillPath(RoundRect(x+3, y+5, colW, h-8, 9), rgba(cInk, 255))
					c.FillPath(RoundRect(x, y+2, colW, h-8, 9), rgba(cInk, 255))
					c.FillPath(RoundRect(x+2, y+4, colW-4, h-12, 7), rgba(0xFFE7C2, 255))
					c.FillPath(Circle(x+17, y+2+(h-8)/2, 9), rgba(cInk, 255))
					c.FillPath(Circle(x+17, y+2+(h-8)/2, 7.5), rgba(cOrange, 255))
					c.TextCentered(fTitle, 10, x+17, y+2+(h-8)/2+3.6, "!", rgba(cInk, 255))
					ty := y + 12 + sz
					for _, t := range txt {
						c.Text(fBodyBold, sz, x+34, ty, t, rgba(cInk, 255))
						ty += lh
					}
				case LPill:
					pw := c.TextWidth(fMonoBold, sz-0.5, l.Text) + 24
					py := y
					if h == 0 {
						py = y - 32
					}
					name := HitName(l.Text)
					c.FillPath(RoundRect(pillX+3, py+3, pw, 22, 11), rgba(cInk, 255))
					c.FillPath(RoundRect(pillX, py, pw, 22, 11), rgba(cInk, 255))
					var fill uint32 = cPaper
					if l.On {
						fill = cTeal
					} else if s.Hover == name {
						fill = cPinkLt
					}
					c.FillPath(RoundRect(pillX+2, py+2, pw-4, 18, 9), rgba(fill, 255))
					c.TextCentered(fMonoBold, sz-0.5, pillX+pw/2, py+15, l.Text, rgba(cInk, 255))
					hits = append(hits, Hit{name, pillX, py, pw + 3, 25})
				}
			}
			if l.Kind == LPill {
				pillX += c.TextWidth(fMonoBold, sz-0.5, l.Text) + 24 + 10
			}
			y += h
			bottom = math.Max(bottom, y)
		}
		return bottom
	}
	size := 11.0
	fits := func() bool {
		together = true
		if pass(size, false)+12 <= y0+maxH {
			return true
		}
		together = false
		return pass(size, false)+12 <= y0+maxH
	}
	for !fits() && size > 8 {
		size -= 0.5
	}
	infoMu.Lock()
	infoFits = pass(size, false)+12 <= y0+maxH
	infoMu.Unlock()
	h := math.Min(maxH, math.Max(240, pass(size, false)+14-y0))
	c.FillPath(RoundRect(x0+5, y0+5, w, h, 14), rgba(cInk, 255))
	c.FillPath(RoundRect(x0, y0, w, h, 14), rgba(cInk, 255))
	c.FillPath(RoundRect(x0+2, y0+2, w-4, h-4, 12), rgba(cPaper, 255))
	c.Text(fTitle, 17, x0+18, y0+30, s.InfoTitle, rgba(cInk, 255))
	dw := c.TextWidth(fMonoBold, 10.5, "DONE") + 30
	dx, dy := x0+w-18-dw, y0+12.0
	c.FillPath(RoundRect(dx+3, dy+3, dw, 22, 11), rgba(cInk, 255))
	c.FillPath(RoundRect(dx, dy, dw, 22, 11), rgba(cInk, 255))
	var fill uint32 = cPaper
	if s.Hover == MenuClose {
		fill = cPinkLt
	}
	c.FillPath(RoundRect(dx+2, dy+2, dw-4, 18, 9), rgba(fill, 255))
	c.TextCentered(fMonoBold, 10.5, dx+dw/2, dy+15, "DONE", rgba(cInk, 255))
	hits = append(hits, Hit{MenuClose, dx, dy, dw + 3, 25})
	pass(size, true)
	hits = append(hits, Hit{"", x0, y0, w, h})
	return hits
}
