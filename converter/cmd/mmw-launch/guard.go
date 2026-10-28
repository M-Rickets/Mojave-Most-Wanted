package main

// The guard: the launcher starts a second copy of itself, --guard, when it starts the game.

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"

	"mmw/internal/p2p"
)

// netSession: setup's net.ini as the guard needs it
type netSession struct {
	Mode    string
	Port    int
	UPnP    bool
	Expires time.Time
}

func readSession(path string) netSession {
	var s netSession
	b, err := os.ReadFile(path)
	if err != nil {
		return s
	}
	for _, l := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		k, v, ok := strings.Cut(l, "=")
		if !ok || strings.HasPrefix(k, ";") {
			continue
		}
		switch k {
		case "mode":
			s.Mode = v
		case "port":
			fmt.Sscan(v, &s.Port)
		case "upnp":
			s.UPnP = v == "1"
		case "expires":
			var u int64
			fmt.Sscan(v, &u)
			s.Expires = time.Unix(u, 0)
		}
	}
	return s
}

type guard struct {
	pluginDir string
	router    func() *p2p.Router
	forwarded int
	freeMouse func()
	gameAlive func() bool
	endGame   func() error
	tell      func(msg string)
}

func (g *guard) netIni() string   { return filepath.Join(g.pluginDir, "net.ini") }
func (g *guard) stopFile() string { return filepath.Join(g.pluginDir, "net.stop") }
func (g *guard) ackFile() string  { return filepath.Join(g.pluginDir, "net.stopped") }

// forward: hosting with UPnP - the router's forward made again for 4 hours (at the start and every half hour)
func (g *guard) forward() {
	s := readSession(g.netIni())
	if s.Mode != "host" || !s.UPnP || s.Port <= 0 || (!s.Expires.IsZero() && time.Now().After(s.Expires)) {
		return
	}
	if r := g.router(); r != nil {
		if _, err := r.MapPort(s.Port, "Mojave Enhanced", 4*3600); err == nil {
			g.forwarded = s.Port
			log.Printf("guard: router forward of UDP %d renewed", s.Port)
		} else {
			log.Printf("guard: router forward of UDP %d not renewed (%v)", s.Port, err)
		}
	}
}

// closeForward: the router's forward taken down (the game closed, or the panic key) - the session's port, or the one
// this guard renewed; kept to try again when the router doesn't answer
func (g *guard) closeForward() {
	port := g.forwarded
	if s := readSession(g.netIni()); s.UPnP && s.Port > 0 {
		port = s.Port
	}
	if port <= 0 {
		return
	}
	r := g.router()
	if r == nil {
		log.Printf("guard: no router answered - the forward of UDP %d is tried again when the game closes", port)
		g.forwarded = port
		return
	}
	if err := r.UnmapPort(port); err == nil {
		log.Printf("guard: router forward of UDP %d taken down", port)
		g.forwarded = 0
	} else {
		log.Printf("guard: router forward of UDP %d not taken down (%v)", port, err)
		g.forwarded = port
	}
}

// panic: the panic key (twice: pressed again within 5 s of the last press - counted when pressed, not when the last one
// is done)
func (g *guard) panic(twice bool) {
	g.freeMouse()
	s := readSession(g.netIni())
	playing := s.Mode == "host" || s.Mode == "join"
	log.Printf("guard: PANIC KEY (playing together: %v%s)", playing, map[bool]string{true: ", pressed twice"}[twice])
	if twice && g.gameAlive() {
		err := g.endGame()
		log.Printf("guard: pressed twice - the game ended (%v)", err)
		g.tell("Panic key, twice: New Vegas was ended.")
		return
	}
	if !playing {
		g.tell("Panic key: mouse freed. Not playing together. Press Ctrl+Alt+End again within 5 s to end the game.")
		return
	}
	os.Remove(g.ackFile())
	os.WriteFile(g.stopFile(), []byte("panic\r\n"), 0o644)
	g.closeForward()
	os.Remove(g.netIni())
	for t := 0; t < 80; t++ {
		if _, err := os.Stat(g.ackFile()); err == nil {
			os.Remove(g.ackFile())
			log.Print("guard: the game dropped everyone and closed its port")
			g.tell("Panic key: playing together is off - everyone dropped, your router's port closed, the join code dead.")
			return
		}
		if !g.gameAlive() {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if g.gameAlive() {
		err := g.endGame()
		log.Printf("guard: the game didn't answer in 8 s (frozen) - ended it (%v)", err)
		g.tell("Panic key: New Vegas didn't answer (frozen), so it was ended. Your router's port is closed, the join code dead.")
		return
	}
	g.tell("Panic key: the game had already closed. Your router's port is closed, the join code dead.")
}
