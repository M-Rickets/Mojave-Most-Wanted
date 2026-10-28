package main

// The game settings Mojave Enhanced keeps on every Play.

import (
	"log"
	"os"
	"path/filepath"
	"strings"
)

func gameSettings(data string) {
	if c := customIni(); c != "" && iniHas(c, "General", "SIntroSequence") {
		if iniRemove(c, "General", "SIntroSequence") == nil {
			log.Print("game settings: the intro skipping's SIntroSequence line taken out of FalloutCustom.ini")
		}
	}
	st := filepath.Join(data, "NVSE", "Plugins", "nvse_stewie_tweaks.ini")
	if !exists(st) {
		return
	}
	for _, k := range []string{"bSkipIntroVideo", "bInstantContinueButton", "bAutoContinueGame"} {
		if iniValue(st, "Tweaks", k) == "1" {
			iniPut(st, "Tweaks", k, "0")
			log.Printf("game settings: lStewieAl's %s back off (the intro skipping is gone)", k)
		}
	}
	if iniValue(st, "Tweaks", "bNoDialogueZoom") != "1" {
		iniPut(st, "Tweaks", "bNoDialogueZoom", "1")
	}
}

// iniHas: the key is in [sec] at all (an empty value counts)
func iniHas(path, sec, key string) bool {
	b, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	in := false
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n") {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			in = strings.EqualFold(t[1:len(t)-1], sec)
			continue
		}
		if k, _, ok := strings.Cut(t, "="); in && ok && strings.EqualFold(strings.TrimSpace(k), key) {
			return true
		}
	}
	return false
}

// iniRemove takes the key's lines out of [sec], keeping every other line
func iniRemove(path, sec, key string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	in := false
	var out []string
	for _, l := range strings.Split(strings.TrimSuffix(strings.ReplaceAll(string(b), "\r\n", "\n"), "\n"), "\n") {
		t := strings.TrimSpace(l)
		if strings.HasPrefix(t, "[") && strings.HasSuffix(t, "]") {
			in = strings.EqualFold(t[1:len(t)-1], sec)
		} else if k, _, ok := strings.Cut(t, "="); in && ok && strings.EqualFold(strings.TrimSpace(k), key) {
			continue
		}
		out = append(out, l)
	}
	return writeKeepingReadOnly(path, []byte(strings.Join(out, "\r\n")+"\r\n"))
}
