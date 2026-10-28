package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// applyPending unpacks MojaveMostWanted\pending\*.zip (builds the developer installs on this PC while the game may be
// running
func applyPending(fnv, managed string) (allIn bool) {
	allIn = true
	dir := filepath.Join(managed, "pending")
	zips, _ := filepath.Glob(filepath.Join(dir, "*.zip"))
	sort.Strings(zips)
	for _, z := range zips {
		r, err := zip.OpenReader(z)
		if err != nil {
			log.Printf("pending: %s unreadable: %v", filepath.Base(z), err)
			allIn = false
			continue
		}
		n, bad := 0, 0
		for _, f := range r.File {
			name := filepath.FromSlash(f.Name)
			if f.FileInfo().IsDir() || !filepath.IsLocal(name) {
				continue
			}
			dst := filepath.Join(fnv, name)
			os.MkdirAll(filepath.Dir(dst), 0o755)
			rc, err := f.Open()
			if err != nil {
				bad++
				continue
			}
			tmp := dst + ".mmwtmp"
			w, err := os.Create(tmp)
			if err == nil {
				_, err = io.Copy(w, rc)
				if cerr := w.Close(); err == nil {
					err = cerr
				}
			}
			rc.Close()
			if err == nil {
				os.Remove(dst)
				err = os.Rename(tmp, dst)
			}
			if err != nil {
				os.Remove(tmp)
			}
			if err != nil {
				bad++
			} else {
				n++
			}
		}
		r.Close()
		if bad == 0 {
			os.Remove(z)
		} else {
			allIn = false
		}
		log.Printf("pending: %s - %d files in, %d failed", filepath.Base(z), n, bad)
	}
	defs, _ := filepath.Glob(filepath.Join(dir, "addons", "*.json"))
	if la := os.Getenv("LOCALAPPDATA"); la != "" && len(defs) > 0 {
		to := filepath.Join(la, "MojaveMostWanted", "addons")
		os.MkdirAll(to, 0o755)
		for _, d := range defs {
			if b, err := os.ReadFile(d); err == nil && os.WriteFile(filepath.Join(to, filepath.Base(d)), b, 0o644) == nil {
				os.Remove(d)
				log.Printf("pending: add-on %s registered", filepath.Base(d))
			}
		}
	}
	return allIn
}

// switchPendingAddons switches on the local add-ons named in MojaveMostWanted\pending\addons-on.txt (one name a line,
// as in their definitions) before the game starts
func switchPendingAddons(fnv, managed string) {
	listPath := filepath.Join(managed, "pending", "addons-on.txt")
	b, err := os.ReadFile(listPath)
	la := os.Getenv("LOCALAPPDATA")
	if err != nil || la == "" {
		return
	}
	type addon struct {
		Name   string   `json:"name"`
		On     []string `json:"on"`
		InWhen string   `json:"installedWhen"`
	}
	defs, _ := filepath.Glob(filepath.Join(la, "MojaveMostWanted", "addons", "*.json"))
	byName := map[string]addon{}
	for _, d := range defs {
		raw, err := os.ReadFile(d)
		if err != nil {
			continue
		}
		var a addon
		if json.Unmarshal(bytes.TrimPrefix(raw, []byte("\xef\xbb\xbf")), &a) == nil && a.Name != "" && len(a.On) > 0 {
			byName[strings.ToLower(a.Name)] = a
		}
	}
	expand := func(s string) string { return strings.ReplaceAll(s, "{game}", fnv) }
	var left []string
	for _, name := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		if name = strings.TrimSpace(name); name == "" {
			continue
		}
		a, ok := byName[strings.ToLower(name)]
		if !ok {
			log.Printf("add-on %s: no definition", name)
			left = append(left, name)
			continue
		}
		if a.InWhen != "" && exists(expand(a.InWhen)) {
			log.Printf("add-on %s: already on", name)
			continue
		}
		log.Printf("add-on %s: switching on (a big pack takes a few minutes)...", name)
		args := make([]string, len(a.On))
		for i, s := range a.On {
			args[i] = expand(s)
		}
		c := exec.Command(args[0], args[1:]...)
		var err error
		if out, perr := c.StdoutPipe(); perr != nil {
			err = perr
		} else {
			c.Stderr = c.Stdout
			if err = c.Start(); err == nil {
				sc := bufio.NewScanner(out)
				for sc.Scan() {
					if l := strings.TrimSpace(sc.Text()); l != "" {
						log.Printf("  [%s] %s", name, l)
					}
				}
				err = c.Wait()
			}
		}
		if err != nil || (a.InWhen != "" && !exists(expand(a.InWhen))) {
			log.Printf("add-on %s: didn't switch on (%v); it stays listed for the next Play", name, err)
			left = append(left, name)
			continue
		}
		log.Printf("add-on %s: on", name)
	}
	if len(left) == 0 {
		os.Remove(listPath)
	} else {
		os.WriteFile(listPath, []byte(strings.Join(left, "\r\n")+"\r\n"), 0o644)
	}
}
