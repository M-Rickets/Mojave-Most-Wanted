package main

// New Vegas's own vehicles (cars.json game "fnv"), made drivable on the player's PC from their own game files

import (
	"bytes"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"mmw/internal/bsa"
	"mmw/internal/car"
	"mmw/internal/dds"
	"mmw/internal/fnvnif"
	"mmw/internal/gamecar"
)

const gameCarsVersion = "gc15"

func gameCars(data string, set func(k string, v any)) {
	var rows []carRow
	for _, c := range sheetCars {
		if c.Game == "fnv" && c.Model != "" {
			rows = append(rows, c)
		}
	}
	if len(rows) == 0 {
		return
	}
	src := filepath.Join(data, "Fallout - Meshes.bsa")
	a, err := bsa.Open(src)
	if err != nil {
		log.Printf("game vehicles: no Fallout - Meshes.bsa (%v)", err)
		return
	}
	defer a.Close()
	var tyres []string
	for _, f := range a.Files {
		if strings.HasSuffix(f.Path, ".nif") && (strings.Contains(f.Path, "tire") || strings.Contains(f.Path, "tyre") || strings.Contains(f.Path, "wheel") || strings.Contains(f.Path, "trailer") || strings.Contains(f.Path, "cart")) {
			tyres = append(tyres, f.Path)
		}
	}
	log.Printf("game vehicles: wheel and tyre models in the game: %s", strings.Join(tyres, ", "))
	dumpGameModels(a, data, rows)
	for _, c := range rows {
		e, ok := gameCar(a, src, data, c)
		set("car."+c.ID, b2i(ok))
		if !ok {
			continue
		}
		set("car."+c.ID+".halfTrack", e.HalfTrack)
		set("car."+c.ID+".halfBase", e.HalfBase)
		set("car."+c.ID+".halfLength", e.HalfLength)
		set("car."+c.ID+".halfWidth", e.HalfWidth)
		set("car."+c.ID+".front", e.Front)
		set("car."+c.ID+".rear", e.Rear)
		set("car."+c.ID+".clearance", e.Clearance)
		set("car."+c.ID+".wheelRadius", (e.WheelRadiusF+e.WheelRadiusR)/2)
		set("car."+c.ID+".wheelRadiusF", e.WheelRadiusF)
		set("car."+c.ID+".wheelRadiusR", e.WheelRadiusR)
		set("car."+c.ID+".topSpeed", c.TopSpeedMph)
		set("car."+c.ID+".steerAxisX", e.SteerAxis[0])
		set("car."+c.ID+".steerAxisY", e.SteerAxis[1])
		set("car."+c.ID+".steerAxisZ", e.SteerAxis[2])
		set("car."+c.ID+".modelYaw", e.ModelYaw)
		set("car."+c.ID+".originX", e.Origin[0])
		set("car."+c.ID+".originY", e.Origin[1])
		set("car."+c.ID+".originZ", e.Origin[2])
		if e.Cab {
			set("car."+c.ID+".eyeX", e.Eye[0])
			set("car."+c.ID+".eyeY", e.Eye[1])
			set("car."+c.ID+".eyeZ", e.Eye[2]-c.EyeDown)
			set("car."+c.ID+".hoodY", e.Hood[0])
			set("car."+c.ID+".hoodZ", e.Hood[1])
		}
	}
}

func gameCar(a *bsa.Archive, src, data string, c carRow) (gamecar.Built, bool) {
	drive := filepath.Join(data, "meshes", "mmw", "cars", c.ID, "drive.nif")
	s := sig(src) + "|" + gameCarsVersion + "|" + c.Front + "|" + c.WheelsFrom + "|" + fmt.Sprint(c.TyreRadius) + "|" + c.SteeringFrom + "|" + fmt.Sprint(c.RideRaise) + "|" + c.Extra
	key := "gamecar:" + c.ID
	cacheMu.Lock()
	e, ok := cache[key]
	cacheMu.Unlock()
	if ok && e.Sig == s && exists(drive) && e.Game != nil {
		log.Printf("%s: up to date", c.Label)
		return *e.Game, true
	}
	keep, err := readGameModel(a, c.Model)
	if err != nil {
		log.Printf("%s: %v", c.Label, err)
		return gamecar.Built{}, false
	}
	bike := strings.Contains(c.Model, "motorcycle")
	l := gamecar.Find(keep, bike, c.Front)
	for _, n := range l.Notes {
		log.Printf("%s: %s", c.Label, n)
	}
	var donor *gamecar.Donor
	if c.WheelsFrom != "" {
		dm, err := readGameModel(a, c.WheelsFrom)
		if err == nil {
			dl := gamecar.Find(dm, false)
			for _, n := range dl.Notes {
				log.Printf("%s: the wheels' model: %s", c.Label, n)
			}
			donor, err = gamecar.DonorFrom(dm, dl)
		}
		if err != nil {
			log.Printf("%s: no wheels from %s (%v) - its own", c.Label, c.WheelsFrom, err)
			donor = nil
		} else {
			for _, n := range donor.Notes {
				log.Printf("%s: the wheels' model: %s", c.Label, n)
			}
			donor.Want, donor.Raise = c.TyreRadius, c.RideRaise
			log.Printf("%s: wheels from %s (radius %.1f; left %d shapes, right %d)", c.Label, c.WheelsFrom, donor.Radius, len(donor.Left), len(donor.Right))
		}
	}
	if !bike {
		st, from := steeringFor(a, c)
		if st == nil {
			tx, nt := "", ""
			if donor != nil && len(donor.Left) > 0 {
				tx, nt = donor.Left[0].Diffuse, donor.Left[0].NormalTx
			} else if len(keep) > 0 {
				tx, nt = keep[0].Diffuse, keep[0].NormalMap
			}
			st, from = gamecar.MadeSteeringWheel(tx, nt), "the launcher"
		}
		if st != nil {
			if donor == nil {
				donor = &gamecar.Donor{}
			}
			donor.Steer = st
			colTx := filepath.Join(data, "textures", "mmw", "gadgets", "column.dds")
			os.MkdirAll(filepath.Dir(colTx), 0o755)
			if os.WriteFile(colTx, dds.Solid(40, 41, 44, 255), 0o644) == nil {
				donor.ColumnTx = `textures\mmw\gadgets\column.dds`
			}
			log.Printf("%s: steering wheel from %s (%s, r %.1f)", c.Label, from, st.From, st.Radius)
		}
	}
	if c.Extra != "" {
		var ex car.Extra
		if err := json.Unmarshal([]byte(c.Extra), &ex); err != nil {
			log.Printf("%s: its extras don't read (%v)", c.Label, err)
		} else if ex.Gadgets != nil {
			nodes, err := car.DashNodes(ex.Gadgets, data)
			if err != nil {
				log.Printf("%s: dash gadgets: %v", c.Label, err)
			} else {
				if donor == nil {
					donor = &gamecar.Donor{}
				}
				donor.Extra = nodes
				log.Printf("%s: dash gadgets: %d nodes (Pip-Boy %v, %d game models, shelf %v, cable %v)", c.Label, len(nodes), ex.Gadgets.PipBoy != nil, len(ex.Gadgets.Props), ex.Gadgets.Shelf != nil, ex.Gadgets.Cable != nil)
			}
		}
	}
	w, built, err := gamecar.Build(keep, l, bike, donor)
	if err != nil {
		log.Printf("%s: can't be made drivable: %v", c.Label, err)
		return gamecar.Built{}, false
	}
	os.MkdirAll(filepath.Dir(drive), 0o755)
	if err := w.Save(drive); err != nil {
		log.Printf("%s: %v", c.Label, err)
		return gamecar.Built{}, false
	}
	log.Printf("%s: made drivable from %s - wheels %s; body %d triangles, wheels %d; half track %.0f, half wheelbase %.0f, clearance %.0f, wheels %.2f / %.2f m",
		c.Label, c.Model, strings.Join(built.Wheels, " "), built.BodyTris, built.WheelTris, built.HalfTrack, built.HalfBase, built.Clearance, built.WheelRadiusF, built.WheelRadiusR)
	for _, n := range built.WheelNotes {
		log.Printf("%s: %s", c.Label, n)
	}
	log.Printf("%s: front %s (model yaw %.0f); cab %v - eye %.0f %.0f %.0f, hood camera %.0f %.0f", c.Label, orDefault(c.Front, "found"), built.ModelYaw, built.Cab, built.Eye[0], built.Eye[1], built.Eye[2], built.Hood[0], built.Hood[1])
	cacheMu.Lock()
	cache[key] = cacheEntry{Sig: s, Game: &built}
	cacheMu.Unlock()
	return built, true
}

// readGameModel: a model's meshes out of the archive, its damage stages and blast nodes left out
func readGameModel(a *bsa.Archive, path string) ([]*fnvnif.Mesh, error) {
	var f *bsa.File
	for i := range a.Files {
		if a.Files[i].Path == strings.ToLower(path) {
			f = &a.Files[i]
		}
	}
	if f == nil {
		return nil, fmt.Errorf("%s isn't in Fallout - Meshes.bsa", path)
	}
	b, err := a.Read(*f)
	if err != nil {
		return nil, err
	}
	nf, err := fnvnif.Parse(b)
	if err != nil {
		return nil, err
	}
	ms, err := nf.Meshes()
	if err != nil {
		return nil, err
	}
	var keep []*fnvnif.Mesh
	for _, m := range ms {
		skip := m.Hidden
		for _, p := range m.Path {
			lp := strings.ToLower(p)
			if strings.Contains(lp, "damagestage") || strings.Contains(lp, "blast") {
				skip = true
			}
		}
		if !skip {
			keep = append(keep, m)
		}
	}
	return keep, nil
}

func orDefault(s, d string) string {
	if s == "" {
		return d
	}
	return s
}

func steeringFor(a *bsa.Archive, c carRow) (*gamecar.Steer, string) {
	var tries []string
	if c.SteeringFrom != "" {
		tries = append(tries, c.SteeringFrom)
	} else {
		t0 := time.Now()
		n := 0
		for _, f := range a.Files {
			if !strings.HasSuffix(f.Path, ".nif") {
				continue
			}
			if strings.Contains(f.Path, "steer") {
				tries = append(tries, f.Path)
				continue
			}
			if time.Since(t0) > 30*time.Second {
				log.Printf("%s: stopped looking for a steering wheel after 30 s (%d models looked through)", c.Label, n)
				break
			}
			b, err := a.Read(f)
			n++
			if err == nil && (bytes.Contains(b, []byte("steer")) || bytes.Contains(b, []byte("Steer")) || bytes.Contains(b, []byte("STEER"))) {
				tries = append(tries, f.Path)
			}
		}
		log.Printf("%s: models with \"steer\" in them (%d looked through in %.1f s): %s", c.Label, n, time.Since(t0).Seconds(), strings.Join(tries, ", "))
	}
	for _, p := range tries {
		ms, err := readGameModel(a, p)
		if err != nil {
			continue
		}
		st, err := gamecar.SteeringWheel(ms, strings.Contains(p, "steer"), c.SteeringFrom != "")
		if err == nil {
			return st, p
		}
		if c.SteeringFrom != "" {
			log.Printf("%s: no steering wheel in %s (%v)", c.Label, p, err)
		}
	}
	return nil, ""
}

// dumpGameModels: copies of the models the game vehicles are built from, in debug\models\, to look at them in the work
// session (the archive is 1 GB)
func dumpGameModels(a *bsa.Archive, data string, rows []carRow) {
	dir := filepath.Join(filepath.Dir(data), "MojaveMostWanted", "debug")
	if !exists(dir) {
		return
	}
	dir = filepath.Join(dir, "models")
	os.MkdirAll(dir, 0o755)
	var radios []string
	for _, f := range a.Files {
		if strings.HasSuffix(f.Path, ".nif") && strings.Contains(f.Path, "radio") && !strings.Contains(f.Path, "radiation") {
			radios = append(radios, f.Path)
			out := filepath.Join(dir, "radio", strings.ReplaceAll(f.Path, "\\", "__"))
			if !exists(out) {
				os.MkdirAll(filepath.Dir(out), 0o755)
				if b, err := a.Read(f); err == nil {
					os.WriteFile(out, b, 0o644)
				}
			}
		}
	}
	log.Printf("debug: radio models in the game (copied to debug\\models\\radio): %s", strings.Join(radios, ", "))
	for _, c := range rows {
		for _, p := range []string{c.Model, c.WheelsFrom, c.SteeringFrom, "meshes\\animobjects\\aopipboy.nif"} {
			if p == "" {
				continue
			}
			out := filepath.Join(dir, filepath.Base(strings.ReplaceAll(strings.ToLower(p), "\\", "/")))
			if exists(out) {
				continue
			}
			for _, f := range a.Files {
				if f.Path == strings.ToLower(p) {
					if b, err := a.Read(f); err == nil && os.WriteFile(out, b, 0o644) == nil {
						log.Printf("debug: %s copied to %s", p, out)
					}
				}
			}
		}
	}
}
