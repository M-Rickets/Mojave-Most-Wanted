package plugins

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func plugin(t *testing.T, dir, name string, masters ...string) {
	var sub []byte
	for _, m := range masters {
		d := append([]byte(m), 0)
		h := make([]byte, 6)
		copy(h, "MAST")
		binary.LittleEndian.PutUint16(h[4:], uint16(len(d)))
		sub = append(append(sub, h...), d...)
	}
	hdr := make([]byte, 24)
	copy(hdr, "TES4")
	binary.LittleEndian.PutUint32(hdr[4:], uint32(len(sub)))
	if err := os.WriteFile(filepath.Join(dir, name), append(hdr, sub...), 0o644); err != nil {
		t.Fatal(err)
	}
}

// FNV takes TTW, the Fallout 3 masters it's made of and everything needing them out; TTW puts them back where they were
func TestTTWSetAndRestore(t *testing.T) {
	d := t.TempDir()
	official := []string{"FalloutNV.esm", "DeadMoney.esm"}
	plugin(t, d, "FalloutNV.esm")
	plugin(t, d, "DeadMoney.esm", "FalloutNV.esm")
	plugin(t, d, "Fallout3.esm", "FalloutNV.esm")
	plugin(t, d, TTW, "FalloutNV.esm", "DeadMoney.esm", "Fallout3.esm")
	plugin(t, d, "YUPTTW.esm", TTW)
	plugin(t, d, "Patch.esp", "YUPTTW.esm")
	plugin(t, d, "Plain.esp", "FalloutNV.esm")
	plugin(t, d, "MojaveMostWanted.esp", "FalloutNV.esm")
	list := []string{"FalloutNV.esm", "DeadMoney.esm", "Fallout3.esm", TTW, "YUPTTW.esm", "Plain.esp", "Patch.esp", "MojaveMostWanted.esp"}
	find := func(n string) string { return filepath.Join(d, n) }
	set := TTWSet(list, find, official)
	if want := []string{"Fallout3.esm", TTW, "YUPTTW.esm", "Patch.esp"}; !reflect.DeepEqual(set, want) {
		t.Fatalf("TTW's plugins: %v, want %v", set, want)
	}
	fnv := Without(list, set)
	if want := []string{"FalloutNV.esm", "DeadMoney.esm", "Plain.esp", "MojaveMostWanted.esp"}; !reflect.DeepEqual(fnv, want) {
		t.Fatalf("FNV: %v", fnv)
	}
	back := Restore(append(fnv, "New.esp"), list, func(string) bool { return true })
	if want := append(append([]string{}, list[:6]...), "Patch.esp", "MojaveMostWanted.esp", "New.esp"); !reflect.DeepEqual(back, want) {
		t.Fatalf("TTW again: %v, want %v", back, want)
	}
}
