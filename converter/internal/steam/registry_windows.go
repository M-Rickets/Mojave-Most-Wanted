package steam

import (
	"syscall"
	"unsafe"
)

var (
	advapi32         = syscall.NewLazyDLL("advapi32.dll")
	procRegOpenKeyEx = advapi32.NewProc("RegOpenKeyExW")
	procRegQueryEx   = advapi32.NewProc("RegQueryValueExW")
	procRegClose     = advapi32.NewProc("RegCloseKey")
)

const (
	hkeyCurrentUser  = 0x80000001
	hkeyLocalMachine = 0x80000002
	keyRead          = 0x20019
	keyWow64_32      = 0x0200
)

func regString(root uintptr, path, name string, flags uint32) string {
	var h syscall.Handle
	p, _ := syscall.UTF16PtrFromString(path)
	if r, _, _ := procRegOpenKeyEx.Call(root, uintptr(unsafe.Pointer(p)), 0, uintptr(keyRead|flags), uintptr(unsafe.Pointer(&h))); r != 0 {
		return ""
	}
	defer procRegClose.Call(uintptr(h))
	n, _ := syscall.UTF16PtrFromString(name)
	buf := make([]uint16, 1024)
	size := uint32(len(buf) * 2)
	var typ uint32
	if r, _, _ := procRegQueryEx.Call(uintptr(h), uintptr(unsafe.Pointer(n)), 0, uintptr(unsafe.Pointer(&typ)), uintptr(unsafe.Pointer(&buf[0])), uintptr(unsafe.Pointer(&size))); r != 0 {
		return ""
	}
	return syscall.UTF16ToString(buf)
}

func registryRoots() []string {
	return []string{
		regString(hkeyCurrentUser, `Software\Valve\Steam`, "SteamPath", 0),
		regString(hkeyLocalMachine, `SOFTWARE\Valve\Steam`, "InstallPath", keyWow64_32),
		regString(hkeyLocalMachine, `SOFTWARE\WOW6432Node\Valve\Steam`, "InstallPath", 0),
	}
}
