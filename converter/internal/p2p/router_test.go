package p2p

import (
	"encoding/binary"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// A router that answers the search only when asked straight (the multicast one never reaches it), then its IGD
// description and the SOAP calls
func TestFindRouterDirect(t *testing.T) {
	var mapped string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == "GET" {
			fmt.Fprint(w, `<root><device><deviceList><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl</controlURL></service></serviceList></device></deviceList></device></root>`)
			return
		}
		b := make([]byte, 4096)
		n, _ := r.Body.Read(b)
		if strings.Contains(r.Header.Get("SOAPAction"), "AddPortMapping") {
			mapped = string(b[:n])
		}
		fmt.Fprint(w, `<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>`)
	}))
	defer srv.Close()
	ss, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Skip(err)
	}
	defer ss.Close()
	go func() {
		b := make([]byte, 2048)
		for {
			n, a, err := ss.ReadFromUDP(b)
			if err != nil {
				return
			}
			if strings.HasPrefix(string(b[:n]), "M-SEARCH") {
				ss.WriteToUDP([]byte("HTTP/1.1 200 OK\r\nLOCATION: "+srv.URL+"/desc.xml\r\n\r\n"), a)
			}
		}
	}()
	ssdpPort = ss.LocalAddr().(*net.UDPAddr).Port
	defer func() { ssdpPort = 1900 }()
	was := Gateways
	Gateways = func() []string { return []string{"127.0.0.1"} }
	defer func() { Gateways = was }()
	r, err := FindRouter(2 * time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if r.Via != "UPnP" || r.Local != "127.0.0.1" {
		t.Fatalf("found %+v", r)
	}
	if _, err := r.MapPort(4321, "x", 60); err != nil || !strings.Contains(mapped, "<NewExternalPort>4321</NewExternalPort>") {
		t.Fatalf("map: %v %q", err, mapped)
	}
	if ip := r.ExternalIP(); ip != "203.0.113.7" {
		t.Fatalf("external %q", ip)
	}
}

// A router with UPnP off that speaks NAT-PMP, and gives another outside port than asked
func TestFindRouterPMP(t *testing.T) {
	pc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Skip(err)
	}
	defer pc.Close()
	go func() {
		b := make([]byte, 64)
		for {
			n, a, err := pc.ReadFromUDP(b)
			if err != nil {
				return
			}
			if n == 2 && b[1] == 0 {
				pc.WriteToUDP([]byte{0, 128, 0, 0, 0, 0, 0, 1, 198, 51, 100, 9}, a)
			} else if n == 12 && b[1] == 1 {
				o := make([]byte, 16)
				o[1] = 129
				copy(o[8:10], b[4:6])
				binary.BigEndian.PutUint16(o[10:], binary.BigEndian.Uint16(b[6:])+1000)
				copy(o[12:16], b[8:12])
				pc.WriteToUDP(o, a)
			}
		}
	}()
	pmpPort = pc.LocalAddr().(*net.UDPAddr).Port
	ssdpPort = 9
	defer func() { pmpPort, ssdpPort = 5351, 1900 }()
	was := Gateways
	Gateways = func() []string { return []string{"127.0.0.1"} }
	defer func() { Gateways = was }()
	r, err := FindRouter(800 * time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	if r.Via != "NAT-PMP" {
		t.Fatalf("found %+v", r)
	}
	if _, err := r.MapPort(4321, "x", 60); err != nil || r.ExternalPort(4321) != 5321 {
		t.Fatalf("map: %v, outside port %d", err, r.ExternalPort(4321))
	}
	if ip := r.ExternalIP(); ip != "198.51.100.9" {
		t.Fatalf("external %q", ip)
	}
	if r.UnmapPort(4321) != nil {
		t.Fatal("unmap")
	}
}

func TestNothingAnswers(t *testing.T) {
	ssdpPort, pmpPort = 9, 9
	defer func() { pmpPort, ssdpPort = 5351, 1900 }()
	was := Gateways
	Gateways = func() []string { return []string{"127.0.0.1"} }
	defer func() { Gateways = was }()
	if _, err := FindRouter(500 * time.Millisecond); err == nil || !strings.Contains(err.Error(), "127.0.0.1") {
		t.Fatalf("got %v", err)
	}
}

func TestShared(t *testing.T) {
	for ip, want := range map[string]bool{"100.72.1.2": true, "192.168.0.5": true, "10.1.1.1": true, "24.121.215.201": false, "100.128.0.1": false, "": false} {
		if Shared(ip) != want {
			t.Errorf("%s: want %v", ip, want)
		}
	}
}
