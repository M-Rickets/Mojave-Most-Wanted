#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_ONLY_MP3
#include "third_party/minimp3/minimp3.h"
#include "mp3.h"

namespace mp3
{
static mp3dec_t g_dec;

void reset() { mp3dec_init(&g_dec); }

int frame(const u8 *data, u32 len, i16 *pcm, u32 *used, int *channels, int *hz)
{
	mp3dec_frame_info_t info;
	memset(&info, 0, sizeof info);
	int n = mp3dec_decode_frame(&g_dec, data, (int)len, pcm, &info);
	*used = (u32)info.frame_bytes;
	*channels = info.channels;
	*hz = info.hz;
	return n;
}

u32 skipTag(const u8 *d, u32 len)
{
	if (len >= 10 && d[0] == 'I' && d[1] == 'D' && d[2] == '3')
	{
		u32 sz = ((u32)(d[6] & 0x7F) << 21) | ((u32)(d[7] & 0x7F) << 14) | ((u32)(d[8] & 0x7F) << 7) | (u32)(d[9] & 0x7F);
		sz += 10 + ((d[5] & 0x10) ? 10 : 0);
		return sz < len ? sz : len;
	}
	return 0;
}
}
