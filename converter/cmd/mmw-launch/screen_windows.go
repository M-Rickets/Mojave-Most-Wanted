//go:build windows

package main

import (
	"syscall"
	"unsafe"
)

// screenSize: the primary monitor's current mode in real pixels (EnumDisplaySettingsW - not scaled by Windows' DPI)
func screenSize() (int, int) {
	var dm [220]byte
	*(*uint16)(unsafe.Pointer(&dm[68])) = 220
	r, _, _ := syscall.NewLazyDLL("user32.dll").NewProc("EnumDisplaySettingsW").Call(0, uintptr(0xFFFFFFFF), uintptr(unsafe.Pointer(&dm[0])))
	if r == 0 {
		return 0, 0
	}
	return int(*(*uint32)(unsafe.Pointer(&dm[172]))), int(*(*uint32)(unsafe.Pointer(&dm[176])))
}
