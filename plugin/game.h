// Fallout: New Vegas 1.4.0.525 addresses (see sheets/hooks.json for sources).
#pragma once
#include "nvse.h"
namespace game
{
enum : u32
{
	ADDR_PLAYER = 0x11DEA3C,
	ADDR_TES = 0x11DEA10,
	ADDR_TERRAIN_HEIGHT = 0x4572E0,
	ADDR_PICK_OBJECT = 0x458440,
	ADDR_FORM_MAP = 0x11C54C0,
	ADDR_MENU_MODE = 0x702360,
	ADDR_QUEUE_UI_MESSAGE = 0x7052F0,
	ADDR_SHOW_MESSAGEBOX = 0x703E80,
	ADDR_MESSAGEBOX_CALLBACK = 0x5B4A70,
	ADDR_MESSAGEBOX_BUTTON = 0x118C684,
	ADDR_MESSAGEBOX_SCRIPTREF = 0x11CAC64,
	ADDR_TOGGLE_FIRST_PERSON = 0x950110,
	OFF_PLAYER_3RD = 0x64A, OFF_PLAYER_NODE1ST = 0x694, OFF_NODE_CHILDREN = 0xA0, OFF_NODE_CHILDCOUNT = 0xA6,
	ADDR_PROCESS_MANAGER = 0x11E0E80,
	OFF_REF_BASE = 0x20, OFF_REF_ROT = 0x24, OFF_REF_POS = 0x30, OFF_REF_CELL = 0x40, OFF_REF_RENDERSTATE = 0x64,
	OFF_RENDERSTATE_NODE = 0x14, OFF_NIAV_FLAGS = 0x30, OFF_FORM_TYPE = 4, OFF_FORM_REFID = 0x0C,
	OFF_ACTOR_LIFESTATE = 0x108, OFF_CELL_FLAGS = 0x24, OFF_CELL_WORLD = 0xC0,
	ADDR_AUDIO_MANAGER = 0x11F6EF0, OFF_AUDIO_VOLUMES = 0x140,
	ADDR_CREATE_TEXTURE_IMAGE = 0x4568C0, OFF_GEOM_MATERIAL = 0xA4, OFF_GEOM_SHADER = 0xA8, OFF_SHADER_TYPE = 0x1C,
	SHADER_PPLIGHTING = 8, OFF_PPL_TEXTURES = 0xAC, OFF_MAT_SPECULAR = 0x1C, OFF_MAT_GLOSS = 0x38,
};
template <typename T> inline T rd(u32 a) { return *(T *)a; }
inline u32 ptr(u32 a) { return a ? *(u32 *)a : 0; }
inline TESObjectREFR *player() { return (TESObjectREFR *)ptr(ADDR_PLAYER); }
inline float *pos(void *ref) { return (float *)((u32)ref + OFF_REF_POS); }
inline float *rot(void *ref) { return (float *)((u32)ref + OFF_REF_ROT); }
inline u32 refid(void *form) { return form ? rd<u32>((u32)form + OFF_FORM_REFID) : 0; }
inline u8 formtype(void *form) { return rd<u8>((u32)form + OFF_FORM_TYPE); }
inline TESForm *lookup(u32 id)
{
	u32 map = id ? ptr(ADDR_FORM_MAP) : 0;
	if (!map) return 0;
	u32 buckets = rd<u32>(map + 4), table = ptr(map + 8);
	if (!buckets || !table) return 0;
	for (u32 e = ptr(table + 4 * (id % buckets)); e; e = ptr(e))
		if (rd<u32>(e + 4) == id) return (TESForm *)ptr(e + 8);
	return 0;
}
inline bool menumode() { return ((bool(__cdecl *)())ADDR_MENU_MODE)(); }
inline void gameMessage(const char *text, float secs = 3.0f) { ((bool(__cdecl *)(const char *, u32, const char *, const char *, float, u8))ADDR_QUEUE_UI_MESSAGE)(text, 0, 0, 0, secs, 0); }
void message(const char *text, float secs = 3.0f, bool replace = true);
inline bool interior()
{
	u32 p = (u32)player(), cell = p ? ptr(p + OFF_REF_CELL) : 0;
	return !cell || (rd<u8>(cell + OFF_CELL_FLAGS) & 1);
}
inline u32 worldspace()
{
	u32 p = (u32)player(), cell = p ? ptr(p + OFF_REF_CELL) : 0;
	if (!cell || (rd<u8>(cell + OFF_CELL_FLAGS) & 1)) return 0;
	return refid((void *)ptr(cell + OFF_CELL_WORLD));
}
inline void setHidden(void *ref, bool hide)
{
	u32 rs = ref ? ptr((u32)ref + OFF_REF_RENDERSTATE) : 0;
	u32 node = rs ? ptr(rs + OFF_RENDERSTATE_NODE) : 0;
	if (!node) return;
	u32 &f = *(u32 *)(node + OFF_NIAV_FLAGS);
	f = hide ? (f | 1u) : (f & ~1u);
}
struct HiddenSet { u32 node[96]; int n; };
void hideChildren(u32 root, HiddenSet &hs);
void showChildren(u32 root, HiddenSet &hs);
inline u32 thirdPersonNode(void *ref) { u32 rs = ref ? ptr((u32)ref + OFF_REF_RENDERSTATE) : 0; return rs ? ptr(rs + OFF_RENDERSTATE_NODE) : 0; }
inline u32 firstPersonNode(void *pl) { return pl ? ptr((u32)pl + OFF_PLAYER_NODE1ST) : 0; }
inline bool thirdPerson(void *pl) { return pl && rd<u8>((u32)pl + OFF_PLAYER_3RD) != 0; }
bool terrainHeight(float x, float y, float *z);
bool rayHit(const float *from, const float *to, float *fraction);
bool rayHitObject(const float *from, const float *to, float *fraction, u32 *object);
bool rayHitNormal(const float *from, const float *to, float *fraction, u32 *object, float *n, bool *nok);
TESObjectREFR *refOfNode(u32 node);
int motionTypeOfNode(u32 node, u32 *body);
float boundRadius(u32 node);
void setBodyVelocity(u32 body, const float *vel);
void forceThirdPerson();
void setFirstPerson(bool first);
bool showMessageBox(const char *text, const char **buttons, int n);
int messageBoxButton();
template <typename F> void forEachActor(F f)
{
	u32 pm = ADDR_PROCESS_MANAGER;
	u32 node = pm + 0x80;
	for (int guard = 0; node && guard < 4096; guard++)
	{
		u32 data = rd<u32>(node), next = rd<u32>(node + 4);
		if (data) f((TESObjectREFR *)data);
		node = next;
	}
}
}
