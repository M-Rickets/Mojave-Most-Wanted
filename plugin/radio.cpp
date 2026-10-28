#include "radio.h"

namespace radio
{
static float expf(float x)
{
	if (x > 80) x = 80;
	if (x < -80) return 0;
	const float ln2 = 0.69314718f;
	float fn = x / ln2;
	int n = (int)(fn + (fn >= 0 ? 0.5f : -0.5f));
	float r = x - (float)n * ln2;
	float e = 1 + r * (1 + r * (0.5f + r * (1.0f / 6 + r * (1.0f / 24 + r * (1.0f / 120 + r * (1.0f / 720))))));
	union { u32 u; float f; } p;
	p.u = (u32)(n + 127) << 23;
	return e * p.f;
}
float dbGain(float db) { return expf(db * 0.11512925f); }

float tanhf(float x)
{
	if (x > 3) return 1;
	if (x < -3) return -1;
	float x2 = x * x;
	return x * (27 + x2) / (27 + 9 * x2);
}

float softClip(float x)
{
	float a = x < 0 ? -x : x;
	if (a <= 0.8f) return x;
	float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
	return x < 0 ? -y : y;
}

static void set(Biquad &q, float b0, float b1, float b2, float a0, float a1, float a2)
{
	q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
}
void Biquad::lowShelf(float fs, float hz, float db)
{
	float A = dbGain(db * 0.5f), w = 2 * rt::PI * hz / fs, c = rt::cos(w), sn = rt::sin(w);
	float al = sn / 2 * 1.41421356f, sa = 2 * rt::sqrt(A) * al;
	set(*this, A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
		(A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
void Biquad::highShelf(float fs, float hz, float db)
{
	float A = dbGain(db * 0.5f), w = 2 * rt::PI * hz / fs, c = rt::cos(w), sn = rt::sin(w);
	float al = sn / 2 * 1.41421356f, sa = 2 * rt::sqrt(A) * al;
	set(*this, A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
		(A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}
void Biquad::peak(float fs, float hz, float q, float db)
{
	float A = dbGain(db * 0.5f), w = 2 * rt::PI * hz / fs, c = rt::cos(w), al = rt::sin(w) / (2 * q);
	set(*this, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}
void Biquad::lowpass(float fs, float hz, float q)
{
	float w = 2 * rt::PI * hz / fs, c = rt::cos(w), al = rt::sin(w) / (2 * q);
	set(*this, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
void Biquad::highpass(float fs, float hz, float q)
{
	float w = 2 * rt::PI * hz / fs, c = rt::cos(w), al = rt::sin(w) / (2 * q);
	set(*this, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
void Biquad::bandpass(float fs, float hz, float q)
{
	float w = 2 * rt::PI * hz / fs, c = rt::cos(w), al = rt::sin(w) / (2 * q);
	set(*this, al, 0, -al, 1 + al, -2 * c, 1 - al);
}

struct ClickPart { float at, noiseHz, noiseQ, noiseTau, noiseAmp, pingHz, pingTau, pingAmp, thumpHz, thumpTau, thumpAmp; };
int clickSound(int kind, float rate, float *out, int cap)
{
	static const ClickPart kParts[kClicks][2] = {
		{{0.000f, 2200, 1.2f, 0.0020f, 0.60f, 1500, 0.006f, 0.15f, 140, 0.012f, 0.50f}, {0.035f, 4000, 1.5f, 0.0012f, 0.25f, 0, 0, 0, 0, 0, 0}},
		{{0.000f, 3000, 1.5f, 0.0015f, 0.70f, 1900, 0.005f, 0.25f, 0, 0, 0}, {0.060f, 3300, 1.5f, 0.0012f, 0.45f, 2200, 0.004f, 0.15f, 0, 0, 0}},
		{{0.000f, 4500, 1.4f, 0.0008f, 0.55f, 3200, 0.0025f, 0.15f, 0, 0, 0}, {-1}},
		{{0.000f, 2200, 1.3f, 0.0010f, 0.50f, 1400, 0.003f, 0.15f, 90, 0.008f, 0.15f}, {-1}},
		{{0.000f, 1600, 1.0f, 0.0025f, 0.70f, 1100, 0.006f, 0.20f, 0, 0, 0}, {0.014f, 1700, 1.0f, 0.0020f, 0.50f, 1150, 0.005f, 0.12f, 0, 0, 0}},
	};
	static const float kLength[kClicks] = {0.090f, 0.100f, 0.025f, 0.045f, 0.080f};
	if (kind < 0 || kind >= kClicks) return 0;
	int n = (int)(kLength[kind] * rate);
	if (n > cap) n = cap;
	for (int i = 0; i < n; i++) out[i] = 0;
	u32 seed = 0x1234567u + (u32)kind * 7919u;
	for (int k = 0; k < 2; k++)
	{
		const ClickPart &c = kParts[kind][k];
		if (c.at < 0) continue;
		Biquad bp;
		if (c.noiseAmp > 0) bp.bandpass(rate, c.noiseHz, c.noiseQ);
		int i0 = (int)(c.at * rate);
		for (int i = i0; i < n; i++)
		{
			float t = (float)(i - i0) / rate, y = 0;
			if (c.noiseAmp > 0)
			{
				seed = seed * 1664525u + 1013904223u;
				float w = (float)((seed >> 9) & 0xFFFF) / 32768.0f - 1;
				y += bp.run(0, w) * c.noiseAmp * 3.0f * expf(-t / c.noiseTau);
			}
			if (c.pingAmp > 0) y += rt::sin(2 * rt::PI * c.pingHz * t) * c.pingAmp * expf(-t / c.pingTau);
			if (c.thumpAmp > 0) y += rt::sin(2 * rt::PI * c.thumpHz * t) * c.thumpAmp * expf(-t / c.thumpTau);
			out[i] += y;
		}
	}
	float pk = 0;
	for (int i = 0; i < n; i++) pk = rt::max(pk, rt::absf(out[i]));
	int fade = (int)(0.001f * rate);
	for (int i = 0; i < n; i++)
	{
		float g = pk > 0 ? 0.9f / pk : 0;
		if (i > n - fade) g *= (float)(n - i) / (float)fade;
		out[i] *= g;
	}
	return n;
}

void Analyzer::configure(float rate)
{
	fs = rate;
	for (int k = 0; k < kBands; k++)
	{
		float hz = 45.0f * expf((float)k / (kBands - 1) * 5.7404f);
		bp[k] = Biquad();
		bp[k].bandpass(rate, hz, 4.0f);
		env[k] = 0;
	}
}
void Analyzer::push(float mono)
{
	for (int k = 0; k < kBands; k++)
	{
		float y = bp[k].run(0, mono);
		float a = y < 0 ? -y : y;
		env[k] += (a - env[k]) * (a > env[k] ? 0.02f : 0.0005f);
	}
}

static const struct { float threshold, gain, chatterMs, hz1, q1, hz2, q2; } kPanels[4] = {
	{0.20f, 1.00f, 7.0f, 140, 6, 360, 8},
	{0.40f, 0.30f, 3.0f, 880, 10, 1900, 12},
	{0.30f, 0.50f, 4.5f, 460, 8, 1050, 9},
	{0.30f, 0.85f, 5.5f, 240, 6, 610, 8},
};

void Dsp::configure(const Settings &settings, int lvl, float rate)
{
	s = settings;
	if (lvl < 0) lvl = 0;
	if (lvl > 10) lvl = 10;
	bool first = level < 0 || rate != fs;
	level = lvl; fs = rate;
	float n = (float)lvl;
	bassGain = dbGain(s.bassDbPerLevel * n);
	highGain = dbGain(s.highDbPerLevel * n);
	drive = n > s.thickFrom && s.thickFrom < 10 ? (n - s.thickFrom) / (10 - s.thickFrom) * s.thickMax : 0;
	Biquad sb = sub, hs = hiShelf;
	sb.peak(fs, s.subHz, s.subQ, s.subDbPerLevel * n);
	hs.highShelf(fs, s.highShelfHz, s.highShelfDbPerLevel * n);
	for (int c = 0; c < 2; c++) { sb.z1[c] = sub.z1[c]; sb.z2[c] = sub.z2[c]; hs.z1[c] = hiShelf.z1[c]; hs.z2[c] = hiShelf.z2[c]; }
	sub = sb; hiShelf = hs;
	if (first)
	{
		xo1 = Biquad(); xo1.lowpass(fs, s.crossoverHz, 0.7071f);
		xo2 = Biquad(); xo2.lowpass(fs, s.crossoverHz, 0.7071f);
		xh1 = Biquad(); xh1.highpass(fs, s.crossoverHz, 0.7071f);
		xh2 = Biquad(); xh2.highpass(fs, s.crossoverHz, 0.7071f);
		thickLp = Biquad(); thickLp.lowpass(fs, 240, 0.7071f);
		outLp = Biquad(); outLp.lowpass(fs, s.outsideHz, 0.8f);
		rattleLp = Biquad(); rattleLp.lowpass(fs, 1500, 0.7071f);
		for (int k = 0; k < 4; k++)
		{
			Panel &p = panel[k];
			p.threshold = kPanels[k].threshold; p.gain = kPanels[k].gain; p.last = 0; p.timer = 0; p.soft = 0;
			p.chatter = kPanels[k].chatterMs * fs / 1000;
			p.res[0] = Biquad(); p.res[0].bandpass(fs, kPanels[k].hz1, kPanels[k].q1);
			p.res[1] = Biquad(); p.res[1].bandpass(fs, kPanels[k].hz2, kPanels[k].q2);
		}
	}
}

void Dsp::process(float &l, float &r, float outsideTarget)
{
	float in[2] = {l, r}, lo[2], hi[2];
	for (int c = 0; c < 2; c++)
	{
		lo[c] = xo2.run(c, xo1.run(c, in[c]));
		hi[c] = xh2.run(c, xh1.run(c, in[c]));
		if (level > 0) hi[c] = hiShelf.run(c, hi[c]) * highGain;
		float b = level > 0 ? sub.run(c, lo[c]) * bassGain : lo[c];
		if (drive > 0)
		{
			b = thickLp.run(c, tanhf(b * (1 + drive) / 0.9f) * 0.9f);
		}
		lo[c] = b;
	}
	float pk = rt::max(rt::absf(lo[0]), rt::absf(lo[1]));
	bassEnvC += (pk - bassEnvC) * (pk > bassEnvC ? 0.004f : 0.00012f);
	float bg = bassEnvC > s.bassCeiling ? s.bassCeiling / bassEnvC : 1;
	lo[0] *= bg; lo[1] *= bg;
	float bass = (lo[0] + lo[1]) * 0.5f;
	float ab = rt::absf(bass);
	env += (ab - env) * (ab > env ? 0.006f : 0.00018f);
	float rat = panels(ab / rt::max(s.bassCeiling, 0.05f));
	outside += (outsideTarget - outside) * 0.0005f;
	float y[2];
	for (int c = 0; c < 2; c++)
	{
		float h = hi[c];
		if (outside > 0.001f) h = h * (1 - outside) + (outLp.run(c, h) + h * s.outsideBleed) * outside;
		y[c] = h + lo[c] + rat;
	}
	float op = rt::max(rt::absf(y[0]), rt::absf(y[1]));
	limEnv += (op - limEnv) * (op > limEnv ? 0.05f : 0.0002f);
	float lg = limEnv > 0.9f ? 0.9f / limEnv : 1;
	l = softClip(y[0] * lg);
	r = softClip(y[1] * lg);
}

float Dsp::panels(float push)
{
	float rat = 0;
	float n = (float)level;
	if (s.rattleGain > 0 && n > s.rattleFrom)
	{
		float amt = s.rattleGain * (n - s.rattleFrom) / (10 - s.rattleFrom);
		for (int k = 0; k < 4; k++)
		{
			Panel &p = panel[k];
			float over = push > p.threshold ? (push - p.threshold) / (1 - p.threshold) : 0;
			float x = 0;
			if (over > 0)
			{
				rnd = rnd * 1664525u + 1013904223u;
				float j = (float)((rnd >> 9) & 0xFFFF) / 65536.0f;
				if (p.last <= 0) { x = over + 0.3f; p.timer = p.chatter * (0.5f + j); }
				else if ((p.timer -= 1) <= 0) { x = over * (0.6f + 0.8f * j); p.timer = p.chatter * (0.6f + 0.8f * j); }
			}
			p.last = over;
			p.soft += (x * 40.0f * p.gain - p.soft) * 0.11f;
			rat += p.res[0].run(0, p.soft) + 0.6f * p.res[1].run(0, p.soft);
		}
		rat = rattleLp.run(0, rat) * amt;
	}
	return rat;
}
}
