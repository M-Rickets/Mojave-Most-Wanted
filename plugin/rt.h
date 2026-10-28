// Freestanding runtime for the Mojave Most Wanted xNVSE plugin: no C runtime, only kernel32/user32 imports.
#pragma once
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef short i16;
typedef signed char i8;
typedef long long i64;
typedef unsigned long long u64;
typedef u32 UInt32;
typedef u8 UInt8;

extern "C" {
typedef void *HANDLE;
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *, u32, u32, void *, u32, u32, HANDLE);
__declspec(dllimport) int __stdcall ReadFile(HANDLE, void *, u32, u32 *, void *);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void *, u32, u32 *, void *);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) u32 __stdcall GetFileSize(HANDLE, u32 *);
__declspec(dllimport) u32 __stdcall SetFilePointer(HANDLE, i32, i32 *, u32);
__declspec(dllimport) HANDLE __stdcall GetProcessHeap();
__declspec(dllimport) void *__stdcall HeapAlloc(HANDLE, u32, u32);
__declspec(dllimport) int __stdcall HeapFree(HANDLE, u32, void *);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64 *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64 *);
__declspec(dllimport) void __stdcall OutputDebugStringA(const char *);
__declspec(dllimport) short __stdcall GetAsyncKeyState(int);
__declspec(dllimport) u32 __stdcall MapVirtualKeyA(u32, u32);
__declspec(dllimport) int __stdcall ToAscii(u32, u32, const u8 *, u16 *, u32);
__declspec(dllimport) short __stdcall GetKeyState(int);
__declspec(dllimport) HANDLE __stdcall GetForegroundWindow();
__declspec(dllimport) u32 __stdcall GetWindowThreadProcessId(HANDLE, u32 *);
__declspec(dllimport) u32 __stdcall GetCurrentProcessId();
__declspec(dllimport) u32 __stdcall GetCurrentThreadId();
__declspec(dllimport) int __stdcall CreateDirectoryA(const char *, void *);
__declspec(dllimport) u32 __stdcall GetFileAttributesA(const char *);
__declspec(dllimport) int __stdcall DeleteFileA(const char *);
__declspec(dllimport) int __stdcall MoveFileA(const char *, const char *);
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
unsigned strlen(const char *s);
}

namespace rt
{
void *alloc(u32 n);
void free(void *p);
void strcpy(char *d, const char *s, u32 cap);
void strcat(char *d, const char *s, u32 cap);
bool streq(const char *a, const char *b);
bool startswith(const char *s, const char *p);
int strcmp(const char *a, const char *b);
int fmt(char *out, u32 cap, const char *f, ...) __attribute__((format(printf, 3, 4)));
float parsef(const char *s);
i32 parsei(const char *s);
u32 parsehex(const char *s);
char *readfile(const char *path, u32 *len);
bool writefile(const char *path, const char *data, u32 len, bool append);
double now();
float sqrt(float x);
float sin(float x);
float cos(float x);
float atan2(float y, float x);
float clamp(float v, float a, float b);
float absf(float v);
inline float max(float a, float b) { return a > b ? a : b; }
inline float min(float a, float b) { return a < b ? a : b; }
const float PI = 3.14159265f;
}
