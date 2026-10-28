//go:build !windows

package main

import "os"

// Tests off Windows: MMW_DOCUMENTS stands in for the known folder
func documentsDir() string { return os.Getenv("MMW_DOCUMENTS") }
