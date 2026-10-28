package help

import (
	"image"
	"image/png"
	"os"
	"path/filepath"
	"testing"

	"mmw/cmd/mmw-setup/scene"
)

// Every panel fits the window at a readable size (its last line drawn inside it), in each kind of game; with MMW_SHOTS
// set to a folder, each is saved there as a picture
func TestPanelsFit(t *testing.T) {
	left, right := Columns(true, false, "GAME FOLDER")
	base := scene.State{Exe: "MOJAVE_ENHANCED.EXE", Button: "PLAY", BtnLook: scene.BtnDone, DoneT: -1,
		Steps:  [5]string{"Find Fallout: New Vegas", "Get the modding tools", "Put in Mojave Enhanced", "Set up vanilla stable (no cars)", "Ready to play"},
		StepSt: [5]scene.StepState{scene.Done, scene.Done, scene.Done, scene.Done, scene.Done}, Progress: 1,
		Status: "All set. Press PLAY (new here? FIRST TIME START-UP).", Links: []string{"details", "report", "undo everything"},
		Pills: []string{"NAME: CHASE", "HOST A GAME", "JOIN A FRIEND"}, PillOn: []bool{false, false, false},
		SideL: left, SideR: right}
	shots := os.Getenv("MMW_SHOTS")
	save := func(name string, s scene.State) {
		img := image.NewRGBA(image.Rect(0, 0, scene.W*2, scene.H*2))
		hits := scene.Render(img, 2, &s)
		if s.Info && !scene.InfoFitted() {
			t.Errorf("%s: its lines don't all fit", name)
		}
		if s.Info {
			for _, h := range hits {
				if h.Y+h.H > scene.H {
					t.Errorf("%s: %q is drawn past the window's edge", name, h.Name)
				}
			}
		}
		if shots == "" {
			return
		}
		f, err := os.Create(filepath.Join(shots, name+".png"))
		if err != nil {
			t.Fatal(err)
		}
		png.Encode(f, img)
		f.Close()
	}
	save("main", base)
	l2, r2 := Columns(true, true, "MOD ORGANIZER 2")
	ttw := base
	ttw.SideL, ttw.SideR = l2, r2
	save("main-ttw-mo2", ttw)
	before := base
	before.SideL, before.SideR = Columns(false, false, "")
	before.Button, before.BtnLook, before.StepSt, before.Progress, before.Pills = "MAKE IT WORK", scene.BtnReady, [5]scene.StepState{}, 0, nil
	before.Status, before.Links = "Ready when you are. Make sure Fallout: New Vegas is closed.", []string{"details", "report"}
	save("main-before-setup", before)
	for name, v := range map[string]ModsView{
		"mods": {Pills: []string{"GAME FOLDER", "MOD ORGANIZER 2", "VORTEX"}, On: []bool{true, false, false}, Say: ModsFolderSay},
		"mods-mo2": {Pills: []string{"GAME FOLDER", "MOD ORGANIZER 2", "VORTEX"}, On: []bool{false, true, false},
			Say: "Mod Organizer 2 (Fallout New Vegas, profile Default): PLAY starts the game through it, with every mod that profile has on. Mojave Enhanced is switched on in its load order."},
	} {
		s := ttw
		s.Info, s.InfoTitle, s.InfoLines, s.InfoCols = true, "YOUR MODS", ModsLines(v), 1
		save(name, s)
	}
	for _, g := range []Game{{}, {Full: true, Weather: true}} {
		sfx := map[bool]string{false: "", true: "-full"}[g.Full]
		for _, p := range []struct {
			name, title string
			lines       []scene.Line
			cols        int
		}{
			{"firsttime", "FIRST TIME START-UP", FirstTimeLines(), 1},
			{"whatsin", "WHAT'S IN IT", WhatsInLines(g), 2},
			{"keybinds", "KEYBINDS", KeysLines(g), 2},
		} {
			s := base
			s.Info, s.InfoTitle, s.InfoLines, s.InfoCols = true, p.title, p.lines, p.cols
			save(p.name+sfx, s)
		}
	}
}
