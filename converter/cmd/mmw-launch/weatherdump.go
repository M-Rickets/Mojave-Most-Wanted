package main

// dumpWeathers: the climates and weathers the game has, for designing the seasons in the work session - the author's PC
// only (when MojaveMostWanted\debug\ is there, only dev builds make), once per set of master files.

import (
	"bufio"
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type wxRec struct {
	file string
	id   uint32
	sig  string
	subs map[string][]byte
	list []string
}

// wxRead: the records of the given top-level groups (and WRLD records themselves, not their children)
func wxRead(path string, want map[string]bool) (masters []string, recs []wxRec, err error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, nil, err
	}
	defer f.Close()
	r := bufio.NewReaderSize(f, 1<<20)
	le := binary.LittleEndian
	hdr := make([]byte, 24)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return nil, nil, err
	}
	th := make([]byte, le.Uint32(hdr[4:]))
	if _, err := io.ReadFull(r, th); err != nil {
		return nil, nil, err
	}
	for o := 0; o+6 <= len(th); {
		s, n := string(th[o:o+4]), int(le.Uint16(th[o+4:]))
		if s == "MAST" && o+6+n <= len(th) {
			masters = append(masters, strings.TrimRight(string(th[o+6:o+6+n]), "\x00"))
		}
		o += 6 + n
	}
	name := filepath.Base(path)
	var top string
	for {
		if _, err := io.ReadFull(r, hdr); err != nil {
			return masters, recs, nil
		}
		sig, size := string(hdr[:4]), le.Uint32(hdr[4:])
		if sig == "GRUP" {
			gtype := le.Uint32(hdr[12:])
			if gtype == 0 {
				top = string(hdr[8:12])
				if !want[top] {
					if _, err := r.Discard(int(size) - 24); err != nil {
						return masters, recs, nil
					}
				}
				continue
			}
			if _, err := r.Discard(int(size) - 24); err != nil {
				return masters, recs, nil
			}
			continue
		}
		body := make([]byte, size)
		if _, err := io.ReadFull(r, body); err != nil {
			return masters, recs, nil
		}
		if !want[sig] {
			continue
		}
		if le.Uint32(hdr[8:])&0x40000 != 0 && len(body) > 4 {
			zr, err := zlib.NewReader(bytes.NewReader(body[4:]))
			if err != nil {
				continue
			}
			body, _ = io.ReadAll(zr)
			zr.Close()
		}
		rec := wxRec{file: name, id: le.Uint32(hdr[12:]), sig: sig, subs: map[string][]byte{}}
		for o := 0; o+6 <= len(body); {
			s, n := string(body[o:o+4]), int(le.Uint16(body[o+4:]))
			if o+6+n > len(body) {
				break
			}
			if _, have := rec.subs[s]; !have {
				rec.subs[s] = body[o+6 : o+6+n]
			} else if s == "SNAM" {
				rec.subs[s] = append(append([]byte{}, rec.subs[s]...), body[o+6:o+6+n]...)
			}
			rec.list = append(rec.list, s)
			o += 6 + n
		}
		recs = append(recs, rec)
	}
}

func dumpWeathers(data string, files []string) {
	dir := filepath.Join(filepath.Dir(data), "MojaveMostWanted", "debug")
	if !exists(dir) {
		return
	}
	out := filepath.Join(dir, "weathers.txt")
	key := ""
	for _, f := range files {
		if fi, err := os.Stat(filepath.Join(data, f)); err == nil {
			key += fmt.Sprintf("%s %d %d;", f, fi.Size(), fi.ModTime().Unix())
		}
	}
	if b, err := os.ReadFile(out); err == nil && strings.HasPrefix(string(b), "# "+key+"\n") {
		return
	}
	want := map[string]bool{"WTHR": true, "CLMT": true, "WRLD": true}
	type gid struct {
		file string
		id   uint32
	}
	edid := map[gid]string{}
	var all []wxRec
	masterOf := map[string][]string{}
	for _, f := range files {
		m, recs, err := wxRead(filepath.Join(data, f), want)
		if err != nil {
			continue
		}
		masterOf[f] = m
		all = append(all, recs...)
	}
	home := func(r wxRec, id uint32) gid {
		m := masterOf[r.file]
		i := int(id >> 24)
		if i < len(m) {
			return gid{m[i], id & 0xFFFFFF}
		}
		return gid{r.file, id & 0xFFFFFF}
	}
	for _, r := range all {
		if e, ok := r.subs["EDID"]; ok {
			edid[home(r, r.id)] = strings.TrimRight(string(e), "\x00")
		}
	}
	name := func(r wxRec, id uint32) string {
		g := home(r, id)
		if n, ok := edid[g]; ok {
			return fmt.Sprintf("%s (%s:%06X)", n, g.file, g.id)
		}
		return fmt.Sprintf("%s:%06X", g.file, g.id)
	}
	le := binary.LittleEndian
	var b strings.Builder
	fmt.Fprintf(&b, "# %s\n", key)
	fmt.Fprintf(&b, "# climates and weathers (later files override earlier ones; each line names the file it comes from)\n\n")
	for _, sec := range []string{"WRLD", "CLMT", "WTHR"} {
		fmt.Fprintf(&b, "== %s\n", sec)
		var lines []string
		for _, r := range all {
			if r.sig != sec {
				continue
			}
			l := fmt.Sprintf("%s in %s", name(r, r.id), r.file)
			switch sec {
			case "WRLD":
				if c, ok := r.subs["CNAM"]; ok && len(c) >= 4 {
					l += " climate " + name(r, le.Uint32(c))
				}
			case "CLMT":
				if w, ok := r.subs["WLST"]; ok {
					step := 12
					if len(w)%12 != 0 && len(w)%8 == 0 {
						step = 8
					}
					for o := 0; o+step <= len(w); o += step {
						l += fmt.Sprintf("\n    %3d%% %s", int32(le.Uint32(w[o+4:])), name(r, le.Uint32(w[o:])))
						if step == 12 && le.Uint32(w[o+8:]) != 0 {
							l += " if " + name(r, le.Uint32(w[o+8:]))
						}
					}
				}
				if t, ok := r.subs["TNAM"]; ok && len(t) >= 6 {
					hm := func(v byte) string { m := int(v) * 10; return fmt.Sprintf("%02d:%02d", m/60, m%60) }
					l += fmt.Sprintf("\n    sunrise %s-%s sunset %s-%s volatility %d", hm(t[0]), hm(t[1]), hm(t[2]), hm(t[3]), t[4])
				}
			case "WTHR":
				if d, ok := r.subs["DATA"]; ok && len(d) >= 12 {
					var cls []string
					for bit, n := range []string{"pleasant", "cloudy", "rainy", "snow"} {
						if d[11]&(1<<bit) != 0 {
							cls = append(cls, n)
						}
					}
					if len(cls) == 0 {
						cls = append(cls, "none")
					}
					l += fmt.Sprintf(" [%s] wind %d precip %d-%d thunder %d-%d freq %d", strings.Join(cls, ","), d[0], d[6], d[7], d[8], d[9], d[10])
					if len(d) >= 15 {
						l += fmt.Sprintf(" lightning %d,%d,%d", d[12], d[13], d[14])
					}
				}
				if s, ok := r.subs["SNAM"]; ok {
					for o := 0; o+8 <= len(s); o += 8 {
						typ := []string{"default", "precip", "wind", "thunder"}
						t := int(le.Uint32(s[o+4:]))
						tn := fmt.Sprint(t)
						if t < len(typ) {
							tn = typ[t]
						}
						l += fmt.Sprintf("\n    sound %s: %s", tn, name(r, le.Uint32(s[o:])))
					}
				}
				for _, ref := range []string{"MNAM", "NNAM", "INAM", "ONAM"} {
					if v, ok := r.subs[ref]; ok && len(v) == 4 {
						l += fmt.Sprintf("\n    %s %s", ref, name(r, le.Uint32(v)))
					}
				}
				seen := map[string]bool{}
				var sl []string
				for _, s := range r.list {
					if !seen[s] {
						seen[s] = true
						sl = append(sl, s)
					}
				}
				l += "\n    subrecords " + strings.Join(sl, " ")
			}
			lines = append(lines, l)
		}
		sort.Strings(lines)
		for _, l := range lines {
			b.WriteString(l + "\n")
		}
		b.WriteString("\n")
	}
	if err := os.WriteFile(out, []byte(b.String()), 0o644); err != nil {
		log.Printf("weathers: %v", err)
		return
	}
	log.Printf("weathers: climates and weathers listed in debug\\weathers.txt (%d records)", len(all))
}
