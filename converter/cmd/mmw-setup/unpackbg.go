//go:build windows

package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"

	"mmw/internal/packkey"
)

// Archives unpacked in the background: while setup is open, every local add-on's.7z /.zip that hasn't been unpacked yet
// is unpacked by mmw-pack at low priority, so even a pack's first switch-on only links files.

var pGetDiskFreeSpaceExW = kernel32.NewProc("GetDiskFreeSpaceExW")

func diskFree(path string) uint64 {
	var free uint64
	p, _ := syscall.UTF16PtrFromString(path)
	if r, _, _ := pGetDiskFreeSpaceExW.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&free)), 0, 0); r == 0 {
		return 0
	}
	return free
}

// archiveSrcs: an add-on's archive sources, when its switch-on is mmw-pack's
func archiveSrcs(a addon, game string) []string {
	if len(a.On) < 2 || !strings.EqualFold(filepath.Base(addonExpand(a.On[0], game)), "mmw-pack.exe") || a.On[1] != "on" {
		return nil
	}
	var out []string
	for i, s := range a.On {
		src := ""
		switch {
		case (s == "--src" || s == "-src") && i+1 < len(a.On):
			src = a.On[i+1]
		case strings.HasPrefix(s, "--src="):
			src = strings.TrimPrefix(s, "--src=")
		}
		if src = addonExpand(src, game); src != "" && packkey.IsArchive(src) {
			out = append(out, src)
		}
	}
	return out
}

var unpackBGStarted bool

func unpackInBackground(game string) {
	if unpackBGStarted || game == "" {
		return
	}
	unpackBGStarted = true
	pack := filepath.Join(mmwDir(game), "mmw-pack.exe")
	if !exists(pack) {
		return
	}
	go func() {
		const reserve = 30 << 30
		done := 0
		for _, a := range loadAddons() {
			var todo []string
			var need uint64
			for _, src := range archiveSrcs(a, game) {
				if packkey.Ready(game, src) != "" {
					continue
				}
				if st, err := os.Stat(src); err == nil {
					todo, need = append(todo, src), need+3*uint64(st.Size())
				}
			}
			if len(todo) == 0 {
				continue
			}
			if free := diskFree(game); free < need+reserve {
				app.logf("Background unpack: %s not unpacked ahead (%.1f GB free on the game's drive, it needs about %.1f GB and 30 GB stay free)", a.Name, float64(free)/(1<<30), float64(need)/(1<<30))
				continue
			}
			args := []string{"unpack", "--game", game}
			for _, s := range todo {
				args = append(args, "--src", s)
			}
			c := hiddenCmd(pack, args...)
			c.SysProcAttr.CreationFlags |= 0x00004000
			if err := c.Run(); err != nil {
				app.logf("Background unpack of %s: %v", a.Name, err)
				continue
			}
			done++
			app.logf("Background unpack: %s ready (its next switch-on only links files)", a.Name)
		}
		if done > 0 {
			app.logf("Background unpack: %d add-on(s) unpacked ahead", done)
		}
	}()
}
