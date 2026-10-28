package main

// The parts of setup that don't need Windows.

import (
	"archive/zip"
	"bytes"
	"fmt"
	"io"
	"path"
	"regexp"
	"sort"
	"strings"

	"github.com/bodgit/sevenzip"
)

func newBytesReader(b []byte) *bytes.Reader { return bytes.NewReader(b) }

// readArchive: every file of a.7z or.zip, by its path with / separators
func readArchive(b []byte, name string) (map[string][]byte, error) {
	out := map[string][]byte{}
	if strings.HasSuffix(strings.ToLower(name), ".zip") || (len(b) > 4 && string(b[:4]) == "PK\x03\x04") {
		zr, err := zip.NewReader(newBytesReader(b), int64(len(b)))
		if err != nil {
			return nil, err
		}
		for _, f := range zr.File {
			if f.FileInfo().IsDir() {
				continue
			}
			rc, err := f.Open()
			if err != nil {
				return nil, err
			}
			d, err := io.ReadAll(rc)
			rc.Close()
			if err != nil {
				return nil, err
			}
			out[path.Clean(strings.ReplaceAll(f.Name, `\`, "/"))] = d
		}
		return out, nil
	}
	r, err := sevenzip.NewReader(newBytesReader(b), int64(len(b)))
	if err != nil {
		return nil, err
	}
	for _, f := range r.File {
		if f.FileInfo().IsDir() {
			continue
		}
		rc, err := f.Open()
		if err != nil {
			return nil, err
		}
		d, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return nil, err
		}
		out[path.Clean(strings.ReplaceAll(f.Name, `\`, "/"))] = d
	}
	return out, nil
}

func versionLess(a, b string) bool {
	pa, pb := nums(a), nums(b)
	for i := 0; i < 4; i++ {
		if pa[i] != pb[i] {
			return pa[i] < pb[i]
		}
	}
	return false
}

func nums(v string) [4]int {
	var o [4]int
	for i, p := range regexp.MustCompile(`\d+`).FindAllString(v, 4) {
		fmt.Sscan(p, &o[i])
	}
	return o
}

// setIni sets keys in a [section], keeping everything else as it was
func setIni(text, section string, kv map[string]string) string {
	nl := "\r\n"
	if !strings.Contains(text, "\r\n") {
		nl = "\n"
	}
	lines := strings.Split(strings.ReplaceAll(text, "\r\n", "\n"), "\n")
	done := map[string]bool{}
	in, secAt := false, -1
	for i, l := range lines {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			if in {
				break
			}
			in = strings.EqualFold(t[1:len(t)-1], section)
			if in {
				secAt = i
			}
			continue
		}
		if !in {
			continue
		}
		if k, _, ok := strings.Cut(t, "="); ok {
			for want, v := range kv {
				if strings.EqualFold(strings.TrimSpace(k), want) {
					lines[i] = want + "=" + v
					done[want] = true
				}
			}
		}
	}
	var keys []string
	for k := range kv {
		if !done[k] {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)
	if len(keys) > 0 {
		var add []string
		for _, k := range keys {
			add = append(add, k+"="+kv[k])
		}
		if secAt < 0 {
			lines = append(lines, "["+section+"]")
			lines = append(lines, add...)
		} else {
			rest := append([]string{}, lines[secAt+1:]...)
			lines = append(append(lines[:secAt+1], add...), rest...)
		}
	}
	return strings.Join(lines, nl)
}

func splitLines(s string) []string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	if s == "" {
		return nil
	}
	return strings.Split(strings.TrimRight(s, "\n"), "\n")
}

func nexusRel(files map[string][]byte, name string) string {
	strip := ""
	var names []string
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		low := strings.ToLower(n)
		if i := strings.Index(low, "data/"); i >= 0 && (i == 0 || low[i-1] == '/') {
			strip = n[:i+5]
			break
		}
	}
	rel := name
	if strip != "" {
		if !strings.HasPrefix(name, strip) {
			return ""
		}
		rel = name[len(strip):]
	}
	low := strings.ToLower(rel)
	if rel == "" || strings.HasSuffix(low, ".pdb") || strings.HasPrefix(low, "fomod/") {
		return ""
	}
	if !strings.Contains(rel, "/") {
		if strings.HasSuffix(low, ".dll") {
			return "NVSE/Plugins/" + rel
		}
		return ""
	}
	return rel
}
