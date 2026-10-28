//go:build !windows

package main

import (
	"os"
	"strconv"
	"strings"
)

// Tests off Windows: MMW_SCREEN=WxH stands in for the monitor
func screenSize() (int, int) {
	w, h, _ := strings.Cut(os.Getenv("MMW_SCREEN"), "x")
	a, _ := strconv.Atoi(w)
	b, _ := strconv.Atoi(h)
	return a, b
}
