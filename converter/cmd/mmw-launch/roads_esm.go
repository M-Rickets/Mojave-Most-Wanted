package main

import (
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

// Road models in New Vegas can be given other textures by their records
func roadRecordTextures(data, managed string) (extra []roadRow) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("roads: reading FalloutNV.esm failed: %v", r)
			extra = nil
		}
	}()
	f, err := os.Open(filepath.Join(data, "FalloutNV.esm"))
	if err != nil {
		log.Printf("roads: %v", err)
		return nil
	}
	defer f.Close()
	type stat struct {
		typ, edid, model string
		mods             []uint32
	}
	txst := map[uint32][6]string{}
	var stats []stat
	hdr := make([]byte, 24)
	le := binary.LittleEndian
	if _, err := io.ReadFull(f, hdr); err != nil || string(hdr[:4]) != "TES4" {
		log.Print("roads: FalloutNV.esm isn't a plugin file")
		return nil
	}
	f.Seek(int64(le.Uint32(hdr[4:])), io.SeekCurrent)
	subs := func(b []byte, fn func(typ string, d []byte)) {
		var big uint32
		for len(b) >= 6 {
			t := string(b[:4])
			n := uint32(le.Uint16(b[4:]))
			b = b[6:]
			if big > 0 {
				n, big = big, 0
			}
			if t == "XXXX" && len(b) >= 4 {
				big = le.Uint32(b)
				b = b[4:]
				continue
			}
			if int(n) > len(b) {
				return
			}
			fn(t, b[:n])
			b = b[n:]
		}
	}
	zstr := func(d []byte) string {
		if i := bytes.IndexByte(d, 0); i >= 0 {
			d = d[:i]
		}
		return strings.ToLower(strings.ReplaceAll(string(d), "/", `\`))
	}
	for {
		if _, err := io.ReadFull(f, hdr); err != nil {
			break
		}
		if string(hdr[:4]) != "GRUP" {
			f.Seek(int64(le.Uint32(hdr[4:])), io.SeekCurrent)
			continue
		}
		size := int64(le.Uint32(hdr[4:])) - 24
		label := string(hdr[8:12])
		if label != "STAT" && label != "SCOL" && label != "TXST" {
			f.Seek(size, io.SeekCurrent)
			continue
		}
		grp := make([]byte, size)
		if _, err := io.ReadFull(f, grp); err != nil {
			break
		}
		for len(grp) >= 24 {
			typ := string(grp[:4])
			n := int(le.Uint32(grp[4:]))
			if typ == "GRUP" {
				grp = grp[n:]
				continue
			}
			flags, id := le.Uint32(grp[8:]), le.Uint32(grp[12:])
			if 24+n > len(grp) {
				break
			}
			body := grp[24 : 24+n]
			grp = grp[24+n:]
			if flags&0x40000 != 0 && len(body) > 4 {
				zr, err := zlib.NewReader(bytes.NewReader(body[4:]))
				if err != nil {
					continue
				}
				body, err = io.ReadAll(zr)
				if err != nil {
					continue
				}
			}
			switch typ {
			case "TXST":
				var t [6]string
				subs(body, func(st string, d []byte) {
					if len(st) == 4 && strings.HasPrefix(st, "TX0") && st[3] >= '0' && st[3] <= '5' {
						t[st[3]-'0'] = zstr(d)
					}
				})
				txst[id] = t
			case "STAT", "SCOL":
				s := stat{typ: typ}
				subs(body, func(st string, d []byte) {
					switch st {
					case "EDID":
						s.edid = zstr(d)
					case "MODL":
						s.model = zstr(d)
					case "MODS":
						if len(d) < 4 {
							return
						}
						cnt := int(le.Uint32(d))
						d = d[4:]
						for i := 0; i < cnt && len(d) >= 4; i++ {
							l := int(le.Uint32(d))
							if 4+l+8 > len(d) {
								return
							}
							s.mods = append(s.mods, le.Uint32(d[4+l:]))
							d = d[4+l+8:]
						}
					}
				})
				stats = append(stats, s)
			}
		}
	}
	isRoad := func(s stat) bool {
		m := s.model
		return strings.Contains(m, `landscape\roads\`) || (strings.Contains(m, `scol\`) && strings.Contains(m, "road")) ||
			strings.Contains(s.edid, "road") || strings.Contains(s.edid, "highway")
	}
	have := map[string]bool{}
	for _, r := range sheetRoads {
		have[strings.ToLower(r.Path)] = true
	}
	uses := map[string]int{}
	var lines []string
	nRoad, nMods := 0, 0
	for _, s := range stats {
		if !isRoad(s) {
			continue
		}
		nRoad++
		var tx []string
		for _, id := range s.mods {
			t, ok := txst[id]
			if !ok {
				tx = append(tx, fmt.Sprintf("TXST %08X (not in FalloutNV.esm)", id))
				continue
			}
			tx = append(tx, t[0]+" + "+t[1])
			for k, p := range t[:2] {
				if p == "" {
					continue
				}
				if !strings.HasPrefix(p, `textures\`) {
					p = `textures\` + p
				}
				uses[p]++
				if !have[p] {
					have[p] = true
					kind := "atlas"
					if k == 1 {
						kind = "normal"
					}
					extra = append(extra, roadRow{Path: p, Kind: kind})
				}
			}
		}
		if len(s.mods) > 0 {
			nMods++
		}
		lines = append(lines, fmt.Sprintf("%s %s | %s | %s", s.typ, s.edid, s.model, strings.Join(tx, " ; ")))
	}
	sort.Strings(lines)
	var top []string
	for p, n := range uses {
		top = append(top, fmt.Sprintf("%5d %s", n, p))
	}
	sort.Sort(sort.Reverse(sort.StringSlice(top)))
	log.Printf("roads: %d road records in FalloutNV.esm (%d TXST), %d swap textures; %d more textures to repave", nRoad, len(txst), nMods, len(extra))
	for i, e := range extra {
		if i == 12 {
			log.Printf("roads: ... and %d more", len(extra)-i)
			break
		}
		log.Printf("roads: also repaving %s (%s)", e.Path, e.Kind)
	}
	return extra
}
