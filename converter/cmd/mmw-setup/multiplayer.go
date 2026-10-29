//go:build windows

package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"mmw/cmd/mmw-setup/help"
	"mmw/cmd/mmw-setup/scene"
	"mmw/internal/p2p"
)

// Playing together from setup

func netStopPath(game string) string {
	return filepath.Join(pluginDir(game), "MojaveMostWanted", "net.stop")
}

func netIniPath(game string) string {
	return filepath.Join(pluginDir(game), "MojaveMostWanted", "net.ini")
}

type netState struct {
	Mode, Name, Code, Host string
	Port                   int
	Expires                time.Time
	UPnP                   bool
}

func readNet(game string) netState {
	var s netState
	b, err := os.ReadFile(netIniPath(game))
	if err != nil {
		return s
	}
	for _, l := range splitLines(string(b)) {
		k, v, ok := strings.Cut(l, "=")
		if !ok {
			continue
		}
		switch k {
		case "mode":
			s.Mode = v
		case "; code":
			s.Code = v
		case "; hostName":
			s.Host = v
		case "name":
			s.Name = v
		case "port":
			fmt.Sscan(v, &s.Port)
		case "expires":
			var u int64
			fmt.Sscan(v, &u)
			s.Expires = time.Unix(u, 0)
		case "upnp":
			s.UPnP = v == "1"
		}
	}
	if s.Mode != "" && !s.Expires.IsZero() && time.Now().After(s.Expires) {
		s.Mode = ""
	}
	return s
}

// The name friends see: the one chosen in game (chat: /name, kept in settings.ini playerName=), else the PC's user name
func playerName() string {
	app.mu.Lock()
	game := app.game
	app.mu.Unlock()
	if game != "" {
		if b, err := os.ReadFile(filepath.Join(pluginDir(game), "MojaveMostWanted", "settings.ini")); err == nil {
			for _, l := range strings.Split(string(b), "\n") {
				if v, ok := strings.CutPrefix(strings.TrimSpace(l), "playerName="); ok && strings.TrimSpace(v) != "" {
					v = strings.TrimSpace(v)
					if len(v) > 20 {
						v = v[:20]
					}
					return v
				}
			}
		}
	}
	n := os.Getenv("USERNAME")
	if n == "" {
		n = "player"
	}
	if len(n) > 20 {
		n = n[:20]
	}
	return n
}

// The pills under the checklist, from where things stand
func netPills() {
	enforceRollout()
	app.mu.Lock()
	game, installed := app.game, app.installed
	app.mu.Unlock()
	var pills []string
	var on []bool
	if installed && game != "" {
		s := readNet(game)
		switch s.Mode {
		case "host":
			pills, on = []string{"COPY JOIN CODE", "STOP HOSTING"}, []bool{true, false}
		case "join":
			pills, on = []string{"JOINING " + strings.ToUpper(s.Host), "LEAVE"}, []bool{true, false}
		default:
			pills, on = []string{"HOST A GAME", "JOIN A FRIEND"}, []bool{false, false}
		}
		pills, on = append([]string{namePill()}, pills...), append([]bool{nameEditing()}, on...)
		if !man.Vanilla {
			pills, on = append(pills, "PLAY WITHOUT CARS"), append(on, man.NoCars)
		}
		var labels, vl, groups, vg []string
		var aon, vo []bool
		n := 0
		if unlocked("addons") {
			labels, aon, groups, n = addonItems(game)
		}
		if unlocked("mmw") {
			vl, vo, vg = versionItems()
		}
		if len(labels)+len(vl) > 0 {
			label := fmt.Sprintf("ADD-ONS %d/%d", n, len(labels))
			switch {
			case man.Vanilla:
				label = "VANILLA STABLE"
			case len(labels) == 0:
				label = "GAME"
			}
			pills, on = append(pills, label), append(on, n > 0 || man.Vanilla)
		}
		s3, s4 := "Build the cars from Need for Speed", "Ready to drive"
		if man.Vanilla {
			s3, s4 = "Set up vanilla stable (no cars)", "Ready to play"
		}
		app.with(func(st *scene.State) { st.Steps[3], st.Steps[4] = s3, s4 })
		title := "GAME & ADD-ONS"
		if len(labels) == 0 {
			title = "GAME"
		} else if len(vl) == 0 {
			title = "ADD-ONS"
		}
		app.with(func(st *scene.State) {
			st.MenuTitle = title
			st.MenuItems, st.MenuOn, st.MenuGroups = append(vl, labels...), append(vo, aon...), append(vg, groups...)
			if len(st.MenuItems) == 0 {
				st.Menu = false
			}
		})
	}
	ttw, mods := false, modsLabel(game)
	if installed && game != "" {
		ttw = ttwOn(game)
	}
	left, right := help.Columns(installed && game != "", ttw, mods)
	app.with(func(st *scene.State) { st.Pills, st.PillOn, st.SideL, st.SideR = pills, on, left, right })
	if infoOpen == "mods" {
		showInfo("mods")
	}
}

// The text panel open now ("" none): "first", "whatsin", "keys", "mods"
var infoOpen string

func showInfo(kind string) {
	weather := unlockedByName("weather")
	app.mu.Lock()
	game := app.game
	g := help.Game{Full: !man.Vanilla, Weather: weather}
	app.mu.Unlock()
	var title string
	var lines []scene.Line
	cols := 2
	switch kind {
	case "first":
		title, lines, cols = "FIRST TIME START-UP", help.FirstTimeLines(), 1
	case "whatsin":
		title, lines = "WHAT'S IN IT", help.WhatsInLines(g)
	case "keys":
		title, lines = "KEYBINDS", help.KeysLines(g)
	case "mods":
		title, lines, cols = "YOUR MODS", modsLines(game), 1
	default:
		return
	}
	infoOpen = kind
	app.with(func(st *scene.State) {
		st.Info, st.InfoTitle, st.InfoLines, st.InfoCols, st.Menu = true, title, lines, cols, false
	})
}

func pillPressed(name string) {
	app.mu.Lock()
	menuOpen, info := app.st.Menu, app.st.Info
	app.mu.Unlock()
	if name == scene.MenuClose {
		infoOpen = ""
		app.with(func(st *scene.State) { st.Menu, st.Info = false, false })
		return
	}
	if info {
		modsPressed(name)
		return
	}
	switch name {
	case scene.HitName(help.FirstTime):
		showInfo("first")
		return
	case scene.HitName(help.WhatsIn):
		showInfo("whatsin")
		return
	case scene.HitName(help.KeysPill):
		showInfo("keys")
		return
	case strings.ToLower(modsFolder), strings.ToLower(modsMO2), strings.ToLower(modsVortex):
		showInfo("mods")
		return
	}
	if worldPressed(name) {
		return
	}
	if strings.HasPrefix(name, "add-ons ") || (name == "vanilla stable" || name == "game") && !menuOpen {
		app.with(func(st *scene.State) { st.Menu = true })
		return
	}
	if strings.HasPrefix(name, "name: ") {
		if !nameEditing() {
			nameStart()
		}
		return
	}
	if versionPressed(name) || addonPressed(name) {
		return
	}
	if name == "play without cars" {
		if waitingNFS {
			select {
			case skipCars <- true:
			default:
			}
			return
		}
		toggleCars()
		return
	}
	if strings.HasPrefix(name, "choose ") {
		chooseFile()
		return
	}
	app.mu.Lock()
	game, busy := app.game, app.busy
	app.mu.Unlock()
	if game == "" || busy {
		return
	}
	switch {
	case name == "host a game":
		go hostGame(game)
	case name == "join a friend":
		joinGame(game)
	case name == "copy join code":
		s := readNet(game)
		if s.Code != "" && setClipboard(s.Code) {
			app.status("Join code copied again: paste it to your friends.")
		}
	case name == "stop hosting" || name == "leave":
		go func() {
			stopNet(game)
			app.status("Playing on your own again (a running game is told at once).")
			netPills()
		}()
	case strings.HasPrefix(name, "joining "):
		app.status("Press PLAY to join. LEAVE to play on your own.")
	}
}

// stopNet: playing together off - a running game drops everyone at once, the router's forward comes down, the session
// (and with it the join code) is gone
func stopNet(game string) {
	st := readNet(game)
	putFile(netStopPath(game), []byte("stop\r\n"))
	if st.UPnP && st.Port > 0 {
		if u, err := p2p.FindRouter(2 * time.Second); err == nil {
			if err := u.UnmapPort(st.Port); err == nil {
				app.logf("Router: UDP %d closed again", st.Port)
			}
		}
	}
	os.Remove(netIniPath(game))
	app.logf("Playing together: off")
}

func hostGame(game string) {
	app.mu.Lock()
	app.busy = true
	app.mu.Unlock()
	defer func() { app.mu.Lock(); app.busy = false; app.mu.Unlock(); netPills() }()
	app.status("Opening the game to your friends...")
	if exists(netIniPath(game)) {
		stopNet(game)
	}
	os.Remove(netStopPath(game))
	port := p2p.RandomPort()
	key, session := p2p.NewSession()
	addrs := []string{}
	public, upnp := "", false
	if u, err := p2p.FindRouter(3 * time.Second); err == nil {
		if perm, err := u.MapPort(port, "Mojave Enhanced", 4*3600); err == nil {
			upnp = true
			how := ", for 4 hours"
			if perm {
				how = "; this router only takes forwards without an end - taken down when the game closes"
			}
			app.logf("Router: UDP %d now comes to this PC (%s, UPnP%s)", port, u.Local, how)
		} else {
			app.logf("Router: couldn't open UDP %d by UPnP (%v)", port, err)
		}
		public = u.ExternalIP()
	} else {
		app.logf("Router: %v", err)
	}
	if public == "" {
		if ip, err := p2p.StunAddr("stun.l.google.com:19302", 3*time.Second); err == nil {
			public = ip
		}
	}
	if public != "" {
		addrs = append(addrs, fmt.Sprintf("%s:%d", public, port))
		app.logf("Internet address: %s", public)
	}
	for _, ip := range p2p.LocalIPv4s() {
		addrs = append(addrs, fmt.Sprintf("%s:%d", ip, port))
	}
	rule := fmt.Sprintf(`advfirewall firewall add rule name="Mojave Enhanced" dir=in action=allow protocol=UDP localport=%d program="%s" enable=yes`, port, filepath.Join(game, "FalloutNV.exe"))
	shellExecute("runas", "netsh.exe", `advfirewall firewall delete rule name="Mojave Most Wanted"`, "", 0)
	shellExecute("runas", "netsh.exe", `advfirewall firewall delete rule name="Mojave Enhanced"`, "", 0)
	time.Sleep(300 * time.Millisecond)
	if err := shellExecute("runas", "netsh.exe", rule, "", 0); err != nil {
		app.logf("Firewall rule: %v (Windows will ask when the game starts instead)", err)
	} else {
		app.logf("Firewall: New Vegas may take UDP %d", port)
	}
	jc := p2p.JoinCode{Key: key, Session: session, Expires: time.Now().Add(p2p.Lifetime), Name: playerName(), Addrs: addrs}
	code := jc.String()
	up := 0
	if upnp {
		up = 1
	}
	ini := fmt.Sprintf("; written by setup: hosting\r\nmode=host\r\nport=%d\r\nkey=%s\r\nsession=%d\r\nexpires=%d\r\nname=%s\r\nupnp=%d\r\n; code=%s\r\n; hostName=%s\r\n",
		port, hex.EncodeToString(jc.Key[:]), jc.Session, jc.Expires.Unix(), jc.Name, up, code, jc.Name)
	if err := putFile(netIniPath(game), []byte(ini)); err != nil {
		app.status("Couldn't write the session: %v", err)
		return
	}
	saveManifest()
	setClipboard(code)
	app.logf("Hosting: join code %s (addresses %s; good until %s)", code, strings.Join(addrs, ", "), jc.Expires.Format("15:04 Jan 2"))
	if public == "" {
		app.status("Join code copied, but no internet address found: friends on your network or VPN can join. Press PLAY.")
	} else {
		app.status("Join code copied (good for 12 hours): send it to your friends, then press PLAY - you let each one in, in game.")
	}
}

func joinGame(game string) {
	text := getClipboard()
	jc, err := p2p.ParseJoinCode(text, time.Now())
	if err != nil {
		m := err.Error()
		app.status("%s.", strings.ToUpper(m[:1])+m[1:])
		return
	}
	os.Remove(netStopPath(game))
	ini := fmt.Sprintf("; written by setup: joining %s\r\nmode=join\r\nkey=%s\r\nsession=%d\r\nexpires=%d\r\nname=%s\r\nhost=%s\r\n; code=%s\r\n; hostName=%s\r\n",
		jc.Name, hex.EncodeToString(jc.Key[:]), jc.Session, jc.Expires.Unix(), playerName(), strings.Join(jc.Addrs, ","), p2p.FindCode(text), jc.Name)
	if err := putFile(netIniPath(game), []byte(ini)); err != nil {
		app.status("Couldn't write the session: %v", err)
		return
	}
	saveManifest()
	app.logf("Joining %s at %s", jc.Name, strings.Join(jc.Addrs, ", "))
	app.status("Joining %s: press PLAY - %s lets you in.", jc.Name, jc.Name)
	netPills()
}

// ---- The clipboard (text)
var (
	pOpenClipboard    = user32.NewProc("OpenClipboard")
	pCloseClipboard   = user32.NewProc("CloseClipboard")
	pEmptyClipboard   = user32.NewProc("EmptyClipboard")
	pGetClipboardData = user32.NewProc("GetClipboardData")
	pSetClipboardData = user32.NewProc("SetClipboardData")
	pGlobalAlloc      = kernel32.NewProc("GlobalAlloc")
	pGlobalLock       = kernel32.NewProc("GlobalLock")
	pGlobalUnlock     = kernel32.NewProc("GlobalUnlock")
)

func setClipboard(s string) bool {
	if r, _, _ := pOpenClipboard.Call(hwndMain); r == 0 {
		return false
	}
	defer pCloseClipboard.Call()
	pEmptyClipboard.Call()
	u, _ := syscall.UTF16FromString(s)
	h, _, _ := pGlobalAlloc.Call(0x0002, uintptr(len(u)*2))
	if h == 0 {
		return false
	}
	p, _, _ := pGlobalLock.Call(h)
	copy((*[1 << 20]uint16)(unsafe.Pointer(p))[:len(u)], u)
	pGlobalUnlock.Call(h)
	r, _, _ := pSetClipboardData.Call(13, h)
	return r != 0
}

func getClipboard() string {
	if r, _, _ := pOpenClipboard.Call(hwndMain); r == 0 {
		return ""
	}
	defer pCloseClipboard.Call()
	h, _, _ := pGetClipboardData.Call(13)
	if h == 0 {
		return ""
	}
	p, _, _ := pGlobalLock.Call(h)
	if p == 0 {
		return ""
	}
	defer pGlobalUnlock.Call(h)
	return syscall.UTF16ToString((*[1 << 20]uint16)(unsafe.Pointer(p))[:])
}
