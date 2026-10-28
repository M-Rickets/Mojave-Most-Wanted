//go:build windows

package main

import "golang.org/x/sys/windows"

// The Documents folder the game uses (it may be moved to another drive or into OneDrive)
func documentsDir() string {
	p, _ := windows.KnownFolderPath(windows.FOLDERID_Documents, 0)
	return p
}
