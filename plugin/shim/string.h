// The plugin has no C runtime: these come from rt.cpp.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
void *memmove(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#ifdef __cplusplus
}
#endif
