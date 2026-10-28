// The car's dash screen: a live equalizer with the song playing.
#pragma once
#include "rt.h"

namespace screen
{
const int W = 256, H = 128;
const int DW = 256, DH = 64;
const int PW = 128, PH = 96;
const int kBars = 24;
struct Input
{
	bool on;
	bool bandsLive;
	float bands[kBars];
	float beat;
	const char *source;
	const char *title;
	int sub;
};
void draw(u32 *px, const Input &in, float dt);
void message(u32 *px, const char *line1, const char *line2);
void drawDin(u32 *px, const Input &in, float dt);
void drawPip(u32 *px, const Input &in, float dt);
const int GW = 256, GH = 128;
struct Gauge
{
	bool lit;
	float mph;
	float rpm;
	float maxRpm;
	int gear;
};
void drawGauge(u32 *px, const Gauge &g, float dt);
}
