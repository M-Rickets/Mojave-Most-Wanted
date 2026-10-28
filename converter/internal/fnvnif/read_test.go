package fnvnif

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// MMW_NIF_DIR=<a folder of New Vegas models> go test./internal/fnvnif -run Dir -v: each model's meshes (dev check
// against an independent reader)
func TestDir(t *testing.T) {
	dir := os.Getenv("MMW_NIF_DIR")
	if dir == "" {
		t.Skip("MMW_NIF_DIR not set")
	}
	files, _ := filepath.Glob(filepath.Join(dir, "*.nif"))
	for _, p := range files {
		b, _ := os.ReadFile(p)
		f, err := Parse(b)
		if err != nil {
			t.Errorf("%s: %v", p, err)
			continue
		}
		ms, err := f.Meshes()
		if err != nil {
			t.Errorf("%s: %v", p, err)
		}
		for _, m := range ms {
			fmt.Printf("%s %s verts %d tris %d uv %d tan %d diffuse %s\n", filepath.Base(p), m.Name, len(m.Pos), len(m.Tris), len(m.UV), len(m.Tangent), m.Diffuse)
		}
	}
}

func TestOne(t *testing.T) {
	p := os.Getenv("MMW_NIF")
	if p == "" {
		t.Skip()
	}
	b, _ := os.ReadFile(p)
	f, _ := Parse(b)
	ms, _ := f.Meshes()
	for _, m := range ms {
		if m.Name == os.Getenv("MMW_MESH") {
			fmt.Println("v10", m.Pos[10], "uv", m.UV[10], "tri0", m.Tris[0], m.Path)
		}
	}
}
