package scene

import (
	"fmt"
	"image"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

// One frame at 150% scaling (the window redraws ~30 times a second)
func BenchmarkFrame(b *testing.B) {
	img := image.NewRGBA(image.Rect(0, 0, W*3/2, H*3/2))
	s := &State{Exe: "MMW_SETUP.EXE", Button: "WORKING", BtnLook: BtnWorking, Progress: 0.4, Status: "Getting xNVSE...", Links: []string{"details", "report"}, DoneT: -1, CarMoves: true}
	for i := 0; i < b.N; i++ {
		s.T = float64(i) * 0.033
		Render(img, 1.5, s)
	}
}

// The panel's pills take clicks before the panel itself (the first hit under the mouse wins)
func TestMenuPillsClickable(t *testing.T) {
	img := image.NewRGBA(image.Rect(0, 0, W, H))
	s := &State{Menu: true, MenuTitle: "X", MenuItems: []string{"A", "B"}, MenuGroups: []string{"G", "G"}, DoneT: -1}
	hits := Render(img, 1, s)
	at := func(name string) string {
		for _, h := range hits {
			if h.Name == name {
				x, y := h.X+h.W/2, h.Y+h.H/2
				for _, g := range hits {
					if x >= g.X && x < g.X+g.W && y >= g.Y && y < g.Y+g.H {
						return g.Name
					}
				}
			}
		}
		return "?"
	}
	for _, n := range []string{"a", "b", MenuClose} {
		if got := at(n); got != n {
			t.Fatalf("click on %q hits %q", n, got)
		}
	}
}

// More add-ons than the window holds: the panel scrolls, and nothing is drawn or clickable past its edge
func TestMenuScrolls(t *testing.T) {
	var items, groups []string
	for g := 0; g < 9; g++ {
		for i := 0; i < 5; i++ {
			items = append(items, fmt.Sprintf("ADD-ON %d-%d WITH A LONG NAME", g, i))
			groups = append(groups, fmt.Sprintf("Group %d", g))
		}
	}
	for _, scroll := range []float64{0, 200, 1e6} {
		img := image.NewRGBA(image.Rect(0, 0, W, H))
		s := &State{Menu: true, MenuTitle: "GAME & ADD-ONS", MenuItems: items, MenuGroups: groups, MenuScroll: scroll, DoneT: -1}
		hits := Render(img, 1, s)
		if MenuScrollMax() <= 0 {
			t.Fatal("45 add-ons fit without scrolling?")
		}
		for _, h := range hits {
			if h.Name != "" && h.Y+h.H > H-14 {
				t.Fatalf("scroll %v: %q clickable past the panel (y %v)", scroll, h.Name, h.Y)
			}
		}
		if dir := os.Getenv("MMW_SHOTS"); dir != "" {
			f, _ := os.Create(filepath.Join(dir, fmt.Sprintf("menu-scroll-%v.png", scroll)))
			png.Encode(f, img)
			f.Close()
		}
	}
}
