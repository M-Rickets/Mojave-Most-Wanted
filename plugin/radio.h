// The car radio's sound processing: the subwoofer (a deep, clean bass boost that gets thicker towards the top), the
// car's panels rattling on the bass, and the muffled boom heard from outside the car.
#pragma once
#include "rt.h"

namespace radio
{
struct Biquad
{
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	float z1[2] = {0, 0}, z2[2] = {0, 0};
	void lowShelf(float fs, float hz, float db);
	void peak(float fs, float hz, float q, float db);
	void lowpass(float fs, float hz, float q);
	void bandpass(float fs, float hz, float q);
	void highpass(float fs, float hz, float q);
	void highShelf(float fs, float hz, float db);
	inline float run(int ch, float x)
	{
		float y = b0 * x + z1[ch];
		z1[ch] = b1 * x - a1 * y + z2[ch];
		z2[ch] = b2 * x - a2 * y;
		return y;
	}
};

const int kSettings = 15;
struct Settings
{
	float crossoverHz;
	float bassDbPerLevel;
	float subHz, subQ, subDbPerLevel;
	float bassCeiling;
	float thickFrom, thickMax;
	float outsideHz, outsideBleed;
	float rattleFrom, rattleGain;
	float highDbPerLevel;
	float highShelfHz, highShelfDbPerLevel;
};

enum { CLICK_POWER, CLICK_BUTTON, CLICK_DETENT, CLICK_DETENT_LOW, CLICK_SWITCH, kClicks };
int clickSound(int kind, float rate, float *out, int cap);

const int kBands = 24;
struct Analyzer
{
	Biquad bp[kBands];
	float env[kBands] = {0};
	float fs = 0;
	void configure(float rate);
	void push(float mono);
};

float dbGain(float db);
float tanhf(float x);
float softClip(float x);

struct Panel
{
	float threshold, gain, chatter;
	Biquad res[2];
	float last, timer, soft;
};

struct Dsp
{
	Settings s;
	int level = -1;
	float fs = 48000;
	Biquad xo1, xo2, xh1, xh2, sub, thickLp, outLp, hiShelf, rattleLp;
	float bassGain = 1, drive = 0, highGain = 1;
	float bassEnvC = 0, limEnv = 0;
	float outside = 0;
	float env = 0;
	Panel panel[4];
	u32 rnd = 0x2545F491;
	void configure(const Settings &settings, int lvl, float rate);
	void process(float &l, float &r, float outsideTarget);
	float panels(float push);
};
}
