// Package plugins reads New Vegas's load order and its plugins' headers, for the world switch
package plugins

import (
	"encoding/binary"
	"io"
	"os"
	"strings"
)

// TTW is Tale of Two Wastelands' own plugin.
const TTW = "TaleOfTwoWastelands.esm"

// Masters: the masters a plugin names in its header (TES4 MAST records)
func Masters(path string) []string {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	return MastersFrom(f)
}

// MastersFrom reads a plugin's header (TES4) from r and returns its masters.
func MastersFrom(r io.Reader) []string {
	hdr := make([]byte, 24)
	if _, err := io.ReadFull(r, hdr); err != nil || string(hdr[:4]) != "TES4" {
		return nil
	}
	size := binary.LittleEndian.Uint32(hdr[4:])
	if size > 1<<20 {
		return nil
	}
	b := make([]byte, size)
	if _, err := io.ReadFull(r, b); err != nil {
		return nil
	}
	var out []string
	for len(b) >= 6 {
		t, n := string(b[:4]), int(binary.LittleEndian.Uint16(b[4:]))
		if 6+n > len(b) {
			break
		}
		if t == "MAST" {
			out = append(out, strings.TrimRight(string(b[6:6+n]), "\x00"))
		}
		b = b[6+n:]
	}
	return out
}

// Read: plugins.txt's plugins in order (comments and blank lines left out; a "*" in front, as some managers write it,
// dropped)
func Read(path string) []string {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var out []string
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		l = strings.TrimPrefix(strings.TrimSpace(l), "*")
		if l != "" && !strings.HasPrefix(l, "#") {
			out = append(out, l)
		}
	}
	return out
}

// Has: name is in list (in any case)
func Has(list []string, name string) bool {
	for _, x := range list {
		if strings.EqualFold(x, name) {
			return true
		}
	}
	return false
}

func TTWSet(list []string, find func(string) string, official []string) []string {
	in := map[string]bool{strings.ToLower(TTW): true}
	for _, m := range Masters(find(TTW)) {
		if !Has(official, m) {
			in[strings.ToLower(m)] = true
		}
	}
	masters := map[string][]string{}
	for _, p := range list {
		masters[strings.ToLower(p)] = Masters(find(p))
	}
	for changed := true; changed; {
		changed = false
		for _, p := range list {
			lp := strings.ToLower(p)
			if in[lp] {
				continue
			}
			for _, m := range masters[lp] {
				if in[strings.ToLower(m)] {
					in[lp], changed = true, true
					break
				}
			}
		}
	}
	var out []string
	for _, p := range list {
		if in[strings.ToLower(p)] {
			out = append(out, p)
		}
	}
	return out
}

// Without: list less the plugins in out
func Without(list, out []string) []string {
	var r []string
	for _, p := range list {
		if !Has(out, p) {
			r = append(r, p)
		}
	}
	return r
}

// Restore: list with the plugins of was that aren't in it put back where they stood in was, each only when have says
// its file is there
func Restore(list, was []string, have func(string) bool) []string {
	out := append([]string{}, list...)
	for i, p := range was {
		if Has(out, p) || !have(p) {
			continue
		}
		at := 0
		for j := i - 1; j >= 0; j-- {
			if k := index(out, was[j]); k >= 0 {
				at = k + 1
				break
			}
		}
		out = append(out[:at], append([]string{p}, out[at:]...)...)
	}
	return out
}

func index(list []string, name string) int {
	for i, x := range list {
		if strings.EqualFold(x, name) {
			return i
		}
	}
	return -1
}
