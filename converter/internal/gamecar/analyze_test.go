package gamecar

import (
	"os"
	"path/filepath"
	"testing"

	"mmw/internal/fnvnif"
)

// MMW_NIF_DIR=<models> go test./internal/gamecar -run Dir -v: runs the finder over every model (dev: no panics)
func TestDir(t *testing.T) {
	dir := os.Getenv("MMW_NIF_DIR")
	if dir == "" {
		t.Skip()
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.nif"))
	for _, p := range files {
		b, _ := os.ReadFile(p)
		f, err := fnvnif.Parse(b)
		if err != nil {
			continue
		}
		ms, _ := f.Meshes()
		l := Find(ms, false)
		t.Logf("%s: %d pieces, %d wheels", filepath.Base(p), len(l.Pieces), len(l.Wheels))
	}
}
