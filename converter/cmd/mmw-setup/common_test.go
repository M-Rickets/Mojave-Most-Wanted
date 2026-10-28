package main

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The real downloads (dev: MMW_TEST_DOWNLOADS points at a folder with them): every one opens, and its files land under
// Data where the author's own hand install put them.
func TestNexusArchives(t *testing.T) {
	dir := os.Getenv("MMW_TEST_DOWNLOADS")
	if dir == "" {
		t.Skip("no MMW_TEST_DOWNLOADS")
	}
	ms, _ := filepath.Glob(filepath.Join(dir, "*"))
	for _, m := range ms {
		low := strings.ToLower(m)
		if !strings.HasSuffix(low, ".7z") && !strings.HasSuffix(low, ".zip") {
			continue
		}
		b, _ := os.ReadFile(m)
		files, err := readArchive(b, m)
		if err != nil {
			t.Errorf("%s: %v", filepath.Base(m), err)
			continue
		}
		var out []string
		for n := range files {
			if r := nexusRel(files, n); r != "" {
				out = append(out, r)
			}
		}
		sort.Strings(out)
		ini := regexp.MustCompile(`(?i)(^|[^a-z])ini([^a-z]|$)`).MatchString(filepath.Base(m))
		t.Logf("%s (settings file: %v): %d files of %d -> %s", filepath.Base(m), ini, len(out), len(files), strings.Join(out, ", "))
	}
}

func TestSetIni(t *testing.T) {
	in := "[General]\r\nsLanguage=ENGLISH\r\n[Archive]\r\nbInvalidateOlderFiles=0\r\nSArchiveList=Fallout - Textures.bsa\r\n[Display]\r\niSize W=1920\r\n"
	out := setIni(in, "Archive", map[string]string{"bInvalidateOlderFiles": "1", "SInvalidationFile": ""})
	want := "[General]\r\nsLanguage=ENGLISH\r\n[Archive]\r\nSInvalidationFile=\r\nbInvalidateOlderFiles=1\r\nSArchiveList=Fallout - Textures.bsa\r\n[Display]\r\niSize W=1920\r\n"
	if out != want {
		t.Errorf("got %q", out)
	}
	if o2 := setIni("[Display]\nx=1\n", "Archive", map[string]string{"bInvalidateOlderFiles": "1"}); !strings.Contains(o2, "[Archive]\nbInvalidateOlderFiles=1") {
		t.Errorf("no section: %q", o2)
	}
	if !versionLess("6.4.8.0", "6.4.9") || versionLess("6.4.9.0", "6.4.9") || !versionLess("1.0.0", "1.0.1") {
		t.Error("versionLess")
	}
}
