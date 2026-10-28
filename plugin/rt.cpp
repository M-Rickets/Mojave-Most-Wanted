#include "rt.h"
#include <stdarg.h>

extern "C" {
int _fltused = 0;
void *memset(void *d, int c, unsigned n) { u8 *p = (u8 *)d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { u8 *p = (u8 *)d; const u8 *q = (const u8 *)s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, unsigned n)
{
	u8 *p = (u8 *)d; const u8 *q = (const u8 *)s;
	if (p < q) while (n--) *p++ = *q++;
	else if (p > q) { p += n; q += n; while (n--) *--p = *--q; }
	return d;
}
int memcmp(const void *a, const void *b, unsigned n) { const u8 *p = (const u8 *)a, *q = (const u8 *)b; for (; n; n--, p++, q++) if (*p != *q) return *p - *q; return 0; }
unsigned strlen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
}

namespace rt
{
void *alloc(u32 n) { return HeapAlloc(GetProcessHeap(), 8 /*zero*/, n ? n : 1); }
void free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
void strcpy(char *d, const char *s, u32 cap) { u32 i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
void strcat(char *d, const char *s, u32 cap) { u32 n = strlen(d); strcpy(d + n, s, cap > n ? cap - n : 0); }
bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(u8)*a - (int)(u8)*b;
}
bool startswith(const char *s, const char *p) { while (*p) if (*s++ != *p++) return false; return true; }

static void put(char *out, u32 cap, u32 &n, char c) { if (n + 1 < cap) out[n] = c; n++; }
static void putu(char *out, u32 cap, u32 &n, u32 v, u32 base, bool upper, int width)
{
	char tmp[16]; int k = 0;
	do { u32 d = v % base; tmp[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v && k < 16);
	while (k < width) tmp[k++] = '0';
	while (k) put(out, cap, n, tmp[--k]);
}
int fmt(char *out, u32 cap, const char *f, ...)
{
	va_list ap; va_start(ap, f); u32 n = 0;
	for (; *f; f++)
	{
		if (*f != '%') { put(out, cap, n, *f); continue; }
		f++;
		int width = 0, prec = -1;
		while (*f == '-' || *f == '+' || *f == ' ' || *f == '#') f++;
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		if (*f == '.') { f++; prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
		while (*f == 'l' || *f == 'h') f++;
		if (!*f) break;
		switch (*f)
		{
		case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; while (*s) put(out, cap, n, *s++); break; }
		case 'd': { i32 v = va_arg(ap, i32); if (v < 0) { put(out, cap, n, '-'); v = -v; } putu(out, cap, n, (u32)v, 10, false, width); break; }
		case 'u': putu(out, cap, n, va_arg(ap, u32), 10, false, width); break;
		case 'x': putu(out, cap, n, va_arg(ap, u32), 16, false, width); break;
		case 'X': putu(out, cap, n, va_arg(ap, u32), 16, true, width); break;
		case 'c': put(out, cap, n, (char)va_arg(ap, int)); break;
		case 'f':
		{
			double v = va_arg(ap, double);
			if (v != v) { put(out, cap, n, 'n'); put(out, cap, n, 'a'); put(out, cap, n, 'n'); break; }
			if (v < 0) { put(out, cap, n, '-'); v = -v; }
			if (v > 4e9) v = 4e9;
			int dp = prec < 0 ? 3 : (prec > 6 ? 6 : prec);
			u32 scale = 1; for (int i = 0; i < dp; i++) scale *= 10;
			u32 ip = (u32)v; u32 fp = (u32)((v - ip) * scale + 0.5);
			if (fp >= scale) { ip++; fp -= scale; }
			putu(out, cap, n, ip, 10, false, 0);
			if (dp > 0) { put(out, cap, n, '.'); putu(out, cap, n, fp, 10, false, dp); }
			break;
		}
		case '%': put(out, cap, n, '%'); break;
		default: put(out, cap, n, *f); break;
		}
	}
	if (cap) out[n < cap ? n : cap - 1] = 0;
	va_end(ap);
	return (int)(n < cap ? n : (cap ? cap - 1 : 0));
}
float parsef(const char *s)
{
	while (*s == ' ') s++;
	float sign = 1; if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
	double v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	if (*s == '.') { s++; double p = 0.1; while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * p; p *= 0.1; } }
	if (*s == 'e' || *s == 'E')
	{
		s++;
		int es = 1; if (*s == '-') { es = -1; s++; } else if (*s == '+') s++;
		int e = 0; while (*s >= '0' && *s <= '9' && e < 400) e = e * 10 + (*s++ - '0');
		for (; e > 0; e--) v = es > 0 ? v * 10 : v / 10;
	}
	return (float)(v * sign);
}
i32 parsei(const char *s) { while (*s == ' ') s++; i32 sign = 1; if (*s == '-') { sign = -1; s++; } i32 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v * sign; }
u32 parsehex(const char *s)
{
	while (*s == ' ') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	u32 v = 0;
	for (;; s++)
	{
		char c = *s; u32 d;
		if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break;
		v = v * 16 + d;
	}
	return v;
}
char *readfile(const char *path, u32 *len)
{
	HANDLE h = CreateFileA(path, 0x80000000 /*GENERIC_READ*/, 1 | 2, 0, 3 /*OPEN_EXISTING*/, 0x80, 0);
	if (h == (HANDLE)-1) return 0;
	u32 n = GetFileSize(h, 0), got = 0;
	char *buf = (char *)alloc(n + 1);
	if (buf && n) ReadFile(h, buf, n, &got, 0);
	CloseHandle(h);
	if (!buf) return 0;
	buf[got] = 0;
	if (len) *len = got;
	return buf;
}
bool writefile(const char *path, const char *data, u32 len, bool append)
{
	HANDLE h = CreateFileA(path, 0x40000000 /*GENERIC_WRITE*/, 1, 0, append ? 4 /*OPEN_ALWAYS*/ : 2 /*CREATE_ALWAYS*/, 0x80, 0);
	if (h == (HANDLE)-1) return false;
	if (append) SetFilePointer(h, 0, 0, 2 /*FILE_END*/);
	u32 w = 0; WriteFile(h, data, len, &w, 0);
	CloseHandle(h);
	return w == len;
}
double now()
{
	static i64 freq = 0; i64 c;
	if (!freq) QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&c);
	return (double)c / (double)freq;
}
float sqrt(float x) { return x > 0 ? __builtin_sqrtf(x) : 0; }
float absf(float v) { return v < 0 ? -v : v; }
float clamp(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float sin(float x)
{
	const float TWO_PI = 6.28318531f;
	if (x > PI || x < -PI) { float k = (float)(i32)(x / TWO_PI); x -= k * TWO_PI; if (x > PI) x -= TWO_PI; else if (x < -PI) x += TWO_PI; }
	if (x > PI / 2) x = PI - x; else if (x < -PI / 2) x = -PI - x;
	float x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
}
float cos(float x) { return sin(x + PI / 2); }
float atan2(float y, float x)
{
	if (x == 0 && y == 0) return 0;
	float ax = absf(x), ay = absf(y);
	float a = (ax > ay ? ay / ax : ax / ay);
	float s = a * a;
	float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
	if (ay > ax) r = PI / 2 - r;
	if (x < 0) r = PI - r;
	if (y < 0) r = -r;
	return r;
}
}
