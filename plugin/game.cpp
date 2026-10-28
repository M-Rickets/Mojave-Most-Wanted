#include "game.h"
namespace game
{
struct alignas(16) RayCastData
{
	float pos0[4], pos1[4];
	u8 byte20, pad21[3];
	u32 layerInfo;
	u32 unk28[6];
	float hitFraction;
	u32 unk44[15];
	void *cdBody;
	u32 unk84[3];
	float vector90[4];
	u32 unkA0[3];
	u8 byteAC, padAD[3];
};
static_assert(sizeof(RayCastData) == 0xB0, "RayCastData");
const float kHavok = 0.142857f;

bool terrainHeight(float x, float y, float *z)
{
	void *tes = (void *)ptr(ADDR_TES);
	if (!tes) return false;
	float xy[2] = {x, y};
	return ((bool(__thiscall *)(void *, float *, float *))ADDR_TERRAIN_HEIGHT)(tes, xy, z);
}

bool rayHit(const float *from, const float *to, float *fraction) { u32 o; return rayHitObject(from, to, fraction, &o); }

TESObjectREFR *refOfNode(u32 node)
{
	for (int depth = 0; node && depth < 64; depth++)
	{
		if (ptr(node) == 0x10A8F90 && ptr(node + 0xCC)) return (TESObjectREFR *)ptr(node + 0xCC);
		node = ptr(node + 0x18);
	}
	return 0;
}

int motionTypeOfNode(u32 node, u32 *body)
{
	*body = 0;
	for (int depth = 0; node && depth < 12; depth++)
	{
		u32 co = ptr(node + 0x1C), wo = co ? ptr(co + 0x10) : 0, hk = wo ? ptr(wo + 8) : 0;
		if (hk && rd<u8>(hk + 0x28) == 1) { *body = hk; return rd<u8>(hk + 0xE8); }
		if (ptr(node) == 0x10A8F90) break;
		node = ptr(node + 0x18);
	}
	return -1;
}
float boundRadius(u32 node)
{
	u32 b = node ? ptr(node + 0x20) : 0;
	return b ? rd<float>(b + 12) : 0;
}
void setBodyVelocity(u32 body, const float *vel)
{
	if (!body) return;
	float *v = (float *)(body + 0x1B0);
	v[0] = vel[0] * kHavok; v[1] = vel[1] * kHavok; v[2] = vel[2] * kHavok; v[3] = 0;
	((void(__thiscall *)(u32))0xC9C1D0)(body);
}

bool rayHitObject(const float *from, const float *to, float *fraction, u32 *object)
{
	*object = 0;
	u32 pl = (u32)player();
	void *tes = (void *)ptr(ADDR_TES);
	if (!pl || !tes) return false;
	RayCastData rc;
	memset(&rc, 0, sizeof rc);
	for (int i = 0; i < 3; i++) { rc.pos0[i] = from[i] * kHavok; rc.pos1[i] = to[i] * kHavok; }
	rc.hitFraction = 1.0f;
	rc.unk44[0] = 0xFFFFFFFF;
	rc.unk44[3] = 0xFFFFFFFF;
	u32 chain = ptr(pl + 0x68);
	const u32 offs[3] = {0x138, 0x594, 0x8};
	for (int i = 0; i < 3; i++) chain = chain ? ptr(chain + offs[i]) : 0;
	u32 group = chain ? rd<u32>(chain + 0x2C) : 0;
	rc.layerInfo = (group & 0xFFFF0000u) | 6;
	void *hitNode = ((void *(__thiscall *)(void *, RayCastData *, bool))ADDR_PICK_OBJECT)(tes, &rc, true);
	if (rc.hitFraction < 1.0f && rc.hitFraction >= 0.0f) { *fraction = rc.hitFraction; *object = (u32)hitNode; return true; }
	return false;
}

bool rayHitNormal(const float *from, const float *to, float *fraction, u32 *object, float *n, bool *nok)
{
	*object = 0; *nok = false;
	u32 pl = (u32)player();
	void *tes = (void *)ptr(ADDR_TES);
	if (!pl || !tes) return false;
	RayCastData rc;
	memset(&rc, 0, sizeof rc);
	for (int i = 0; i < 3; i++) { rc.pos0[i] = from[i] * kHavok; rc.pos1[i] = to[i] * kHavok; }
	rc.hitFraction = 1.0f;
	rc.unk44[0] = 0xFFFFFFFF;
	rc.unk44[3] = 0xFFFFFFFF;
	u32 chain = ptr(pl + 0x68);
	const u32 offs[3] = {0x138, 0x594, 0x8};
	for (int i = 0; i < 3; i++) chain = chain ? ptr(chain + offs[i]) : 0;
	u32 group = chain ? rd<u32>(chain + 0x2C) : 0;
	rc.layerInfo = (group & 0xFFFF0000u) | 6;
	void *hitNode = ((void *(__thiscall *)(void *, RayCastData *, bool))ADDR_PICK_OBJECT)(tes, &rc, true);
	if (!(rc.hitFraction < 1.0f && rc.hitFraction >= 0.0f)) return false;
	*fraction = rc.hitFraction; *object = (u32)hitNode;
	float x = rc.vector90[0], y = rc.vector90[1], z = rc.vector90[2], l = x * x + y * y + z * z;
	if (l > 0.9f && l < 1.1f) { n[0] = x; n[1] = y; n[2] = z; *nok = true; }
	return true;
}

void hideChildren(u32 root, HiddenSet &hs)
{
	if (!root) return;
	u32 data = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; data && i < n && i < 256; i++)
	{
		u32 c = ptr(data + 4 * i);
		if (!c) continue;
		u32 &f = *(u32 *)(c + OFF_NIAV_FLAGS);
		if (f & 1) continue;
		bool known = false;
		for (int k = 0; k < hs.n; k++) if (hs.node[k] == c) known = true;
		if (!known && hs.n >= 96) continue;
		f |= 1;
		if (!known) hs.node[hs.n++] = c;
	}
}

void showChildren(u32 root, HiddenSet &hs)
{
	if (root)
	{
		u32 data = ptr(root + OFF_NODE_CHILDREN);
		u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
		for (u16 i = 0; data && i < n && i < 256; i++)
		{
			u32 c = ptr(data + 4 * i);
			if (!c) continue;
			for (int k = 0; k < hs.n; k++)
				if (hs.node[k] == c) { *(u32 *)(c + OFF_NIAV_FLAGS) &= ~1u; break; }
		}
	}
	hs.n = 0;
}

void setFirstPerson(bool first)
{
	u32 pl = (u32)player();
	if (pl && (rd<u8>(pl + OFF_PLAYER_3RD) != 0) == first)
		((bool(__thiscall *)(void *, bool))ADDR_TOGGLE_FIRST_PERSON)((void *)pl, first);
}

void forceThirdPerson()
{
	u32 pl = (u32)player();
	if (pl && !rd<u8>(pl + OFF_PLAYER_3RD))
		((bool(__thiscall *)(void *, bool))ADDR_TOGGLE_FIRST_PERSON)((void *)pl, false);
}

static double s_boxShownAt = -9;
bool showMessageBox(const char *text, const char **b, int n)
{
	if (rt::now() - s_boxShownAt < 0.25 && *(u8 *)ADDR_MESSAGEBOX_BUTTON == 0xFF) return false;
	const char *bb[10] = {0};
	for (int i = 0; i < n && i < 10; i++) bb[i] = b[i];
	*(u8 *)ADDR_MESSAGEBOX_BUTTON = 0xFF;
	u32 pl = (u32)player();
	*(u32 *)ADDR_MESSAGEBOX_SCRIPTREF = refid((void *)pl);
	typedef bool(__cdecl * ShowFn)(const char *, u32, u32, void *, u32, u32, float, float, ...);
	bool ok = ((ShowFn)ADDR_SHOW_MESSAGEBOX)(text, 0, 0, (void *)ADDR_MESSAGEBOX_CALLBACK, 0, 0x17, 0.0f, 0.0f,
		bb[0], bb[1], bb[2], bb[3], bb[4], bb[5], bb[6], bb[7], bb[8], bb[9], (const char *)0);
	if (ok) s_boxShownAt = rt::now();
	return ok;
}

int messageBoxButton()
{
	u8 b = *(u8 *)ADDR_MESSAGEBOX_BUTTON;
	return b == 0xFF ? -1 : (int)b;
}
}
