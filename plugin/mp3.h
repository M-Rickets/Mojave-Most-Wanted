// MP3 decoding for the car radio (minimp3, CC0: third_party/minimp3).
#pragma once
#include "rt.h"

namespace mp3
{
const int kMaxSamples = 1152 * 2;
void reset();
int frame(const u8 *data, u32 len, i16 *pcm, u32 *used, int *channels, int *hz);
u32 skipTag(const u8 *data, u32 len);
}
