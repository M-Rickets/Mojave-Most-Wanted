// Package bsa reads Fallout 3 / New Vegas archives: the file list and file contents, so the launcher can work from the
// player's own game files.
package bsa

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"strings"
)

type File struct {
	Path   string
	size   uint32
	offset uint32
	comp   bool
}

type Archive struct {
	f     *os.File
	flags uint32
	Files []File
}

const (
	flagDirNames   = 0x1
	flagFileNames  = 0x2
	flagCompressed = 0x4
	flagEmbedNames = 0x100
)

func Open(path string) (*Archive, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	a := &Archive{f: f}
	if err := a.read(); err != nil {
		f.Close()
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	return a, nil
}

func (a *Archive) Close() error { return a.f.Close() }

func (a *Archive) read() error {
	le := binary.LittleEndian
	h := make([]byte, 36)
	if _, err := io.ReadFull(a.f, h); err != nil {
		return err
	}
	if string(h[0:4]) != "BSA\x00" || le.Uint32(h[4:]) != 104 {
		return fmt.Errorf("not a version 104 BSA")
	}
	folderOff := le.Uint32(h[8:])
	a.flags = le.Uint32(h[12:])
	folders := int(le.Uint32(h[16:]))
	files := int(le.Uint32(h[20:]))
	fileNameLen := int(le.Uint32(h[28:]))
	if folders > 100000 || files > 1000000 {
		return fmt.Errorf("bad header")
	}
	fr := make([]byte, folders*16)
	if _, err := a.f.ReadAt(fr, int64(folderOff)); err != nil {
		return err
	}
	type folder struct{ count, off uint32 }
	var fs []folder
	for i := 0; i < folders; i++ {
		fs = append(fs, folder{le.Uint32(fr[i*16+8:]), le.Uint32(fr[i*16+12:])})
	}
	pos := int64(folderOff) + int64(folders*16)
	type rec struct {
		dir          string
		size, offset uint32
	}
	var recs []rec
	for _, fo := range fs {
		dir := ""
		if a.flags&flagDirNames != 0 {
			var n [1]byte
			if _, err := a.f.ReadAt(n[:], pos); err != nil {
				return err
			}
			b := make([]byte, int(n[0]))
			if _, err := a.f.ReadAt(b, pos+1); err != nil {
				return err
			}
			dir = strings.TrimRight(string(b), "\x00")
			pos += 1 + int64(n[0])
		}
		b := make([]byte, int(fo.count)*16)
		if _, err := a.f.ReadAt(b, pos); err != nil {
			return err
		}
		for i := 0; i < int(fo.count); i++ {
			recs = append(recs, rec{dir, le.Uint32(b[i*16+8:]), le.Uint32(b[i*16+12:])})
		}
		pos += int64(fo.count) * 16
	}
	var names []string
	if a.flags&flagFileNames != 0 {
		b := make([]byte, fileNameLen)
		if _, err := a.f.ReadAt(b, pos); err != nil {
			return err
		}
		names = strings.Split(strings.TrimRight(string(b), "\x00"), "\x00")
	}
	for i, r := range recs {
		name := fmt.Sprintf("#%d", i)
		if i < len(names) {
			name = names[i]
		}
		p := strings.ToLower(r.dir + `\` + name)
		comp := a.flags&flagCompressed != 0
		if r.size&(1<<30) != 0 {
			comp = !comp
		}
		a.Files = append(a.Files, File{Path: p, size: r.size &^ (3 << 30), offset: r.offset, comp: comp})
	}
	return nil
}

// Read returns a file's contents (decompressed).
func (a *Archive) Read(f File) ([]byte, error) {
	b := make([]byte, f.size)
	if _, err := a.f.ReadAt(b, int64(f.offset)); err != nil {
		return nil, err
	}
	if a.flags&flagEmbedNames != 0 && len(b) > 0 {
		n := int(b[0])
		if 1+n <= len(b) {
			b = b[1+n:]
		}
	}
	if !f.comp {
		return b, nil
	}
	if len(b) < 4 {
		return nil, fmt.Errorf("%s: short", f.Path)
	}
	orig := binary.LittleEndian.Uint32(b)
	z, err := zlib.NewReader(bytes.NewReader(b[4:]))
	if err != nil {
		return nil, err
	}
	out := make([]byte, 0, orig)
	buf := bytes.NewBuffer(out)
	if _, err := io.Copy(buf, z); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}
