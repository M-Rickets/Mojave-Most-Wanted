//go:build windows

package p2p

import (
	"net"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// gateways: each working adapter's IPv4 default gateways (GetAdaptersAddresses with gateways)
func gateways() []string {
	size := uint32(16 << 10)
	var buf []byte
	for i := 0; i < 3; i++ {
		buf = make([]byte, size)
		err := windows.GetAdaptersAddresses(windows.AF_INET, windows.GAA_FLAG_INCLUDE_GATEWAYS|windows.GAA_FLAG_SKIP_ANYCAST|windows.GAA_FLAG_SKIP_MULTICAST|windows.GAA_FLAG_SKIP_DNS_SERVER, 0, (*windows.IpAdapterAddresses)(unsafe.Pointer(&buf[0])), &size)
		if err == nil {
			break
		}
		if err != windows.ERROR_BUFFER_OVERFLOW {
			return nil
		}
		buf = nil
	}
	if buf == nil {
		return nil
	}
	var out []string
	seen := map[string]bool{}
	for a := (*windows.IpAdapterAddresses)(unsafe.Pointer(&buf[0])); a != nil; a = a.Next {
		if a.OperStatus != windows.IfOperStatusUp {
			continue
		}
		for g := a.FirstGatewayAddress; g != nil; g = g.Next {
			if r := g.Address.Sockaddr; r != nil && r.Addr.Family == windows.AF_INET {
				s4 := (*syscall.RawSockaddrInet4)(unsafe.Pointer(r))
				ip := net.IP(s4.Addr[:]).String()
				if ip != "0.0.0.0" && !seen[ip] {
					seen[ip] = true
					out = append(out, ip)
				}
			}
		}
	}
	return out
}
