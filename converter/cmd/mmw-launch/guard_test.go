package main

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"mmw/internal/p2p"
)

// The panic key: a game that answers gets net.stop and nothing ends it; a frozen one is ended after 8 s; twice ends it
func TestPanic(t *testing.T) {
	dir := t.TempDir()
	write := func() {
		os.WriteFile(filepath.Join(dir, "net.ini"), []byte("mode=host\r\nport=41000\r\nupnp=0\r\nexpires="+itoa(time.Now().Add(time.Hour).Unix())+"\r\n"), 0o644)
	}
	ended, alive, freed := 0, true, 0
	var told string
	g := &guard{pluginDir: dir, router: func() *p2p.Router { return nil },
		freeMouse: func() { freed++ }, gameAlive: func() bool { return alive },
		endGame: func() error { ended++; alive = false; return nil }, tell: func(m string) { told = m }}
	write()
	go func() {
		for i := 0; i < 50; i++ {
			if b, err := os.ReadFile(filepath.Join(dir, "net.stop")); err == nil && string(b) == "panic\r\n" {
				os.Remove(filepath.Join(dir, "net.stop"))
				os.WriteFile(filepath.Join(dir, "net.stopped"), []byte("stopped"), 0o644)
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
	}()
	g.panic(false)
	if ended != 0 || freed != 1 {
		t.Fatalf("answering game: ended %d, freed %d (%s)", ended, freed, told)
	}
	if _, err := os.Stat(filepath.Join(dir, "net.ini")); err == nil {
		t.Error("the session wasn't ended")
	}
	write()
	t0 := time.Now()
	g.panic(false)
	if ended != 1 || time.Since(t0) < 7*time.Second {
		t.Fatalf("frozen game: ended %d after %v (%s)", ended, time.Since(t0), told)
	}
	alive = true
	g.panic(false)
	if ended != 1 {
		t.Fatal("ended without playing together")
	}
	g.panic(true)
	if ended != 2 {
		t.Fatalf("twice didn't end it (%s)", told)
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }
