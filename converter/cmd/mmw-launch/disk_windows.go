//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

var pDiskFree = syscall.NewLazyDLL("kernel32.dll").NewProc("GetDiskFreeSpaceExW")

// diskFree: the bytes free to us on path's drive (0 when it can't be told)
func diskFree(path string) uint64 {
	var free uint64
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0
	}
	if r, _, _ := pDiskFree.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&free)), 0, 0); r == 0 {
		return 0
	}
	return free
}
