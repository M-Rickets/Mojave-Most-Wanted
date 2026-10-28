package p2p

import (
	"crypto/sha256"
	"strings"
	"testing"
	"time"
)

func TestJoinCode(t *testing.T) {
	now := time.Unix(1792280000, 0)
	key, sess := NewSession()
	j := JoinCode{Key: key, Session: sess, Expires: now.Add(Lifetime), Name: "Courier", Addrs: []string{"203.0.113.7:41234", "192.168.1.20:41234", "100.64.1.2:41234"}}
	s := j.String()
	t.Logf("join code (%d chars): %s", len(s), s)
	k, err := ParseJoinCode("hey join me: "+strings.ToLower(s)+" see you", now)
	if err != nil || k.Name != j.Name || k.Key != j.Key || k.Session != j.Session || !k.Expires.Equal(j.Expires) || strings.Join(k.Addrs, ",") != strings.Join(j.Addrs, ",") {
		t.Fatalf("%s -> %+v %v", s, k, err)
	}
	mis := strings.NewReplacer("0", "O", "1", "l").Replace(s[5:])
	if _, err := ParseJoinCode("MMW2-"+mis, now); err != nil {
		t.Errorf("misread letters: %v", err)
	}
	alpha := "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
	body := s[5:]
	bad := 0
	for i := 0; i < len(body); i++ {
		for _, c := range alpha {
			if byte(c) == body[i] {
				continue
			}
			v := body[:i] + string(c) + body[i+1:]
			if k2, err := ParseJoinCode("MMW2-"+v, now); err != ErrTypo {
				if err == nil && k2.Key == j.Key && k2.Session == j.Session && strings.Join(k2.Addrs, ",") == strings.Join(j.Addrs, ",") {
					continue
				}
				t.Logf("slip at %d (%c) -> %v", i, c, err)
				bad++
			}
		}
		if _, err := ParseJoinCode("MMW2-"+body[:i]+body[i+1:], now); err != ErrTypo && err != ErrNone {
			bad++
		}
		if i+1 < len(body) && body[i] != body[i+1] {
			v := body[:i] + string(body[i+1]) + string(body[i]) + body[i+2:]
			if _, err := ParseJoinCode("MMW2-"+v, now); err != ErrTypo {
				bad++
			}
		}
	}
	if bad > 0 {
		t.Errorf("%d slips not caught", bad)
	}
	if _, err := ParseJoinCode(s, now.Add(Lifetime+time.Minute)); err != ErrExpired {
		t.Errorf("expired: %v", err)
	}
	if _, err := ParseJoinCode("MMW1-0123456789ABCDEFGHJKMNPQ", now); err != ErrOld {
		t.Errorf("old: %v", err)
	}
	if _, err := ParseJoinCode("nothing here", now); err != ErrNone {
		t.Errorf("none: %v", err)
	}
	if p := RandomPort(); p < 20000 || p > 59999 {
		t.Errorf("port %d", p)
	}
}

// A code made by hand with lines in its name (they'd go into net.ini) is refused; a setup's own code never has them
func TestJoinCodeName(t *testing.T) {
	now := time.Unix(1792280000, 0)
	j := JoinCode{Session: 5, Expires: now.Add(time.Hour), Name: "Bob\r\nmode=host\r\nport=3389", Addrs: []string{"203.0.113.7:41234"}}
	s := j.String()
	k, err := ParseJoinCode(s, now)
	if err != nil || strings.ContainsAny(k.Name, "\r\n=;") || len(k.Name) > 20 {
		t.Fatalf("own code: %q %v", k.Name, err)
	}
	j.Name = "x"
	p := j.payload()
	evil := "Bob\r\nmode=host\r\nupnp=1"
	p = append(append(append(append([]byte{}, p[:24]...), byte(len(evil))), evil...), p[26:]...)
	sum := sha256.Sum256(p)
	if _, err := ParseJoinCode("MMW2-"+b32.EncodeToString(append(p, sum[:4]...)), now); err != ErrTypo {
		t.Errorf("a name with lines: %v", err)
	}
}
