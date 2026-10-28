package main

import (
	"archive/zip"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func TestApplyPendingAndAddons(t *testing.T) {
	game := t.TempDir()
	managed := filepath.Join(game, "MojaveMostWanted")
	pend := filepath.Join(managed, "pending")
	os.MkdirAll(filepath.Join(pend, "addons"), 0o755)
	zf, _ := os.Create(filepath.Join(pend, "1-x.zip"))
	zw := zip.NewWriter(zf)
	w, _ := zw.Create("Data/meshes/AnimGroupOverride/MojaveMostWanted.json")
	w.Write([]byte("[]"))
	w, _ = zw.Create("../escape.txt")
	w.Write([]byte("no"))
	zw.Close()
	zf.Close()
	la := t.TempDir()
	t.Setenv("LOCALAPPDATA", la)
	marker := filepath.Join(game, "on.txt")
	sh := []string{"sh", "-c", "echo hi > " + marker}
	if runtime.GOOS == "windows" {
		sh = []string{"cmd", "/c", "echo hi > " + marker}
	}
	def := `{"name":"Test Pack","on":["` + sh[0] + `","` + sh[1] + `","` + sh[2] + `"],"off":["x"],"installedWhen":"{game}/on.txt"}`
	os.WriteFile(filepath.Join(pend, "addons", "t.json"), []byte(def), 0o644)
	os.WriteFile(filepath.Join(pend, "addons-on.txt"), []byte("Test Pack\r\nMissing Pack\r\n"), 0o644)

	applyPending(game, managed)
	if _, err := os.Stat(filepath.Join(game, "Data", "meshes", "AnimGroupOverride", "MojaveMostWanted.json")); err != nil {
		t.Fatal("zip not unpacked")
	}
	if _, err := os.Stat(filepath.Join(filepath.Dir(game), "escape.txt")); err == nil {
		t.Fatal("a path outside the game folder was written")
	}
	if _, err := os.Stat(filepath.Join(la, "MojaveMostWanted", "addons", "t.json")); err != nil {
		t.Fatal("add-on not registered")
	}
	switchPendingAddons(game, managed)
	if _, err := os.Stat(marker); err != nil {
		t.Fatal("add-on not switched on")
	}
	left, _ := os.ReadFile(filepath.Join(pend, "addons-on.txt"))
	if string(left) != "Missing Pack\r\n" {
		t.Fatalf("list left: %q", left)
	}
}
