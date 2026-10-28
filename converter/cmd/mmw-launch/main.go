package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"mmw/internal/gamecar"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"mmw/internal/audio"
	"mmw/internal/bnd2"
	"mmw/internal/car"
	"mmw/internal/steam"
)

type cacheEntry struct {
	Sig                                        string
	HalfTrack, HalfBase, HalfLength, HalfWidth float32
	Front, Rear, Clearance                     float32
	WheelRadius                                float32
	WheelRadiusF, WheelRadiusR                 float32
	SteerWheel                                 bool
	SteerAxis                                  [3]float32
	Engine                                     bool
	AudibleLo, AudibleHi                       float32
	Seconds                                    float64
	LampsDone                                  bool
	Lamps                                      [4]int
	Game                                       *gamecar.Built `json:",omitempty"`
	LampY                                      [4]float32
}

// The lamp check's line: each role's triangles and where they sit; a car with no headlights at the front or no
// tail/brake lamps at the back is flagged.
func lampLine(label string, n [4]int, y [4]float32) string {
	warn := ""
	if n[car.LightHead] == 0 || y[car.LightHead] < 0.5 {
		warn += " NO-HEADLIGHTS"
	}
	if n[car.LightTail]+n[car.LightBrake] == 0 {
		warn += " NO-TAIL-LIGHTS"
	} else if n[car.LightTail] == 0 {
		warn += " (tail lights use the brake lamps)"
	}
	return fmt.Sprintf("%s: lamps head %d (%.1f m), tail %d (%.1f m), brake %d (%.1f m), reverse %d (%.1f m)%s", label,
		n[0], y[0], n[1], y[1], n[2], y[2], n[3], y[3], warn)
}

var (
	cache   = map[string]cacheEntry{}
	cacheMu sync.Mutex
)

func main() {
	fnv := flag.String("fnv", "", "Fallout: New Vegas folder")
	mwFlag := flag.String("mw", "", "Need for Speed Most Wanted folder (default: found in Steam)")
	hpFlag := flag.String("hp", "", "Need for Speed Hot Pursuit Remastered folder (default: found in Steam)")
	convertOnly := flag.Bool("convert-only", false, "convert and write mmw.ini, but don't start the game")
	uninstall := flag.Bool("uninstall", false, "remove everything the launcher made (setup's \"undo everything\")")
	guardMode := flag.Bool("guard", false, "run beside the game: the panic key (Ctrl+Alt+End) and the router's forward (guard.go)")
	flag.Parse()

	exe, _ := os.Executable()
	managed := filepath.Dir(exe)
	if *guardMode {
		runGuard(filepath.Clean(strings.TrimSpace(strings.TrimRight(*fnv, `"`))), managed)
		return
	}
	os.RemoveAll(filepath.Join(managed, "dev"))
	if f, err := os.Create(filepath.Join(managed, "mmw-launch.log")); err == nil {
		log.SetOutput(io.MultiWriter(os.Stdout, f))
		defer f.Close()
	}
	log.SetFlags(log.Ltime)
	log.Print("Mojave Enhanced: getting the game ready")
	*fnv = strings.TrimSpace(strings.TrimRight(*fnv, `"`))
	if *fnv == "" {
		cwd, _ := os.Getwd()
		for _, p := range []string{filepath.Dir(managed), managed, cwd, steam.Find("22380", "Fallout New Vegas")} {
			if p != "" && exists(filepath.Join(p, "FalloutNV.exe")) {
				*fnv = p
				break
			}
		}
	}
	if *fnv == "" || !exists(filepath.Join(*fnv, "FalloutNV.exe")) {
		fail("couldn't find Fallout: New Vegas (no FalloutNV.exe in \"" + *fnv + "\"); start with --fnv <game folder>")
	}
	*fnv = filepath.Clean(*fnv)
	log.Printf("Fallout: New Vegas: %s", *fnv)
	data := filepath.Join(*fnv, "Data")
	pluginDir := filepath.Join(data, "NVSE", "Plugins", "MojaveMostWanted")
	installPendingPlugin(filepath.Join(data, "NVSE", "Plugins", "MojaveMostWanted.dll"))
	cachePath := filepath.Join(managed, "mmw-cache.json")
	if b, err := os.ReadFile(cachePath); err == nil {
		json.Unmarshal(b, &cache)
	}

	if *uninstall {
		uninstallAll(data, managed, pluginDir)
		return
	}
	if applyPending(*fnv, managed) {
		pruneAnims(data, managed)
	} else {
		log.Print("animations: a pending zip didn't go in fully - nothing pruned this time")
	}
	switchPendingAddons(*fnv, managed)
	paths := readPaths(filepath.Join(managed, "paths.ini"))
	mw := findGame("mw", *mwFlag, paths)
	if strings.EqualFold(paths["cars"], "off") && mw != "" {
		log.Print("cars: off (setup's PLAY WITHOUT CARS) - none built, the garage stays empty")
		mw = ""
	}
	hp := findGame("hp", *hpFlag, paths)
	prof := readProfile(data, paths)
	if err := applyProfile(prof, data, managed); err != nil {
		fail(err.Error())
	}
	alwaysActive(pluginDir, managed)
	gameSettings(data)
	fillScreen(pluginDir, paths)
	if prof.vanilla {
		mw, hp = "", ""
	}

	ini := []string{"; written by mmw-launch.exe on every Play from your own game installs"}
	set := func(k string, v any) {
		switch x := v.(type) {
		case float32:
			v = strconv.FormatFloat(float64(x), 'f', -1, 32)
		case float64:
			v = strconv.FormatFloat(x, 'f', -1, 64)
		}
		ini = append(ini, fmt.Sprintf("%s=%v", k, v))
	}
	set("mw", b2i(mw != ""))
	set("hp", b2i(hp != ""))
	set("topSpeedScale", topSpeedScale)
	set("vanilla", b2i(prof.vanilla))
	sa, saSec := smoothAds(prof, pluginDir, data)
	set("smoothAds", b2i(sa))
	set("smoothAdsSec", saSec)
	if prof.vanilla {
		ini = append(ini, vanillaTuning...)
	}
	set("maxSpeedMs", maxSpeedMs)

	car.Logf = func(f string, a ...any) {}
	if err := car.WriteSharedPaint(data, paintGlossLevel, sheetPaintMaskLevels); err != nil {
		log.Printf("paint shop textures: %v", err)
	}
	if mw != "" {
		type carOut struct {
			e      cacheEntry
			ok, en bool
		}
		outs := make([]carOut, len(sheetCars))
		jobs := make(chan int)
		var wg sync.WaitGroup
		var done int32
		workers := runtime.NumCPU()
		if workers > 4 {
			workers = 4
		}
		if workers < 1 {
			workers = 1
		}
		for w := 0; w < workers; w++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for i := range jobs {
					c := sheetCars[i]
					e, ok := convertCar(mw, data, c, filepath.Join(mw, "SHADERS.BNDL"))
					en := ok && convertEngine(mw, pluginDir, c)
					outs[i] = carOut{e, ok, en}
					if n := atomic.AddInt32(&done, 1); n%10 == 0 {
						log.Printf("cars: %d/%d checked", n, len(sheetCars))
					}
				}
			}()
		}
		for i, c := range sheetCars {
			if c.Game == "mw" {
				jobs <- i
			}
		}
		close(jobs)
		wg.Wait()
		have := 0
		for i, c := range sheetCars {
			if c.Game != "mw" {
				continue
			}
			e, ok := outs[i].e, outs[i].ok
			set("car."+c.ID, b2i(ok))
			if ok {
				have++
				set("car."+c.ID+".halfTrack", e.HalfTrack)
				set("car."+c.ID+".halfBase", e.HalfBase)
				set("car."+c.ID+".halfLength", e.HalfLength)
				set("car."+c.ID+".halfWidth", e.HalfWidth)
				set("car."+c.ID+".front", e.Front)
				set("car."+c.ID+".rear", e.Rear)
				set("car."+c.ID+".clearance", e.Clearance)
				set("car."+c.ID+".wheelRadius", e.WheelRadius)
				set("car."+c.ID+".wheelRadiusF", e.WheelRadiusF)
				set("car."+c.ID+".wheelRadiusR", e.WheelRadiusR)
				set("car."+c.ID+".topSpeed", c.TopSpeedMph)
				if e.SteerWheel {
					set("car."+c.ID+".steerAxisX", e.SteerAxis[0])
					set("car."+c.ID+".steerAxisY", e.SteerAxis[1])
					set("car."+c.ID+".steerAxisZ", e.SteerAxis[2])
				} else {
					set("car."+c.ID+".steerAxisX", 0)
					set("car."+c.ID+".steerAxisY", 0)
					set("car."+c.ID+".steerAxisZ", 0)
				}
				if outs[i].en {
					set("car."+c.ID+".engine", 1)
					cacheMu.Lock()
					ee := cache["engine:"+c.ID]
					cacheMu.Unlock()
					if ee.AudibleHi > 0 {
						set("car."+c.ID+".rpmLow", ee.AudibleLo)
						set("car."+c.ID+".rpmHigh", ee.AudibleHi)
					}
				}
			}
		}
		log.Printf("cars: %d of %d ready (DLC cars appear when their pack is installed)", have, len(sheetCars))
	}
	gameCars(data, func(k string, v any) { set(k, v) })
	set("skids", b2i(convertSkids(pluginDir, mw)))
	set("crashes", b2i(convertCrashes(pluginDir, mw)))
	set("smoke", tyreSmoke(data, managed, hp, func(k string, v any) { set(k, v) }))
	set("gameFonts", gameFonts(data, pluginDir))
	weather := !prof.vanilla || paths["weather"] == "1" || localUnlocked("weather")
	if !weather {
		removeRainfall(data)
	}
	set("weather", b2i(weather))
	set("snowfall", b2i(weather && makeSnowfall(data)))
	set("rainfall", b2i(weather && makeRainfall(data)))
	dumpSkyFiles(data)
	dumpWeathers(data, []string{"FalloutNV.esm", "DeadMoney.esm", "HonestHearts.esm", "OldWorldBlues.esm", "LonesomeRoad.esm",
		"Fallout3.esm", "Anchorage.esm", "ThePitt.esm", "BrokenSteel.esm", "PointLookout.esm", "Zeta.esm",
		"TaleOfTwoWastelands.esm", "YUPTTW.esm"})
	if id, name := snowWeather(filepath.Join(data, "FalloutNV.esm"), managed); id != 0 {
		set("snowWeather", fmt.Sprintf("%08X", id))
		log.Printf("snow sky: weather %s (%08X)", name, id)
	} else {
		log.Printf("snow sky: no overcast weather found in FalloutNV.esm (snow falls under the usual sky)")
	}
	tracks := convertRadio(managed, map[string]string{"mw": mw, "hp": hp})
	set("radio", b2i(len(tracks) > 0))
	set("radioDir", filepath.Join(managed, "radio")+`\`)
	set("tracks", len(tracks))
	for i, t := range tracks {
		set(fmt.Sprintf("track.%d", i+1), t)
	}

	winter, winterStage := winterState(pluginDir, paths)
	winter = winter && weather
	set("winter", b2i(winter))
	set("winterStages", winterStages)
	if err := os.MkdirAll(pluginDir, 0o755); err != nil {
		fail(err.Error())
	}
	if err := os.WriteFile(filepath.Join(pluginDir, "mmw.ini"), []byte(strings.Join(ini, "\r\n")+"\r\n"), 0o644); err != nil {
		fail("couldn't write mmw.ini: " + err.Error())
	}
	log.Print("wrote mmw.ini")
	roadExtra := roadRecordTextures(data, managed)
	repaveRoads(data, managed, !prof.vanilla && paths["cleanRoads"] != "0", roadExtra)
	roadSet := map[string]bool{}
	for _, r := range append(append([]roadRow{}, sheetRoads...), roadExtra...) {
		roadSet[strings.ToLower(r.Path)] = true
	}
	winterize(data, managed, winter, winterStage, prof.ttw, roadSet)
	if !weather {
		if d := filepath.Join(managed, "winter"); exists(d) {
			if err := os.RemoveAll(d); err == nil {
				log.Print("winter: the snowed ground's stages removed (no weather)")
			}
		}
	}
	if b, err := json.MarshalIndent(cache, "", " "); err == nil {
		os.WriteFile(cachePath, b, 0o644)
	}
	logArchiveSettings()
	if old, _ := filepath.Glob(filepath.Join(pluginDir, "sound", "hfx_*.mmwx")); len(old) > 0 {
		for _, f := range old {
			os.Remove(f)
		}
		log.Printf("exhaust pops and backfires: removed (%d files)", len(old))
	}
	if *convertOnly {
		return
	}
	startGame(*fnv)
	spawnGuard(*fnv, exe)
}

// startGame starts New Vegas with xNVSE.
func startGame(fnv string) {
	if exists(filepath.Join(fnv, "FalloutNV_backup.exe")) && exists(filepath.Join(fnv, "steam_api.dll")) {
		if err := exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", "steam://rungameid/22380").Start(); err == nil {
			log.Printf("started New Vegas through Steam (steam://rungameid/22380; the 4GB Patcher loads xNVSE)")
			return
		} else {
			log.Printf("Steam link: %v", err)
		}
		roots := steam.Roots()
		for _, root := range roots {
			exe := filepath.Join(root, "steam.exe")
			if !exists(exe) {
				log.Printf("Steam: no steam.exe in %s", root)
				continue
			}
			cmd := exec.Command(exe, "-applaunch", "22380")
			if err := cmd.Start(); err != nil {
				log.Printf("Steam: couldn't start %s: %v", exe, err)
				continue
			}
			log.Printf("started New Vegas through Steam (4GB Patcher loads xNVSE)")
			cmd.Process.Release()
			return
		}
		log.Printf("Steam folders checked: %v", roots)
		log.Print("Steam not found; trying nvse_loader.exe")
	}
	if exists(filepath.Join(fnv, "steam_api.dll")) && !exists(filepath.Join(fnv, "steam_appid.txt")) {
		os.WriteFile(filepath.Join(fnv, "steam_appid.txt"), []byte("22380"), 0o644)
	}
	loader := filepath.Join(fnv, "nvse_loader.exe")
	cmd := exec.Command(loader)
	cmd.Dir = fnv
	if err := cmd.Start(); err != nil {
		fail("couldn't start " + loader + ": " + err.Error())
	}
	log.Printf("started New Vegas through xNVSE (pid %d)", cmd.Process.Pid)
}

func fail(msg string) {
	log.Print("error: " + msg)
	time.Sleep(8 * time.Second)
	os.Exit(1)
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

// paths.ini (next to this program) lets a player point at installs Steam doesn't know about: mw=..., hp=...
func readPaths(path string) map[string]string {
	out := map[string]string{}
	b, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	for _, line := range strings.Split(string(b), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok && !strings.HasPrefix(k, ";") {
			out[strings.TrimSpace(k)] = strings.Trim(strings.TrimSpace(v), `"`)
		}
	}
	return out
}

func findGame(id, flagPath string, paths map[string]string) string {
	var g gameRow
	for _, r := range sheetGames {
		if r.ID == id {
			g = r
		}
	}
	try := []string{flagPath, paths[id]}
	if p := steam.Find(g.AppID, g.Dir); p != "" {
		try = append(try, p)
	}
	for _, p := range try {
		if p == "" {
			continue
		}
		if _, err := os.Stat(filepath.Join(p, g.Exe)); err == nil {
			log.Printf("%s: %s", g.Name, p)
			return p
		}
	}
	log.Printf("%s: not installed (looked in your Steam libraries)", g.Name)
	return ""
}

// Signature of source files: changes when the game updates or the converter changes
func sig(files ...string) string {
	parts := []string{fmt.Sprint(converterVersion)}
	for _, f := range files {
		if st, err := os.Stat(f); err == nil {
			parts = append(parts, fmt.Sprintf("%s:%d:%d", filepath.Base(f), st.Size(), st.ModTime().Unix()))
		} else {
			parts = append(parts, filepath.Base(f)+":missing")
		}
	}
	return strings.Join(parts, "|")
}

// localUnlocked: the author's own PC opens a feature by its name in %LOCALAPPDATA%\MojaveMostWanted\unlocked.txt
// (setup's roll-out reads the same file; "all" doesn't count here), which no release ships
func localUnlocked(f string) bool {
	b, err := os.ReadFile(filepath.Join(os.Getenv("LOCALAPPDATA"), "MojaveMostWanted", "unlocked.txt"))
	if err != nil {
		return false
	}
	for _, x := range strings.Fields(strings.ToLower(string(b))) {
		if x == f {
			return true
		}
	}
	return false
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

func convertCar(gameDir, data string, c carRow, shaders string) (cacheEntry, bool) {
	drive := filepath.Join(data, "meshes", "mmw", "cars", c.ID, "drive.nif")
	for _, b := range c.Bundles {
		src := filepath.Join(gameDir, filepath.FromSlash(b))
		if !exists(src) {
			continue
		}
		s := sig(src, shaders) + fmt.Sprintf("|paint%v", c.Paint) + "|snow3|gauge1"
		if c.ScreenTex != "" {
			s += fmt.Sprintf("|screen%s%v%s", c.ScreenTex, c.ScreenUV, c.ScreenKind)
		}
		if c.ArchFit {
			s += "|archfit"
		}
		var extra car.Extra
		if c.Extra != "" {
			if err := json.Unmarshal([]byte(c.Extra), &extra); err != nil {
				log.Printf("%s: extra settings: %v", c.Label, err)
			}
			s += "|x" + c.Extra + "|links2"
		}
		if hd, _ := filepath.Glob(filepath.Join(data, "textures", "mmw", "hd", c.ID+"_*.dds")); len(hd) > 0 {
			s += "|hd" + sig(hd...)
		}
		key := "car:" + c.ID
		cacheMu.Lock()
		e, ok := cache[key]
		cacheMu.Unlock()
		if ok && e.Sig == s && e.LampsDone && exists(drive) && exists(filepath.Join(data, "meshes", "mmw", "wheels", c.ID+"_f.nif")) {
			log.Printf("%s: up to date", c.Label)
			log.Print(lampLine(c.Label, e.Lamps, e.LampY))
			return e, true
		}
		log.Printf("%s: converting from %s", c.Label, b)
		r, err := car.Convert(car.Options{ID: c.ID, Bundles: []string{src}, Shared: []string{shaders}, Paint: c.Paint, DataDir: data, MaxTex: maxTextureSize, Bone: "Bip01", Scale: carScale,
			ScreenTex: c.ScreenTex, ScreenUV: c.ScreenUV, ScreenKind: c.ScreenKind, ArchFit: c.ArchFit, Extra: extra})
		if err != nil {
			log.Printf("%s: %v", c.Label, err)
			continue
		}
		e = cacheEntry{Sig: s, HalfTrack: r.HalfTrack, HalfBase: r.HalfBase, HalfLength: r.HalfLength, HalfWidth: r.Width * unitsPerMetre / 2,
			Front: r.Front, Rear: r.Rear, Clearance: r.Clearance, WheelRadius: r.WheelRadius, WheelRadiusF: r.WheelRadiusF, WheelRadiusR: r.WheelRadiusR, SteerWheel: r.SteerWheel, SteerAxis: r.SteerAxis,
			LampsDone: true, Lamps: r.Lamps, LampY: r.LampY}
		cacheMu.Lock()
		cache[key] = e
		cacheMu.Unlock()
		log.Printf("%s: %d triangles, %.2f m long", c.Label, r.Triangles, r.Length)
		if r.SteerWheel {
			log.Printf("%s: steering wheel %.2f m across, %d triangles, column (%.2f %.2f %.2f)", c.Label, 2*r.SteerRadius, r.SteerTris, r.SteerAxis[0], r.SteerAxis[1], r.SteerAxis[2])
		} else {
			log.Printf("%s: no steering wheel found (it won't turn, no dash gauges)", c.Label)
		}
		log.Print(lampLine(c.Label, r.Lamps, r.LampY))
		return e, true
	}
	log.Printf("%s: not installed", c.Label)
	return cacheEntry{}, false
}

func convertEngine(gameDir, pluginDir string, c carRow) bool {
	out := filepath.Join(pluginDir, "sound", "eng_"+c.ID+".mmwe")
	for _, b := range c.Bundles {
		src := filepath.Join(gameDir, filepath.FromSlash(b))
		if !exists(src) {
			continue
		}
		s := sig(src) + "|audible1"
		key := "engine:" + c.ID
		cacheMu.Lock()
		e, ok := cache[key]
		cacheMu.Unlock()
		if ok && e.Sig == s && exists(out) {
			return e.Engine
		}
		bd, err := bnd2.Open(src)
		if err != nil {
			continue
		}
		on, off, err := audio.PickEngine(bd)
		if err != nil {
			log.Printf("%s engine: %v", c.Label, err)
			continue
		}
		os.MkdirAll(filepath.Dir(out), 0o755)
		if err := os.WriteFile(out, on.WriteEngine(), 0o644); err != nil {
			log.Printf("%s engine: %v", c.Label, err)
			return false
		}
		offPath := strings.TrimSuffix(out, ".mmwe") + "_off.mmwe"
		if off != nil {
			os.WriteFile(offPath, off.WriteEngine(), 0o644)
		} else {
			os.Remove(offPath)
		}
		lo, hi, _ := audio.AudibleRPM(bd)
		cacheMu.Lock()
		cache[key] = cacheEntry{Sig: s, Engine: true, AudibleLo: lo, AudibleHi: hi}
		cacheMu.Unlock()
		log.Printf("%s engine: recorded %.0f-%.0f rpm, heard %.0f-%.0f", c.Label, on.MinRPM, on.MaxRPM, lo, hi)
		return true
	}
	return false
}

// convertRadio repacks every song of both soundtracks to MP3 (no re-encoding) in <managed>\radio and returns the
// playlist entries "file|title", shuffled.
func convertRadio(managed string, gameDirs map[string]string) []string {
	radioDir := filepath.Join(managed, "radio")
	os.MkdirAll(radioDir, 0o755)
	type job struct{ src, out, title, key string }
	var jobs []job
	for _, r := range sheetRadio {
		dir := gameDirs[r.Game]
		if dir == "" {
			continue
		}
		files, _ := filepath.Glob(filepath.Join(dir, filepath.FromSlash(r.SongsDir), "*.SPS"))
		sort.Strings(files)
		for i, f := range files {
			base := strings.TrimSuffix(filepath.Base(f), filepath.Ext(f))
			jobs = append(jobs, job{src: f, out: filepath.Join(radioDir, r.Game+"_"+base+".mp3"),
				title: fmt.Sprintf("%s, track %d of %d", r.Station, i+1, len(files)), key: "song:" + r.Game + ":" + base})
		}
	}
	if len(jobs) == 0 {
		return nil
	}
	log.Printf("radio: %d songs", len(jobs))
	var mu sync.Mutex
	var out []string
	done := 0
	work := make(chan job)
	var wg sync.WaitGroup
	for w := 0; w < 4; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := range work {
				s := sig(j.src)
				cacheMu.Lock()
				e, ok := cache[j.key]
				cacheMu.Unlock()
				good := ok && e.Sig == s && exists(j.out)
				if !good {
					b, err := os.ReadFile(j.src)
					if err == nil {
						var info *audio.SPSInfo
						var payload []byte
						info, payload, err = audio.ReadSPS(b)
						if err == nil && info.Codec != 5 {
							err = fmt.Errorf("codec %d", info.Codec)
						}
						if err == nil {
							var mp3 []byte
							mp3, _, err = audio.EALayer3ToMP3(payload)
							if err == nil {
								err = os.WriteFile(j.out, mp3, 0o644)
							}
							if err == nil {
								e = cacheEntry{Sig: s, Seconds: float64(info.Samples) / float64(info.Rate)}
								cacheMu.Lock()
								cache[j.key] = e
								cacheMu.Unlock()
								good = true
							}
						}
					}
					if err != nil {
						log.Printf("radio: %s: %v", filepath.Base(j.src), err)
					}
				}
				mu.Lock()
				done++
				if good {
					out = append(out, filepath.Base(j.out)+"|"+j.title)
				}
				if done%10 == 0 || done == len(jobs) {
					log.Printf("radio: %d/%d", done, len(jobs))
				}
				mu.Unlock()
			}
		}()
	}
	for _, j := range jobs {
		work <- j
	}
	close(work)
	wg.Wait()
	rand.Shuffle(len(out), func(i, k int) { out[i], out[k] = out[k], out[i] })
	return out
}

// installPendingPlugin puts a plugin update in place: the developer's copy link can't replace the plugin while the game
// has it open, so an update waits beside it as MojaveMostWanted.dll.new until the next Play.
func installPendingPlugin(dll string) {
	pending := dll + ".new"
	if !exists(pending) {
		return
	}
	for try := 0; try < 10; try++ {
		if err := os.Rename(pending, dll); err == nil {
			log.Printf("plugin update installed")
			return
		} else if try == 9 {
			log.Printf("plugin update: %v (is the game still running?)", err)
		}
		time.Sleep(500 * time.Millisecond)
	}
}

// uninstallAll removes what the launcher made
func uninstallAll(data, managed, pluginDir string) {
	log.Print("Mojave Enhanced: removing what the launcher made")
	restoreSaves(managed)
	os.Remove(filepath.Join(managed, "mmw-guard.exe"))
	os.Remove(filepath.Join(managed, "mmw-guard.log"))
	os.Remove(filepath.Join(data, "meshes", "AnimGroupOverride", "MojaveMostWanted.json.off"))
	extra := roadRecordTextures(data, managed)
	repaveRoads(data, managed, false, extra)
	roadSet := map[string]bool{}
	for _, r := range append(append([]roadRow{}, sheetRoads...), extra...) {
		roadSet[strings.ToLower(r.Path)] = true
	}
	winterize(data, managed, false, 0, true, roadSet)
	os.RemoveAll(filepath.Join(managed, "winter"))
	n := 0
	for _, d := range []string{filepath.Join(data, "meshes", "mmw"), pluginDir} {
		if exists(d) {
			filepath.Walk(d, func(string, os.FileInfo, error) error { n++; return nil })
			os.RemoveAll(d)
		}
	}
	tex := filepath.Join(data, "textures", "mmw")
	if ents, err := os.ReadDir(tex); err == nil {
		for _, e := range ents {
			if strings.EqualFold(e.Name(), "hd") {
				continue
			}
			os.RemoveAll(filepath.Join(tex, e.Name()))
			n++
		}
		os.Remove(tex)
	}
	os.Remove(filepath.Join(managed, "mmw-cache.json"))
	os.RemoveAll(filepath.Join(managed, "radio"))
	log.Printf("removed %d files and folders", n)
}
