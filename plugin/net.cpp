#include "net.h"

// Winsock (ws2_32), declared here: the plugin has no SDK headers
extern "C" {
struct sockaddr_in4 { u16 family; u16 port; u32 addr; u8 zero[8]; };
__declspec(dllimport) int __stdcall WSAStartup(u16, void *);
__declspec(dllimport) u32 __stdcall socket(int, int, int);
__declspec(dllimport) int __stdcall bind(u32, const void *, int);
__declspec(dllimport) int __stdcall sendto(u32, const char *, int, int, const void *, int);
__declspec(dllimport) int __stdcall recvfrom(u32, char *, int, int, void *, int *);
__declspec(dllimport) int __stdcall closesocket(u32);
__declspec(dllimport) int __stdcall ioctlsocket(u32, i32, u32 *);
__declspec(dllimport) int __stdcall WSAGetLastError();
__declspec(dllimport) void __stdcall GetSystemTimeAsFileTime(u64 *);
}

namespace net
{
void (*logFn)(const char *) = 0;
#define NLOG(...) do { if (logFn) { char _b[400]; rt::fmt(_b, sizeof _b, __VA_ARGS__); logFn(_b); } } while (0)

static const u32 kMagic = 0x31574D4D;
static const u32 kProto = 9;
enum { HELLO = 1, WELCOME, STATE, PING, PONG, BYE, FULL, ROSTER, LOOT, SKEL, EVENTS, ENEMY, LOOK, WAIT, REFUSED, CLOCK, MODS, FILEOFFER, FILEPART, FILEACK, EVENTS2, EVACK };

#pragma pack(push, 1)
struct Hdr { u32 magic, proto; u8 type, from; u16 pad; u32 seq, session, t; };
struct StatePk
{
	float pos[3], yaw, pitch, roll; u32 world, cell; u8 mode, flags; i16 car; float speed, hour; u32 weather; u8 snow;
	i16 parkCar; float parkPos[3], parkYaw, parkPitch, parkRoll; u32 parkWorld, parkCell;
	u8 ride, invite; FormKey weapon; u16 shots;
	u8 health, status, downLeft, rules;
	u8 level; u16 maxHealth; u16 dmgFor[4];
	i8 wheelOff[4]; i8 steer, wheelie, shakeZ, shakePitch, shakeRoll; i16 gunQ[4];
};
enum { kMaxPoseBytes = 1 + kPoseBones * 7 + 8 * 6 };
struct EnemyPk { u8 slot, count; u32 low, modHash; float pos[3], yaw, health; u8 flags; u32 wlow, wmod; u16 shots; };
struct LootHdr { u8 nDrop, nTaken; };
struct DropPk { u32 id; u32 low, modHash; u16 count; float pos[3]; u32 world, cell; };
struct TakenPk { u8 owner; u32 id; };
struct HelloPk { char name[24]; u32 build; u32 stamp, nonce; };
struct CtlPk { u32 nonce; i32 skew; };
struct WelcomePk { u8 id; char hostName[24]; u32 hostBuild; u32 nonce; };
struct PingPk { u32 sentLocal; u32 hostT; };
struct RosterPk { u8 n; struct { u8 id, epoch; char name[24]; } who[3]; };
#pragma pack(pop)
enum { kMaxBody = 1024 - (int)sizeof(Hdr) - 8 };

static u32 s_build;
static u32 s_sock = ~0u;
static int s_mode;
static u16 s_port = 27050;
static u8 s_key[16];
static char s_name[24] = "player";
static u8 s_myId;
static bool s_joined;
static u32 s_seq;
static u32 s_nonce;
static u8 s_epoch[3], s_epochSeen[3];
static double s_base = -1;
static u32 s_session;
static u32 s_expires;
static double s_refusedUntil;
static double s_clockOff;
static bool s_haveClock;
static float s_hostHour = -1;
static u32 s_hostWeather;
static u8 s_hostSnow;
static bool s_haveSky;
static char s_status[160] = "off";
static bool s_solo;

struct Addr { u32 ip; u16 port; };
static Addr s_hostAddrs[8];
static int s_nHost;
static Addr s_host;

static inline u64 rotl(u64 x, int b) { return (x << b) | (x >> (64 - b)); }
static u64 siphash(const u8 *k, const u8 *m, u32 n)
{
	u64 k0 = 0, k1 = 0;
	for (int i = 0; i < 8; i++) { k0 |= (u64)k[i] << (8 * i); k1 |= (u64)k[8 + i] << (8 * i); }
	u64 v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1, v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
	auto round = [&]() {
		v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
		v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
		v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
		v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
	};
	u32 end = n - n % 8;
	for (u32 i = 0; i < end; i += 8)
	{
		u64 w = 0;
		for (int j = 0; j < 8; j++) w |= (u64)m[i + j] << (8 * j);
		v3 ^= w; round(); round(); v0 ^= w;
	}
	u64 b = (u64)n << 56;
	for (u32 j = 0; j < n % 8; j++) b |= (u64)m[end + j] << (8 * j);
	v3 ^= b; round(); round(); v0 ^= b;
	v2 ^= 0xff;
	round(); round(); round(); round();
	return v0 ^ v1 ^ v2 ^ v3;
}

struct Snap { double t; StatePk s; u16 poseLen; u8 pose[kMaxPoseBytes]; };
struct ESnap { double t; EnemyPk e; u16 poseLen; u8 pose[kMaxPoseBytes]; };
struct EnemySlot { bool used; double heard; ESnap snap[8]; int n; bool haveSkel; Skel skel; };
struct Peer
{
	bool used;
	u8 id;
	Addr addr;
	char name[24];
	u32 build;
	Snap snap[32];
	int n;
	double heard;
	bool haveLoot;
	Drop drops[kMaxDrops]; int nDrops;
	Taken taken[kMaxTaken]; int nTaken;
	bool haveSkel; Skel skel;
	EnemySlot enemy[kMaxEnemies];
	u32 evEpoch, evAckMine; bool evAcks;
	u16 lookLen; u8 look[kMaxLook];
	u16 nMods, modsTotal; u32 mods[kMaxMods]; u32 modsHeard;
};
struct Window { bool init; u32 top; u64 bits; };
static Window s_win[4];
static bool fresh(Window &w, u32 seq)
{
	if (!w.init) { w.init = true; w.top = seq; w.bits = 1; return true; }
	if (seq > w.top)
	{
		u32 d = seq - w.top;
		w.bits = d >= 64 ? 1 : (w.bits << d) | 1;
		w.top = seq;
		return true;
	}
	u32 d = w.top - seq;
	if (d >= 64 || (w.bits >> d & 1)) return false;
	w.bits |= (u64)1 << d;
	return true;
}
struct Asking { bool used; Addr addr; char name[24]; u32 build, nonce; double first, last, named; };
static Asking s_asking[4];
static u32 s_allowed[16]; static int s_nAllowed;
static u32 s_refusedIp[32]; static int s_nRefused;
static bool hasIp(const u32 *list, int n, u32 ip) { for (int i = 0; i < n; i++) if (list[i] == ip) return true; return false; }
struct Junk { u32 ip; int n; double since, told; };
static Junk s_junk[8];
static void junk(u32 ip, double now)
{
	Junk *j = 0;
	for (auto &q : s_junk) if (q.ip == ip) j = &q;
	if (!j) { j = &s_junk[0]; for (auto &q : s_junk) if (q.since < j->since) j = &q; memset(j, 0, sizeof *j); j->ip = ip; j->since = now; j->told = -999; }
	if (now - j->since > 5) { j->n = 0; j->since = now; }
	if (++j->n > 30 && now - j->told > 60)
	{
		j->told = now;
		NLOG("net: *.*.*.%u sends packets that aren't ours (%d in 5 s) - dropped", ip >> 24, j->n);
	}
}
static u32 parseu(const char *v) { u32 x = 0; while (*v >= '0' && *v <= '9') x = x * 10 + (u32)(*v++ - '0'); return x; }
static u32 unixNow()
{
	u64 ft = 0;
	GetSystemTimeAsFileTime(&ft);
	double sec = ((double)(u32)(ft >> 32) * 4294967296.0 + (double)(u32)ft) / 1e7 - 11644473600.0;
	return (u32)sec;
}
static Peer s_peers[4];
struct Inbox { u32 epoch; Event ev[kPeerEv]; int nEv; u32 taken, ackUpTo, skipTo; double used; };
static Inbox s_inbox[4];
static Inbox *inboxOf(u32 epoch)
{
	if (!epoch) return 0;
	for (auto &b : s_inbox) if (b.epoch == epoch) return &b;
	return 0;
}
static const u8 kSoloId = 3;

static double hostNow(double now) { return s_mode == 2 ? now + s_clockOff : now - s_base; }
static u32 ms32(double sec) { return sec <= 0 ? 0 : sec >= 4.0e6 ? 4000000000u : (u32)(sec * 1000.0); }

static void hexKey(const char *h)
{
	for (int i = 0; i < 16 && h[2 * i] && h[2 * i + 1]; i++)
	{
		auto v = [](char c) -> u8 { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
		s_key[i] = (u8)(v(h[2 * i]) << 4 | v(h[2 * i + 1]));
	}
}

static bool parseAddr(const char *s, Addr *a)
{
	u32 parts[4] = {0, 0, 0, 0}, port = 0;
	int i = 0;
	const char *p = s;
	while (*p && i < 4)
	{
		if (*p >= '0' && *p <= '9') parts[i] = parts[i] * 10 + (*p - '0');
		else if (*p == '.') i++;
		else if (*p == ':') { i++; p++; break; }
		else return false;
		p++;
	}
	if (i != 4) return false;
	while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
	if (!port) port = 27050;
	a->ip = parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24;
	a->port = (u16)((port >> 8) | (port & 0xFF) << 8);
	return true;
}

static void addrText(const Addr &a, char *out, u32 cap)
{
	u32 ip = a.ip;
	rt::fmt(out, cap, "*.*.*.%u:%u", ip >> 24, (u32)((a.port >> 8) | (a.port & 0xFF) << 8));
}

static bool sameAddr(const Addr &a, const Addr &b) { return a.ip == b.ip && a.port == b.port; }

static void sendRaw(const Addr &to, u8 type, const void *body, u32 n, double now, u8 from)
{
	if (s_sock == ~0u) return;
	u8 buf[1024];
	if (sizeof(Hdr) + n + 8 > sizeof buf) { NLOG("net: a packet of type %u too big to send (%u bytes) - dropped", (u32)type, n); return; }
	Hdr h = {kMagic, kProto, type, from, 0, ++s_seq, s_session, ms32(hostNow(now))};
	memcpy(buf, &h, sizeof h);
	if (n) memcpy(buf + sizeof h, body, n);
	u64 tag = siphash(s_key, buf, sizeof h + n);
	memcpy(buf + sizeof h + n, &tag, 8);
	sockaddr_in4 sa = {2, to.port, to.ip, {0}};
	sendto(s_sock, (const char *)buf, sizeof h + n + 8, 0, &sa, sizeof sa);
}

static u32 packQ(const float *q)
{
	int big = 0;
	for (int i = 1; i < 4; i++) if (rt::absf(q[i]) > rt::absf(q[big])) big = i;
	float sg = q[big] < 0 ? -1.0f : 1.0f;
	u32 out = (u32)big << 30;
	int sh = 20;
	for (int i = 0; i < 4; i++)
	{
		if (i == big) continue;
		float v = q[i] * sg * 0.70710678f + 0.5f;
		if (v < 0) v = 0; if (v > 1) v = 1;
		out |= (u32)(v * 1023.0f + 0.5f) << sh;
		sh -= 10;
	}
	return out;
}
static void unpackQ(u32 v, float *q)
{
	int big = (int)(v >> 30), sh = 20;
	float sum = 0;
	for (int i = 0; i < 4; i++)
	{
		if (i == big) continue;
		float c = (((v >> sh) & 1023) / 1023.0f - 0.5f) * 1.41421356f;
		q[i] = c; sum += c * c; sh -= 10;
	}
	q[big] = sum < 1 ? rt::sqrt(1 - sum) : 0;
}

static bool poseValid(const u8 *pose, int len)
{
	if (len < 1) return false;
	int n = pose[0], at = 1;
	if (n > kPoseBones) return false;
	for (int i = 0; i < n; i++)
	{
		if (at + 7 > len) return false;
		u8 fl = pose[at + 2];
		at += 7 + ((fl & 1) ? 6 : 0);
	}
	return at <= len;
}
static void decodePose(const u8 *pose, int len, Pose &o)
{
	o.n = 0;
	if (!poseValid(pose, len)) return;
	int n = pose[0], at = 1;
	for (int i = 0; i < n; i++)
	{
		u16 h; memcpy(&h, pose + at, 2);
		u8 fl = pose[at + 2];
		u32 q; memcpy(&q, pose + at + 3, 4);
		o.hash[i] = h; unpackQ(q, o.q[i]); o.hasT[i] = (fl & 1) != 0;
		at += 7;
		if (fl & 1) { i16 t[3]; memcpy(t, pose + at, 6); for (int k = 0; k < 3; k++) o.t[i][k] = t[k] / 8.0f; at += 6; }
	}
	o.n = n;
}
static int encodePose(const Pose &P, u8 *out)
{
	int n = P.n > kPoseBones ? kPoseBones : P.n, at = 1, nt = 0;
	out[0] = (u8)n;
	for (int i = 0; i < n; i++)
	{
		bool t = P.hasT[i] && nt < 8;
		memcpy(out + at, &P.hash[i], 2);
		out[at + 2] = t ? 1 : 0;
		u32 q = packQ(P.q[i]); memcpy(out + at + 3, &q, 4);
		at += 7;
		if (t)
		{
			nt++;
			for (int k = 0; k < 3; k++) { float v = P.t[i][k] * 8.0f; i16 c = (i16)(v > 32000 ? 32000 : v < -32000 ? -32000 : v); memcpy(out + at + 2 * k, &c, 2); }
			at += 6;
		}
	}
	return at;
}
static void push(Peer &p, double t, const StatePk &s, const u8 *pose, int poseLen)
{
	if (p.n > 0 && t <= p.snap[p.n - 1].t) return;
	if (p.n == 32) { memmove(p.snap, p.snap + 1, sizeof(Snap) * 31); p.n = 31; }
	Snap &sn = p.snap[p.n];
	sn.t = t; sn.s = s; sn.poseLen = 0;
	if (pose && poseLen > 0 && poseLen <= kMaxPoseBytes && poseValid(pose, poseLen)) { memcpy(sn.pose, pose, poseLen); sn.poseLen = (u16)poseLen; }
	p.n++;
}
static void storeLook(Peer &p, const u8 *body, int bn) { if (bn > 0 && bn <= kMaxLook) { memcpy(p.look, body, bn); p.lookLen = (u16)bn; } }
static void storeMods(Peer &p, const u8 *body, int bn)
{
	if (bn < 5) return;
	u16 total, first; memcpy(&total, body, 2); memcpy(&first, body + 2, 2);
	int n = body[4];
	if (bn < 5 + 4 * n || total > kMaxMods || first + n > total) return;
	memcpy(p.mods + first, body + 5, 4 * n);
	if (first == 0 || total != p.modsTotal) p.modsHeard = 0;
	if (first == p.modsHeard) p.modsHeard += n;
	p.modsTotal = total;
	if (p.modsHeard >= total) p.nMods = total;
}
static void storeSkel(Peer &p, const u8 *body, int bn)
{
	if (bn < 2) return;
	u8 subject = body[0], n = body[1];
	if (n > kPoseBones || bn < 2 + n * 8) return;
	Skel *sk = subject == 0 ? &p.skel : subject <= kMaxEnemies ? &p.enemy[subject - 1].skel : 0;
	if (!sk) return;
	for (int i = 0; i < n; i++)
	{
		const u8 *e = body + 2 + 8 * i;
		memcpy(&sk->hash[i], e, 2);
		i16 t[3]; memcpy(t, e + 2, 6);
		for (int k = 0; k < 3; k++) sk->t[i][k] = t[k] / 16.0f;
	}
	sk->n = n;
	if (subject == 0) p.haveSkel = true; else p.enemy[subject - 1].haveSkel = true;
}
static void storeEnemy(Peer &p, double t, const u8 *body, int bn)
{
	if (bn < (int)sizeof(EnemyPk)) return;
	EnemyPk e; memcpy(&e, body, sizeof e);
	if (e.slot >= kMaxEnemies) return;
	for (int i = e.count; i < kMaxEnemies; i++) p.enemy[i].used = false;
	if (e.count == 0 || e.slot >= e.count) return;
	EnemySlot &es = p.enemy[e.slot];
	if (es.used && es.n > 0 && (es.snap[es.n - 1].e.low != e.low || es.snap[es.n - 1].e.modHash != e.modHash)) { es.n = 0; es.haveSkel = false; }
	if (!es.used) { es.n = 0; es.haveSkel = false; }
	es.used = true;
	es.heard = rt::now();
	if (es.n > 0 && t <= es.snap[es.n - 1].t) return;
	if (es.n == 8) { memmove(es.snap, es.snap + 1, sizeof(ESnap) * 7); es.n = 7; }
	ESnap &sn = es.snap[es.n++];
	sn.t = t; sn.e = e; sn.poseLen = 0;
	int pl = bn - (int)sizeof e;
	if (pl > 0 && pl <= kMaxPoseBytes && poseValid(body + sizeof e, pl)) { memcpy(sn.pose, body + sizeof e, pl); sn.poseLen = (u16)pl; }
}
static void storeEvents2(Peer &p, const u8 *body, int bn)
{
	if (bn < 33) return;
	char nm[24]; memcpy(nm, body, 24); nm[23] = 0;
	if (nm[0] && !rt::streq(nm, p.name)) { NLOG("net: player %d is now %s", (int)p.id, nm); rt::strcpy(p.name, nm, sizeof p.name); }
	u32 epoch, base; memcpy(&epoch, body + 24, 4); memcpy(&base, body + 28, 4);
	int n = body[32], at = 33;
	if (!epoch) return;
	p.evEpoch = epoch;
	Inbox *ib = inboxOf(epoch);
	if (!ib)
	{
		ib = &s_inbox[0];
		for (auto &b : s_inbox) if (!b.epoch || b.used < ib->used) { ib = &b; if (!b.epoch) break; }
		ib->epoch = epoch; ib->nEv = 0; ib->skipTo = 0; ib->taken = ib->ackUpTo = base ? base - 1 : 0;
	}
	ib->used = rt::now();
	Inbox &q = *ib;
	if (base && base - 1 > q.ackUpTo)
	{
		NLOG("net: %u of %s's events were gone before they got here (they're kept 10 minutes or 4096 at most)", base - 1 - q.ackUpTo, p.name);
		q.ackUpTo = base - 1;
		if (q.skipTo < base - 1) q.skipTo = base - 1;
	}
	for (int i = 0; i < n; i++)
	{
		if (at + 6 > bn) break;
		Event e;
		memcpy(&e.id, body + at, 4); e.type = body[at + 4]; e.len = body[at + 5];
		if (e.len > sizeof e.data || at + 6 + e.len > bn) break;
		memcpy(e.data, body + at + 6, e.len);
		at += 6 + e.len;
		if (e.id <= q.taken) continue;
		int k = 0;
		while (k < q.nEv && q.ev[k].id < e.id) k++;
		if (k < q.nEv && q.ev[k].id == e.id) continue;
		if (q.nEv == kPeerEv)
		{
			if (k == q.nEv || q.ev[q.nEv - 1].id <= q.ackUpTo) continue;
			q.nEv--;
		}
		memmove(q.ev + k + 1, q.ev + k, sizeof(Event) * (q.nEv - k));
		q.ev[k] = e; q.nEv++;
	}
	if (q.ackUpTo < q.taken) q.ackUpTo = q.taken;
	for (int i = 0; i < q.nEv; i++) { if (q.ev[i].id == q.ackUpTo + 1) q.ackUpTo++; else if (q.ev[i].id > q.ackUpTo + 1) break; }
}
static u32 s_evEpoch;
static void storeAck(Peer &p, const u8 *body, int bn, u8 me)
{
	if (bn < 1) return;
	int n = body[0];
	for (int i = 0; i < n && 1 + i * 9 + 9 <= bn; i++)
	{
		const u8 *q = body + 1 + i * 9;
		u32 ep, up; memcpy(&ep, q + 1, 4); memcpy(&up, q + 5, 4);
		if (q[0] != me || ep != s_evEpoch) continue;
		p.evAcks = true;
		if (up > p.evAckMine) p.evAckMine = up;
	}
}
static void storeLoot(Peer &p, const u8 *body, int bn)
{
	if (bn < (int)sizeof(LootHdr)) return;
	LootHdr lh; memcpy(&lh, body, sizeof lh);
	if (lh.nDrop > kMaxDrops || lh.nTaken > kMaxTaken) return;
	if (bn < (int)(sizeof lh + lh.nDrop * sizeof(DropPk) + lh.nTaken * sizeof(TakenPk))) return;
	const u8 *q = body + sizeof lh;
	p.nDrops = lh.nDrop;
	for (int i = 0; i < lh.nDrop; i++, q += sizeof(DropPk))
	{
		DropPk d; memcpy(&d, q, sizeof d);
		Drop &o = p.drops[i];
		o.id = d.id; o.item.low = d.low; o.item.modHash = d.modHash; o.count = d.count;
		memcpy(o.pos, d.pos, 12); o.world = d.world; o.cell = d.cell;
	}
	p.nTaken = lh.nTaken;
	for (int i = 0; i < lh.nTaken; i++, q += sizeof(TakenPk))
	{
		TakenPk t; memcpy(&t, q, sizeof t);
		p.taken[i].owner = t.owner; p.taken[i].id = t.id;
	}
	p.haveLoot = true;
}

static void startClock()
{
	if (s_base >= 0) return;
	s_base = rt::now();
	u32 un = unixNow();
	s_seq = (un % 4000000u) * 1024u;
	s_nonce = (u32)(s_base * 1.0e6) ^ (un * 2654435761u) ^ (u32)(unsigned long)&s_nonce;
	if (!s_nonce) s_nonce = 1;
}
void init(const char *iniPath)
{
	startClock();
	u32 len = 0;
	char *txt = rt::readfile(iniPath, &len);
	s_mode = 0;
	if (txt)
	{
		char *line = txt;
		while (line && *line)
		{
			char *nl = line;
			while (*nl && *nl != '\n' && *nl != '\r') nl++;
			char save = *nl; *nl = 0;
			char *eq = line;
			while (*eq && *eq != '=') eq++;
			if (*eq == '=' && *line != ';')
			{
				*eq = 0;
				const char *k = line, *v = eq + 1;
				if (rt::streq(k, "mode")) s_mode = rt::streq(v, "host") ? 1 : rt::streq(v, "join") ? 2 : 0;
				else if (rt::streq(k, "port")) s_port = (u16)rt::parsei(v);
				else if (rt::streq(k, "key")) hexKey(v);
				else if (rt::streq(k, "session")) s_session = parseu(v);
				else if (rt::streq(k, "expires")) s_expires = parseu(v);
				else if (rt::streq(k, "name")) rt::strcpy(s_name, v, sizeof s_name);
				else if (rt::streq(k, "host"))
				{
					const char *p = v;
					while (*p && s_nHost < 8)
					{
						char one[40]; int j = 0;
						while (*p && *p != ',' && j < 39) one[j++] = *p++;
						one[j] = 0;
						if (*p == ',') p++;
						if (parseAddr(one, &s_hostAddrs[s_nHost])) s_nHost++;
					}
				}
			}
			*nl = save;
			line = *nl ? nl + 1 : nl;
			while (*line == '\n' || *line == '\r') line++;
		}
		rt::free(txt);
	}
	if (s_mode == 0) { rt::strcpy(s_status, "off (no game hosted or joined in setup)", sizeof s_status); NLOG("net: %s", s_status); return; }
	u8 wsa[512];
	if (WSAStartup(0x0202, wsa) != 0) { rt::strcpy(s_status, "winsock didn't start", sizeof s_status); NLOG("net: %s", s_status); s_mode = 0; return; }
	s_sock = socket(2, 2, 17);
	u32 nb = 1;
	ioctlsocket(s_sock, (i32)0x8004667E, &nb);
	sockaddr_in4 sa = {2, (u16)(s_mode == 1 ? ((s_port >> 8) | (s_port & 0xFF) << 8) : 0), 0, {0}};
	if (bind(s_sock, &sa, sizeof sa) != 0)
	{
		rt::fmt(s_status, sizeof s_status, "couldn't use UDP port %u (error %d): is another copy running?", (u32)s_port, WSAGetLastError());
		NLOG("net: %s", s_status);
		closesocket(s_sock); s_sock = ~0u; s_mode = 0;
		return;
	}
	if (s_mode == 1)
	{
		s_myId = 0; s_joined = true;
		rt::fmt(s_status, sizeof s_status, "hosting on UDP port %u as %s, waiting for friends", (u32)s_port, s_name);
	}
	else rt::fmt(s_status, sizeof s_status, "joining (%d host addresses) as %s", s_nHost, s_name);
	NLOG("net: %s", s_status);
}

bool active() { return s_mode != 0 || s_solo; }
bool hosting() { return s_mode == 1; }
const char *statusLine() { return s_status; }

static int pack(const Self &me, u8 *out)
{
	StatePk s;
	memset(&s, 0, sizeof s);
	memcpy(s.pos, me.pos, 12);
	s.yaw = me.yaw; s.pitch = me.pitch; s.roll = me.roll;
	s.world = me.world; s.cell = me.cell; s.mode = me.mode; s.flags = me.flags; s.car = me.car; s.speed = me.speed; s.hour = me.hour;
	s.weather = me.weather; s.snow = me.snow;
	s.parkCar = me.parkCar; memcpy(s.parkPos, me.parkPos, 12); s.parkYaw = me.parkYaw; s.parkPitch = me.parkPitch; s.parkRoll = me.parkRoll;
	s.parkWorld = me.parkWorld; s.parkCell = me.parkCell;
	s.ride = me.ride; s.invite = me.invite; s.weapon = me.weapon; s.shots = me.shots;
	s.health = me.health; s.status = me.status; s.downLeft = me.downLeft; s.rules = me.rules;
	s.level = me.level; s.maxHealth = me.maxHealth; memcpy(s.dmgFor, me.dmgFor, sizeof s.dmgFor);
	memcpy(s.wheelOff, me.wheelOff, 4); s.steer = me.steer; s.wheelie = me.wheelie; s.shakeZ = me.shakeZ; s.shakePitch = me.shakePitch; s.shakeRoll = me.shakeRoll;
	for (int k = 0; k < 4; k++) { float q = me.gunQ[k] < -1 ? -1 : me.gunQ[k] > 1 ? 1 : me.gunQ[k]; s.gunQ[k] = (i16)(q * 32767.0f); }
	memcpy(out, &s, sizeof s);
	int n = sizeof s;
	if (me.hasPose && me.pose.n > 0) n += encodePose(me.pose, out + n);
	return n;
}
static Self unpack(const StatePk &s)
{
	Self o;
	memset(&o, 0, sizeof o);
	memcpy(o.pos, s.pos, 12); o.yaw = s.yaw; o.pitch = s.pitch; o.roll = s.roll;
	o.world = s.world; o.cell = s.cell; o.mode = s.mode; o.flags = s.flags; o.car = s.car; o.speed = s.speed; o.hour = s.hour;
	o.weather = s.weather; o.snow = s.snow;
	o.parkCar = s.parkCar; memcpy(o.parkPos, s.parkPos, 12); o.parkYaw = s.parkYaw; o.parkPitch = s.parkPitch; o.parkRoll = s.parkRoll;
	o.parkWorld = s.parkWorld; o.parkCell = s.parkCell;
	o.ride = s.ride; o.invite = s.invite; o.weapon = s.weapon; o.shots = s.shots;
	o.health = s.health; o.status = s.status; o.downLeft = s.downLeft; o.rules = s.rules;
	o.level = s.level; o.maxHealth = s.maxHealth; memcpy(o.dmgFor, s.dmgFor, sizeof o.dmgFor);
	memcpy(o.wheelOff, s.wheelOff, 4); o.steer = s.steer; o.wheelie = s.wheelie; o.shakeZ = s.shakeZ; o.shakePitch = s.shakePitch; o.shakeRoll = s.shakeRoll;
	for (int k = 0; k < 4; k++) o.gunQ[k] = s.gunQ[k] / 32767.0f;
	if (o.gunQ[0] == 0 && o.gunQ[1] == 0 && o.gunQ[2] == 0 && o.gunQ[3] == 0) o.gunQ[0] = 1;
	return o;
}

static u32 s_soloSock = ~0u;
static Addr s_soloAddr;
void soloTest(bool on)
{
	startClock();
	s_solo = on;
	if (on && s_soloSock == ~0u)
	{
		u8 wsa[512];
		WSAStartup(0x0202, wsa);
		s_soloSock = socket(2, 2, 17);
		u32 nb = 1;
		ioctlsocket(s_soloSock, (i32)0x8004667E, &nb);
		sockaddr_in4 sa = {2, 0, 0x0100007F, {0}};
		bind(s_soloSock, &sa, sizeof sa);
		sockaddr_in4 me = {2, (u16)((27059 >> 8) | (27059 & 0xFF) << 8), 0x0100007F, {0}};
		closesocket(s_soloSock);
		s_soloSock = socket(2, 2, 17);
		ioctlsocket(s_soloSock, (i32)0x8004667E, &nb);
		if (bind(s_soloSock, &me, sizeof me) != 0) NLOG("net: solo test couldn't bind 127.0.0.1:27059 (%d)", WSAGetLastError());
		s_soloAddr.ip = 0x0100007F; s_soloAddr.port = me.port;
		if (s_mode == 0) for (int i = 0; i < 16; i++) s_key[i] = (u8)(0x5A ^ i * 17);
	}
	s_peers[kSoloId].used = on; s_peers[kSoloId].id = kSoloId; s_peers[kSoloId].n = 0;
	rt::strcpy(s_peers[kSoloId].name, "you, 2 s ago", sizeof s_peers[kSoloId].name);
	NLOG("net: solo test %s", on ? "on (your own state, looped back through the network 2 s late)" : "off");
}
bool soloTesting() { return s_solo; }

static bool isHostAddr(const Addr &a)
{
	for (int i = 0; i < s_nHost; i++) if (sameAddr(s_hostAddrs[i], a)) return true;
	return false;
}
static void ctl(const Addr &to, u8 type, u32 nonce, i32 skew, double now) { CtlPk c = {nonce, skew}; sendRaw(to, type, &c, sizeof c, now, 0); }

#pragma pack(push, 1)
struct FileOfferPk { u32 id, size, chunks, crc; char name[48]; };
struct FilePartHdr { u32 id, idx; };
struct FileAckHdr { u32 id, have, lowest; u8 state; u16 nMiss; };
#pragma pack(pop)
enum { kChunk = 900, kMaxFile = 64 << 20, kMaxMiss = 200 };
static u32 crc32(const u8 *p, u32 n)
{
	u32 c = 0xFFFFFFFFu;
	for (u32 i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
	return ~c;
}
static bool s_fileWanted;
void setFileWanted(bool on) { s_fileWanted = on; }
static bool safeFileName(const char *s)
{
	return rt::streq(s, "MMW-Session.fos") || rt::streq(s, "MMW-Session.nvse");
}
struct Outgoing { bool on; u8 peer; u32 id, size, chunks, crc; char name[48]; u8 *data; float *sentAt; u8 *acked; u32 next, have; double lastOffer, lastAck, lastTick, started; double rate, budget; u32 sent, resent; u8 result; };
struct Incoming { bool on; u32 id, size, chunks, crc; char name[48]; u8 *data, *have; u32 nHave; double lastAck, started, doneAt; u8 state; };
static Outgoing s_out;
static Incoming s_in;
static char s_fileDir[300];
static u32 s_fileSeq;
static char s_lastFile[48]; static u8 s_lastFileState;
static void outFree() { if (s_out.data) rt::free(s_out.data); if (s_out.sentAt) rt::free(s_out.sentAt); if (s_out.acked) rt::free(s_out.acked); memset(&s_out, 0, sizeof s_out); }
static void inFree() { if (s_in.data) rt::free(s_in.data); if (s_in.have) rt::free(s_in.have); memset(&s_in, 0, sizeof s_in); }
static void fileOffer(const u8 *body, int bn, double now)
{
	if (bn < (int)sizeof(FileOfferPk)) return;
	FileOfferPk o; memcpy(&o, body, sizeof o); o.name[47] = 0;
	if (s_in.on && s_in.id == o.id) return;
	if (!s_fileWanted)
	{
		static u32 said;
		if (said != o.id) { said = o.id; NLOG("net: the host offered %s, but you haven't asked for their world - not taken", o.name); }
		return;
	}
	if (!safeFileName(o.name) || !o.size || o.size > kMaxFile || o.chunks != (o.size + kChunk - 1) / kChunk) { NLOG("net: a file offered with a bad name or size - not taken"); return; }
	inFree();
	s_in.data = (u8 *)rt::alloc(o.size); s_in.have = (u8 *)rt::alloc(o.chunks);
	if (!s_in.data || !s_in.have) { inFree(); NLOG("net: no memory for a %u-byte file", o.size); return; }
	memset(s_in.have, 0, o.chunks);
	s_in.on = true; s_in.id = o.id; s_in.size = o.size; s_in.chunks = o.chunks; s_in.crc = o.crc; s_in.started = now;
	rt::strcpy(s_in.name, o.name, sizeof s_in.name);
	NLOG("net: the host is sending %s (%u bytes, %u parts)", s_in.name, o.size, o.chunks);
}
static void filePart(const u8 *body, int bn)
{
	if (!s_in.on || s_in.state || bn < (int)sizeof(FilePartHdr)) return;
	FilePartHdr h; memcpy(&h, body, sizeof h);
	if (h.id != s_in.id || h.idx >= s_in.chunks || s_in.have[h.idx]) return;
	u32 at = h.idx * kChunk, len = s_in.size - at < (u32)kChunk ? s_in.size - at : (u32)kChunk;
	if (bn - (int)sizeof h != (int)len) return;
	memcpy(s_in.data + at, body + sizeof h, len);
	s_in.have[h.idx] = 1; s_in.nHave++;
	if (s_in.nHave == s_in.chunks)
	{
		u32 c = crc32(s_in.data, s_in.size);
		bool ok = c == s_in.crc;
		if (ok)
		{
			char p[400];
			rt::fmt(p, sizeof p, "%s%s", s_fileDir, s_in.name);
			ok = rt::writefile(p, (const char *)s_in.data, s_in.size, false);
			NLOG("net: %s came in whole in %.1f s and was written to %s%s", s_in.name, rt::now() - s_in.started, p, ok ? "" : " - FAILED to write");
		}
		else NLOG("net: %s came in but its check is wrong (%08x, want %08x) - thrown away", s_in.name, c, s_in.crc);
		s_in.state = ok ? 1 : 2;
		rt::strcpy(s_lastFile, s_in.name, sizeof s_lastFile); s_lastFileState = s_in.state;
		rt::free(s_in.data); s_in.data = 0;
		s_in.lastAck = 0; s_in.doneAt = rt::now();
	}
}
static void fileAck(u8 slot, const u8 *body, int bn, double now)
{
	if (!s_out.on || slot != s_out.peer || bn < (int)sizeof(FileAckHdr)) return;
	FileAckHdr a; memcpy(&a, body, sizeof a);
	if (a.id != s_out.id || bn < (int)sizeof a + 4 * a.nMiss || a.nMiss > kMaxMiss) return;
	s_out.lastAck = now;
	if (a.state)
	{
		s_out.result = a.state;
		NLOG("net: %s %s in %.1f s (%u parts sent, %u again)", s_out.name, a.state == 1 ? "arrived whole" : "FAILED its check there", now - s_out.started, s_out.sent, s_out.resent);
		s_out.on = false;
		return;
	}
	u32 miss[kMaxMiss];
	memcpy(miss, body + sizeof a, 4 * a.nMiss);
	for (u32 i = 0; i < s_out.chunks && i < a.lowest; i++) s_out.acked[i] = 1;
	if (a.nMiss)
	{
		u32 tail = miss[a.nMiss - 1];
		for (u32 i = a.lowest; i <= tail && i < s_out.chunks; i++) s_out.acked[i] = 1;
		for (int k = 0; k < a.nMiss; k++) if (miss[k] < s_out.chunks) s_out.acked[miss[k]] = 0;
	}
	if (a.lowest < s_out.chunks) s_out.acked[a.lowest] = 0;
	s_out.have = a.have;
	if (a.nMiss > 60) s_out.rate *= 0.85; else s_out.rate *= 1.06;
	if (s_out.rate < 100) s_out.rate = 100; if (s_out.rate > 3000) s_out.rate = 3000;
}
bool sendFile(int peer, const char *path, const char *name)
{
	if (s_mode != 1 || peer < 1 || peer > 2 || !s_peers[peer].used || s_out.on || !safeFileName(name)) return false;
	u32 n = 0;
	char *d = rt::readfile(path, &n);
	if (!d || !n || n > (u32)kMaxFile) { if (d) rt::free(d); NLOG("net: %s couldn't be read to send", path); return false; }
	outFree();
	s_out.data = (u8 *)d; s_out.size = n; s_out.chunks = (n + kChunk - 1) / kChunk; s_out.crc = crc32(s_out.data, n);
	s_out.sentAt = (float *)rt::alloc(4 * s_out.chunks); s_out.acked = (u8 *)rt::alloc(s_out.chunks);
	if (!s_out.sentAt || !s_out.acked) { outFree(); return false; }
	memset(s_out.sentAt, 0, 4 * s_out.chunks); memset(s_out.acked, 0, s_out.chunks);
	s_out.on = true; s_out.peer = (u8)peer; s_out.id = (unixNow() << 8) ^ ++s_fileSeq; s_out.rate = 400; s_out.started = rt::now();
	rt::strcpy(s_out.name, name, sizeof s_out.name);
	NLOG("net: sending %s (%u bytes, %u parts) to %s", name, n, s_out.chunks, s_peers[peer].name);
	return true;
}
bool sending(u32 *have, u32 *total) { if (have) *have = s_out.have; if (total) *total = s_out.chunks; return s_out.on; }
u8 sendResult() { return s_out.on ? 0 : s_out.result; }
void setFileDir(const char *dir) { rt::strcpy(s_fileDir, dir, sizeof s_fileDir); }
bool receiving(char *name, int cap, u32 *have, u32 *total)
{
	if (!s_in.on || s_in.state) return false;
	if (name) rt::strcpy(name, s_in.name, cap);
	if (have) *have = s_in.nHave; if (total) *total = s_in.chunks;
	return true;
}
u8 lastFile(char *name, int cap) { if (name) rt::strcpy(name, s_lastFile, cap); return s_lastFileState; }
void clearLastFile() { s_lastFile[0] = 0; s_lastFileState = 0; }
static void fileTick(double now)
{
	if (s_out.on)
	{
		if (!s_peers[s_out.peer].used) { NLOG("net: %s not sent - %s left", s_out.name, s_peers[s_out.peer].name); s_out.result = 2; s_out.on = false; return; }
		double dt = s_out.lastTick ? now - s_out.lastTick : 0;
		s_out.lastTick = now;
		const Addr &to = s_peers[s_out.peer].addr;
		if (!s_out.lastAck)
		{
			if (now - s_out.lastOffer > 0.5) { s_out.lastOffer = now; FileOfferPk o = {s_out.id, s_out.size, s_out.chunks, s_out.crc, {0}}; rt::strcpy(o.name, s_out.name, sizeof o.name); sendRaw(to, FILEOFFER, &o, sizeof o, now, 0); }
			if (now - s_out.started > 15) { NLOG("net: %s not sent - no answer", s_out.name); s_out.result = 2; s_out.on = false; }
			return;
		}
		if (now - s_out.lastAck > 15) { NLOG("net: %s stopped - no answer for 15 s", s_out.name); s_out.result = 2; s_out.on = false; return; }
		s_out.budget += s_out.rate * dt;
		if (s_out.budget > 80) s_out.budget = 80;
		u8 pk[sizeof(FilePartHdr) + kChunk];
		float t = (float)(now - s_out.started);
		auto sendPart = [&](u32 i) {
			FilePartHdr h = {s_out.id, i};
			u32 at = i * kChunk, len = s_out.size - at < (u32)kChunk ? s_out.size - at : (u32)kChunk;
			memcpy(pk, &h, sizeof h); memcpy(pk + sizeof h, s_out.data + at, len);
			sendRaw(to, FILEPART, pk, sizeof h + len, now, 0);
			if (s_out.sentAt[i]) s_out.resent++;
			s_out.sentAt[i] = t + 0.001f; s_out.sent++; s_out.budget -= 1;
		};
		static u32 scan;
		for (u32 k = 0; k < s_out.next && s_out.budget >= 1 && k < 4000; k++)
		{
			if (scan >= s_out.next) scan = 0;
			u32 i = scan++;
			if (!s_out.acked[i] && t - s_out.sentAt[i] > 0.4f) sendPart(i);
		}
		while (s_out.budget >= 1 && s_out.next < s_out.chunks) sendPart(s_out.next++);
		if (s_out.next >= s_out.chunks && s_out.budget >= 1 && t - s_out.sentAt[s_out.chunks - 1] > 0.6f && !s_out.acked[s_out.chunks - 1]) sendPart(s_out.chunks - 1);
	}
	if (s_in.on && s_mode == 2 && s_joined && now - s_in.lastAck > 0.2)
	{
		s_in.lastAck = now;
		u8 b[sizeof(FileAckHdr) + 4 * kMaxMiss];
		FileAckHdr a = {s_in.id, s_in.nHave, 0, s_in.state, 0};
		u32 *miss = (u32 *)(b + sizeof a);
		if (!s_in.state)
		{
			u32 i = 0;
			while (i < s_in.chunks && s_in.have[i]) i++;
			a.lowest = i;
			u32 top = 0;
			for (u32 k = s_in.chunks; k > 0; k--) if (s_in.have[k - 1]) { top = k - 1; break; }
			for (; i < top && a.nMiss < kMaxMiss; i++) if (!s_in.have[i]) miss[a.nMiss++] = i;
		}
		else a.lowest = s_in.chunks;
		memcpy(b, &a, sizeof a);
		sendRaw(s_host, FILEACK, b, sizeof a + 4 * a.nMiss, now, s_myId);
		if (s_in.state && now - s_in.doneAt > 3) inFree();
	}
}
static void handle(const u8 *buf, int n, const Addr &from, double now, bool soloSock)
{
	if (n < (int)(sizeof(Hdr) + 8)) { if (!soloSock) junk(from.ip, now); return; }
	Hdr h;
	memcpy(&h, buf, sizeof h);
	u64 tag;
	memcpy(&tag, buf + n - 8, 8);
	if (h.magic != kMagic || siphash(s_key, buf, n - 8) != tag) { if (!soloSock) junk(from.ip, now); return; }
	if (h.proto != kProto)
	{
		rt::fmt(s_status, sizeof s_status, "a player has a different Mojave Enhanced (protocol %u, ours %u): update both", h.proto, kProto);
		return;
	}
	if (h.session != s_session) return;
	const u8 *body = buf + sizeof h;
	int bn = n - (int)sizeof h - 8;
	double t = h.t / 1000.0;
	if (soloSock)
	{
		if (!fresh(s_win[kSoloId], h.seq)) return;
		if (h.type == STATE && bn >= (int)sizeof(StatePk))
		{
			StatePk s; memcpy(&s, body, sizeof s);
			Peer &p = s_peers[kSoloId];
			p.heard = now;
			push(p, t + 2.0, s, body + sizeof s, bn - (int)sizeof s);
		}
		else if (h.type == LOOT) storeLoot(s_peers[kSoloId], body, bn);
		else if (h.type == SKEL) storeSkel(s_peers[kSoloId], body, bn);
		else if (h.type == LOOK) storeLook(s_peers[kSoloId], body, bn);
		else if (h.type == MODS) storeMods(s_peers[kSoloId], body, bn);
		else if (h.type == EVENTS2) storeEvents2(s_peers[kSoloId], body, bn);
		else if (h.type == ENEMY) storeEnemy(s_peers[kSoloId], t + 2.0, body, bn);
		return;
	}
	if (s_mode == 1)
	{
		int slot = -1;
		for (int i = 1; i <= 2; i++) if (s_peers[i].used && sameAddr(s_peers[i].addr, from)) slot = i;
		if (h.type == HELLO && bn >= (int)sizeof(HelloPk))
		{
			HelloPk hp; memcpy(&hp, body, sizeof hp); hp.name[23] = 0;
			for (int k = 0; k < 23 && hp.name[k]; k++) if ((u8)hp.name[k] < 32 || (u8)hp.name[k] > 126) hp.name[k] = '?';
			char at[32]; addrText(from, at, sizeof at);
			if (slot < 0)
			{
				u32 un = unixNow();
				i32 skew = (i32)(hp.stamp - un);
				if (skew > 120 || skew < -120)
				{
					static double told; static u32 toldIp;
					if (toldIp != from.ip || now - told > 60) { told = now; toldIp = from.ip; NLOG("net: %s at %s asks to join, but their PC's clock is %d s off - told them", hp.name, at, (int)skew); }
					ctl(from, CLOCK, hp.nonce, skew, now);
					return;
				}
				if (hasIp(s_refusedIp, s_nRefused, from.ip)) { ctl(from, REFUSED, hp.nonce, 0, now); return; }
				if (!hasIp(s_allowed, s_nAllowed, from.ip))
				{
					if (s_expires && un > s_expires) return;
					Asking *a = 0;
					for (auto &q : s_asking) if (q.used && q.addr.ip == from.ip) a = &q;
					if (!a) for (auto &q : s_asking) if (!q.used) { a = &q; memset(a, 0, sizeof *a); a->used = true; a->first = now; a->named = -999; break; }
					if (!a) return;
					a->addr = from;
					if (!rt::streq(a->name, hp.name) && now - a->named > 10)
					{
						a->named = now;
						rt::strcpy(a->name, hp.name, sizeof a->name);
						NLOG("net: %s asks to join from %s (plugin %u) - waiting for your answer", a->name, at, hp.build);
					}
					a->build = hp.build; a->nonce = hp.nonce; a->last = now;
					ctl(from, WAIT, hp.nonce, 0, now);
					return;
				}
				for (int i = 1; i <= 2 && slot < 0; i++)
					if (!s_peers[i].used)
					{
						slot = i;
						Peer &p = s_peers[i];
						memset(&p, 0, sizeof p);
						p.used = true; p.id = (u8)i; p.addr = from; p.heard = now;
						rt::strcpy(p.name, hp.name, sizeof p.name);
						p.build = hp.build;
						s_win[i].init = false;
						s_epoch[i]++;
						NLOG("net: %s joined from %s (player %d) - plugin %u%s", p.name, at, i, hp.build, hp.build == s_build ? ", the same as yours" : ", NOT the same as yours");
					}
			}
			if (slot < 0) { ctl(from, FULL, hp.nonce, 0, now); return; }
			WelcomePk w = {(u8)slot, {0}, s_build, hp.nonce};
			rt::strcpy(w.hostName, s_name, sizeof w.hostName);
			sendRaw(from, WELCOME, &w, sizeof w, now, 0);
			return;
		}
		if (slot < 0 || h.from != slot || !fresh(s_win[slot], h.seq)) return;
		Peer &p = s_peers[slot];
		p.heard = now;
		if ((h.type == STATE && bn >= (int)sizeof(StatePk)) || h.type == LOOT || h.type == SKEL || h.type == EVENTS2 || h.type == EVACK || h.type == ENEMY || h.type == LOOK || h.type == MODS)
		{
			if (h.type == STATE) { StatePk s; memcpy(&s, body, sizeof s); push(p, t, s, body + sizeof s, bn - (int)sizeof s); }
			else if (h.type == LOOT) storeLoot(p, body, bn);
			else if (h.type == SKEL) storeSkel(p, body, bn);
			else if (h.type == EVENTS2) storeEvents2(p, body, bn);
			else if (h.type == EVACK) storeAck(p, body, bn, 0);
			else if (h.type == LOOK) storeLook(p, body, bn);
			else if (h.type == MODS) storeMods(p, body, bn);
			else storeEnemy(p, t, body, bn);
			for (int i = 1; i <= 2; i++) if (i != slot && s_peers[i].used) {
				sockaddr_in4 sa = {2, s_peers[i].addr.port, s_peers[i].addr.ip, {0}};
				sendto(s_sock, (const char *)buf, n, 0, &sa, sizeof sa);
			}
		}
		else if (h.type == PING && bn == (int)sizeof(PingPk))
		{
			PingPk pp; memcpy(&pp, body, sizeof pp);
			pp.hostT = ms32(hostNow(now));
			sendRaw(from, PONG, &pp, sizeof pp, now, 0);
		}
		else if (h.type == FILEACK) fileAck((u8)slot, body, bn, now);
		else if (h.type == BYE) { NLOG("net: %s left", p.name); p.used = false; }
		return;
	}
	if (!s_joined)
	{
		if (!isHostAddr(from) || h.from != 0) return;
		if (h.type == WELCOME && bn >= (int)sizeof(WelcomePk))
		{
			WelcomePk w; memcpy(&w, body, sizeof w); w.hostName[23] = 0;
			if (w.nonce != s_nonce || w.id < 1 || w.id > 2) return;
			s_joined = true; s_myId = w.id; s_host = from;
			for (auto &wi : s_win) wi.init = false;
			fresh(s_win[0], h.seq);
			for (auto &e : s_epochSeen) e = 0;
			Peer &hp = s_peers[0];
			memset(&hp, 0, sizeof hp);
			hp.used = true; hp.id = 0; hp.heard = now;
			for (int k = 0; k < 23 && w.hostName[k]; k++) if ((u8)w.hostName[k] < 32 || (u8)w.hostName[k] > 126) w.hostName[k] = '?';
			rt::strcpy(hp.name, w.hostName, sizeof hp.name);
			hp.build = w.hostBuild;
			char at[32]; addrText(from, at, sizeof at);
			NLOG("net: the host runs plugin %u%s", w.hostBuild, w.hostBuild == s_build ? ", the same as yours" : ", NOT the same as yours");
			rt::fmt(s_status, sizeof s_status, "joined %s's game at %s as player %d", w.hostName, at, (int)w.id);
			NLOG("net: %s", s_status);
			return;
		}
		if (bn < (int)sizeof(CtlPk)) return;
		CtlPk c; memcpy(&c, body, sizeof c);
		if (c.nonce != s_nonce) return;
		if (h.type == FULL) rt::strcpy(s_status, "the host's game is full (3 players)", sizeof s_status);
		else if (h.type == WAIT) rt::strcpy(s_status, "waiting for the host to let you in", sizeof s_status);
		else if (h.type == REFUSED)
		{
			if (now >= s_refusedUntil) NLOG("net: the host didn't let you in - asking again in 30 s");
			rt::strcpy(s_status, "the host didn't let you in", sizeof s_status);
			s_refusedUntil = now + 30;
		}
		else if (h.type == CLOCK)
		{
			rt::fmt(s_status, sizeof s_status, "your PC's clock is %d minutes off the host's: set Windows' time automatically, then join again", (c.skew < 0 ? -c.skew : c.skew) / 60 + 1);
			static double told; if (now - told > 30) { told = now; NLOG("net: %s", s_status); }
		}
		return;
	}
	if (!sameAddr(from, s_host) || h.from > 2 || h.from == s_myId || !fresh(s_win[h.from], h.seq)) return;
	if (h.type == PONG && bn == (int)sizeof(PingPk))
	{
		PingPk pp; memcpy(&pp, body, sizeof pp);
		double sent = pp.sentLocal / 1000.0, rtt = (now - s_base) - sent;
		if (rtt >= 0 && rtt < 2)
		{
			double off = pp.hostT / 1000.0 + rtt / 2 - now;
			if (!s_haveClock) { s_clockOff = off; s_haveClock = true; }
			else s_clockOff += (off - s_clockOff) * 0.1;
		}
		return;
	}
	if (h.type == STATE && bn >= (int)sizeof(StatePk))
	{
		StatePk s; memcpy(&s, body, sizeof s);
		Peer &p = s_peers[h.from];
		if (!p.used) { memset(&p, 0, sizeof p); p.used = true; p.id = h.from; rt::fmt(p.name, sizeof p.name, "player %d", (int)h.from); NLOG("net: player %d is here", (int)h.from); }
		p.heard = now;
		push(p, t, s, body + sizeof s, bn - (int)sizeof s);
		if (h.from == 0) { s_hostHour = s.hour; s_hostWeather = s.weather; s_hostSnow = s.snow; s_haveSky = true; }
	}
	else if (h.type == LOOT && s_peers[h.from].used) storeLoot(s_peers[h.from], body, bn);
	else if (h.type == SKEL && s_peers[h.from].used) storeSkel(s_peers[h.from], body, bn);
	else if (h.type == LOOK && s_peers[h.from].used) storeLook(s_peers[h.from], body, bn);
	else if (h.type == MODS && s_peers[h.from].used) storeMods(s_peers[h.from], body, bn);
	else if (h.type == EVENTS2 && s_peers[h.from].used) storeEvents2(s_peers[h.from], body, bn);
	else if (h.type == EVACK && s_peers[h.from].used) storeAck(s_peers[h.from], body, bn, s_myId);
	else if (h.type == ENEMY && s_peers[h.from].used) storeEnemy(s_peers[h.from], t, body, bn);
	else if (h.type == FILEOFFER && h.from == 0) fileOffer(body, bn, now);
	else if (h.type == FILEPART && h.from == 0) filePart(body, bn);
	else if (h.type == BYE)
	{
		s_peers[h.from].used = false;
		if (h.from == 0) { s_joined = false; s_haveClock = false; for (auto &wi : s_win) wi.init = false; rt::strcpy(s_status, "the host left - waiting for them to come back", sizeof s_status); NLOG("net: %s", s_status); }
	}
	else if (h.type == ROSTER && h.from == 0 && bn == (int)sizeof(RosterPk))
	{
		RosterPk r; memcpy(&r, body, sizeof r);
		for (int i = 0; i < r.n && i < 3; i++)
		{
			u8 id = r.who[i].id;
			if (id > 2 || id == s_myId) continue;
			if (id >= 1 && r.who[i].epoch != s_epochSeen[id])
			{
				s_epochSeen[id] = r.who[i].epoch; s_win[id].init = false;
				Peer &q = s_peers[id]; q.evEpoch = q.evAckMine = 0; q.evAcks = false;
			}
			r.who[i].name[23] = 0;
			for (int k = 0; k < 23 && r.who[i].name[k]; k++) if ((u8)r.who[i].name[k] < 32 || (u8)r.who[i].name[k] > 126) r.who[i].name[k] = '?';
			Peer &p = s_peers[id];
			if (p.used && !rt::streq(p.name, r.who[i].name)) { NLOG("net: player %d is %s", (int)id, r.who[i].name); rt::strcpy(p.name, r.who[i].name, sizeof p.name); }
		}
	}
}

static double s_lastSend, s_lastHello, s_lastPing, s_lastStatus, s_lastLoot;
static Skel s_skel, s_enemySkel[kMaxEnemies];
static Enemy s_enemies[kMaxEnemies];
static int s_nEnemies;
enum { kMyEv = 4096 };
static Event s_myEv[kMyEv];
static double s_myEvT[kMyEv], s_myEvSent[kMyEv];
static int s_nMyEv;
static u32 s_evSeq;
static bool s_evDirty;
void setName(const char *name) { if (name && name[0] && !rt::streq(name, s_name)) { rt::strcpy(s_name, name, sizeof s_name); s_evDirty = true; NLOG("net: your name is %s", s_name); } }
void setSkel(const Skel &sk) { s_skel = sk; }
static u32 s_myMods[kMaxMods]; static int s_nMyMods;
void setMods(const u32 *hashes, int n) { if (n < 0) n = 0; if (n > kMaxMods) n = kMaxMods; memcpy(s_myMods, hashes, 4 * n); s_nMyMods = n; }
int peerMods(int id, u32 *out, int max)
{
	if (id < 0 || id > 3 || !s_peers[id].used || !s_peers[id].nMods) return 0;
	int n = s_peers[id].nMods < max ? s_peers[id].nMods : max;
	memcpy(out, s_peers[id].mods, 4 * n);
	return n;
}
static u8 s_look[kMaxLook]; static int s_lookLen;
void setLook(const void *b, int n) { if (n > 0 && n <= kMaxLook) { memcpy(s_look, b, n); s_lookLen = n; } }
int peerLook(int id, void *out, int max)
{
	if (id < 0 || id > 3 || !s_peers[id].used || !s_peers[id].lookLen || s_peers[id].lookLen > max) return 0;
	memcpy(out, s_peers[id].look, s_peers[id].lookLen);
	return s_peers[id].lookLen;
}
void setEnemies(const Enemy *e, int n) { if (n > kMaxEnemies) n = kMaxEnemies; memcpy(s_enemies, e, sizeof(Enemy) * n); s_nEnemies = n; }
void setEnemySkel(int slot, const Skel &sk) { if (slot >= 0 && slot < kMaxEnemies) s_enemySkel[slot] = sk; }
void addEvent(u8 type, const void *data, u8 len)
{
	if (len > sizeof(((Event *)0)->data)) return;
	if (!s_evEpoch) { startClock(); double t = rt::now(), f = t - 1048576.0 * (double)(int)(t / 1048576.0); s_evEpoch = (unixNow() * 2654435761u) ^ s_nonce ^ (u32)(int)(f * 2048.0); if (!s_evEpoch) s_evEpoch = 1; }
	if (s_nMyEv == kMyEv) { NLOG("net: 4096 of your events not yet had by a friend's game - the oldest is let go"); memmove(s_myEv, s_myEv + 1, sizeof(Event) * (kMyEv - 1)); memmove(s_myEvT, s_myEvT + 1, sizeof(double) * (kMyEv - 1)); memmove(s_myEvSent, s_myEvSent + 1, sizeof(double) * (kMyEv - 1)); s_nMyEv = kMyEv - 1; }
	Event &e = s_myEv[s_nMyEv];
	e.id = ++s_evSeq; e.type = type; e.len = len; memcpy(e.data, data, len);
	s_myEvT[s_nMyEv] = rt::now(); s_myEvSent[s_nMyEv] = 0;
	s_nMyEv++;
	s_evDirty = true;
}
bool peerSkel(int id, Skel *out)
{
	if (id < 0 || id > 3 || !s_peers[id].used || !s_peers[id].haveSkel) return false;
	*out = s_peers[id].skel;
	return true;
}
bool peerEnemySkel(int id, int slot, Skel *out)
{
	if (id < 0 || id > 3 || slot < 0 || slot >= kMaxEnemies || !s_peers[id].used || !s_peers[id].enemy[slot].haveSkel) return false;
	*out = s_peers[id].enemy[slot].skel;
	return true;
}
int peerEvents(int id, Event *out, int max)
{
	if (id < 0 || id > 3 || !s_peers[id].used) return 0;
	if (id == kSoloId && !s_solo) return 0;
	Inbox *q = inboxOf(s_peers[id].evEpoch);
	if (!q) return 0;
	int n = q->nEv < max ? q->nEv : max;
	memcpy(out, q->ev, sizeof(Event) * n);
	return n;
}
bool peekEvent(int id, Event *out)
{
	if (id < 0 || id > 3 || !s_peers[id].used) return false;
	if (id == kSoloId && !s_solo) return false;
	Inbox *ib = inboxOf(s_peers[id].evEpoch);
	if (!ib) return false;
	Inbox &p = *ib;
	u32 t = p.taken;
	if (t < p.skipTo && (!p.nEv || p.ev[0].id != t + 1)) t = p.nEv && p.ev[0].id - 1 < p.skipTo ? p.ev[0].id - 1 : p.skipTo;
	if (!p.nEv || p.ev[0].id != t + 1) return false;
	*out = p.ev[0];
	return true;
}
int takeEvents(int id, Event *out, int max)
{
	if (id < 0 || id > 3 || !s_peers[id].used) return 0;
	if (id == kSoloId && !s_solo) return 0;
	Inbox *ib = inboxOf(s_peers[id].evEpoch);
	if (!ib) return 0;
	Inbox &p = *ib;
	p.used = rt::now();
	int n = 0;
	for (;;)
	{
		if (p.taken < p.skipTo && (n >= p.nEv || p.ev[n].id != p.taken + 1))
			p.taken = n < p.nEv && p.ev[n].id - 1 < p.skipTo ? p.ev[n].id - 1 : p.skipTo;
		if (n >= max || n >= p.nEv || p.ev[n].id != p.taken + 1) break;
		out[n] = p.ev[n]; n++; p.taken++;
	}
	if (n) { memmove(p.ev, p.ev + n, sizeof(Event) * (p.nEv - n)); p.nEv -= n; }
	return n;
}
static float s_sendHz = 30;
void setSendHz(float hz) { s_sendHz = hz < 5 ? 5 : hz > 60 ? 60 : hz; }
static u8 s_lootPk[sizeof(LootHdr) + kMaxDrops * sizeof(DropPk) + kMaxTaken * sizeof(TakenPk)];
static int s_lootLen;
static bool s_lootDirty;

void setLoot(const Drop *mine, int n, const Taken *taken, int nt)
{
	if (n > kMaxDrops) n = kMaxDrops;
	if (nt > kMaxTaken) nt = kMaxTaken;
	u8 pk[sizeof s_lootPk];
	LootHdr lh = {(u8)n, (u8)nt};
	memcpy(pk, &lh, sizeof lh);
	int len = sizeof lh;
	for (int i = 0; i < n; i++)
	{
		DropPk d = {mine[i].id, mine[i].item.low, mine[i].item.modHash, mine[i].count, {mine[i].pos[0], mine[i].pos[1], mine[i].pos[2]}, mine[i].world, mine[i].cell};
		memcpy(pk + len, &d, sizeof d); len += sizeof d;
	}
	for (int i = 0; i < nt; i++) { TakenPk t = {taken[i].owner, taken[i].id}; memcpy(pk + len, &t, sizeof t); len += sizeof t; }
	if (len != s_lootLen || memcmp(pk, s_lootPk, len)) { memcpy(s_lootPk, pk, len); s_lootLen = len; s_lootDirty = true; }
}
int peerLoot(int id, Drop *out, int max, Taken *taken, int *nt, int maxT)
{
	*nt = 0;
	if (id < 0 || id > 3 || !s_peers[id].used || !s_peers[id].haveLoot) return -1;
	if (id == kSoloId && !s_solo) return -1;
	Peer &p = s_peers[id];
	int n = p.nDrops < max ? p.nDrops : max;
	memcpy(out, p.drops, sizeof(Drop) * n);
	*nt = p.nTaken < maxT ? p.nTaken : maxT;
	memcpy(taken, p.taken, sizeof(Taken) * *nt);
	return n;
}
void setBuild(u32 b) { s_build = b; }
u32 peerBuild(int id) { return id >= 0 && id < 4 && s_peers[id].used ? s_peers[id].build : 0; }
u8 myId() { return s_solo && s_mode == 0 ? 0 : s_myId; }
void hashName(const char *name, u32 *out)
{
	u32 h = 2166136261u;
	for (const char *c = name; *c; c++) { u8 ch = (u8)*c; if (ch >= 'A' && ch <= 'Z') ch += 32; h = (h ^ ch) * 16777619u; }
	*out = h;
}

void tick(double now, const Self &me)
{
	u8 buf[1024];
	sockaddr_in4 sa;
	int sl;
	if (s_sock != ~0u)
		for (int k = 0; k < 128; k++)
		{
			sl = sizeof sa;
			int n = recvfrom(s_sock, (char *)buf, sizeof buf, 0, &sa, &sl);
			if (n < 0 && WSAGetLastError() == 10035) break;
			if (n <= 0) continue;
			Addr a = {sa.addr, sa.port};
			handle(buf, n, a, now, false);
		}
	if (s_soloSock != ~0u)
		for (int k = 0; k < 64; k++)
		{
			sl = sizeof sa;
			int n = recvfrom(s_soloSock, (char *)buf, sizeof buf, 0, &sa, &sl);
			if (n <= 0) break;
			Addr a = {sa.addr, sa.port};
			handle(buf, n, a, now, true);
		}
	fileTick(now);
	u8 me1[sizeof(StatePk) + kMaxPoseBytes];
	int me1n = pack(me, me1);
	if (s_mode == 2 && !s_joined && now >= s_refusedUntil && now - s_lastHello > 0.5)
	{
		s_lastHello = now;
		HelloPk hp; memset(&hp, 0, sizeof hp);
		rt::strcpy(hp.name, s_name, sizeof hp.name);
		hp.build = s_build;
		hp.stamp = unixNow();
		hp.nonce = s_nonce;
		for (int i = 0; i < s_nHost; i++) sendRaw(s_hostAddrs[i], HELLO, &hp, sizeof hp, now, 0xFF);
	}
	if (s_mode == 2 && s_joined && now - s_lastPing > 1.0)
	{
		s_lastPing = now;
		PingPk pp = {ms32(now - s_base), 0};
		sendRaw(s_host, PING, &pp, sizeof pp, now, s_myId);
	}
	if (now - s_lastSend >= 1.0 / s_sendHz)
	{
		s_lastSend = now;
		if (s_mode == 1)
			for (int i = 1; i <= 2; i++) { if (s_peers[i].used) sendRaw(s_peers[i].addr, STATE, me1, me1n, now, 0); }
		else if (s_mode == 2 && s_joined && (s_haveClock || now - s_lastHello > 2))
			sendRaw(s_host, STATE, me1, me1n, now, s_myId);
		if (s_solo && s_soloSock != ~0u)
		{
			double saved = s_clockOff; int savedMode = s_mode;
			s_mode = 0;
			u32 sock = s_sock; s_sock = s_soloSock;
			sendRaw(s_soloAddr, STATE, me1, me1n, now, kSoloId);
			if (s_lootLen && (now - s_lastLoot > 1.0 || s_lootDirty)) sendRaw(s_soloAddr, LOOT, s_lootPk, s_lootLen, now, kSoloId);
			s_sock = sock; s_mode = savedMode; s_clockOff = saved;
		}
	}
	{
		static double lastSkel, lastEv, lastEnemy;
		auto toAll = [&](u8 type, const void *b, u32 len) {
			if (s_mode == 1) { for (int i = 1; i <= 2; i++) if (s_peers[i].used) sendRaw(s_peers[i].addr, type, b, len, now, 0); }
			else if (s_mode == 2 && s_joined) sendRaw(s_host, type, b, len, now, s_myId);
			if (s_solo && s_soloSock != ~0u)
			{
				int savedMode = s_mode; u32 sock = s_sock;
				s_mode = 0; s_sock = s_soloSock;
				sendRaw(s_soloAddr, type, b, len, now, kSoloId);
				s_sock = sock; s_mode = savedMode;
			}
		};
		if (now - lastSkel > 2.0)
		{
			lastSkel = now;
			u8 b[2 + kPoseBones * 8];
			for (int subj = 0; subj <= s_nEnemies; subj++)
			{
				const Skel &sk = subj == 0 ? s_skel : s_enemySkel[subj - 1];
				if (sk.n <= 0) continue;
				b[0] = (u8)subj; b[1] = (u8)sk.n;
				for (int i = 0; i < sk.n; i++)
				{
					u8 *e = b + 2 + 8 * i;
					memcpy(e, &sk.hash[i], 2);
					for (int k = 0; k < 3; k++) { float v = sk.t[i][k] * 16.0f; i16 c = (i16)(v > 32000 ? 32000 : v < -32000 ? -32000 : v); memcpy(e + 2 + 2 * k, &c, 2); }
				}
				toAll(SKEL, b, 2 + 8 * sk.n);
			}
			if (s_lookLen) toAll(LOOK, s_look, s_lookLen);
			for (int first = 0; s_nMyMods && first < s_nMyMods; )
			{
				u8 mb[kMaxBody];
				int n = s_nMyMods - first, cap = (kMaxBody - 5) / 4;
				if (n > cap) n = cap;
				u16 tot = (u16)s_nMyMods, f16 = (u16)first;
				memcpy(mb, &tot, 2); memcpy(mb + 2, &f16, 2); mb[4] = (u8)n;
				memcpy(mb + 5, s_myMods + first, 4 * n);
				toAll(MODS, mb, 5 + 4 * n);
				first += n;
			}
		}
		auto wanted = [&](int i) {
			for (int k = 0; k < 4; k++)
			{
				const Peer &q = s_peers[k];
				if (!q.used || k == kSoloId || k == (int)myId() || now - q.heard > 30) continue;
				if (!q.evAcks || q.evAckMine < s_myEv[i].id) return true;
			}
			return false;
		};
		{
			int drop = 0;
			while (drop < s_nMyEv && now - s_myEvT[drop] >= 5 && (now - s_myEvT[drop] > 600 || !wanted(drop))) drop++;
			if (drop)
			{
				memmove(s_myEv, s_myEv + drop, sizeof(Event) * (s_nMyEv - drop)); memmove(s_myEvT, s_myEvT + drop, sizeof(double) * (s_nMyEv - drop));
				memmove(s_myEvSent, s_myEvSent + drop, sizeof(double) * (s_nMyEv - drop)); s_nMyEv -= drop;
			}
		}
		if (s_evDirty || now - lastEv > 0.25)
		{
			bool named = s_evDirty;
			if (now - lastEv > 0.25) lastEv = now;
			s_evDirty = false;
			u8 b[kMaxBody];
			memset(b, 0, 33);
			rt::strcpy((char *)b, s_name, 24);
			u32 base = s_nMyEv ? s_myEv[0].id : s_evSeq + 1;
			memcpy(b + 24, &s_evEpoch, 4); memcpy(b + 28, &base, 4);
			int at = 33, n = 0;
			for (int pass = 0; pass < 2; pass++)
				for (int i = 0; i < s_nMyEv && n < 255; i++)
				{
					if (pass == 0 ? s_myEvSent[i] != 0 : (s_myEvSent[i] == 0 || now - s_myEvSent[i] < 0.5 || !wanted(i))) continue;
					const Event &e = s_myEv[i];
					if (at + 6 + e.len > (int)sizeof b) { pass = 2; break; }
					memcpy(b + at, &e.id, 4); b[at + 4] = e.type; b[at + 5] = e.len; memcpy(b + at + 6, e.data, e.len);
					at += 6 + e.len; n++;
					s_myEvSent[i] = now;
				}
			b[32] = (u8)n;
			static double lastHdr;
			if (n || named || now - lastHdr > 2) { lastHdr = now; toAll(EVENTS2, b, at); }
		}
		static double lastAck;
		if (now - lastAck > 0.2)
		{
			lastAck = now;
			u8 b[1 + 4 * 9]; int n = 0;
			for (int k = 0; k < 4; k++)
			{
				const Peer &q = s_peers[k];
				const Inbox *ib = q.used && k != kSoloId ? inboxOf(q.evEpoch) : 0;
				if (!ib) continue;
				u8 *e = b + 1 + n * 9;
				e[0] = (u8)k; memcpy(e + 1, &ib->epoch, 4); memcpy(e + 5, &ib->ackUpTo, 4);
				n++;
			}
			b[0] = (u8)n;
			if (n) toAll(EVACK, b, 1 + n * 9);
		}
		if (s_nEnemies && now - lastEnemy > 1.0 / 15)
		{
			lastEnemy = now;
			static u32 round;
			round++;
			for (int i = 0; i < s_nEnemies; i++)
			{
				if (i >= 4 && round % 3) continue;
				const Enemy &e = s_enemies[i];
				u8 b[sizeof(EnemyPk) + kMaxPoseBytes];
				EnemyPk pk = {(u8)i, (u8)s_nEnemies, e.ref.low, e.ref.modHash, {e.pos[0], e.pos[1], e.pos[2]}, e.yaw, e.health, e.flags, e.weapon.low, e.weapon.modHash, e.shots};
				memcpy(b, &pk, sizeof pk);
				int len = sizeof pk;
				if (e.hasPose && e.pose.n > 0) len += encodePose(e.pose, b + len);
				toAll(ENEMY, b, len);
			}
		}
		static int lastCount;
		if (!s_nEnemies && lastCount) { EnemyPk pk; memset(&pk, 0, sizeof pk); pk.slot = 0; pk.count = 0; pk.low = 0; toAll(ENEMY, &pk, sizeof pk); }
		lastCount = s_nEnemies;
	}
	if (s_lootLen && (now - s_lastLoot > 1.0 || s_lootDirty))
	{
		if (s_mode == 1) { for (int i = 1; i <= 2; i++) if (s_peers[i].used) sendRaw(s_peers[i].addr, LOOT, s_lootPk, s_lootLen, now, 0); }
		else if (s_mode == 2 && s_joined) sendRaw(s_host, LOOT, s_lootPk, s_lootLen, now, s_myId);
		s_lastLoot = now; s_lootDirty = false;
	}
	static double lastRoster;
	if (s_mode == 1 && now - lastRoster > 2)
	{
		lastRoster = now;
		RosterPk r; memset(&r, 0, sizeof r);
		r.who[r.n].id = 0; rt::strcpy(r.who[r.n].name, s_name, 24); r.n++;
		for (int i = 1; i <= 2; i++) if (s_peers[i].used) { r.who[r.n].id = (u8)i; r.who[r.n].epoch = s_epoch[i]; rt::strcpy(r.who[r.n].name, s_peers[i].name, 24); r.n++; }
		for (int i = 1; i <= 2; i++) if (s_peers[i].used) sendRaw(s_peers[i].addr, ROSTER, &r, sizeof r, now, 0);
	}
	for (int i = 0; i <= 2; i++)
		if (s_peers[i].used && i != s_myId && now - s_peers[i].heard > 15)
		{
			NLOG("net: %s timed out", s_peers[i].name);
			s_peers[i].used = false;
			if (i == 0 && s_mode == 2) { s_joined = false; s_haveClock = false; for (auto &wi : s_win) wi.init = false; rt::strcpy(s_status, "lost the host - trying again", sizeof s_status); }
		}
	for (auto &q : s_asking) if (q.used && now - q.last > 10) { NLOG("net: %s stopped asking to join", q.name); q.used = false; }
	if (s_mode && now - s_lastStatus > 30)
	{
		s_lastStatus = now;
		int others = 0;
		for (int i = 0; i <= 2; i++) if (s_peers[i].used && i != s_myId) others++;
		NLOG("net: %s; %d other player%s, clock offset %.3f s", s_status, others, others == 1 ? "" : "s", s_clockOff);
	}
}

static float lerpAngle(float a, float b, float k)
{
	float d = b - a;
	while (d > 3.14159265f) d -= 6.2831853f;
	while (d < -3.14159265f) d += 6.2831853f;
	return a + d * k;
}

static void blendPoses(const Pose &pa, const Pose &pb, float u, Pose &o)
{
	o = pb;
	for (int i = 0; i < pb.n; i++)
	{
		int j = (i < pa.n && pa.hash[i] == pb.hash[i]) ? i : -1;
		for (int k = 0; j < 0 && k < pa.n; k++) if (pa.hash[k] == pb.hash[i]) j = k;
		if (j < 0) continue;
		const float *qa = pa.q[j], *qb = pb.q[i];
		float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], sg = d < 0 ? -1.0f : 1.0f, l = 0;
		for (int k = 0; k < 4; k++) { o.q[i][k] = qa[k] * (1 - u) + qb[k] * sg * u; l += o.q[i][k] * o.q[i][k]; }
		l = rt::sqrt(l);
		if (l > 1e-6f) for (int k = 0; k < 4; k++) o.q[i][k] /= l;
		if (pa.hasT[j] && pb.hasT[i]) for (int k = 0; k < 3; k++) o.t[i][k] = pa.t[j][k] + (pb.t[i][k] - pa.t[j][k]) * u;
	}
}
static void snapPoseTo(const u8 *raw, int len, bool *has, Pose &o) { decodePose(raw, len, o); *has = o.n > 0; }

int peerEnemies(int id, Enemy *out, int max)
{
	if (id < 0 || id > 3 || !s_peers[id].used) return 0;
	if (id == kSoloId && !s_solo) return 0;
	double now = rt::now(), rt0 = (id == kSoloId ? now - s_base : hostNow(now)) - 0.1;
	int n = 0;
	for (int i = 0; i < kMaxEnemies && n < max; i++)
	{
		EnemySlot &es = s_peers[id].enemy[i];
		if (!es.used || es.n == 0 || now - es.heard > 2.0) continue;
		Enemy &o = out[n];
		int k = es.n - 1;
		while (k > 0 && es.snap[k - 1].t > rt0) k--;
		const ESnap &b = es.snap[k];
		const ESnap &a = k > 0 ? es.snap[k - 1] : b;
		float u = (k > 0 && rt0 < b.t && b.t > a.t) ? (float)((rt0 - a.t) / (b.t - a.t)) : 1.0f;
		if (u < 0) u = 0;
		o.ref.low = b.e.low; o.ref.modHash = b.e.modHash;
		float dx = b.e.pos[0] - a.e.pos[0], dy = b.e.pos[1] - a.e.pos[1];
		bool jump = dx * dx + dy * dy > 420.0f * 420.0f;
		for (int j = 0; j < 3; j++) o.pos[j] = jump ? b.e.pos[j] : a.e.pos[j] + (b.e.pos[j] - a.e.pos[j]) * u;
		o.yaw = jump ? b.e.yaw : lerpAngle(a.e.yaw, b.e.yaw, u);
		o.health = b.e.health; o.flags = b.e.flags; o.weapon.low = b.e.wlow; o.weapon.modHash = b.e.wmod; o.shots = b.e.shots;
		bool ha = false, hb = false;
		Pose pa;
		snapPoseTo(b.pose, b.poseLen, &hb, o.pose);
		if (hb && &a != &b && !jump && u < 1)
		{
			snapPoseTo(a.pose, a.poseLen, &ha, pa);
			if (ha) { Pose pb = o.pose; blendPoses(pa, pb, u, o.pose); }
		}
		o.hasPose = hb;
		n++;
	}
	return n;
}

int ghosts(Ghost *out, int max)
{
	double now = rt::now();
	int n = 0;
	for (int i = 0; i <= 3 && n < max; i++)
	{
		Peer &p = s_peers[i];
		if (!p.used || p.n == 0 || (i == s_myId && i != kSoloId && s_mode)) continue;
		if (i == kSoloId && !s_solo) continue;
		double rt0 = (i == kSoloId ? now - s_base : hostNow(now)) - 0.1;
		Ghost &g = out[n];
		memset(&g, 0, sizeof g);
		g.id = p.id;
		rt::strcpy(g.name, p.name, sizeof g.name);
		g.since = (float)(now - p.heard);
		g.fresh = g.since < 3;
		int k = p.n - 1;
		while (k > 0 && p.snap[k - 1].t > rt0) k--;
		const Snap &b = p.snap[k];
		if (k == 0 || rt0 >= b.t)
		{
			const Snap &a = k > 0 ? p.snap[k - 1] : b;
			float dt = (float)(b.t - a.t), ex = (float)(rt0 - b.t);
			if (ex > 0.25f) ex = 0.25f;
			if (ex < 0) ex = 0;
			g.s = unpack(b.s);
			if (dt > 0.001f && k > 0)
				for (int j = 0; j < 3; j++) { g.vel[j] = (b.s.pos[j] - a.s.pos[j]) / dt; g.s.pos[j] += g.vel[j] * ex; }
			if (b.poseLen) snapPoseTo(b.pose, b.poseLen, &g.s.hasPose, g.s.pose);
		}
		else
		{
			const Snap &a = p.snap[k - 1];
			float u = (float)((rt0 - a.t) / (b.t - a.t));
			float dx = b.s.pos[0] - a.s.pos[0], dy = b.s.pos[1] - a.s.pos[1], dz = b.s.pos[2] - a.s.pos[2];
			bool jump = dx * dx + dy * dy + dz * dz > 420.0f * 420.0f || a.s.mode != b.s.mode || a.s.cell != b.s.cell;
			g.s = unpack(b.s);
			if (!jump) g.s.mode = a.s.mode;
			for (int j = 0; j < 3; j++) g.s.pos[j] = jump ? b.s.pos[j] : a.s.pos[j] + (b.s.pos[j] - a.s.pos[j]) * u;
			float dt = (float)(b.t - a.t);
			for (int j = 0; j < 3; j++) g.vel[j] = dt > 0.001f ? (b.s.pos[j] - a.s.pos[j]) / dt : 0;
			if (!jump)
			{
				g.s.yaw = lerpAngle(a.s.yaw, b.s.yaw, u);
				g.s.pitch = a.s.pitch + (b.s.pitch - a.s.pitch) * u;
				g.s.roll = a.s.roll + (b.s.roll - a.s.roll) * u;
				g.s.speed = a.s.speed + (b.s.speed - a.s.speed) * u;
			}
			if (b.poseLen)
			{
				snapPoseTo(b.pose, b.poseLen, &g.s.hasPose, g.s.pose);
				static Pose pa, pb;
				bool ha = false;
				if (a.poseLen && !jump) snapPoseTo(a.pose, a.poseLen, &ha, pa);
				if (ha && g.s.hasPose) { pb = g.s.pose; blendPoses(pa, pb, u, g.s.pose); }
			}
		}
		n++;
	}
	return n;
}

bool hostHour(float *hour)
{
	if (s_mode != 2 || s_hostHour < 0) return false;
	*hour = s_hostHour;
	return true;
}

bool hostSky(u32 *weather, u8 *snow)
{
	if (s_mode != 2 || !s_haveSky) return false;
	*weather = s_hostWeather; *snow = s_hostSnow;
	return true;
}

bool asking(char *name, int cap, u32 *build, char *addr, int acap)
{
	Asking *a = 0;
	for (auto &q : s_asking) if (q.used && (!a || q.first < a->first)) a = &q;
	if (!a) return false;
	rt::strcpy(name, a->name[0] ? a->name : "someone", cap);
	if (build) *build = a->build;
	if (addr) { char at[32]; addrText(a->addr, at, sizeof at); for (int i = 0; at[i]; i++) if (at[i] == ':') at[i] = 0; rt::strcpy(addr, at, acap); }
	return true;
}
void answer(bool letIn)
{
	Asking *a = 0;
	for (auto &q : s_asking) if (q.used && (!a || q.first < a->first)) a = &q;
	if (!a) return;
	char at[32]; addrText(a->addr, at, sizeof at);
	if (letIn)
	{
		if (!hasIp(s_allowed, s_nAllowed, a->addr.ip)) { if (s_nAllowed == 16) { memmove(s_allowed, s_allowed + 1, 15 * sizeof(u32)); s_nAllowed = 15; } s_allowed[s_nAllowed++] = a->addr.ip; }
		NLOG("net: you let %s in (%s)", a->name, at);
	}
	else
	{
		if (!hasIp(s_refusedIp, s_nRefused, a->addr.ip)) { if (s_nRefused == 32) { memmove(s_refusedIp, s_refusedIp + 1, 31 * sizeof(u32)); s_nRefused = 31; } s_refusedIp[s_nRefused++] = a->addr.ip; }
		ctl(a->addr, REFUSED, a->nonce, 0, rt::now());
		NLOG("net: you refused %s (%s) - that address, for this session", a->name, at);
	}
	a->used = false;
}
bool peerPos(int id, float *pos, u32 *world, u32 *cell)
{
	if (id < 0 || id > 3 || !s_peers[id].used || s_peers[id].n == 0) return false;
	const Snap &b = s_peers[id].snap[s_peers[id].n - 1];
	memcpy(pos, b.s.pos, 12); *world = b.s.world; *cell = b.s.cell;
	return true;
}
void stopNow(const char *why)
{
	shutdown();
	for (int i = 0; i <= 2; i++) s_peers[i].used = false;
	for (auto &q : s_asking) q.used = false;
	s_mode = 0; s_joined = false;
	rt::fmt(s_status, sizeof s_status, "stopped (%s)", why);
	NLOG("net: %s", s_status);
}
void shutdown()
{
	if (s_sock == ~0u) return;
	double now = rt::now();
	if (s_mode == 1) { for (int i = 1; i <= 2; i++) if (s_peers[i].used) sendRaw(s_peers[i].addr, BYE, 0, 0, now, 0); }
	else if (s_joined) sendRaw(s_host, BYE, 0, 0, now, s_myId);
	closesocket(s_sock);
	s_sock = ~0u;
}
}
