// The dash screen's video
#pragma once
#include "rt.h"

namespace mirror
{
enum Status { NONE, NO_WINDOW, MINIMIZED, BLACK, OK, SEARCHING };
void setActive(bool on);
int frame(u32 *px, int w, int h, u32 *ver);
const char *windowTitle();
bool lastNote(char *out, int cap, u32 *ver);
void gameWindowInfo(char *out, int cap);
bool makeBorderless(char *out, int cap);
}
