#include "screen.h"
#include "font5x7.h"

namespace screen
{
static float g_bar[kBars], g_peak[kBars], g_hold[kBars], g_noise[kBars], g_noiseTo[kBars], g_noiseT, g_scroll;
static u32 g_seed = 0x2545F491;
static int g_cw = W, g_ch = H;
static float rnd01()
{
	g_seed = g_seed * 1664525u + 1013904223u;
	return (float)((g_seed >> 9) & 0xFFFF) / 65536.0f;
}
static inline u32 rgb(int r, int g, int b) { return 0xFF000000u | ((u32)r << 16) | ((u32)g << 8) | (u32)b; }
static u32 mix(u32 a, u32 b, float t)
{
	t = rt::clamp(t, 0, 1);
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	return rgb(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t), ab + (int)((bb - ab) * t));
}
static void rect(u32 *px, int x0, int y0, int x1, int y1, u32 c)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_cw) x1 = g_cw;
	if (y1 > g_ch) y1 = g_ch;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) px[y * g_cw + x] = c;
}
static float log2a(float x)
{
	if (x <= 1e-9f) return -30;
	union { float f; u32 u; } v;
	v.f = x;
	float e = (float)(int)((v.u >> 23) & 255) - 127;
	v.u = (v.u & 0x007FFFFF) | 0x3F800000;
	float m = v.f;
	return e + (-0.33333f * m + 2.0f) * m - 1.66667f;
}

static const unsigned char *glyph(char c)
{
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);
	for (int i = 0; i < kFont5x7Count; i++)
		if (kFont5x7[i].c == c) return kFont5x7[i].rows;
	return 0;
}
static int textWidth(const char *s, int scale)
{
	int n = 0;
	while (s[n]) n++;
	return n * 6 * scale - scale;
}
static void text(u32 *px, int x, int y, const char *s, int scale, u32 c, int clip0, int clip1)
{
	for (; *s; s++, x += 6 * scale)
	{
		if (x > clip1) break;
		if (x + 5 * scale < clip0) continue;
		const unsigned char *g = glyph(*s);
		if (!g) continue;
		for (int r = 0; r < 7; r++)
			for (int col = 0; col < 5; col++)
				if (g[r] & (0x10 >> col))
					for (int dy = 0; dy < scale; dy++)
						for (int dx = 0; dx < scale; dx++)
						{
							int xx = x + col * scale + dx, yy = y + r * scale + dy;
							if (xx >= clip0 && xx < clip1 && xx >= 0 && xx < g_cw && yy >= 0 && yy < g_ch) px[yy * g_cw + xx] = c;
						}
	}
}

static void glass(u32 *px)
{
	for (int y = 0; y < g_ch; y++)
		for (int x = 0; x < g_cw; x++)
		{
			float t = (float)y / g_ch, sheen = rt::max(0, 1 - rt::absf((float)(x - 2 * y) - 70) / 40.0f) * 0.25f;
			px[y * g_cw + x] = mix(mix(rgb(20, 22, 26), rgb(6, 7, 9), t), rgb(40, 44, 50), sheen);
		}
}

void message(u32 *px, const char *line1, const char *line2)
{
	g_cw = W; g_ch = H;
	glass(px);
	text(px, (W - textWidth(line1, 2)) / 2, 40, line1, 2, rgb(230, 60, 50), 2, W - 2);
	text(px, (W - textWidth(line2, 1)) / 2, 74, line2, 1, rgb(200, 205, 215), 2, W - 2);
}

static void updateBars(const Input &in, float dt)
{
	g_noiseT -= dt;
	if (g_noiseT <= 0)
	{
		g_noiseT = 0.09f;
		for (int i = 0; i < kBars; i++) g_noiseTo[i] = rnd01();
	}
	for (int i = 0; i < kBars; i++)
	{
		g_noise[i] += (g_noiseTo[i] - g_noise[i]) * rt::clamp(dt * 12, 0, 1);
		float target;
		if (in.bandsLive)
		{
			float db = 6.0206f * log2a(in.bands[i]) + (float)i * 0.25f;
			target = rt::clamp((db + 50) / 44, 0, 1);
		}
		else
		{
			float shape = i < 6 ? 1.0f : 1.0f - 0.45f * (float)(i - 6) / (kBars - 6);
			target = in.beat > 0.01f ? rt::clamp(in.beat * shape * (0.5f + 0.5f * g_noise[i]) + 0.04f, 0, 1) : 0;
		}
		g_bar[i] = target > g_bar[i] ? target : rt::max(target, g_bar[i] - dt * 1.6f);
		if (g_bar[i] >= g_peak[i]) { g_peak[i] = g_bar[i]; g_hold[i] = 0.45f; }
		else if ((g_hold[i] -= dt) <= 0) g_peak[i] = rt::max(g_bar[i], g_peak[i] - dt * 0.7f);
	}
}
static void scrollText(u32 *px, const char *t, int y, int scale, u32 c, int area0, int area1, float dt)
{
	int tw = textWidth(t, scale);
	if (tw <= area1 - area0) { g_scroll = 0; text(px, area0 + (area1 - area0 - tw) / 2, y, t, scale, c, area0, area1); return; }
	int loop = tw + 24 * scale;
	g_scroll += dt * 16 * scale;
	if (g_scroll >= (float)loop) g_scroll -= (float)loop;
	int x = area0 - (int)g_scroll;
	text(px, x, y, t, scale, c, area0, area1);
	text(px, x + loop, y, t, scale, c, area0, area1);
}
static u32 barColour(float f)
{
	return f < 0.5f ? mix(rgb(0, 190, 255), rgb(40, 230, 90), f / 0.5f)
		: f < 0.8f ? mix(rgb(40, 230, 90), rgb(255, 210, 0), (f - 0.5f) / 0.3f)
		: mix(rgb(255, 210, 0), rgb(255, 40, 30), (f - 0.8f) / 0.2f);
}

void draw(u32 *px, const Input &in, float dt)
{
	g_cw = W; g_ch = H;
	if (!in.on) { glass(px); return; }
	dt = rt::clamp(dt, 0, 0.2f);
	for (int y = 0; y < H; y++)
	{
		u32 c = mix(rgb(14, 18, 30), rgb(3, 4, 8), (float)y / H);
		for (int x = 0; x < W; x++) px[y * W + x] = c;
	}
	updateBars(in, dt);
	const u32 accent = rgb(230, 60, 50), dim = rgb(130, 140, 155), white = rgb(235, 238, 242);
	text(px, 5, 3, in.source ? in.source : "", 1, accent, 0, W);
	if (in.sub > 0)
	{
		char s[16];
		rt::fmt(s, sizeof s, "SUB %d", in.sub);
		text(px, W - 5 - textWidth(s, 1), 3, s, 1, dim, 0, W);
	}
	rect(px, 4, 13, W - 4, 14, rgb(40, 46, 60));
	scrollText(px, in.title && in.title[0] ? in.title : "", 19, 2, white, 5, W - 5, dt);
	const int top = 40, bottom = 123, span = bottom - top, bw = 8, gap = 2, x0 = (W - (kBars * (bw + gap) - gap)) / 2;
	for (int i = 0; i < kBars; i++)
	{
		int x = x0 + i * (bw + gap), hgt = (int)(g_bar[i] * span);
		for (int seg = 0; seg * 4 < hgt; seg++)
		{
			int y1 = bottom - seg * 4, y0 = y1 - 3;
			rect(px, x, y0, x + bw, y1, barColour((float)(seg * 4) / span));
		}
		for (int seg = hgt / 4 + 1; seg * 4 < span; seg++) { int y1 = bottom - seg * 4; rect(px, x, y1 - 3, x + bw, y1, rgb(18, 22, 32)); }
		if (g_peak[i] > 0.02f) { int py = bottom - (int)(g_peak[i] * span) - 2; rect(px, x, py, x + bw, py + 1, white); }
	}
	rect(px, 4, H - 3, W - 4, H - 2, rgb(40, 46, 60));
}

void drawDin(u32 *px, const Input &in, float dt)
{
	g_cw = DW; g_ch = DH;
	if (!in.on) { glass(px); return; }
	dt = rt::clamp(dt, 0, 0.2f);
	for (int y = 0; y < DH; y++)
	{
		u32 c = mix(rgb(8, 14, 18), rgb(2, 4, 6), (float)y / DH);
		for (int x = 0; x < DW; x++) px[y * DW + x] = c;
	}
	updateBars(in, dt);
	const u32 accent = rgb(230, 60, 50), dim = rgb(120, 150, 160), white = rgb(225, 245, 250);
	const char *src = in.source ? in.source : "";
	int sx1 = 4 + textWidth(src, 1);
	text(px, 4, 3, src, 1, accent, 0, DW);
	char sub[16] = "";
	if (in.sub > 0) rt::fmt(sub, sizeof sub, "SUB %d", in.sub);
	int subW = sub[0] ? textWidth(sub, 1) : 0;
	if (sub[0]) text(px, DW - 4 - subW, 3, sub, 1, dim, 0, DW);
	scrollText(px, in.title && in.title[0] ? in.title : "", 3, 1, white, sx1 + 8, DW - 4 - subW - 8, dt);
	rect(px, 4, 12, DW - 4, 13, rgb(24, 40, 48));
	const int top = 16, bottom = DH - 3, segH = 3, span = bottom - top, nSeg = span / segH;
	const int cw = 8, gap = 2, x0 = (DW - (kBars * (cw + gap) - gap)) / 2;
	for (int i = 0; i < kBars; i++)
	{
		int x = x0 + i * (cw + gap), lit = (int)(g_bar[i] * nSeg + 0.5f);
		for (int seg = 0; seg < nSeg; seg++)
		{
			int y1 = bottom - seg * segH, y0 = y1 - segH + 1;
			u32 c = seg < lit ? barColour((float)seg / nSeg) : rgb(14, 22, 28);
			rect(px, x, y0, x + cw / 2 - 1, y1, c);
			rect(px, x + cw / 2, y0, x + cw - 1, y1, c);
		}
		if (g_peak[i] > 0.04f)
		{
			int seg = (int)(g_peak[i] * nSeg);
			if (seg >= nSeg) seg = nSeg - 1;
			int y1 = bottom - seg * segH, y0 = y1 - segH + 1;
			rect(px, x, y0, x + cw - 1, y1, white);
		}
	}
}

static void phosphorGlass(u32 *px, float level)
{
	for (int y = 0; y < g_ch; y++)
		for (int x = 0; x < g_cw; x++)
		{
			float dx = ((float)x - g_cw * 0.5f) / (g_cw * 0.5f), dy = ((float)y - g_ch * 0.5f) / (g_ch * 0.5f);
			float c = rt::clamp(1 - 0.55f * (dx * dx + dy * dy), 0, 1) * level;
			px[y * g_cw + x] = rgb(2 + (int)(4 * c), 8 + (int)(16 * c), 4 + (int)(8 * c));
		}
}
void drawPip(u32 *px, const Input &in, float dt)
{
	g_cw = PW; g_ch = PH;
	if (!in.on) { phosphorGlass(px, 0.25f); return; }
	dt = rt::clamp(dt, 0, 0.2f);
	updateBars(in, dt);
	phosphorGlass(px, 1);
	const u32 hi = rgb(110, 255, 150), lit = rgb(52, 225, 105), mid = rgb(30, 150, 70), faint = rgb(10, 42, 20);
	const char *src = in.source ? in.source : "";
	const int m = 9;
	text(px, m, 6, src, 1, hi, m, PW - m);
	if (in.sub > 0)
	{
		char s[16];
		rt::fmt(s, sizeof s, "SUB %d", in.sub);
		text(px, PW - m - textWidth(s, 1), 6, s, 1, mid, m, PW - m);
	}
	for (int x = m - 2; x < PW - m + 2; x += 2) px[15 * PW + x] = mid;
	scrollText(px, in.title && in.title[0] ? in.title : "", 18, 2, lit, m, PW - m, dt);
	for (int x = m - 2; x < PW - m + 2; x += 2) px[34 * PW + x] = mid;
	const int nb = 12, bw = 6, gap = 3, x0 = (PW - (nb * (bw + gap) - gap)) / 2, top = 38, bottom = 81, pitch = 4;
	const int nSeg = (bottom - top) / pitch;
	for (int i = 0; i < nb; i++)
	{
		int a = i * kBars / nb, b = (i + 1) * kBars / nb;
		float v = 0, pk = 0;
		for (int k = a; k < b && k < kBars; k++) { v = rt::max(v, g_bar[k]); pk = rt::max(pk, g_peak[k]); }
		int x = x0 + i * (bw + gap), n = (int)(v * nSeg + 0.5f);
		for (int sg = 0; sg < nSeg; sg++)
		{
			int y1 = bottom - sg * pitch, y0 = y1 - 3;
			rect(px, x, y0, x + bw, y1, sg < n ? (sg == n - 1 ? hi : lit) : faint);
		}
		if (pk > 0.03f)
		{
			int sg = (int)(pk * nSeg);
			if (sg >= nSeg) sg = nSeg - 1;
			int y = bottom - sg * pitch - 4;
			rect(px, x, y, x + bw, y + 2, hi);
		}
	}
	for (int i = 0; i <= nb; i += 3) { int x = x0 + i * (bw + gap) - 1; if (x >= 0 && x < PW) rect(px, x, bottom + 1, x + 1, bottom + 4, mid); }
	text(px, x0, bottom + 5, "LO", 1, mid, 0, PW);
	text(px, PW - x0 - textWidth("HI", 1), bottom + 5, "HI", 1, mid, 0, PW);
	static u32 copy[PW * PH];
	for (int i = 0; i < PW * PH; i++) copy[i] = px[i];
	float flick = 0.95f + 0.05f * rnd01();
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++)
		{
			int g = 0;
			if (x > 0) g += (copy[y * PW + x - 1] >> 8) & 255;
			if (x < PW - 1) g += (copy[y * PW + x + 1] >> 8) & 255;
			if (y > 0) g += (copy[(y - 1) * PW + x] >> 8) & 255;
			if (y < PH - 1) g += (copy[(y + 1) * PW + x] >> 8) & 255;
			u32 c = copy[y * PW + x];
			float k = (y & 1) ? 0.8f * flick : flick;
			int r = (int)(((c >> 16) & 255) * k), gg = (int)((((c >> 8) & 255) + g / 4 * 0.3f) * k), bb = (int)((c & 255) * k);
			if (gg > 255) gg = 255;
			px[y * PW + x] = rgb(r, gg, bb + gg / 12 > 255 ? 255 : bb + gg / 12);
		}
}
}

// ---- The dash gauges
namespace screen
{
static inline void over(u32 *px, int x, int y, u32 c, float a)
{
	if (x < 0 || y < 0 || x >= g_cw || y >= g_ch || a <= 0) return;
	u32 &d = px[y * g_cw + x];
	d = a >= 1 ? c : mix(d, c, a);
}
static void seg(u32 *px, float x0, float y0, float x1, float y1, float w, u32 c)
{
	int bx0 = (int)(rt::min(x0, x1) - w - 1), bx1 = (int)(rt::max(x0, x1) + w + 2);
	int by0 = (int)(rt::min(y0, y1) - w - 1), by1 = (int)(rt::max(y0, y1) + w + 2);
	float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
	for (int y = by0; y < by1; y++)
		for (int x = bx0; x < bx1; x++)
		{
			float px0 = (float)x + 0.5f - x0, py0 = (float)y + 0.5f - y0;
			float t = l2 > 0 ? rt::clamp((px0 * dx + py0 * dy) / l2, 0, 1) : 0;
			float ex = px0 - t * dx, ey = py0 - t * dy;
			float d = rt::sqrt(ex * ex + ey * ey);
			over(px, x, y, c, w * 0.5f + 0.5f - d);
		}
}
static void disc(u32 *px, float cx, float cy, float r, u32 c)
{
	for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
		for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++)
		{
			float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
			over(px, x, y, c, r + 0.5f - rt::sqrt(dx * dx + dy * dy));
		}
}
static void dialPoint(float cx, float cy, float r, float f, float *x, float *y)
{
	float a = (225.0f - 270.0f * f) * rt::PI / 180.0f;
	*x = cx + r * rt::cos(a);
	*y = cy - r * rt::sin(a);
}
static void dial(u32 *px, float cx, float cy, float r, int majors, int minorsPer, int labelEvery, int labelStep, float redFrom, bool lit)
{
	u32 ink = lit ? rgb(232, 228, 214) : rgb(70, 70, 66), red = lit ? rgb(235, 52, 40) : rgb(80, 30, 26);
	for (int y = (int)(cy - r - 3); y <= (int)(cy + r + 3); y++)
		for (int x = (int)(cx - r - 3); x <= (int)(cx + r + 3); x++)
		{
			float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = rt::sqrt(dx * dx + dy * dy);
			if (d > r + 2.5f) continue;
			u32 face = mix(rgb(22, 23, 26), rgb(8, 8, 10), d / r);
			if (d > r) face = mix(rgb(120, 122, 128), rgb(40, 40, 44), (d - r) / 2.5f);
			over(px, x, y, face, r + 3 - d);
		}
	int n = majors * minorsPer;
	for (int i = 0; i <= n; i++)
	{
		float f = (float)i / n;
		bool major = i % minorsPer == 0;
		float x0, y0, x1, y1;
		dialPoint(cx, cy, r - 1.5f, f, &x0, &y0);
		dialPoint(cx, cy, r - (major ? 9.0f : 5.0f), f, &x1, &y1);
		seg(px, x0, y0, x1, y1, major ? 1.8f : 1.0f, f >= redFrom ? red : ink);
		int m = i / minorsPer;
		if (major && m % labelEvery == 0)
		{
			char b[8];
			rt::fmt(b, sizeof b, "%d", m * labelStep);
			float lx, ly;
			dialPoint(cx, cy, r - 17, f, &lx, &ly);
			text(px, (int)(lx - textWidth(b, 1) * 0.5f + 0.5f), (int)(ly - 3), b, 1, f >= redFrom ? red : ink, 0, g_cw);
		}
	}
}
static void needle(u32 *px, float cx, float cy, float r, float f, bool lit)
{
	float x1, y1, x0, y0;
	f = rt::clamp(f, -0.01f, 1.02f);
	dialPoint(cx, cy, r - 4, f, &x1, &y1);
	dialPoint(cx, cy, -9, f, &x0, &y0);
	u32 c = lit ? rgb(255, 92, 30) : rgb(110, 50, 30);
	if (lit) seg(px, x0, y0, x1, y1, 5.0f, rgb(90, 30, 10));
	seg(px, x0, y0, x1, y1, 2.4f, c);
	disc(px, cx, cy, 5.5f, rgb(30, 30, 34));
	disc(px, cx, cy, 3.0f, rgb(90, 92, 98));
}
void drawGauge(u32 *px, const Gauge &g, float dt)
{
	g_cw = GW; g_ch = GH;
	static float speedF, revF;
	const float topMph = 220;
	float rpmTop = rt::max(4000.0f, (float)((int)(g.maxRpm / 1000.0f + 0.999f)) * 1000.0f);
	float wantS = g.lit ? rt::clamp(g.mph / topMph, 0, 1.02f) : 0, wantR = g.lit ? rt::clamp(g.rpm / rpmTop, 0, 1.02f) : 0;
	float k = rt::clamp(dt * 14, 0, 1);
	speedF += (wantS - speedF) * k;
	revF += (wantR - revF) * k;
	for (int y = 0; y < GH; y++)
		for (int x = 0; x < GW; x++) px[y * GW + x] = mix(rgb(14, 14, 16), rgb(4, 4, 5), (float)y / GH);
	float r = 58, cyy = 64;
	dial(px, 64, cyy, r, 11, 2, 2, 20, 2.0f, g.lit);
	int majorsR = (int)(rpmTop / 1000.0f + 0.5f);
	dial(px, 192, cyy, r, majorsR, 4, 1, 1, g.maxRpm * 0.92f / rpmTop, g.lit);
	u32 dim = g.lit ? rgb(150, 150, 140) : rgb(55, 55, 52), ink = g.lit ? rgb(240, 236, 222) : rgb(70, 70, 66);
	text(px, 64 - textWidth("MPH", 1) / 2, 40, "MPH", 1, dim, 0, GW);
	text(px, 192 - textWidth("X1000", 1) / 2, 40, "X1000", 1, dim, 0, GW);
	char b[12];
	if (g.lit)
	{
		rt::fmt(b, sizeof b, "%d", (int)(g.mph + 0.5f));
		text(px, 64 - textWidth(b, 2) / 2, 86, b, 2, ink, 0, GW);
		const char *gear = g.gear < 0 ? "N" : g.gear == 0 ? "R" : 0;
		if (!gear) { rt::fmt(b, sizeof b, "%d", g.gear); gear = b; }
		text(px, 192 - textWidth(gear, 2) / 2, 86, gear, 2, g.gear == 0 ? rgb(235, 52, 40) : ink, 0, GW);
	}
	needle(px, 64, cyy, r, speedF, g.lit);
	needle(px, 192, cyy, r, revF, g.lit);
}
}
