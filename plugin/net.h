#pragma once
#include "rt.h"

namespace net
{
struct FormKey { u32 low, modHash; };
enum { kPoseBones = 100 };
struct Pose
{
	int n;
	u16 hash[kPoseBones];
	float q[kPoseBones][4];
	bool hasT[kPoseBones];
	float t[kPoseBones][3];
};
struct Skel { int n; u16 hash[kPoseBones]; float t[kPoseBones][3]; };
struct Enemy
{
	FormKey ref;
	float pos[3], yaw;
	float health;
	u8 flags;
	FormKey weapon;
	u16 shots;
	bool hasPose;
	Pose pose;
};
enum { kMaxEnemies = 8 };
enum { EV_CHAT = 1, EV_CONTAINER = 2, EV_KILL = 3, EV_DAMAGE = 4, EV_SESSIONASK = 5, EV_QUESTITEMS = 6, EV_QUESTSTEP = 7, EV_SESSIONNO = 8, EV_DOOR = 9, EV_HURT = 10, EV_REVIVE = 11, EV_CHARGEN = 12, EV_XP = 13, EV_TRADE = 14, EV_VENDOR = 15, EV_HOSTWORLD = 16, EV_OBJ = 17, EV_TALK = 18 };
struct Event { u32 id; u8 type, len; u8 data[124]; };
enum { kPeerEv = 1024 };
struct EvContainer { FormKey cont, item; i32 delta; };
struct EvKill { FormKey ref; };
struct EvDamage { FormKey ref; float amount; };
struct EvDoor { FormKey ref; u8 open, locked; };
struct EvHurt { u8 to, fromPlayer; float amount; };
struct EvRevive { u8 to, how; };
struct EvXp { u16 amount; u8 stage, fromQuest; FormKey quest; float pos[3]; u32 world, cell; };
enum { TR_OFFER = 1, TR_GIFT = 2, TR_UPDATE = 3, TR_ACCEPT = 4, TR_DECLINE = 5, TR_CANCEL = 6, TR_DONE = 7, TR_READY = 8, TR_REFUSE = 9 };
struct EvTrade { u8 op, to, n, ready; u32 id; u16 ver, seen; FormKey item[10]; u16 count[10]; };
struct EvTalk { u32 line; u8 part, parts; char speaker[24]; char text[94]; };
struct EvObj { FormKey ref; float dmg; float pos[3]; u32 world, cell; u8 op, pad[3]; };
struct EvVendor { FormKey cont; u8 n, pad[3]; FormKey item[8]; i32 delta[8]; };
struct Drop { u32 id; FormKey item; u16 count; float pos[3]; u32 world, cell; };
struct Taken { u8 owner; u32 id; };
enum { kMaxDrops = 10, kMaxTaken = 8 };
struct Self
{
	float pos[3];
	float yaw, pitch;
	float roll;
	u32 world, cell;
	u8 mode;
	u8 flags;
	i16 car;
	float speed;
	float hour;
	u32 weather;
	u8 snow;
	i16 parkCar;
	float parkPos[3], parkYaw, parkPitch, parkRoll;
	u32 parkWorld, parkCell;
	u8 ride;
	u8 invite;
	FormKey weapon;
	u16 shots;
	u8 health;
	u8 status;
	u8 downLeft;
	u8 rules;
	u8 level;
	u16 maxHealth;
	u16 dmgFor[4];
	i8 wheelOff[4]; i8 steer, wheelie, shakeZ, shakePitch, shakeRoll;
	float gunQ[4];
	bool hasPose;
	Pose pose;
};
struct Ghost
{
	u8 id;
	char name[24];
	Self s;
	float vel[3];
	float since;
	bool fresh;
};
void init(const char *iniPath);
void tick(double now, const Self &me);
int ghosts(Ghost *out, int max);
bool hostHour(float *hour);
bool hostSky(u32 *weather, u8 *snow);
bool active();
bool hosting();
const char *statusLine();
void soloTest(bool on);
bool soloTesting();
void setLoot(const Drop *mine, int n, const Taken *taken, int nt);
int peerLoot(int id, Drop *out, int max, Taken *taken, int *nt, int maxT);
u8 myId();
void setBuild(u32 build);
u32 peerBuild(int id);
void setName(const char *name);
void setSkel(const Skel &s);
bool peerSkel(int id, Skel *out);
void setEnemies(const Enemy *e, int n);
void setEnemySkel(int slot, const Skel &s);
int peerEnemies(int id, Enemy *out, int max);
bool peerEnemySkel(int id, int slot, Skel *out);
void addEvent(u8 type, const void *data, u8 len);
int peerEvents(int id, Event *out, int max);
bool peekEvent(int id, Event *out);
int takeEvents(int id, Event *out, int max);
enum { kMaxLook = 900, kMaxMods = 512 };
void setLook(const void *b, int n);
void setMods(const u32 *hashes, int n);
int peerMods(int id, u32 *out, int max);
void setFileDir(const char *dir);
void setFileWanted(bool on);
bool sendFile(int peer, const char *path, const char *name);
bool sending(u32 *have, u32 *total);
u8 sendResult();
bool receiving(char *name, int cap, u32 *have, u32 *total);
u8 lastFile(char *name, int cap);
void clearLastFile();
int peerLook(int id, void *out, int max);
void setSendHz(float hz);
void hashName(const char *name, u32 *out);
void shutdown();
bool asking(char *name, int cap, u32 *build, char *addr, int acap);
void answer(bool letIn);
bool peerPos(int id, float *pos, u32 *world, u32 *cell);
void stopNow(const char *why);
extern void (*logFn)(const char *);
}
