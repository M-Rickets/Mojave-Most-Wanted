//go:build windows

package main

import (
	"archive/zip"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Switching on an add-on that --needs a file another add-on puts in: that one comes first
func TestAddonProviders(t *testing.T) {
	game := t.TempDir()
	os.MkdirAll(filepath.Join(game, "Data", "NVSE", "Plugins"), 0o755)
	jg := addon{Name: "Script extender add", On: []string{"x", "on"}, Off: []string{"x", "off"}, InWhen: `{game}\Data\NVSE\Plugins\jg.dll`}
	big := addon{Name: "World", On: []string{"pack", "on", "--needs", `NVSE\Plugins\jg.dll`, "--src", "s"}, Off: []string{"x"}, InWhen: `{game}\Data\W.esm`}
	other := addon{Name: "Other", On: []string{"x"}, Off: []string{"x"}, InWhen: `{game}\Data\O.esm`}
	got := addonProviders(big, game, []addon{other, jg, big})
	if len(got) != 1 || got[0].Name != jg.Name {
		t.Fatalf("got %v", got)
	}
	os.WriteFile(filepath.Join(game, "Data", "NVSE", "Plugins", "jg.dll"), []byte("x"), 0o644)
	if got := addonProviders(big, game, []addon{jg, big}); len(got) != 0 {
		t.Fatalf("got %v with it in", got)
	}
	if n := addonNeeds(addon{On: []string{"--needs=a.dll", "--needs", "b.dll"}}); len(n) != 2 {
		t.Fatalf("needs %v", n)
	}
}

// A provider is found by what its source holds (installedWhen is its pack record), and its own needs come first
func TestAddonProvidersFromSource(t *testing.T) {
	game, src := t.TempDir(), t.TempDir()
	os.MkdirAll(filepath.Join(game, "Data", "NVSE", "Plugins"), 0o755)
	os.MkdirAll(filepath.Join(src, "B", "nvse", "plugins"), 0o755)
	os.WriteFile(filepath.Join(src, "B", "nvse", "plugins", "b.dll"), []byte("x"), 0o644)
	zp := filepath.Join(src, "A-1.0.zip")
	zf, _ := os.Create(zp)
	zw := zip.NewWriter(zf)
	w, _ := zw.Create("A-1.0/NVSE/Plugins/A.dll")
	w.Write([]byte("x"))
	zw.Close()
	zf.Close()
	os.MkdirAll(filepath.Join(src, "World"), 0o755)
	os.WriteFile(filepath.Join(src, "World", "World.esm"), []byte("x"), 0o644)
	rec := func(id string) string { return `{game}\MojaveMostWanted\packs\` + id + `.json` }
	a := addon{Name: "A", On: []string{"pack", "on", "--src", zp}, Off: []string{"x"}, InWhen: rec("a")}
	b := addon{Name: "B", On: []string{"pack", "on", "--src", filepath.Join(src, "B")}, Off: []string{"x"}, InWhen: rec("b")}
	world := addon{Name: "World", On: []string{"pack", "on", "--src", filepath.Join(src, "World"), "--needs", `NVSE\Plugins\a.dll`, "--needs", `NVSE\Plugins\B.dll`}, Off: []string{"x"}, InWhen: rec("w")}
	guns := addon{Name: "Guns", On: []string{"pack", "on", "--src", src, "--needs", "World.esm"}, Off: []string{"x"}, InWhen: rec("g")}
	all := []addon{a, b, guns, world}
	got := addonProviders(guns, game, all)
	var names []string
	for _, p := range got {
		names = append(names, p.Name)
	}
	if strings.Join(names, ",") != "A,B,World" {
		t.Fatalf("got %v", names)
	}
	os.WriteFile(filepath.Join(game, "Data", "NVSE", "Plugins", "a.dll"), []byte("x"), 0o644)
	names = nil
	for _, p := range addonProviders(guns, game, all) {
		names = append(names, p.Name)
	}
	if strings.Join(names, ",") != "B,World" {
		t.Fatalf("with A in: got %v", names)
	}
}

// Switching a requirement off: what's in and needs it comes out first - the weapon pack before the world that needs the
// requirement
func TestAddonDependents(t *testing.T) {
	game, src := t.TempDir(), t.TempDir()
	os.MkdirAll(filepath.Join(game, "MojaveMostWanted", "packs"), 0o755)
	os.MkdirAll(filepath.Join(src, "A", "nvse", "plugins"), 0o755)
	os.WriteFile(filepath.Join(src, "A", "nvse", "plugins", "a.dll"), []byte("x"), 0o644)
	os.MkdirAll(filepath.Join(src, "World"), 0o755)
	os.WriteFile(filepath.Join(src, "World", "World.esm"), []byte("x"), 0o644)
	os.MkdirAll(filepath.Join(src, "Guns"), 0o755)
	os.WriteFile(filepath.Join(src, "Guns", "Guns.esp"), []byte("x"), 0o644)
	rec := func(id string) string {
		p := filepath.Join(game, "MojaveMostWanted", "packs", id+".json")
		os.WriteFile(p, []byte("{}"), 0o644)
		return p
	}
	a := addon{Name: "A", On: []string{"pack", "on", "--src", filepath.Join(src, "A")}, Off: []string{"x"}, InWhen: rec("a")}
	world := addon{Name: "World", On: []string{"pack", "on", "--src", filepath.Join(src, "World"), "--needs", `NVSE\Plugins\a.dll`}, Off: []string{"x"}, InWhen: rec("w")}
	guns := addon{Name: "Guns", On: []string{"pack", "on", "--src", filepath.Join(src, "Guns"), "--needs", "World.esm", "--needs", `NVSE\Plugins\A.dll`}, Off: []string{"x"}, InWhen: rec("g")}
	other := addon{Name: "Other", On: []string{"pack", "on", "--src", filepath.Join(src, "Guns")}, Off: []string{"x"}, InWhen: rec("o")}
	all := []addon{a, guns, other, world}
	var names []string
	for _, d := range addonDependents(a, game, all) {
		names = append(names, d.Name)
	}
	if strings.Join(names, ",") != "Guns,World" {
		t.Fatalf("got %v", names)
	}
	if d := addonDependents(guns, game, all); len(d) != 0 {
		t.Fatalf("nothing needs the weapon pack: %v", d)
	}
}

// A hidden add-on comes in while everything it comes with is in, goes out when one isn't, and isn't retried this run
// once it stayed out (a mod contests it)
func TestAutoPlan(t *testing.T) {
	game := t.TempDir()
	packs := filepath.Join(game, "MojaveMostWanted", "packs")
	os.MkdirAll(packs, 0o755)
	rec := func(id string) string { return filepath.Join(packs, id+".json") }
	put := func(id string, in bool) {
		if in {
			os.WriteFile(rec(id), []byte("{}"), 0o644)
		} else {
			os.Remove(rec(id))
		}
	}
	world := addon{Name: "World", InWhen: rec("w")}
	guns := addon{Name: "Guns", InWhen: rec("g")}
	patch := addon{Name: "Patch", Hidden: true, ComesWith: []string{"World", "guns "}, InWhen: rec("p")}
	all := []addon{world, guns, patch}
	names := func(as []addon) string {
		var n []string
		for _, a := range as {
			n = append(n, a.Name)
		}
		return strings.Join(n, ",")
	}
	put("w", true)
	if in, out := autoPlan(all, game, nil); len(in)+len(out) != 0 {
		t.Fatalf("only the world in: %s / %s", names(in), names(out))
	}
	put("g", true)
	if in, _ := autoPlan(all, game, nil); names(in) != "Patch" {
		t.Fatalf("both in: %s", names(in))
	}
	if in, _ := autoPlan(all, game, map[string]bool{"patch": true}); len(in) != 0 {
		t.Fatal("retried after staying out")
	}
	put("p", true)
	put("g", false)
	if _, out := autoPlan(all, game, nil); names(out) != "Patch" {
		t.Fatalf("guns out: %s", names(out))
	}
}
