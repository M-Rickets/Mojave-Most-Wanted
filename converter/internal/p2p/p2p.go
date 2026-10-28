// Package p2p: playing together - the join code, the router's port (UPnP), the internet address (STUN) and this PC's
// addresses.
package p2p

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base32"
	"encoding/binary"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"regexp"
	"strings"
	"time"
)

// JoinCode: what a host sends friends.
type JoinCode struct {
	Key     [16]byte
	Session uint32
	Expires time.Time
	Name    string
	Addrs   []string
}

// Lifetime: how long a join code works after hosting (a new one with each HOST A GAME)
const Lifetime = 12 * time.Hour

var b32 = base32.NewEncoding("0123456789ABCDEFGHJKMNPQRSTVWXYZ").WithPadding(base32.NoPadding)

// cleanName: a name as a code carries it - printable ASCII (others "?"), at most 20 (it goes into net.ini lines)
func cleanName(n string) string {
	b := []byte(n)
	if len(b) > 20 {
		b = b[:20]
	}
	for i, c := range b {
		if c < 32 || c > 126 || c == '=' || c == ';' {
			b[i] = '?'
		}
	}
	return string(b)
}

func (j JoinCode) payload() []byte {
	var b bytes.Buffer
	b.Write(j.Key[:])
	binary.Write(&b, binary.BigEndian, j.Session)
	binary.Write(&b, binary.BigEndian, uint32(j.Expires.Unix()))
	name := cleanName(j.Name)
	b.WriteByte(byte(len(name)))
	b.WriteString(name)
	var as [][6]byte
	for _, a := range j.Addrs {
		host, port, err := net.SplitHostPort(a)
		ip := net.ParseIP(host).To4()
		var p int
		fmt.Sscan(port, &p)
		if err != nil || ip == nil || p <= 0 || p > 65535 || len(as) >= 6 {
			continue
		}
		var e [6]byte
		copy(e[:4], ip)
		binary.BigEndian.PutUint16(e[4:], uint16(p))
		as = append(as, e)
	}
	b.WriteByte(byte(len(as)))
	for _, e := range as {
		b.Write(e[:])
	}
	return b.Bytes()
}

func (j JoinCode) String() string {
	p := j.payload()
	sum := sha256.Sum256(p)
	return "MMW2-" + b32.EncodeToString(append(p, sum[:4]...))
}

var (
	reCode     = regexp.MustCompile(`MMW([0-9])-([0-9A-Z]{20,})`)
	ErrNone    = errors.New("no join code (MMW2-...) found - copy the whole code your friend sent")
	ErrOld     = errors.New("that's an old join code (MMW1): ask your friend to update setup and HOST A GAME again")
	ErrTypo    = errors.New("that join code is copied wrong (its check doesn't match) - copy it again, the whole thing")
	ErrExpired = errors.New("that join code has expired - ask your friend to HOST A GAME again for a new one")
)

// FindCode: the join code in text (a pasted chat message), upper case, with the letters people misread put right
func FindCode(text string) string {
	up := strings.Map(func(r rune) rune {
		switch r {
		case 'O':
			return '0'
		case 'I', 'L':
			return '1'
		}
		return r
	}, strings.ToUpper(text))
	return reCode.FindString(up)
}

// ParseJoinCode finds a join code anywhere in text and checks it: copied right, not expired (at now)
func ParseJoinCode(text string, now time.Time) (JoinCode, error) {
	var j JoinCode
	m := reCode.FindStringSubmatch(FindCode(text))
	if m == nil {
		return j, ErrNone
	}
	if m[1] != "2" {
		return j, ErrOld
	}
	b, err := b32.DecodeString(m[2])
	if err != nil || len(b) < 16+4+4+1+1+4 {
		return j, ErrTypo
	}
	p, sum := b[:len(b)-4], b[len(b)-4:]
	if s := sha256.Sum256(p); !bytes.Equal(s[:4], sum) {
		return j, ErrTypo
	}
	copy(j.Key[:], p[:16])
	j.Session = binary.BigEndian.Uint32(p[16:])
	j.Expires = time.Unix(int64(binary.BigEndian.Uint32(p[20:])), 0)
	n := int(p[24])
	if len(p) < 25+n+1 {
		return j, ErrTypo
	}
	j.Name = string(p[25 : 25+n])
	if n > 20 || cleanName(j.Name) != j.Name {
		return j, ErrTypo
	}
	k := int(p[25+n])
	a := p[26+n:]
	if len(a) != 6*k {
		return j, ErrTypo
	}
	for i := 0; i < k; i++ {
		e := a[6*i:]
		j.Addrs = append(j.Addrs, fmt.Sprintf("%d.%d.%d.%d:%d", e[0], e[1], e[2], e[3], binary.BigEndian.Uint16(e[4:6])))
	}
	if now.After(j.Expires) {
		return j, ErrExpired
	}
	return j, nil
}

// NewSession: a new key and session id
func NewSession() (key [16]byte, session uint32) {
	rand.Read(key[:])
	var s [4]byte
	rand.Read(s[:])
	session = binary.BigEndian.Uint32(s[:])
	if session == 0 {
		session = 1
	}
	return
}

// RandomPort: a UDP port for this session (20000-59999: not a fixed one anyone could scan the internet for), free on
// this PC
func RandomPort() int {
	for i := 0; i < 50; i++ {
		n, _ := rand.Int(rand.Reader, big.NewInt(40000))
		p := 20000 + int(n.Int64())
		if c, err := net.ListenUDP("udp4", &net.UDPAddr{Port: p}); err == nil {
			c.Close()
			return p
		}
	}
	return 27050
}

// LocalIPv4s: this PC's addresses friends on the same network or VPN can reach (LAN, Tailscale, ZeroTier, Radmin)
func LocalIPv4s() []string {
	var out []string
	ifs, _ := net.Interfaces()
	for _, i := range ifs {
		if i.Flags&net.FlagUp == 0 || i.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, _ := i.Addrs()
		for _, a := range addrs {
			if n, ok := a.(*net.IPNet); ok {
				if ip := n.IP.To4(); ip != nil && !ip.IsLinkLocalUnicast() {
					out = append(out, ip.String())
				}
			}
		}
	}
	return out
}

// ---- UPnP: ask the router to send a UDP port to this PC (most home routers allow it), and its internet address
type Router struct {
	control, typ string
	Local        string
}

func FindRouter(timeout time.Duration) (*Router, error) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{})
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	dst := &net.UDPAddr{IP: net.IPv4(239, 255, 255, 250), Port: 1900}
	for _, st := range []string{"urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANPPPConnection:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:1"} {
		msg := "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nST: " + st + "\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n"
		conn.WriteTo([]byte(msg), dst)
	}
	conn.SetReadDeadline(time.Now().Add(timeout))
	buf := make([]byte, 2048)
	seen := map[string]bool{}
	for {
		n, from, err := conn.ReadFrom(buf)
		if err != nil {
			return nil, errors.New("no router answered (UPnP off, or not a home router)")
		}
		loc := ""
		for _, l := range strings.Split(string(buf[:n]), "\r\n") {
			if k, v, ok := strings.Cut(l, ":"); ok && strings.EqualFold(strings.TrimSpace(k), "location") {
				loc = strings.TrimSpace(v)
			}
		}
		if loc == "" || seen[loc] {
			continue
		}
		seen[loc] = true
		if s, err := describe(loc); err == nil {
			if c, err := net.Dial("udp4", from.(*net.UDPAddr).IP.String()+":1900"); err == nil {
				s.Local = c.LocalAddr().(*net.UDPAddr).IP.String()
				c.Close()
			}
			return s, nil
		}
	}
}

func describe(loc string) (*Router, error) {
	c := &http.Client{Timeout: 4 * time.Second}
	resp, err := c.Get(loc)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	type svc struct {
		Type    string `xml:"serviceType"`
		Control string `xml:"controlURL"`
	}
	type dev struct {
		Services []svc `xml:"serviceList>service"`
		Devices  []dev `xml:"deviceList>device"`
	}
	var root struct {
		URLBase string `xml:"URLBase"`
		Device  dev    `xml:"device"`
	}
	if err := xml.Unmarshal(b, &root); err != nil {
		return nil, err
	}
	var walk func(d dev) *svc
	walk = func(d dev) *svc {
		for i := range d.Services {
			t := d.Services[i].Type
			if strings.Contains(t, "WANIPConnection") || strings.Contains(t, "WANPPPConnection") {
				return &d.Services[i]
			}
		}
		for _, sd := range d.Devices {
			if s := walk(sd); s != nil {
				return s
			}
		}
		return nil
	}
	s := walk(root.Device)
	if s == nil {
		return nil, errors.New("the router has no WAN connection service")
	}
	base := root.URLBase
	if base == "" {
		if i := strings.Index(loc[8:], "/"); i >= 0 {
			base = loc[:8+i]
		} else {
			base = loc
		}
	}
	ctl := s.Control
	if !strings.HasPrefix(ctl, "http") {
		ctl = strings.TrimRight(base, "/") + "/" + strings.TrimLeft(ctl, "/")
	}
	return &Router{control: ctl, typ: s.Type}, nil
}

func (u *Router) soap(action, args string) (string, error) {
	body := `<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><u:` +
		action + ` xmlns:u="` + u.typ + `">` + args + `</u:` + action + `></s:Body></s:Envelope>`
	req, _ := http.NewRequest("POST", u.control, strings.NewReader(body))
	req.Header.Set("Content-Type", `text/xml; charset="utf-8"`)
	req.Header.Set("SOAPAction", `"`+u.typ+"#"+action+`"`)
	resp, err := (&http.Client{Timeout: 5 * time.Second}).Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != 200 {
		return string(b), fmt.Errorf("router said %s", resp.Status)
	}
	return string(b), nil
}

// MapPort asks the router to send UDP port to this PC for lease seconds (it falls back to no expiry for routers that
// only take that - Permanent says so: the guard then takes it down when the game closes)
func (u *Router) MapPort(port int, desc string, lease int) (permanent bool, err error) {
	for _, l := range []int{lease, 0} {
		var body string
		body, err = u.soap("AddPortMapping", fmt.Sprintf("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>", port, port, u.Local, desc, l))
		if err == nil {
			return l == 0, nil
		}
		if !strings.Contains(body, "<errorCode>725</errorCode>") {
			return false, err
		}
	}
	return false, err
}

// UnmapPort takes the router's forward of UDP port down
func (u *Router) UnmapPort(port int) error {
	_, err := u.soap("DeletePortMapping", fmt.Sprintf("<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>UDP</NewProtocol>", port))
	return err
}

// ExternalIP: the address the internet sees the router at ("" unknown)
func (u *Router) ExternalIP() string {
	r, err := u.soap("GetExternalIPAddress", "")
	if err != nil {
		return ""
	}
	m := regexp.MustCompile(`<NewExternalIPAddress>([0-9.]+)</NewExternalIPAddress>`).FindStringSubmatch(r)
	if m == nil {
		return ""
	}
	return m[1]
}

// StunAddr: this PC's internet address as a public STUN server sees it
func StunAddr(server string, timeout time.Duration) (string, error) {
	c, err := net.DialTimeout("udp4", server, timeout)
	if err != nil {
		return "", err
	}
	defer c.Close()
	req := make([]byte, 20)
	binary.BigEndian.PutUint16(req[0:], 0x0001)
	binary.BigEndian.PutUint32(req[4:], 0x2112A442)
	rand.Read(req[8:20])
	c.SetDeadline(time.Now().Add(timeout))
	if _, err := c.Write(req); err != nil {
		return "", err
	}
	buf := make([]byte, 512)
	n, err := c.Read(buf)
	if err != nil || n < 20 {
		return "", errors.New("no answer")
	}
	if !bytes.Equal(buf[8:20], req[8:20]) {
		return "", errors.New("an answer to another request")
	}
	for p := 20; p+4 <= n; {
		t, l := binary.BigEndian.Uint16(buf[p:]), int(binary.BigEndian.Uint16(buf[p+2:]))
		v := buf[p+4 : min(n, p+4+l)]
		if (t == 0x0020 || t == 0x0001) && len(v) >= 8 && v[1] == 1 {
			ip := net.IP(append([]byte{}, v[4:8]...))
			if t == 0x0020 {
				for i := range ip {
					ip[i] ^= req[4+i]
				}
			}
			return ip.String(), nil
		}
		p += 4 + (l+3)/4*4
	}
	return "", errors.New("no address in the answer")
}
