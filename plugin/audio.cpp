#include "audio.h"
#include "radio.h"
#include "mp3.h"

extern "C" {
struct WAVEFORMATEX_ { u16 wFormatTag, nChannels; u32 nSamplesPerSec, nAvgBytesPerSec; u16 nBlockAlign, wBitsPerSample, cbSize; };
struct WAVEHDR_ { char *lpData; u32 dwBufferLength, dwBytesRecorded; u32 dwUser, dwFlags, dwLoops; WAVEHDR_ *lpNext; u32 reserved; };
__declspec(dllimport) u32 __stdcall waveOutOpen(HANDLE *, u32, const WAVEFORMATEX_ *, u32, u32, u32);
__declspec(dllimport) u32 __stdcall waveOutPrepareHeader(HANDLE, WAVEHDR_ *, u32);
__declspec(dllimport) u32 __stdcall waveOutUnprepareHeader(HANDLE, WAVEHDR_ *, u32);
__declspec(dllimport) u32 __stdcall waveOutWrite(HANDLE, WAVEHDR_ *, u32);
__declspec(dllimport) u32 __stdcall waveOutReset(HANDLE);
__declspec(dllimport) u32 __stdcall waveOutClose(HANDLE);
__declspec(dllimport) u32 __stdcall mciSendStringA(const char *, char *, u32, HANDLE);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, u32, u32(__stdcall *)(void *), void *, u32, u32 *);
__declspec(dllimport) HANDLE __stdcall CreateEventA(void *, int, int, const char *);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE, u32);
__declspec(dllimport) void __stdcall Sleep(u32);
__declspec(dllimport) long __stdcall CoInitializeEx(void *, u32);
}

namespace audio
{
static const u32 kRate = 48000, kFrames = 1024, kBuffers = 4;
static const u32 WHDR_DONE = 1, CALLBACK_EVENT = 0x50000, WAVE_MAPPER = 0xFFFFFFFF;

struct Engine
{
	u32 ver, rate, nR, nG, nS;
	float minR, maxR;
	u32 *rpmPos, *grains;
	i16 *s;
	char *blob;
	u32 g;
	float p, oldP, fade;
};

static char g_dir[300];
static volatile long g_lock;
static Engine *g_on, *g_off;
static volatile bool g_engOn;
static volatile float g_rpm = 1000, g_throttle, g_engVol, g_effects = 1, g_radioVol = 1, g_radioPan = 0;
struct Loop { i16 *s; u32 n, rate; float p; };
static Loop g_skid[3];
static const int kShots = 3 + radio::kClicks, kVoices = 20;
static Loop g_shot[kShots];
struct Voice { volatile bool on; const i16 *s; u32 end, rate; float p, vol, pitch; };
static Voice g_voice[kVoices];
static volatile bool g_limOn;
static volatile float g_limHz = 13, g_limDrop = 0.045f, g_limCut = 0.45f;
static u32 g_xr = 0x1F2E3D4C;
static u32 xrnd(u32 n) { g_xr = g_xr * 1664525u + 1013904223u; return n ? (g_xr >> 8) % n : 0; }
static volatile float g_skidAmt, g_skidPitch = 1, g_skidVol = 1, g_skidGain[3] = {0.7f, 1, 1};
static volatile float g_windLvl, g_windMuf;
static float g_windSm, g_windGust = 0.5f, g_windGustT = 0.5f, g_windLp[2], g_windBp[2], g_windHp[2], g_windLo2[2];
static u32 g_windGustN;
static volatile float g_rainLvl, g_rainMuf;
static float g_rainSm, g_rainMufSm, g_rainHp[2], g_rainHpX[2], g_rainLp[2], g_rainBed[2];
struct Drop { float y1, y2, a, b, g; int pan; };
static Drop g_drops[12];
static volatile bool g_run;
static HANDLE g_thread;
static volatile int g_radioCmd;
static char g_radioPath[300];
static volatile bool g_radioFinished, g_radioOpen;
static volatile u32 g_radioVolErr = 0xFFFFFFFF, g_radioVolSet = 0xFFFFFFFF;
static const u32 kRing = 1u << 17;
static i16 g_ring[kRing * 2];
static volatile u32 g_rW, g_rR;
static volatile u32 g_rRate = 48000;
static volatile bool g_rLive, g_rPaused, g_rEof, g_rWantOff, g_rSilent = true;
static u8 *g_mp3; static u32 g_mp3Len, g_mp3Pos;
static u32 g_rPos; static float g_rFrac, g_rGain;
static radio::Dsp g_dsp;
static radio::Analyzer g_an;
static volatile bool g_analyze;
static volatile float g_extPush;
static volatile bool g_extOn;
static float g_extSm;
static radio::Settings g_dspSet;
static volatile bool g_dspSetOk;
static volatile int g_subLevel, g_subApplied = -1;
static volatile float g_radioOutside, g_bassEnv;
static char g_engWant[64], g_engHave[64];
static volatile long g_engReq;
static HANDLE g_radioThread;

static void lock() { while (__sync_lock_test_and_set(&g_lock, 1)) Sleep(0); }
static void unlock() { __sync_lock_release(&g_lock); }

static Engine *readEngine(const char *path)
{
	u32 len = 0;
	char *b = rt::readfile(path, &len);
	if (!b) return 0;
	if (len < 32 || memcmp(b, "MMWE", 4)) { rt::free(b); return 0; }
	u32 *h = (u32 *)(b + 4);
	Engine *e = (Engine *)rt::alloc(sizeof(Engine));
	memset(e, 0, sizeof *e);
	e->ver = h[0]; e->rate = h[1];
	memcpy(&e->minR, &h[2], 4); memcpy(&e->maxR, &h[3], 4);
	e->nR = h[4]; e->nG = h[5]; e->nS = h[6];
	u32 need = 32 + (e->nR + e->nG) * 4 + e->nS * 2;
	if (len < need || e->nR < 2 || e->nG < 2 || !e->rate) { rt::free(b); rt::free(e); return 0; }
	e->rpmPos = (u32 *)(b + 32);
	e->grains = e->rpmPos + e->nR;
	e->s = (i16 *)(e->grains + e->nG);
	e->blob = b;
	e->rpmPos++; e->nR--;
	for (u32 pass = 0; pass < 2; pass++)
		for (u32 i = 0; i < e->nR; i++)
		{
			i32 v = (i32)e->rpmPos[i];
			if (v >= 0 && (u32)v < e->nS) continue;
			if (i + 1 < e->nR && (i32)e->rpmPos[i + 1] >= 0 && e->rpmPos[i + 1] < e->nS) e->rpmPos[i] = e->rpmPos[i + 1];
			else if (i > 0) e->rpmPos[i] = e->rpmPos[i - 1];
		}
	return e;
}
static void freeEngine(Engine *e) { if (e) { rt::free(e->blob); rt::free(e); } }

static u32 grainFor(Engine *e, float rpm)
{
	float f = (rpm - e->minR) / (e->maxR - e->minR) * (float)(e->nR - 1);
	f = rt::clamp(f, 0, (float)(e->nR - 1) - 0.001f);
	u32 i = (u32)f; float t = f - (float)i;
	float pos = (float)e->rpmPos[i] * (1 - t) + (float)e->rpmPos[i + 1] * t;
	u32 lo = 0, hi = e->nG - 2;
	while (lo < hi) { u32 mid = (lo + hi + 1) / 2; if ((float)e->grains[mid] <= pos) lo = mid; else hi = mid - 1; }
	return lo;
}

static inline float at(Engine *e, float p)
{
	u32 i = (u32)p;
	if (i + 1 >= e->nS) return 0;
	float t = p - (float)i;
	return (float)e->s[i] * (1 - t) + (float)e->s[i + 1] * t;
}

static u32 g_rnd = 0x1234567;
static u32 rnd5() { g_rnd = g_rnd * 1664525u + 1013904223u; return (g_rnd >> 16) % 5; }
static float engineNext(Engine *e, float rpm)
{
	if (!e) return 0;
	float step = (float)e->rate / (float)kRate;
	if (e->p >= (float)e->grains[e->g + 1] || e->p < (float)e->grains[e->g])
	{
		u32 t = grainFor(e, rpm), last = e->nG - 2;
		u32 lo = t > 2 ? t - 2 : 0, hi = t + 2 < last ? t + 2 : last;
		if (e->ver >= 2)
		{
			u32 a = grainFor(e, rpm * 0.985f), b = grainFor(e, rpm * 1.015f);
			if (a < lo) lo = a;
			if (b > hi) hi = b < last ? b : last;
		}
		u32 next = e->g + 1, ng;
		bool onward = e->p >= (float)e->grains[e->g + 1] && next >= lo && next <= hi;
		if (onward) ng = next;
		else
		{
			ng = lo + (e->ver >= 2 ? xrnd((hi - lo) / 2 + 1) : rnd5());
			if (ng > hi) ng = hi;
			if (ng == e->g) ng = ng < hi ? ng + 1 : lo;
			e->oldP = e->p; e->fade = 1;
		}
		e->g = ng;
		if (!onward) e->p = (float)e->grains[ng];
	}
	float v = at(e, e->p);
	if (e->fade > 0)
	{
		v = v * (1 - e->fade) + at(e, e->oldP) * e->fade;
		e->oldP += step;
		e->fade -= e->ver >= 2 ? 1.0f / 480.0f : 1.0f / 192.0f;
	}
	e->p += step;
	return v;
}

static void mix(i16 *out)
{
	lock();
	float rpm = g_rpm, thr = g_throttle;
	float evTarget = g_engOn ? g_engVol * g_effects : 0;
	static float ev;
	if (g_dspSetOk && g_subApplied != g_subLevel) { g_dsp.configure(g_dspSet, g_subLevel, (float)kRate); g_subApplied = g_subLevel; }
	float rvk = rt::clamp(g_radioVol, 0, 1.6f), rv = rt::min(rvk, 1), rDrive = rt::max(rvk, 1), rpan = rt::clamp(g_radioPan, -1, 1);
	float ra = (rpan + 1) * 0.25f * rt::PI;
	float rgl = rt::min(1, rv * rt::cos(ra) * 1.41421f) * 32767, rgr = rt::min(1, rv * rt::sin(ra) * 1.41421f) * 32767;
	float rstep = (float)g_rRate / (float)kRate, rOut = rt::clamp(g_radioOutside, 0, 1);
	bool rPlay = g_rLive && !g_rPaused && !g_rWantOff;
	for (u32 i = 0; i < kFrames; i++)
	{
		float l = 0, r = 0;
		g_rGain += rt::clamp((rPlay ? 1.0f : 0.0f) - g_rGain, -1.0f / 2400.0f, 1.0f / 2400.0f);
		if (g_rLive && g_rGain > 0 && g_rW - g_rPos >= 3)
		{
			const u32 m = kRing - 1;
			u32 p0 = g_rPos ? g_rPos - 1 : 0;
			float t = g_rFrac, o[2];
			for (int c = 0; c < 2; c++)
			{
				float y0 = g_ring[(p0 & m) * 2 + c], y1 = g_ring[(g_rPos & m) * 2 + c];
				float y2 = g_ring[((g_rPos + 1) & m) * 2 + c], y3 = g_ring[((g_rPos + 2) & m) * 2 + c];
				float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
				o[c] = (((c3 * t + c2) * t + c1) * t + y1) * rDrive / 32768.0f;
			}
			g_rFrac += rstep;
			while (g_rFrac >= 1) { g_rFrac -= 1; g_rPos++; }
			g_rR = g_rPos ? g_rPos - 1 : 0;
			g_dsp.process(o[0], o[1], rOut);
			if (g_analyze) g_an.push((o[0] + o[1]) * 0.5f * g_rGain);
			l += o[0] * g_rGain * rgl; r += o[1] * g_rGain * rgr;
		}
		else if (g_analyze && (i & 7) == 0) g_an.push(0);
		g_extSm += ((g_extOn ? g_extPush : 0) - g_extSm) * 0.003f;
		ev += rt::clamp(evTarget - ev, -1.0f / 2000.0f, 1.0f / 2000.0f);
		static float thrS;
		thrS += (thr - thrS) * (1.0f / 1500.0f);
		static float limPh, limEnv, cutS;
		float limCut = 0, rpmMul = 1;
		limEnv += ((g_limOn ? 1.0f : 0.0f) - limEnv) * (1.0f / 480.0f);
		if (limEnv > 0.01f)
		{
			limPh += g_limHz / (float)kRate;
			if (limPh >= 1) limPh -= 1;
			limCut = (limPh < 0.3f ? 1.0f : 0.0f) * limEnv * g_limCut;
			rpmMul = 1 - g_limDrop * limEnv * (limPh < 0.3f ? limPh / 0.3f : 1 - (limPh - 0.3f) / 0.7f);
		}
		else limPh = 0;
		cutS += (limCut - cutS) * (1.0f / 96.0f);
		if (ev > 0.0005f && g_on)
		{
			float a = engineNext(g_on, rpm * rpmMul);
			float b = g_off ? engineNext(g_off, rpm * rpmMul) : a;
			float k = thrS * thrS * (3 - 2 * thrS);
			float ga = rt::sin(k * rt::PI * 0.5f), gb = rt::cos(k * rt::PI * 0.5f);
			float v = (a * (0.35f + 0.65f * ga) * (1 - 0.82f * cutS) + b * gb * 0.8f * (1 - 0.4f * cutS)) * ev;
			l += v; r += v;
		}
		{
			static float sa;
			sa += rt::clamp(g_skidAmt - sa, -1.0f / 1000.0f, 1.0f / 1000.0f);
			if (sa > 0.0005f)
			{
				const float lo[3] = {0.0f, 0.1f, 0.5f};
				for (int k = 0; k < 3; k++)
				{
					Loop &L = g_skid[k];
					if (!L.s) continue;
					float w = rt::clamp((sa - lo[k]) / 0.3f, 0, 1) * g_skidGain[k];
					u32 i0 = (u32)L.p; float t = L.p - (float)i0;
					u32 i1 = i0 + 1 < L.n ? i0 + 1 : 0;
					float v = ((float)L.s[i0] * (1 - t) + (float)L.s[i1] * t) * w * g_skidVol * g_effects;
					l += v; r += v;
					L.p += (float)L.rate / (float)kRate * g_skidPitch;
					if (L.p >= (float)L.n) L.p -= (float)L.n;
				}
			}
		}
		for (int k = 0; k < kVoices; k++)
		{
			Voice &vo = g_voice[k];
			if (!vo.on) continue;
			u32 i0 = (u32)vo.p;
			if (!vo.s || i0 + 1 >= vo.end) { vo.on = false; continue; }
			float t = vo.p - (float)i0;
			float g = vo.vol;
			float v = ((float)vo.s[i0] * (1 - t) + (float)vo.s[i0 + 1] * t) * g;
			l += v * g_effects; r += v * g_effects;
			vo.p += (float)vo.rate / (float)kRate * vo.pitch;
		}
		if (g_windLvl > 0.0005f || g_windSm > 0.0005f)
		{
			g_windSm += (g_windLvl - g_windSm) * (1.0f / 24000.0f);
			if (++g_windGustN >= 4800)
			{
				g_windGustN = 0;
				g_windGustT = 0.25f + 0.75f * (float)xrnd(1000) / 1000.0f;
			}
			g_windGust += (g_windGustT - g_windGust) * (1.0f / 30000.0f);
			float m = rt::clamp(g_windMuf, 0, 1);
			float cut = (0.02f + 0.06f * g_windGust) * (1 - 0.7f * m);
			float amp = g_windSm * (0.45f + 0.55f * g_windGust) * (1 - 0.6f * m) * g_effects * 15000.0f;
			for (int c = 0; c < 2; c++)
			{
				float nz = ((float)xrnd(65536) / 32768.0f - 1.0f);
				g_windLp[c] += (nz - g_windLp[c]) * cut;
				float f = 2 * rt::sin(rt::PI * (480.0f + 420.0f * g_windGust + (c ? 37.0f : 0.0f)) / (float)kRate), q = 0.08f;
				g_windLo2[c] += f * g_windBp[c];
				g_windHp[c] = nz - g_windLo2[c] - q * g_windBp[c];
				g_windBp[c] += f * g_windHp[c];
				float v = (g_windLp[c] * 3.2f + g_windBp[c] * 0.12f * g_windGust * (1 - m)) * amp;
				if (c) r += v; else l += v;
			}
		}
		if (g_rainLvl > 0.0005f || g_rainSm > 0.0005f)
		{
			g_rainSm += (g_rainLvl - g_rainSm) * (1.0f / 12000.0f);
			g_rainMufSm += (g_rainMuf - g_rainMufSm) * (1.0f / 6000.0f);
			float m = rt::clamp(g_rainMufSm, 0, 1), lv = g_rainSm;
			float amp = lv * g_effects * 9000.0f * (1 - 0.45f * m);
			float hpK = 0.16f, lpK = 0.62f - 0.55f * m;
			for (int c = 0; c < 2; c++)
			{
				float nz = ((float)xrnd(65536) / 32768.0f - 1.0f);
				g_rainHp[c] = (1 - hpK) * (g_rainHp[c] + nz - g_rainHpX[c]);
				g_rainHpX[c] = nz;
				g_rainLp[c] += (g_rainHp[c] * (1 - m) + nz * 0.5f * m - g_rainLp[c]) * lpK;
				g_rainBed[c] += (nz - g_rainBed[c]) * 0.01f;
				float v = (g_rainLp[c] * 0.55f + g_rainBed[c] * 2.2f * lv * (0.4f + m)) * amp;
				if (c) r += v; else l += v;
			}
			if (xrnd(1u << 16) < (u32)(65536.0f * lv * 400.0f / (float)kRate))
			{
				Drop &d = g_drops[xrnd(12)];
				float f = m > 0.5f ? 250.0f + (float)xrnd(650) : 2000.0f + (float)xrnd(4000);
				float rr = m > 0.5f ? 0.9985f : 0.993f;
				d.a = 2 * rr * rt::cos(2 * rt::PI * f / (float)kRate);
				d.b = -rr * rr;
				d.y1 += (0.3f + (float)xrnd(1000) / 1000.0f) * (m > 0.5f ? 0.9f : 0.6f) * rt::sin(2 * rt::PI * f / (float)kRate);
				d.g = 1;
				d.pan = (int)xrnd(3);
			}
			for (int k = 0; k < 12; k++)
			{
				Drop &d = g_drops[k];
				if (d.g == 0) continue;
				float y = d.a * d.y1 + d.b * d.y2;
				d.y2 = d.y1; d.y1 = y;
				if (rt::absf(y) < 1e-4f && rt::absf(d.y2) < 1e-4f) { d.y1 = d.y2 = 0; d.g = 0; continue; }
				float v = y * amp * 0.35f;
				l += v * (d.pan == 0 ? 1.0f : d.pan == 1 ? 0.7f : 0.4f);
				r += v * (d.pan == 2 ? 1.0f : d.pan == 1 ? 0.7f : 0.4f);
			}
		}
		out[i * 2] = (i16)(radio::softClip(l / 32767.0f) * 32767);
		out[i * 2 + 1] = (i16)(radio::softClip(r / 32767.0f) * 32767);
	}
	g_rSilent = g_rGain <= 0;
	g_bassEnv = g_extOn ? g_extSm : g_rLive && g_rGain > 0 ? g_dsp.env * g_rGain : g_bassEnv * 0.8f;
	unlock();
}

static void radioFadeOut()
{
	if (!g_rLive) return;
	g_rWantOff = true;
	for (int i = 0; i < 15 && !g_rSilent; i++) Sleep(10);
}
static void radioClose()
{
	radioFadeOut();
	lock(); g_rLive = false; unlock();
	g_rWantOff = false;
	if (g_mp3) { rt::free(g_mp3); g_mp3 = 0; }
	if (g_radioOpen) { mciSendStringA("close mmwradio", 0, 0, 0); g_radioOpen = false; }
}
static void radioDecode(u32 ahead)
{
	static i16 pcm[mp3::kMaxSamples];
	for (int guard = 0; guard < 600 && !g_rEof; guard++)
	{
		u32 queued = g_rW - g_rR;
		if (queued >= ahead || queued + 1152 + 8 >= kRing) break;
		u32 used = 0; int ch = 0, hz = 0;
		int n = mp3::frame(g_mp3 + g_mp3Pos, g_mp3Len - g_mp3Pos, pcm, &used, &ch, &hz);
		if (!used) { g_rEof = true; break; }
		g_mp3Pos += used;
		if (n <= 0 || ch < 1) continue;
		if (hz >= 8000) g_rRate = (u32)hz;
		u32 w = g_rW;
		for (int k = 0; k < n; k++)
		{
			u32 idx = ((w + (u32)k) & (kRing - 1)) * 2;
			g_ring[idx] = pcm[k * ch];
			g_ring[idx + 1] = pcm[k * ch + (ch > 1 ? 1 : 0)];
		}
		__sync_synchronize();
		g_rW = w + (u32)n;
	}
}
static bool radioOpenDecoded(const char *path)
{
	u32 len = 0;
	char *b = rt::readfile(path, &len);
	if (!b || len < 4096) { if (b) rt::free(b); return false; }
	g_mp3 = (u8 *)b; g_mp3Len = len; g_mp3Pos = mp3::skipTag(g_mp3, len);
	mp3::reset();
	g_rEof = false;
	lock(); g_rW = g_rR = 0; g_rPos = 0; g_rFrac = 0; g_rGain = 0; unlock();
	radioDecode(kRate / 2);
	if (g_rW == 0) { rt::free(g_mp3); g_mp3 = 0; return false; }
	lock(); g_rPaused = false; g_rLive = true; unlock();
	return true;
}

static void radioService()
{
	char cmd[400], ret[64];
	int c = g_radioCmd;
	if (c)
	{
		g_radioCmd = 0;
		if (c == 1 || c == 2)
		{
			radioClose();
			g_radioFinished = false;
		}
		if (c == 1 && !radioOpenDecoded(g_radioPath))
		{
			rt::fmt(cmd, sizeof cmd, "open \"%s\" type mpegvideo alias mmwradio", g_radioPath);
			if (mciSendStringA(cmd, 0, 0, 0) == 0)
			{
				g_radioOpen = true;
				rt::fmt(cmd, sizeof cmd, "setaudio mmwradio volume to %d", (int)(rt::clamp(g_radioVol, 0, 1) * 1000));
				mciSendStringA(cmd, 0, 0, 0);
				mciSendStringA("play mmwradio", 0, 0, 0);
			}
			else g_radioFinished = true;
		}
		if (c == 3) { g_rPaused = true; if (g_radioOpen) mciSendStringA("pause mmwradio", 0, 0, 0); }
		if (c == 4) { g_rPaused = false; if (g_radioOpen) mciSendStringA("resume mmwradio", 0, 0, 0); }
	}
	if (g_rLive)
	{
		radioDecode(kRate * 3 / 2);
		if (g_rEof && g_rW - g_rR <= 4 && !g_radioCmd) g_radioFinished = true;
		g_radioVolSet = (u32)(rt::clamp(g_radioVol, 0, 1) * 1000); g_radioVolErr = 0;
	}
	static u32 tick;
	static int lastL = -1, lastR = -1;
	if (!g_radioOpen) lastL = lastR = -1;
	float v = rt::clamp(g_radioVol, 0, 1), pan = rt::clamp(g_radioPan, -1, 1);
	float a = (pan + 1) * 0.25f * rt::PI;
	int lv = (int)(v * rt::cos(a) * 1.41421f * 1000), rv = (int)(v * rt::sin(a) * 1.41421f * 1000);
	if (lv > 1000) lv = 1000;
	if (rv > 1000) rv = 1000;
	if (g_radioOpen && (lastL < 0 || lv - lastL > 9 || lastL - lv > 9 || rv - lastR > 9 || lastR - rv > 9 || (lv == 0) != (lastL == 0) || (rv == 0) != (lastR == 0)))
	{
		lastL = lv; lastR = rv;
		u32 r;
		if (lv == rv)
		{
			rt::fmt(cmd, sizeof cmd, "setaudio mmwradio volume to %d", lv);
			r = mciSendStringA(cmd, 0, 0, 0);
		}
		else r = 1;
		if (r)
		{
			rt::fmt(cmd, sizeof cmd, "setaudio mmwradio left volume to %d", lv); r = mciSendStringA(cmd, 0, 0, 0);
			rt::fmt(cmd, sizeof cmd, "setaudio mmwradio right volume to %d", rv); mciSendStringA(cmd, 0, 0, 0);
			if (r && lv != rv)
			{
				rt::fmt(cmd, sizeof cmd, "setaudio mmwradio volume to %d", lv > rv ? lv : rv);
				r = mciSendStringA(cmd, 0, 0, 0);
			}
		}
		g_radioVolErr = r; g_radioVolSet = (u32)(lv > rv ? lv : rv);
	}
	if (g_radioOpen && ++tick % 20 == 0)
	{
		ret[0] = 0;
		mciSendStringA("status mmwradio mode", ret, sizeof ret, 0);
		if (rt::startswith(ret, "stopped")) g_radioFinished = true;
	}
}

static void engineService()
{
	if (!__sync_lock_test_and_set(&g_engReq, 0)) return;
	char id[64], p[360];
	lock(); rt::strcpy(id, g_engWant, sizeof id); unlock();
	if (rt::streq(id, g_engHave) && g_on) return;
	rt::fmt(p, sizeof p, "%ssound\\eng_%s.mmwe", g_dir, id);
	Engine *on = readEngine(p);
	rt::fmt(p, sizeof p, "%ssound\\eng_%s_off.mmwe", g_dir, id);
	Engine *off = on ? readEngine(p) : 0;
	lock();
	Engine *a = g_on, *b = g_off;
	g_on = on; g_off = off;
	unlock();
	freeEngine(a); freeEngine(b);
	rt::strcpy(g_engHave, id, sizeof g_engHave);
}
static u32 __stdcall radioThreadMain(void *)
{
	CoInitializeEx(0, 2);
	while (g_run)
	{
		engineService();
		radioService();
		Sleep(15);
	}
	radioClose();
	return 0;
}

static u32 __stdcall threadMain(void *)
{
	HANDLE ev = CreateEventA(0, 0, 0, 0);
	WAVEFORMATEX_ fmt = {1, 2, kRate, kRate * 4, 4, 16, 0};
	HANDLE wo = 0;
	if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, (u32)ev, 0, CALLBACK_EVENT) != 0) wo = 0;
	static i16 buf[kBuffers][kFrames * 2];
	static WAVEHDR_ hdr[kBuffers];
	if (wo)
		for (u32 i = 0; i < kBuffers; i++)
		{
			memset(&hdr[i], 0, sizeof hdr[i]);
			hdr[i].lpData = (char *)buf[i];
			hdr[i].dwBufferLength = kFrames * 4;
			waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
			mix(buf[i]);
			waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
		}
	while (g_run)
	{
		if (wo)
		{
			WaitForSingleObject(ev, 50);
			for (u32 i = 0; i < kBuffers; i++)
				if (hdr[i].dwFlags & WHDR_DONE)
				{
					hdr[i].dwFlags &= ~WHDR_DONE;
					mix(buf[i]);
					waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
				}
		}
		else Sleep(20);
	}
	if (wo)
	{
		waveOutReset(wo);
		for (u32 i = 0; i < kBuffers; i++) waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]);
		waveOutClose(wo);
	}
	return 0;
}

static bool loadWav(const char *name, i16 **s, u32 *n, u32 *rate)
{
	char p[360];
	rt::fmt(p, sizeof p, "%ssound\\%s", g_dir, name);
	u32 len = 0;
	char *b = rt::readfile(p, &len);
	if (!b) return false;
	u32 r = 0, ch = 0, bits = 0; char *data = 0; u32 dlen = 0;
	for (u32 o = 12; o + 8 <= len;)
	{
		u32 sz = *(u32 *)(b + o + 4);
		if (!memcmp(b + o, "fmt ", 4) && sz >= 16) { ch = *(u16 *)(b + o + 10); r = *(u32 *)(b + o + 12); bits = *(u16 *)(b + o + 22); }
		if (!memcmp(b + o, "data", 4)) { data = b + o + 8; dlen = sz; if (o + 8 + dlen > len) dlen = len - o - 8; break; }
		o += 8 + sz + (sz & 1);
	}
	if (!data || ch != 1 || bits != 16 || !r || dlen < 64) { rt::free(b); return false; }
	*s = (i16 *)data; *n = dlen / 2; *rate = r;
	return true;
}

bool start(const char *pluginDir)
{
	rt::strcpy(g_dir, pluginDir, sizeof g_dir);
	const char *skids[3] = {"skid_body.wav", "skid_mid.wav", "skid_hi.wav"};
	for (int k = 0; k < 3; k++) loadWav(skids[k], &g_skid[k].s, &g_skid[k].n, &g_skid[k].rate);
	const char *shots[3] = {"crash_light.wav", "crash_mid.wav", "crash_heavy.wav"};
	for (int k = 0; k < 3; k++) loadWav(shots[k], &g_shot[k].s, &g_shot[k].n, &g_shot[k].rate);
	for (int k = 0; k < radio::kClicks; k++)
	{
		static float tmp[8192];
		int n = radio::clickSound(k, (float)kRate, tmp, 8192);
		Loop &L = g_shot[3 + k];
		L.s = n > 1 ? (i16 *)rt::alloc((u32)n * 2) : 0;
		if (!L.s) continue;
		for (int i = 0; i < n; i++) L.s[i] = (i16)(tmp[i] * 32767.0f);
		L.n = (u32)n; L.rate = kRate;
	}
	g_an.configure((float)kRate);
	g_run = true;
	u32 tid;
	g_thread = CreateThread(0, 0, threadMain, 0, 0, &tid);
	g_radioThread = CreateThread(0, 512 * 1024, radioThreadMain, 0, 0, &tid);
	return g_thread != 0;
}
void stop() { g_run = false; }

bool loadEngine(const char *carId)
{
	lock(); rt::strcpy(g_engWant, carId, sizeof g_engWant); unlock();
	__sync_lock_test_and_set(&g_engReq, 1);
	return true;
}
void unloadEngine() { loadEngine("-none-"); }

void setEngine(bool on, float rpm, float throttle, float volume) { g_engOn = on; g_rpm = rpm; g_throttle = rt::clamp(throttle, 0, 1); g_engVol = volume; }
void setMaster(float effects, float radio) { g_effects = effects; g_radioVol = radio; }
void setRadioPan(float pan) { g_radioPan = pan; }
void setRain(float level, float muffled) { g_rainLvl = rt::clamp(level, 0, 1); g_rainMuf = rt::clamp(muffled, 0, 1); }
void setWind(float level, float muffled) { g_windLvl = rt::clamp(level, 0, 1); g_windMuf = rt::clamp(muffled, 0, 1); }
void setSkid(float amount, float pitch, float volume) { g_skidAmt = rt::clamp(amount, 0, 1); g_skidPitch = rt::clamp(pitch, 0.5f, 2.0f); g_skidVol = rt::clamp(volume, 0, 2); }
void setSkidLayers(float body, float mid, float high) { g_skidGain[0] = body; g_skidGain[1] = mid; g_skidGain[2] = high; }
bool haveSkids() { return g_skid[0].s || g_skid[1].s || g_skid[2].s; }
static void startVoice(const Loop &L, u32 from, u32 to, float volume, float pitch)
{
	if (!L.s || L.n < 2) return;
	if (to > L.n) to = L.n;
	if (from + 2 >= to) return;
	int best = 0;
	for (int k = 0; k < kVoices; k++)
	{
		if (!g_voice[k].on) { best = k; break; }
		if (g_voice[k].vol < g_voice[best].vol) best = k;
	}
	Voice &v = g_voice[best];
	v.on = false;
	v.s = L.s; v.end = to; v.rate = L.rate; v.p = (float)from; v.vol = volume; v.pitch = rt::clamp(pitch, 0.5f, 2.0f);
	v.on = true;
}
static void voice(int which, float volume, float pitch)
{
	if (which < 0 || which >= kShots) return;
	startVoice(g_shot[which], 0, g_shot[which].n, volume, pitch);
}
void playShot(int which, float volume, float pitch) { if (which >= 0 && which <= 2) voice(which, volume, pitch); }
void playClick(int kind, float volume, float pitch) { if (kind >= 0 && kind < radio::kClicks) voice(3 + kind, volume, pitch); }
void setLimiter(bool on, float hz, float drop, float cut)
{
	g_limHz = rt::clamp(hz, 2, 40); g_limDrop = rt::clamp(drop, 0, 0.3f);
	g_limCut = rt::clamp(cut, 0, 1);
	g_limOn = on;
}
float engineMinRPM() { Engine *e = g_on; return e ? e->minR : 1000; }
float engineMaxRPM() { Engine *e = g_on; return e ? e->maxR : 7000; }

void radioPlay(const char *mp3Path) { rt::strcpy(g_radioPath, mp3Path, sizeof g_radioPath); g_radioFinished = false; g_radioCmd = 1; }
void radioStop() { g_radioCmd = 2; }
void radioPause(bool paused) { g_radioCmd = paused ? 3 : 4; }
bool radioFinished() { return g_radioFinished; }
void radioVolumeState(u32 *level, u32 *mciResult) { *level = g_radioVolSet; *mciResult = g_radioVolErr; }
bool radioDecoded() { return g_rLive; }
void setSubwoofer(int level, const float *tuning)
{
	radio::Settings &st = g_dspSet;
	st.crossoverHz = tuning[0]; st.bassDbPerLevel = tuning[1]; st.subHz = tuning[2]; st.subQ = tuning[3];
	st.subDbPerLevel = tuning[4]; st.bassCeiling = tuning[5]; st.thickFrom = tuning[6]; st.thickMax = tuning[7];
	st.outsideHz = tuning[8]; st.outsideBleed = tuning[9]; st.rattleFrom = tuning[10]; st.rattleGain = tuning[11];
	st.highDbPerLevel = tuning[12]; st.highShelfHz = tuning[13]; st.highShelfDbPerLevel = tuning[14];
	g_dspSetOk = true;
	g_subLevel = level < 0 ? 0 : level > 10 ? 10 : level;
}
void setRadioOutside(float outside) { g_radioOutside = outside; }
float bassEnv() { return g_bassEnv; }
void setAnalyze(bool on)
{
	if (on && !g_analyze) for (int k = 0; k < radio::kBands; k++) g_an.env[k] = 0;
	g_analyze = on;
}
void bands(float *out) { for (int k = 0; k < radio::kBands; k++) out[k] = g_an.env[k]; }
void setExternalBass(bool on, float push) { g_extPush = rt::clamp(push, 0, 1); g_extOn = on; }
}
