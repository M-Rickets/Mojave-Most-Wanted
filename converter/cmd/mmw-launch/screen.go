package main

// Fill the screen (the game ran windowed at 1920x1080 in a corner of a bigger monitor).

import (
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

func fillScreen(pluginDir string, paths map[string]string) {
	if strings.EqualFold(paths["fillScreen"], "off") || paths["fillScreen"] == "0" {
		return
	}
	w, h := screenSize()
	if w < 640 || h < 480 {
		log.Printf("screen: the monitor's size wasn't found (%dx%d) - left as it is", w, h)
		return
	}
	cw, _ := gameIniValue("Display", "iSize W")
	ch, _ := gameIniValue("Display", "iSize H")
	if cw != strconv.Itoa(w) || ch != strconv.Itoa(h) {
		okW := gameIniPut("Display", "iSize W", strconv.Itoa(w))
		okH := gameIniPut("Display", "iSize H", strconv.Itoa(h))
		log.Printf("screen: the game draws at the monitor's %dx%d (was %sx%s)%s", w, h, cw, ch, map[bool]string{true: "", false: " - couldn't be written"}[okW && okH])
	}
	full, _ := gameIniValue("Display", "bFull Screen")
	if full == "1" {
		return
	}
	p := filepath.Join(pluginDir, "settings.ini")
	b, _ := os.ReadFile(p)
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	if strings.Contains(s, "\nborderless=1") || strings.HasPrefix(s, "borderless=1") {
		return
	}
	var out []string
	found := false
	for _, l := range strings.Split(strings.TrimRight(s, "\n"), "\n") {
		if strings.HasPrefix(strings.TrimSpace(l), "borderless=") {
			l, found = "borderless=1", true
		}
		if l != "" || len(out) > 0 {
			out = append(out, l)
		}
	}
	if !found {
		out = append(out, "borderless=1")
	}
	os.MkdirAll(pluginDir, 0o755)
	if err := os.WriteFile(p, []byte(strings.Join(out, "\r\n")+"\r\n"), 0o644); err == nil {
		log.Print("screen: windowed - the plugin makes it a borderless window over the whole monitor (settings.ini borderless=1)")
	}
}
