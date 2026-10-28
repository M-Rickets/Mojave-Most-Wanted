//go:build !windows

package main

func diskFree(path string) uint64 { return 1 << 50 }
