//go:build !windows

package steam

import "os"

// registryRoots: developer runs off Windows can point MMW_STEAM at a folder laid out like a Steam library.
func registryRoots() []string { return []string{os.Getenv("MMW_STEAM")} }
