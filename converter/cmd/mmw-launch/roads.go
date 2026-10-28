package main

import (
	"log"
	"os"
	"path/filepath"
	"strings"
)

// logArchiveSettings notes whether loose files can replace the game's archived ones (needed for road textures).
func logArchiveSettings() {
	home, _ := os.UserHomeDir()
	for _, p := range []string{filepath.Join(home, "Documents", "My Games", "FalloutNV", "Fallout.ini"), filepath.Join(home, "OneDrive", "Documents", "My Games", "FalloutNV", "Fallout.ini")} {
		b, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		for _, l := range strings.Split(string(b), "\n") {
			ll := strings.ToLower(strings.TrimSpace(l))
			if strings.HasPrefix(ll, "binvalidateolderfiles") || strings.HasPrefix(ll, "sinvalidationfile") || strings.HasPrefix(ll, "sarchivelist") {
				log.Printf("Fallout.ini: %s", strings.TrimSpace(l))
			}
		}
		return
	}
	log.Print("Fallout.ini: not found in Documents\\My Games\\FalloutNV")
}
