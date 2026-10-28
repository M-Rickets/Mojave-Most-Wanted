#include "music.h"

// Windows' media session controls (Windows.Media.Control, Windows 10 1809+) through their raw WinRT interfaces, and the
// volume mixer (Core Audio sessions).
extern "C" {
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, u32, u32(__stdcall *)(void *), void *, u32, u32 *);
__declspec(dllimport) void __stdcall Sleep(u32);
__declspec(dllimport) long __stdcall CoCreateInstance(const void *, void *, u32, const void *, void **);
__declspec(dllimport) long __stdcall CoInitializeEx(void *, u32);
__declspec(dllimport) HANDLE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(HANDLE, const char *);
__declspec(dllimport) HANDLE __stdcall OpenProcess(u32, int, u32);
__declspec(dllimport) int __stdcall QueryFullProcessImageNameW(HANDLE, u32, wchar_t *, u32 *);
__declspec(dllimport) int __stdcall WideCharToMultiByte(u32, u32, const wchar_t *, int, char *, int, const char *, int *);
__declspec(dllimport) u32 __stdcall GetCurrentProcessId();
}

namespace music
{
static long(__stdcall *RoInitialize)(int);
static long(__stdcall *RoGetActivationFactory)(void *, const void *, void **);
static long(__stdcall *WindowsCreateString)(const wchar_t *, u32, void **);
static long(__stdcall *WindowsDeleteString)(void *);
static const wchar_t *(__stdcall *WindowsGetStringRawBuffer)(void *, u32 *);
static bool loadWinRT()
{
	HANDLE m = LoadLibraryA("combase.dll");
	if (!m) return false;
	RoInitialize = (long(__stdcall *)(int))GetProcAddress(m, "RoInitialize");
	RoGetActivationFactory = (long(__stdcall *)(void *, const void *, void **))GetProcAddress(m, "RoGetActivationFactory");
	WindowsCreateString = (long(__stdcall *)(const wchar_t *, u32, void **))GetProcAddress(m, "WindowsCreateString");
	WindowsDeleteString = (long(__stdcall *)(void *))GetProcAddress(m, "WindowsDeleteString");
	WindowsGetStringRawBuffer = (const wchar_t *(__stdcall *)(void *, u32 *))GetProcAddress(m, "WindowsGetStringRawBuffer");
	return RoInitialize && RoGetActivationFactory && WindowsCreateString && WindowsDeleteString && WindowsGetStringRawBuffer;
}

struct Guid { u32 a; u16 b, c; u8 d[8]; };
static const Guid kSessionManagerStatics = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
static const Guid kAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const Guid kDeviceEnumeratorClass = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const Guid kDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const Guid kAudioSessionManager2 = {0x77aa99a0, 0x1bd6, 0x484f, {0x8b, 0xc7, 0x2c, 0x65, 0x4c, 0x9a, 0x9b, 0x6f}};
static const Guid kAudioSessionControl2 = {0xbfb7ff88, 0x7239, 0x4fc9, {0x8f, 0xa2, 0x07, 0xc9, 0x50, 0xbe, 0x9c, 0x6d}};
static const Guid kSimpleAudioVolume = {0x87ce5498, 0x68d6, 0x44e5, {0x92, 0x15, 0x6d, 0xa4, 0x7e, 0xf8, 0x83, 0xd8}};
static const Guid kAudioMeterInformation = {0xc02216f6, 0x8c67, 0x4b5b, {0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64}};

template <typename... A> static long vc(void *o, int slot, A... a)
{
	typedef long(__stdcall * Fn)(void *, A...);
	return ((Fn)((*(void ***)o)[slot]))(o, a...);
}
static void rel(void *o) { if (o) vc(o, 2); }

static volatile long g_lock, g_started, g_active;
static void lock() { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void unlock() { __sync_lock_release(&g_lock); }
static State g_st = {-1, 0, 0, "", "", 0, -1, 0, "", 0, 0};
static int g_q[16];
static u32 g_qHead, g_qTail;

void command(int c)
{
	lock();
	if (g_qHead - g_qTail < 16) g_q[g_qHead++ & 15] = c;
	unlock();
}
static int nextCommand()
{
	lock();
	int c = g_qTail != g_qHead ? g_q[g_qTail++ & 15] : 0;
	unlock();
	return c;
}
void state(State *out)
{
	lock();
	*out = g_st;
	unlock();
}
void setActive(bool on) { g_active = on ? 1 : 0; }
static void copyz(char *d, const char *s, int cap)
{
	int i = 0;
	for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}
static void setNote(const char *s)
{
	lock();
	copyz(g_st.note, s, sizeof g_st.note);
	g_st.noteVer++;
	unlock();
}

static int wlen(const wchar_t *s) { int n = 0; while (s[n]) n++; return n; }
static bool weq(const wchar_t *a, const wchar_t *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void wcopy(wchar_t *d, const wchar_t *s, int cap)
{
	int i = 0;
	for (; s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}
static void lowerW(wchar_t *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (wchar_t)(*s + 32); }
static bool containsW(const wchar_t *hay, const wchar_t *needle)
{
	int n = wlen(needle);
	if (n < 3) return false;
	for (; *hay; hay++)
	{
		int i = 0;
		while (i < n && hay[i] == needle[i]) i++;
		if (i == n) return true;
	}
	return false;
}
static void hget(void *h, wchar_t *out, int cap)
{
	out[0] = 0;
	if (!h) return;
	u32 len = 0;
	const wchar_t *p = WindowsGetStringRawBuffer(h, &len);
	int i = 0;
	for (; p && i < (int)len && i < cap - 1; i++) out[i] = p[i];
	out[i] = 0;
	WindowsDeleteString(h);
}
static void narrow(const wchar_t *w, char *out, int cap)
{
	int r = WideCharToMultiByte(1252, 0, w, -1, out, cap, "?", 0);
	if (r <= 0) out[0] = 0;
	out[cap - 1] = 0;
}
static void appKey(const wchar_t *id, wchar_t *key, int cap)
{
	const wchar_t *p = id;
	for (const wchar_t *q = id; *q; q++) if (*q == '!' || *q == '\\') p = q + 1;
	wcopy(key, p, cap);
	lowerW(key);
	int n = wlen(key);
	if (n > 4 && key[n - 4] == '.' && key[n - 3] == 'e' && key[n - 2] == 'x' && key[n - 1] == 'e') key[n -= 4] = 0;
	bool hex = n == 16;
	for (int i = 0; hex && i < 16; i++) hex = (key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f');
	if (hex) wcopy(key, L"firefox", cap);
}
static void appName(const wchar_t *key, char *out, int cap)
{
	static const struct { const wchar_t *k; const char *name; } kNames[] = {
		{L"spotify", "Spotify"}, {L"chrome", "Chrome"}, {L"msedge", "Edge"}, {L"firefox", "Firefox"}, {L"brave", "Brave"},
		{L"opera", "Opera"}, {L"vivaldi", "Vivaldi"}, {L"vlc", "VLC"}, {L"zunemusic", "Media Player"}, {L"music.ui", "Media Player"},
		{L"applemusic", "Apple Music"}, {L"itunes", "iTunes"}, {L"tidal", "TIDAL"}, {L"deezer", "Deezer"}, {L"amazon", "Amazon Music"}};
	for (const auto &n : kNames)
		if (containsW(key, n.k)) { copyz(out, n.name, cap); return; }
	narrow(key, out, cap);
	if (out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

static bool await(void *op, int ms)
{
	void *info = 0;
	if (!op || vc(op, 0, &kAsyncInfo, &info) < 0 || !info) return false;
	int st = 0;
	for (int t = 0; t <= ms; t += 5)
	{
		if (vc(info, 7, &st) < 0 || st != 0) break;
		Sleep(5);
	}
	rel(info);
	return st == 1;
}
static void *g_mgr;
static bool getManager()
{
	const wchar_t *name = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
	void *h = 0, *f = 0, *op = 0;
	if (WindowsCreateString(name, (u32)wlen(name), &h) < 0) return false;
	long hr = RoGetActivationFactory(h, &kSessionManagerStatics, &f);
	WindowsDeleteString(h);
	if (hr < 0 || !f) return false;
	bool ok = vc(f, 6, &op) >= 0 && op && await(op, 5000);
	if (ok) ok = vc(op, 8, &g_mgr) >= 0 && g_mgr;
	rel(op);
	rel(f);
	return ok;
}
static int statusOf(void *s)
{
	void *pi = 0;
	int st = 0;
	if (vc(s, 9, &pi) >= 0 && pi) { vc(pi, 7, &st); rel(pi); }
	return st;
}
static void idOf(void *s, wchar_t *out, int cap)
{
	void *h = 0;
	out[0] = 0;
	if (vc(s, 6, &h) >= 0) hget(h, out, cap);
}
static wchar_t g_lastId[128];
static void *pickSession(int *count, int *status, wchar_t *id)
{
	*count = 0; *status = 0; id[0] = 0;
	void *vv = 0, *best = 0;
	int bestScore = -1;
	if (vc(g_mgr, 7, &vv) < 0 || !vv) return 0;
	u32 n = 0;
	vc(vv, 7, &n);
	*count = (int)n;
	for (u32 i = 0; i < n && i < 16; i++)
	{
		void *s = 0;
		if (vc(vv, 6, i, &s) < 0 || !s) continue;
		wchar_t sid[128];
		idOf(s, sid, 128);
		int st = statusOf(s);
		int score = (st == 4 ? 4 : 0) + (g_lastId[0] && weq(sid, g_lastId) ? 2 : 0) + (st == 5 ? 1 : 0);
		if (score > bestScore) { rel(best); best = s; bestScore = score; *status = st; wcopy(id, sid, 128); }
		else rel(s);
	}
	rel(vv);
	return best;
}
static void titleOf(void *s, wchar_t *out, int cap)
{
	out[0] = 0;
	void *op = 0, *p = 0;
	if (vc(s, 7, &op) < 0 || !op) return;
	if (await(op, 1500) && vc(op, 8, &p) >= 0 && p)
	{
		wchar_t t[160], a[96];
		void *h = 0;
		if (vc(p, 6, &h) >= 0) hget(h, t, 160); else t[0] = 0;
		h = 0;
		if (vc(p, 9, &h) >= 0) hget(h, a, 96); else a[0] = 0;
		int k = 0;
		if (a[0])
		{
			for (int i = 0; a[i] && k < cap - 4; i++) out[k++] = a[i];
			out[k++] = ' '; out[k++] = '-'; out[k++] = ' ';
		}
		for (int i = 0; t[i] && k < cap - 1; i++) out[k++] = t[i];
		out[k] = 0;
		rel(p);
	}
	rel(op);
}
static int press(void *s, int slot)
{
	void *op = 0;
	int took = -1;
	if (vc(s, slot, &op) >= 0 && op)
	{
		if (await(op, 1000)) { u8 b = 0; if (vc(op, 8, &b) >= 0) took = b ? 1 : 0; }
		rel(op);
	}
	return took;
}
static int pressAt(void *s, i64 pos)
{
	void *op = 0;
	int took = -1;
	if (vc(s, 24, pos, &op) >= 0 && op)
	{
		if (await(op, 1000)) { u8 b = 0; if (vc(op, 8, &b) >= 0) took = b ? 1 : 0; }
		rel(op);
	}
	return took;
}
static void controlsOf(void *s, bool *next, bool *prev, bool *seek)
{
	*next = *prev = *seek = false;
	void *pi = 0, *c = 0;
	if (vc(s, 9, &pi) < 0 || !pi) return;
	if (vc(pi, 6, &c) >= 0 && c)
	{
		u8 b = 0;
		if (vc(c, 12, &b) >= 0) *next = b != 0;
		b = 0;
		if (vc(c, 13, &b) >= 0) *prev = b != 0;
		b = 0;
		if (vc(c, 20, &b) >= 0) *seek = b != 0;
		rel(c);
	}
	rel(pi);
}
static i64 endOf(void *s)
{
	void *tl = 0;
	i64 end = 0, start = 0;
	if (vc(s, 8, &tl) >= 0 && tl) { vc(tl, 6, &start); vc(tl, 7, &end); rel(tl); }
	return end > start ? end : 0;
}

static bool exeOf(u32 pid, wchar_t *out, int cap)
{
	HANDLE h = OpenProcess(0x1000, 0, pid);
	if (!h) return false;
	wchar_t path[300];
	u32 n = 300;
	bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
	CloseHandle(h);
	if (!ok) return false;
	const wchar_t *b = path;
	for (const wchar_t *q = path; *q; q++) if (*q == '\\') b = q + 1;
	wcopy(out, b, cap);
	lowerW(out);
	int len = wlen(out);
	if (len > 4 && out[len - 4] == '.') out[len - 4] = 0;
	return true;
}
static int matchSessions(const wchar_t *key, void **out, int cap)
{
	void *en = 0, *dev = 0, *mgr = 0, *se = 0;
	int nh = 0;
	if (CoCreateInstance(&kDeviceEnumeratorClass, 0, 0x17, &kDeviceEnumerator, &en) < 0 || !en) return 0;
	if (vc(en, 4, 0, 1, &dev) >= 0 && dev &&
		vc(dev, 3, &kAudioSessionManager2, 0x17u, (void *)0, &mgr) >= 0 && mgr &&
		vc(mgr, 5, &se) >= 0 && se)
	{
		int n = 0;
		vc(se, 3, &n);
		void *loud = 0;
		float loudPeak = 0.001f;
		u32 self = GetCurrentProcessId();
		for (int i = 0; i < n && i < 64; i++)
		{
			void *c = 0, *c2 = 0, *m = 0;
			if (vc(se, 4, i, &c) < 0 || !c) continue;
			u32 pid = 0;
			if (vc(c, 0, &kAudioSessionControl2, &c2) >= 0 && c2) { vc(c2, 14, &pid); rel(c2); }
			wchar_t exe[96];
			if (!pid || pid == self || !exeOf(pid, exe, 96)) { rel(c); continue; }
			if (key[0] && (containsW(exe, key) || containsW(key, exe)) && nh < cap) { out[nh++] = c; continue; }
			float pk = 0;
			if (vc(c, 0, &kAudioMeterInformation, &m) >= 0 && m) { vc(m, 3, &pk); rel(m); }
			if (pk > loudPeak) { rel(loud); loud = c; loudPeak = pk; }
			else rel(c);
		}
		if (!nh && loud && cap > 0) { out[nh++] = loud; loud = 0; }
		rel(loud);
	}
	rel(se); rel(mgr); rel(dev); rel(en);
	return nh;
}
static int volumeStep(const wchar_t *key, int dir)
{
	void *hits[16];
	int nh = matchSessions(key, hits, 16);
	float v = -1;
	for (int k = 0; k < nh; k++)
	{
		void *sv = 0;
		if (vc(hits[k], 0, &kSimpleAudioVolume, &sv) >= 0 && sv)
		{
			if (v < 0)
			{
				float cur = 1;
				vc(sv, 4, &cur);
				v = (float)(int)(cur * 10 + 0.5f) / 10 + dir * 0.1f;
				v = v < 0 ? 0 : v > 1 ? 1 : v;
			}
			vc(sv, 3, v, (const Guid *)0);
			rel(sv);
		}
		rel(hits[k]);
	}
	return v >= 0 ? (int)(v * 100 + 0.5f) : -1;
}

static wchar_t g_meterKey[64];
static volatile u32 g_meterKeyVer;
static volatile float g_level;
float level() { return g_level; }
static u32 __stdcall meterThread(void *)
{
	CoInitializeEx(0, 0);
	void *meters[4];
	int nm = 0;
	u32 keyVer = ~0u;
	double lastScan = 0, last = rt::now();
	float slowMax = 0.05f, fast = 0, avg = 0;
	for (;;)
	{
		double t = rt::now();
		float dt = (float)rt::clamp((float)(t - last), 0.001f, 0.1f);
		last = t;
		if (!g_active)
		{
			for (int k = 0; k < nm; k++) rel(meters[k]);
			nm = 0; g_level = 0; fast = avg = 0; keyVer = ~0u;
			Sleep(200);
			continue;
		}
		if (keyVer != g_meterKeyVer || t - lastScan > 2.0)
		{
			wchar_t key[64];
			lock(); wcopy(key, g_meterKey, 64); keyVer = g_meterKeyVer; unlock();
			for (int k = 0; k < nm; k++) rel(meters[k]);
			nm = 0;
			void *ctl[4];
			int nc = matchSessions(key, ctl, 4);
			for (int k = 0; k < nc; k++)
			{
				void *m = 0;
				if (vc(ctl[k], 0, &kAudioMeterInformation, &m) >= 0 && m) meters[nm++] = m;
				rel(ctl[k]);
			}
			lastScan = t;
		}
		float pk = 0;
		for (int k = 0; k < nm; k++) { float v = 0; vc(meters[k], 3, &v); pk = rt::max(pk, v); }
		slowMax = rt::max(pk, slowMax * (1 - dt / 4.0f));
		if (slowMax < 0.02f) slowMax = 0.02f;
		float norm = pk / slowMax;
		fast = rt::max(norm, fast * (1 - dt / 0.08f));
		avg += (norm - avg) * rt::clamp(dt / 0.6f, 0, 1);
		float kick = rt::clamp((fast - avg) / rt::max(1 - avg, 0.15f), 0, 1);
		g_level = pk > 0.0005f ? rt::clamp(0.25f * fast + 0.9f * kick, 0, 1) : 0;
		Sleep(10);
	}
}

static u32 __stdcall worker(void *)
{
	if (!loadWinRT())
	{
		lock(); g_st.available = 0; unlock();
		setNote("music: no WinRT on this Windows (Windows 10 October 2018 update or newer needed)");
		return 0;
	}
	RoInitialize(1);
	if (!getManager())
	{
		lock(); g_st.available = 0; unlock();
		setNote("music: Windows' media controls aren't available (Windows 10 October 2018 update or newer needed)");
		return 0;
	}
	lock(); g_st.available = 1; unlock();
	setNote("music: Windows' media controls ready");
	wchar_t prevId[128] = {0}, prevTitle[192] = {0}, title[192] = {0};
	int prevStatus = -1, prevCount = -1;
	u32 tick = 0, titleTick = 0;
	for (;; tick++)
	{
		int cmd = nextCommand();
		if (!cmd && !g_active) { Sleep(250); continue; }
		int count = 0, st = 0;
		wchar_t id[128];
		void *s = pickSession(&count, &st, id);
		if (cmd)
		{
			wchar_t key[64];
			appKey(id, key, 64);
			char name[48], line[160];
			appName(key, name, sizeof name);
			if (cmd == CMD_VOLUP || cmd == CMD_VOLDOWN)
			{
				int v = volumeStep(key, cmd == CMD_VOLUP ? 1 : -1);
				lock(); g_st.volume = v; g_st.volVer++; unlock();
				rt::fmt(line, sizeof line, "music: %s volume -> %d%%", name, v);
			}
			else if (s)
			{
				static const int kSlot[5] = {0, 10, 11, 16, 17};
				static const char *kWhat[5] = {"", "play", "pause", "next", "previous"};
				bool hasNext, hasPrev, hasSeek;
				controlsOf(s, &hasNext, &hasPrev, &hasSeek);
				int took = press(s, kSlot[cmd]);
				const char *how = "";
				if ((cmd == CMD_NEXT || cmd == CMD_PREV) && took != 1 && hasSeek)
				{
					i64 end = endOf(s);
					if (cmd == CMD_NEXT && end > 20000000) { took = pressAt(s, end - 10000000); how = " (jumped to the end)"; }
					else if (cmd == CMD_PREV) { took = pressAt(s, 0); how = " (back to the start)"; }
				}
				titleTick = tick - 100;
				rt::fmt(line, sizeof line, "music: %s -> %s: %s%s (buttons: next %d, previous %d, position %d)", kWhat[cmd], name,
					took == 1 ? "done" : took == 0 ? "refused" : "no answer", how, hasNext, hasPrev, hasSeek);
			}
			else rt::fmt(line, sizeof line, "music: no player to send %d to", cmd);
			setNote(line);
			rel(s);
			Sleep(150);
			continue;
		}
		if (s) wcopy(g_lastId, id, 128);
		if (s && !weq(id, prevId))
		{
			wchar_t key[64];
			appKey(id, key, 64);
			lock(); wcopy(g_meterKey, key, 64); g_meterKeyVer++; unlock();
		}
		bool idChanged = !weq(id, prevId);
		if (!s) title[0] = 0;
		else if (idChanged || st != prevStatus || tick - titleTick >= 8) { titleOf(s, title, 192); titleTick = tick; }
		rel(s);
		wchar_t key[64];
		appKey(id, key, 64);
		char app[48], narrowTitle[160];
		if (id[0]) appName(key, app, sizeof app); else app[0] = 0;
		narrow(title, narrowTitle, sizeof narrowTitle);
		bool changed = idChanged || !weq(title, prevTitle);
		lock();
		g_st.sessions = count;
		g_st.status = st;
		if (changed) { copyz(g_st.app, app, sizeof g_st.app); copyz(g_st.title, narrowTitle, sizeof g_st.title); g_st.titleVer++; }
		g_st.polls++;
		unlock();
		if (changed || st != prevStatus || count != prevCount)
		{
			char line[160];
			rt::fmt(line, sizeof line, "music: %d player(s); %s status %d", count, id[0] ? app : "none", st);
			setNote(line);
			wcopy(prevId, id, 128);
			wcopy(prevTitle, title, 192);
			prevStatus = st; prevCount = count;
		}
		Sleep(200);
	}
}

void start()
{
	if (__sync_lock_test_and_set(&g_started, 1)) return;
	u32 tid;
	CreateThread(0, 256 * 1024, worker, 0, 0, &tid);
	CreateThread(0, 256 * 1024, meterThread, 0, 0, &tid);
}
}
