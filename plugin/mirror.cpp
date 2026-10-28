#include "mirror.h"

// The browser's window is copied with PrintWindow (PW_CLIENTONLY | PW_RENDERFULLCONTENT: Chromium browsers draw with
// the GPU, and only "render full content" gets their picture) into a DIB on a thread of its own.
extern "C" {
typedef int(__stdcall *EnumProc)(HANDLE, long);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, u32, u32(__stdcall *)(void *), void *, u32, u32 *);
__declspec(dllimport) void __stdcall Sleep(u32);
__declspec(dllimport) int __stdcall EnumWindows(EnumProc, long);
__declspec(dllimport) int __stdcall IsWindowVisible(HANDLE);
__declspec(dllimport) int __stdcall IsIconic(HANDLE);
__declspec(dllimport) int __stdcall IsWindow(HANDLE);
__declspec(dllimport) int __stdcall GetWindowTextW(HANDLE, wchar_t *, int);
__declspec(dllimport) int __stdcall GetClientRect(HANDLE, int *);
__declspec(dllimport) int __stdcall PrintWindow(HANDLE, HANDLE, u32);
__declspec(dllimport) HANDLE __stdcall GetDC(HANDLE);
__declspec(dllimport) int __stdcall ReleaseDC(HANDLE, HANDLE);
__declspec(dllimport) HANDLE __stdcall CreateCompatibleDC(HANDLE);
__declspec(dllimport) HANDLE __stdcall CreateDIBSection(HANDLE, const void *, u32, void **, HANDLE, u32);
__declspec(dllimport) HANDLE __stdcall SelectObject(HANDLE, HANDLE);
__declspec(dllimport) int __stdcall DeleteObject(HANDLE);
__declspec(dllimport) int __stdcall DeleteDC(HANDLE);
__declspec(dllimport) int __stdcall GdiFlush();
__declspec(dllimport) HANDLE __stdcall OpenProcess(u32, int, u32);
__declspec(dllimport) int __stdcall QueryFullProcessImageNameW(HANDLE, u32, wchar_t *, u32 *);
__declspec(dllimport) u32 __stdcall GetCurrentProcessId();
__declspec(dllimport) int __stdcall WideCharToMultiByte(u32, u32, const wchar_t *, int, char *, int, const char *, int *);
__declspec(dllimport) int __stdcall GetWindowRect(HANDLE, int *);
__declspec(dllimport) HANDLE __stdcall MonitorFromWindow(HANDLE, u32);
__declspec(dllimport) int __stdcall GetMonitorInfoW(HANDLE, void *);
__declspec(dllimport) int __stdcall SetWindowPos(HANDLE, HANDLE, int, int, int, int, u32);
__declspec(dllimport) long __stdcall GetWindowLongA(HANDLE, int);
__declspec(dllimport) long __stdcall SetWindowLongA(HANDLE, int, long);
}

namespace mirror
{
static const int OW = 256, OH = 128;
static volatile long g_lock, g_started, g_active;
static void lock() { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void unlock() { __sync_lock_release(&g_lock); }
static u32 *g_out;
static volatile u32 g_outVer;
static volatile int g_status = NONE;
static char g_title[160];
static u32 __stdcall worker(void *);
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

struct GamePick { HANDLE best; int area; u32 self; };
static int __stdcall gameEnum(HANDLE h, long lp)
{
	GamePick &p = *(GamePick *)lp;
	u32 pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid != p.self || !IsWindowVisible(h)) return 1;
	int rc[4] = {0, 0, 0, 0};
	GetWindowRect(h, rc);
	int area = (rc[2] - rc[0]) * (rc[3] - rc[1]);
	if (area > p.area) { p.best = h; p.area = area; }
	return 1;
}
struct MonInfo { u32 size; int mon[4], work[4]; u32 flags; };
static HANDLE gameWindow(int wr[4], MonInfo *mi)
{
	GamePick p = {0, 0, GetCurrentProcessId()};
	EnumWindows(gameEnum, (long)&p);
	if (!p.best) return 0;
	GetWindowRect(p.best, wr);
	mi->size = 40;
	HANDLE m = MonitorFromWindow(p.best, 2 /* nearest */);
	if (!m || !GetMonitorInfoW(m, mi)) return 0;
	return p.best;
}
void gameWindowInfo(char *out, int cap)
{
	int wr[4] = {0, 0, 0, 0};
	MonInfo mi = {40, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
	HANDLE g = gameWindow(wr, &mi);
	if (!g) { rt::fmt(out, cap, "game window not found"); return; }
	u32 st = (u32)GetWindowLongA(g, -16);
	bool covers = wr[0] <= mi.mon[0] && wr[1] <= mi.mon[1] && wr[2] >= mi.mon[2] && wr[3] >= mi.mon[3];
	rt::fmt(out, cap, "game window %dx%d at %d,%d (style %x: %s), monitor %dx%d%s", wr[2] - wr[0], wr[3] - wr[1], wr[0], wr[1], st,
		(st & 0x00C00000) == 0x00C00000 ? "title bar" : (st & 0x80000000) ? "popup" : "plain", mi.mon[2] - mi.mon[0], mi.mon[3] - mi.mon[1],
		covers ? ", covers it" : "");
}
bool makeBorderless(char *out, int cap)
{
	int wr[4] = {0, 0, 0, 0};
	MonInfo mi = {40, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
	HANDLE g = gameWindow(wr, &mi);
	if (!g) return false;
	u32 st = (u32)GetWindowLongA(g, -16), ex = (u32)GetWindowLongA(g, -20);
	if ((st & 0x00C00000) != 0x00C00000)
	{
		rt::fmt(out, cap, "borderless: the game's window has no title bar (style %x) - the game is set to full screen, not windowed", st);
		return true;
	}
	SetWindowLongA(g, -16, (long)((st & ~(0x00C00000u | 0x00040000u | 0x00080000u | 0x00020000u | 0x00010000u)) | 0x80000000u));
	SetWindowLongA(g, -20, (long)(ex & ~(0x1u | 0x100u | 0x200u | 0x20000u)));
	SetWindowPos(g, 0, mi.mon[0], mi.mon[1], mi.mon[2] - mi.mon[0], mi.mon[3] - mi.mon[1], 0x0020 | 0x0040);
	rt::fmt(out, cap, "borderless: the game's window now fills the %dx%d monitor (was %dx%d)", mi.mon[2] - mi.mon[0], mi.mon[3] - mi.mon[1],
		wr[2] - wr[0], wr[3] - wr[1]);
	return true;
}
void setActive(bool on)
{
	g_active = on ? 1 : 0;
	if (on && !__sync_lock_test_and_set(&g_started, 1))
	{
		u32 tid;
		CreateThread(0, 256 * 1024, worker, 0, 0, &tid);
	}
}
static char g_note[160];
static volatile u32 g_noteVer;
static void note(const char *msg)
{
	lock();
	rt::fmt(g_note, sizeof g_note, "%s", msg);
	g_noteVer++;
	unlock();
}
bool lastNote(char *out, int cap, u32 *ver)
{
	bool fresh = false;
	lock();
	if (*ver != g_noteVer) { rt::fmt(out, cap, "%s", g_note); *ver = g_noteVer; fresh = true; }
	unlock();
	return fresh;
}
int frame(u32 *px, int w, int h, u32 *ver)
{
	int st = g_status;
	if (w != OW || h != OH || !g_out) return st;
	lock();
	if (*ver != g_outVer) { memcpy(px, g_out, OW * OH * 4); *ver = g_outVer; }
	unlock();
	return st;
}
const char *windowTitle() { return g_title; }

static void lowerW(wchar_t *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (wchar_t)(*s + 32); }
static bool hasW(const wchar_t *hay, const wchar_t *needle)
{
	for (; *hay; hay++)
	{
		int i = 0;
		while (needle[i] && hay[i] == needle[i]) i++;
		if (!needle[i]) return true;
	}
	return false;
}
static bool browser(u32 pid)
{
	HANDLE h = OpenProcess(0x1000, 0, pid);
	if (!h) return false;
	wchar_t path[300];
	u32 n = 300;
	bool ok = QueryFullProcessImageNameW(h, 0, path, &n) != 0;
	CloseHandle(h);
	if (!ok) return false;
	lowerW(path);
	return hasW(path, L"opera") || hasW(path, L"chrome") || hasW(path, L"msedge") || hasW(path, L"firefox") ||
		hasW(path, L"brave") || hasW(path, L"vivaldi");
}
struct Pick { HANDLE best; int score; u32 self; };
static int __stdcall enumProc(HANDLE h, long lp)
{
	Pick &p = *(Pick *)lp;
	if (!IsWindowVisible(h)) return 1;
	u32 pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (!pid || pid == p.self) return 1;
	wchar_t t[256];
	int n = GetWindowTextW(h, t, 256);
	if (n < 0) n = 0;
	t[n < 255 ? n : 255] = 0;
	lowerW(t);
	int rc[4] = {0, 0, 0, 0};
	GetClientRect(h, rc);
	int w = rc[2] - rc[0], hh = rc[3] - rc[1];
	bool icon = IsIconic(h) != 0;
	if (!icon && (w < 160 || hh < 90)) return 1;
	int score = 0;
	if (hasW(t, L"youtube")) score += 3;
	if (hasW(t, L"picture in picture") || hasW(t, L"picture-in-picture") || hasW(t, L"pop out") || hasW(t, L"popout") || hasW(t, L"video pop")) score += 4;
	bool br = browser(pid);
	if (br && !n && !icon && hh > 0 && w * 10 >= hh * 15 && w * 10 <= hh * 20) score += 3;
	if (!score) return 1;
	if (br) score += 1;
	if (icon) score -= 1;
	if (score > p.score) { p.best = h; p.score = score; }
	return 1;
}
static HANDLE findWindow()
{
	Pick p = {0, 0, GetCurrentProcessId()};
	EnumWindows(enumProc, (long)&p);
	return p.best;
}

static HANDLE g_dc, g_bmp, g_oldBmp;
static u32 *g_bits;
static int g_bw, g_bh;
static bool ensureDib(int w, int h)
{
	if (g_bits && w == g_bw && h == g_bh) return true;
	if (!g_dc)
	{
		HANDLE screen = GetDC(0);
		g_dc = CreateCompatibleDC(screen);
		ReleaseDC(0, screen);
		if (!g_dc) return false;
	}
	if (g_bmp) { SelectObject(g_dc, g_oldBmp); DeleteObject(g_bmp); g_bmp = 0; g_bits = 0; }
	struct { u32 size; i32 w, h; u16 planes, bits; u32 comp, image, xppm, yppm, used, important; } bi = {40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0};
	void *bits = 0;
	g_bmp = CreateDIBSection(g_dc, &bi, 0, &bits, 0, 0);
	if (!g_bmp || !bits) { g_bmp = 0; return false; }
	g_oldBmp = SelectObject(g_dc, g_bmp);
	g_bits = (u32 *)bits; g_bw = w; g_bh = h;
	return true;
}

static const int AW = 192, AHMAX = 192;
static u8 g_prev[AW * AHMAX], g_chg[AW * AHMAX];
static float g_acc[AW * AHMAX];
static int g_ah, g_chgN;
static float g_lumMean;
static bool g_primed;
static inline int lum(u32 c) { return (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10; }
static void motion(int w, int h)
{
	int ah = h * AW / w;
	if (ah > AHMAX) ah = AHMAX;
	if (ah < 8) ah = 8;
	if (ah != g_ah) { memset(g_acc, 0, sizeof g_acc); memset(g_prev, 0, sizeof g_prev); g_ah = ah; g_primed = false; }
	g_chgN = 0;
	long lsum = 0;
	for (int y = 0; y < ah; y++)
		for (int x = 0; x < AW; x++)
		{
			int sx = (x * 2 + 1) * w / (AW * 2), sy = (y * 2 + 1) * h / (ah * 2);
			int l = lum(g_bits[sy * w + sx]), i = y * AW + x;
			int d = l - g_prev[i];
			g_prev[i] = (u8)l;
			lsum += l;
			bool c = g_primed && (d > 8 || d < -8);
			g_chg[i] = c ? 1 : 0;
			g_chgN += c ? 1 : 0;
			g_acc[i] = g_acc[i] * 0.93f + (c ? 1.0f : 0.0f);
		}
	g_lumMean = (float)lsum / (float)(AW * ah);
	g_primed = true;
}
static void run(const float *v, int n, float thresh, int maxGap, int *a, int *b)
{
	int bestA = 0, bestB = -1, s = -1, gap = 0, last = -1;
	for (int i = 0; i <= n; i++)
	{
		bool on = i < n && v[i] > thresh;
		if (on) { if (s < 0) s = i; last = i; gap = 0; }
		else if (s >= 0 && (++gap > maxGap || i == n))
		{
			if (last - s > bestB - bestA) { bestA = s; bestB = last; }
			s = -1; gap = 0;
		}
	}
	*a = bestA; *b = bestB;
}
static bool movingBox(int w, int h, int box[4])
{
	float col[AW], row[AHMAX];
	memset(col, 0, sizeof col); memset(row, 0, sizeof row);
	for (int y = 0; y < g_ah; y++)
		for (int x = 0; x < AW; x++)
			if (g_acc[y * AW + x] > 1.5f) { col[x] += 1; row[y] += 1; }
	float cm = 0, rm = 0;
	for (int x = 0; x < AW; x++) cm = rt::max(cm, col[x]);
	for (int y = 0; y < g_ah; y++) rm = rt::max(rm, row[y]);
	if (cm < 3 || rm < 3) return false;
	int x0, x1, y0, y1;
	run(col, AW, cm * 0.25f, 1, &x0, &x1);
	run(row, g_ah, rm * 0.25f, 1, &y0, &y1);
	if (x1 - x0 < 8 || y1 - y0 < 5) return false;
	box[0] = x0 * w / AW; box[1] = y0 * h / g_ah; box[2] = (x1 + 1) * w / AW; box[3] = (y1 + 1) * h / g_ah;
	return true;
}

static bool cutBox(int w, int h, int box[4])
{
	if (g_chgN * 100 < AW * g_ah * 2) return false;
	float col[AW], row[AHMAX];
	memset(col, 0, sizeof col); memset(row, 0, sizeof row);
	for (int y = 0; y < g_ah; y++)
		for (int x = 0; x < AW; x++)
			if (g_chg[y * AW + x]) { col[x] += 1; row[y] += 1; }
	float cm = 0, rm = 0;
	for (int x = 0; x < AW; x++) cm = rt::max(cm, col[x]);
	for (int y = 0; y < g_ah; y++) rm = rt::max(rm, row[y]);
	int x0, x1, y0, y1;
	run(col, AW, cm * 0.3f, 1, &x0, &x1);
	run(row, g_ah, rm * 0.3f, 1, &y0, &y1);
	int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
	if (bw < 16 || bh < 9) return false;
	int n = 0;
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++) n += g_chg[y * AW + x];
	if (n * 10 < bw * bh * 6) return false;
	box[0] = x0 * w / AW; box[1] = y0 * h / g_ah; box[2] = (x1 + 1) * w / AW; box[3] = (y1 + 1) * h / g_ah;
	return true;
}

static u32 g_tmp[OW * OH];
static float g_dark;
static void output(int w, int h, const int box[4])
{
	float bw = (float)(box[2] - box[0]), bh = (float)(box[3] - box[1]);
	const float screenAspect = 1.89f;
	float a = bw / rt::max(bh, 1.0f), fw = 1, fh = 1;
	if (a > screenAspect) fh = screenAspect / a; else fw = a / screenAspect;
	int ow = (int)(OW * fw), oh = (int)(OH * fh), ox = (OW - ow) / 2, oy = (OH - oh) / 2;
	long sum = 0;
	for (int i = 0; i < OW * OH; i++) g_tmp[i] = 0xFF000000u;
	for (int y = 0; y < oh; y++)
	{
		float sy0 = box[1] + bh * y / oh, sy1 = box[1] + bh * (y + 1) / oh;
		for (int x = 0; x < ow; x++)
		{
			float sx0 = box[0] + bw * x / ow, sx1 = box[0] + bw * (x + 1) / ow;
			u32 r = 0, g = 0, b = 0;
			for (int k = 0; k < 4; k++)
			{
				int sx = (int)(sx0 + (sx1 - sx0) * ((k & 1) ? 0.75f : 0.25f)), sy = (int)(sy0 + (sy1 - sy0) * ((k & 2) ? 0.75f : 0.25f));
				if (sx >= w) sx = w - 1;
				if (sy >= h) sy = h - 1;
				u32 c = g_bits[sy * w + sx];
				r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
			}
			r >>= 2; g >>= 2; b >>= 2;
			sum += r + g + b;
			g_tmp[(oy + y) * OW + ox + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
		}
	}
	g_dark = (float)sum / (3.0f * (float)imax(ow * oh, 1));
	lock();
	memcpy(g_out, g_tmp, sizeof g_tmp);
	g_outVer++;
	unlock();
}

static const int kBoxes = 40;
static const double kBoxKeep = 20.0;
static int g_box[kBoxes][4];
static double g_boxT[kBoxes];
static int g_boxN, g_boxAt;
static int g_crop[4], g_cut[4];
static bool g_haveCut;
static double g_searchSince, g_lastBox, g_holdUntil;
static int g_resets, g_cuts;
static float g_outsideMoved;
static void boxesClear() { g_boxN = 0; g_boxAt = 0; }
static void boxesAdd(const int b[4], double t)
{
	memcpy(g_box[g_boxAt], b, sizeof g_box[0]);
	g_boxT[g_boxAt] = t;
	g_boxAt = (g_boxAt + 1) % kBoxes;
	if (g_boxN < kBoxes) g_boxN++;
}
static int area(const int b[4]) { return (b[2] - b[0]) * (b[3] - b[1]); }
static int overlap(const int a[4], const int b[4])
{
	int x0 = imax(a[0], b[0]), y0 = imax(a[1], b[1]), x1 = imin(a[2], b[2]), y1 = imin(a[3], b[3]);
	return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}
static void cropUpdate(double t)
{
	int u[4] = {0, 0, 0, 0};
	bool any = g_haveCut;
	if (any) memcpy(u, g_cut, sizeof u);
	for (int i = 0; i < g_boxN; i++)
	{
		if (t - g_boxT[i] > kBoxKeep) continue;
		const int *b = g_box[i];
		if (!any) { memcpy(u, b, sizeof u); any = true; }
		else { u[0] = imin(u[0], b[0]); u[1] = imin(u[1], b[1]); u[2] = imax(u[2], b[2]); u[3] = imax(u[3], b[3]); }
	}
	if (any) memcpy(g_crop, u, sizeof u);
}
static void trackReset(double t)
{
	g_crop[0] = g_crop[1] = g_crop[2] = g_crop[3] = 0;
	g_haveCut = false;
	boxesClear();
	memset(g_acc, 0, sizeof g_acc);
	g_searchSince = t;
	g_holdUntil = 0;
}
static void track(int w, int h, double t)
{
	int cells = AW * g_ah, cb[4], before[4];
	memcpy(before, g_crop, sizeof before);
	if (t < g_holdUntil)
	{
		if (t + 0.06 >= g_holdUntil) memset(g_acc, 0, sizeof g_acc);
		return;
	}
	const int startMin = w * h / 50;
	bool cut = cutBox(w, h, cb);
	if (cut && area(cb) * 100 > w * h * 85) cut = false;
	if (cut)
	{
		g_cuts++;
		bool inside = g_crop[2] && overlap(cb, g_crop) * 10 >= area(cb) * 7;
		if (inside && g_haveCut)
		{
			g_cut[0] = imin(g_cut[0], cb[0]); g_cut[1] = imin(g_cut[1], cb[1]); g_cut[2] = imax(g_cut[2], cb[2]); g_cut[3] = imax(g_cut[3], cb[3]);
			cropUpdate(t);
		}
		else if (inside || (!g_crop[2] && area(cb) >= startMin) || area(cb) * 20 > w * h)
		{
			if (g_crop[2] && !inside) boxesClear();
			memcpy(g_cut, cb, sizeof g_cut);
			g_haveCut = true;
			cropUpdate(t);
		}
	}
	if (t - g_lastBox > 0.5)
	{
		g_lastBox = t;
		int b[4];
		if (movingBox(w, h, b))
		{
			if (!g_crop[2]) { if (area(b) >= startMin) boxesAdd(b, t); }
			else if (overlap(b, g_crop) * 10 >= area(b) * 7) boxesAdd(b, t);
			else if (area(b) * 10 > w * h) { boxesClear(); g_haveCut = false; boxesAdd(b, t); }
		}
		cropUpdate(t);
	}
	bool page;
	if (g_crop[2])
	{
		int cx0 = g_crop[0] * AW / w, cx1 = g_crop[2] * AW / w, cy0 = g_crop[1] * g_ah / h, cy1 = g_crop[3] * g_ah / h;
		int mx = imax(1, (cx1 - cx0) / 12), my = imax(1, (cy1 - cy0) / 12);
		int x0 = cx0 - mx, x1 = cx1 + mx, y0 = cy0 - my, y1 = cy1 + my;
		int out = 0, outN = 0;
		for (int y = 0; y < g_ah; y++)
			for (int x = 0; x < AW; x++)
				if (x < x0 || x > x1 || y < y0 || y > y1) { outN++; out += g_acc[y * AW + x] > 0.5f ? 1 : 0; }
		g_outsideMoved = outN ? (float)out / (float)outN : 0;
		page = outN * 20 > cells && out * 5 > outN;
	}
	else page = !cut && g_chgN * 100 >= cells * 35;
	if (page)
	{
		if (g_crop[2])
		{
			char m[100];
			rt::fmt(m, sizeof m, "the page changed (%d%% around the video): finding the video again", (int)(g_outsideMoved * 100));
			note(m);
		}
		trackReset(t);
		g_holdUntil = t + 0.75;
		g_resets++;
		return;
	}
	if (g_crop[2] && (rt::absf((float)(g_crop[0] - before[0])) + rt::absf((float)(g_crop[1] - before[1])) +
		rt::absf((float)(g_crop[2] - before[2])) + rt::absf((float)(g_crop[3] - before[3]))) > 0.02f * (float)(w + h))
	{
		char m[160];
		rt::fmt(m, sizeof m, "video at %d,%d %dx%d in the %dx%d window%s", g_crop[0], g_crop[1], g_crop[2] - g_crop[0], g_crop[3] - g_crop[1],
			w, h, g_haveCut ? " (scene cut)" : "");
		note(m);
	}
}
static bool fullScreen(HANDLE wnd)
{
	int wr[4] = {0, 0, 0, 0};
	struct { u32 size; int mon[4], work[4]; u32 flags; } mi = {40, {0, 0, 0, 0}, {0, 0, 0, 0}, 0};
	HANDLE m = MonitorFromWindow(wnd, 2 /* nearest */);
	if (!m || !GetMonitorInfoW(m, &mi) || !GetWindowRect(wnd, wr)) return false;
	return wr[0] <= mi.mon[0] && wr[1] <= mi.mon[1] && wr[2] >= mi.mon[2] && wr[3] >= mi.mon[3];
}

static u32 __stdcall worker(void *)
{
	g_out = (u32 *)rt::alloc(OW * OH * 4);
	HANDLE wnd = 0;
	double lastFind = -10, darkSince = -1;
	int cropW = 0, cropH = 0;
	bool wasFull = false;
	double sumT = 0;
	int copies = 0, still = 0;
	float chgSum = 0, lumSum = 0;
	for (;;)
	{
		if (!g_active) { Sleep(100); continue; }
		double t = rt::now();
		if (!wnd || !IsWindow(wnd) || t - lastFind > 3.0)
		{
			HANDLE w = findWindow();
			lastFind = t;
			if (w != wnd)
			{
				wnd = w;
				trackReset(t);
				if (wnd)
				{
					wchar_t tw[160];
					int n = GetWindowTextW(wnd, tw, 159);
					tw[n > 0 ? n : 0] = 0;
					if (WideCharToMultiByte(1252, 0, tw, -1, g_title, sizeof g_title, "?", 0) <= 0) g_title[0] = 0;
				}
				else g_title[0] = 0;
			}
		}
		if (!wnd) { g_status = NO_WINDOW; Sleep(250); continue; }
		if (IsIconic(wnd)) { g_status = MINIMIZED; Sleep(250); continue; }
		int rc[4] = {0, 0, 0, 0};
		GetClientRect(wnd, rc);
		int w = rc[2] - rc[0], h = rc[3] - rc[1];
		if (w < 32 || h < 32 || w > 7680 || h > 4320 || !ensureDib(w, h)) { g_status = NO_WINDOW; Sleep(250); continue; }
		if (!PrintWindow(wnd, g_dc, 3)) { g_status = BLACK; Sleep(100); continue; }
		GdiFlush();
		motion(w, h);
		copies++;
		chgSum += (float)g_chgN * 100.0f / (float)(AW * g_ah);
		lumSum += g_lumMean;
		if (!g_chgN) still++;
		if (t - sumT >= 5.0)
		{
			char m[160];
			if (copies > 1 && sumT > 0)
			{
				rt::fmt(m, sizeof m, "%.0f copies/s, %.1f%% changing, %d%% of copies unchanged, brightness %.0f, %s, cuts %d, page changes %d",
					copies / (t - sumT), chgSum / copies, still * 100 / copies, lumSum / copies,
					wasFull ? "browser full screen" : g_crop[2] ? "video found" : "video not found", g_cuts, g_resets);
				note(m);
			}
			sumT = t; copies = still = 0; chgSum = lumSum = 0; g_cuts = g_resets = 0;
		}
		if (w != cropW || h != cropH) { trackReset(t); cropW = w; cropH = h; }
		int full[4] = {0, 0, w, h};
		bool fs = fullScreen(wnd);
		if (fs != wasFull) { wasFull = fs; if (fs) note("the browser is full screen: showing all of it"); }
		const int *show = full;
		if (!fs)
		{
			track(w, h, t);
			if (!g_crop[2] && t - g_searchSince < 4.0) { g_status = SEARCHING; Sleep(40); continue; }
			if (g_crop[2]) show = g_crop;
		}
		output(w, h, show);
		if (g_dark < 4) { if (darkSince < 0) darkSince = t; }
		else darkSince = -1;
		g_status = darkSince >= 0 && t - darkSince > 1.5 ? BLACK : OK;
		double spent = rt::now() - t;
		int wait = (int)((0.05 - spent) * 1000);
		Sleep(wait > 5 ? (u32)wait : 5);
	}
}
}
