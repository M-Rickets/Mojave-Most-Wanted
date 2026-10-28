//go:build windows

package main

import (
	"debug/pe"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"golang.org/x/sys/windows/registry"
)

func windowsVersion() string {
	k := `SOFTWARE\Microsoft\Windows NT\CurrentVersion`
	name := regString(registry.LOCAL_MACHINE, k, "ProductName")
	build := regString(registry.LOCAL_MACHINE, k, "CurrentBuild")
	if b := build; b != "" {
		var n int
		fmt.Sscan(b, &n)
		if n >= 22000 {
			name = strings.Replace(name, "Windows 10", "Windows 11", 1)
		}
	}
	return fmt.Sprintf("%s %s (build %s)", name, regString(registry.LOCAL_MACHINE, k, "DisplayVersion"), build)
}

// laaText: whether FalloutNV.exe can use 4 GB (the 4GB Patcher, or the flag set some other way)
func laaText(exe string) string {
	f, err := pe.Open(exe)
	if err != nil {
		return "unreadable"
	}
	defer f.Close()
	s := "2 GB"
	if f.FileHeader.Characteristics&0x20 != 0 {
		s = "4 GB aware"
	}
	if exists(filepath.Join(filepath.Dir(exe), "FalloutNV_backup.exe")) {
		s += ", 4GB Patcher"
	}
	return s
}

func tailFile(p string, n int) string {
	b, err := os.ReadFile(p)
	if err != nil {
		return "(none)"
	}
	lines := splitLines(string(b))
	if len(lines) > n {
		lines = append([]string{fmt.Sprintf("... (%d lines before)", len(lines)-n)}, lines[len(lines)-n:]...)
	}
	return strings.Join(lines, "\r\n")
}

// The game's crashes from Windows' own log (Application Error, event 1000): when, and in which module
func gameCrashes() []string {
	out, err := hiddenCmd("wevtutil.exe", "qe", "Application", "/q:*[System[(EventID=1000)]]", "/c:60", "/rd:true", "/f:text").Output()
	if err != nil {
		return []string{"(couldn't read the Windows event log: " + err.Error() + ")"}
	}
	var res []string
	blocks := strings.Split(string(out), "Event[")
	reApp := regexp.MustCompile(`(?i)Faulting application name: ([^\r\n,]+)`)
	reMod := regexp.MustCompile(`(?i)Faulting module name: ([^\r\n,]+)`)
	reOff := regexp.MustCompile(`(?i)Fault offset: (0x[0-9a-f]+)`)
	reDate := regexp.MustCompile(`(?i)Date: ([^\r\n]+)`)
	for _, b := range blocks {
		a := reApp.FindStringSubmatch(b)
		if a == nil || !strings.EqualFold(strings.TrimSpace(a[1]), "FalloutNV.exe") {
			continue
		}
		m, o, d := reMod.FindStringSubmatch(b), reOff.FindStringSubmatch(b), reDate.FindStringSubmatch(b)
		line := ""
		if d != nil {
			line += strings.TrimSpace(d[1]) + ": "
		}
		if m != nil {
			line += "crashed in " + strings.TrimSpace(m[1])
		}
		if o != nil {
			line += " at " + o[1]
		}
		res = append(res, line)
		if len(res) >= 8 {
			break
		}
	}
	if len(res) == 0 {
		res = append(res, "no Fallout: New Vegas crashes in Windows' log")
	}
	return res
}

func gpuNames() []string {
	var out []string
	base := `SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}`
	for i := 0; i < 6; i++ {
		k := fmt.Sprintf(`%s\%04d`, base, i)
		if d := regString(registry.LOCAL_MACHINE, k, "DriverDesc"); d != "" {
			out = append(out, d+" (driver "+regString(registry.LOCAL_MACHINE, k, "DriverVersion")+")")
		}
	}
	return out
}

// The report: everything needed to see what's wrong, in one file
func makeReport() {
	app.mu.Lock()
	game := app.game
	app.mu.Unlock()
	if game == "" {
		game = findGame(false)
	}
	var b strings.Builder
	w := func(f string, v ...any) { fmt.Fprintf(&b, f+"\r\n", v...) }
	w("Mojave Enhanced - report (setup %s, %s)", appVersion, time.Now().Format("2006-01-02 15:04"))
	w("Send this whole file to whoever gave you the mod. It changed nothing in the game.")
	w("")
	w("== PC")
	w("Windows: %s", windowsVersion())
	w("CPU: %s", strings.TrimSpace(regString(registry.LOCAL_MACHINE, `HARDWARE\DESCRIPTION\System\CentralProcessor\0`, "ProcessorNameString")))
	w("Memory: %.0f GB", memoryGB())
	for _, g := range gpuNames() {
		w("Graphics: %s", g)
	}
	w("Running as administrator: %v", isAdmin())
	w("")
	w("== Fallout: New Vegas")
	if game == "" {
		w("not found")
	} else {
		exe := filepath.Join(game, "FalloutNV.exe")
		st, _ := os.Stat(exe)
		size := int64(0)
		if st != nil {
			size = st.Size()
		}
		w("Folder: %s", redact(game))
		w("FalloutNV.exe: version %s, %d bytes, %s", fileVersion(exe), size, laaText(exe))
		w("xNVSE: %s (nvse_1_4.dll), loader %v", fileVersion(filepath.Join(game, "nvse_1_4.dll")), exists(filepath.Join(game, "nvse_loader.exe")))
		w("Setup's record: %v", loadManifest(game))
		w("")
		w("== NVSE plugins (Data\\NVSE\\Plugins)")
		dlls, _ := filepath.Glob(filepath.Join(pluginDir(game), "*.dll"))
		inis, _ := filepath.Glob(filepath.Join(pluginDir(game), "*.ini"))
		dlls = append(dlls, inis...)
		sort.Strings(dlls)
		for _, d := range dlls {
			st, _ := os.Stat(d)
			w("  %-32s %-14s %s", filepath.Base(d), fileVersion(d), st.ModTime().Format("2006-01-02"))
		}
		w("")
		w("== Load order (plugins.txt)")
		w("%s", tailFile(pluginsTxtPath(), 80))
		esps, _ := filepath.Glob(filepath.Join(dataDir(game), "*.es[mp]"))
		w("(%d .esm/.esp files in Data)", len(esps))
		w("")
		w("== Plugins the load order names that aren't in Data")
		for _, l := range missingPlugins(game) {
			w("%s", l)
		}
		w("")
		w("== Mod managers")
		notes := modManagers(game, false)
		if len(notes) == 0 {
			w("none found")
		}
		for _, n := range notes {
			w("%s", n)
		}
		w("Your mods: %s (load order %s); world: %s", modsLabel(game), redact(loadOrderFile(game)), map[bool]string{true: "Tale of Two Wastelands", false: "New Vegas"}[ttwOn(game)])
		if lo := loadOrderFile(game); lo != pluginsTxtPath() {
			w("%s", tailFile(lo, 80))
		}
		w("")
		w("== Crashes")
		for _, c := range gameCrashes() {
			w("%s", c)
		}
		if cl, _ := filepath.Glob(filepath.Join(game, "crash*.log")); len(cl) > 0 {
			sort.Strings(cl)
			w("crash logs in the game folder: %s", strings.Join(baseNames(cl), ", "))
			w("--- %s", filepath.Base(cl[len(cl)-1]))
			w("%s", tailFile(cl[len(cl)-1], 80))
		}
		w("")
		w("== xNVSE's log (nvse.log)")
		w("%s", tailFile(filepath.Join(game, "nvse.log"), 60))
		w("")
		w("== Mojave Enhanced's log (the last game)")
		w("%s", tailFile(filepath.Join(pluginDir(game), "MojaveMostWanted.log"), 150))
		w("")
		w("== The launcher's log (the last Play)")
		w("%s", tailFile(filepath.Join(mmwDir(game), "mmw-launch.log"), 80))
	}
	w("")
	w("== Setup's log")
	app.mu.Lock()
	lines := app.logLines
	if len(lines) > 150 {
		lines = lines[len(lines)-150:]
	}
	w("%s", strings.Join(lines, "\r\n"))
	app.mu.Unlock()
	text := b.String()
	if home := os.Getenv("USERPROFILE"); home != "" {
		text = strings.ReplaceAll(text, home, `%USERPROFILE%`)
		text = strings.ReplaceAll(text, strings.ToLower(home), `%USERPROFILE%`)
	}
	p := filepath.Join(knownFolder(folderDesktop), "Mojave Enhanced report.txt")
	if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
		p = filepath.Join(os.TempDir(), "Mojave Enhanced report.txt")
		os.WriteFile(p, []byte(text), 0o644)
	}
	app.status("Report saved on your desktop: Mojave Enhanced report.txt (send it to whoever gave you the mod).")
	shellExecute("open", "notepad.exe", `"`+p+`"`, "", 1)
}

func baseNames(ps []string) []string {
	var o []string
	for _, p := range ps {
		o = append(o, filepath.Base(p))
	}
	return o
}
