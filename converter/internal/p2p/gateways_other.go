//go:build !windows

package p2p

import (
	"bufio"
	"encoding/binary"
	"encoding/hex"
	"net"
	"os"
	"strings"
)

// gateways: the default routes' gateways from /proc/net/route (Linux; elsewhere none)
func gateways() []string {
	f, err := os.Open("/proc/net/route")
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []string
	s := bufio.NewScanner(f)
	for s.Scan() {
		c := strings.Fields(s.Text())
		if len(c) < 3 || c[1] != "00000000" {
			continue
		}
		b, err := hex.DecodeString(c[2])
		if err != nil || len(b) != 4 {
			continue
		}
		ip := make(net.IP, 4)
		binary.LittleEndian.PutUint32(ip, binary.BigEndian.Uint32(b))
		if !ip.Equal(net.IPv4zero) {
			out = append(out, ip.String())
		}
	}
	return out
}
