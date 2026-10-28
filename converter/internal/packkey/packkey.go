// Package packkey names the folder an archive is unpacked to once (mmw-pack's packs\.unpacked\<key>), so setup and
// mmw-pack agree on it: the archive's path (without case), size and date - a new download gets a new folder.
package packkey

import (
	"crypto/sha1"
	"encoding/hex"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Root: where the unpacked copies live, for the game folder game
func Root(game string) string { return filepath.Join(game, "MojaveMostWanted", "packs", ".unpacked") }

// Dir: the folder src is (or would be) unpacked to, and its absolute path
func Dir(game, src string) (dir, abs string, err error) {
	st, err := os.Stat(src)
	if err != nil {
		return "", "", err
	}
	abs, err = filepath.Abs(src)
	if err != nil {
		return "", "", err
	}
	h := sha1.Sum([]byte(strings.ToLower(abs) + "|" + strconv.FormatInt(st.Size(), 10) + "|" + strconv.FormatInt(st.ModTime().UnixNano(), 10)))
	return filepath.Join(Root(game), hex.EncodeToString(h[:8])), abs, nil
}

// Ready: src's unpacked files folder when its unpack is complete ("" otherwise)
func Ready(game, src string) string {
	dir, _, err := Dir(game, src)
	if err != nil {
		return ""
	}
	if _, err := os.Stat(filepath.Join(dir, "complete")); err != nil {
		return ""
	}
	return filepath.Join(dir, "files")
}

// IsArchive: a.7z or.zip pack source
func IsArchive(src string) bool {
	e := strings.ToLower(filepath.Ext(src))
	return e == ".7z" || e == ".zip"
}
