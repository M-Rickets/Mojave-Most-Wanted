package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestProfileIniAndPlugins(t *testing.T) {
	d := t.TempDir()
	ini := filepath.Join(d, "Fallout.ini")
	os.WriteFile(ini, []byte("[General]\r\nSLocalSavePath=Saves\\\r\nsLanguage=ENGLISH\r\n[Display]\r\niSize W=1920\r\n"), 0o644)
	if !iniPut(ini, "General", "SLocalSavePath", `Saves\TTW\`) || iniValue(ini, "General", "SLocalSavePath") != `Saves\TTW\` {
		t.Fatal("set existing key")
	}
	iniPut(ini, "Display", "bFull Screen", "1")
	iniPut(ini, "Tweaks", "bX", "0")
	b, _ := os.ReadFile(ini)
	s := string(b)
	if !strings.Contains(s, "iSize W=1920\r\nbFull Screen=1\r\n[Tweaks]\r\nbX=0") || !strings.Contains(s, "sLanguage=ENGLISH") {
		t.Fatalf("ini: %q", s)
	}
	if (profile{vanilla: true, ttw: true}).saves() != `Saves\TTW\` || (profile{vanilla: true}).saves() != `Saves\` {
		t.Fatal("saves")
	}
}

func TestIniPutReadOnly(t *testing.T) {
	ini := filepath.Join(t.TempDir(), "Fallout.ini")
	os.WriteFile(ini, []byte("[General]\r\nSLocalSavePath=Saves\\\r\n"), 0o444)
	if !iniPut(ini, "General", "SLocalSavePath", `Saves\Vanilla\`) || iniValue(ini, "General", "SLocalSavePath") != `Saves\Vanilla\` {
		t.Fatal("read-only ini not written")
	}
	if fi, _ := os.Stat(ini); fi.Mode().Perm()&0o200 != 0 {
		t.Fatal("read-only not kept")
	}
}

// Fallout.ini read-only, FalloutPrefs.ini a full copy with SLocalSavePath=Saves\: the save folder goes into
// FalloutCustom.ini (the game's last word), FalloutPrefs.ini agrees, Fallout.ini is left as it is
func TestProfileSavesCustomIni(t *testing.T) {
	docs, managed, data := t.TempDir(), t.TempDir(), t.TempDir()
	t.Setenv("MMW_DOCUMENTS", docs)
	my := filepath.Join(docs, "My Games", "FalloutNV")
	os.MkdirAll(my, 0o755)
	fo := "[General]\r\nSLocalSavePath=Saves\\\r\n"
	os.WriteFile(filepath.Join(my, "Fallout.ini"), []byte(fo), 0o444)
	os.WriteFile(filepath.Join(my, "FalloutPrefs.ini"), []byte("[General]\r\nSLocalSavePath=Saves\\\r\nSCharGenQuest=00102037\r\n"), 0o644)
	if err := applyProfile(profile{vanilla: true, ttw: true}, data, managed); err != nil {
		t.Fatal(err)
	}
	if v, _ := gameIniValue("General", "SLocalSavePath"); v != `Saves\TTW\` {
		t.Fatalf("the game's save folder: %q", v)
	}
	if iniValue(filepath.Join(my, "FalloutCustom.ini"), "General", "SLocalSavePath") != `Saves\TTW\` ||
		iniValue(filepath.Join(my, "FalloutPrefs.ini"), "General", "SLocalSavePath") != `Saves\TTW\` {
		t.Fatal("FalloutCustom.ini / FalloutPrefs.ini not set")
	}
	if !exists(filepath.Join(my, "Saves", "TTW")) {
		t.Fatal("the save folder wasn't made")
	}
	for _, p := range []profile{{vanilla: true}, {}} {
		if err := applyProfile(p, data, managed); err != nil {
			t.Fatal(err)
		}
		if v, _ := gameIniValue("General", "SLocalSavePath"); v != `Saves\` {
			t.Fatalf("%s: %q", p, v)
		}
	}
}

// A TTW save in the New Vegas folder goes to the TTW folder (with the files beside it), a New Vegas save in the TTW
// folder goes back
func TestSortSaves(t *testing.T) {
	root := t.TempDir()
	mk := func(rel, name, body string) {
		d := filepath.Join(root, filepath.FromSlash(rel))
		os.MkdirAll(d, 0o755)
		os.WriteFile(filepath.Join(d, name), []byte(body), 0o644)
	}
	ttwSave := "FO3SAVEGAME....|FalloutNV.esm|Fallout3.esm|TaleOfTwoWastelands.esm|"
	nvSave := "FO3SAVEGAME....|FalloutNV.esm|MojaveMostWanted.esp|"
	mk("Saves", "autosave.fos", ttwSave)
	mk("Saves", "autosave.nvse", "n")
	mk("Saves", "autosave.fos.bak", "b")
	mk("Saves", "Save 30.fos", nvSave)
	mk("Saves/TTW", "Save 2.fos", nvSave)
	mk("Saves/TTW", "autosave.fos", ttwSave)
	mk("Saves/TTW Vanilla", "Save 35.fos", ttwSave)
	mk("Saves/TTW Vanilla", "Save 35.nvse", "n")
	mk("Saves/Vanilla", "Save 1.fos", nvSave)
	sortSaves(root, `Saves\`)
	for _, p := range []string{"Saves/TTW/autosave (moved).fos", "Saves/TTW/autosave (moved).nvse",
		"Saves/TTW/autosave (moved).fos.bak", "Saves/TTW/autosave.fos", "Saves/Save 30.fos", "Saves/Save 2.fos",
		"Saves/TTW/Save 35.fos", "Saves/TTW/Save 35.nvse", "Saves/Save 1.fos"} {
		if !exists(filepath.Join(root, filepath.FromSlash(p))) {
			t.Errorf("missing %s", p)
		}
	}
	for _, p := range []string{"Saves/autosave.fos", "Saves/TTW/Save 2.fos", "Saves/Vanilla", "Saves/TTW Vanilla"} {
		if exists(filepath.Join(root, filepath.FromSlash(p))) {
			t.Errorf("%s still there", p)
		}
	}
}

// Windowed at 1920x1080 on a 2560x1440 monitor: the game draws at 2560x1440 (FalloutPrefs.ini agrees with
// FalloutCustom.ini) and the plugin's settings.ini asks for the borderless window; fillScreen=off leaves it all
func TestFillScreen(t *testing.T) {
	docs, plug := t.TempDir(), t.TempDir()
	t.Setenv("MMW_DOCUMENTS", docs)
	t.Setenv("MMW_SCREEN", "2560x1440")
	my := filepath.Join(docs, "My Games", "FalloutNV")
	os.MkdirAll(my, 0o755)
	os.WriteFile(filepath.Join(my, "FalloutPrefs.ini"), []byte("[Display]\r\nbFull Screen=0\r\niSize W=1920\r\niSize H=1080\r\n"), 0o644)
	os.WriteFile(filepath.Join(plug, "settings.ini"), []byte("; x\r\nradioVolume=0.50\r\nborderless=0\r\nheadlights=1\r\n"), 0o644)
	fillScreen(plug, map[string]string{"fillScreen": "off"})
	if v, _ := gameIniValue("Display", "iSize W"); v != "1920" {
		t.Fatalf("off: %s", v)
	}
	fillScreen(plug, map[string]string{})
	if iniValue(filepath.Join(my, "FalloutPrefs.ini"), "Display", "iSize W") != "2560" || iniValue(filepath.Join(my, "FalloutPrefs.ini"), "Display", "iSize H") != "1440" {
		t.Fatal("FalloutPrefs.ini not at the monitor's size")
	}
	b, _ := os.ReadFile(filepath.Join(plug, "settings.ini"))
	if !strings.Contains(string(b), "borderless=1") || strings.Contains(string(b), "borderless=0") || !strings.Contains(string(b), "headlights=1") {
		t.Fatalf("settings.ini: %q", b)
	}
}
