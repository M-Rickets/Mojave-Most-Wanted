package scene

import (
	"image"
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
