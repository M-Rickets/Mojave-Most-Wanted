// Your own music in the car.
#pragma once
#include "rt.h"

namespace music
{
enum { CMD_PLAY = 1, CMD_PAUSE, CMD_NEXT, CMD_PREV, CMD_VOLUP, CMD_VOLDOWN };
struct State
{
	int available;
	int sessions;
	int status;
	char app[48];
	char title[160];
	u32 titleVer;
	int volume;
	u32 volVer;
	char note[160];
	u32 noteVer;
	u32 polls;
};
void start();
void command(int cmd);
void setActive(bool on);
void state(State *out);
float level();
}
