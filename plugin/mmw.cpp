// Mojave Most Wanted: xNVSE plugin.
#include "nvse.h"
#include "game.h"
#include "sheets_gen.h"
#include "audio.h"
#include "music.h"
#include "screen.h"
#include "mirror.h"
#include "radio.h"
#include "rider.h"
#include "net.h"

using namespace game;

static PluginHandle g_handle;
static NVSEScriptInterface *g_script;
static NVSEConsoleInterface *g_console;
static NVSECommandTableInterface *g_cmds;
static bool cmdExists(const char *name) { return g_cmds && g_cmds->GetByName && g_cmds->GetByName(name); }
static NVSESerializationInterface *g_serial;
static char g_dir[260];
static char g_logPath[300];
static char g_pluginDir[300];

// ---- logging
static char g_logBuf[16384];
static u32 g_logLen;
static double g_logAt;
static volatile long g_logLock;
static void logFlushLocked()
{
	if (g_logLen) rt::writefile(g_logPath, g_logBuf, g_logLen, true);
	g_logLen = 0;
	g_logAt = rt::now();
}
static void logFlush()
{
	for (int i = 0; __sync_lock_test_and_set(&g_logLock, 1); i++) if (i > 100000) return;
	logFlushLocked();
	__sync_lock_release(&g_logLock);
}
static void logs(const char *s)
{
	char line[600];
	u32 n = (u32)rt::fmt(line, sizeof line, "[%f] %s\r\n", rt::now(), s);
	if (n >= sizeof line) n = (u32)strlen(line);
	for (int i = 0; __sync_lock_test_and_set(&g_logLock, 1); i++) if (i > 100000) { rt::writefile(g_logPath, line, n, true); return; }
	if (g_logLen + n > sizeof g_logBuf) logFlushLocked();
	memcpy(g_logBuf + g_logLen, line, n);
	g_logLen += n;
	if (rt::now() - g_logAt > 1.0) logFlushLocked();
	__sync_lock_release(&g_logLock);
}
#define LOG(...) do { char _b[512]; rt::fmt(_b, sizeof _b, __VA_ARGS__); logs(_b); } while (0)

// ---- crash report
extern "C" {
__declspec(dllimport) HANDLE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) HANDLE __stdcall GetModuleHandleA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(HANDLE, const char *);
__declspec(dllimport) void __stdcall Sleep(u32);
__declspec(dllimport) HANDLE __stdcall CreateThread(void *, u32, u32(__stdcall *)(void *), void *, u32, u32 *);
}
static const char *volatile g_crumb = "loading";
static double g_gearChangedAt = -99;
#define CRUMB(s) (g_crumb = (s))
static void *(__stdcall *s_vq)(const void *, void *, u32);
static u32(__stdcall *s_modName)(void *, char *, u32);
static void *(__stdcall *s_addVeh)(u32, long(__stdcall *)(void *));
static u32 g_ourBase;
static bool addrInfo(u32 a, char *out, u32 cap, bool needExec)
{
	u32 mbi[7] = {0};
	if (!s_vq || !s_vq((const void *)a, mbi, sizeof mbi) || mbi[4] != 0x1000) return false;
	if (needExec && !(mbi[5] & 0xF0)) return false;
	u32 base = mbi[1];
	char path[260]; path[0] = 0;
	if (!base || !s_modName || !s_modName((void *)base, path, sizeof path)) return false;
	const char *nm = path;
	for (const char *q = path; *q; q++) if (*q == '\\' || *q == '/') nm = q + 1;
	rt::fmt(out, cap, "%s+%x", nm, a - base);
	return true;
}
extern "C" { volatile u32 g_gOn, g_gEsp, g_gEbp, g_gFault, g_gThread, g_gFaultEip; u32 __cdecl guardedCall2(u32 fn, u32 a, u32 b); void guardResume(); }
__asm__(
	".text\n"
	".globl _guardedCall2\n"
	"_guardedCall2:\n"
	"  pushl %ebp\n  movl %esp, %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
	"  movl %ebp, _g_gEbp\n  movl %esp, _g_gEsp\n"
	"  movl $0, _g_gFault\n  movl $1, _g_gOn\n"
	"  pushl 16(%ebp)\n  pushl 12(%ebp)\n  call *8(%ebp)\n  addl $8, %esp\n"
	"  movl $0, _g_gOn\n"
	"  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n"
	".globl _guardResume\n"
	"_guardResume:\n"
	"  movl _g_gEsp, %esp\n  movl _g_gEbp, %ebp\n  movl $0, _g_gOn\n  movl $1, _g_gFault\n  xorl %eax, %eax\n"
	"  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n");
static long __stdcall crashHandler(void *ep)
{
	static int logged;
	static u32 seen[8];
	u32 *er = *(u32 **)ep;
	u8 *ctx = *(u8 **)((u8 *)ep + 4);
	u32 code = er ? er[0] : 0;
	if (g_gOn && code == 0xC0000005 && GetCurrentThreadId() == g_gThread)
	{
		g_gOn = 0;
		g_gFaultEip = *(u32 *)(ctx + 0xB8);
		*(u32 *)(ctx + 0xB8) = (u32)&guardResume;
		return -1;
	}
	if (code != 0xC0000005 && code != 0xC000001D && code != 0xC00000FD && code != 0xC0000094 && code != 0xC0000096 && code != 0xC0000409) return 0;
	u32 eip = *(u32 *)(ctx + 0xB8), esp = *(u32 *)(ctx + 0xC4);
	for (int i = 0; i < logged; i++) if (seen[i] == eip) return 0;
	if (logged >= 8) return 0;
	seen[logged++] = eip;
	char where[300] = "?";
	addrInfo(eip, where, sizeof where, false);
	LOG("EXCEPTION %08x at %08x (%s) %s %08x - last: %s", code, eip, where, er[5] == 1 ? "writing" : er[5] == 8 ? "running" : "reading", er[6], g_crumb);
	LOG("  eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x", *(u32 *)(ctx + 0xB0), *(u32 *)(ctx + 0xA4),
		*(u32 *)(ctx + 0xAC), *(u32 *)(ctx + 0xA8), *(u32 *)(ctx + 0xA0), *(u32 *)(ctx + 0x9C), *(u32 *)(ctx + 0xB4), esp);
	u32 mbi[7] = {0};
	if (s_vq && s_vq((const void *)esp, mbi, sizeof mbi))
	{
		u32 top = mbi[0] + mbi[3];
		char line[480]; int n = 0, hits = 0;
		for (u32 a = esp; a + 4 <= top && a < esp + 4096 && hits < 24; a += 4)
		{
			u32 v = *(u32 *)a;
			if (v < 0x10000) continue;
			char w[120];
			if (!addrInfo(v, w, sizeof w, true)) continue;
			n += rt::fmt(line + n, sizeof line - n, " %s", w);
			hits++;
			if (n > 380) { LOG("  stack:%s", line); n = 0; }
		}
		if (n) LOG("  stack:%s", line);
	}
	{
		logFlush();
		u32 ln = 0;
		char *all = rt::readfile(g_logPath, &ln), cp[320];
		rt::fmt(cp, sizeof cp, "%sData\\NVSE\\Plugins\\MojaveMostWanted-crash.log", g_dir);
		if (all) { rt::writefile(cp, all, ln, false); rt::free(all); }
	}
	return 0;
}
static void crashReportStart()
{
	HANDLE k = LoadLibraryA("kernel32.dll");
	if (!k) return;
	s_vq = (void *(__stdcall *)(const void *, void *, u32))GetProcAddress(k, "VirtualQuery");
	s_modName = (u32(__stdcall *)(void *, char *, u32))GetProcAddress(k, "GetModuleFileNameA");
	s_addVeh = (void *(__stdcall *)(u32, long(__stdcall *)(void *)))GetProcAddress(k, "AddVectoredExceptionHandler");
	if (s_addVeh) s_addVeh(1, crashHandler);
	u32 mbi[7] = {0};
	if (s_vq && s_vq((const void *)&crashReportStart, mbi, sizeof mbi)) g_ourBase = mbi[1];
	LOG("crash report on (this plugin at %08x)", g_ourBase);
}

// Can n bytes at p be read (committed, readable, no guard page)? - for looking through a game record's fields for a
// pointer of a known kind
static bool readable(u32 p, u32 n)
{
	if (!s_vq || p < 0x10000) return false;
	u32 mbi[7] = {0};
	if (!s_vq((const void *)p, mbi, sizeof mbi)) return false;
	u32 prot = mbi[5];
	if (mbi[4] != 0x1000 || (prot & 0x101) || !(prot & 0xEE)) return false;
	return p + n <= mbi[0] + mbi[3];
}
// ---- Config (written by the launcher)
static char *g_ini;
// mmw.ini lookups: an index of its keys (hash -> value), rebuilt when it's loaded
static u32 g_iniGen = 1;
static u32 g_iniIdxGen;
static struct IniKey { u32 h; const char *line; u32 kl; } g_iniIdx[4096];
static u32 iniHash(const char *k, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) h = (h ^ (u8)k[i]) * 16777619u; return h ? h : 1; }
static void iniIndex()
{
	g_iniIdxGen = g_iniGen;
	memset(g_iniIdx, 0, sizeof g_iniIdx);
	if (!g_ini) return;
	for (const char *p = g_ini; *p;)
	{
		const char *e = p; while (*e && *e != '\n' && *e != '\r') e++;
		const char *q = p; while (q < e && *q != '=') q++;
		if (q < e && q > p)
		{
			u32 kl = (u32)(q - p), h = iniHash(p, kl);
			for (u32 i = h & 4095, n = 0; n < 4096; i = (i + 1) & 4095, n++)
				if (!g_iniIdx[i].h) { g_iniIdx[i].h = h; g_iniIdx[i].line = p; g_iniIdx[i].kl = kl; break; }
				else if (g_iniIdx[i].h == h && g_iniIdx[i].kl == kl && !memcmp(g_iniIdx[i].line, p, kl)) break;
		}
		p = e; while (*p == '\n' || *p == '\r') p++;
	}
}
static const char *iniGet(const char *key)
{
	if (!g_ini) return 0;
	if (g_iniIdxGen != g_iniGen) iniIndex();
	u32 kl = strlen(key), h = iniHash(key, kl);
	for (u32 i = h & 4095, n = 0; n < 4096 && g_iniIdx[i].h; i = (i + 1) & 4095, n++)
		if (g_iniIdx[i].h == h && g_iniIdx[i].kl == kl && !memcmp(g_iniIdx[i].line, key, kl)) return g_iniIdx[i].line + kl + 1;
	return 0;
}
static float iniF(const char *key, float def) { const char *v = iniGet(key); return v ? rt::parsef(v) : def; }
// Sheets/tuning.json value, overridable by mmw.ini for tests.
static u32 g_tuneGen = 1;
static float feelScale(const char *id);
#define TUNE(n) ([]() -> float { static float v_; static u32 g_; if (g_ != g_tuneGen) { v_ = iniF(#n, kT_##n) * feelScale(#n); g_ = g_tuneGen; } return v_; }())
static int iniI(const char *key, int def) { const char *v = iniGet(key); return v ? rt::parsei(v) : def; }
static void iniStr(const char *key, char *out, u32 cap, const char *def)
{
	const char *v = iniGet(key);
	if (!v) { rt::strcpy(out, def, cap); return; }
	u32 n = 0; while (v[n] && v[n] != '\r' && v[n] != '\n' && n + 1 < cap) { out[n] = v[n]; n++; }
	out[n] = 0;
}

// ---- scripts
static Script *s_placeN, *s_shot;
// A container's count changed by a friend; a death; a fighter held still; its health; a friend's hit; chat
static Script *s_contAdj, *s_kill, *s_restrain, *s_healthPct, *s_dmgPct;
static Script *s_setXY, *s_place, *s_move, *s_remove, *s_sound, *s_hurt, *s_hit, *s_modIndex, *s_getCam[4], *s_hudRGB[3], *s_mate, *s_ride, *s_weaponOut;
static u32 g_mod = 0xFF;
static bool g_ready;
#define PLUGIN_BUILD 295
#define STR2(x) #x
#define STR(x) STR2(x)
static const u32 kPluginBuild = PLUGIN_BUILD;

// Function scripts in the inline-parameter style (Begin Function {ref r,...}).
static const char *kPlace =
	"Begin Function {ref rBase, float fx, float fy, float fz, float faz}\n"
	"ref rNew = Player.PlaceAtMe rBase 1\n"
	"if rNew\nrNew.SetAngle Z faz\nrNew.SetPos X fx\nrNew.SetPos Y fy\nrNew.SetPos Z fz\nendif\n"
	"SetFunctionValue rNew\nEnd";
static const char *kMove =
	"Begin Function {ref rObj, float fx, float fy, float fz, float fax, float fay, float faz}\n"
	"rObj.SetAngle X fax\nrObj.SetAngle Y fay\nrObj.SetAngle Z faz\nrObj.SetPos X fx\nrObj.SetPos Y fy\nrObj.SetPos Z fz\nEnd";
static const char *kPlaceN =
	"Begin Function {ref rBase, float fn, float fx, float fy, float fz}\n"
	"ref rNew = Player.PlaceAtMe rBase fn\n"
	"if rNew\nrNew.SetPos X fx\nrNew.SetPos Y fy\nrNew.SetPos Z fz\nendif\n"
	"SetFunctionValue rNew\nEnd";
static const char *kShot = "Begin Function {ref rM, ref rS, float fx, float fy, float fz}\nrM.SetPos X fx\nrM.SetPos Y fy\nrM.SetPos Z fz\nrM.PlaySound3D rS\nEnd";
static const char *kContAdj = "Begin Function {ref rC, ref rI, float fN}\nif fN > 0\nrC.AddItem rI fN 1\nelse\nset fN to 0 - fN\nrC.RemoveItem rI fN 1\nendif\nEnd";
static const char *kKill = "Begin Function {ref rA}\nrA.Kill\nEnd";
static const char *kRestrain = "Begin Function {ref rA, float f}\nif f > 0\nrA.SetRestrained 1\nelse\nrA.SetRestrained 0\nendif\nEnd";
static const char *kHealthPct = "Begin Function {ref rA}\nSetFunctionValue (rA.GetHealthPercentage)\nEnd";
static const char *kDmgPct = "Begin Function {ref rA, float f}\nfloat fB\nlet fB := rA.GetBaseAV Health\nlet fB := fB * f\nrA.DamageAV Health fB\nEnd";
// The chat box is ours now (JIP's text menu never showed on their PCs): typed keys are read here and the box is drawn
// with the F8 menu's lettering; while it's open the game's keys are off (DisableKey, put back after)
static const char *kKeyOff = "Begin Function {float k}\nif IsKeyDisabled k\nSetFunctionValue 1\nelse\nDisableKey k\nSetFunctionValue 0\nendif\nEnd";
static const char *kKeyOn = "Begin Function {float k}\nEnableKey k\nEnd";
static Script *s_keyOff, *s_keyOn;
static const char *kSetXY = "Begin Function {ref rObj, float fx, float fy}\nrObj.SetPos X fx\nrObj.SetPos Y fy\nEnd";
static const char *kRemove = "Begin Function {ref rObj}\nrObj.Disable\nrObj.MarkForDelete\nEnd";
static const char *kSound = "Begin Function {ref rObj, ref rSound}\nrObj.PlaySound3D rSound\nEnd";
static const char *kHurt = "Begin Function {ref rActor, float fDmg}\nrActor.DamageAV Health fDmg\nEnd";
// Your car hits someone: knocked over, away from the car (rSrc); hurt unless they're your companion, who is only
// knocked over by a real hit (fDmg > 0), not by a shove while you park.
static const char *kHit =
	"Begin Function {ref rSrc, ref rActor, float fForce, float fDmg}\n"
	"int iMate = rActor.GetPlayerTeammate\n"
	"if iMate == 0 || fDmg > 0\nrSrc.PushActorAway rActor fForce\nendif\n"
	"if iMate == 0 && fDmg > 0\nrActor.DamageAV Health fDmg\nendif\n"
	"SetFunctionValue iMate\nEnd";
static const char *kModIndex = "Begin Function {}\nSetFunctionValue (GetModIndex \"MojaveMostWanted.esp\")\nEnd";
static const char *kMate = "Begin Function {ref rActor}\nSetFunctionValue (rActor.GetPlayerTeammate)\nEnd";
// reloads: the rounds lost with a dropped magazine are taken out of your inventory (silently), and the count left is
// returned - the clip is then set from it
static const char *kAmmo = "Begin Function {ref rAmmo, float fLost}\nif fLost > 0\nPlayer.RemoveItem rAmmo fLost 1\nendif\nSetFunctionValue (Player.GetItemCount rAmmo)\nEnd";
static Script *s_ammo;
// A stand-in made from a copy of your own character record, dressed in a friend's look and clothes, whose body is
// copied and then removed
static const char *kStrip = "Begin Function {ref rA}\nrA.SetRestrained 1\nrA.RemoveAllItems\nEnd";
static const char *kDressItem = "Begin Function {ref rA, ref rI}\nrA.AddItem rI 1 1\nrA.EquipItem rI 0 1\nEnd";
static Script *s_strip, *s_dressItem;
// The dropkick's hit: knocked flying, hurt by a share of their full health but never below 1 - never your companion.
static const char *kDropHit =
	"Begin Function {ref rA, float fForce, float fPct}\n"
	"float fB\nfloat fH\nint iDone\n"
	"if (rA.GetPlayerTeammate) == 0\n"
	"let fB := (rA.GetBaseAV Health) * fPct\nlet fH := rA.GetAV Health\n"
	"if fB > fH - 1\nlet fB := fH - 1\nendif\n"
	"if fB > 0\nrA.DamageAV Health fB\nendif\n"
	"Player.PushActorAway rA fForce\nlet iDone := 1\nendif\n"
	"SetFunctionValue iDone\nEnd";
static Script *s_dropHit;
// A game vehicle in the world taken (hidden while it's yours: Disable, never deleted) or put back
static const char *kDisable = "Begin Function {ref rObj}\nrObj.Disable\nEnd";
static const char *kEnable = "Begin Function {ref rObj}\nrObj.Enable\nEnd";
// What's in your crosshair; a door's state (GetOpenState: 1 open, 2 opening, 3 closed, 4 closing; +10 locked) and
// setting it
static const char *kCrossRef = "Begin Function {}\nSetFunctionValue (GetCrosshairRef)\nEnd";
static const char *kDoorGet = "Begin Function {ref rD}\nint i\nlet i := rD.GetOpenState\nif rD.GetLocked\nlet i := i + 10\nendif\nSetFunctionValue i\nEnd";
static const char *kDoorSet = "Begin Function {ref rD, float fOpen, float fLock}\nif fLock > 0\nrD.Lock\nelse\nrD.Unlock\nendif\nif fOpen > 0\nrD.SetOpenState 1\nelse\nrD.SetOpenState 0\nendif\nEnd";
static Script *s_crossRef, *s_doorGet, *s_doorSet;
// A friend's stand-in body for hits (see proxies): set up (held still, nothing carried, a big pool of health that is
// topped up after every hit), its health read
static const char *kProxySet = "Begin Function {ref rA}\nrA.SetRestrained 1\nrA.RemoveAllItems\nrA.SetAV Health 5000\nrA.SetIgnoreFriendlyHits 1\nEnd";
static const char *kProxyHp = "Begin Function {ref rA}\nSetFunctionValue (rA.GetAV Health)\nEnd";
static const char *kProxyTop = "Begin Function {ref rA}\nrA.RestoreAV Health 9999\nrA.StopCombat\nEnd";
static Script *s_proxySet, *s_proxyHp, *s_proxyTop;
// A quest step from a friend's game is set here with SetStage, and the people round you look at what they should be
// doing again (EvaluatePackage) - the stage's own script doesn't always have them do it
static const char *kEvp = "Begin Function {ref rA}\nrA.EvaluatePackage\nEnd";
static Script *s_evp;
// Shared XP and each player's own loot: your XP now; XP given (the game's own RewardXP: its HUD note, level-ups);
// whether an actor wears a thing
static const char *kXpNow = "Begin Function {}\nSetFunctionValue (player.GetAV XP)\nEnd";
static const char *kGiveXp = "Begin Function {float fN}\nRewardXP fN\nEnd";
static const char *kWornBy = "Begin Function {ref rA, ref rI}\nSetFunctionValue (rA.GetEquipped rI)\nEnd";
static const char *kTeammate = "Begin Function {ref rA}\nSetFunctionValue (rA.GetPlayerTeammate)\nEnd";
static Script *s_xpNow, *s_giveXp, *s_wornBy, *s_teammate;
// Trades between friends (a box of ours opened, its things given back or gone) and the vendor you barter with
static const char *kBoxOpen = "Begin Function {ref rB}\nrB.Activate player\nEnd";
static const char *kBoxBack = "Begin Function {ref rB}\nrB.RemoveAllItems player\nEnd";
static const char *kBoxEmpty = "Begin Function {ref rB}\nrB.RemoveAllItems\nEnd";
static const char *kItemName = "Begin Function {ref rI}\nSetFunctionValue $rI\nEnd";
static const char *kBarterRef = "Begin Function {}\nSetFunctionValue (GetMenuTargetRef 1053)\nEnd";
// The trade window is the game's own message box: its text changed while it's open (message_menu.xml MM_MessageText)
static const char *kBoxText = "string_var s\nBegin Function {s}\nSetUIString \"MessageMenu/NOGLOW_BRANCH/MM_MainRect/MM_MessageText/string\" $s\nEnd";
static const char *kBoxTextGet = "Begin Function {}\nstring_var s\nlet s := GetUIString \"MessageMenu/NOGLOW_BRANCH/MM_MainRect/MM_MessageText/string\"\nSetFunctionValue s\nEnd";
static Script *s_boxText, *s_boxTextGet;
static Script *s_boxOpen, *s_boxBack, *s_boxEmpty, *s_itemName, *s_barterRef;
// Things in the world your shots break (Sunny's bottles), and a quest's people the host's world has out
static const char *kObjDamage = "Begin Function {ref rR, float fD}\nrR.DamageObject fD\nEnd";
static const char *kObjOff = "Begin Function {ref rR}\nrR.Disable\nEnd";
static const char *kRefOn = "Begin Function {ref rR}\nrR.Enable\nEnd";
static Script *s_objDamage, *s_objOff, *s_refOn;
// Who you're in a conversation with (JIP LN: 1009, the actor being spoken to)
static const char *kTalkRef = "Begin Function {}\nSetFunctionValue (GetMenuTargetRef 1009)\nEnd";
static Script *s_talkRef;
// Listen-in: what's said in a conversation (the game's own dialogue menu, dialog_menu.xml: DM_SpeakerText and
// DM_SpeakerNameLabel under NOGLOW_BRANCH - read with JIP LN's GetUIString)
static const char *kTalkText = "Begin Function {}\nstring_var s\nlet s := GetUIString \"DialogMenu/NOGLOW_BRANCH/DM_SpeakerText/string\"\nSetFunctionValue s\nEnd";
static const char *kTalkName = "Begin Function {}\nstring_var s\nlet s := GetUIString \"DialogMenu/NOGLOW_BRANCH/DM_SpeakerNameLabel/string\"\nSetFunctionValue s\nEnd";
static Script *s_talkText, *s_talkName;
static const char *kItemCount = "Begin Function {ref rI}\nSetFunctionValue (Player.GetItemCount rI)\nEnd";
static const char *kHealthNow = "Begin Function {}\nSetFunctionValue (Player.GetAV Health)\nEnd";
static Script *s_itemCount, *s_healthNow;
// JIP LN's ToggleImmortalMode: 1 - health never goes below 1; 2 - you can't be hit at all; 0 off.
static const char *kImmortal = "Begin Function {float f}\nint i\nint iWas\nlet i := f\nlet iWas := ToggleImmortalMode i\nSetFunctionValue iWas\nEnd";
static Script *s_immortal;
// The downed system's immortal mode kept the hit off you too: the hit goes on with it off for that moment (the hit is
// never the one that would kill - that one puts you down instead)
static const char *kHurtMortal = "Begin Function {float fDmg}\nint iWas\nlet iWas := ToggleImmortalMode 0\nplayer.DamageAV Health fDmg\nToggleImmortalMode iWas\nEnd";
static Script *s_hurtMortal;
// Downed, you're knocked over (a push of your own, the game's ragdoll), and pushed over again whenever the game starts
// standing you up while you're still down
static const char *kKnockDown = "Begin Function {float f}\nplayer.PushActorAway player f\nEnd";
static const char *kKnocked = "Begin Function {}\nSetFunctionValue (player.GetKnockedState)\nEnd";
static Script *s_knockDown, *s_knocked;
// A gun's damage as yours would be (JIP LN's GetCalculatedWeaponDamage: your skill and perks, at full condition) and
// its own (GetAttackDamage); your level
static const char *kCalcDmg = "Begin Function {ref rW}\nfloat f\nlet f := GetCalculatedWeaponDamage rW\nSetFunctionValue f\nEnd";
static const char *kBaseDmg = "Begin Function {ref rW}\nfloat f\nlet f := GetAttackDamage rW\nSetFunctionValue f\nEnd";
static const char *kMyLevel = "Begin Function {}\nSetFunctionValue (player.GetLevel)\nEnd";
static Script *s_calcDmg, *s_baseDmg, *s_myLevel;
// Which character creation menu is open (the menu codes: JIP LN GameUI.h), and whether Doc Mitchell's quest (VCG01) is
// over
static const char *kCharMenu =
	"Begin Function {}\nint i\n"
	"if MenuMode 1036\nlet i := 1036\nelseif MenuMode 1051\nlet i := 1051\nelseif MenuMode 1074\nlet i := 1074\n"
	"elseif MenuMode 1060\nlet i := 1060\nelseif MenuMode 1076\nlet i := 1076\nelseif MenuMode 1027\nlet i := 1027\nendif\n"
	"SetFunctionValue i\nEnd";
static const char *kCharDone = "Begin Function {}\nSetFunctionValue (GetQuestCompleted VCG01)\nEnd";
static Script *s_charMenu, *s_charDone;
static Script *s_disable, *s_enable;
static u32 g_handPlug[8] = {255, 255, 255, 255, 255, 255, 255, 255};
static const char *kWeaponOut = "Begin Function {}\nSetFunctionValue (Player.IsWeaponOut)\nEnd";
// Your companion gets in (fOn 0: gone from the world while riding) or out (1: back beside the car)
static const char *kRide =
	"Begin Function {ref rActor, float fOn, float fx, float fy, float fz}\n"
	"if fOn == 0\nrActor.Disable\nelse\nrActor.Enable\nrActor.MoveTo Player\nrActor.SetPos X fx\nrActor.SetPos Y fy\nrActor.SetPos Z fz\nendif\nEnd";
// Camera settings we change while driving, read first so we can put them back
static const char *kCamNames[4] = {"fVanityModeMinDist", "fVanityModeMaxDist", "fOverShoulderPosX:Camera", "fOverShoulderPosZ:Camera"};
static const bool kCamIsIni[4] = {false, false, true, true};

static inline void *F(float f) { union { float f; void *p; } u; u.f = f; return u.p; }

static bool callNum(Script *s, double *out);
static bool compileAll()
{
	s_place = g_script->CompileScript(kPlace);
	s_placeN = g_script->CompileScript(kPlaceN);
	s_shot = g_script->CompileScript(kShot);
	s_contAdj = g_script->CompileScript(kContAdj);
	s_kill = g_script->CompileScript(kKill);
	s_restrain = g_script->CompileScript(kRestrain);
	s_healthPct = g_script->CompileScript(kHealthPct);
	s_dmgPct = g_script->CompileScript(kDmgPct);
	s_keyOff = g_script->CompileScript(kKeyOff); s_keyOn = g_script->CompileScript(kKeyOn);
	LOG("compile (playing together): container %x kill %x restrain %x health %x damage %x chat keys %x/%x", (u32)s_contAdj, (u32)s_kill, (u32)s_restrain, (u32)s_healthPct, (u32)s_dmgPct, (u32)s_keyOff, (u32)s_keyOn);
	s_move = g_script->CompileScript(kMove);
	s_remove = g_script->CompileScript(kRemove);
	s_setXY = g_script->CompileScript(kSetXY);
	s_sound = g_script->CompileScript(kSound);
	s_hurt = g_script->CompileScript(kHurt);
	s_evp = g_script->CompileScript(kEvp);
	s_xpNow = g_script->CompileScript(kXpNow); s_giveXp = g_script->CompileScript(kGiveXp); s_wornBy = g_script->CompileScript(kWornBy); s_teammate = g_script->CompileScript(kTeammate);
	LOG("compile (shared XP, own loot): %x %x %x %x", (u32)s_xpNow, (u32)s_giveXp, (u32)s_wornBy, (u32)s_teammate);
	s_boxOpen = g_script->CompileScript(kBoxOpen); s_boxBack = g_script->CompileScript(kBoxBack); s_boxEmpty = g_script->CompileScript(kBoxEmpty);
	s_itemName = g_script->CompileScript(kItemName); s_barterRef = g_script->CompileScript(kBarterRef);
	s_boxText = g_script->CompileScript(kBoxText); s_boxTextGet = g_script->CompileScript(kBoxTextGet);
	LOG("compile (the trade's box): text %x / %x", (u32)s_boxText, (u32)s_boxTextGet);
	s_talkRef = g_script->CompileScript(kTalkRef);
	s_talkText = g_script->CompileScript(kTalkText); s_talkName = g_script->CompileScript(kTalkName);
	LOG("compile (listen-in): text %x, name %x", (u32)s_talkText, (u32)s_talkName);
	s_objDamage = g_script->CompileScript(kObjDamage); s_objOff = g_script->CompileScript(kObjOff); s_refOn = g_script->CompileScript(kRefOn);
	LOG("compile (shared world things): damage %x, off %x, on %x", (u32)s_objDamage, (u32)s_objOff, (u32)s_refOn);
	LOG("compile (trades, vendors): box %x %x %x, names %x, barter %x", (u32)s_boxOpen, (u32)s_boxBack, (u32)s_boxEmpty, (u32)s_itemName, (u32)s_barterRef);
	s_proxySet = g_script->CompileScript(kProxySet); s_proxyHp = g_script->CompileScript(kProxyHp); s_proxyTop = g_script->CompileScript(kProxyTop);
	LOG("compile (hits on friends): %x %x %x", (u32)s_proxySet, (u32)s_proxyHp, (u32)s_proxyTop);
	s_immortal = g_script->CompileScript(kImmortal);
	s_hurtMortal = g_script->CompileScript(kHurtMortal);
	s_knockDown = g_script->CompileScript(kKnockDown); s_knocked = g_script->CompileScript(kKnocked);
	s_charMenu = g_script->CompileScript(kCharMenu); s_charDone = g_script->CompileScript(kCharDone);
	LOG("compile (character creation together): %x %x", (u32)s_charMenu, (u32)s_charDone);
	s_calcDmg = g_script->CompileScript(kCalcDmg); s_baseDmg = g_script->CompileScript(kBaseDmg); s_myLevel = g_script->CompileScript(kMyLevel);
	LOG("compile (scaling): damage %x/%x level %x", (u32)s_calcDmg, (u32)s_baseDmg, (u32)s_myLevel);
	LOG("compile (downed): JIP's immortal mode %x%s", (u32)s_immortal, s_immortal ? "" : " - not there (no JIP LN?): downed only at 2% health");
	s_itemCount = g_script->CompileScript(kItemCount); s_healthNow = g_script->CompileScript(kHealthNow);
	s_crossRef = g_script->CompileScript(kCrossRef); s_doorGet = g_script->CompileScript(kDoorGet); s_doorSet = g_script->CompileScript(kDoorSet);
	LOG("compile (shared world): crosshair %x door %x/%x", (u32)s_crossRef, (u32)s_doorGet, (u32)s_doorSet);
	s_hit = g_script->CompileScript(kHit);
	s_modIndex = g_script->CompileScript(kModIndex);
	s_mate = g_script->CompileScript(kMate);
	s_weaponOut = g_script->CompileScript(kWeaponOut);
	s_ride = g_script->CompileScript(kRide);
	s_ammo = g_script->CompileScript(kAmmo);
	s_strip = g_script->CompileScript(kStrip);
	s_dressItem = g_script->CompileScript(kDressItem);
	s_dropHit = g_script->CompileScript(kDropHit);
	s_disable = g_script->CompileScript(kDisable);
	s_enable = g_script->CompileScript(kEnable);
	LOG("compile (the dropkick): hit %x", (u32)s_dropHit);
	LOG("compile (friends as themselves): strip %x dress %x", (u32)s_strip, (u32)s_dressItem);
	for (int i = 0; i < kNumHandPlugins && i < 8; i++)
	{
		char src[200];
		rt::fmt(src, sizeof src, "Begin Function {}\nSetFunctionValue (GetModIndex \"%s\")\nEnd", kHandPlugins[i]);
		Script *sc = g_script->CompileScript(src);
		double v = 255;
		if (sc) callNum(sc, &v);
		g_handPlug[i] = v >= 0 && v < 255 ? (u32)v : 255;
		LOG("handling: %s is load-order index %u%s", kHandPlugins[i], g_handPlug[i], g_handPlug[i] == 255 ? " (not loaded)" : "");
	}
	for (int i = 0; i < 4; i++)
	{
		char src[200];
		rt::fmt(src, sizeof src, "Begin Function {}\nSetFunctionValue (%s \"%s\")\nEnd",
			kCamIsIni[i] ? "GetNumericIniSetting" : "GetNumericGameSetting", kCamNames[i]);
		s_getCam[i] = g_script->CompileScript(src);
	}
	static const char *kHud[3] = {"iSystemColorHUDMainRed:Interface", "iSystemColorHUDMainGreen:Interface", "iSystemColorHUDMainBlue:Interface"};
	for (int i = 0; i < 3; i++)
	{
		char src[200];
		rt::fmt(src, sizeof src, "Begin Function {}\nSetFunctionValue (GetNumericIniSetting \"%s\")\nEnd", kHud[i]);
		s_hudRGB[i] = g_script->CompileScript(src);
	}
	LOG("compile: place %x move %x remove %x sound %x hurt %x hit %x modIndex %x cam %x", (u32)s_place, (u32)s_move, (u32)s_remove,
		(u32)s_sound, (u32)s_hurt, (u32)s_hit, (u32)s_modIndex, (u32)s_getCam[0]);
	return s_place && s_move && s_remove && s_hurt && s_modIndex;
}

static bool callNum(Script *s, double *out)
{
	if (!s) return false;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s, player(), 0, &res, 0);
	if (res.type != 1) return false;
	*out = res.num;
	return true;
}

static TESObjectREFR *placeRef(u32 low, float x, float y, float z, float headingDeg)
{
	TESForm *base = lookup((g_mod << 24) | low);
	if (!base || !s_place) { LOG("place: form %x missing", (g_mod << 24) | low); return 0; }
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_place, player(), 0, &res, 5, base, F(x), F(y), F(z), F(headingDeg));
	return res.type == 2 ? (TESObjectREFR *)res.form : 0;
}
// Any base form (placeRef takes ours), and an item stack of n
static TESObjectREFR *placeForm(TESForm *base, float x, float y, float z, float headingDeg)
{
	if (!base || !s_place) return 0;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_place, player(), 0, &res, 5, base, F(x), F(y), F(z), F(headingDeg));
	return res.type == 2 ? (TESObjectREFR *)res.form : 0;
}
static TESObjectREFR *placeCount(TESForm *base, u32 n, float x, float y, float z)
{
	if (!base || !s_placeN) return 0;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_placeN, player(), 0, &res, 5, base, F((float)(n ? n : 1)), F(x), F(y), F(z));
	return res.type == 2 ? (TESObjectREFR *)res.form : 0;
}
static void moveRef(TESObjectREFR *r, float x, float y, float z, float ax, float ay, float az)
{
	if (r && r == player()) { static int n; if (n++ < 20) LOG("(ours moved you to %.0f %.0f %.0f - %s)", x, y, z, g_crumb); }
	if (r && (rd<u32>((u32)r + 8) & 0x20)) { static int d; if (d++ < 20) LOG("moveRef: refused a deleted reference %08x (%s)", refid(r), g_crumb); return; }
	if (!(x > -4e6f && x < 4e6f && y > -4e6f && y < 4e6f && z > -1e6f && z < 1e6f)) { static int m; if (m++ < 20) LOG("moveRef: refused %.0f %.0f %.0f (%s)", x, y, z, g_crumb); return; }
	if (r && s_move) g_script->CallFunctionAlt(s_move, player(), 7, r, F(x), F(y), F(z), F(ax), F(ay), F(az));
}
static void removeRef(TESObjectREFR *r) { if (r && r != player() && !(rd<u32>((u32)r + 8) & 0x20) && s_remove) g_script->CallFunctionAlt(s_remove, player(), 1, r); }
// An actor's life state
static bool deadNow(const void *a) { u32 ls = *(const u32 *)((u32)a + OFF_ACTOR_LIFESTATE); return ls == 1 || ls == 2; }
static void hurt(TESObjectREFR *a, float amt) { if (a && s_hurt) g_script->CallFunctionAlt(s_hurt, player(), 2, a, F(amt)); }
// Returns 1 if they're your companion, 0 if not, -1 if the script is missing
static int hitActor(TESObjectREFR *src, TESObjectREFR *a, float force, float dmg)
{
	if (!a || !src || !s_hit) return -1;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_hit, player(), 0, &res, 4, src, a, F(force), F(dmg));
	return res.type == 1 && res.num != 0 ? 1 : 0;
}
static void console(const char *line) { if (g_console) g_console->RunScriptLine2(line, 0, true); }

// ---- orientation
static void carAxes(float headingDeg, float pitchUpDeg, float rollRightUpDeg, float *rr, float *f, float *u)
{
	const float d = rt::PI / 180.0f;
	float h = headingDeg * d, p = pitchUpDeg * d, r = rollRightUpDeg * d;
	float sh = rt::sin(h), ch = rt::cos(h), sp = rt::sin(p), cp = rt::cos(p), sr = rt::sin(r), cr = rt::cos(r);
	f[0] = sh * cp; f[1] = ch * cp; f[2] = sp;
	float r0[3] = {ch, -sh, 0};
	float u0[3] = {r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0]};
	for (int i = 0; i < 3; i++) { rr[i] = r0[i] * cr + u0[i] * sr; u[i] = u0[i] * cr - r0[i] * sr; }
}
static void carAngles(float headingDeg, float pitchUpDeg, float rollRightUpDeg, float *ax, float *ay, float *az)
{
	const float d = rt::PI / 180.0f;
	float f[3], rr[3], u[3];
	carAxes(headingDeg, pitchUpDeg, rollRightUpDeg, rr, f, u);
	float b = rt::atan2(-u[0], rt::sqrt(1 - u[0] * u[0]));
	float a = rt::atan2(u[1], u[2]);
	float g = rt::atan2(f[0], rr[0]);
	*ax = a / d; *ay = b / d; *az = g / d;
	if (*az < 0) *az += 360;
}

// ---- vehicles
struct Vehicle
{
	u32 refID;
	float x, y, z, vz;
	float heading;
	float pitch, roll;
	float speed;
	float halfTrack, halfBase, halfLength;
	float front, rear, clearance;
	float spawnX, spawnY;
	u32 low;
	u32 oldRef;
	float wheelRadius;
	float steerAxis[3];
	float spin, steer;
	float spinRear, slipMs;
	float course; bool useCourse;
	float wheelie, wheelieVel;
	float shakePitch, shakeRoll, shakeZ;
	float shakeZVel, shakePitchVel, shakeAvg, shakeJump, shakeSide;
	float pitchVel, rollVel;
	float lastSpeed, lastHeading, accSm, latSm;
	float wheelOff[4];
	float wheelRest[4];
	float wheelRestXY[4][2];
	bool wheelRestOK;
	float dbgRide, dbgFloor;
	float tPrev, tVel;
	bool tOK;
	float gw[4];
	bool gwOK;
	int oldWait;
};
static TESObjectREFR *vref(Vehicle &v) { TESForm *f = lookup(v.refID); return f ? (TESObjectREFR *)f : 0; }
static void removeRef(TESObjectREFR *r);

static float g_pitchSign = 1, g_rollSign = 1, g_rideLift = 2;

// Ground height under a point: landscape, or a surface (road, bridge, floor) hit by a downward ray
static float groundAt(float x, float y, float zref)
{
	float tz = -1e9f;
	bool haveT = terrainHeight(x, y, &tz);
	float from[3] = {x, y, zref + 200.0f}, to[3] = {x, y, zref - 700.0f}, fr;
	if (rayHit(from, to, &fr))
	{
		float hz = from[2] + (to[2] - from[2]) * fr;
		if (!haveT || hz > tz) return hz;
	}
	return haveT ? tz : zref;
}

static bool looseObject(u32 node, u32 *body);

// Ground under one of the car's own points (settle): zExp is the car's plane there.
static float groundUnder(float x, float y, float zExp, float zref)
{
	float tz = -1e9f;
	bool haveT = terrainHeight(x, y, &tz);
	float top = rt::min(zref + 200.0f, zExp + TUNE(groundStepMaxUnits)), bottom = zref - 700.0f;
	float best = haveT ? tz : -1e9f;
	if (top > bottom)
	{
		float from[3] = {x, y, top}, to[3] = {x, y, bottom}, fr;
		u32 obj, body;
		bool hit = rayHitObject(from, to, &fr, &obj);
		for (int k = 0; k < 2 && hit && obj && looseObject(obj, &body); k++)
		{
			float hz = top + (bottom - top) * fr, r = boundRadius(obj);
			top = hz - 2.0f - 2.0f * rt::clamp(r, 0, TUNE(looseMaxUnits));
			from[2] = top;
			hit = top > bottom && rayHitObject(from, to, &fr, &obj);
		}
		if (hit)
		{
			if (fr * (top - bottom) < 2.0f) return haveT && tz > zExp ? tz : zExp;
			float hz = top + (bottom - top) * fr;
			if (hz > best) best = hz;
		}
	}
	return best > -1e8f ? best : zExp;
}

// E^-y for y >= 0 (no CRT): halved until small, a few series terms, squared back
static float expNeg(float y) { if (y > 30) return 0; int n = 0; while (y > 0.25f) { y *= 0.5f; n++; } float e = 1 - y + y * y * 0.5f - y * y * y / 6 + y * y * y * y / 24; while (n--) e *= e; return e; }
// A critically damped decay to 0, stepped exactly - stable however short the blend
static void decay(float &x, float &v, float w, float dt) { float e = expNeg(w * dt), k = v + w * x; x = (x + k * dt) * e; v = (v - w * k * dt) * e; }

static void spring(float &x, float &vel, float target, float hz, float damping, float dt, float targetVel = 0)
{
	float w = 2 * rt::PI * hz, k = w * w, c = 2 * damping * w;
	int n = (int)(dt / 0.008f) + 1;
	float h = dt / n;
	for (int i = 0; i < n; i++)
	{
		vel += (k * (target - x) + c * (targetVel - vel)) * h;
		x += vel * h;
	}
}
static void settle(Vehicle &v, float dt, bool snap, float maxTilt = 40)
{
	float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
	float ht = v.halfTrack, hb = v.halfBase;
	float fr = (v.front > 0 ? v.front : v.halfLength) * 0.95f, rr = (v.rear > 0 ? v.rear : v.halfLength) * 0.95f;
	float cl = rt::clamp((v.clearance > 0 ? v.clearance : 10) - 2, 3, 30);
	const float pts[15][3] = {{-ht, hb, 0}, {ht, hb, 0}, {-ht, -hb, 0}, {ht, -hb, 0},
		{0, fr, cl}, {0, -rr, cl}, {0, 0, cl}, {-ht, 0, cl}, {ht, 0, cl},
		{0, fr * 0.6f, cl}, {0, -rr * 0.6f, cl}, {-ht * 0.8f, fr * 0.85f, cl}, {ht * 0.8f, fr * 0.85f, cl},
		{-ht * 0.8f, -rr * 0.85f, cl}, {ht * 0.8f, -rr * 0.85f, cl}};
	float g[15];
	float blur = TUNE(groundBlurUnits);
	float base = v.z - g_rideLift, tp0 = rt::sin(v.pitch * rt::PI / 180.0f), tr0 = rt::sin(v.roll * rt::PI / 180.0f);
	auto gnd = [&](float wx, float wy, float lx, float ly) {
		return snap ? groundAt(wx, wy, v.z) : groundUnder(wx, wy, base + ly * tp0 + lx * tr0, v.z);
	};
	float lead = v.speed * kUnitsPerMetre / (2 * rt::PI * TUNE(groundSmoothHz));
	for (int i = 0; i < 15; i++)
	{
		float wx = v.x + pts[i][0] * c + pts[i][1] * s, wy = v.y - pts[i][0] * s + pts[i][1] * c;
		if (i < 4 && blur > 0)
		{
			wx += s * lead; wy += c * lead;
			float sx = c * blur * 0.5f, sy = -s * blur * 0.5f;
			float lx = pts[i][0], ly = pts[i][1] + lead;
			float p0 = gnd(wx, wy, lx, ly), p1 = gnd(wx + s * blur, wy + c * blur, lx, ly + blur), p2 = gnd(wx - s * blur, wy - c * blur, lx, ly - blur);
			float p3 = gnd(wx + sx, wy + sy, lx + blur * 0.5f, ly), p4 = gnd(wx - sx, wy - sy, lx - blur * 0.5f, ly);
			if (snap)
				g[i] = rt::max(rt::max(p0, p1), rt::max(rt::max(p2, p3), p4));
			else
				g[i] = (p0 * 2 + p1 + p2 + p3 + p4) / 6.0f;
		}
		else
			g[i] = gnd(wx, wy, pts[i][0], pts[i][1]);
	}
	{
		float k = snap || !v.gwOK ? 1.0f : rt::clamp(dt * TUNE(groundSmoothHz) * 2 * rt::PI, 0, 1);
		for (int i = 0; i < 4; i++)
		{
			if (snap || !v.gwOK || rt::absf(g[i] - v.gw[i]) > 120.0f) v.gw[i] = g[i];
			else v.gw[i] += (g[i] - v.gw[i]) * k;
		}
		v.gwOK = true;
	}
	float pitchT = rt::atan2((v.gw[0] + v.gw[1]) * 0.5f - (v.gw[2] + v.gw[3]) * 0.5f, 2 * hb) * 180.0f / rt::PI;
	float rollT = rt::atan2((v.gw[1] + v.gw[3]) * 0.5f - (v.gw[0] + v.gw[2]) * 0.5f, 2 * ht) * 180.0f / rt::PI;
	float pitchG = pitchT, rollG = rollT;
	if (!snap && dt > 0)
	{
		float acc = (v.speed - v.lastSpeed) / dt;
		float dh = v.heading - v.lastHeading;
		while (dh > 180) dh -= 360;
		while (dh < -180) dh += 360;
		float lat = v.speed * (dh * rt::PI / 180.0f) / dt;
		float k = rt::clamp(dt * 6, 0, 1);
		v.accSm += (rt::clamp(acc, -30, 30) - v.accSm) * k;
		v.latSm += (rt::clamp(lat, -30, 30) - v.latSm) * k;
		float mx = TUNE(maxBodyTilt);
		pitchT += rt::clamp(v.accSm * TUNE(pitchPerAccel), -mx, mx);
		rollT += rt::clamp(v.latSm * TUNE(rollPerAccel), -mx, mx);
	}
	v.lastSpeed = v.speed; v.lastHeading = v.heading;
	if (snap)
	{
		v.pitch = rt::clamp(pitchT, -maxTilt, maxTilt); v.roll = rt::clamp(rollT, -maxTilt, maxTilt); v.pitchVel = v.rollVel = 0;
	}
	else
	{
		spring(v.pitch, v.pitchVel, pitchT, TUNE(tiltHz), TUNE(tiltDamping), dt);
		spring(v.roll, v.rollVel, rollT, TUNE(tiltHz), TUNE(tiltDamping), dt);
		v.pitch = rt::clamp(v.pitch, -40, 40);
		v.roll = rt::clamp(v.roll, -40, 40);
	}
	float tp = rt::sin(v.pitch * rt::PI / 180.0f), tr = rt::sin(v.roll * rt::PI / 180.0f);
	float ride = 0;
	for (int i = 0; i < 4; i++) ride += (v.gw[i] - pts[i][1] * tp - pts[i][0] * tr) * 0.25f;
	ride += g_rideLift;
	float floorZ = -1e9f;
	{
		float tpg = rt::sin(pitchG * rt::PI / 180.0f), trg = rt::sin(rollG * rt::PI / 180.0f);
		for (int i = 4; i < 15; i++)
		{
			float need = g[i] - pts[i][1] * tpg - pts[i][0] * trg - pts[i][2];
			if (need > floorZ) floorZ = need;
		}
	}
	floorZ += g_rideLift;
	float sagT = TUNE(suspensionSag);
	float target = floorZ - sagT > ride ? floorZ - sagT : ride;
	v.dbgRide = ride; v.dbgFloor = floorZ;
	if (snap)
	{
		v.z = target;
		v.vz = 0;
	}
	else
	{
		float sag = TUNE(suspensionSag), droop = TUNE(wheelDroop);
		if (v.z > target + droop)
		{
			float gr = 9.81f * kUnitsPerMetre * TUNE(airGravityScale);
			int n = (int)(dt / 0.008f) + 1;
			float st = dt / n;
			for (int i = 0; i < n; i++) { v.vz -= gr * st; v.z += v.vz * st; }
		}
		else
		{
			float tv = v.tOK && dt > 0 ? (target - v.tPrev) / dt : 0;
			tv = rt::clamp(tv, -900, 900);
			v.tVel += (tv - v.tVel) * rt::clamp(dt * 10, 0, 1);
			spring(v.z, v.vz, target, TUNE(suspensionHz), TUNE(suspensionDamping), dt, v.tVel);
		}
		if (v.z < floorZ - 1.5f * sag) { v.z = floorZ - 1.5f * sag; if (v.vz < 0) v.vz = 0; }
		if (v.z > target + 2000.0f) { v.z = target; v.vz = 0; }
		v.tPrev = target; v.tOK = true;
	}
	float up = TUNE(wheelBump), down = TUNE(wheelDroop);
	for (int i = 0; i < 4; i++)
	{
		float atRest = v.z - g_rideLift + pts[i][1] * tp + pts[i][0] * tr;
		v.wheelOff[i] = snap ? 0 : rt::clamp(v.gw[i] - atRest, -down, up);
		if (i < 2 && v.wheelie > 1.5f) v.wheelOff[i] = -down;
	}
}

// Moves the vehicle by its speed; returns the impact speed (m/s) if it hit something.

static Vehicle g_car;
static void ridersMoveTo(u32 oldRef, TESObjectREFR *n);
static void respawnIfFar(Vehicle &v)
{
	if (v.oldRef)
	{
		TESObjectREFR *cur = vref(v);
		u32 nr = cur ? thirdPersonNode(cur) : 0;
		if (cur && (!nr || !ptr(nr + 0x18)) && ++v.oldWait < 30) return;
		if (&v == &g_car) ridersMoveTo(v.oldRef, cur);
		removeRef((TESObjectREFR *)lookup(v.oldRef));
		v.oldRef = 0;
		v.oldWait = 0;
	}
	float dx = v.x - v.spawnX, dy = v.y - v.spawnY;
	if (dx * dx + dy * dy < 4000.0f * 4000.0f) return;
	u32 old = v.refID;
	TESObjectREFR *n = placeRef(v.low, v.x, v.y, v.z, v.heading);
	if (!n) return;
	v.oldRef = old;
	v.oldWait = 0;
	v.refID = refid(n);
	v.spawnX = v.x; v.spawnY = v.y;
	u32 nr = thirdPersonNode(n);
	if (nr && ptr(nr + 0x18))
	{
		if (&v == &g_car) ridersMoveTo(old, n);
		setHidden(lookup(old), true);
	}
}

// Where a point of the car (x right, y forward, z up from its origin) is in the world as the car is drawn this frame:
// with its pitch, roll, wheelie and bass shake (the seat and hood cameras ride exactly with the body)
static void carPoint(const Vehicle &v, float lx, float ly, float lz, float *out)
{
	float rr[3], f[3], u[3];
	carAxes(v.heading, (v.pitch + v.wheelie + v.shakePitch) * g_pitchSign, (v.roll + v.shakeRoll) * g_rollSign, rr, f, u);
	float lift = v.wheelie > 0 ? v.halfBase * rt::sin(v.wheelie * rt::PI / 180.0f) : 0;
	float h = v.heading * rt::PI / 180.0f;
	float back = v.wheelie > 0 ? v.halfBase * (1 - rt::cos(v.wheelie * rt::PI / 180.0f)) : 0;
	float o[3] = {v.x - rt::sin(h) * back, v.y - rt::cos(h) * back, v.z + lift + v.shakeZ};
	for (int i = 0; i < 3; i++) out[i] = o[i] + rr[i] * lx + f[i] * ly + u[i] * lz;
}
static void pushRef(TESObjectREFR *r, Vehicle &v)
{
	float ax, ay, az;
	carAngles(v.heading, (v.pitch + v.wheelie + v.shakePitch) * g_pitchSign, (v.roll + v.shakeRoll) * g_rollSign, &ax, &ay, &az);
	float lift = v.wheelie > 0 ? v.halfBase * rt::sin(v.wheelie * rt::PI / 180.0f) : 0;
	float h = v.heading * rt::PI / 180.0f;
	float back = v.wheelie > 0 ? v.halfBase * (1 - rt::cos(v.wheelie * rt::PI / 180.0f)) : 0;
	moveRef(r, v.x - rt::sin(h) * back, v.y - rt::cos(h) * back, v.z + lift + v.shakeZ, ax, ay, az);
}
static void pushVehicle(Vehicle &v)
{
	pushRef(vref(v), v);
	if (v.oldRef) pushRef((TESObjectREFR *)lookup(v.oldRef), v);
}

static float g_wheelSpinSign = 1, g_wheelSteerSign = 1;
static void setRotX(u32 node, float a)
{
	float c = rt::cos(a), sn = rt::sin(a);
	float *m = (float *)(node + 0x34);
	m[0] = 1; m[1] = 0; m[2] = 0;
	m[3] = 0; m[4] = c; m[5] = -sn;
	m[6] = 0; m[7] = sn; m[8] = c;
}
static void setRotZ(u32 node, float a)
{
	float c = rt::cos(a), sn = rt::sin(a);
	float *m = (float *)(node + 0x34);
	m[0] = c; m[1] = -sn; m[2] = 0;
	m[3] = sn; m[4] = c; m[5] = 0;
	m[6] = 0; m[7] = 0; m[8] = 1;
}
// A turn of a about the unit axis n (Rodrigues), written like setRotZ (which is this with n = Z)
static void setRotAxis(u32 node, const float *n, float a)
{
	float c = rt::cos(a), sn = rt::sin(a), t = 1 - c, x = n[0], y = n[1], z = n[2];
	float *m = (float *)(node + 0x34);
	m[0] = c + x * x * t;     m[1] = x * y * t - z * sn; m[2] = x * z * t + y * sn;
	m[3] = y * x * t + z * sn; m[4] = c + y * y * t;     m[5] = y * z * t - x * sn;
	m[6] = z * x * t - y * sn; m[7] = z * y * t + x * sn; m[8] = c + z * z * t;
}
static void updateNode(u32 node)
{
	static u32 upd[3];
	void **vt = *(void ***)node;
	if (vt) ((void(__thiscall *)(void *, const void *, u32))vt[0xA4 / 4])((void *)node, upd, 0);
}
// Lights. drive.nif has MMWLightOff<Role> (shown) and MMWLightOn<Role> (hidden) nodes per light role; the plugin swaps
// them with the NiAVObject hidden flag. mask bits: 1 head, 2 tail, 4 brake, 8 reverse.
static void setLights(Vehicle &v, int mask)
{
	TESObjectREFR *r = vref(v);
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) return;
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 256) return;
	static const char *roles[4] = {"Head", "Tail", "Brake", "Reverse"};
	{
		bool hasTail = false, hasBrake = false;
		for (u16 i = 0; i < n; i++)
		{
			u32 c = ptr(arr + 4 * i);
			const char *nm = c ? (const char *)ptr(c + 8) : 0;
			if (!nm || memcmp(nm, "MMWLightOn", 10)) continue;
			if (!rt::strcmp(nm + 10, "Tail")) hasTail = true;
			if (!rt::strcmp(nm + 10, "Brake")) hasBrake = true;
		}
		if (!hasTail && hasBrake && (mask & 2)) mask |= 4;
		if (hasTail && !hasBrake && (mask & 4)) mask |= 2;
	}
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || memcmp(nm, "MMWLight", 8)) continue;
		bool on = nm[8] == 'O' && nm[9] == 'n';
		const char *role = nm + (on ? 10 : 11);
		for (int k = 0; k < 4; k++)
			if (!rt::strcmp(role, roles[k]))
			{
				bool lit = (mask >> k) & 1;
				u32 &f = *(u32 *)(c + OFF_NIAV_FLAGS);
				bool hide = on ? !lit : lit;
				f = hide ? (f | 1u) : (f & ~1u);
			}
	}
}
// Suspension links (open-wheel cars).
static const char *suspF(const char *s, float *out, int n)
{
	for (int i = 0; i < n; i++)
	{
		while (*s == ' ') s++;
		if (!*s) return 0;
		out[i] = rt::parsef(s);
		while (*s && *s != ' ') s++;
	}
	return s;
}
static void setRotBetween(u32 node, const float *a, const float *b)
{
	float c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	float sn = rt::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), cs = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	if (sn < 1e-6f) { float z[3] = {0, 0, 1}; setRotAxis(node, z, 0); return; }
	c[0] /= sn; c[1] /= sn; c[2] /= sn;
	setRotAxis(node, c, rt::atan2(sn, cs));
}
static void suspensionLinks(u32 arr, u16 n, const float *dzw, float steerA, float pin[2][2], const bool *hasPin)
{
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || memcmp(nm, "MMWSusp ", 8)) continue;
		const char *q = nm + 8;
		int wi = (q[0] == 'F' ? 0 : 2) + (q[1] == 'R' ? 1 : 0);
		char kind = q[3];
		float f[10];
		if (!suspF(q + 4, f, kind == 'H' ? 10 : 7)) continue;
		float k = f[0], dz = dzw[wi] * k;
		float *t = (float *)(c + 0x58);
		if (kind == 'H')
		{
			float *A = f + 1, *h = f + 4, *o = f + 7;
			float hl = rt::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
			if (hl < 1e-4f) continue;
			float hn[3] = {h[0] / hl, h[1] / hl, h[2] / hl};
			float oh = o[0] * hn[0] + o[1] * hn[1] + o[2] * hn[2];
			float op[3] = {o[0] - oh * hn[0], o[1] - oh * hn[1], o[2] - oh * hn[2]};
			float a = op[2], b = hn[0] * op[1] - hn[1] * op[0];
			float rho = rt::sqrt(a * a + b * b);
			float th = 0;
			if (rho > 1e-3f)
			{
				float cc = rt::clamp((a + dz) / rho, -1, 1);
				float ac = rt::atan2(rt::sqrt(1 - cc * cc), cc), phi = rt::atan2(b, a);
				float t1 = phi - ac, t2 = phi + ac;
				while (t1 > rt::PI) t1 -= 2 * rt::PI; while (t1 < -rt::PI) t1 += 2 * rt::PI;
				while (t2 > rt::PI) t2 -= 2 * rt::PI; while (t2 < -rt::PI) t2 += 2 * rt::PI;
				th = rt::absf(t1) < rt::absf(t2) ? t1 : t2;
			}
			setRotAxis(c, hn, th);
			t[0] = A[0]; t[1] = A[1]; t[2] = A[2];
		}
		else
		{
			float *B = f + 1, *vv = f + 4;
			float nb[3] = {B[0], B[1], B[2] + dz};
			int side = wi & 1;
			if (kind == 'P' && wi < 2 && hasPin[side])
			{
				float cs = rt::cos(steerA), sn = rt::sin(steerA), rx = B[0] - pin[side][0], ry = B[1] - pin[side][1];
				nb[0] = pin[side][0] + cs * rx - sn * ry;
				nb[1] = pin[side][1] + sn * rx + cs * ry;
			}
			float w[3] = {B[0] + vv[0] - nb[0], B[1] + vv[1] - nb[1], B[2] + vv[2] - nb[2]};
			setRotBetween(c, vv, w);
			t[0] = nb[0]; t[1] = nb[1]; t[2] = nb[2];
		}
		updateNode(c);
	}
}
static void turnWheels(Vehicle &v, float dt)
{
	TESObjectREFR *r = vref(v);
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) return;
	float rad = v.wheelRadius > 0.1f ? v.wheelRadius : 0.33f;
	v.spin -= v.speed * dt / rad;
	while (v.spin > rt::PI) v.spin -= 2 * rt::PI;
	while (v.spin < -rt::PI) v.spin += 2 * rt::PI;
	v.spinRear -= (v.speed + v.slipMs) * dt / rad;
	while (v.spinRear > rt::PI) v.spinRear -= 2 * rt::PI;
	while (v.spinRear < -rt::PI) v.spinRear += 2 * rt::PI;
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 256) return;
	float pin[2][2]; bool hasPin[2] = {false, false}, links = false;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || (memcmp(nm, "MMWPin", 6) && memcmp(nm, "MMWSusp", 7))) continue;
		if (nm[3] == 'S') { links = true; continue; }
		int side = nm[7] == 'R' ? 1 : 0;
		pin[side][0] = *(float *)(c + 0x58); pin[side][1] = *(float *)(c + 0x5C);
		hasPin[side] = true;
	}
	float dzw[4] = {0, 0, 0, 0};
	float steerA = -v.steer * g_wheelSteerSign * rt::PI / 180.0f;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (nm && !rt::strcmp(nm, "MMWCockpitWheel"))
		{
			const float *ax = v.steerAxis;
			if (ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2] > 0.5f)
			{
				float ratio = TUNE(steeringWheelDegreesAtLock) / rt::max(TUNE(maxSteerDegrees), 1);
				setRotAxis(c, ax, -v.steer * ratio * g_wheelSteerSign * TUNE(steeringWheelSign) * rt::PI / 180.0f);
				updateNode(c);
			}
			continue;
		}
		if (!nm || memcmp(nm, "MMWSteer", 8)) continue;
		int wi = (nm[8] == 'F' ? 0 : 2) + (nm[9] == 'R' ? 1 : 0);
		float *tz = (float *)(c + 0x60);
		float *txy = (float *)(c + 0x58);
		if (!v.wheelRestOK) { v.wheelRest[wi] = *tz; v.wheelRestXY[wi][0] = txy[0]; v.wheelRestXY[wi][1] = txy[1]; }
		float wx = (wi & 1) ? v.halfTrack : -v.halfTrack, wy = wi < 2 ? v.halfBase : -v.halfBase;
		float shake = v.shakeZ + wy * rt::sin(v.shakePitch * rt::PI / 180.0f) + wx * rt::sin(v.shakeRoll * rt::PI / 180.0f);
		dzw[wi] = v.wheelOff[wi] - shake;
		*tz = v.wheelRest[wi] + dzw[wi];
		setRotZ(c, nm[8] == 'F' ? steerA : 0);
		if (wi < 2 && hasPin[wi])
		{
			float cs = rt::cos(steerA), sn = rt::sin(steerA);
			float rx = v.wheelRestXY[wi][0] - pin[wi][0], ry = v.wheelRestXY[wi][1] - pin[wi][1];
			txy[0] = pin[wi][0] + cs * rx - sn * ry;
			txy[1] = pin[wi][1] + sn * rx + cs * ry;
		}
		u32 ca = ptr(c + OFF_NODE_CHILDREN);
		u32 w = ca && rd<u16>(c + OFF_NODE_CHILDCOUNT) ? ptr(ca) : 0;
		if (w) setRotX(w, (nm[8] == 'R' ? v.spinRear : v.spin) * g_wheelSpinSign);
		updateNode(c);
	}
	v.wheelRestOK = true;
	if (links) suspensionLinks(arr, n, dzw, steerA, pin, hasPin);
}

// ---- state
struct CarState { bool available; float halfTrack, halfBase, halfLength, halfWidth, topSpeed, front, rear, clearance, wheelRadius, steerAxis[3], wheelRadiusF, wheelRadiusR; float modelYaw, origin[3]; };
static CarState g_cars[kNumCars];
static int g_activeCar = -1;
static u32 g_parkedRef;
static bool g_driving;
static u32 g_display[8];
static int g_slotCar[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static u32 g_slotAge[8], g_slotClock;
static int g_menuClass, g_menuPage;
struct Beam { u32 ref, old; float sx, sy; };
static Beam g_headBeam, g_headNear, g_brakeBeam;
static Beam g_headFar;
static int g_headMode = 1;
static int g_headOnMode = 1;
static int g_menu = 0;
static int g_menuMap[10];
static int g_pendingChoice = -1, g_pendingMenu;
static double g_lastTime;
static bool g_mwFound, g_hpFound;
// The game picked in setup (mmw.ini vanilla=1: "vanilla stable" - the engine fixes and our weapon feel, nothing added)
// and smooth ADS (O, live): the camera easing onto the sights (Stewie's Tweaks) and our gun settling in
static bool g_vanilla, g_smoothAds = true;
static float g_tpCamSide, g_fovKick;
static double g_blkAt = -9;
static u32 g_hudColor = 0xFFFFB642;
static bool callNum(Script *s, double *out);
static void hudColorTick()
{
	static double hudAt = -99;
	if (rt::now() - hudAt <= 5 || !player()) return;
	hudAt = rt::now();
	double c[3] = {255, 182, 66};
	for (int i = 0; i < 3; i++) { double v; if (callNum(s_hudRGB[i], &v) && v >= 0 && v <= 255) c[i] = v; }
	g_hudColor = 0xFF000000 | ((u32)c[0] << 16) | ((u32)c[1] << 8) | (u32)c[2];
}
static double g_loadedAt = -99;
static bool g_hinted;
static bool g_keyPrev[256];
static double g_camOrig[4];
static bool g_camSaved, g_camChanged, g_playerHidden, g_tcl;
static bool g_resume;
static bool g_afterLoadPending;
static bool g_worldUp;
static u32 g_loads;
static bool g_needRelease;
static float g_holdG;
static int g_camMode;
static bool g_lean;
static float g_leanBlend;
static float g_leanYaw, g_leanPitch;
static inline float leanEase() { float p = g_leanBlend; return p * p * p * (p * (p * 6 - 15) + 10); }
static float g_camHeading;
static bool g_wasThirdPerson;
static float g_lookYaw, g_lookPitch, g_lookIdle;
static double g_quietUntil;
static u32 g_loadedCops[6];
static u32 g_loadedBeams[4], g_loadedOldDisplay[8];

struct HitMemo { u32 id; float t; };
static HitMemo g_hits[8];

static int g_tracks, g_track = 0;

static bool g_chatTyping;
static bool pressed(int vk)
{
	bool down = !g_chatTyping && (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool edge = down && !g_keyPrev[vk & 255];
	g_keyPrev[vk & 255] = down;
	return edge;
}
static bool held(int vk) { return !g_chatTyping && (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool gameFocused()
{
	u32 pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

static void loadConfig()
{
	char p[300];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\mmw.ini", g_dir);
	rt::free(g_ini);
	g_ini = rt::readfile(p, 0);
	g_iniGen++; g_tuneGen++;
	g_mwFound = iniI("mw", g_ini ? 0 : 1) != 0;
	g_hpFound = iniI("hp", g_ini ? 0 : 1) != 0;
	g_vanilla = iniI("vanilla", 0) != 0;
	g_smoothAds = iniI("smoothAds", 1) != 0;
	for (int i = 0; i < kNumCars; i++)
	{
		char k[96];
		rt::fmt(k, sizeof k, "car.%s", kCars[i].id); g_cars[i].available = iniI(k, g_ini ? 0 : 1) != 0;
		rt::fmt(k, sizeof k, "car.%s.halfTrack", kCars[i].id); g_cars[i].halfTrack = iniF(k, 54);
		rt::fmt(k, sizeof k, "car.%s.halfBase", kCars[i].id); g_cars[i].halfBase = iniF(k, 87);
		rt::fmt(k, sizeof k, "car.%s.halfLength", kCars[i].id); g_cars[i].halfLength = iniF(k, 157);
		rt::fmt(k, sizeof k, "car.%s.topSpeed", kCars[i].id); g_cars[i].topSpeed = iniF(k, 180);
		rt::fmt(k, sizeof k, "car.%s.halfWidth", kCars[i].id); g_cars[i].halfWidth = iniF(k, g_cars[i].halfTrack + 16);
		rt::fmt(k, sizeof k, "car.%s.wheelRadius", kCars[i].id); g_cars[i].wheelRadius = iniF(k, 0.33f);
		rt::fmt(k, sizeof k, "car.%s.wheelRadiusF", kCars[i].id); g_cars[i].wheelRadiusF = iniF(k, g_cars[i].wheelRadius);
		rt::fmt(k, sizeof k, "car.%s.wheelRadiusR", kCars[i].id); g_cars[i].wheelRadiusR = iniF(k, g_cars[i].wheelRadius);
		rt::fmt(k, sizeof k, "car.%s.steerAxisX", kCars[i].id); g_cars[i].steerAxis[0] = iniF(k, 0);
		rt::fmt(k, sizeof k, "car.%s.steerAxisY", kCars[i].id); g_cars[i].steerAxis[1] = iniF(k, 0);
		rt::fmt(k, sizeof k, "car.%s.steerAxisZ", kCars[i].id); g_cars[i].steerAxis[2] = iniF(k, 0);
		{
			float *a = g_cars[i].steerAxis, l = rt::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
			if (l > 0.5f && l < 2) { a[0] /= l; a[1] /= l; a[2] /= l; }
			else a[0] = a[1] = a[2] = 0;
		}
		rt::fmt(k, sizeof k, "car.%s.front", kCars[i].id); g_cars[i].front = iniF(k, g_cars[i].halfLength);
		rt::fmt(k, sizeof k, "car.%s.rear", kCars[i].id); g_cars[i].rear = iniF(k, g_cars[i].halfLength);
		rt::fmt(k, sizeof k, "car.%s.clearance", kCars[i].id); g_cars[i].clearance = iniF(k, 10);
		rt::fmt(k, sizeof k, "car.%s.modelYaw", kCars[i].id); g_cars[i].modelYaw = iniF(k, 0);
		for (int a = 0; a < 3; a++) { rt::fmt(k, sizeof k, "car.%s.origin%c", kCars[i].id, 'X' + a); g_cars[i].origin[a] = iniF(k, 0); }
	}
	g_pitchSign = iniF("pitchSign", 1);
	g_rollSign = iniF("rollSign", 1);
	g_rideLift = iniF("rideLift", 2);
	g_wheelSpinSign = iniF("wheelSpinSign", 1);
	g_wheelSteerSign = iniF("wheelSteerSign", 1);
	g_tracks = iniI("tracks", 0);
	LOG("config: ini %s mw %d hp %d tracks %d", g_ini ? "yes" : "no", g_mwFound, g_hpFound, g_tracks);
}

// ---- Camera / controls
static void driveMode(bool on)
{
	if (on)
	{
		console("DisablePlayerControls 1 0 1 1 1 0 1");
		if (!g_tcl) { console("tcl"); g_tcl = true; }
		g_wasThirdPerson = thirdPerson(player());
		g_camHeading = g_car.heading;
		g_lookYaw = g_lookPitch = 0; g_lookIdle = 0;
		setFirstPerson(true);
	}
	else
	{
		g_lean = false; g_leanBlend = 0;
		console("EnablePlayerControls");
		if (g_tcl) { console("tcl"); g_tcl = false; }
		if (g_wasThirdPerson) setFirstPerson(false);
	}
}
static void restoreCamera()
{
	if (!g_camChanged || !g_camSaved) return;
	char line[160];
	for (int i = 0; i < 4; i++)
	{
		rt::fmt(line, sizeof line, kCamIsIni[i] ? "SetNumericIniSetting \"%s\" %f" : "SetNumericGameSetting %s %f", kCamNames[i], g_camOrig[i]);
		console(line);
	}
	g_camChanged = false;
}

// Game volume sliders (BSAudioManager volumes: 0 master, 3 effects, 4 music, 5 radio; see JIP LN GameSound.h)
static float *gameVolumes() { return (float *)(ADDR_AUDIO_MANAGER + OFF_AUDIO_VOLUMES); }
static float g_savedMusicVol = -1, g_savedRadioVol = -1;
static bool g_radioOn;
static float g_radioKnob = -1;
static float g_radioFall = 1;
static int g_sub = 0;
static int g_musicMode = 2;
static int g_screenMode = 1;
static int g_borderless = 0;
// Our chat box: what's typed so far (drawn at the frame's Present by feel::chatBox; g_chatTyping above)
static char g_chatBuf[100]; static int g_chatLen;
static int g_friendlyFire = 1;
static char g_playerName[24];
static char g_radioTitle[160];
static void radioKnobLoad()
{
	char p[320];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\settings.ini", g_dir);
	char *b = rt::readfile(p, 0);
	g_radioKnob = TUNE(radioVolumeDefault);
	g_sub = (int)TUNE(subDefaultLevel);
	if (b)
	{
		for (char *q = b; *q; q++)
		{
			if (rt::startswith(q, "radioVolume=")) g_radioKnob = rt::clamp(rt::parsef(q + 12), 0, 1.5f);
			if (rt::startswith(q, "subwoofer=")) g_sub = (int)rt::clamp((float)rt::parsei(q + 10), 0, 10);
			if (rt::startswith(q, "radioSource=")) g_musicMode = (int)rt::clamp((float)rt::parsei(q + 12), 0, 2);
			if (rt::startswith(q, "screen=")) g_screenMode = (int)rt::clamp((float)rt::parsei(q + 7), 0, 2);
			if (rt::startswith(q, "borderless=")) g_borderless = rt::parsei(q + 11) == 1 ? 1 : 0;
			if (rt::startswith(q, "friendlyFire=")) g_friendlyFire = rt::parsei(q + 13) ? 1 : 0;
			if (rt::startswith(q, "playerName="))
			{
				int k = 0; const char *v = q + 11;
				while (v[k] && v[k] != '\r' && v[k] != '\n' && k < 23) { g_playerName[k] = v[k]; k++; }
				g_playerName[k] = 0;
			}
			if (rt::startswith(q, "headlights=")) { g_headMode = (int)rt::clamp((float)rt::parsei(q + 11), 0, 2); if (g_headMode) g_headOnMode = g_headMode; }
		}
		rt::free(b);
	}
}
static void radioKnobSave()
{
	char p[320], line[256];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\settings.ini", g_dir);
	int n = rt::fmt(line, sizeof line, "; Mojave Enhanced settings you change in game\r\nradioVolume=%.2f\r\nsubwoofer=%d\r\nradioSource=%d\r\nscreen=%d\r\nborderless=%d\r\nheadlights=%d\r\nfriendlyFire=%d\r\n%s%s%s",
		g_radioKnob, g_sub, g_musicMode, g_screenMode, g_borderless, g_headMode, g_friendlyFire, g_playerName[0] ? "playerName=" : "", g_playerName, g_playerName[0] ? "\r\n" : "");
	rt::writefile(p, line, (u32)n, false);
}
// The subwoofer's level and tuning to the mixer (it rebuilds its filters only when the level changes)
static void subApply()
{
	float t[radio::kSettings] = {TUNE(subCrossoverHz), TUNE(subBassDbPerLevel), TUNE(subThumpHz), TUNE(subThumpQ), TUNE(subThumpDbPerLevel),
		TUNE(subBassCeiling), TUNE(subThickFromLevel), TUNE(subThickMax), TUNE(radioOutsideLowpassHz),
		TUNE(radioOutsideBleed), TUNE(subRattleFromLevel), TUNE(subRattleGain), TUNE(subHighDbPerLevel),
		TUNE(subHighShelfHz), TUNE(subHighShelfDbPerLevel)};
	audio::setSubwoofer(g_sub, t);
}
// The bass shaking a car.
static void spring(float &x, float &vel, float target, float hz, float damping, float dt, float targetVel);
static void bassShake(Vehicle &v, bool on, float dt)
{
	float k = on ? rt::clamp(((float)g_sub - TUNE(subShakeFromLevel)) / (10 - TUNE(subShakeFromLevel)), 0, 1) : 0;
	float a = rt::clamp(audio::bassEnv(), 0, 1.2f) * k;
	if (!on && rt::absf(v.shakeZ) < 0.05f && rt::absf(v.shakePitch) < 0.01f && rt::absf(v.shakeZVel) < 1 && rt::absf(v.shakePitchVel) < 1)
	{
		v.shakeZ = v.shakePitch = v.shakeRoll = v.shakeZVel = v.shakePitchVel = v.shakeAvg = v.shakeJump = 0;
		return;
	}
	static u32 seed = 0x9E3779B9;
	seed = seed * 1664525u + 1013904223u;
	float rn = (float)((seed >> 9) & 0xFFFF) / 32768.0f - 1;
	float hz = TUNE(subShakeHz), damp = TUNE(subShakeDamping);
	spring(v.shakeZ, v.shakeZVel, -a * TUNE(subShakeUnits), hz, damp, dt, 0);
	v.shakeAvg += (a - v.shakeAvg) * rt::clamp(dt * 3, 0, 1);
	float jump = a - v.shakeAvg;
	if (v.shakeSide == 0) v.shakeSide = 1;
	if (jump > 0.15f && v.shakeJump <= 0.15f) v.shakeSide = -v.shakeSide;
	v.shakeJump = jump;
	spring(v.shakePitch, v.shakePitchVel, jump * TUNE(subShakeDegrees) * v.shakeSide, hz * 0.8f, damp, dt, 0);
	v.shakeRoll = a * TUNE(subShakeRollDegrees) * rn;
}
static Vehicle g_parkedPose;
static u32 g_parkedPoseRef;

// The game's music and its radio quiet while the car plays the Need for Speed stations or your music - the Pip-Boy's
// radio is the car radio's third source, so it never plays over the other two
static void muteGameMusic(bool mute)
{
	float *v = gameVolumes();
	if (mute && g_savedMusicVol < 0) { g_savedMusicVol = v[4]; v[4] = 0; }
	if (!mute && g_savedMusicVol >= 0) { v[4] = g_savedMusicVol; g_savedMusicVol = -1; }
	if (mute && g_savedRadioVol < 0) { g_savedRadioVol = v[5]; v[5] = 0; }
	if (!mute && g_savedRadioVol >= 0) { v[5] = g_savedRadioVol; g_savedRadioVol = -1; }
}
// The car radio's sources in M's order: the game's stations, your music, then the Need for Speed stations when the
// launcher found their songs (setup's radio on)
static bool nfsRadioAvail() { return g_tracks > 0 && iniI("radio", 1); }
static int nextSource(int m)
{
	int n = m == 2 ? 1 : m == 1 ? 0 : 2;
	if (n == 0 && !nfsRadioAvail()) n = 2;
	return n;
}
static const char *sourceName(int m) { return m == 1 ? "your music" : m == 0 ? "the Need for Speed stations" : "the game's radio"; }

static void radioNext()
{
	if (g_tracks <= 0 || !iniI("radio", 1)) return;
	g_track = g_track % g_tracks + 1;
	char k[32], val[300], path[400], line[300];
	rt::fmt(k, sizeof k, "track.%d", g_track);
	iniStr(k, val, sizeof val, "");
	char *bar = val; while (*bar && *bar != '|') bar++;
	const char *title = *bar ? bar + 1 : "Need for Speed";
	*bar = 0;
	char dir[300];
	iniStr("radioDir", dir, sizeof dir, "");
	if (!dir[0]) rt::fmt(dir, sizeof dir, "%sradio\\", g_pluginDir);
	rt::fmt(path, sizeof path, "%s%s", dir, val);
	audio::radioPlay(path);
	g_radioOn = true;
	muteGameMusic(true);
	rt::fmt(line, sizeof line, "Radio: %s", title);
	message(line, 4);
	rt::strcpy(g_radioTitle, title, sizeof g_radioTitle);
}
static void radioOff()
{
	if (!g_radioOn) return;
	audio::radioStop();
	g_radioOn = false;
	muteGameMusic(false);
}
// The radio's buttons click (made in radio.cpp; a touch different each time)
static void click(int kind)
{
	static const float kLoud[radio::kClicks] = {1.0f, 0.85f, 0.6f, 0.75f, 1.0f};
	static u32 seed = 0x2545F491;
	seed = seed * 1664525u + 1013904223u;
	float j = (float)((seed >> 9) & 0xFFFF) / 65536.0f;
	audio::playClick(kind, TUNE(radioClickVolume) * kLoud[kind], 0.96f + 0.08f * j);
}
static void radioPrev()
{
	if (g_tracks <= 0) return;
	g_track = ((g_track - 2) % g_tracks + g_tracks) % g_tracks;
	radioNext();
}

// Your own music as the car's radio (music.h): whatever plays on your PC - Spotify, a YouTube video - is started when
// you get in or switch to it (M), paused when you get out or switch back; N / B skip, = / - its volume.
static bool g_musicOn, g_musicWantPlay;
static u32 g_musicTitleVer, g_musicVolVer, g_musicNoteVer, g_musicPolls;
static int g_musicAvail = -1;
static double g_musicOnT;
static void musicOn(bool announce)
{
	music::start();
	music::setActive(true);
	radioOff();
	muteGameMusic(true);
	g_musicOn = true; g_musicWantPlay = true; g_musicOnT = rt::now();
	music::State st;
	music::state(&st);
	g_musicTitleVer = st.titleVer - 1;
	g_musicPolls = st.polls;
	if (announce)
	{
		char m[160];
		rt::fmt(m, sizeof m, "Radio: your music (N next, B back, = / - its volume, M: %s)", sourceName(nextSource(1)));
		message(m, 4);
	}
}
static void musicOff(bool pause)
{
	if (!g_musicOn) return;
	music::State st;
	music::state(&st);
	if (pause && st.status == 4) music::command(music::CMD_PAUSE);
	music::setActive(false);
	audio::setExternalBass(false, 0);
	g_musicOn = false; g_musicWantPlay = false;
	muteGameMusic(false);
}
// ---- The dash screen
template <typename... A> static long vcall(u32 o, int slot, A... a)
{
	typedef long(__stdcall * Fn)(u32, A...);
	return ((Fn)(ptr(ptr(o) + 4 * slot)))(o, a...);
}
// ---- The game's own radio in the car
namespace gradio
{
static radio::Analyzer an;
static u32 snd, buf, bufBytes, lastPos, align = 4, chans = 2, bits = 16, dsLo;
static float rate;
static bool live;
static char station[64], title[160];
static u32 loggedOff = 0xFFFFFFFF, loggedSnd;
static u32 findSound()
{
	u32 map = ADDR_AUDIO_MANAGER + 0x54;
	u32 nb = rd<u32>(map + 4), bk = ptr(map + 8);
	if (!nb || nb > 4096 || !readable(bk, nb * 4)) return 0;
	u32 best = 0;
	for (u32 i = 0; i < nb; i++)
	{
		int guard = 0;
		for (u32 e = ptr(bk + 4 * i); e && guard < 64; guard++)
		{
			if (!readable(e, 12)) break;
			u32 s = ptr(e + 8);
			if (readable(s, 0x230))
			{
				u32 fl = rd<u32>(s + 8), st = rd<u32>(s + 0x10);
				if ((fl & 0x100000) && !(st & 0x10)) { best = s; if (s == snd) return s; }
			}
			e = ptr(e);
		}
	}
	return best;
}
static u32 __cdecl probeThunk(u32 o, u32)
{
	u32 caps[5] = {20, 0, 0, 0, 0};
	if (vcall(o, 3, (u32)caps) != 0 || caps[2] < 1024 || caps[2] > (16u << 20)) return 0;
	u16 wf[9] = {0}; u32 got = 0;
	if (vcall(o, 5, (u32)wf, (u32)sizeof wf, (u32)&got) != 0) return 0;
	if (wf[0] != 1 || (wf[1] != 1 && wf[1] != 2) || (wf[7] != 16)) return 0;
	bufBytes = caps[2]; chans = wf[1]; bits = wf[7]; align = wf[6] ? wf[6] : chans * 2;
	rate = (float)(*(u32 *)&wf[2]);
	return 1;
}
static u32 findBuffer(u32 s)
{
	if (!dsLo)
	{
		u32 h = (u32)LoadLibraryA("dsound.dll");
		dsLo = h;
		if (!h) return 0;
	}
	for (u32 off = 0x198; off < 0x230; off += 4)
	{
		u32 o = ptr(s + off);
		if (!readable(o, 4) || !readable(ptr(o), 0x50)) continue;
		u32 mbi[7] = {0};
		if (!s_vq || !s_vq((const void *)ptr(o), mbi, sizeof mbi) || mbi[1] != dsLo) continue;
		g_gThread = GetCurrentThreadId();
		u32 ok = guardedCall2((u32)&probeThunk, o, 0);
		if (g_gFault) { g_gFault = 0; continue; }
		if (ok)
		{
			if (off != loggedOff) { loggedOff = off; LOG("game radio: its DirectSound buffer at sound+%x (%u bytes, %u Hz, %u channels)", off, bufBytes, (u32)rate, chans); }
			return o;
		}
	}
	return 0;
}
static u32 __cdecl tapThunk(u32 o, u32 maxBytes)
{
	u32 play = 0, write = 0;
	if (vcall(o, 4, (u32)&play, (u32)&write) != 0 || play >= bufBytes) return 0;
	u32 n = (play + bufBytes - lastPos) % bufBytes;
	if (n > maxBytes) { lastPos = (play + bufBytes - maxBytes) % bufBytes; n = maxBytes; }
	n -= n % align;
	if (n < align) return 0;
	u32 p1 = 0, l1 = 0, p2 = 0, l2 = 0;
	if (vcall(o, 11, lastPos, n, (u32)&p1, (u32)&l1, (u32)&p2, (u32)&l2, (u32)0) != 0) return 0;
	for (int part = 0; part < 2; part++)
	{
		const short *q = (const short *)(part ? p2 : p1);
		u32 frames = (part ? l2 : l1) / align;
		for (u32 i = 0; q && i < frames; i++)
		{
			float m = chans == 2 ? 0.5f * ((float)q[2 * i] + (float)q[2 * i + 1]) : (float)q[i];
			an.push(m * (1.0f / 32768.0f));
		}
	}
	vcall(o, 19, p1, l1, p2, l2);
	lastPos = play;
	return 1;
}
static bool hasI(const char *h, const char *n)
{
	for (; *h; h++)
	{
		int k = 0;
		while (n[k] && h[k] && (h[k] | 32) == (n[k] | 32)) k++;
		if (!n[k]) return true;
	}
	return false;
}
static void songTitle(const char *path)
{
	const char *nm = path;
	for (const char *q = path; *q; q++) if (*q == '\\' || *q == '/') nm = q + 1;
	if (hasI(path, "\\voice\\") || hasI(path, "dialog")) { rt::strcpy(title, "ON AIR", sizeof title); return; }
	if ((nm[0] == 'm' || nm[0] == 'M') && (nm[1] == 'u' || nm[1] == 'U') && (nm[2] == 's' || nm[2] == 'S') && nm[3] == '_') nm += 4;
	int k = 0;
	for (; *nm && *nm != '.' && k < (int)sizeof title - 1; nm++) title[k++] = *nm == '_' ? ' ' : (*nm >= 'a' && *nm <= 'z' ? *nm - 32 : *nm);
	title[k] = 0;
}
static void stationName()
{
	station[0] = 0;
	u32 e = ptr(0x11DD42C);
	u32 ref = readable(e, 4) ? ptr(e) : 0;
	u32 base = readable(ref, 0x24) ? ptr(ref + OFF_REF_BASE) : 0;
	const char *nm = readable(base, 0x38) ? (const char *)ptr(base + 0x34) : 0;
	if (!nm || !readable((u32)nm, 64)) return;
	for (int k = 0; k < 63 && nm[k]; k++)
	{
		if ((u8)nm[k] < 32 || (u8)nm[k] > 126) { station[0] = 0; return; }
		station[k] = nm[k] >= 'a' && nm[k] <= 'z' ? nm[k] - 32 : nm[k];
		station[k + 1] = 0;
	}
}
static void census()
{
	u32 map = ADDR_AUDIO_MANAGER + 0x54;
	u32 nb = rd<u32>(map + 4), bk = ptr(map + 8), cnt = rd<u32>(map + 12);
	stationName();
	u32 e0 = ptr(0x11DD42C);
	LOG("game radio: none found - %u sounds playing; the Pip-Boy's station: %s (entry %x)", cnt, station[0] ? station : "none", e0);
	if (!nb || nb > 4096 || !readable(bk, nb * 4)) return;
	int shown = 0, all = 0;
	for (u32 i = 0; i < nb; i++)
		for (u32 e = ptr(bk + 4 * i), guard = 0; e && guard < 64; guard++, e = readable(e, 12) ? ptr(e) : 0)
		{
			if (!readable(e, 12)) break;
			u32 s = ptr(e + 8);
			all++;
			if (!readable(s, 0x198)) continue;
			char path[160];
			int k = 0;
			for (; k < 159 && *(const char *)(s + 0x36 + k); k++) path[k] = *(const char *)(s + 0x36 + k);
			path[k] = 0;
			u32 fl = rd<u32>(s + 8);
			if (!(fl & (0x800 | 0x100000 | 4)) && !hasI(path, "song") && !hasI(path, "radio") && !hasI(path, "music") && !hasI(path, "voice")) continue;
			if (shown++ < 24) LOG("game radio:   key %x flags %x state %x vol %.2f %s", rd<u32>(e + 4), fl, rd<u32>(s + 0x10), rd<float>(s + 0x24), path);
		}
	LOG("game radio:   (%d of %d sounds looked through were music, radio or voice)", shown, all);
}
static double nextScan, nextStation, lastCensus = -999;
static int censuses;
static void tick(float dt)
{
	double now = rt::now();
	if (snd && (!readable(snd, 0x198) || !(rd<u32>(snd + 8) & 0x100000) || (rd<u32>(snd + 0x10) & 0x10))) { snd = 0; buf = 0; nextScan = 0; }
	if (!snd && now >= nextScan)
	{
		nextScan = now + 1;
		u32 s = findSound();
		if (s) { snd = s; buf = 0; lastPos = 0; if (s != loggedSnd) { loggedSnd = s; LOG("game radio: playing %s", (const char *)(s + 0x36)); } }
		else if (now - lastCensus > 60 && censuses < 3) { lastCensus = now; censuses++; census(); }
	}
	if (now >= nextStation) { nextStation = now + 1; stationName(); }
	u32 s = snd;
	if (!s) { live = false; for (int k = 0; k < radio::kBands; k++) an.env[k] *= 0.8f; title[0] = 0; return; }
	char path[256];
	for (int k = 0; k < 253; k++) { path[k] = *(const char *)(s + 0x36 + k); if (!path[k]) break; path[k + 1] = 0; }
	path[253] = 0;
	songTitle(path);
	if (!buf)
	{
		buf = findBuffer(s);
		if (!buf) { live = false; return; }
		an.configure(rate);
		g_gThread = GetCurrentThreadId();
		lastPos = 0;
		guardedCall2((u32)&tapThunk, buf, 0);
		if (g_gFault) { g_gFault = 0; buf = 0; return; }
	}
	g_gThread = GetCurrentThreadId();
	u32 max = (u32)(rate * rt::clamp(dt, 0.005f, 0.2f)) * align + 64 * align;
	u32 ok = guardedCall2((u32)&tapThunk, buf, max);
	if (g_gFault) { g_gFault = 0; LOG("game radio: reading its buffer faulted - looked for again"); buf = 0; live = false; return; }
	live = ok != 0 || live;
}
static void bands(float *out) { for (int k = 0; k < radio::kBands; k++) out[k] = an.env[k]; }
}

// The first object called `name` under node (depth levels down)
static u32 findNamed(u32 node, const char *name, int depth)
{
	u32 arr = ptr(node + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(node + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 512) return 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (nm && !rt::strcmp(nm, name)) return c;
	}
	if (depth <= 0) return 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		u32 vt = c ? ptr(c) : 0;
		if (!vt || ptr(vt + 0x0C) != 0x6815C0) continue;
		if (u32 f = findNamed(c, name, depth - 1)) return f;
	}
	return 0;
}
static u32 g_screenPx[screen::W * screen::H];
static double g_screenT;
static bool g_screenDark;
static u32 g_screenLogged;
static u32 screenTexture(TESObjectREFR *ref)
{
	u32 root = ref ? thirdPersonNode(ref) : 0;
	u32 arr = root ? ptr(root + OFF_NODE_CHILDREN) : 0;
	u16 n = root ? rd<u16>(root + OFF_NODE_CHILDCOUNT) : 0;
	if (!arr || n > 512) return 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm) continue;
		u32 sh = 0;
		if (!rt::strcmp(nm, "MMWScreen")) sh = ptr(c + OFF_GEOM_SHADER);
		else if (!memcmp(nm, "MMWPipBoy ", 10))
		{
			u32 g = findNamed(c, "glare", 4);
			sh = g ? ptr(g + OFF_GEOM_SHADER) : 0;
		}
		else continue;
		if (!sh || rd<u32>(sh + OFF_SHADER_TYPE) != SHADER_PPLIGHTING) return 0;
		u32 slot = ptr(sh + OFF_PPL_TEXTURES), tex = slot ? ptr(slot) : 0, td = tex ? ptr(tex + 0x24) : 0;
		return td ? ptr(td + 0x64) : 0;
	}
	return 0;
}
// The screen's kind, from its texture's size: 1 a full screen (equalizer and video), 2 a single-DIN radio's display
// strip (equalizer only), 0 none we can draw on
static int g_screenKind;
static int screenKindOf(u32 d3d)
{
	static u32 known;
	static int kind;
	if (d3d == known) return kind;
	u32 desc[8] = {0};
	if (vcall(d3d, 17, 0u, (void *)desc) < 0) return 0;
	bool argb = desc[0] == 21 || desc[0] == 22;
	kind = !argb ? 0 : desc[6] == (u32)screen::W && desc[7] == (u32)screen::H ? 1 : desc[6] == (u32)screen::DW && desc[7] == (u32)screen::DH ? 2
		: desc[6] == (u32)screen::PW && (desc[7] == (u32)screen::PH || desc[7] == (u32)screen::PW) ? 3 : 0;
	known = d3d;
	if (g_screenLogged != d3d)
	{
		g_screenLogged = d3d;
		LOG("screen: texture %x format %u pool %u %ux%u%s", d3d, desc[0], desc[3], desc[6], desc[7], kind == 1 ? "" : kind == 2 ? " (single-DIN display)" : kind == 3 ? " (dash Pip-Boy)" : " - can't draw on it");
	}
	return kind;
}
// The picture's turn and slant on the car's screen
static float g_scrTurn, g_scrSlant;
static int g_scrCar = -2;
static void screenTurnPath(char *p, u32 cap) { rt::fmt(p, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\screen.ini", g_dir); }
static void screenTurnLoad(int car)
{
	g_scrCar = car;
	g_scrTurn = car >= 0 ? kCars[car].scrTurn : 0;
	g_scrSlant = car >= 0 ? kCars[car].scrSlant : 0;
	if (car < 0) return;
	char p[320];
	screenTurnPath(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (!b) return;
	u32 n = (u32)strlen(kCars[car].id);
	for (char *q = b; *q; q++)
		if ((q == b || q[-1] == '\n') && !memcmp(q, kCars[car].id, n) && q[n] == '=')
		{
			char *v = q + n + 1;
			g_scrTurn = rt::clamp(rt::parsef(v), -45, 45);
			while (*v && *v != ',' && *v != '\n') v++;
			if (*v == ',') g_scrSlant = rt::clamp(rt::parsef(v + 1), -45, 45);
			break;
		}
	rt::free(b);
}
static void screenTurnSave()
{
	if (g_scrCar < 0) return;
	char p[320];
	screenTurnPath(p, sizeof p);
	static char out[8192];
	int n = rt::fmt(out, sizeof out, "; Mojave Enhanced: each car's dash screen picture, turned and slanted (degrees) - J + arrow keys in the car\r\n");
	char *b = rt::readfile(p, 0);
	u32 idn = (u32)strlen(kCars[g_scrCar].id);
	if (b)
	{
		for (char *q = b; *q;)
		{
			char *e = q;
			while (*e && *e != '\n') e++;
			bool mine = !memcmp(q, kCars[g_scrCar].id, idn) && q[idn] == '=';
			if (!mine && *q != ';' && e > q + 2 && n + (e - q) + 2 < (int)sizeof out)
			{
				memcpy(out + n, q, (u32)(e - q)); n += (int)(e - q);
				if (out[n - 1] != '\r') out[n++] = '\r';
				out[n++] = '\n';
			}
			q = *e ? e + 1 : e;
		}
		rt::free(b);
	}
	n += rt::fmt(out + n, sizeof out - n, "%s=%.1f,%.1f\r\n", kCars[g_scrCar].id, g_scrTurn, g_scrSlant);
	rt::writefile(p, out, (u32)n, false);
}
static bool screenUpload(u32 d3d)
{
	int kind = screenKindOf(d3d);
	if (!kind) return false;
	int w = kind == 2 ? screen::DW : kind == 3 ? screen::PW : screen::W, h = kind == 2 ? screen::DH : kind == 3 ? screen::PH : screen::H;
	if (g_scrCar != g_activeCar) screenTurnLoad(g_activeCar);
	struct { i32 pitch; u8 *bits; } lr = {0, 0};
	if (vcall(d3d, 19, 0u, (void *)&lr, (void *)0, 0u) < 0 || !lr.bits) return false;
	if (kind == 3)
	{
		u32 desc[8] = {0};
		vcall(d3d, 17, 0u, (void *)desc);
		int th = desc[7] ? (int)desc[7] : h;
		static u32 lv[2][screen::PW * screen::PW];
		bool keep = th <= screen::PW && w <= screen::PW;
		for (int y = 0; y < th; y++)
		{
			int sy = y * h / th;
			memcpy(lr.bits + y * lr.pitch, g_screenPx + sy * w, w * 4);
			if (keep) memcpy(lv[0] + y * w, g_screenPx + sy * w, w * 4);
		}
		vcall(d3d, 20, 0u);
		u32 levels = keep ? (u32)vcall(d3d, 13) : 1;
		int cw = w, ch = th, cur = 0;
		for (u32 l = 1; l < levels && l < 9 && (cw > 1 || ch > 1); l++)
		{
			int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
			const u32 *a = lv[cur];
			u32 *b = lv[cur ^ 1];
			for (int y = 0; y < nh; y++)
				for (int x = 0; x < nw; x++)
				{
					int x0 = 2 * x < cw ? 2 * x : cw - 1, x1 = 2 * x + 1 < cw ? 2 * x + 1 : cw - 1;
					int y0 = 2 * y < ch ? 2 * y : ch - 1, y1 = 2 * y + 1 < ch ? 2 * y + 1 : ch - 1;
					u32 p[4] = {a[y0 * cw + x0], a[y0 * cw + x1], a[y1 * cw + x0], a[y1 * cw + x1]}, o = 0;
					for (int c = 0; c < 32; c += 8)
						o |= (((p[0] >> c & 255) + (p[1] >> c & 255) + (p[2] >> c & 255) + (p[3] >> c & 255) + 2) / 4) << c;
					b[y * nw + x] = o;
				}
			struct { i32 pitch; u8 *bits; } ml = {0, 0};
			if (vcall(d3d, 19, l, (void *)&ml, (void *)0, 0u) < 0 || !ml.bits) break;
			for (int y = 0; y < nh; y++) memcpy(ml.bits + y * ml.pitch, b + y * nw, nw * 4);
			vcall(d3d, 20, l);
			cw = nw; ch = nh; cur ^= 1;
		}
		return true;
	}
	if (g_scrTurn == 0 && g_scrSlant == 0)
		for (int y = 0; y < h; y++) memcpy(lr.bits + y * lr.pitch, g_screenPx + y * w, w * 4);
	else
	{
		float th = g_scrTurn * rt::PI / 180.0f, c = rt::cos(th), sn = rt::sin(th);
		float sk = rt::sin(g_scrSlant * rt::PI / 180.0f) / rt::cos(g_scrSlant * rt::PI / 180.0f);
		float cx = (float)(w - 1) * 0.5f, cy = (float)(h - 1) * 0.5f;
		const u32 bg = 0xFF0C0806;
		for (int y = 0; y < h; y++)
		{
			u32 *row = (u32 *)(lr.bits + y * lr.pitch);
			for (int x = 0; x < w; x++)
			{
				float dx = (float)x - cx, dy = (float)y - cy;
				float x1 = dx * c - dy * sn, y1 = dx * sn + dy * c;
				float sx = x1 + sk * y1 + cx, sy = y1 + cy;
				int ix = (int)(sx + 1000) - 1000, iy = (int)(sy + 1000) - 1000;
				float fx = sx - (float)ix, fy = sy - (float)iy;
				u32 p00 = bg, p10 = bg, p01 = bg, p11 = bg;
				if (iy >= 0 && iy < h)
				{
					if (ix >= 0 && ix < w) p00 = g_screenPx[iy * w + ix];
					if (ix + 1 >= 0 && ix + 1 < w) p10 = g_screenPx[iy * w + ix + 1];
				}
				if (iy + 1 >= 0 && iy + 1 < h)
				{
					if (ix >= 0 && ix < w) p01 = g_screenPx[(iy + 1) * w + ix];
					if (ix + 1 >= 0 && ix + 1 < w) p11 = g_screenPx[(iy + 1) * w + ix + 1];
				}
				u32 o = 0xFF000000u;
				for (int sh = 0; sh < 24; sh += 8)
				{
					float a = (float)((p00 >> sh) & 255) * (1 - fx) + (float)((p10 >> sh) & 255) * fx;
					float b = (float)((p01 >> sh) & 255) * (1 - fx) + (float)((p11 >> sh) & 255) * fx;
					o |= (u32)(a * (1 - fy) + b * fy + 0.5f) << sh;
				}
				row[x] = o;
			}
		}
	}
	vcall(d3d, 20, 0u);
	return true;
}
static void screenTick(double t)
{
	bool want = g_driving && g_screenMode != 0;
	{
		u32 d = g_driving ? screenTexture(vref(g_car)) : 0;
		g_screenKind = d ? screenKindOf(d) : 0;
	}
	bool din = g_screenKind == 2 || g_screenKind == 3;
	audio::setAnalyze(g_driving && (g_screenMode == 1 || (din && g_screenMode == 2)) && !g_musicOn);
	bool videoOn = g_driving && g_screenMode == 2 && !din;
	mirror::setActive(videoOn);
	static bool wasVideo;
	if (videoOn && !wasVideo)
	{
		char m[200];
		mirror::gameWindowInfo(m, sizeof m);
		LOG("screen video on: %s", m);
	}
	wasVideo = videoOn;
	if (!want && g_screenDark) return;
	if (want && g_screenMode == 2 && !din)
	{
		static u32 ver, shown;
		static int lastSt = -1;
		static double msgT, okT = -10;
		static bool videoUp;
		if (t - g_screenT < 1.0 / 30) return;
		g_screenT = t;
		u32 d3d = screenTexture(vref(g_car));
		if (!d3d) return;
		int st = mirror::frame(g_screenPx, screen::W, screen::H, &ver);
		if (st != lastSt)
		{
			static const char *kNames[6] = {"starting", "no video window", "browser minimized", "no picture", "showing", "finding the video"};
			LOG("screen video: %s (window \"%s\")", kNames[st >= 0 && st < 6 ? st : 0], mirror::windowTitle());
			lastSt = st; msgT = 0; shown = ver - 1;
		}
		if (st == mirror::OK) okT = t;
		static u32 noteVer;
		char nb[160];
		if (mirror::lastNote(nb, sizeof nb, &noteVer)) LOG("screen video: %s", nb);
		if (st == mirror::OK && ver == shown) return;
		if (st != mirror::OK && videoUp && t - okT < (st == mirror::SEARCHING ? 3.0 : 0.75)) return;
		if (st == mirror::OK)
		{
			u32 k = (u32)(rt::clamp(TUNE(screenVideoBrightness), 0, 1) * 256);
			for (int i = 0; i < screen::W * screen::H; i++)
			{
				u32 c = g_screenPx[i];
				g_screenPx[i] = 0xFF000000u | ((((c >> 16) & 255) * k >> 8) << 16) | ((((c >> 8) & 255) * k >> 8) << 8) | ((c & 255) * k >> 8);
			}
			if (screenUpload(d3d)) { shown = ver; g_screenDark = false; videoUp = true; }
		}
		else if (t - msgT > 0.5)
		{
			msgT = t;
			videoUp = false;
			if (st == mirror::MINIMIZED) screen::message(g_screenPx, "BROWSER MINIMIZED", "RESTORE IT - IT CAN STAY BEHIND THE GAME");
			else if (st == mirror::BLACK) screen::message(g_screenPx, "NO PICTURE", "THE BROWSER ISN'T DRAWING ITS VIDEO");
			else if (st == mirror::NO_WINDOW) screen::message(g_screenPx, "NO VIDEO", "PLAY A YOUTUBE VIDEO IN YOUR BROWSER");
			else if (st == mirror::SEARCHING) screen::message(g_screenPx, "VIDEO", "FINDING THE VIDEO...");
			else screen::message(g_screenPx, "VIDEO", "LOOKING FOR YOUR BROWSER...");
			if (screenUpload(d3d)) g_screenDark = false;
		}
		return;
	}
	if (want && t - g_screenT < 1.0 / 30) return;
	float dt = (float)rt::clamp((float)(t - g_screenT), 0, 0.2f);
	g_screenT = t;
	TESObjectREFR *ref = g_driving ? vref(g_car) : (g_parkedRef ? (TESObjectREFR *)lookup(g_parkedRef) : 0);
	u32 d3d = screenTexture(ref);
	if (!d3d)
	{
		if (!want) g_screenDark = true;
		return;
	}
	screen::Input in;
	memset(&in, 0, sizeof in);
	in.on = want;
	static char src[48], title[200];
	if (g_musicOn)
	{
		music::State st;
		music::state(&st);
		rt::fmt(src, sizeof src, "%s", st.app[0] ? st.app : "YOUR MUSIC");
		rt::fmt(title, sizeof title, "%s", st.title);
		in.beat = music::level();
	}
	else if (!g_radioOn && g_musicMode == 2)
	{
		gradio::tick(dt);
		rt::fmt(src, sizeof src, "%s", gradio::station[0] ? gradio::station : "RADIO");
		rt::fmt(title, sizeof title, "%s", gradio::snd ? (gradio::title[0] ? gradio::title : "ON AIR") : "TUNE IN ON YOUR PIP-BOY");
		in.bandsLive = gradio::live;
		float b[radio::kBands];
		gradio::bands(b);
		for (int k = 0; k < screen::kBars && k < radio::kBands; k++) in.bands[k] = b[k];
	}
	else
	{
		rt::fmt(src, sizeof src, "%s", g_radioOn ? "NFS RADIO" : "RADIO");
		rt::fmt(title, sizeof title, "%s", g_radioOn ? g_radioTitle : "PRESS N");
		in.bandsLive = g_radioOn;
		float b[radio::kBands];
		audio::bands(b);
		for (int k = 0; k < screen::kBars && k < radio::kBands; k++) in.bands[k] = b[k];
	}
	in.source = src; in.title = title; in.sub = g_sub;
	int kind = screenKindOf(d3d);
	if (kind == 2) screen::drawDin(g_screenPx, in, dt);
	else if (kind == 3) screen::drawPip(g_screenPx, in, dt);
	else screen::draw(g_screenPx, in, dt);
	if (screenUpload(d3d)) g_screenDark = !want;
}

// The game's radio followed even in a car with no dash screen (its log lines tell whether it's found), 10 times a
// second
static void gradioIdle(double t)
{
	static double last;
	if (!g_driving || g_musicMode != 2 || g_radioOn || g_musicOn || g_screenKind) return;
	if (t - last < 0.1) return;
	float dt = (float)rt::clamp((float)(t - last), 0, 0.2f);
	last = t;
	gradio::tick(dt);
}
static void musicTick()
{
	music::State st;
	music::state(&st);
	if (st.noteVer != g_musicNoteVer) { g_musicNoteVer = st.noteVer; if (st.note[0]) LOG("%s", st.note); }
	if (!g_musicOn) return;
	if (st.available == 0 && g_musicAvail != 0) message("Your music needs Windows 10 (October 2018 update) or newer.", 5);
	g_musicAvail = st.available;
	if (st.available != 1) return;
	if (g_musicWantPlay && st.polls != g_musicPolls)
	{
		if (st.status == 4) g_musicWantPlay = false;
		else if (st.sessions > 0) { music::command(music::CMD_PLAY); g_musicWantPlay = false; }
		else if (rt::now() - g_musicOnT > 1.5)
		{
			message("Your music: no player is open - start Spotify or a YouTube video on your PC.", 5);
			g_musicWantPlay = false;
		}
	}
	if (st.titleVer != g_musicTitleVer)
	{
		g_musicTitleVer = st.titleVer;
		if (st.title[0])
		{
			char m[240];
			rt::fmt(m, sizeof m, "Your music (%s): %s", st.app[0] ? st.app : "player", st.title);
			message(m, 4);
		}
	}
	if (st.volVer != g_musicVolVer)
	{
		g_musicVolVer = st.volVer;
		char m[96];
		if (st.volume >= 0) rt::fmt(m, sizeof m, "%s volume: %d%%", st.app[0] ? st.app : "Player", st.volume);
		else rt::fmt(m, sizeof m, "Your music: couldn't find the player's volume");
		message(m, 1.2f);
	}
}

// Engine note: gears over the car's own top speed, revs between the recording's lowest and highest RPM
static int g_gear;
static float g_rpm;
static float g_revsOverride = -1;
static void beamFollow(Beam &b, u32 form, float x, float y, float z)
{
	if (b.old) { removeRef((TESObjectREFR *)lookup(b.old)); b.old = 0; }
	TESObjectREFR *r = b.ref ? (TESObjectREFR *)lookup(b.ref) : 0;
	float dx = x - b.sx, dy = y - b.sy;
	if (!r || dx * dx + dy * dy > 4000.0f * 4000.0f)
	{
		TESObjectREFR *n = placeRef(form, x, y, z, 0);
		if (!n) return;
		if (r) b.old = b.ref;
		b.ref = refid(n); b.sx = x; b.sy = y;
		return;
	}
	moveRef(r, x, y, z, 0, 0, 0);
}
static void beamRemove(Beam &b)
{
	if (b.ref) removeRef((TESObjectREFR *)lookup(b.ref));
	if (b.old) removeRef((TESObjectREFR *)lookup(b.old));
	b.ref = b.old = 0;
}
// The cockpit of the car you're driving (x right, y forward, z up from the ground between the axles)
static float carIni(const char *what, float def)
{
	if (g_activeCar < 0) return def;
	char k[64];
	rt::fmt(k, sizeof k, "car.%s.%s", kCars[g_activeCar].id, what);
	return iniF(k, def);
}
static const CarDef *cockpitRow() { return g_activeCar >= 0 && kCars[g_activeCar].cockpit ? &kCars[g_activeCar] : 0; }
static void seatEyePos(float e[3])
{
	const CarDef *cd = cockpitRow();
	if (cd) { e[0] = cd->eye[0]; e[1] = cd->eye[1]; e[2] = cd->eye[2]; }
	else { e[0] = -g_car.halfTrack * iniF("seatSide", 0.42f); e[1] = -g_car.halfBase * iniF("seatBack", 0.12f); e[2] = TUNE(seatEye); }
	e[0] = carIni("eyeX", e[0]); e[1] = carIni("eyeY", e[1]); e[2] = carIni("eyeZ", e[2]);
}
static void hoodEyePos(float e[3])
{
	const CarDef *cd = cockpitRow();
	e[0] = 0;
	if (cd && cd->hood[1] > 0) { e[1] = cd->hood[0]; e[2] = cd->hood[1]; }
	else { e[1] = g_car.halfBase * iniF("hoodForward", 0.55f); e[2] = iniF("hoodHeight", 92); }
	e[1] = carIni("hoodY", e[1]); e[2] = carIni("hoodZ", e[2]);
}
static float cabinFloor() { const CarDef *cd = cockpitRow(); return carIni("floor", cd ? cd->floor : g_car.clearance + TUNE(ridersFloorAboveClearance)); }
static float seatRecline() { const CarDef *cd = cockpitRow(); return carIni("recline", cd && cd->recline > 0 ? cd->recline : TUNE(ridersReclineDegrees)); }

// Headlights: off, low beams or high beams (a third source highBeamReachUnits further out, lighting the road far
// ahead).
static void carLights(bool braking, bool reversing)
{
	bool on = g_headMode > 0, high = g_headMode == 2;
	setLights(g_car, (on ? 1 | 2 : 0) | (braking ? 4 : 0) | (reversing ? 8 : 0));
	float h = g_car.heading * rt::PI / 180.0f, sn = rt::sin(h), cs = rt::cos(h);
	float eye[3]; seatEyePos(eye);
	float seat = eye[1], gap = TUNE(lightCabinGapUnits);
	float cabinFront = seat + TUNE(lightCabinAheadUnits), cabinBack = seat - TUNE(lightCabinBehindUnits);
	float f = rt::max(kLight_head_fwd + g_car.front, cabinFront + kLight_head_radius + gap);
	float off = on ? 0 : 4000.0f;
	beamFollow(g_headBeam, kLight_head, g_car.x + sn * f, g_car.y + cs * f, g_car.z + kLight_head_up - off);
	float fn = rt::max(kLight_headNear_fwd + g_car.front, cabinFront + kLight_headNear_radius + gap);
	beamFollow(g_headNear, kLight_headNear, g_car.x + sn * fn, g_car.y + cs * fn, g_car.z + kLight_headNear_up - off);
	float ff = f + TUNE(highBeamReachUnits);
	beamFollow(g_headFar, kLight_head, g_car.x + sn * ff, g_car.y + cs * ff, g_car.z + kLight_head_up + TUNE(highBeamUpUnits) - (high ? 0 : 4000.0f));
	float b = rt::min(kLight_brake_fwd - g_car.rear, cabinBack - kLight_brake_radius - gap);
	beamFollow(g_brakeBeam, kLight_brake, g_car.x + sn * b, g_car.y + cs * b, g_car.z + kLight_brake_up - (braking ? 0 : 4000.0f));
}

static bool driftRevving(float speed, bool throttle);
static void limiterTick(bool throttle, float speed, float frac);
static float g_revFrac;
static float g_gearFrac;
// Shift lights: the converter puts a glowing disc over each LED painted on the dash, MMWShiftLED0..n-1 left to right,
// hidden.
static void shiftLights(Vehicle &v, bool on)
{
	TESObjectREFR *r = vref(v);
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) return;
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 256) return;
	u32 led[16];
	int cnt = 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || memcmp(nm, "MMWShiftLED", 11)) continue;
		int k = rt::parsei(nm + 11);
		if (k < 0 || k >= 16) continue;
		while (cnt <= k) led[cnt++] = 0;
		led[k] = c;
	}
	if (!cnt) return;
	float lo = TUNE(shiftLightFirst), hi = TUNE(shiftLightLast);
	bool flash = on && (g_gearFrac >= TUNE(shiftLightFlash) || g_revFrac >= 0.975f);
	bool blinkOn = ((int)(rt::now() * 2 * TUNE(shiftLightBlinkHz))) & 1;
	for (int i = 0; i < cnt; i++)
	{
		if (!led[i]) continue;
		float thr = cnt > 1 ? lo + i * (hi - lo) / (cnt - 1) : lo;
		bool lit = flash ? blinkOn : on && g_revFrac >= thr;
		u32 &f = *(u32 *)(led[i] + OFF_NIAV_FLAGS);
		f = lit ? (f & ~1u) : (f | 1u);
	}
}
static void updateEngine(float dt, float speed, float topMs, bool throttle)
{
	static const float kGearTop[6] = {0.24f, 0.38f, 0.53f, 0.68f, 0.84f, 1.0f};
	float minR = audio::engineMinRPM(), maxR = audio::engineMaxRPM();
	{
		float lo = carIni("rpmLow", 0), hi = carIni("rpmHigh", 0);
		if (hi > lo + 500 && lo > 0)
		{
			float rlo = minR, rhi = maxR;
			minR = rt::clamp(lo, rlo, rhi - 500);
			maxR = rt::clamp(hi, minR + 500, rhi);
		}
	}
	float v = rt::absf(speed);
	if (speed < -0.5f) g_gear = 0;
	float frac = v / (topMs * kGearTop[g_gear]);
	if (frac > 0.96f && g_gear < 5 && speed > 0) { g_gear++; frac = v / (topMs * kGearTop[g_gear]); }
	if (g_gear > 0 && v / (topMs * kGearTop[g_gear - 1]) < 0.62f) { g_gear--; frac = v / (topMs * kGearTop[g_gear]); }
	g_gearFrac = speed > 0 && g_gear < 5 ? frac : 0;
	float target = minR + rt::clamp(frac, 0, 1.02f) * (maxR - minR);
	if (v < 3 && throttle) target = rt::clamp(target, minR + (maxR - minR) * 0.45f, maxR);
	if (g_revsOverride >= 0) target = minR + (maxR - minR) * rt::clamp(g_revsOverride, 0, 1.02f);
	if (driftRevving(speed, throttle)) target = minR + (maxR - minR) * 1.03f;
	g_rpm += (target - g_rpm) * rt::clamp(dt * 7, 0, 1);
	if (g_rpm < minR) g_rpm = minR;
	g_revFrac = (g_rpm - minR) / rt::max(maxR - minR, 1);
	limiterTick(throttle, speed, (g_rpm - minR) / rt::max(maxR - minR, 1));
	static float load;
	float want = throttle ? 1.0f : 0.0f;
	load += rt::clamp(want - load, -dt / rt::max(TUNE(engineLoadOffSeconds), 0.01f), dt / rt::max(TUNE(engineLoadOnSeconds), 0.01f));
	audio::setEngine(true, g_rpm, load, iniF("engineVolume", 0.55f));
}
// ---- The dash gauges
static u32 gaugeTexture(TESObjectREFR *ref)
{
	u32 root = ref ? thirdPersonNode(ref) : 0;
	u32 arr = root ? ptr(root + OFF_NODE_CHILDREN) : 0;
	u16 n = root ? rd<u16>(root + OFF_NODE_CHILDCOUNT) : 0;
	if (!arr || n > 512) return 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || rt::strcmp(nm, "MMWGauge")) continue;
		u32 sh = ptr(c + OFF_GEOM_SHADER);
		if (!sh || rd<u32>(sh + OFF_SHADER_TYPE) != SHADER_PPLIGHTING) return 0;
		u32 slot = ptr(sh + OFF_PPL_TEXTURES), tex = slot ? ptr(slot) : 0, td = tex ? ptr(tex + 0x24) : 0;
		u32 d3d = td ? ptr(td + 0x64) : 0;
		if (!d3d) return 0;
		static u32 checked, ok;
		if (d3d != checked)
		{
			u32 desc[8] = {0};
			checked = d3d;
			ok = vcall(d3d, 17, 0u, (void *)desc) >= 0 && (desc[0] == 21 || desc[0] == 22) && desc[6] == (u32)screen::GW && desc[7] == (u32)screen::GH;
			LOG("gauges: texture %x format %u %ux%u%s", d3d, desc[0], desc[6], desc[7], ok ? "" : " - can't draw on it");
		}
		return ok ? d3d : 0;
	}
	return 0;
}
static void gaugeTick(double t)
{
	static double last;
	static bool wasLit;
	static u32 px[screen::GW * screen::GH];
	bool lit = g_driving;
	if (!lit && !wasLit) return;
	if (lit && t - last < 1.0 / 30) return;
	float dt = (float)rt::clamp((float)(t - last), 0, 0.2f);
	last = t;
	TESObjectREFR *ref = g_driving ? vref(g_car) : (g_parkedRef ? (TESObjectREFR *)lookup(g_parkedRef) : 0);
	u32 d3d = gaugeTexture(ref);
	if (!d3d) { wasLit = false; return; }
	screen::Gauge g;
	g.lit = lit;
	g.mph = rt::absf(g_car.speed) * 2.23694f;
	g.rpm = g_rpm;
	g.maxRpm = audio::engineMaxRPM();
	{
		float hi = carIni("rpmHigh", 0);
		if (hi > 1000) g.maxRpm = rt::min(g.maxRpm, hi);
	}
	g.gear = g_car.speed < -0.5f ? 0 : rt::absf(g_car.speed) < 0.5f ? -1 : g_gear + 1;
	for (int i = 0; i < (lit ? 1 : 25); i++) screen::drawGauge(px, g, lit ? dt : 0.05f);
	struct { i32 pitch; u8 *bits; } lr = {0, 0};
	if (vcall(d3d, 19, 0u, (void *)&lr, (void *)0, 0u) < 0 || !lr.bits) return;
	for (int y = 0; y < screen::GH; y++) memcpy(lr.bits + y * lr.pitch, px + y * screen::GW, screen::GW * 4);
	vcall(d3d, 20, 0u);
	wasLit = lit;
}

// ---- Enter / exit
static double g_enterT;
// ---- burnout
static bool g_burning;
static float g_launchT, g_smokeT, g_yawRate, g_driftSlip;
static float g_rearGrip = 1;
static float g_wheelieAim;
static float g_standLaunchT;
static bool g_standArmed;
static bool g_sliding;
static float rnd01();
static bool driftRevving(float speed, bool throttle)
{
	return TUNE(exhaustOn) > 0 && throttle && g_sliding && rt::absf(speed) > 3 && rt::absf(g_driftSlip) > TUNE(driftLimiterDegrees);
}
// The rev limiter bouncing while drifting on the throttle (or holding a low gear at the top): the mixer bounces the
// revs, gently (audio::setLimiter).
static void limiterTick(bool throttle, float speed, float frac)
{
	bool lim = TUNE(exhaustOn) > 0 && throttle && frac >= 0.985f && (g_gear < 5 || driftRevving(speed, throttle));
	audio::setLimiter(lim, TUNE(limiterHz), TUNE(limiterDrop), TUNE(limiterCut));
}
static u32 g_smokeRef[2];
static int g_smokeVar = -1;
static bool smokeHave(int i) { char k[16]; rt::fmt(k, sizeof k, "smoke%d", i); return iniI(k, 0) != 0; }
static void smokeStyleSave()
{
	char p[320];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\smoke2.ini", g_dir);
	char line[64];
	int n = rt::fmt(line, sizeof line, "smokeStyle=%d\r\n", g_smokeVar);
	rt::writefile(p, line, (u32)n, false);
}
static void smokeStyleLoad()
{
	char p[320];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\smoke2.ini", g_dir);
	char *b = rt::readfile(p, 0);
	g_smokeVar = b && rt::startswith(b, "smokeStyle=") ? rt::parsei(b + 11) : 0;
	if (b) rt::free(b);
	for (int k = 0; k < kFx_tiresmoke_variants && !smokeHave(g_smokeVar); k++) g_smokeVar = (g_smokeVar + 1) % kFx_tiresmoke_variants;
}
static void smokeRemove()
{
	for (int i = 0; i < 2; i++) if (g_smokeRef[i]) { removeRef((TESObjectREFR *)lookup(g_smokeRef[i])); g_smokeRef[i] = 0; }
}
static void smokeFollow(Vehicle &v)
{
	if (!iniI("smoke", 0)) return;
	if (g_smokeVar < 0 || g_smokeVar >= kFx_tiresmoke_variants) smokeStyleLoad();
	if (!smokeHave(g_smokeVar)) return;
	float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
	for (int i = 0; i < 2; i++)
	{
		float side = (i ? 1.0f : -1.0f) * v.halfTrack, fwd = -v.halfBase - 10.0f;
		float x = v.x + side * c + fwd * s, y = v.y - side * s + fwd * c, z = v.z + 4.0f;
		TESObjectREFR *r = g_smokeRef[i] ? (TESObjectREFR *)lookup(g_smokeRef[i]) : 0;
		if (!r)
		{
			r = placeRef(kFx_tiresmoke + (u32)g_smokeVar, x, y, z, 0);
			if (!r) return;
			g_smokeRef[i] = refid(r);
			char cmd[64];
			rt::fmt(cmd, sizeof cmd, "%x.SetScale %.2f", g_smokeRef[i], iniF("smokeScale", kFx_tiresmoke_scale));
			console(cmd);
			LOG("burnout: smoke %x (style %d)", g_smokeRef[i], g_smokeVar + 1);
		}
		else moveRef(r, x, y, z, 0, 0, 0);
	}
}
static void ridersBegin();
static void ridersEnd();
static void startDriving(int idx, Vehicle &v)
{
	g_enterT = rt::now();
	g_car = v;
	g_car.course = g_car.heading; g_car.useCourse = true; g_car.wheelie = g_car.wheelieVel = 0;
	g_rearGrip = 1; g_sliding = false; g_yawRate = 0; g_wheelieAim = 0; g_standLaunchT = 0; g_standArmed = false;
	g_activeCar = idx;
	g_driving = true;
	CRUMB("getting in: drive mode");
	driveMode(true);
	if (!audio::loadEngine(kCars[idx].id) && kCars[idx].model[0] && !audio::loadEngine("mw_300097")) LOG("%s: no engine sound (none of its own, and no Need for Speed truck's here)", kCars[idx].id);
	g_gear = 0; g_rpm = 0;
	g_radioFall = 1; audio::setRadioPan(0);
	CRUMB("getting in: radio");
	if (g_musicMode == 0 && !nfsRadioAvail()) g_musicMode = 2;
	if (g_musicMode == 1) musicOn(false);
	else if (g_musicMode == 0) { if (!g_radioOn) radioNext(); }
	else radioOff();
	click(radio::CLICK_POWER);
	CRUMB("getting in: riders");
	ridersBegin();
}

static void enterCar(int idx, float x, float y, float z, float heading)
{
	CarState &cs = g_cars[idx];
	Vehicle v; memset(&v, 0, sizeof v);
	v.low = kCars[idx].drive;
	v.x = x; v.y = y; v.z = z; v.heading = heading; v.course = heading; v.useCourse = true;
	v.halfTrack = cs.halfTrack; v.halfBase = cs.halfBase; v.halfLength = cs.halfLength;
	v.front = cs.front; v.rear = cs.rear; v.clearance = cs.clearance; v.wheelRadius = cs.wheelRadius;
	v.steerAxis[0] = cs.steerAxis[0]; v.steerAxis[1] = cs.steerAxis[1]; v.steerAxis[2] = cs.steerAxis[2];
	LOG("getting in: %s", kCars[idx].id);
	CRUMB("getting in: ground under the car");
	settle(v, 0, true);
	CRUMB("getting in: placing the car");
	TESObjectREFR *r = placeRef(v.low, v.x, v.y, v.z, v.heading);
	if (!r) { message("Mojave Enhanced: couldn't place the car (is MojaveMostWanted.esp active?)."); return; }
	v.refID = refid(r);
	v.spawnX = v.x; v.spawnY = v.y;
	CRUMB("getting in: moving the car");
	pushVehicle(v);
	CRUMB("getting in: start driving");
	startDriving(idx, v);
	CRUMB("driving");
	char line[128];
	rt::fmt(line, sizeof line, "%s: W/S drive, A/D steer, Space handbrake, mouse looks around, C camera, G get out, %sM %s.",
		kCars[idx].label, g_musicMode == 2 ? "radio on your Pip-Boy, " : "N next song, ", sourceName(nextSource(g_musicMode)));
	message(line, 6);
	LOG("enter car %s at %f %f %f heading %f", kCars[idx].id, v.x, v.y, v.z, v.heading);
}

static void driftFovOff(TESObjectREFR *pl);
static bool g_exitDead;
static void exitCar()
{
	g_holdG = -1;
	if (!g_driving) return;
	g_driving = false;
	driftFovOff(player());
	ridersEnd();
	musicOff(true);
	g_screenDark = false;
	TESObjectREFR *drive = vref(g_car);
	g_car.wheelie = g_car.wheelieVel = 0; g_wheelieAim = 0;
	g_car.shakePitch = g_car.shakeRoll = g_car.shakeZ = g_car.shakeZVel = g_car.shakePitchVel = g_car.shakeAvg = 0;
	TESObjectREFR *parked = placeRef(kCars[g_activeCar].parked, g_car.x, g_car.y, g_car.z, g_car.heading);
	if (parked)
	{
		pushRef(parked, g_car);
		g_parkedRef = refid(parked);
		g_parkedPose = g_car; g_parkedPose.shakePitch = g_parkedPose.shakeRoll = g_parkedPose.shakeZ = 0;
		g_parkedPoseRef = g_parkedRef;
	}
	removeRef(drive);
	if (g_car.oldRef) { removeRef((TESObjectREFR *)lookup(g_car.oldRef)); g_car.oldRef = 0; }
	float h = g_car.heading * rt::PI / 180.0f;
	float px = g_car.x - rt::cos(h) * (g_car.halfTrack + 70), py = g_car.y + rt::sin(h) * (g_car.halfTrack + 70);
	float pz = groundAt(px, py, g_car.z) + 10;
	if (!g_exitDead) moveRef(player(), px, py, pz, 0, 0, g_car.heading);
	driveMode(false);
	audio::setEngine(false, g_rpm, 0, 0);
	audio::setLimiter(false, 13, 0, 0);
	audio::setSkid(0, 1, 0); g_burning = false; g_launchT = 0; g_revsOverride = -1; g_car.slipMs = 0; smokeRemove();
	beamRemove(g_headBeam); beamRemove(g_headNear); beamRemove(g_brakeBeam); beamRemove(g_headFar);
	LOG("exit car at %f %f %f", g_car.x, g_car.y, g_car.z);
}

static u32 g_takenRef;
struct WorldCar { u32 ref; int car; };
static WorldCar g_worldCars[32]; static int g_nWorldCars;
static u32 g_worldScanCell; static double g_worldScanT;
static char lcase(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool sameModel(const char *a, const char *b)
{
	if (!a || !b) return false;
	if ((lcase(a[0]) == 'm' && lcase(a[1]) == 'e' && lcase(a[2]) == 's' && lcase(a[3]) == 'h' && lcase(a[4]) == 'e' && lcase(a[5]) == 's' && a[6] == '\\')) a += 7;
	while (*a && *b && lcase(*a) == lcase(*b)) { a++; b++; }
	return !*a && !*b;
}
// The placed game vehicles near you (looked for again when your cell changes)
static void worldCarsScan(TESObjectREFR *pl)
{
	u32 cell = ptr((u32)pl + OFF_REF_CELL);
	if (cell == g_worldScanCell && rt::now() - g_worldScanT < 10) return;
	g_worldScanCell = cell; g_worldScanT = rt::now(); g_nWorldCars = 0;
	bool any = false;
	for (int i = 0; i < kNumCars && !any; i++) any = kCars[i].model[0] && g_cars[i].available;
	if (!any) return;
	u32 map = ptr(ADDR_FORM_MAP);
	u32 buckets = map ? rd<u32>(map + 4) : 0, table = map ? ptr(map + 8) : 0;
	if (!buckets || !table || buckets > (1u << 24)) return;
	const float *pp = pos(pl);
	for (u32 b = 0; b < buckets && g_nWorldCars < 32; b++)
		for (u32 e = ptr(table + 4 * b); e && g_nWorldCars < 32; e = ptr(e))
		{
			u32 f = ptr(e + 8);
			if (!f || formtype((void *)f) != 0x3A || (rd<u32>(f + 8) & 0x20)) continue;
			u32 base = ptr(f + OFF_REF_BASE);
			if (!base || formtype((void *)base) != 0x20) continue;
			const char *path = (const char *)ptr(base + 0x34);
			if (!path) continue;
			const float *rp = (const float *)(f + OFF_REF_POS);
			float dx = rp[0] - pp[0], dy = rp[1] - pp[1];
			if (dx * dx + dy * dy > 8000.0f * 8000.0f) continue;
			for (int i = 0; i < kNumCars; i++)
				if (kCars[i].model[0] && g_cars[i].available && sameModel(path, kCars[i].model)) { g_worldCars[g_nWorldCars].ref = rd<u32>(f + 0xC); g_worldCars[g_nWorldCars].car = i; g_nWorldCars++; break; }
		}
}
// The nearest placed game vehicle you could take (within reach of its side), or 0
static TESObjectREFR *worldCarNear(TESObjectREFR *pl, int *car, float *d2out)
{
	TESObjectREFR *best = 0; float bd = 1e18f;
	const float *pp = pos(pl);
	for (int i = 0; i < g_nWorldCars; i++)
	{
		TESObjectREFR *r = (TESObjectREFR *)lookup(g_worldCars[i].ref);
		if (!r || (rd<u32>((u32)r + 8) & 0x20) || (rd<u32>((u32)r + 8) & 0x800)) continue;
		const float *rp = pos(r);
		float dx = rp[0] - pp[0], dy = rp[1] - pp[1], d = dx * dx + dy * dy;
		float reach = g_cars[g_worldCars[i].car].halfLength + 140;
		if (d < reach * reach && d < bd && rt::absf(rp[2] - pp[2]) < 250) { bd = d; best = r; *car = g_worldCars[i].car; }
	}
	if (d2out) *d2out = bd;
	return best;
}
// A game vehicle stays hidden in a save only while it's yours there (your car out or being driven, the one taken)
static void worldPutBack(const char *why)
{
	if (!g_takenRef) return;
	TESObjectREFR *o = (TESObjectREFR *)lookup(g_takenRef);
	if (o && s_enable) { g_script->CallFunctionAlt(s_enable, player(), 1, o); LOG("world car: %08x put back where it was found (%s)", g_takenRef, why); }
	g_takenRef = 0;
}
static bool hasEnableParent(u32 ref)
{
	int n = 0;
	for (u32 x = ptr(ref + 0x44 + 4); x && n < 256; x = ptr(x + 8), n++)
		if (rd<u8>(x + 4) == 0x37) return true;
	return false;
}
static void worldCarsRestore()
{
	bool own = g_takenRef && g_activeCar >= 0 && g_activeCar < kNumCars && kCars[g_activeCar].model[0] && (g_parkedRef || g_resume);
	if (g_takenRef && !own) { LOG("world car: %08x was taken in this save but no car of it is out - it goes back", g_takenRef); g_takenRef = 0; }
	if (!s_enable) return;
	u32 map = ptr(ADDR_FORM_MAP);
	u32 buckets = map ? rd<u32>(map + 4) : 0, table = map ? ptr(map + 8) : 0;
	if (!buckets || !table || buckets > (1u << 24)) return;
	int back = 0;
	for (u32 b = 0; b < buckets && back < 32; b++)
		for (u32 e = ptr(table + 4 * b); e && back < 32; e = ptr(e))
		{
			u32 f = ptr(e + 8);
			if (!f || formtype((void *)f) != 0x3A) continue;
			u32 fl = rd<u32>(f + 8);
			if ((fl & 0x20) || !(fl & 0x800)) continue;
			u32 id = rd<u32>(f + 0xC);
			if (own && id == g_takenRef) continue;
			u32 base = ptr(f + OFF_REF_BASE);
			if (!base || formtype((void *)base) != 0x20) continue;
			const char *path = (const char *)ptr(base + 0x34);
			bool ours = false;
			for (int i = 0; i < kNumCars && !ours; i++) ours = kCars[i].model[0] && sameModel(path, kCars[i].model);
			if (!ours || hasEnableParent(f)) continue;
			g_script->CallFunctionAlt(s_enable, player(), 1, (TESObjectREFR *)f);
			LOG("world car: %08x (%s) was left hidden in this save - back where it stood", id, path);
			back++;
		}
	g_worldScanCell = 0;
}
static void enterCar(int idx, float x, float y, float z, float heading);
static void worldTake(TESObjectREFR *w, int idx)
{
	if (!w || !s_disable) return;
	if (g_takenRef != refid(w)) worldPutBack("another one taken");
	if (g_parkedRef) { TESObjectREFR *pk = (TESObjectREFR *)lookup(g_parkedRef); if (pk) removeRef(pk); g_parkedRef = 0; }
	const float *p = pos(w);
	float yaw = rot(w)[2], c = rt::cos(yaw), sn = rt::sin(yaw);
	const CarState &cs = g_cars[idx];
	float x = p[0] + cs.origin[0] * c + cs.origin[1] * sn, y = p[1] - cs.origin[0] * sn + cs.origin[1] * c, z = p[2] + cs.origin[2];
	float heading = yaw * 180.0f / rt::PI + cs.modelYaw;
	while (heading >= 360) heading -= 360;
	while (heading < 0) heading += 360;
	g_script->CallFunctionAlt(s_disable, player(), 1, w);
	g_takenRef = refid(w);
	LOG("world car: took %08x (%s) at %.0f %.0f %.0f facing %.0f deg - the drivable one there facing %.0f deg", g_takenRef, kCars[idx].id, p[0], p[1], p[2], yaw * 180 / rt::PI, heading);
	g_activeCar = idx;
	enterCar(idx, x, y, z, heading);
}

// Parked cars have no Havok collision (it crashed the game), so keep the Courier out of them by hand.
static void keepOut(TESObjectREFR *pl, TESObjectREFR *car, int idx)
{
	if (!pl || !car || idx < 0 || !s_setXY) return;
	float *cp = pos(car), *pp = pos(pl);
	if (pp[2] > cp[2] + 170 || pp[2] < cp[2] - 80) return;
	float h = rot(car)[2], sn = rt::sin(h), cs = rt::cos(h);
	float dx = pp[0] - cp[0], dy = pp[1] - cp[1];
	float lx = dx * cs - dy * sn, ly = dx * sn + dy * cs;
	float ex = g_cars[idx].halfWidth + 20, ey = g_cars[idx].halfLength + 20;
	if (rt::absf(lx) >= ex || rt::absf(ly) >= ey) return;
	if (ex - rt::absf(lx) < ey - rt::absf(ly)) lx = lx < 0 ? -ex : ex; else ly = ly < 0 ? -ey : ey;
	g_script->CallFunctionAlt(s_setXY, player(), 3, pl, F(cp[0] + lx * cs + ly * sn), F(cp[1] - lx * sn + ly * cs));
}

// ---- garage
static void garageXY(int slot, float *x, float *y)
{
	float h = kGarage[3] * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
	*x = kGarage[0] + c * kSlots[slot].right + s * kSlots[slot].forward;
	*y = kGarage[1] - s * kSlots[slot].right + c * kSlots[slot].forward;
}
static float g_bayXY[8][2];
static bool g_baysReady;
struct BayFit { float tilt, rise, dz; bool clear; };
static BayFit bayFit(float x, float y, float headingDeg, bool full)
{
	const float ht = 56, hb = 92, hl = 168, hw = 70;
	float h = headingDeg * rt::PI / 180.0f, sn = rt::sin(h), c = rt::cos(h);
	float zr = kGarage[2];
	BayFit f; f.tilt = 99; f.rise = 99; f.dz = 999; f.clear = false;
	const float wp[4][2] = {{-ht, hb}, {ht, hb}, {-ht, -hb}, {ht, -hb}};
	const float pat[5][2] = {{0, 0}, {0, 22}, {0, -22}, {11, 0}, {-11, 0}};
	float w[4];
	for (int i = 0; i < 4; i++)
	{
		w[i] = -1e9f;
		for (int k = 0; k < 5; k++)
		{
			float rx = wp[i][0] + pat[k][0], fy = wp[i][1] + pat[k][1];
			w[i] = rt::max(w[i], groundAt(x + rx * c + fy * sn, y - rx * sn + fy * c, zr));
		}
	}
	float zc = (w[0] + w[1] + w[2] + w[3]) * 0.25f;
	float tp = ((w[0] + w[1]) - (w[2] + w[3])) * 0.5f / (2 * hb), tr = ((w[1] + w[3]) - (w[0] + w[2])) * 0.5f / (2 * ht);
	f.tilt = rt::max(rt::absf(rt::atan2(tp, 1)), rt::absf(rt::atan2(tr, 1))) * 180.0f / rt::PI;
	f.dz = zc - zr;
	if (!full || f.tilt > 4 || rt::absf(f.dz) > 90) return f;
	f.rise = -99;
	for (int ix = -1; ix <= 1; ix++)
		for (int iy = -2; iy <= 2; iy++)
		{
			float rx = ix * hw * 0.8f, fy = iy * hl * 0.45f;
			float gz = groundAt(x + rx * c + fy * sn, y - rx * sn + fy * c, zr);
			f.rise = rt::max(f.rise, gz - (zc + fy * tp + rx * tr));
		}
	if (f.rise > 10) return f;
	const float ex = hw + 25, ey = hl + 25;
	const float cor[4][2] = {{-ex, ey}, {ex, ey}, {ex, -ey}, {-ex, -ey}};
	const int seg[6][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 2}, {1, 3}};
	f.clear = true;
	for (int k = 0; k < 6 && f.clear; k++)
		for (int up = 60; up <= 120 && f.clear; up += 60)
		{
			const float *A = cor[seg[k][0]], *B = cor[seg[k][1]];
			float from[3] = {x + A[0] * c + A[1] * sn, y - A[0] * sn + A[1] * c, zc + up};
			float to[3] = {x + B[0] * c + B[1] * sn, y - B[0] * sn + B[1] * c, zc + up};
			float fr;
			if (rayHit(from, to, &fr) && fr < 1) f.clear = false;
		}
	return f;
}
static bool bayGood(const BayFit &f) { return f.clear && f.tilt <= 2.5f && f.rise <= 6 && rt::absf(f.dz) <= 60; }
static void bayLocal(float x, float y, float *right, float *fwd)
{
	float h = kGarage[3] * rt::PI / 180.0f, sn = rt::sin(h), c = rt::cos(h), dx = x - kGarage[0], dy = y - kGarage[1];
	*right = dx * c - dy * sn; *fwd = dx * sn + dy * c;
}
static void baysPath(char *out, u32 cap) { rt::fmt(out, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\garage.ini", g_dir); }
static const int kBayLayout = 1;
static bool baysLoad()
{
	char p[320]; baysPath(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (!b) return false;
	int n = 0, ver = 0; float ax = 0, ay = 0;
	for (char *q = b; *q; q++)
	{
		if (q != b && q[-1] != '\n') continue;
		if (rt::startswith(q, "layout=")) ver = rt::parsei(q + 7);
		else if (rt::startswith(q, "anchor="))
		{
			ax = rt::parsef(q + 7);
			char *k = q; while (*k && *k != ',') k++;
			if (*k) ay = rt::parsef(k + 1);
		}
		else if (rt::startswith(q, "bay") && q[3] >= '0' && q[3] <= '7' && q[4] == '=')
		{
			int i = q[3] - '0';
			g_bayXY[i][0] = rt::parsef(q + 5);
			char *k = q + 5; while (*k && *k != ',') k++;
			if (*k) { g_bayXY[i][1] = rt::parsef(k + 1); n++; }
		}
	}
	rt::free(b);
	bool ok = ver == kBayLayout && rt::absf(ax - kGarage[0]) < 1 && rt::absf(ay - kGarage[1]) < 1 && n >= kNumSlots;
	if (ok) LOG("garage: bays from garage.ini");
	return ok;
}
static void baysSave()
{
	char p[320], buf[512]; baysPath(p, sizeof p);
	int n = rt::fmt(buf, sizeof buf, "; Mojave Enhanced garage bays (worked out in game; delete to redo)\r\nlayout=%d\r\nanchor=%.1f,%.1f\r\n", kBayLayout, kGarage[0], kGarage[1]);
	for (int i = 0; i < kNumSlots && i < 8; i++) n += rt::fmt(buf + n, sizeof buf - n, "bay%d=%.1f,%.1f\r\n", i, g_bayXY[i][0], g_bayXY[i][1]);
	rt::writefile(p, buf, (u32)n, false);
}
static void baysLayout()
{
	g_baysReady = true;
	if (baysLoad()) return;
	double t0 = rt::now();
	int ns = kNumSlots < 8 ? kNumSlots : 8;
	bool done[8] = {false};
	BayFit fit[8];
	for (int i = 0; i < ns; i++)
	{
		garageXY(i, &g_bayXY[i][0], &g_bayXY[i][1]);
		if (kSlots[i].keep) { done[i] = true; LOG("garage: bay %d as in the sheet (approved)", i); continue; }
		fit[i] = bayFit(g_bayXY[i][0], g_bayXY[i][1], kSlots[i].heading, true);
		done[i] = bayGood(fit[i]);
		if (done[i]) LOG("garage: bay %d kept (tilt %.1f, rise %.0f, height %.0f)", i, fit[i].tilt, fit[i].rise, fit[i].dz);
		else LOG("garage: bay %d needs moving (tilt %.1f, rise %.0f, height %.0f, clear %d)", i, fit[i].tilt, fit[i].rise, fit[i].dz, fit[i].clear);
	}
	float u = kUnitsPerMetre, maxR = TUNE(bayMoveMetres) * u, step = 0.5f * u;
	for (int i = 0; i < ns; i++)
	{
		if (done[i]) continue;
		float ox, oy; garageXY(i, &ox, &oy);
		bool found = false; int tried = 0;
		for (float r = step; r <= maxR && !found; r += step)
		{
			int na = (int)(2 * rt::PI * r / step) + 1;
			for (int a = 0; a < na && !found; a++)
			{
				float ang = 2 * rt::PI * a / na, x = ox + rt::cos(ang) * r, y = oy + rt::sin(ang) * r;
				float rx, fy; bayLocal(x, y, &rx, &fy);
				bool overlap = false;
				for (int j = 0; j < ns; j++)
				{
					if (j == i || !done[j]) continue;
					float jr, jf; bayLocal(g_bayXY[j][0], g_bayXY[j][1], &jr, &jf);
					if (rt::absf(jr - rx) < 3.4f * u && rt::absf(jf - fy) < 5.4f * u) overlap = true;
				}
				if (overlap) continue;
				tried++;
				BayFit f = bayFit(x, y, kSlots[i].heading, true);
				if (bayGood(f)) { g_bayXY[i][0] = x; g_bayXY[i][1] = y; fit[i] = f; found = true; }
			}
		}
		done[i] = true;
		float d = rt::sqrt((g_bayXY[i][0] - ox) * (g_bayXY[i][0] - ox) + (g_bayXY[i][1] - oy) * (g_bayXY[i][1] - oy)) / u;
		LOG("garage: bay %d %s %.1f m (tilt %.1f, rise %.0f, height %.0f; %d spots tried)", i, found ? "moved" : "no better spot within", found ? d : TUNE(bayMoveMetres),
			fit[i].tilt, fit[i].rise, fit[i].dz, tried);
	}
	baysSave();
	LOG("garage: bays worked out in %.2f s", (float)(rt::now() - t0));
}
static void bayXY(int slot, float *x, float *y)
{
	if (!g_baysReady) baysLayout();
	*x = g_bayXY[slot][0]; *y = g_bayXY[slot][1];
}

// Bays: each shows one car (g_slotCar).
static void fillSlots()
{
	for (int s = 0; s < kNumSlots && s < 8; s++)
	{
		if (g_slotCar[s] >= 0 && g_slotCar[s] < kNumCars && g_cars[g_slotCar[s]].available && !kCars[g_slotCar[s]].model[0]) continue;
		g_slotCar[s] = -1;
		for (int i = 0; i < kNumCars; i++)
		{
			if (!g_cars[i].available || kCars[i].model[0]) continue;
			bool used = false;
			for (int t = 0; t < kNumSlots && t < 8; t++) used = used || g_slotCar[t] == i;
			if (!used) { g_slotCar[s] = i; break; }
		}
	}
}

static void ensureDisplay()
{
	fillSlots();
	for (int s = 0; s < kNumSlots && s < 8; s++)
	{
		int i = g_slotCar[s];
		if (i < 0 || !g_cars[i].available)
		{
			if (g_display[s]) { TESObjectREFR *old = (TESObjectREFR *)lookup(g_display[s]); if (old) removeRef(old); LOG("garage: bay %d's display (%x) taken away - its car isn't here now", s, g_display[s]); g_display[s] = 0; }
			continue;
		}
		if (i == g_activeCar) continue;
		float x, y;
		bayXY(s, &x, &y);
		if (g_display[s])
		{
			TESObjectREFR *old = (TESObjectREFR *)lookup(g_display[s]);
			float *op = old ? pos(old) : 0;
			if (op && (op[0] - x) * (op[0] - x) + (op[1] - y) * (op[1] - y) < 80.0f * 80.0f) continue;
			if (old) removeRef(old);
			g_display[s] = 0;
		}
		Vehicle v; memset(&v, 0, sizeof v);
		v.x = x; v.y = y; v.z = kGarage[2]; v.heading = kSlots[s].heading;
		v.halfTrack = g_cars[i].halfTrack; v.halfBase = g_cars[i].halfBase; v.halfLength = g_cars[i].halfLength;
		v.front = g_cars[i].front; v.rear = g_cars[i].rear; v.clearance = g_cars[i].clearance;
		settle(v, 0, true, 3);
		TESObjectREFR *r = placeRef(kCars[i].parked, v.x, v.y, v.z, v.heading);
		if (r) { pushRef(r, v); g_display[s] = refid(r); }
		LOG("garage: bay %d shows %s (%x)", s, kCars[i].id, g_display[s]);
	}
}

// ---- paint shop
struct Paint { i32 rgb; u8 finish; };
static Paint g_paint[kNumCars];
static u32 g_paintVer[kNumCars];
static bool g_paintLoaded;
static void paintIni(char *out, u32 cap) { rt::fmt(out, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\paint.ini", g_dir); }
static void hex6(char *out, u32 rgb)
{
	static const char *d = "0123456789ABCDEF";
	for (int i = 0; i < 6; i++) out[i] = d[(rgb >> (20 - 4 * i)) & 15];
	out[6] = 0;
}
static void paintLoad()
{
	g_paintLoaded = true;
	for (int i = 0; i < kNumCars; i++) { g_paint[i].rgb = -1; g_paint[i].finish = 0; }
	char p[320];
	paintIni(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (!b) return;
	int n = 0;
	for (char *q = b; *q;)
	{
		char *e = q;
		while (*e && *e != '\n' && *e != '\r') e++;
		char keep = *e; *e = 0;
		char *eq = q;
		while (*eq && *eq != '=') eq++;
		if (*q != ';' && *eq == '=')
		{
			*eq = 0;
			for (int i = 0; i < kNumCars; i++)
				if (rt::streq(q, kCars[i].id))
				{
					const char *v = eq + 1;
					g_paint[i].rgb = rt::startswith(v, "factory") ? -1 : (i32)(rt::parsehex(v) & 0xFFFFFF);
					while (*v && *v != ',') v++;
					int f = *v == ',' ? rt::parsei(v + 1) : 0;
					g_paint[i].finish = (u8)(f >= 0 && f < kNumFinishes ? f : 0);
					n++;
				}
		}
		*e = keep;
		q = *e ? e + 1 : e;
	}
	rt::free(b);
	LOG("paint: %d cars painted in paint.ini", n);
}
static void paintSave()
{
	char p[320];
	paintIni(p, sizeof p);
	u32 cap = 96 + kNumCars * 48, n = 0;
	char *out = (char *)rt::alloc(cap);
	if (!out) return;
	n += rt::fmt(out + n, cap - n, "; Mojave Enhanced paint shop: car=RRGGBB (or factory),finish\r\n");
	for (int i = 0; i < kNumCars; i++)
	{
		if (g_paint[i].rgb < 0 && g_paint[i].finish == 0) continue;
		char h[8];
		if (g_paint[i].rgb >= 0) hex6(h, (u32)g_paint[i].rgb); else rt::strcpy(h, "factory", sizeof h);
		n += rt::fmt(out + n, cap - n, "%s=%s,%d\r\n", kCars[i].id, h, g_paint[i].finish);
	}
	rt::writefile(p, out, n, false);
	rt::free(out);
}
static u32 paintColor(int car) { return g_paint[car].rgb >= 0 ? (u32)g_paint[car].rgb : kCars[car].paint; }

// colours: hue in degrees, saturation and value 0..1
static void rgbToHsv(u32 rgb, float *h, float *s, float *v)
{
	float r = ((rgb >> 16) & 255) / 255.0f, g = ((rgb >> 8) & 255) / 255.0f, b = (rgb & 255) / 255.0f;
	float mx = rt::max(r, rt::max(g, b)), mn = rt::min(r, rt::min(g, b)), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if (d <= 0) { *h = 0; return; }
	float hh = mx == r ? (g - b) / d : mx == g ? 2 + (b - r) / d : 4 + (r - g) / d;
	hh *= 60;
	if (hh < 0) hh += 360;
	*h = hh;
}
static u32 hsvToRgb(float h, float s, float v)
{
	while (h < 0) h += 360;
	while (h >= 360) h -= 360;
	s = rt::clamp(s, 0, 1); v = rt::clamp(v, 0, 1);
	float c = v * s, hp = h / 60, x = c * (1 - rt::absf((hp - 2 * (int)(hp / 2)) - 1)), m = v - c;
	float r = 0, g = 0, b = 0;
	switch ((int)hp)
	{
	case 0: r = c; g = x; break;
	case 1: r = x; g = c; break;
	case 2: g = c; b = x; break;
	case 3: g = x; b = c; break;
	case 4: r = x; b = c; break;
	default: r = c; b = x; break;
	}
	u32 R = (u32)((r + m) * 255 + 0.5f), G = (u32)((g + m) * 255 + 0.5f), B = (u32)((b + m) * 255 + 0.5f);
	return (R << 16) | (G << 8) | B;
}

// A small one-colour DDS (8x8, 4 mips, 32-bit BGRA: the converter's dds.Solid), written once per colour
static bool solidTexture(const char *rel, u8 r, u8 g, u8 b, u8 a)
{
	char full[400];
	rt::fmt(full, sizeof full, "%sData\\%s", g_dir, rel);
	if (GetFileAttributesA(full) != 0xFFFFFFFFu) return true;
	char dir[400];
	rt::fmt(dir, sizeof dir, "%sData\\textures\\mmw", g_dir); CreateDirectoryA(dir, 0);
	rt::fmt(dir, sizeof dir, "%sData\\textures\\mmw\\paint", g_dir); CreateDirectoryA(dir, 0);
	static u8 buf[128 + 85 * 4];
	memset(buf, 0, 128);
	u32 *h = (u32 *)buf;
	h[0] = 0x20534444; h[1] = 124; h[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000; h[3] = 8; h[4] = 8; h[5] = 32; h[7] = 4;
	h[19] = 32; h[20] = 0x41; h[22] = 32; h[23] = 0x00FF0000; h[24] = 0x0000FF00; h[25] = 0x000000FF; h[26] = 0xFF000000;
	h[27] = 0x1000 | 0x8 | 0x400000;
	for (int i = 0; i < 85; i++) { u8 *q = buf + 128 + 4 * i; q[0] = b; q[1] = g; q[2] = r; q[3] = a; }
	bool ok = rt::writefile(full, (const char *)buf, sizeof buf, false);
	if (!ok) LOG("paint: couldn't write %s", full);
	return ok;
}
typedef void(__thiscall *CreateTextureImageFn)(void *tes, const char *path, u32 *out, bool mustFindFile, bool archiveOnly);
static u32 loadTexture(const char *rel)
{
	u32 tex = 0;
	((CreateTextureImageFn)ADDR_CREATE_TEXTURE_IMAGE)((void *)ptr(ADDR_TES), rel, &tex, false, false);
	return tex;
}
static void niRelease(u32 o)
{
	if (o && __atomic_sub_fetch((volatile i32 *)(o + 4), 1, __ATOMIC_SEQ_CST) <= 0)
		((void(__thiscall *)(u32))ptr(ptr(o) + 4))(o);
}
static bool swapTexture(u32 shader, int slot, u32 tex)
{
	u32 arr = ptr(shader + OFF_PPL_TEXTURES + 4 * slot);
	if (!arr || !tex) return false;
	u32 old = ptr(arr);
	if (old == tex) return true;
	__atomic_add_fetch((volatile i32 *)(tex + 4), 1, __ATOMIC_SEQ_CST);
	*(u32 *)arr = tex;
	niRelease(old);
	return true;
}
struct PaintJob { u32 diffuse, mask; float spec[3], gloss; int shapes, swapped; u32 badType; };
static void paintNode(u32 node, PaintJob &j, int depth)
{
	u32 arr = ptr(node + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(node + OFF_NODE_CHILDCOUNT);
	if (!arr || cnt > 512) return;
	for (u16 i = 0; i < cnt; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm) continue;
		if (!memcmp(nm, "MMWPaint", 8))
		{
			u32 sh = ptr(c + OFF_GEOM_SHADER), mat = ptr(c + OFF_GEOM_MATERIAL);
			j.shapes++;
			if (!sh) continue;
			u32 type = rd<u32>(sh + OFF_SHADER_TYPE);
			if (type != SHADER_PPLIGHTING) { j.badType = type; continue; }
			bool ok = swapTexture(sh, 0, j.diffuse);
			swapTexture(sh, 5, j.mask);
			if (mat)
			{
				float *sp = (float *)(mat + OFF_MAT_SPECULAR);
				sp[0] = j.spec[0]; sp[1] = j.spec[1]; sp[2] = j.spec[2];
				*(float *)(mat + OFF_MAT_GLOSS) = j.gloss;
			}
			if (ok) j.swapped++;
		}
		else if (depth < 4 && (!memcmp(nm, "MMWSteer", 8) || !memcmp(nm, "MMWWheel", 8) || rt::streq(nm, "MMWRim") || !memcmp(nm, "MMWSusp", 7)))
			paintNode(c, j, depth + 1);
	}
}
static void applyPaint(u32 root, int car)
{
	const Finish &f = kFinishes[g_paint[car].finish < kNumFinishes ? g_paint[car].finish : 0];
	u32 base = paintColor(car);
	float h, sat, val;
	rgbToHsv(base, &h, &sat, &val);
	float v = val * f.shade, knee = TUNE(paintBrightKnee);
	if (v > knee) v = knee + (v - knee) * TUNE(paintBrightSqueeze);
	u32 diff = hsvToRgb(h, sat, v);
	float lum = (0.2126f * ((base >> 16) & 255) + 0.7152f * ((base >> 8) & 255) + 0.0722f * (base & 255)) / 255.0f;
	float bright = rt::clamp((lum - 0.55f) / 0.4f, 0, 1);
	bright = bright * bright * (3 - 2 * bright);
	float th = h + f.specHueShift, ts = sat;
	if (f.specHueShift != 0 && sat < 0.15f) { th = 200; ts = 0.3f; }
	u32 tint = hsvToRgb(th, rt::max(ts, f.specTint > 0 ? 0.25f : 0), 1);
	PaintJob j; memset(&j, 0, sizeof j);
	for (int k = 0; k < 3; k++)
	{
		float tc = ((tint >> (16 - 8 * k)) & 255) / 255.0f;
		j.spec[k] = f.specular * ((1 - f.specTint) + f.specTint * tc);
	}
	j.gloss = f.gloss;
	u8 lvl = (u8)(rt::clamp(f.reflection * (1 - TUNE(paintBrightReflectionCut) * bright), 0, 1) * 255 + 0.5f);
	char cHex[8], cRel[96], mRel[96];
	hex6(cHex, diff);
	rt::fmt(cRel, sizeof cRel, "textures\\mmw\\paint\\c%s.dds", cHex);
	static const char *d = "0123456789ABCDEF";
	char lv[3] = {d[lvl >> 4], d[lvl & 15], 0};
	rt::fmt(mRel, sizeof mRel, "textures\\mmw\\paint\\m%s.dds", lv);
	if (solidTexture(cRel, (u8)(diff >> 16), (u8)(diff >> 8), (u8)diff, 255)) j.diffuse = loadTexture(cRel);
	if (solidTexture(mRel, lvl, lvl, lvl, lvl)) j.mask = loadTexture(mRel);
	paintNode(root, j, 0);
	LOG("paint: %s %s %s: %d paint parts, %d repainted%s (textures %x %x)", kCars[car].id, cHex, f.label, j.shapes, j.swapped,
		j.shapes ? "" : " - no MMWPaint parts: run the launcher so the cars are converted again", j.diffuse, j.mask);
	if (j.badType) LOG("paint: unexpected shader type %u", j.badType);
	niRelease(j.diffuse);
	niRelease(j.mask);
}
// ---- Wheel shop: other cars' wheels
static i32 g_rim[kNumCars];
static u32 g_rimVer[kNumCars];
static bool g_rimLoaded;
static void rimIni(char *out, u32 cap) { rt::fmt(out, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\wheels.ini", g_dir); }
static void rimLoad()
{
	g_rimLoaded = true;
	for (int i = 0; i < kNumCars; i++) g_rim[i] = -1;
	char p[320];
	rimIni(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (!b) return;
	for (char *q = b; *q;)
	{
		char *e = q;
		while (*e && *e != '\n' && *e != '\r') e++;
		char keep = *e; *e = 0;
		char *eq = q;
		while (*eq && *eq != '=') eq++;
		if (*q != ';' && *eq == '=')
		{
			*eq = 0;
			int car = -1, donor = -1;
			for (int i = 0; i < kNumCars; i++)
			{
				if (rt::streq(q, kCars[i].id)) car = i;
				if (rt::streq(eq + 1, kCars[i].id)) donor = i;
			}
			if (car >= 0) g_rim[car] = donor;
		}
		*e = keep;
		q = *e ? e + 1 : e;
	}
	rt::free(b);
}
static void rimSave()
{
	char p[320];
	rimIni(p, sizeof p);
	u32 cap = 96 + kNumCars * 48, n = 0;
	char *out = (char *)rt::alloc(cap);
	if (!out) return;
	n += rt::fmt(out + n, cap - n, "; Mojave Enhanced wheel shop: car=the car whose wheels it wears\r\n");
	for (int i = 0; i < kNumCars; i++)
		if (g_rim[i] >= 0) n += rt::fmt(out + n, cap - n, "%s=%s\r\n", kCars[i].id, kCars[g_rim[i]].id);
	rt::writefile(p, out, n, false);
	rt::free(out);
}
static inline void **vtbl(u32 o) { return *(void ***)o; }
// A private copy of a model (relative to Data\Meshes), 0 if it can't be loaded
static u32 loadModelCopy(const char *path)
{
	u32 ml = *(u32 *)0x11C3B3C;
	if (!ml) return 0;
	u32 src = ((u32(__thiscall *)(u32, const char *, u32, u32, u32, u32, u32))0x447080)(ml, path, 0, 1, 0, 0, 0);
	if (!src) return 0;
	u32 info[16];
	memset(info, 0, sizeof info);
	((void(__thiscall *)(void *, u32))0x4AD0C0)(info, 0x97);
	float one = 1.0f;
	memcpy(&info[4], &one, 4); memcpy(&info[5], &one, 4); memcpy(&info[6], &one, 4);
	u32 copy = ((u32(__thiscall *)(u32, void *))vtbl(src)[0x48 / 4])(src, info);
	if (copy) ((void(__thiscall *)(u32, void *))vtbl(src)[0x68 / 4])(src, info);
	((void(__thiscall *)(void *))0x4AD1D0)(info);
	if (!copy) return 0;
	*(u8 *)(copy + OFF_NIAV_FLAGS) &= 0xFE;
	return copy;
}
static void niAddChild(u32 parent, u32 child) { ((void(__thiscall *)(u32, u32, u32))vtbl(parent)[0xDC / 4])(parent, child, 1); }
static void niRemoveChild(u32 parent, u32 child) { ((void(__thiscall *)(u32, u32))vtbl(parent)[0xE8 / 4])(parent, child); }
// Puts car `car`'s chosen wheels on a 3D root (its own when it has none chosen)
static void applyRims(u32 root, int car)
{
	int donor = car >= 0 && car < kNumCars ? g_rim[car] : -1;
	if (donor >= 0 && !g_cars[donor].available) donor = -1;
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 256) return;
	int fitted = 0;
	for (u16 i = 0; i < n; i++)
	{
		u32 st = ptr(arr + 4 * i);
		const char *nm = st ? (const char *)ptr(st + 8) : 0;
		if (!nm || memcmp(nm, "MMWSteer", 8) || rt::streq(nm, "MMWCockpitWheel")) continue;
		bool front = nm[8] == 'F', right = nm[9] == 'R';
		u32 sa = ptr(st + OFF_NODE_CHILDREN);
		u32 spin = sa && rd<u16>(st + OFF_NODE_CHILDCOUNT) ? ptr(sa) : 0;
		if (!spin) continue;
		u32 kids = ptr(spin + OFF_NODE_CHILDREN);
		u16 kn = rd<u16>(spin + OFF_NODE_CHILDCOUNT);
		if (!kids || kn > 64) continue;
		u32 old[8]; int no = 0;
		for (u16 k = 0; k < kn; k++)
		{
			u32 c = ptr(kids + 4 * k);
			const char *cn = c ? (const char *)ptr(c + 8) : 0;
			if (cn && rt::streq(cn, "MMWRim") && no < 8) old[no++] = c;
		}
		for (int k = 0; k < no; k++) niRemoveChild(spin, old[k]);
		u32 rim = 0;
		if (donor >= 0)
		{
			char path[96];
			rt::fmt(path, sizeof path, "mmw\\wheels\\%s_%s.nif", kCars[donor].id, front ? "f" : "r");
			rim = loadModelCopy(path);
			if (!rim) LOG("wheels: couldn't load %s", path);
		}
		kids = ptr(spin + OFF_NODE_CHILDREN);
		kn = rd<u16>(spin + OFF_NODE_CHILDCOUNT);
		for (u16 k = 0; kids && k < kn; k++)
		{
			u32 c = ptr(kids + 4 * k);
			if (!c) continue;
			u32 &f = *(u32 *)(c + OFF_NIAV_FLAGS);
			f = rim ? (f | 1u) : (f & ~1u);
		}
		if (rim)
		{
			float mine = front ? g_cars[car].wheelRadiusF : g_cars[car].wheelRadiusR;
			float theirs = front ? g_cars[donor].wheelRadiusF : g_cars[donor].wheelRadiusR;
			float *m = (float *)(rim + 0x34);
			float sgn = right ? -1.0f : 1.0f;
			m[0] = sgn; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = sgn; m[5] = 0; m[6] = 0; m[7] = 0; m[8] = 1;
			float *t = (float *)(rim + 0x58);
			t[0] = t[1] = t[2] = 0;
			*(float *)(rim + 0x64) = theirs > 0.05f && mine > 0.05f ? rt::clamp(mine / theirs, 0.6f, 1.6f) : 1;
			niAddChild(spin, rim);
			fitted++;
			static u32 upd[3];
			((void(__thiscall *)(u32, const void *))vtbl(spin)[0xC0 / 4])(spin, upd);
		}
		updateNode(spin);
	}
	if (donor >= 0) LOG("wheels: %s wears %s wheels (%d fitted)", kCars[car].id, kCars[donor].id, fitted);
	if (fitted && (g_paint[car].rgb >= 0 || g_paint[car].finish || g_paintVer[car])) applyPaint(root, car);
}
struct RimTrack { u32 ref, root, ver; int car; };
static RimTrack g_rtrack[11];
static void rimTrack(int k, u32 refID, int car)
{
	RimTrack &t = g_rtrack[k];
	TESObjectREFR *r = refID && car >= 0 && car < kNumCars ? (TESObjectREFR *)lookup(refID) : 0;
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) { t.root = 0; return; }
	u32 ver = g_rimVer[car] + 1;
	if (t.ref == refID && t.root == root && t.ver == ver && t.car == car) return;
	t.ref = refID; t.root = root; t.ver = ver; t.car = car;
	if (g_rim[car] < 0 && g_rimVer[car] == 0) return;
	applyRims(root, car);
}

// The dash Pip-Boy
static u32 g_pipTex;
// Also "MMWProp r00.. r22 scale <model>": any of the game's own models (its path under meshes\, from the player's
// archives) at that place - the game vehicles' dash radio, the speaker the Pip-Boy plays through
static void attachPipBoy(u32 root)
{
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	if (!arr || n > 256) return;
	for (u16 i = 0; i < n; i++)
	{
		u32 mnt = ptr(arr + 4 * i);
		const char *nm = mnt ? (const char *)ptr(mnt + 8) : 0;
		bool pip = nm && !memcmp(nm, "MMWPipBoy ", 10), prop = nm && !memcmp(nm, "MMWProp ", 8);
		if (!pip && !prop) continue;
		if (rd<u16>(mnt + OFF_NODE_CHILDCOUNT) > 0) continue;
		float f[10];
		const char *rest = suspF(nm + (pip ? 10 : 8), f, 10);
		if (!rest) continue;
		while (*rest == ' ') rest++;
		const char *model = pip ? "AnimObjects\\AOPipBoy.nif" : rest;
		if (!*model) continue;
		float *m = (float *)(mnt + 0x34);
		for (int k = 0; k < 9; k++) m[k] = f[k];
		*(float *)(mnt + 0x64) = rt::clamp(f[9], 0.02f, 4);
		u32 pb = loadModelCopy(model);
		if (!pb)
		{
			static int warned;
			if (warned < 4) { warned++; LOG("dash: couldn't load %s from the game's archives", model); }
			continue;
		}
		niAddChild(mnt, pb);
		static u32 upd[3];
		((void(__thiscall *)(u32, const void *))vtbl(mnt)[0xC0 / 4])(mnt, upd);
		if (prop) { updateNode(mnt); LOG("dash: %s on (scale %.2f)", model, f[9]); continue; }
		u32 glare = findNamed(pb, "glare", 4);
		u32 sh = glare ? ptr(glare + OFF_GEOM_SHADER) : 0;
		bool scr = false;
		if (sh && rd<u32>(sh + OFF_SHADER_TYPE) == SHADER_PPLIGHTING)
		{
			if (!g_pipTex) g_pipTex = loadTexture("textures\\mmw\\screen_pip.dds");
			if (g_pipTex) { swapTexture(sh, 0, g_pipTex); swapTexture(sh, 2, g_pipTex); scr = true; }
		}
		updateNode(mnt);
		LOG("pip-boy: on the dash (scale %.2f)%s", f[9], scr ? ", its screen ready for the equalizer" : " - its screen wasn't found");
	}
}
static u32 g_pipRoot[11];
static void pipTrack(int k, u32 refID)
{
	TESObjectREFR *r = refID ? (TESObjectREFR *)lookup(refID) : 0;
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) { g_pipRoot[k] = 0; return; }
	if (g_pipRoot[k] == root) return;
	g_pipRoot[k] = root;
	attachPipBoy(root);
}

// The cars on show, the one you parked and the one you drive get their paint whenever their 3D appears (a new
// reference, a cell coming back into view) or their paint changes. the showroom car
static u32 g_previewRef;
static int g_previewCar = -1;
struct PaintTrack { u32 ref, root, ver; int car; };
static PaintTrack g_ptrack[11];
static void paintTrack(int k, u32 refID, int car)
{
	PaintTrack &t = g_ptrack[k];
	TESObjectREFR *r = refID && car >= 0 && car < kNumCars ? (TESObjectREFR *)lookup(refID) : 0;
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) { t.root = 0; return; }
	u32 ver = g_paintVer[car] + 1;
	if (t.ref == refID && t.root == root && t.ver == ver && t.car == car) return;
	t.ref = refID; t.root = root; t.ver = ver; t.car = car;
	u32 fc = paintColor(car);
	bool light = (0.2126f * ((fc >> 16) & 255) + 0.7152f * ((fc >> 8) & 255) + 0.0722f * (fc & 255)) / 255.0f > 0.55f;
	if (g_paint[car].rgb < 0 && g_paint[car].finish == 0 && g_paintVer[car] == 0 && !light) return;
	applyPaint(root, car);
}
static void paintTick()
{
	if (!g_paintLoaded) paintLoad();
	if (!g_rimLoaded) rimLoad();
	for (int s = 0; s < kNumSlots && s < 8; s++) { paintTrack(s, g_display[s], g_slotCar[s]); rimTrack(s, g_display[s], g_slotCar[s]); }
	paintTrack(8, g_parkedRef, g_activeCar); rimTrack(8, g_parkedRef, g_activeCar);
	paintTrack(9, g_driving ? g_car.refID : 0, g_activeCar); rimTrack(9, g_driving ? g_car.refID : 0, g_activeCar);
	paintTrack(10, g_previewRef, g_previewCar); rimTrack(10, g_previewRef, g_previewCar);
	for (int s = 0; s < kNumSlots && s < 8; s++) pipTrack(s, g_display[s]);
	pipTrack(8, g_parkedRef); pipTrack(9, g_driving ? g_car.refID : 0); pipTrack(10, g_previewRef);
}
static void paintChanged(int car)
{
	g_paintVer[car]++;
	paintSave();
	paintTick();
}

// ---- despawning
namespace snowfx { static u32 refs[8], rainRefs[4]; }
static u32 g_ghostCars[8];
static u32 g_mzRefs[8];
static void strayCleanup(const char *why)
{
	u32 keep[48]; int nk = 0;
	for (int i = 0; i < 8; i++) keep[nk++] = g_ghostCars[i];
	for (int i = 0; i < 8; i++) keep[nk++] = g_display[i];
	keep[nk++] = g_parkedRef; keep[nk++] = g_driving ? g_car.refID : 0; keep[nk++] = g_previewRef;
	keep[nk++] = g_headBeam.ref; keep[nk++] = g_headBeam.old; keep[nk++] = g_brakeBeam.ref; keep[nk++] = g_brakeBeam.old;
	keep[nk++] = g_headNear.ref; keep[nk++] = g_headNear.old; keep[nk++] = g_headFar.ref; keep[nk++] = g_headFar.old;
	keep[nk++] = g_smokeRef[0]; keep[nk++] = g_smokeRef[1];
	for (int i = 0; i < 8; i++) keep[nk++] = snowfx::refs[i];
	for (int i = 0; i < 4; i++) keep[nk++] = snowfx::rainRefs[i];
	for (int i = 0; i < 8; i++) keep[nk++] = g_mzRefs[i];
	u32 map = ptr(ADDR_FORM_MAP);
	if (!map || g_mod > 0xFE) return;
	u32 buckets = rd<u32>(map + 4), table = ptr(map + 8);
	if (!buckets || !table || buckets > (1u << 24)) return;
	TESObjectREFR *gone[64]; int ng = 0;
	u32 seen = 0;
	for (u32 b = 0; b < buckets && ng < 64; b++)
		for (u32 e = ptr(table + 4 * b); e && ng < 64; e = ptr(e))
		{
			seen++;
			u32 id = rd<u32>(e + 4), f = ptr(e + 8);
			if ((id >> 24) != 0xFF || !f || formtype((void *)f) != 0x3A) continue;
			if (rd<u32>(f + 8) & 0x20) continue;
			u32 base = ptr(f + OFF_REF_BASE);
			if (!base || (refid((void *)base) >> 24) != g_mod) continue;
			bool used = false;
			for (int k = 0; k < nk; k++) used = used || keep[k] == id;
			if (!used) gone[ng++] = (TESObjectREFR *)f;
		}
	for (int i = 0; i < ng; i++) removeRef(gone[i]);
	if (ng) LOG("cleanup (%s): removed %d unused car references (%u forms checked)", why, ng, seen);
}
static void sendCarHome()
{
	if (!g_parkedRef || g_activeCar < 0) { message("You have no car out.", 2); return; }
	TESObjectREFR *r = (TESObjectREFR *)lookup(g_parkedRef);
	if (r) removeRef(r);
	char m[120];
	rt::fmt(m, sizeof m, "Your %s went back to the garage.", kCars[g_activeCar].label);
	message(m, 3);
	LOG("sent %s home (ref %x %s)", kCars[g_activeCar].id, g_parkedRef, r ? "removed" : "not loaded");
	worldPutBack("your car sent home");
	g_parkedRef = 0;
	g_activeCar = -1;
	radioOff(); g_radioFall = 1; audio::setRadioPan(0);
}

// ---- garage menus
static char g_btnText[10][48];
enum { PICK_DRIVE, PICK_PAINT, PICK_WHEELS, PICK_DONOR };
static int g_pick;
static bool g_paintPick;
static int g_paintCar = -1, g_colorPage, g_wheelCar = -1;
static int availableIn(int cls)
{
	int n = 0;
	for (int i = 0; i < kNumCars; i++) if (g_cars[i].available && kCars[i].cls == cls) n++;
	return n;
}
static void showHome()
{
	const char *btn[5]; int n = 0;
	g_menuMap[n] = 0; btn[n++] = "Pick a car";
	g_menuMap[n] = 1; btn[n++] = "Paint shop";
	g_menuMap[n] = 3; btn[n++] = "Wheel shop";
	if (g_parkedRef && g_activeCar >= 0)
	{
		rt::fmt(g_btnText[0], sizeof g_btnText[0], "Send my %s home", kCars[g_activeCar].label);
		g_menuMap[n] = 2; btn[n++] = g_btnText[0];
	}
	g_menuMap[n] = -1; btn[n++] = "Close";
	if (showMessageBox("MOJAVE MOST WANTED GARAGE", btn, n)) g_menu = 3;
}
static void showClasses()
{
	const char *btn[10]; int n = 0;
	for (int k = 0; k < kNumClasses && n < 9; k++)
	{
		int c = availableIn(k);
		if (!c) continue;
		rt::fmt(g_btnText[n], sizeof g_btnText[n], "%s (%d)", kClasses[k].label, c);
		g_menuMap[n] = k; btn[n] = g_btnText[n]; n++;
	}
	if (!n) { message("Mojave Enhanced: no cars were converted. Check the launcher log.", 6); return; }
	g_menuMap[n] = -1; btn[n++] = "Back";
	static const char *titles[4] = {"MOJAVE MOST WANTED GARAGE\nPick a class:", "PAINT SHOP\nWhich car? Pick a class:",
		"WHEEL SHOP\nWhich car gets new wheels? Pick a class:", "WHEEL SHOP\nWhose wheels? Pick a class:"};
	if (showMessageBox(titles[g_pick], btn, n)) g_menu = 1;
}
static void showCars()
{
	int list[kNumCars], total = 0;
	for (int i = 0; i < kNumCars; i++) if (g_cars[i].available && kCars[i].cls == g_menuClass) list[total++] = i;
	for (int a = 1; a < total; a++)
		for (int b = a; b > 0 && rt::strcmp(kCars[list[b]].label, kCars[list[b - 1]].label) < 0; b--)
		{ int t = list[b]; list[b] = list[b - 1]; list[b - 1] = t; }
	int per = total <= 9 ? 9 : 8, pages = (total + per - 1) / per;
	if (g_menuPage >= pages) g_menuPage = 0;
	const char *btn[10]; int n = 0;
	for (int j = g_menuPage * per; j < total && n < per; j++)
	{
		g_menuMap[n] = list[j];
		bool cur = g_pick == PICK_DONOR && g_wheelCar >= 0 && (g_rim[g_wheelCar] < 0 ? list[j] == g_wheelCar : list[j] == g_rim[g_wheelCar]);
		rt::fmt(g_btnText[n], sizeof g_btnText[n], "%s%s", cur ? "> " : "", kCars[list[j]].label);
		btn[n] = g_btnText[n]; n++;
	}
	if (pages > 1) { g_menuMap[n] = -3; btn[n++] = "More..."; }
	g_menuMap[n] = -2; btn[n++] = "Back";
	char title[96];
	const char *head = g_pick == PICK_PAINT ? "PAINT SHOP" : g_pick >= PICK_WHEELS ? "WHEEL SHOP" : "MOJAVE MOST WANTED GARAGE";
	if (pages > 1) rt::fmt(title, sizeof title, "%s\n%s, page %d of %d:", head, kClasses[g_menuClass].label, g_menuPage + 1, pages);
	else rt::fmt(title, sizeof title, "%s\n%s:", head, kClasses[g_menuClass].label);
	if (showMessageBox(title, btn, n)) g_menu = 2;
}

static void openGarage()
{
	if (!g_mwFound)
	{
		message("Mojave Enhanced: Need for Speed Most Wanted isn't installed, so the garage is empty.", 6);
		return;
	}
	showHome();
}

static void takeCar(int idx)
{
	CRUMB("garage: taking a car (old car and bay cleared)");
	if (g_parkedRef) { removeRef((TESObjectREFR *)lookup(g_parkedRef)); g_parkedRef = 0; }
	worldPutBack("a car from the garage");
	int slot = -1;
	for (int s = 0; s < kNumSlots && s < 8; s++) if (g_slotCar[s] == idx) slot = s;
	if (slot < 0)
	{
		slot = 0;
		for (int s = 1; s < kNumSlots && s < 8; s++) if (g_slotAge[s] < g_slotAge[slot]) slot = s;
		g_slotCar[slot] = idx;
	}
	g_slotAge[slot] = ++g_slotClock;
	if (g_display[slot]) { removeRef((TESObjectREFR *)lookup(g_display[slot])); g_display[slot] = 0; }
	float x, y;
	bayXY(slot, &x, &y);
	enterCar(idx, x, y, kGarage[2], kSlots[slot].heading);
}

// ---- Paint shop showroom
static u32 g_previewRoot;
static float g_previewM0[9], g_previewSpin;
static bool g_liveEdit, g_liveRelease;
static u32 g_loadedPreview;
static Paint g_liveUndo;
static double g_liveApplyT, g_liveNoteT;
static bool g_liveDirty, g_liveNoteDirty;
static double g_reopenPaintAt;
static float g_hsv[3];
static void previewRemove()
{
	if (g_previewRef) removeRef((TESObjectREFR *)lookup(g_previewRef));
	g_previewRef = 0; g_previewCar = -1; g_previewRoot = 0;
	g_ptrack[10].root = 0; g_rtrack[10].root = 0;
}
static void previewPlace(int idx, bool live)
{
	previewRemove();
	TESObjectREFR *pl = player();
	if (!pl || idx < 0 || idx >= kNumCars) return;
	float *pp = pos(pl);
	float h = rot(pl)[2], sn = rt::sin(h), c = rt::cos(h);
	float u = kUnitsPerMetre;
	float dist = (live ? TUNE(showroomLiveDistance) : TUNE(showroomDistance)) * u, side = live ? 0 : TUNE(showroomSide) * u;
	float dx = sn * dist + c * side, dy = c * dist - sn * side;
	float len = rt::sqrt(dx * dx + dy * dy), fr = 1;
	float from[3] = {pp[0], pp[1], pp[2] + 100}, to[3] = {pp[0] + dx, pp[1] + dy, pp[2] + 60};
	if (rayHit(from, to, &fr) && fr < 1)
	{
		float k = rt::clamp((fr * len - 3.5f * u) / len, 0.45f, 1);
		dx *= k; dy *= k;
	}
	Vehicle v; memset(&v, 0, sizeof v);
	v.x = pp[0] + dx; v.y = pp[1] + dy; v.z = pp[2];
	v.heading = h * 180.0f / rt::PI + 180.0f + 35.0f;
	while (v.heading >= 360) v.heading -= 360;
	v.halfTrack = g_cars[idx].halfTrack; v.halfBase = g_cars[idx].halfBase; v.halfLength = g_cars[idx].halfLength;
	v.front = g_cars[idx].front; v.rear = g_cars[idx].rear; v.clearance = g_cars[idx].clearance;
	settle(v, 0, true);
	TESObjectREFR *r = placeRef(kCars[idx].parked, v.x, v.y, v.z, v.heading);
	if (!r) { LOG("showroom: couldn't place %s", kCars[idx].id); return; }
	pushRef(r, v);
	g_previewRef = refid(r); g_previewCar = idx; g_previewSpin = 0;
	LOG("showroom: %s at %.1f m%s", kCars[idx].id, rt::sqrt(dx * dx + dy * dy) / u, live ? " (live editor)" : "");
}
// The showroom car turns about the vertical: its root's local rotation becomes Rz(spin) *
static void previewTick(float dt)
{
	if (!g_previewRef) return;
	TESObjectREFR *r = (TESObjectREFR *)lookup(g_previewRef);
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) return;
	if (root != g_previewRoot) { memcpy(g_previewM0, (float *)(root + 0x34), sizeof g_previewM0); g_previewRoot = root; g_previewSpin = 0; }
	g_previewSpin += TUNE(showroomSpin) * rt::PI / 180.0f * dt;
	if (g_previewSpin > 2 * rt::PI) g_previewSpin -= 2 * rt::PI;
	float c = rt::cos(g_previewSpin), sn = rt::sin(g_previewSpin);
	const float *a = g_previewM0;
	float *m = (float *)(root + 0x34);
	for (int j = 0; j < 3; j++)
	{
		m[j] = c * a[j] - sn * a[3 + j];
		m[3 + j] = sn * a[j] + c * a[3 + j];
		m[6 + j] = a[6 + j];
	}
	updateNode(root);
}
static void colorName(int car, char *out, u32 cap)
{
	if (g_paint[car].rgb < 0) { rt::strcpy(out, "Factory", cap); return; }
	for (int i = 0; i < kNumColors; i++)
		if (kColors[i].rgb == (u32)g_paint[car].rgb) { rt::strcpy(out, kColors[i].label, cap); return; }
	char h[8];
	hex6(h, (u32)g_paint[car].rgb);
	rt::fmt(out, cap, "Custom #%s", h);
}
static void showPaintMain()
{
	int c = g_paintCar;
	char title[200], cn[48];
	colorName(c, cn, sizeof cn);
	rt::fmt(title, sizeof title, "PAINT SHOP: %s\nColor: %s\nFinish: %s", kCars[c].label, cn, kFinishes[g_paint[c].finish].label);
	const char *btn[6] = {"Colors", "Live color editor", "Finish", "Factory color", "Another car", "Done"};
	if (showMessageBox(title, btn, 6)) g_menu = 4;
}
static void showColors()
{
	int per = 8, pages = (kNumColors + per - 1) / per;
	if (g_colorPage >= pages) g_colorPage = 0;
	const char *btn[10]; int n = 0;
	for (int j = g_colorPage * per; j < kNumColors && n < per; j++)
	{
		bool cur = g_paint[g_paintCar].rgb == (i32)kColors[j].rgb;
		rt::fmt(g_btnText[n], sizeof g_btnText[n], "%s%s", cur ? "> " : "", kColors[j].label);
		g_menuMap[n] = j; btn[n] = g_btnText[n]; n++;
	}
	if (pages > 1) { g_menuMap[n] = -3; btn[n++] = "More colors..."; }
	g_menuMap[n] = -2; btn[n++] = "Back";
	char title[160], cn[48];
	colorName(g_paintCar, cn, sizeof cn);
	rt::fmt(title, sizeof title, "PAINT SHOP: %s\nColor: %s (page %d of %d)", kCars[g_paintCar].label, cn, g_colorPage + 1, pages);
	if (showMessageBox(title, btn, n)) g_menu = 5;
}
static void showFinishes()
{
	const char *btn[10]; int n = 0;
	for (int k = 0; k < kNumFinishes && n < 9; k++)
	{
		rt::fmt(g_btnText[n], sizeof g_btnText[n], "%s%s", g_paint[g_paintCar].finish == k ? "> " : "", kFinishes[k].label);
		g_menuMap[n] = k; btn[n] = g_btnText[n]; n++;
	}
	g_menuMap[n] = -2; btn[n++] = "Back";
	char title[128];
	rt::fmt(title, sizeof title, "PAINT SHOP: %s\nFinish: %s", kCars[g_paintCar].label, kFinishes[g_paint[g_paintCar].finish].label);
	if (showMessageBox(title, btn, n)) g_menu = 6;
}
static void showLiveHelp()
{
	const char *btn[2] = {"Start", "Back"};
	if (showMessageBox("LIVE COLOR EDITOR\nThe car stands in front of you; hold a key and watch it change.\n\n"
		"A / D or Left / Right: hue\nW / S or Up / Down: brightness\nZ / X: less / more color\nF: next finish\n"
		"Enter or G: keep it     Backspace: undo", btn, 2)) g_menu = 7;
}
static void liveNote()
{
	char m[128];
	rt::fmt(m, sizeof m, "Hue %d   Color %d%%   Brightness %d%%   %s", (int)(g_hsv[0] + 0.5f), (int)(g_hsv[1] * 100 + 0.5f),
		(int)(g_hsv[2] * 100 + 0.5f), kFinishes[g_paint[g_paintCar].finish].label);
	message(m, 0.9f);
}
static void liveStart()
{
	g_liveUndo = g_paint[g_paintCar];
	rgbToHsv(paintColor(g_paintCar), &g_hsv[0], &g_hsv[1], &g_hsv[2]);
	console("DisablePlayerControls 1 1 1 1 1 1 1");
	console("player.SetAngle X 4");
	g_liveEdit = true;
	previewPlace(g_paintCar, true);
	g_liveApplyT = g_liveNoteT = 0; g_liveDirty = false; g_liveNoteDirty = true;
	pressed(VK_PAINTKEEP); pressed(VK_USE); pressed(VK_PAINTUNDO); pressed(VK_PAINTFINISH);
	LOG("live editor: %s", kCars[g_paintCar].id);
}
static void showPaintMain();
static void liveStop(bool keep)
{
	g_liveEdit = false;
	console("EnablePlayerControls");
	if (!keep) g_paint[g_paintCar] = g_liveUndo;
	paintChanged(g_paintCar);
	message(keep ? "Paint kept." : "Paint undone.", 1.5f);
	previewPlace(g_paintCar, false);
	g_reopenPaintAt = rt::now() + 0.2;
}
static void liveTick(float dt, bool focus, double t)
{
	bool hl = held(VK_PAINTHUELEFT) || held(VK_PAINTHUELEFTALT), hr = held(VK_PAINTHUERIGHT) || held(VK_PAINTHUERIGHTALT);
	bool vu = held(VK_PAINTBRIGHTER) || held(VK_PAINTBRIGHTERALT), vd = held(VK_PAINTDARKER) || held(VK_PAINTDARKERALT);
	bool sl = held(VK_PAINTLESSCOLOR), sm = held(VK_PAINTMORECOLOR);
	bool fin = pressed(VK_PAINTFINISH), keep1 = pressed(VK_PAINTKEEP), keep2 = pressed(VK_USE), undo = pressed(VK_PAINTUNDO);
	if (!focus) return;
	if (keep1 || keep2) { liveStop(true); return; }
	if (undo) { liveStop(false); return; }
	float *hs = g_hsv;
	float h0 = hs[0], s0 = hs[1], v0 = hs[2];
	if (hr) hs[0] += kPaintRateHue * dt;
	if (hl) hs[0] -= kPaintRateHue * dt;
	if (sm) hs[1] += kPaintRateSat * dt;
	if (sl) hs[1] -= kPaintRateSat * dt;
	if (vu) hs[2] += kPaintRateVal * dt;
	if (vd) hs[2] -= kPaintRateVal * dt;
	while (hs[0] >= 360) hs[0] -= 360;
	while (hs[0] < 0) hs[0] += 360;
	hs[1] = rt::clamp(hs[1], 0, 1);
	hs[2] = rt::clamp(hs[2], 0.04f, 1);
	if (hs[0] != h0 || hs[1] != s0 || hs[2] != v0) g_liveDirty = true;
	if (fin)
	{
		g_paint[g_paintCar].finish = (u8)((g_paint[g_paintCar].finish + 1) % kNumFinishes);
		g_liveDirty = true;
		g_liveApplyT = 0;
	}
	if (g_liveDirty && t - g_liveApplyT >= 0.08)
	{
		g_paint[g_paintCar].rgb = (i32)hsvToRgb((float)(int)(hs[0] + 0.5f), (int)(hs[1] * 100 + 0.5f) / 100.0f, (int)(hs[2] * 100 + 0.5f) / 100.0f);
		g_paintVer[g_paintCar]++;
		paintTick();
		g_liveApplyT = t; g_liveDirty = false; g_liveNoteDirty = true;
	}
	if (g_liveNoteDirty && t - g_liveNoteT >= 0.9) { liveNote(); g_liveNoteT = t; g_liveNoteDirty = false; }
}
static void wheelShop(int idx);
static void paintShop(int idx)
{
	g_paintCar = idx;
	g_paintPick = false; g_pick = PICK_DRIVE;
	if (!g_paintLoaded) paintLoad();
	console("player.SetAngle X 4");
	previewPlace(idx, false);
	showPaintMain();
}

// The wheel shop: the car stands in the showroom while you try other cars' wheels on it
static void showWheelMain()
{
	int c = g_wheelCar;
	char title[200];
	if (g_rim[c] >= 0) rt::fmt(title, sizeof title, "WHEEL SHOP: %s\nWheels: from the %s", kCars[c].label, kCars[g_rim[c]].label);
	else rt::fmt(title, sizeof title, "WHEEL SHOP: %s\nWheels: its own", kCars[c].label);
	const char *btn[4] = {"Try another car's wheels", "Its own wheels", "Another car", "Done"};
	if (showMessageBox(title, btn, 4)) g_menu = 8;
}
static void wheelShop(int idx)
{
	g_wheelCar = idx;
	if (!g_rimLoaded) rimLoad();
	console("player.SetAngle X 4");
	previewPlace(idx, false);
	showWheelMain();
}

static void garageChosen(int menu, int button)
{
	if (button < 0 || button > 9) return;
	int v = g_menuMap[button];
	if (menu == 3)
	{
		if (v == 0) { g_pick = PICK_DRIVE; g_paintPick = false; showClasses(); }
		else if (v == 1) { g_pick = PICK_PAINT; g_paintPick = true; showClasses(); }
		else if (v == 3) { g_pick = PICK_WHEELS; g_paintPick = false; showClasses(); }
		else if (v == 2) { sendCarHome(); ensureDisplay(); }
	}
	else if (menu == 1)
	{
		if (v < 0) { if (g_pick == PICK_DONOR && g_wheelCar >= 0) showWheelMain(); else showHome(); return; }
		g_menuClass = v; g_menuPage = 0;
		showCars();
	}
	else if (menu == 2)
	{
		if (v == -3) { g_menuPage++; showCars(); }
		else if (v == -2) showClasses();
		else if (v >= 0 && v < kNumCars)
		{
			if (g_pick == PICK_PAINT) paintShop(v);
			else if (g_pick == PICK_WHEELS) wheelShop(v);
			else if (g_pick == PICK_DONOR && g_wheelCar >= 0)
			{
				g_rim[g_wheelCar] = v == g_wheelCar ? -1 : v;
				g_rimVer[g_wheelCar]++;
				rimSave();
				paintTick();
				showCars();
			}
			else takeCar(v);
		}
	}
	else if (menu == 4 && g_paintCar >= 0)
	{
		Paint &p = g_paint[g_paintCar];
		switch (button)
		{
		case 0: g_colorPage = 0; showColors(); break;
		case 1: showLiveHelp(); break;
		case 2: showFinishes(); break;
		case 3: p.rgb = -1; paintChanged(g_paintCar); showPaintMain(); break;
		case 4: previewRemove(); g_pick = PICK_PAINT; g_paintPick = true; showClasses(); break;
		default: previewRemove(); g_paintCar = -1; break;
		}
	}
	else if (menu == 5 && g_paintCar >= 0)
	{
		if (v == -3) { g_colorPage++; showColors(); }
		else if (v == -2) showPaintMain();
		else if (v >= 0 && v < kNumColors) { g_paint[g_paintCar].rgb = (i32)kColors[v].rgb; paintChanged(g_paintCar); showColors(); }
	}
	else if (menu == 6 && g_paintCar >= 0)
	{
		if (v == -2) showPaintMain();
		else if (v >= 0 && v < kNumFinishes) { g_paint[g_paintCar].finish = (u8)v; paintChanged(g_paintCar); showFinishes(); }
	}
	else if (menu == 8 && g_wheelCar >= 0)
	{
		switch (button)
		{
		case 0: g_pick = PICK_DONOR; showClasses(); break;
		case 1: g_rim[g_wheelCar] = -1; g_rimVer[g_wheelCar]++; rimSave(); paintTick(); showWheelMain(); break;
		case 2: previewRemove(); g_pick = PICK_WHEELS; showClasses(); break;
		default: previewRemove(); g_wheelCar = -1; g_pick = PICK_DRIVE; break;
		}
	}
	else if (menu == 7 && g_paintCar >= 0)
	{
		if (button == 0) liveStart();
		else showPaintMain();
	}
}

// ---- Collisions (your car)
static float wrap180(float d);
// Your car is a solid box: its outline (half width, nose, tail) is swept along each frame's move.
struct CarHit { float frac, nx, ny, lx, ly; u32 node; };
// Why the last wall was a wall (for the crash log): bumper-ray distance, lower ray's (1 missed), its slope angle, the
// upper ray's angle (1 missed)
static float g_wallD, g_wallD3 = -1, g_wallAng3 = -1, g_wallAng2 = -1;

// Loose things don't stop the car (up to looseMaxUnits across)
static bool looseObject(u32 node, u32 *body)
{
	int mt = motionTypeOfNode(node, body);
	if (mt < 0 || mt == 5 || mt == 7) return false;
	TESObjectREFR *r = refOfNode(node);
	if (mt == 4)
	{
		TESForm *base = r ? (TESForm *)ptr((u32)r + OFF_REF_BASE) : 0;
		if (!base || formtype(base) != 0x22) return false;
		*body = 0;
	}
	else if (!(mt & 2)) return false;
	u32 root = r ? thirdPersonNode(r) : node;
	float rad = boundRadius(root ? root : node);
	return rad > 0 && rad <= TUNE(looseMaxUnits);
}
static void kickLoose(const Vehicle &v, u32 body, float hx, float hy)
{
	if (!body) return;
	float U = kUnitsPerMetre, sp = rt::absf(v.speed);
	float hc = v.course * rt::PI / 180.0f, dir = v.speed >= 0 ? 1.0f : -1.0f;
	float ox = hx - v.x, oy = hy - v.y, ol = rt::sqrt(ox * ox + oy * oy);
	if (ol > 1) { ox /= ol; oy /= ol; }
	float k = TUNE(looseKickScale);
	float vel[3] = {rt::sin(hc) * dir * sp * U * k + ox * sp * U * 0.3f, rt::cos(hc) * dir * sp * U * k + oy * sp * U * 0.3f,
		(TUNE(looseKickUpMs) + sp * 0.15f) * U};
	setBodyVelocity(body, vel);
}

static bool sweepWorld(const Vehicle &v, float hw, float mx, float my, float len, CarHit &out)
{
	float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
	float pr = v.pitch * g_pitchSign * rt::PI / 180.0f, sp = rt::sin(pr), cp = rt::cos(pr);
	float fwdDot = mx * s + my * c;
	float dz = sp * (fwdDot >= 0 ? 1 : -1), dxy = cp;
	float F = v.front > 0 ? v.front : v.halfLength, R = v.rear > 0 ? v.rear : v.halfLength;
	const float pts[16][2] = {{-hw, F}, {-hw * 0.5f, F}, {0, F}, {hw * 0.5f, F}, {hw, F},
		{-hw, -R}, {-hw * 0.5f, -R}, {0, -R}, {hw * 0.5f, -R}, {hw, -R},
		{-hw, F * 0.5f}, {-hw, 0}, {-hw, -R * 0.5f}, {hw, F * 0.5f}, {hw, 0}, {hw, -R * 0.5f}};
	float inset = TUNE(crashInsetUnits), margin = 10;
	float zLow = v.z + TUNE(obstacleRayUnits), step = TUNE(obstacleStepUnits);
	float total = inset + len + margin;
	float best = 1e9f; int bi = -1; float bwx = 0, bwy = 0; u32 bnode = 0;
	for (int i = 0; i < 16; i++)
	{
		float px = pts[i][0], py = pts[i][1];
		float ox = px * c + py * s, oy = -px * s + py * c;
		float ol = rt::sqrt(ox * ox + oy * oy);
		if (ol < 1 || (ox * mx + oy * my) / ol < 0.2f) continue;
		float wx = v.x + ox, wy = v.y + oy;
		float zp = zLow + py * sp;
		float from[3] = {wx - mx * inset * dxy, wy - my * inset * dxy, zp - dz * inset};
		float to[3] = {from[0] + mx * total * dxy, from[1] + my * total * dxy, from[2] + dz * total};
		float fr; u32 obj;
		if (!rayHitObject(from, to, &fr, &obj)) continue;
		float d = fr * total - inset;
		if (d > len + 2) continue;
		if (d < -8) continue;
		TESObjectREFR *hr = obj ? refOfNode(obj) : 0;
		if (hr && (formtype(hr) == 0x3B || formtype(hr) == 0x3C)) continue;
		u32 body;
		if (obj && looseObject(obj, &body))
		{
			kickLoose(v, body, from[0] + (to[0] - from[0]) * fr, from[1] + (to[1] - from[1]) * fr);
			continue;
		}
		bool wall = true;
		float dbgD3 = -1, dbgA3 = -1, dbgA2 = -1;
		float drop = TUNE(obstacleRayUnits) - TUNE(obstacleLowUnits);
		float from3[3] = {from[0], from[1], from[2] - drop}, to3[3] = {to[0], to[1], to[2] - drop}, fr3; u32 obj3;
		if (!hr)
		{
			float from2[3] = {from[0], from[1], from[2] + step}, to2[3] = {to[0], to[1], to[2] + step}, fr2; u32 obj2;
			wall = rayHitObject(from2, to2, &fr2, &obj2) &&
				rt::atan2(step, rt::max(fr2 * total - inset - d, 0.01f)) * 180.0f / rt::PI > TUNE(maxClimbDegrees);
		}
		else if (drop > 1 && !rayHitObject(from3, to3, &fr3, &obj3))
		{
			float hx = from[0] + (to[0] - from[0]) * fr + mx * 4, hy = from[1] + (to[1] - from[1]) * fr + my * 4;
			float hz = from[2] + (to[2] - from[2]) * fr;
			float dn0[3] = {hx, hy, hz + step + 30}, dn1[3] = {hx, hy, hz - TUNE(obstacleRayUnits)}, frd; u32 objd;
			if (rayHitObject(dn0, dn1, &frd, &objd))
			{
				float top = dn0[2] + (dn1[2] - dn0[2]) * frd;
				dbgD3 = -2; dbgA3 = top - hz;
				if (top - hz <= step * 0.4f) wall = false;
			}
		}
		else if (drop > 1)
		{
			float d3 = fr3 * total - inset;
			dbgD3 = d3; dbgA3 = rt::atan2(drop, rt::max(d - d3, 0.01f)) * 180.0f / rt::PI;
			if (dbgA3 <= TUNE(maxClimbDegrees))
			{
				wall = false;
				float from2[3] = {from[0], from[1], from[2] + step}, to2[3] = {to[0], to[1], to[2] + step}, fr2; u32 obj2;
				if (rayHitObject(from2, to2, &fr2, &obj2))
				{
					float d2 = fr2 * total - inset;
					dbgA2 = rt::atan2(step, rt::max(d2 - d, 0.01f)) * 180.0f / rt::PI;
					if (dbgA2 > TUNE(maxClimbDegrees)) wall = true;
				}
			}
		}
		if (!wall) continue;
		if (d < best) { best = d; bi = i; bwx = wx; bwy = wy; bnode = obj; g_wallD = d; g_wallD3 = dbgD3; g_wallAng3 = dbgA3; g_wallAng2 = dbgA2; }
	}
	if (bi < 0) return false;
	out.frac = len > 0.01f ? rt::clamp((best - 2) / len, 0, 1) : 0;
	out.node = bnode;
	out.lx = pts[bi][0]; out.ly = pts[bi][1];
	float qx = -my, qy = mx, off = 14;
	float hx[2], hy[2]; int got = 0;
	for (int k = 0; k < 2; k++)
	{
		float sx = bwx + qx * off * (k ? 1 : -1), sy = bwy + qy * off * (k ? 1 : -1);
		float tot = inset + len + margin + 60;
		float from[3] = {sx - mx * inset * dxy, sy - my * inset * dxy, zLow + pts[bi][1] * sp - dz * inset};
		float to[3] = {from[0] + mx * tot * dxy, from[1] + my * tot * dxy, from[2] + dz * tot};
		float fr; u32 obj;
		if (rayHitObject(from, to, &fr, &obj)) { hx[got] = from[0] + (to[0] - from[0]) * fr; hy[got] = from[1] + (to[1] - from[1]) * fr; got++; }
		else break;
	}
	float nx = -mx, ny = -my;
	if (got == 2)
	{
		float tx = hx[1] - hx[0], ty = hy[1] - hy[0], tl = rt::sqrt(tx * tx + ty * ty);
		if (tl > 1)
		{
			nx = -ty / tl; ny = tx / tl;
			if (nx * mx + ny * my > 0) { nx = -nx; ny = -ny; }
		}
	}
	out.nx = nx; out.ny = ny;
	return true;
}

// Two boxes on the ground (centre, heading in degrees, half width, nose and tail lengths): if they overlap, the
// shortest push (and its direction, from b to a) that separates them
static bool boxOverlap(float ax, float ay, float ah, float aw, float af, float ar, float bx, float by, float bh, float bw, float bf, float br,
	float *nx, float *ny, float *depth)
{
	float a1 = ah * rt::PI / 180.0f, b1 = bh * rt::PI / 180.0f;
	float afx = rt::sin(a1), afy = rt::cos(a1), arx = afy, ary = -afx;
	float bfx = rt::sin(b1), bfy = rt::cos(b1), brx = bfy, bry = -bfx;
	float acx = ax + afx * (af - ar) * 0.5f, acy = ay + afy * (af - ar) * 0.5f, al = (af + ar) * 0.5f;
	float bcx = bx + bfx * (bf - br) * 0.5f, bcy = by + bfy * (bf - br) * 0.5f, bl = (bf + br) * 0.5f;
	float axes[4][2] = {{afx, afy}, {arx, ary}, {bfx, bfy}, {brx, bry}};
	float dx = acx - bcx, dy = acy - bcy;
	float best = 1e9f, bnx = 0, bny = 0;
	for (int k = 0; k < 4; k++)
	{
		float ux = axes[k][0], uy = axes[k][1];
		float ra = al * rt::absf(afx * ux + afy * uy) + aw * rt::absf(arx * ux + ary * uy);
		float rb = bl * rt::absf(bfx * ux + bfy * uy) + bw * rt::absf(brx * ux + bry * uy);
		float dist = dx * ux + dy * uy;
		float o = ra + rb - rt::absf(dist);
		if (o <= 0) return false;
		if (o < best) { best = o; bnx = dist < 0 ? -ux : ux; bny = dist < 0 ? -uy : uy; }
	}
	*nx = bnx; *ny = bny; *depth = best;
	return true;
}

// The outline must not end up inside anything: the turn isn't swept, so a spin after a hit (or steering along a wall)
// swings the corners into what was hit, and then the ground under them was the top of it.
static bool depenetrate(Vehicle &v, float hw)
{
	bool moved = false;
	for (int pass = 0; pass < 2; pass++)
	{
		float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
		float sp = rt::sin(v.pitch * g_pitchSign * rt::PI / 180.0f);
		float F = v.front > 0 ? v.front : v.halfLength, R = v.rear > 0 ? v.rear : v.halfLength;
		const float pts[16][2] = {{-hw, F}, {-hw * 0.5f, F}, {0, F}, {hw * 0.5f, F}, {hw, F},
			{-hw, -R}, {-hw * 0.5f, -R}, {0, -R}, {hw * 0.5f, -R}, {hw, -R},
			{-hw, F * 0.5f}, {-hw, 0}, {-hw, -R * 0.5f}, {hw, F * 0.5f}, {hw, 0}, {hw, -R * 0.5f}};
		float zLow = v.z + TUNE(obstacleRayUnits);
		float best = TUNE(crashPushMinUnits), bx = 0, by = 0;
		for (int i = 0; i < 16; i++)
		{
			float px = pts[i][0], py = pts[i][1], ix = px * 0.35f, iy = py * 0.35f;
			float from[3] = {v.x + ix * c + iy * s, v.y - ix * s + iy * c, zLow + iy * sp};
			float to[3] = {v.x + px * c + py * s, v.y - px * s + py * c, zLow + py * sp};
			float fr; u32 obj;
			if (!rayHitObject(from, to, &fr, &obj) || !obj) continue;
			TESObjectREFR *hr = refOfNode(obj);
			if (hr && (formtype(hr) == 0x3B || formtype(hr) == 0x3C)) continue;
			float hx = from[0] + (to[0] - from[0]) * fr, hy = from[1] + (to[1] - from[1]) * fr, hz = from[2] + (to[2] - from[2]) * fr, tz;
			u32 body;
			if (looseObject(obj, &body)) { kickLoose(v, body, hx, hy); continue; }
			if (terrainHeight(hx, hy, &tz) && rt::absf(hz - tz) < 4) continue;
			float dx = from[0] - to[0], dy = from[1] - to[1], dl = rt::sqrt(dx * dx + dy * dy);
			float depth = (1 - fr) * dl;
			if (dl > 1 && depth > best) { best = depth; bx = dx / dl * depth; by = dy / dl * depth; }
		}
		if (bx == 0 && by == 0) break;
		v.x += bx; v.y += by;
		moved = true;
	}
	return moved;
}

static float crashResponse(Vehicle &v, float nx, float ny, float lx, float ly)
{
	float hc = v.course * rt::PI / 180.0f;
	float vx = rt::sin(hc) * v.speed, vy = rt::cos(hc) * v.speed;
	float vn = vx * nx + vy * ny;
	if (vn >= 0) return 0;
	float impact = -vn;
	float tx = -ny, ty = nx, vt = vx * tx + vy * ty;
	if (vt < 0) { tx = -tx; ty = -ty; vt = -vt; }
	float approach = rt::atan2(impact, vt) * 180.0f / rt::PI;
	float e = TUNE(crashRestitution), mu = TUNE(crashFriction);
	if (v.speed > 0 && approach < TUNE(crashScrapeDegrees))
	{
		float along = rt::max(vt - mu * impact, 0), off = e * impact;
		float ns = rt::sqrt(along * along + off * off);
		float cx = along * tx + off * nx, cy = along * ty + off * ny;
		if (ns < 0.3f) { v.speed = 0; v.course = v.heading; return impact; }
		v.speed = ns;
		v.course = rt::atan2(cx, cy) * 180.0f / rt::PI;
		float dh = wrap180(v.course - v.heading);
		v.heading += dh * TUNE(crashAlign);
		g_yawRate *= 0.3f;
	}
	else
	{
		float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
		float rx = (lx * c + ly * s) / kUnitsPerMetre, ry = (-lx * s + ly * c) / kUnitsPerMetre;
		float J = (1 + e) * impact;
		float torque = rx * (J * ny) - ry * (J * nx);
		g_yawRate = rt::clamp(g_yawRate - torque * TUNE(crashSpinGain), -220, 220);
		v.speed = v.speed > 0 ? -e * impact : e * impact;
		v.course = v.heading;
	}
	while (v.heading >= 360) v.heading -= 360;
	while (v.heading < 0) v.heading += 360;
	while (v.course >= 360) v.course -= 360;
	while (v.course < 0) v.course += 360;
	return impact;
}

static double g_crashSoundT;
static void crashEffects(float impact, TESObjectREFR *pl)
{
	if (impact < TUNE(crashSoundMinMs)) return;
	double t = rt::now();
	if (t - g_crashSoundT > 0.25)
	{
		g_crashSoundT = t;
		int which = impact < 6 ? 0 : impact < 14 ? 1 : 2;
		audio::playShot(which, TUNE(crashVolume) * rt::clamp(impact / 12.0f, 0.3f, 1.5f), 0.9f + 0.2f * (float)((u32)(t * 1000) % 100) / 100.0f);
	}
	if (impact > 12) hurt(pl, (impact - 12) * TUNE(crashDamagePerMs));
	if (impact > 6) LOG("crash: %.1f m/s", impact);
}

// Moves your car by its speed, solid against the world and the show cars; returns the hardest impact (m/s)
static float advanceCar(float dt, TESObjectREFR *pl)
{
	Vehicle &v = g_car;
	float hw = g_activeCar >= 0 ? g_cars[g_activeCar].halfWidth : v.halfTrack + 16;
	float hc = v.course * rt::PI / 180.0f;
	float vx = rt::sin(hc) * v.speed, vy = rt::cos(hc) * v.speed;
	float spd = rt::sqrt(vx * vx + vy * vy);
	float len = spd * kUnitsPerMetre * dt;
	float impact = 0;
	if (len > 0.01f)
	{
		float mx = vx / spd, my = vy / spd;
		CarHit hit;
		bool got = sweepWorld(v, hw, mx, my, len, hit);
		float mv = got ? len * hit.frac : len;
		v.x += mx * mv; v.y += my * mv;
		if (got)
		{
			float imp = crashResponse(v, hit.nx, hit.ny, hit.lx, hit.ly);
			impact = rt::max(impact, imp);
			if (imp > TUNE(crashSoundMinMs))
			{
				TESObjectREFR *r = hit.node ? refOfNode(hit.node) : 0;
				TESForm *base = r ? (TESForm *)ptr((u32)r + OFF_REF_BASE) : 0;
				u32 body; int mt = hit.node ? motionTypeOfNode(hit.node, &body) : -1;
				LOG("crash into %s %x (base %x, form type %x), Havok motion %d, size %.0f; bumper ray %.0f, lower ray %.0f (%.0f deg), upper %.0f deg, car pitch %.1f",
					r ? "ref" : "the landscape or unknown", refid(r), refid(base), base ? formtype(base) : 0, mt, boundRadius(r ? thirdPersonNode(r) : hit.node),
					g_wallD, g_wallD3, g_wallAng3, g_wallAng2, v.pitch);
			}
		}
	}
	for (int sl = 0; sl < kNumSlots && sl < 8; sl++)
	{
		if (!g_display[sl] || g_slotCar[sl] < 0) continue;
		int sc = g_slotCar[sl];
		float bx, by;
		bayXY(sl, &bx, &by);
		float nx, ny, depth;
		float vf = v.front > 0 ? v.front : v.halfLength, vr = v.rear > 0 ? v.rear : v.halfLength;
		if (!boxOverlap(v.x, v.y, v.heading, hw, vf, vr, bx, by, kSlots[sl].heading, g_cars[sc].halfWidth, g_cars[sc].front > 0 ? g_cars[sc].front : g_cars[sc].halfLength,
				g_cars[sc].rear > 0 ? g_cars[sc].rear : g_cars[sc].halfLength, &nx, &ny, &depth))
			continue;
		v.x += nx * depth; v.y += ny * depth;
		float h = v.heading * rt::PI / 180.0f, s = rt::sin(h), c = rt::cos(h);
		float lx = rt::clamp((-nx) * c - (-ny) * s, -1, 1) * hw, ly = rt::clamp((-nx) * s + (-ny) * c, -1, 1) * vf;
		impact = rt::max(impact, crashResponse(v, nx, ny, lx, ly));
	}
	if (depenetrate(v, hw)) g_yawRate *= 0.5f;
	crashEffects(impact, pl);
	return impact;
}

static float rnd01()
{
	static u32 st;
	if (!st) st = (u32)(rt::now() * 1000.0) | 1;
	st = st * 1664525u + 1013904223u;
	return (float)(st >> 8) / 16777216.0f;
}

static float wrap180(float d)
{
	while (d > 180) d -= 360;
	while (d < -180) d += 360;
	return d;
}

// ---- Running people over
static void hitTick(float dt, TESObjectREFR *pl)
{
	for (int i = 0; i < 8; i++) if (g_hits[i].t > 0) g_hits[i].t -= dt;
	float carSpeed = rt::absf(g_car.speed);
	if (g_driving && carSpeed > TUNE(hitMinMs))
	{
		float h = g_car.heading * rt::PI / 180.0f, hs = rt::sin(h), hcos = rt::cos(h);
		float hw = g_activeCar >= 0 ? g_cars[g_activeCar].halfWidth : g_car.halfTrack + 16;
		float nose = g_car.front > 0 ? g_car.front : g_car.halfLength, tail = g_car.rear > 0 ? g_car.rear : g_car.halfLength;
		float pr = TUNE(hitRadiusUnits), reach = rt::max(nose, tail) + rt::max(hw, 1.0f) + pr;
		TESObjectREFR *carRef = g_car.refID ? (TESObjectREFR *)lookup(g_car.refID) : 0;
		u32 inside[16]; int nIn = 0;
		forEachActor([&](TESObjectREFR *a) {
			u8 ft = formtype(a);
			if (nIn >= 16 || a == pl || (ft != 0x3B && ft != 0x3C) || rd<u32>((u32)a + OFF_ACTOR_LIFESTATE) != 0) return;
			float *ap = pos(a); float dx = ap[0] - g_car.x, dy = ap[1] - g_car.y, dz = ap[2] - g_car.z;
			if (dz < -100 || dz > 150 || dx * dx + dy * dy > reach * reach) return;
			float lx = dx * hcos - dy * hs, ly = dx * hs + dy * hcos;
			if (rt::absf(lx) > hw + pr || ly > nose + pr || ly < -tail - pr) return;
			inside[nIn++] = refid(a);
		});
		for (int k = 0; k < nIn; k++)
		{
			u32 id = inside[k];
			TESObjectREFR *a = (TESObjectREFR *)lookup(id);
			if (!a || rd<u32>((u32)a + OFF_ACTOR_LIFESTATE) != 0) continue;
			u8 ft = formtype(a);
			int freeSlot = -1;
			bool recent = false;
			for (int i = 0; i < 8; i++)
			{
				if (g_hits[i].t > 0 && g_hits[i].id == id) recent = true;
				if (g_hits[i].t <= 0 && freeSlot < 0) freeSlot = i;
			}
			if (recent) continue;
			if (freeSlot >= 0) { g_hits[freeSlot].id = id; g_hits[freeSlot].t = TUNE(hitCooldownSeconds); }
			bool hard = carSpeed > TUNE(hitHurtFromMs);
			float force = rt::clamp(carSpeed * TUNE(hitPushPerMs), TUNE(hitPushMin), 80);
			float dmg = hard ? carSpeed * TUNE(hitDamagePerMs) : 0;
			int mate = hitActor(carRef ? carRef : pl, a, force, dmg);
			if (mate < 0)
			{
				char cmd[64];
				rt::fmt(cmd, sizeof cmd, "player.pushactoraway %X %d", id, (int)force);
				console(cmd);
				if (hard) hurt(a, dmg);
			}
			LOG("hit %x (%s) at %.1f m/s: force %.0f damage %.0f%s", id, ft == 0x3B ? "person" : "creature", carSpeed, force, mate == 1 ? 0.0f : dmg,
				mate == 1 ? " (companion)" : "");
			audio::playShot(0, TUNE(crashVolume) * (hard ? 0.6f : 0.3f), hard ? 1.1f : 1.25f);
			if (!hard || mate == 1) continue;
			g_car.speed *= TUNE(hitSlowdown);
		}
	}
}

static HiddenSet g_hidden3rd, g_hidden1st;
// Widens the world view by `add` degrees over what it was when you got in.
namespace rtx {
static void *setVar;
static bool off, failed;
static void toggle()
{
	if (failed) { message("RTX Remix isn't usable here (see the log)", 3); return; }
	if (!setVar)
	{
		static const char *const mods[] = {"d3d9_remix.dll", "d3d9.dll"};
		int (__stdcall *init)(const void *, void *) = 0;
		const char *from = 0;
		for (int i = 0; i < 2 && !init; i++)
		{
			HANDLE d = GetModuleHandleA(mods[i]);
			init = d ? (int (__stdcall *)(const void *, void *))GetProcAddress(d, "remixapi_InitializeLibrary") : 0;
			from = mods[i];
		}
		if (!init) { failed = true; LOG("rtx: no RTX Remix bridge loaded (d3d9_remix.dll / d3d9.dll without remixapi_InitializeLibrary)"); message("RTX Remix isn't in the game", 3); return; }
		LOG("rtx: the Remix bridge is %s", from);
		struct { u32 sType; void *next; u64 version; } info = {1, 0, ((u64)0 << 48) | ((u64)6 << 16) | 5};
		void *fns[64] = {};
		int r = init(&info, fns);
		if (r != 0 || !fns[10]) { failed = true; LOG("rtx: Remix API refused (error %d) - its bridge.conf needs exposeRemixApi = True", r); message("RTX Remix refused the switch (see the log)", 3); return; }
		setVar = fns[10];
		LOG("rtx: Remix API ready");
	}
	off = !off;
	int r = ((int (__stdcall *)(const char *, const char *))setVar)("rtx.enableRaytracing", off ? "False" : "True");
	LOG("rtx: path tracing %s (SetConfigVariable %d)", off ? "off" : "on", r);
	message(off ? "RTX off (Insert): the game's own rendering" : "RTX on (Insert): path tracing", 2);
}
}
// ---- A read-only quest logger: every quest
namespace coop { bool inSession(); bool sharing(); void questStep(u32 quest, const char *edid, const char *name, int from, int to, u8 flags); void walletRoll(); }
namespace questlog
{
struct Q { u32 form, id; u8 flags, stage; u32 doneMask[8]; };
static Q *qs;
static int nq;
static bool checked, on, fileOn;
static double snapAt, nextPoll;
static int snapN, lastPeers = -1;
static const char *edid(u32 f) { u32 s = ptr(f + 0x64); return s && readable(s, 4) ? (const char *)s : ""; }
static const char *qname(u32 f) { u32 s = ptr(f + 0x34); return s && readable(s, 4) ? (const char *)s : ""; }
static void doneOf(u32 f, u32 *mask)
{
	memset(mask, 0, 32);
	int n = 0;
	for (u32 node = f + 0x44; node && n < 300; node = ptr(node + 4), n++)
	{
		u32 si = ptr(node);
		if (!si) continue;
		u8 st = rd<u8>(si), done = rd<u8>(si + 1);
		if (done) mask[st >> 5] |= 1u << (st & 31);
	}
}
static void build()
{
	u32 map = ptr(ADDR_FORM_MAP);
	if (!map) return;
	u32 buckets = rd<u32>(map + 4), table = ptr(map + 8);
	if (!buckets || !table || buckets > (1u << 24)) return;
	int cap = 4096;
	qs = (Q *)rt::alloc(cap * sizeof(Q));
	for (u32 b = 0; b < buckets && nq < cap; b++)
		for (u32 e = ptr(table + 4 * b); e && nq < cap; e = ptr(e))
		{
			u32 f = ptr(e + 8);
			if (!f || formtype((void *)f) != 0x47 || !readable(f, 0x6C)) continue;
			Q &q = qs[nq++];
			q.form = f; q.id = rd<u32>(e + 4);
			q.flags = rd<u8>(f + 0x3C); q.stage = rd<u8>(f + 0x60);
			doneOf(f, q.doneMask);
		}
	LOG("quest: %d quests in the game, watched read-only (questlog.on)", nq);
}
static int doneList(const u32 *m, char *out, int cap)
{
	int n = 0;
	out[0] = 0;
	for (int s = 0; s < 256 && n < cap - 6; s++) if (m[s >> 5] & (1u << (s & 31))) n += rt::fmt(out + n, cap - n, "%s%d", n ? "," : "", s);
	return n;
}
static void snapshot(const char *why)
{
	snapN++;
	char p[400];
	rt::fmt(p, sizeof p, "%sMojaveMostWanted\\quests-%d.txt", g_dir, snapN % 10);
	int cap = 1 << 20, len = 0, running = 0, finished = 0;
	char *buf = (char *)rt::alloc(cap);
	len += rt::fmt(buf + len, cap - len, "# quest snapshot %d (%s), %d quests: form id, editor id, name, flags (1 running, 2 finished), current stage, stages done\r\n", snapN, why, nq);
	for (int i = 0; i < nq; i++)
	{
		Q &q = qs[i];
		char d[400];
		doneList(q.doneMask, d, sizeof d);
		if (len < cap - 600) len += rt::fmt(buf + len, cap - len, "%08x\t%s\t%s\t%x\t%d\t%s\r\n", q.id, edid(q.form), qname(q.form), q.flags, q.stage, d);
		if (q.flags & 1) running++;
		if (q.flags & 2) finished++;
	}
	rt::writefile(p, buf, (u32)len, false);
	rt::free(buf);
	LOG("quest: snapshot %d (%s) - %d running, %d finished, %d quests; in MojaveMostWanted\\quests-%d.txt", snapN, why, running, finished, nq, snapN % 10);
	int shown = 0;
	for (int i = 0; i < nq && shown < 80; i++)
	{
		Q &q = qs[i];
		bool any = q.stage || (q.flags & 2);
		for (int k = 0; k < 8 && !any; k++) any = q.doneMask[k] != 0;
		if (!any) continue;
		char d[200];
		doneList(q.doneMask, d, sizeof d);
		LOG("quest:   %s %08x %s \"%s\" stage %d (done %s)", (q.flags & 2) ? "finished" : (q.flags & 1) ? "running " : "waiting ", q.id, edid(q.form), qname(q.form), q.stage, d[0] ? d : "none");
		shown++;
	}
}
static void poll()
{
	for (int i = 0; i < nq; i++)
	{
		Q &q = qs[i];
		u8 fl = rd<u8>(q.form + 0x3C), st = rd<u8>(q.form + 0x60);
		u32 m[8];
		doneOf(q.form, m);
		bool newDone = false;
		for (int k = 0; k < 8; k++) newDone = newDone || (m[k] & ~q.doneMask[k]);
		if (fl == q.flags && st == q.stage && !newDone) continue;
		char d[200] = "";
		if (newDone)
		{
			u32 add[8];
			for (int k = 0; k < 8; k++) add[k] = m[k] & ~q.doneMask[k];
			doneList(add, d, sizeof d);
		}
		const char *what = (fl & 2) && !(q.flags & 2) ? "finished" : (fl & 1) && !(q.flags & 1) ? "started" : !(fl & 1) && (q.flags & 1) ? "stopped" : "changed";
		if (newDone && coop::sharing())
		{
			int from = q.stage;
			for (int k = 0; k < 256; k++) if ((m[k >> 5] & ~q.doneMask[k >> 5]) & (1u << (k & 31))) { coop::questStep(q.id, edid(q.form), qname(q.form), from, k, fl); from = k; }
		}
		if (fileOn) LOG("quest: %s %08x %s \"%s\" - stage %d -> %d%s%s (flags %x -> %x)", what, q.id, edid(q.form), qname(q.form), q.stage, st,
			d[0] ? ", stages done now: " : "", d, q.flags, fl);
		q.flags = fl; q.stage = st;
		memcpy(q.doneMask, m, sizeof m);
	}
}
void loaded() { snapAt = rt::now() + 3; }
void tick(double t)
{
	if (!checked)
	{
		checked = true;
		char p[400];
		rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\questlog.on", g_dir);
		fileOn = GetFileAttributesA(p) != 0xFFFFFFFF;
	}
	if (!on)
	{
		on = true;
		build();
		if (!snapAt) snapAt = t + 3;
	}
	if (!on || !nq) return;
	if (snapAt && t >= snapAt)
	{
		snapAt = 0;
		for (int i = 0; i < nq; i++) { Q &q = qs[i]; q.flags = rd<u8>(q.form + 0x3C); q.stage = rd<u8>(q.form + 0x60); doneOf(q.form, q.doneMask); }
		if (fileOn) snapshot("after a load");
	}
	net::Ghost g[4];
	int peers = net::ghosts(g, 4);
	if (fileOn && lastPeers >= 0 && peers != lastPeers) snapshot(peers > lastPeers ? "a friend joined" : "a friend left");
	lastPeers = peers;
	if (!snapAt && t >= nextPoll) { nextPoll = t + 0.5; poll(); coop::walletRoll(); }
}
}

// Once a game run, 30 s into play: where every gun of every weapon pack can turn up.
namespace lootcheck {
static double at;
static bool done;
static const char *const kBase[] = {"FalloutNV.esm", "DeadMoney.esm", "HonestHearts.esm", "OldWorldBlues.esm", "LonesomeRoad.esm",
	"GunRunnersArsenal.esm", "CaravanPack.esm", "ClassicPack.esm", "MercenaryPack.esm", "TribalPack.esm", "Fallout3.esm",
	"Anchorage.esm", "ThePitt.esm", "BrokenSteel.esm", "PointLookout.esm", "Zeta.esm", "TaleOfTwoWastelands.esm", "YUPTTW.esm",
	"MojaveMostWanted.esp"};
static const char *const kVendorLists[] = {"188NCRArmsMerchantWeap", "DLC01VendorWeaponsSmallGunsRare", "GunRunnerGunsAllTier1",
	"GunRunnerGunsAllTier2", "GunRunnerGunsAllTier3", "GunRunnerGunsAllTier4", "GunRunnerGunsAllTier5", "VendorWeaponModsGunsTier2",
	"VendorWeaponModsGunsTier4", "VendorWeaponsExplosivesCommon", "VendorWeaponsExplosivesCommon75DC", "VendorWeaponsExplosivesCommonDC",
	"VendorWeaponsExplosivesRare", "VendorWeaponsExplosivesTier2", "VendorWeaponsExplosivesTier3", "VendorWeaponsExplosivesTier4",
	"VendorWeaponsExplosivesTier5", "VendorWeaponsGunsCommon", "VendorWeaponsGunsCommon75", "VendorWeaponsGunsTier1",
	"VendorWeaponsGunsTier2", "VendorWeaponsGunsTier3", "VendorWeaponsGunsTier4", "VendorWeaponsGunsTier5", "VendorWeaponsHoldout",
	"VendorWeaponsSmallGunsCommon75DC", "VendorWeaponsSmallGunsCommonDC"};
static const int kNV = sizeof kVendorLists / sizeof kVendorLists[0];
enum { kVendor = 1, kChars = 2, kConts = 4, kSeen = 8, kBusy = 16, kAbove = 32 };
static const char *modName(u32 i)
{
	u32 dh = ptr(0x11C3F2C);
	if (!dh || i >= rd<u32>(dh + 0x218)) return "?";
	u32 mi = ptr(dh + 0x21C + 4 * i);
	return mi ? (const char *)(mi + 0x20) : "?";
}
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static bool same(const char *a, const char *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return !*a && !*b; }
static bool has(const char *s, const char *w)
{
	for (; *s; s++) { const char *p = s, *q = w; while (*q && lower(*p) == lower(*q)) { p++; q++; } if (!*q) return true; }
	return false;
}
static u32 *s_rg, s_nrg;
static void regions()
{
	const u32 cap = 1u << 17;
	s_rg = (u32 *)rt::alloc(cap * 8); s_nrg = 0;
	for (u32 a = 0x10000, guard = 0; s_vq && guard < (1u << 20); guard++)
	{
		u32 mbi[7] = {0};
		if (!s_vq((const void *)a, mbi, sizeof mbi)) break;
		u32 lo = mbi[0], hi = mbi[0] + mbi[3], prot = mbi[5];
		if (hi <= a) break;
		if (mbi[4] == 0x1000 && !(prot & 0x101) && (prot & 0xEE))
		{
			if (s_nrg && s_rg[2 * s_nrg - 1] == lo) s_rg[2 * s_nrg - 1] = hi;
			else if (s_nrg < cap) { s_rg[2 * s_nrg] = lo; s_rg[2 * s_nrg + 1] = hi; s_nrg++; }
		}
		a = hi;
	}
}
static bool ok(u32 p, u32 n)
{
	u32 a = 0, b = s_nrg;
	while (a < b) { u32 m = (a + b) / 2; if (s_rg[2 * m + 1] <= p) a = m + 1; else b = m; }
	return a < s_nrg && s_rg[2 * a] <= p && p + n <= s_rg[2 * a + 1];
}
template <typename F> static void eachEntry(u32 node, u32 formAt, F f)
{
	for (int guard = 0; node && guard < 1024 && ok(node, 8); guard++, node = ptr(node + 4))
	{
		u32 d = ptr(node);
		if (d && ok(d, formAt + 4)) { u32 fm = ptr(d + formAt); if (fm && ok(fm, 0x10)) f(fm); }
	}
}
struct Index {
	u32 *key, *val, cap;
	void init(u32 n) { cap = 1; while (cap < n * 2 + 16) cap <<= 1; key = (u32 *)rt::alloc(cap * 4); val = (u32 *)rt::alloc(cap * 4); }
	void put(u32 k, u32 v) { u32 i = (k >> 3) & (cap - 1); while (key[i] && key[i] != k) i = (i + 1) & (cap - 1); key[i] = k; val[i] = v; }
	int get(u32 k) const { u32 i = (k >> 3) & (cap - 1); while (key[i]) { if (key[i] == k) return (int)val[i]; i = (i + 1) & (cap - 1); } return -1; }
	void drop() { rt::free(key); rt::free(val); }
};
static u32 *s_list, *s_par, *s_parAt, *s_wpar, *s_wparAt;
static u8 *s_flag;
static u8 reach(u32 l, int depth)
{
	u8 &f = s_flag[l];
	if (f & kSeen) return f & 7;
	if (f & kBusy || depth > 64) return f & 7;
	f |= kBusy;
	u8 r = f & 7;
	for (u32 k = s_parAt[l]; k < s_parAt[l + 1]; k++) r |= reach(s_par[k], depth + 1);
	f = (u8)((f & ~kBusy) | kSeen | r);
	return r;
}
static void run()
{
	double t0 = rt::now();
	regions();
	LOG("loot: %u readable memory regions (%d ms)", s_nrg, (int)((rt::now() - t0) * 1000));
	u32 map = ptr(ADDR_FORM_MAP);
	if (!map || !readable(map, 12)) { LOG("loot: no form map"); return; }
	u32 nb = rd<u32>(map + 4), bk = ptr(map + 8);
	if (!nb || !readable(bk, nb * 4)) { LOG("loot: no form map"); return; }
	bool base[256] = {};
	u32 dh = ptr(0x11C3F2C), nmods = dh ? rd<u32>(dh + 0x218) : 0;
	for (u32 i = 0; i < nmods && i < 255; i++)
		for (u32 k = 0; k < sizeof kBase / sizeof kBase[0]; k++) if (same(modName(i), kBase[k])) base[i] = true;
	u32 nl = 0, nw = 0, nh = 0;
	for (int pass = 0; pass < 2; pass++)
	{
		if (pass) { s_list = (u32 *)rt::alloc(nl * 4 + 4); s_wpar = (u32 *)rt::alloc(nw * 4 + 4); s_par = (u32 *)rt::alloc(nh * 4 + 4); nl = nw = nh = 0; }
		for (u32 i = 0; i < nb; i++)
			for (u32 e = ptr(bk + 4 * i), g = 0; e && g < 4096 && ok(e, 12); e = ptr(e), g++)
			{
				u32 fm = ptr(e + 8);
				if (!fm || !ok(fm, 0x70)) continue;
				u8 ty = formtype((void *)fm);
				u32 id = refid((void *)fm);
				if (ty == 0x34) { if (pass) s_list[nl] = fm; nl++; }
				else if (ty == 0x28 && id >> 24 < 255 && !base[id >> 24]) { if (pass) s_wpar[nw] = fm; nw++; }
				else if (ty == 0x1B || ty == 0x2A || ty == 0x2B) { if (pass) s_par[nh] = fm; nh++; }
			}
	}
	LOG("loot: %u leveled item lists, %u containers and characters, %u guns of weapon packs (%u mod files) - %d ms so far", nl, nh, nw, nmods, (int)((rt::now() - t0) * 1000));
	u32 *hold = s_par;
	Index li; li.init(nl);
	for (u32 i = 0; i < nl; i++) li.put(s_list[i], i);
	Index wi; wi.init(nw);
	for (u32 i = 0; i < nw; i++) wi.put(s_wpar[i], i);
	u32 *wfm = s_wpar;
	s_flag = (u8 *)rt::alloc(nl + 1);
	int vendorsFound = 0;
	static const char *const kWays[3] = {
		"Begin Function {}\nSetFunctionValue (EditorIDToFormID \"%s\")\nEnd",
		"Begin Function {}\nref rL\nset rL to EditorIDToFormID \"%s\"\nSetFunctionValue rL\nEnd",
		"Begin Function {}\nref rL\nlet rL := GetFormByEditorID \"%s\"\nSetFunctionValue rL\nEnd"};
	bool haveJG = cmdExists("EditorIDToFormID"), haveGF = cmdExists("GetFormByEditorID");
	int way = haveJG ? -1 : haveGF ? 2 : 3;
	if (way == 3) LOG("loot: no command to look up an editor id (JohnnyGuitar's EditorIDToFormID or GetFormByEditorID) - vendor lists unknown");
	for (int i = 0; i < kNV && way < 3; i++)
	{
		char src[200];
		Script *sc = 0;
		for (int w = way < 0 ? 0 : way; w < (haveGF ? 3 : 2) && !sc; w++)
		{
			rt::fmt(src, sizeof src, kWays[w], kVendorLists[i]);
			sc = g_script->CompileScript(src);
			if (sc && way < 0) { way = w; LOG("loot: vendor lists by name - way %d compiles", w); }
			if (way >= 0) break;
		}
		if (!sc) { LOG("loot: none of 3 ways to look up an editor id compiled - vendor lists unknown"); break; }
		NVSEElement res; memset(&res, 0, sizeof res);
		g_script->CallFunction(sc, player(), 0, &res, 0);
		int l = res.type == 2 && res.form ? li.get((u32)res.form) : -1;
		if (l >= 0) { s_flag[l] |= kVendor; vendorsFound++; }
	}
	LOG("loot: %d of %d vendor lists found by name", vendorsFound, kNV);
	u32 *lc = (u32 *)rt::alloc((nl + 1) * 4), *wc = (u32 *)rt::alloc((nw + 1) * 4);
	for (u32 i = 0; i < nl; i++)
		eachEntry(s_list[i] + 0x34, 0, [&](u32 fm) {
			int c = li.get(fm);
			if (c >= 0) lc[c]++;
			else { int w = wi.get(fm); if (w >= 0) wc[w]++; }
		});
	s_parAt = (u32 *)rt::alloc((nl + 2) * 4);
	s_wparAt = (u32 *)rt::alloc((nw + 2) * 4);
	for (u32 i = 0; i < nl; i++) s_parAt[i + 1] = s_parAt[i] + lc[i];
	for (u32 i = 0; i < nw; i++) s_wparAt[i + 1] = s_wparAt[i] + wc[i];
	u32 *lpar = (u32 *)rt::alloc(s_parAt[nl] * 4 + 4), *wpar = (u32 *)rt::alloc(s_wparAt[nw] * 4 + 4);
	memset(lc, 0, (nl + 1) * 4); memset(wc, 0, (nw + 1) * 4);
	for (u32 i = 0; i < nl; i++)
		eachEntry(s_list[i] + 0x34, 0, [&](u32 fm) {
			int c = li.get(fm);
			if (c >= 0) lpar[s_parAt[c] + lc[c]++] = i;
			else { int w = wi.get(fm); if (w >= 0) wpar[s_wparAt[w] + wc[w]++] = i; }
		});
	{
		u32 *stack = (u32 *)rt::alloc(nl * 4 + 4), sp = 0;
		for (u32 i = 0; i < nl; i++) if (s_flag[i] & kVendor) { s_flag[i] |= kAbove; stack[sp++] = i; }
		while (sp)
		{
			u32 l = stack[--sp];
			for (u32 k = s_parAt[l]; k < s_parAt[l + 1]; k++) if (!(s_flag[lpar[k]] & kAbove)) { s_flag[lpar[k]] |= kAbove; stack[sp++] = lpar[k]; }
		}
		rt::free(stack);
	}
	u32 chests = 0;
	for (u32 i = 0; i < nh; i++)
	{
		u32 h = hold[i];
		u8 ty = formtype((void *)h);
		u32 inv = h + (ty == 0x1B ? 0x30 : 0x64) + 4;
		bool vend = false;
		if (ty == 0x1B) eachEntry(inv, 4, [&](u32 fm) { int l = li.get(fm); if (l >= 0 && (s_flag[l] & kAbove)) vend = true; });
		if (vend) { chests++; eachEntry(inv, 4, [&](u32 fm) { int l = li.get(fm); if (l >= 0) s_flag[l] |= kVendor; }); continue; }
		u8 mark = ty == 0x1B ? kConts : kChars;
		eachEntry(inv, 4, [&](u32 fm) { int l = li.get(fm); if (l >= 0) s_flag[l] |= mark; });
	}
	rt::free(hold);
	s_par = lpar; s_wpar = wpar;
	static struct Sum { u32 guns, vend, chars, conts, none; } sum[256];
	memset(sum, 0, sizeof sum);
	for (int pass = 0; pass < 2; pass++)
		for (u32 w = 0; w < nw; w++)
		{
			u32 fm = wfm[w], id = refid((void *)fm), m = id >> 24;
			u8 r = 0;
			u32 n = s_wparAt[w + 1] - s_wparAt[w];
			for (u32 k = s_wparAt[w]; k < s_wparAt[w + 1]; k++) r |= reach(s_par[k], 0);
			if (!pass)
			{
				Sum &s = sum[m]; s.guns++;
				if (r & kVendor) s.vend++;
				if (r & kChars) s.chars++;
				if (r & kConts) s.conts++;
				if (!n) s.none++;
				continue;
			}
			const char *nm = (const char *)ptr(fm + 0x34);
			if (!nm || !readable((u32)nm, 4)) nm = "";
			if (!n) LOG("loot:   %s %08X %s: in no list (placed by hand, a variant, or not handed out)", modName(m), id, nm);
			else LOG("loot:   %s %08X %s: %u list%s, %s, %s, %s", modName(m), id, nm, n, n == 1 ? "" : "s",
				(r & kVendor) ? "vendors" : "no vendor", (r & kChars) ? "carried" : "not carried", (r & kConts) ? "in containers" : "no containers");
		}
	for (u32 m = 0; m < 256; m++)
		if (sum[m].guns)
			LOG("loot: %s: %u guns - at vendors %u, carried by characters %u, in containers %u, in no list %u", modName(m),
				sum[m].guns, sum[m].vend, sum[m].chars, sum[m].conts, sum[m].none);
	LOG("loot: %u vendor chests; checked in %d ms", chests, (int)((rt::now() - t0) * 1000));
	li.drop(); wi.drop();
	rt::free(s_list); rt::free(s_par); rt::free(s_wpar); rt::free(s_parAt); rt::free(s_wparAt); rt::free(s_flag);
	rt::free(lc); rt::free(wc); rt::free(wfm); rt::free(s_rg); s_rg = 0; s_nrg = 0;
}
static void tick(double t)
{
	if (done) return;
	if (!at) { at = t + 30; return; }
	if (t < at) return;
	done = true;
	run();
}
}
static float g_baseFov = -1, g_baseIniFov, g_fovAdd, g_fovWrote = -1, g_fovStable = -1, g_fovSeen = -1;
static double g_fovSeenAt;
static void fovWatch(TESObjectREFR *pl)
{
	if (!pl || g_fovAdd != 0) return;
	float w = *(float *)((u32)pl + 0x670);
	double t = rt::now();
	if (rt::absf(w - g_fovSeen) > 0.05f) { g_fovSeen = w; g_fovSeenAt = t; }
	else if (t - g_fovSeenAt > 0.4 && !menumode() && w > 30 && w < 150) g_fovStable = w;
}
static void driftFov(TESObjectREFR *pl, float add)
{
	if (!pl) return;
	float *wf = (float *)((u32)pl + 0x670), *ini = (float *)(0x120315C + 4);
	u32 sg = ptr(0x11DEB7C);
	float *cam = sg ? (float *)(sg + 0xBC) : 0;
	if (g_fovAdd == 0)
	{
		if (add == 0 || menumode() || g_fovStable < 0 || rt::absf(*wf - g_fovStable) > 0.3f) return;
		g_baseFov = g_fovStable;
		g_baseIniFov = (*ini > 30 && *ini < 150) ? *ini : g_baseFov;
	}
	else if (rt::absf(*wf - g_fovWrote) > 0.3f)
	{
		static int lets;
		if (lets++ < 20) LOG("fov: the game took the view (world %.1f, ours %.1f) - letting go, default back to %.1f", *wf, g_fovWrote, g_baseIniFov);
		*ini = g_baseIniFov;
		g_fovAdd = 0; g_fovWrote = -1; g_baseFov = -1; g_fovStable = -1; g_fovSeen = -1;
		return;
	}
	static double logT;
	static int fovLogs;
	if (add > 0 && fovLogs < 60 && rt::now() - logT > 0.25)
	{
		logT = rt::now(); fovLogs++;
		LOG("drift fov: want %.1f, found world %.1f default %.1f camera %.1f", g_baseFov + add, *wf, *ini, cam ? *cam : -1.0f);
	}
	g_fovAdd = add;
	float fov = g_baseFov + add;
	*wf = fov;
	*ini = add == 0 ? g_baseIniFov : g_baseIniFov + add;
	g_fovWrote = add == 0 ? -1 : fov;
	(void)sg; (void)cam;
}
// A save keeps the view's FOV (PlayerCharacter worldFOV +0x670): one set with the console while tweaking came back with
// every load, over fDefaultWorldFOV.
static double g_fovFixAt;
static void fovDefault(TESObjectREFR *pl, double t)
{
	if (!g_fovFixAt || t < g_fovFixAt || !pl || menumode() || g_fovAdd != 0 || g_driving) return;
	u32 proc = ptr((u32)pl + 0x68);
	if (proc && rd<u32>(proc + 0x28) <= 1 && rd<u8>(proc + 0x228)) return;
	g_fovFixAt = 0;
	float *wf = (float *)((u32)pl + 0x670), ini = *(float *)(0x120315C + 4);
	if (ini < 30 || ini > 150 || rt::absf(*wf - ini) < 0.5f) return;
	LOG("fov: the save's view was %.1f - back to your setting %.1f (fDefaultWorldFOV)", *wf, ini);
	*wf = ini;
	g_fovStable = -1; g_fovSeen = -1;
}
static void driftFovOff(TESObjectREFR *pl)
{
	if (g_fovAdd != 0) driftFov(pl, 0);
	g_baseFov = -1; g_fovAdd = 0; g_fovWrote = -1;
}
// Leaning out (hold the lean key, the right mouse button)
static double g_leanLogT;
// (for the log) the first-person model while leaning out: its root and children, hidden or not, the gun out or not
static void leanLog()
{
	TESObjectREFR *pl = player();
	u32 n = firstPersonNode(pl);
	double out = -1;
	callNum(s_weaponOut, &out);
	char line[400];
	int k = rt::fmt(line, sizeof line, "lean: gun out %d, first person %d, 1st-person root %x flags %x:", (int)out, (int)!thirdPerson(pl), n, n ? rd<u32>(n + OFF_NIAV_FLAGS) : 0);
	u32 data = n ? ptr(n + OFF_NODE_CHILDREN) : 0;
	u16 cnt = n ? rd<u16>(n + OFF_NODE_CHILDCOUNT) : 0;
	for (u16 i = 0; data && i < cnt && i < 12 && k < 360; i++)
	{
		u32 c = ptr(data + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		k += rt::fmt(line + k, sizeof line - k, " %s(%x)", nm ? nm : "?", c ? rd<u32>(c + OFF_NIAV_FLAGS) & 0x100001 : 0);
	}
	LOG("%s", line);
}
static void leanTick(bool key, float dt)
{
	if (g_leanLogT > 0 && rt::now() > g_leanLogT) { g_leanLogT = 0; leanLog(); }
	if (key != g_lean)
	{
		g_lean = key;
		if (key)
		{
			console("EnablePlayerControls 0 0 1 0 0 0 0");
			if (g_leanBlend < 0.05f) { g_leanYaw = TUNE(leanStartYaw); g_leanPitch = 0; }
			double out = 0;
			if (callNum(s_weaponOut, &out) && out == 0) console("Player.SetAlert 1");
			g_leanLogT = rt::now() + 0.6;
		}
		else console("DisablePlayerControls 0 0 1 0 0 0 0");
	}
	if (g_lean) g_leanBlend = rt::min(1.0f, g_leanBlend + dt / rt::max(TUNE(leanInSeconds), 0.05f));
	else g_leanBlend = rt::max(0.0f, g_leanBlend - dt / rt::max(TUNE(leanOutSeconds), 0.05f));
}
static void placeRider(TESObjectREFR *pl, float dt)
{
	g_playerHidden = true;
	setFirstPerson(true);
	if (g_lean && g_leanBlend > 0.75f) showChildren(firstPersonNode(pl), g_hidden1st);
	else hideChildren(firstPersonNode(pl), g_hidden1st);
	hideChildren(thirdPersonNode(pl), g_hidden3rd);
	{
		u32 in = ptr(0x11F35CC);
		int mx = in ? rd<int>(in + 0x1B24) : 0, my = in ? rd<int>(in + 0x1B28) : 0;
		if (rd<int>(0x11E0A5C + 4)) my = -my;
		float sens = rd<float>(0x11E0A6C + 4);
		if (!(sens > 0.0001f && sens < 1.0f)) sens = 0.002f;
		float k = sens * 180.0f / rt::PI * TUNE(mouseLookScale);
		if (g_lean)
		{
			g_leanYaw = rt::clamp(wrap180(g_leanYaw + mx * k), -175, 90);
			g_leanPitch = rt::clamp(g_leanPitch + my * k, -50, 50);
		}
		else if (mx || my)
		{
			g_lookYaw = wrap180(g_lookYaw + mx * k);
			g_lookPitch = rt::clamp(g_lookPitch + my * k, -40, 55);
			g_lookIdle = 0;
		}
		else g_lookIdle += dt;
		if (g_lookIdle > TUNE(lookReturnSeconds) && rt::absf(g_car.speed) > 3 && !g_lean)
		{
			float k = rt::clamp(dt * 2.5f, 0, 1);
			g_lookYaw -= g_lookYaw * k;
			g_lookPitch -= g_lookPitch * k;
		}
	}
	static float driftCam;
	{
		float want = 0;
		if (g_sliding && rt::absf(g_car.speed) > TUNE(driftCamFromMs))
			want = rt::clamp((rt::absf(g_driftSlip) - TUNE(driftCamFromDegrees)) / TUNE(driftCamFullOverDegrees), 0, 1);
		driftCam += (want - driftCam) * rt::clamp(dt * (want > driftCam ? TUNE(driftCamInHz) : TUNE(driftCamOutHz)), 0, 1);
	}
	float aim = g_car.heading - (g_camMode == 0 ? g_driftSlip * TUNE(driftCamFollow) * driftCam : 0);
	float d = wrap180(aim - g_camHeading);
	g_camHeading += d * (g_camMode == 0 ? rt::clamp(dt * (iniF("chaseSwing", 5) + TUNE(driftCamSwingBoost) * driftCam), 0, 1) : 1.0f);
	if (g_camHeading >= 360) g_camHeading -= 360;
	if (g_camHeading < 0) g_camHeading += 360;
	float eye = iniF("playerEye", 120);
	static float camZ, camZVel;
	if (rt::absf(camZ - g_car.z) > 600) { camZ = g_car.z; camZVel = 0; }
	spring(camZ, camZVel, g_car.z, TUNE(cameraFollowHz), 1.0f, dt);
	float x, y, z, pitch, heading;
	if (g_camMode == 0)
	{
		float back = (g_car.halfLength + iniF("chaseDistance", 330)) * (1 + TUNE(driftCamPullBack) * driftCam);
		float up = iniF("chaseHeight", 135) - 55.0f - TUNE(driftCamLowerUnits) * driftCam;
		float dist = rt::sqrt(back * back + up * up);
		float elev = rt::clamp(rt::atan2(up, back) * 180.0f / rt::PI + g_lookPitch, -8, 65);
		heading = g_camHeading + g_lookYaw;
		float h = heading * rt::PI / 180.0f, e = elev * rt::PI / 180.0f;
		x = g_car.x - rt::sin(h) * rt::cos(e) * dist;
		y = g_car.y - rt::cos(h) * rt::cos(e) * dist;
		z = camZ + 55.0f + rt::sin(e) * dist;
		float gz = groundAt(x, y, z);
		float lowZ = camZ + TUNE(driftCamHeightUnits);
		if (driftCam > 0.001f) z += (lowZ - z) * driftCam;
		float floorAbove = 40 + (12 - 40) * driftCam;
		static float floorZ, floorVel;
		if (rt::absf(floorZ - gz) > 300) { floorZ = gz; floorVel = 0; }
		spring(floorZ, floorVel, gz, 6, 1.0f, dt);
		if (z < floorZ + floorAbove) z = floorZ + floorAbove;
		float lookZ = camZ + 55 - TUNE(driftCamLookDownUnits) * driftCam;
		pitch = rt::atan2(z - lookZ, rt::cos(e) * dist) * 180.0f / rt::PI;
	}
	else
	{
		float sx, sy, sz;
		float ep[3];
		if (g_camMode == 1) hoodEyePos(ep);
		else { seatEyePos(ep); ep[2] += TUNE(seatCameraLiftUnits); }
		sx = ep[0]; sy = ep[1]; sz = ep[2];
		float cp[3];
		carPoint(g_car, sx, sy, sz, cp);
		x = cp[0]; y = cp[1]; z = cp[2];
		g_lookYaw = rt::clamp(g_lookYaw, -165, 165);
		heading = g_car.heading + g_lookYaw;
		pitch = rt::clamp(-(g_car.pitch + g_car.wheelie) * g_pitchSign + iniF("cockpitPitch", 2) + g_lookPitch, -60, 70);
	}
	if (g_leanBlend > 0.001f)
	{
		float lp[3];
		float side = (g_activeCar >= 0 ? g_cars[g_activeCar].halfWidth : g_car.halfTrack + 16) + TUNE(leanOutUnits);
		float se[3]; seatEyePos(se);
		carPoint(g_car, -side, se[1] + 3, se[2] + TUNE(leanUpUnits), lp);
		float k = leanEase();
		x += (lp[0] - x) * k; y += (lp[1] - y) * k; z += (lp[2] - z) * k;
		z += rt::sin(k * rt::PI) * TUNE(leanArcUnits);
		float lh = g_car.heading + g_leanYaw;
		heading += wrap180(lh - heading) * k;
		pitch += (g_leanPitch - pitch) * k;
	}
	while (heading >= 360) heading -= 360;
	while (heading < 0) heading += 360;
	moveRef(pl, x, y, z - eye, pitch, 0, heading);
	{
		static float fovBlend;
		float want = g_camMode == 0 ? driftCam : 0;
		fovBlend += (want - fovBlend) * rt::clamp(dt * (want > fovBlend ? TUNE(driftCamFovInHz) : TUNE(driftCamFovOutHz)), 0, 1);
		float k = fovBlend < 0.002f ? 0 : fovBlend * fovBlend * (3 - 2 * fovBlend);
		driftFov(pl, TUNE(driftCamFovAdd) * k);
	}
}

// ---- People in the car (rider.cpp)
static u32 g_drvTpl, g_passTpl;
// The first seat takes the template itself (held by us, so it outlives the car's 3D); only a later seat (the car's 3D
// made again) copies it
static bool g_drvTplSeated, g_passTplSeated;
static u32 seatBody(u32 tpl, bool &seated)
{
	if (!tpl) return 0;
	if (!seated) { seated = true; return tpl; }
	return rider::copy(tpl);
}
static void copyTimes(const char *who)
{
	float m[5]; rider::lastCopyMs(m);
	float tot = m[0] + m[1] + m[2] + m[3] + m[4];
	if (tot > 30) LOG("riders: copying %s took %.0f ms (parts off %.0f, the game's clone %.0f, its fix-up %.0f, strip %.0f, prune %.0f)", who, tot, m[0], m[1], m[2], m[3], m[4]);
}
static bool g_friendSeat;
static rider::Body g_drv, g_pass;
static u32 g_riderCarNode;
static u32 g_passRef;
static double g_shiftT = -10;
static int g_shiftGear;

// Our nodes in your third-person skeleton - the shoulders' and the gun's turn, the left arm's reach - are put straight
// before any copy of it is made, so a copy never keeps a turn frozen in
namespace tpgun { static void identity(); static void apply(TESObjectREFR *pl, const float *aimT, float dt, bool aim, bool sneak, bool forFriends = false); static bool muzzle(float *pos, const float **rot); static bool barrel(float *pos, float *dir, const float *aimDir, bool tell); }
namespace move { static void armNeutral(); }
static void neutral3rd() { tpgun::identity(); move::armNeutral(); }
static u32 templateOf(u32 node, HiddenSet *hs)
{
	neutral3rd();
	u32 t = rider::copy(node);
	if (!t) return 0;
	rider::hold(t);
	if (hs && hs->n)
	{
		u32 od = ptr(node + OFF_NODE_CHILDREN), td = ptr(t + OFF_NODE_CHILDREN);
		u16 on = rd<u16>(node + OFF_NODE_CHILDCOUNT), tn = rd<u16>(t + OFF_NODE_CHILDCOUNT);
		for (u16 i = 0; od && td && i < on && i < tn && i < 256; i++)
			for (int k = 0; k < hs->n; k++)
				if (hs->node[k] == ptr(od + 4 * i)) { u32 c = ptr(td + 4 * i); if (c) *(u32 *)(c + OFF_NIAV_FLAGS) &= ~1u; }
	}
	return t;
}
static bool isTeammate(TESObjectREFR *a)
{
	if (!s_mate) return false;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_mate, player(), 0, &res, 1, a);
	return res.type == 1 && res.num != 0;
}
static void companionOut()
{
	TESObjectREFR *a = g_passRef ? (TESObjectREFR *)lookup(g_passRef) : 0;
	if (a)
	{
		float h = g_car.heading * rt::PI / 180.0f;
		float px = g_car.x + rt::cos(h) * (g_car.halfTrack + 70), py = g_car.y - rt::sin(h) * (g_car.halfTrack + 70);
		float pz = groundAt(px, py, g_car.z) + 10;
		char cmd[80];
		rt::fmt(cmd, sizeof cmd, "%x.Enable", g_passRef); console(cmd);
		rt::fmt(cmd, sizeof cmd, "%x.MoveTo player", g_passRef); console(cmd);
		rt::fmt(cmd, sizeof cmd, "%x.SetPos X %.1f", g_passRef, px); console(cmd);
		rt::fmt(cmd, sizeof cmd, "%x.SetPos Y %.1f", g_passRef, py); console(cmd);
		rt::fmt(cmd, sizeof cmd, "%x.SetPos Z %.1f", g_passRef, pz); console(cmd);
		LOG("riders: companion %x gets out (flags %x)", g_passRef, rd<u32>((u32)a + 8));
	}
	g_passRef = 0;
}
// Copies are never made of someone with a gun in the hand
static double g_drvWaitT = -1, g_passWaitT = -1;
static u32 g_passWaitRef;
static bool gunDrawn(u32 node)
{
	int pr[4];
	rider::scanSpecial(node, pr);
	return pr[3] > 0;
}
static void ridersDrop()
{
	memset(&g_drv, 0, sizeof g_drv); memset(&g_pass, 0, sizeof g_pass);
	g_riderCarNode = 0;
	if (g_drvTpl) { rider::release(g_drvTpl); g_drvTpl = 0; }
	if (g_passTpl) { rider::release(g_passTpl); g_passTpl = 0; }
	g_drvTplSeated = g_passTplSeated = false;
	g_drvWaitT = g_passWaitT = -1; g_passWaitRef = 0;
}
static void copyYou()
{
	TESObjectREFR *pl = player();
	{
		int pr[4]; rider::scanSpecial(thirdPersonNode(pl), pr);
		LOG("riders: copying you (your 3D has %d lights, %d particle systems, %d muzzle flashes, %d drawn guns)", pr[0], pr[1], pr[2], pr[3]);
	}
	CRUMB("riders: copying you");
	g_drvTpl = templateOf(thirdPersonNode(pl), &g_hidden3rd); g_drvTplSeated = false;
	CRUMB("riders: copied you");
	copyTimes("you");
	int pr[4]; rider::lastPruned(pr);
	if (pr[0] || pr[1] || pr[2] || pr[3]) LOG("riders: your copy leaves out %d lights, %d particle systems, %d muzzle flashes, %d drawn guns", pr[0], pr[1], pr[2], pr[3]);
}
static bool copyCompanion(TESObjectREFR *best)
{
	CRUMB("riders: copying your companion");
	g_passTpl = templateOf(thirdPersonNode(best), 0); g_passTplSeated = false;
	CRUMB("riders: copied your companion");
	copyTimes("your companion");
	{
		int pr[4]; rider::lastPruned(pr);
		if (pr[0] || pr[1] || pr[2] || pr[3]) LOG("riders: the companion's copy leaves out %d lights, %d particle systems, %d muzzle flashes, %d drawn guns", pr[0], pr[1], pr[2], pr[3]);
	}
	if (!g_passTpl) return false;
	g_passRef = refid(best);
	u32 before = rd<u32>((u32)best + 8);
	char cmd[48];
	rt::fmt(cmd, sizeof cmd, "%x.Disable", g_passRef);
	console(cmd);
	int out = 0, n = rider::skinCheck(g_passTpl, &out);
	LOG("riders: companion %x - flags %x -> %x (0x800: out of the world), copy has %d skinned meshes (%d on their own bones)",
		g_passRef, before, rd<u32>((u32)best + 8), n, out);
	return true;
}
// A copy made after the others were seated: in the car with them
static void seatLate(rider::Body &b, u32 tpl, const char *who)
{
	if (!g_riderCarNode || !tpl || b.root) return;
	u32 d = seatBody(tpl, tpl == g_drvTpl ? g_drvTplSeated : g_passTplSeated);
	if (!d) return;
	rider::attach(g_riderCarNode, d);
	if (!rider::bind(b, d)) LOG("riders: %s copy has no human skeleton", who);
}
namespace vibe { static void reset(); }
static void ridersBegin()
{
	ridersDrop();
	vibe::reset();
	g_shiftGear = g_gear; g_shiftT = -10;
	if (!TUNE(ridersOn)) return;
	TESObjectREFR *pl = player();
	if (gunDrawn(thirdPersonNode(pl)))
	{
		console("Player.SetAlert 0");
		g_drvWaitT = rt::now();
		LOG("riders: your gun is out - you're copied once it's put away");
	}
	else copyYou();
	if (g_passRef) companionOut();
	float *pp = pos(pl);
	TESObjectREFR *best = 0;
	float bestD = TUNE(ridersCompanionReachUnits) * TUNE(ridersCompanionReachUnits);
	forEachActor([&](TESObjectREFR *a) {
		if (a == pl || formtype(a) != 0x3B || rd<u32>((u32)a + OFF_ACTOR_LIFESTATE) != 0 || !thirdPersonNode(a)) return;
		float *ap = pos(a), dx = ap[0] - pp[0], dy = ap[1] - pp[1], d = dx * dx + dy * dy;
		if (d < bestD && isTeammate(a)) { best = a; bestD = d; }
	});
	if (best && g_activeCar >= 0 && kCars[g_activeCar].seats == 1)
	{
		LOG("riders: single-seater - companion %x stays behind", refid(best));
		best = 0;
	}
	if (best)
	{
		if (gunDrawn(thirdPersonNode(best)))
		{
			g_passWaitRef = refid(best); g_passWaitT = rt::now();
			LOG("riders: companion %x has a gun out - they get in once it's put away", g_passWaitRef);
		}
		else copyCompanion(best);
	}
	LOG("riders: you %s; companion %x %s", g_drvTpl ? "copied" : g_drvWaitT >= 0 ? "waiting" : "NOT copied", g_passRef ? g_passRef : g_passWaitRef,
		g_passRef ? "gets in" : g_passWaitRef ? "waiting" : "(none close by)");
}
// The copies waiting on a gun being put away
static void ridersWaitTick()
{
	double now = rt::now();
	if (g_drvWaitT >= 0)
	{
		TESObjectREFR *pl = player();
		if (!gunDrawn(thirdPersonNode(pl))) { g_drvWaitT = -1; copyYou(); seatLate(g_drv, g_drvTpl, "your"); }
		else if (now - g_drvWaitT > 1.5) { g_drvWaitT = -1; LOG("riders: your gun stayed out - copied without it"); copyYou(); seatLate(g_drv, g_drvTpl, "your"); }
	}
	if (g_passWaitT >= 0)
	{
		TESObjectREFR *a = g_passWaitRef ? (TESObjectREFR *)lookup(g_passWaitRef) : 0;
		u32 n = a ? thirdPersonNode(a) : 0;
		if (!n) { LOG("riders: companion %x gone - they stay behind", g_passWaitRef); g_passWaitT = -1; g_passWaitRef = 0; }
		else if (!gunDrawn(n) || now - g_passWaitT > 3)
		{
			if (gunDrawn(n)) LOG("riders: companion %x keeps their weapon out - copied without it", g_passWaitRef);
			g_passWaitT = -1; g_passWaitRef = 0;
			if (copyCompanion(a)) seatLate(g_pass, g_passTpl, "the companion's");
		}
	}
}
static void ridersEnd()
{
	ridersDrop();
	if (g_passRef) companionOut();
}
// The car's 3D is replaced (respawnIfFar): the copies move across to the new one while the old is still there
static void ridersMoveTo(u32 oldRef, TESObjectREFR *n)
{
	u32 oldRoot = thirdPersonNode(lookup(oldRef)), newRoot = n ? thirdPersonNode(n) : 0;
	if (!newRoot || !oldRoot || oldRoot != g_riderCarNode) return;
	rider::Body *bs[2] = {&g_drv, &g_pass};
	for (int i = 0; i < 2; i++)
	{
		u32 r = bs[i]->root;
		if (!r) continue;
		rider::hold(r);
		rider::detach(oldRoot, r);
		rider::attach(newRoot, r);
		rider::release(r);
	}
	g_riderCarNode = newRoot;
}
// The companion vibing.
namespace vibe
{
static const int kRate = 50, kLen = 300;
static float on[kLen];
static int n, w;
static float acc, slowE, lastE, calcT, bpm = 0, conf, phase, nodAmp, lastO, lastO2;
static int cand, candHits;
static u32 seed = 0x2545F491;
static float rnd() { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 9) & 0xFFFF) / 65536.0f; }
static int lookState;
static float lookT, lookDur, lookHold, nextLook = 8, fromYaw, fromPitch, toYaw, toPitch, yaw, pitch;
static float ease(float x) { x = rt::clamp(x, 0, 1); return x * x * x * (x * (x * 6 - 15) + 10); }
static float log2f_(float x)
{
	if (x <= 1e-9f) return -30;
	union { float f; u32 u; } v;
	v.f = x;
	float e = (float)(int)((v.u >> 23) & 255) - 127;
	v.u = (v.u & 0x007FFFFF) | 0x3F800000;
	float m = v.f;
	return e + (-0.33333f * m + 2.0f) * m - 1.66667f;
}
static void reset() { n = w = 0; acc = slowE = lastE = calcT = 0; bpm = conf = phase = nodAmp = 0; candHits = 0; lookState = 0; yaw = pitch = 0; nextLook = 6 + 6 * rnd(); }
static void estimate()
{
	if (n < kLen * 2 / 3) { conf = 0; return; }
	int lo = kRate * 60 / 180, hi = kRate * 60 / 70;
	float best = 0, sum = 0, ac[64];
	int bl = 0;
	float energy = 0;
	for (int i = 0; i < n; i++) { float v = on[(w - 1 - i + kLen) % kLen]; energy += v * v; }
	if (energy < 1e-6f) { conf = 0; return; }
	for (int L = lo; L <= hi && L - lo < 64; L++)
	{
		float a = 0;
		for (int i = 0; i + L < n; i++) a += on[(w - 1 - i + kLen) % kLen] * on[(w - 1 - i - L + 2 * kLen) % kLen];
		a /= energy;
		float b = 60.0f * kRate / L, oct = rt::absf(log2f_(b / 115.0f));
		float wgt = 1.0f / (1.0f + 2.5f * oct * oct);
		ac[L - lo] = a * wgt;
		sum += ac[L - lo];
		if (ac[L - lo] > best) { best = ac[L - lo]; bl = L; }
	}
	int cnt = hi - lo + 1;
	float mean = sum / cnt;
	conf = rt::clamp((best - mean) / (mean + 0.02f) * 0.5f, 0, 1);
	float fl = (float)bl;
	if (bl > lo && bl < hi)
	{
		float a0 = ac[bl - 1 - lo], a1 = ac[bl - lo], a2 = ac[bl + 1 - lo], den = a0 - 2 * a1 + a2;
		if (rt::absf(den) > 1e-6f) fl += rt::clamp(0.5f * (a0 - a2) / den, -0.5f, 0.5f);
	}
	float nb = 60.0f * kRate / fl;
	if (bpm <= 0 || rt::absf(nb - bpm) / bpm < 0.05f) { bpm = bpm <= 0 ? nb : bpm + (nb - bpm) * 0.3f; candHits = 0; }
	else if (rt::absf((float)cand - nb) / nb < 0.05f) { if (++candHits >= 2) { bpm = nb; candHits = 0; } }
	else { cand = (int)nb; candHits = 1; }
}
static void tick(float dt, bool playing, float *outYaw, float *outPitch, float *outRoll)
{
	dt = rt::clamp(dt, 0, 0.1f);
	float e = playing ? rt::clamp(audio::bassEnv(), 0, 1.5f) : 0;
	acc += dt;
	while (acc >= 1.0f / kRate)
	{
		acc -= 1.0f / kRate;
		slowE += (e - slowE) * (1.0f / kRate / 0.25f);
		float o = rt::max(0, e - slowE * 1.08f);
		on[w] = o; w = (w + 1) % kLen; if (n < kLen) n++;
		if (lastO > 0.02f && lastO >= lastO2 && lastO > o && conf > 0.25f && bpm > 0)
		{
			float err = phase - (float)(int)(phase + 0.5f);
			phase -= err * 0.18f;
		}
		lastO2 = lastO; lastO = o;
	}
	calcT += dt;
	if (calcT >= 0.5f) { calcT = 0; estimate(); }
	if (bpm > 0) { phase += dt * bpm / 60.0f; if (phase > 1000) phase -= 1000; }
	static float level;
	level += (e - level) * rt::clamp(dt * 0.7f, 0, 1);
	float want = playing && bpm > 0 && level > 0.03f ? rt::clamp(0.35f + 0.65f * conf, 0, 1) : 0;
	nodAmp += (want - nodAmp) * rt::clamp(dt * 0.8f, 0, 1);
	float ph = phase - (float)(int)phase;
	float dip = 0.5f + 0.5f * rt::cos(2 * rt::PI * ph);
	dip = dip * dip * (3 - 2 * dip);
	float half = rt::sin(rt::PI * phase);
	float nodDeg = TUNE(vibeNodDegrees) * nodAmp, swayDeg = TUNE(vibeSwayDegrees) * nodAmp;
	if (lookState == 0)
	{
		nextLook -= dt;
		if (nextLook <= 0)
		{
			float r = rnd(), ym = TUNE(vibeLookYawDegrees);
			if (r < 0.45f) { toYaw = -(0.55f + 0.45f * rnd()) * ym; toPitch = -2 + 5 * rnd(); }
			else if (r < 0.7f) { toYaw = (0.35f + 0.35f * rnd()) * ym; toPitch = 1 + 4 * rnd(); }
			else if (r < 0.88f) { toYaw = (rnd() - 0.5f) * 0.3f * ym; toPitch = -4 + 3 * rnd(); }
			else { toYaw = (0.15f + 0.2f * rnd()) * ym; toPitch = 10 + 6 * rnd(); }
			toYaw *= rt::PI / 180.0f; toPitch *= rt::PI / 180.0f;
			fromYaw = yaw; fromPitch = pitch;
			lookState = 1; lookT = 0;
			lookDur = TUNE(vibeLookTurnSeconds) * (0.8f + 0.5f * rnd());
			lookHold = 1.5f + 3.0f * rnd();
		}
	}
	else
	{
		lookT += dt;
		float k = ease(lookT / lookDur);
		if (lookState == 1) { yaw = fromYaw + (toYaw - fromYaw) * k; pitch = fromPitch + (toPitch - fromPitch) * k; if (lookT >= lookDur) { lookState = 2; lookT = 0; } }
		else if (lookState == 2) { if (lookT >= lookHold) { lookState = 3; lookT = 0; fromYaw = yaw; fromPitch = pitch; lookDur *= 1.15f; } }
		else { yaw = fromYaw * (1 - k); pitch = fromPitch * (1 - k); if (lookT >= lookDur) { lookState = 0; yaw = pitch = 0; nextLook = TUNE(vibeLookMinSeconds) + (TUNE(vibeLookMaxSeconds) - TUNE(vibeLookMinSeconds)) * rnd(); } }
	}
	float away = rt::clamp(rt::absf(yaw) / (30 * rt::PI / 180.0f), 0, 1);
	*outYaw = yaw;
	*outPitch = pitch + nodDeg * (1 - 0.4f * away) * dip * rt::PI / 180.0f;
	*outRoll = swayDeg * half * rt::PI / 180.0f;
}
}
static void ridersTick(float dt)
{
	ridersWaitTick();
	TESObjectREFR *car = vref(g_car);
	u32 root = car ? thirdPersonNode(car) : 0;
	if (!root || (!g_drvTpl && !g_passTpl)) return;
	if (!ptr(root + 0x18) && g_riderCarNode && g_car.oldRef && thirdPersonNode(lookup(g_car.oldRef)) == g_riderCarNode) root = g_riderCarNode;
	if (root != g_riderCarNode && ptr(root + 0x18) && g_car.oldRef && thirdPersonNode(lookup(g_car.oldRef)) == g_riderCarNode)
	{
		ridersMoveTo(g_car.oldRef, car);
		setHidden(lookup(g_car.oldRef), true);
	}
	if (root != g_riderCarNode)
	{
		g_riderCarNode = root;
		if (g_drv.root && g_drv.root == g_drvTpl) rider::unpose(g_drv);
		if (g_pass.root && g_pass.root == g_passTpl) rider::unpose(g_pass);
		memset(&g_drv, 0, sizeof g_drv); memset(&g_pass, 0, sizeof g_pass);
		u32 d = seatBody(g_drvTpl, g_drvTplSeated), c = seatBody(g_passTpl, g_passTplSeated);
		if (d) { rider::attach(root, d); if (!rider::bind(g_drv, d)) LOG("riders: your copy has no human skeleton"); }
		if (c) { rider::attach(root, c); if (!rider::bind(g_pass, c)) LOG("riders: the companion's copy has no human skeleton"); }
		static int seatLogs;
		if (seatLogs < 3)
		{
			seatLogs++;
			int out = 0, n = d ? rider::skinCheck(d, &out) : 0;
			LOG("riders: seated in car 3D %x - you %x (%d skinned meshes, %d still on your own bones), companion %x", root, d, n, out, c);
		}
	}
	rider::Pose p;
	memset(&p, 0, sizeof p);
	seatEyePos(p.eye);
	p.side = -1;
	p.floorZ = cabinFloor();
	p.recline = seatRecline();
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < n && i < 256; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm || rt::strcmp(nm, "MMWCockpitWheel")) continue;
		const float *ax = g_car.steerAxis;
		if (ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2] < 0.5f) break;
		const float *lt = (const float *)(c + 0x58);
		u32 bound = ptr(c + 0x20);
		float rs = rd<float>(root + 0x98), br = bound ? rd<float>(bound + 0xC) : 0;
		p.wheel = true;
		for (int k = 0; k < 3; k++) { p.wheelC[k] = lt[k]; p.wheelA[k] = ax[k]; }
		p.wheelR = rt::clamp(br / (rs > 0.01f ? rs : 1) * TUNE(ridersWheelRimScale), 8, 18);
		float ratio = TUNE(steeringWheelDegreesAtLock) / rt::max(TUNE(maxSteerDegrees), 1);
		p.wheelAngle = -g_car.steer * ratio * g_wheelSteerSign * TUNE(steeringWheelSign) * rt::PI / 180.0f;
		p.handsFollow = TUNE(ridersHandsFollowDegrees) * rt::PI / 180.0f;
		break;
	}
	const CarDef *cdl = cockpitRow();
	if (cdl && cdl->lever[2] > 0) { p.lever[0] = cdl->lever[0]; p.lever[1] = cdl->lever[1]; p.lever[2] = cdl->lever[2]; }
	else { p.lever[0] = p.eye[0] * 0.15f; p.lever[1] = p.eye[1] + 22; p.lever[2] = p.floorZ + 26; }
	if (g_gear != g_shiftGear) { if (g_gear > 0 && g_shiftGear > 0) g_shiftT = rt::now(); g_shiftGear = g_gear; }
	float st = (float)(rt::now() - g_shiftT), dur = TUNE(ridersShiftSeconds), sh = 0;
	if (st >= 0 && st < dur) sh = st < dur * 0.27f ? st / (dur * 0.27f) : st < dur * 0.6f ? 1 : 1 - (st - dur * 0.6f) / (dur * 0.4f);
	p.shift = g_leanBlend > 0.05f ? 0 : sh * sh * (3 - 2 * sh);
	p.hideHead = g_camMode == 2 || g_leanBlend > 0.3f;
	p.lean = leanEase() * TUNE(leanBodyDegrees) * rt::PI / 180.0f;
	p.curl = TUNE(ridersFingerCurlDegrees);
	if (g_drv.root) { rider::pose(g_drv, p); updateNode(g_drv.root); }
	if (g_pass.root)
	{
		p.eye[0] = -p.eye[0]; p.side = 1; p.wheel = false; p.shift = 0; p.hideHead = false; p.lean = 0;
		vibe::tick(dt, (g_radioOn || g_musicOn) && TUNE(vibeOn) > 0, &p.headYaw, &p.headPitch, &p.headRoll);
		rider::pose(g_pass, p);
		u32 &pf = *(u32 *)(g_pass.root + OFF_NIAV_FLAGS);
		pf = g_friendSeat ? (pf | 1u) : (pf & ~1u);
		updateNode(g_pass.root);
	}
	static u32 passLogged;
	if (g_pass.root && passLogged != g_pass.root)
	{
		passLogged = g_pass.root;
		float hd[3], pv[3];
		rider::bonePos(g_pass, rider::HEAD, hd); rider::bonePos(g_pass, rider::PELVIS, pv);
		LOG("riders: companion posed - head %.0f %.0f %.0f, pelvis %.0f %.0f %.0f, copy flags %x, parent %x (car 3D %x)",
			hd[0], hd[1], hd[2], pv[0], pv[1], pv[2], rd<u32>(g_pass.root + 0x30), ptr(g_pass.root + 0x18), root);
	}
	static int poseLogs;
	if (g_drv.root && poseLogs < 2)
	{
		poseLogs++;
		float hd[3], pv[3], lh[3], lf[3];
		rider::bonePos(g_drv, rider::HEAD, hd); rider::bonePos(g_drv, rider::PELVIS, pv); rider::bonePos(g_drv, rider::L_HAND, lh); rider::bonePos(g_drv, rider::L_FOOT, lf);
		LOG("riders: posed - head %.0f %.0f %.0f, pelvis %.0f %.0f %.0f, left hand %.0f %.0f %.0f, left foot %.0f %.0f %.0f; wheel %s at %.0f %.0f %.0f r %.1f",
			hd[0], hd[1], hd[2], pv[0], pv[1], pv[2], lh[0], lh[1], lh[2], lf[0], lf[1], lf[2], p.wheel ? "found" : "not found", p.wheelC[0], p.wheelC[1], p.wheelC[2], p.wheelR);
	}
}

// ---- frame
static void afterLoad()
{
	LOG("after load: active car %d, parked %x, resume %d", g_activeCar, g_parkedRef, g_resume);
	if (g_passRef && !g_resume)
	{
		char cmd[48];
		rt::fmt(cmd, sizeof cmd, "%x.Enable", g_passRef); console(cmd);
		rt::fmt(cmd, sizeof cmd, "%x.MoveTo player", g_passRef); console(cmd);
		g_passRef = 0;
	}
	for (int i = 0; i < 6; i++) if (g_loadedCops[i]) { removeRef((TESObjectREFR *)lookup(g_loadedCops[i])); g_loadedCops[i] = 0; }
	for (int i = 0; i < 4; i++) if (g_loadedBeams[i]) { removeRef((TESObjectREFR *)lookup(g_loadedBeams[i])); g_loadedBeams[i] = 0; }
	for (int i = 0; i < 8; i++) if (g_loadedOldDisplay[i]) { removeRef((TESObjectREFR *)lookup(g_loadedOldDisplay[i])); g_loadedOldDisplay[i] = 0; }
	if (g_loadedPreview) { removeRef((TESObjectREFR *)lookup(g_loadedPreview)); g_loadedPreview = 0; }
	if (g_activeCar >= 0 && g_activeCar < kNumCars && !g_cars[g_activeCar].available)
	{
		LOG("after load: your %s isn't here now (cars off, or not built) - its parked copy %x taken away", kCars[g_activeCar].id, g_parkedRef);
		if (g_parkedRef) { TESObjectREFR *pk = (TESObjectREFR *)lookup(g_parkedRef); if (pk) removeRef(pk); g_parkedRef = 0; }
		g_activeCar = -1; g_resume = false;
	}
	strayCleanup("after load");
	worldCarsRestore();
	if (g_liveRelease) { console("EnablePlayerControls"); g_liveRelease = false; }
	if (g_resume)
	{
		g_resume = false;
		TESObjectREFR *d = g_activeCar >= 0 && g_activeCar < kNumCars ? vref(g_car) : 0;
		if (d)
		{
			float *p = pos(d);
			g_car.x = p[0]; g_car.y = p[1]; g_car.z = p[2]; g_car.spawnX = p[0]; g_car.spawnY = p[1];
			g_car.speed = 0; g_car.vz = 0;
			g_car.halfTrack = g_cars[g_activeCar].halfTrack; g_car.halfBase = g_cars[g_activeCar].halfBase; g_car.halfLength = g_cars[g_activeCar].halfLength;
			g_car.front = g_cars[g_activeCar].front; g_car.rear = g_cars[g_activeCar].rear; g_car.clearance = g_cars[g_activeCar].clearance;
			g_car.wheelRadius = g_cars[g_activeCar].wheelRadius;
			g_car.low = kCars[g_activeCar].drive;
			startDriving(g_activeCar, g_car);
			LOG("resumed driving %s", kCars[g_activeCar].id);
			g_needRelease = false;
			return;
		}
	}
	if (g_needRelease) driveMode(false);
	g_needRelease = false;
}

// settings.ini borderless=1: a windowed game becomes a borderless window filling its screen (from the main menu on)
static void borderlessTick()
{
	static int state;
	static double lastT;
	if (state == 2) return;
	double t = rt::now();
	if (t - lastT < 1.0) return;
	lastT = t;
	if (!state)
	{
		char p[320];
		rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\settings.ini", g_dir);
		char *b = rt::readfile(p, 0);
		bool on = false;
		if (b)
		{
			for (char *q = b; *q; q++)
				if (rt::startswith(q, "borderless=")) on = rt::parsei(q + 11) == 1;
			rt::free(b);
		}
		g_borderless = on ? 1 : 0;
		state = on ? 1 : 2;
		if (!on) return;
	}
	char m[200];
	if (mirror::makeBorderless(m, sizeof m)) { state = 2; LOG("%s", m); }
}
// ---- Snow (after test 81: "Make it snow and
namespace nights
{
enum { MAXW = 320, MAXI = 96 };
struct Saved { u32 form; u8 col[10][6][4]; u8 cloud[4][6][4]; };
static Saved saved[MAXW];
static int nSaved = -1;
struct SavedIms { u32 form; float traits[33]; };
static SavedIms savedIms[MAXI];
static int nIms;
static bool realism = true;
static const u8 kMidnight[10][3] = {{2, 3, 6}, {4, 5, 8}, {3, 3, 5}, {6, 7, 10}, {14, 16, 24}, {0, 0, 0}, {0, 0, 0}, {3, 4, 7}, {4, 5, 8}, {3, 3, 5}};
static float darkness() { return rt::clamp(TUNE(nightDarkness), 0, 1); }
static void mix(u8 *c, const u8 *o, float mul, const float *to, float k)
{
	float lo = (0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2]) * mul, lt = 0.299f * to[0] + 0.587f * to[1] + 0.114f * to[2];
	float s = lt > 1 ? lo / lt : 0;
	for (int i = 0; i < 3; i++) c[i] = (u8)rt::clamp((o[i] * mul) * (1 - k) + to[i] * s * k, 0, 255);
}
static void applyAll()
{
	float d = darkness(), r = realism ? rt::clamp(TUNE(lightRealism), 0, 1) : 0;
	for (int n = 0; n < nSaved; n++)
	{
		Saved &w = saved[n];
		u32 f = w.form;
		u8 *col = (u8 *)(f + 0x108), *cl = (u8 *)(f + 0x68);
		memcpy(col, w.col, sizeof w.col);
		memcpy(cl, w.cloud, sizeof w.cloud);
		static const int kDayTimes[4] = {0, 1, 2, 4};
		if (r > 0)
			for (int ti = 0; ti < 4; ti++)
			{
				int t = kDayTimes[ti];
				bool golden = t == 0 || t == 2;
				auto at = [&](int ty) { return col + (ty * 6 + t) * 4; };
				auto orig = [&](int ty) { return &w.col[ty][t][0]; };
				static const float skyFill[3] = {118, 140, 175}, duskFill[3] = {150, 120, 140};
				static const float sunDay[3] = {255, 246, 228}, sunNoon[3] = {255, 250, 240}, sunGold[3] = {255, 168, 92};
				static const float hazeDay[3] = {185, 200, 215}, hazeGold[3] = {240, 170, 120};
				static const float skyDay[3] = {64, 116, 186}, skyGold[3] = {90, 90, 150}, lowDay[3] = {150, 180, 210};
				mix(at(3), orig(3), 1 - 0.22f * r, golden ? duskFill : skyFill, 0.18f * r);
				mix(at(4), orig(4), golden ? 1 + 0.08f * r : t == 4 ? 1 + 0.05f * r : 1, golden ? sunGold : t == 4 ? sunNoon : sunDay, (golden ? 0.45f : t == 4 ? 0.08f : 0.12f) * r);
				mix(at(1), orig(1), 1, golden ? hazeGold : hazeDay, (golden ? 0.35f : 0.18f) * r);
				mix(at(8), orig(8), 1, golden ? hazeGold : hazeDay, (golden ? 0.35f : 0.18f) * r);
				mix(at(0), orig(0), 1, golden ? skyGold : skyDay, (golden ? 0.2f : 0.15f) * r);
				if (!golden) mix(at(7), orig(7), 1, lowDay, 0.1f * r);
			}
		if (d <= 0) continue;
		float keep = 1 - (1 - rt::clamp(TUNE(nightKeep), 0, 1)) * d;
		for (int ty = 0; ty < 10; ty++)
		{
			if (ty == 5 || ty == 6) continue;
			for (int c = 0; c < 3; c++)
			{
				float o3 = w.col[ty][3][c], o5 = w.col[ty][5][c];
				col[(ty * 6 + 3) * 4 + c] = (u8)rt::clamp(o3 * keep, 0, 255);
				col[(ty * 6 + 5) * 4 + c] = (u8)rt::clamp(o5 + (kMidnight[ty][c] - o5) * d, 0, 255);
			}
		}
		for (int l = 0; l < 4; l++)
			for (int c = 0; c < 3; c++)
			{
				float o3 = w.cloud[l][3][c], o5 = w.cloud[l][5][c];
				cl[(l * 6 + 3) * 4 + c] = (u8)rt::clamp(o3 * keep, 0, 255);
				cl[(l * 6 + 5) * 4 + c] = (u8)rt::clamp(o5 + (3 - o5) * d, 0, 255);
			}
	}
	for (int n = 0; n < nIms; n++)
	{
		float *tr = (float *)(savedIms[n].form + 0x18);
		const float *o = savedIms[n].traits;
		memcpy(tr, o, sizeof savedIms[n].traits);
		if (r <= 0) continue;
		tr[17] = o[17] * (1 - TUNE(lightBloomCut) * r);
		tr[16] = o[16] * (1 - TUNE(lightBloomCut) * 0.7f * r);
		tr[0] = o[0] * (1 - 0.3f * r);
		if (o[25] > 0.01f) tr[25] = o[25] * (1 + TUNE(lightSaturation) * r);
		if (o[27] > 0.01f) tr[27] = o[27] * (1 + TUNE(lightContrast) * r);
	}
}
static void once()
{
	if (nSaved >= 0) return;
	nSaved = 0;
	u32 map = ptr(ADDR_FORM_MAP);
	u32 buckets = map ? rd<u32>(map + 4) : 0, table = map ? ptr(map + 8) : 0;
	for (u32 b = 0; table && b < buckets; b++)
		for (u32 e = ptr(table + 4 * b); e; e = ptr(e))
		{
			u32 f = ptr(e + 8);
			if (!f) continue;
			u8 ty = formtype((void *)f);
			if (ty == 0x35 && nSaved < MAXW)
			{
				Saved &w = saved[nSaved++];
				w.form = f;
				memcpy(w.col, (void *)(f + 0x108), sizeof w.col);
				memcpy(w.cloud, (void *)(f + 0x68), sizeof w.cloud);
			}
			else if (ty == 0x53 && nIms < MAXI)
			{
				savedIms[nIms].form = f;
				memcpy(savedIms[nIms].traits, (void *)(f + 0x18), sizeof savedIms[nIms].traits);
				nIms++;
			}
		}
	applyAll();
	if (nSaved)
	{
		const Saved &w = saved[0];
		const u8 *c = (const u8 *)(w.form + 0x108);
		LOG("nights: %d weathers (darkness %.2f, night keeps %.2f), %d image spaces; realistic daylight %.2f; first: ambient day %d %d %d -> %d %d %d, sunlight sunset %d %d %d -> %d %d %d, midnight %d %d %d -> %d %d %d",
			nSaved, darkness(), TUNE(nightKeep), nIms, TUNE(lightRealism), w.col[3][1][0], w.col[3][1][1], w.col[3][1][2], c[(3 * 6 + 1) * 4], c[(3 * 6 + 1) * 4 + 1], c[(3 * 6 + 1) * 4 + 2],
			w.col[4][2][0], w.col[4][2][1], w.col[4][2][2], c[(4 * 6 + 2) * 4], c[(4 * 6 + 2) * 4 + 1], c[(4 * 6 + 2) * 4 + 2],
			w.col[3][5][0], w.col[3][5][1], w.col[3][5][2], c[(3 * 6 + 5) * 4], c[(3 * 6 + 5) * 4 + 1], c[(3 * 6 + 5) * 4 + 2]);
	}
	else LOG("nights: no weathers found");
}
static void toggle()
{
	realism = !realism;
	applyAll();
	u32 sky = ptr(0x11DEA20), w = sky ? ptr(sky + 0x10) : 0;
	if (w)
	{
		char cmd[48];
		rt::fmt(cmd, sizeof cmd, "ForceWeather %08X 0", refid((void *)w));
		console(cmd);
	}
	message(realism ? "Realistic lighting on (F11)" : "Realistic lighting off (F11): the game's own", 2);
	LOG("nights: realistic daylight %s", realism ? "on" : "off");
}
static float daylight()
{
	TESForm *g = lookup(0x38);
	float h = g ? rd<float>((u32)g + 0x24) : 12;
	if (!(h >= 0 && h <= 24)) h = 12;
	float x = h < 12 ? h : 24 - h;
	float k = rt::clamp((x - 0.5f) / 5.5f, 0, 1);
	return k * k * (3 - 2 * k);
}
static float now() { return darkness() * (1 - daylight()); }
}

// ---- (plugin 209, 2026-10-18, the author: "working seasons, snow and snowing weather, and snow building up in the
// world
namespace wintertex
{
struct Ent { u32 hash; u16 w, h; u8 mips; u32 bytes; char path[100]; };
static Ent *ents;
static int nEnt, cap2;
static int *slot;
static bool loaded, live = true, liveChecked;
static u32 pathOff = 0x30, anchor;
static volatile int linkWant = -1, linkDone = -1, linkFail;
static int want = -1;
static double nextWalk;
struct Job { u32 tex; int ent, stage; char *buf; u32 len; volatile int state; };
static const int kJobs = 1024;
static Job jobs[kJobs];
static volatile int nJobs, nRead;
static int nDone;
static volatile long jlock;
static int (__stdcall *pLink)(const char *, const char *, void *);
static int (__stdcall *pMove)(const char *, const char *, u32);
static bool started;
static u32 hashOf(const char *p) { u32 h = 2166136261u; for (; *p; p++) { char c = *p; if (c >= 'A' && c <= 'Z') c += 32; if (c == '/') c = '\\'; h = (h ^ (u8)c) * 16777619u; } return h; }
static int find(const char *p)
{
	if (!nEnt) return -1;
	u32 h = hashOf(p);
	for (int i = h & (cap2 - 1), g = 0; g < cap2; i = (i + 1) & (cap2 - 1), g++)
	{
		if (slot[i] < 0) return -1;
		if (ents[slot[i]].hash == h) return slot[i];
	}
	return -1;
}
static void load()
{
	loaded = true;
	char p[400];
	rt::fmt(p, sizeof p, "%sMojaveMostWanted\\winter\\list.txt", g_dir);
	u32 len = 0;
	char *b = rt::readfile(p, &len);
	if (!b) { LOG("winter: no stage list (%s) - the ground's snow follows only at the next Play", p); return; }
	int lines = 0;
	for (u32 i = 0; i < len; i++) if (b[i] == '\n') lines++;
	ents = (Ent *)rt::alloc((lines + 1) * sizeof(Ent));
	for (char *q = b; *q;)
	{
		char *e = q; while (*e && *e != '\n') e++;
		char save = *e; *e = 0;
		if (*q == '#')
		{
			for (char *f = q; *f; f++)
				if (rt::startswith(f, "linked at stage ")) { int L = rt::parsei(f + 16); if (L >= 0 && L < 4) { linkDone = linkWant = want = L; } break; }
		}
		else if (*q)
		{
			Ent &n = ents[nEnt];
			int k = 0; char *f = q;
			while (*f && *f != '\t' && k < 99) n.path[k++] = *f++;
			n.path[k] = 0;
			if (*f == '\t')
			{
				f++; n.w = (u16)rt::parsei(f); while (*f && *f != '\t') f++; if (*f) f++;
				n.h = (u16)rt::parsei(f); while (*f && *f != '\t') f++; if (*f) f++;
				n.mips = (u8)rt::parsei(f); while (*f && *f != '\t') f++; if (*f) f++;
				n.bytes = (u32)rt::parsei(f);
				n.hash = hashOf(n.path);
				if (n.w && n.h) nEnt++;
			}
		}
		*e = save;
		q = *e ? e + 1 : e;
	}
	rt::free(b);
	cap2 = 64; while (cap2 < nEnt * 2) cap2 <<= 1;
	slot = (int *)rt::alloc(cap2 * 4);
	for (int i = 0; i < cap2; i++) slot[i] = -1;
	for (int i = 0; i < nEnt; i++)
	{
		int j = ents[i].hash & (cap2 - 1);
		while (slot[j] >= 0) j = (j + 1) & (cap2 - 1);
		slot[j] = i;
	}
	LOG("winter: %d landscape textures in stages", nEnt);
}
static void jl() { while (__sync_lock_test_and_set(&jlock, 1)) Sleep(0); }
static void ju() { __sync_lock_release(&jlock); }
static u32 __stdcall worker(void *)
{
	char src[400], dst[400], tmp[420];
	int at = 0, for_ = -1, n = 0, fails = 0;
	for (;;)
	{
		if (nRead < nJobs)
		{
			Job &j = jobs[nRead];
			rt::fmt(src, sizeof src, "%sMojaveMostWanted\\winter\\s%d\\%s", g_dir, j.stage, ents[j.ent].path);
			u32 len = 0;
			char *b = rt::readfile(src, &len);
			j.buf = b; j.len = len;
			j.state = b ? 1 : 2;
			__sync_synchronize();
			nRead = nRead + 1;
			continue;
		}
		int w = linkWant;
		if (w >= 0 && w != linkDone && pLink && pMove)
		{
			if (w != for_) { for_ = w; at = n = fails = 0; }
			for (int k = 0; k < 32 && at < nEnt; k++, at++)
			{
				rt::fmt(src, sizeof src, "%sMojaveMostWanted\\winter\\s%d\\%s", g_dir, w, ents[at].path);
				rt::fmt(dst, sizeof dst, "%sData\\%s", g_dir, ents[at].path);
				rt::fmt(tmp, sizeof tmp, "%s.mmwnew", dst);
				DeleteFileA(tmp);
				if (pLink(tmp, src, 0) && pMove(tmp, dst, 1)) n++;
				else { DeleteFileA(tmp); fails++; }
			}
			if (at >= nEnt) { linkDone = w; linkFail = fails; LOG("winter: the ground's files linked to stage %d (%d, %d in use or missing)", w, n, fails); }
			Sleep(1);
			continue;
		}
		Sleep(15);
	}
}
static void start()
{
	if (started) return;
	started = true;
	HANDLE k = LoadLibraryA("kernel32.dll");
	pLink = (int(__stdcall *)(const char *, const char *, void *))GetProcAddress(k, "CreateHardLinkA");
	pMove = (int(__stdcall *)(const char *, const char *, u32))GetProcAddress(k, "MoveFileExA");
	u32 tid;
	CreateThread(0, 64 * 1024, worker, 0, 0, &tid);
}
static const char *texPath(u32 t)
{
	if (!readable(t, 0x40)) return 0;
	const char *s = (const char *)ptr(t + pathOff);
	return s && readable((u32)s, 8) ? s : 0;
}
static const char *landPart(const char *s)
{
	static const char kL[] = "textures\\landscape\\";
	for (const char *q = s; *q; q++)
	{
		int i = 0;
		for (; kL[i]; i++)
		{
			char c = q[i];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c == '/') c = '\\';
			if (c != kL[i]) break;
		}
		if (!kL[i]) return q;
	}
	return 0;
}
static bool checkList()
{
	liveChecked = true;
	anchor = loadTexture("textures\\mmw\\snow_depth.dds");
	if (!anchor) { LOG("winter: no texture of ours to find the game's textures by - loaded ones change at the next load only"); return false; }
	__atomic_add_fetch((volatile i32 *)(anchor + 4), 1, __ATOMIC_SEQ_CST);
	static const u32 offs[3] = {0x30, 0x34, 0x38};
	for (int i = 0; i < 3; i++)
	{
		pathOff = offs[i];
		const char *s = texPath(anchor);
		bool hit = false;
		for (const char *q = s; q && *q && !hit; q++) if ((q[0] | 32) == 's' && (q[1] | 32) == 'n' && (q[2] | 32) == 'o' && (q[3] | 32) == 'w' && q[4] == '_') hit = true;
		if (hit) { LOG("winter: textures name their file at +%x (%s)", pathOff, s); return true; }
	}
	LOG("winter: our texture's file name not found on it - loaded textures change at the next load only");
	return false;
}
static u32 __cdecl walkThunk(u32 st, u32)
{
	int found = 0, seen = 0;
	for (int dir = 0; dir < 2; dir++)
	{
		u32 off = dir ? 0x2C : 0x28;
		for (u32 t = ptr(anchor + off); t && seen < 200000; t = ptr(t + off), seen++)
		{
			if (!readable(t, 0x40)) break;
			if (ptr(t) != ptr(anchor)) continue;
			const char *s = texPath(t);
			const char *lp = s ? landPart(s) : 0;
			int e = lp ? find(lp) : -1;
			if (e < 0 || nJobs >= kJobs || rd<i32>(t + 4) <= 0) continue;
			__atomic_add_fetch((volatile i32 *)(t + 4), 1, __ATOMIC_SEQ_CST);
			Job &j = jobs[nJobs];
			j.tex = t; j.ent = e; j.stage = (int)st; j.buf = 0; j.len = 0; j.state = 0;
			__sync_synchronize();
			nJobs = nJobs + 1;
			found++;
		}
	}
	return (u32)(found | (seen << 12));
}
static u32 __cdecl rewriteThunk(u32 jp, u32);
static bool rewrite(const Job &j)
{
	g_gThread = GetCurrentThreadId();
	u32 r = guardedCall2((u32)&rewriteThunk, (u32)&j, 0);
	if (g_gFault) { g_gFault = 0; live = false; LOG("winter: rewriting a loaded texture faulted (at %08x) - loaded textures change at the next load only", g_gFaultEip); return false; }
	return r != 0;
}
static u32 __cdecl rewriteThunk(u32 jp, u32)
{
	const Job &j = *(const Job *)jp;
	const Ent &e = ents[j.ent];
	const u8 *f = (const u8 *)j.buf;
	if (j.len < 128 || *(const u32 *)f != 0x20534444 || *(const u32 *)(f + 84) != 0x35545844) return 0;
	u32 fw = *(const u32 *)(f + 16), fh = *(const u32 *)(f + 12);
	u32 td = ptr(j.tex + 0x24), d3d = td ? ptr(td + 0x64) : 0;
	if (!d3d || !readable(d3d, 4)) return 0;
	u32 levels = (u32)vcall(d3d, 13);
	if (!levels || levels > 16) return 0;
	int done = 0;
	for (u32 L = 0; L < levels; L++)
	{
		u32 desc[8] = {0};
		if (vcall(d3d, 17, L, (void *)desc) < 0 || desc[0] != 0x35545844 || desc[3] == 0) { if (!L) return 0; break; }
		u32 off = 128, lw = fw, lh = fh;
		bool have = false;
		for (int k = 0; k < 16 && off < j.len; k++)
		{
			u32 sz = ((lw + 3) / 4 < 1 ? 1 : (lw + 3) / 4) * ((lh + 3) / 4 < 1 ? 1 : (lh + 3) / 4) * 16;
			if (lw == desc[6] && lh == desc[7]) { have = off + sz <= j.len; break; }
			off += sz; lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
		}
		if (!have) continue;
		struct { i32 pitch; u8 *bits; } lr = {0, 0};
		if (vcall(d3d, 19, L, (void *)&lr, (void *)0, 0u) < 0 || !lr.bits) break;
		u32 rows = (lh + 3) / 4 < 1 ? 1 : (lh + 3) / 4, rb = ((lw + 3) / 4 < 1 ? 1 : (lw + 3) / 4) * 16;
		for (u32 r = 0; r < rows; r++) memcpy(lr.bits + r * lr.pitch, f + off + r * rb, rb);
		vcall(d3d, 20, L);
		done++;
	}
	(void)e;
	return done > 0 ? 1 : 0;
}
static void tick(int stage)
{
	if (!loaded) load();
	if (!nEnt) return;
	start();
	if (stage != want)
	{
		want = stage;
		linkWant = stage;
		if (live && !liveChecked) live = checkList();
		nextWalk = rt::now();
	}
	if (live && nextWalk > 0 && rt::now() >= nextWalk && nRead >= nJobs)
	{
		nextWalk = 0;
		for (int i = nDone; i < nJobs; i++) { if (jobs[i].buf) rt::free(jobs[i].buf); if (jobs[i].tex) niRelease(jobs[i].tex); }
		nJobs = nRead = nDone = 0;
		g_gThread = GetCurrentThreadId();
		u32 r = guardedCall2((u32)&walkThunk, (u32)want, 0);
		if (g_gFault) { g_gFault = 0; live = false; LOG("winter: walking the game's textures faulted (at %08x) - loaded textures change at the next load only", g_gFaultEip); }
		else LOG("winter: stage %d - %d loaded landscape textures to rewrite (of %d textures)", want, r & 0xFFF, r >> 12);
	}
	if (nDone < nRead)
	{
		Job &j = jobs[nDone];
		static int ok, bad;
		if (j.state == 1 && j.stage == want) { if (rewrite(j)) ok++; else if (bad++ < 5) LOG("winter: %s not rewritten (not DXT5 / its size / its pool)", ents[j.ent].path); }
		if (j.buf) { rt::free(j.buf); j.buf = 0; }
		if (j.tex) { niRelease(j.tex); j.tex = 0; }
		nDone++;
		if (nDone == nJobs && nJobs) { LOG("winter: stage %d in the world (%d textures rewritten, %d not)", want, ok, bad); ok = bad = 0; }
	}
}
}

// ---- Seasons by the game's calendar (the author: "seasoned base matching games calendar.
namespace season
{
enum { W_CLEAR, W_OVERCAST, W_RAIN, W_THUNDER, W_SNOW, W_BLIZZARD };
static const char *kWName[6] = {"clear", "overcast", "rain", "a thunderstorm", "snowfall", "a blizzard"};
static const char *kSName[4] = {"winter", "spring", "summer", "fall"};
static bool loaded, on = true;
static int hold;
static int weather = W_CLEAR, snowLevel = 1;
static double nextH = -1, lastH = -1;
static float ground;
static int stage;
static double savedAt;
static u32 rs = 0x9E3779B9;
static float rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return (float)(rs & 0xFFFFFF) / 16777216.0f; }
static float glob(u32 id) { TESForm *f = lookup(id); return f && formtype(f) == 0x06 ? rd<float>((u32)f + 0x24) : -1; }
static int seasonOf(int month) { return month == 11 || month <= 1 ? 0 : month <= 4 ? 1 : month <= 7 ? 2 : 3; }
static float weight(int s, int w, int month, int day)
{
	float v = 0;
	switch (s * 6 + w)
	{
	case 0: v = TUNE(seasonWinterClear); break; case 1: v = TUNE(seasonWinterOvercast); break; case 2: v = TUNE(seasonWinterRain); break;
	case 3: v = TUNE(seasonWinterThunder); break; case 4: v = TUNE(seasonWinterSnow); break; case 5: v = TUNE(seasonWinterBlizzard); break;
	case 6: v = TUNE(seasonSpringClear); break; case 7: v = TUNE(seasonSpringOvercast); break; case 8: v = TUNE(seasonSpringRain); break;
	case 9: v = TUNE(seasonSpringThunder); break; case 10: v = TUNE(seasonSpringSnow); break; case 11: v = TUNE(seasonSpringBlizzard); break;
	case 12: v = TUNE(seasonSummerClear); break; case 13: v = TUNE(seasonSummerOvercast); break; case 14: v = TUNE(seasonSummerRain); break;
	case 15: v = TUNE(seasonSummerThunder); break; case 16: v = TUNE(seasonSummerSnow); break; case 17: v = TUNE(seasonSummerBlizzard); break;
	case 18: v = TUNE(seasonFallClear); break; case 19: v = TUNE(seasonFallOvercast); break; case 20: v = TUNE(seasonFallRain); break;
	case 21: v = TUNE(seasonFallThunder); break; case 22: v = TUNE(seasonFallSnow); break; case 23: v = TUNE(seasonFallBlizzard); break;
	}
	if (w == W_SNOW && ((month == 10 && day >= 20) || (month == 2 && day <= 10))) v = rt::max(v, TUNE(seasonEdgeSnow));
	if ((w == W_SNOW || w == W_BLIZZARD) && s == 2) v = 0;
	return rt::max(v, 0.0f);
}
static void path(char *p, u32 cap) { rt::fmt(p, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\season.ini", g_dir); }
static void save()
{
	char p[320], b[320];
	path(p, sizeof p);
	int n = rt::fmt(b, sizeof b, "on=%d\r\nhold=%d\r\nweather=%d\r\nlevel=%d\r\nnext=%.3f\r\nlast=%.3f\r\nground=%.4f\r\nstage=%d\r\n",
		on ? 1 : 0, hold, weather, snowLevel, (float)nextH, (float)lastH, ground, stage);
	rt::writefile(p, b, (u32)n, false);
}
static void load()
{
	loaded = true;
	char p[320];
	path(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (b)
	{
		for (char *q = b; *q; q++)
		{
			if (q != b && q[-1] != '\n') continue;
			if (rt::startswith(q, "on=")) on = rt::parsei(q + 3) != 0;
			else if (rt::startswith(q, "hold=")) hold = (int)rt::clamp((float)rt::parsei(q + 5), 0, 7);
			else if (rt::startswith(q, "weather=")) weather = (int)rt::clamp((float)rt::parsei(q + 8), 0, 5);
			else if (rt::startswith(q, "level=")) snowLevel = (int)rt::clamp((float)rt::parsei(q + 6), 1, 2);
			else if (rt::startswith(q, "next=")) nextH = rt::parsef(q + 5);
			else if (rt::startswith(q, "last=")) lastH = rt::parsef(q + 5);
			else if (rt::startswith(q, "ground=")) ground = rt::clamp(rt::parsef(q + 7), 0, 1);
			else if (rt::startswith(q, "stage=")) stage = (int)rt::clamp((float)rt::parsei(q + 6), 0, 3);
		}
		rt::free(b);
	}
	rs ^= (u32)(rt::now() * 1000);
	LOG("season: %s, weather %s, ground snow %.2f (stage %d)%s", on ? "on" : "off", kWName[weather], ground, stage, hold ? " - held by Y" : "");
}
static int effective()
{
	if (!on) return W_CLEAR;
	switch (hold) { case 1: return W_CLEAR; case 2: return W_OVERCAST; case 3: return W_RAIN; case 4: return W_THUNDER; case 5: case 6: return W_SNOW; case 7: return W_BLIZZARD; }
	return weather;
}
static int effLevel() { return hold == 5 ? 1 : hold == 6 ? 2 : effective() == W_BLIZZARD ? 3 : snowLevel; }
bool snowing() { int w = effective(); return w == W_SNOW || w == W_BLIZZARD; }
int level() { return effLevel(); }
bool overcast() { return effective() == W_OVERCAST; }
int rain() { int w = effective(); return w == W_RAIN ? 1 : w == W_THUNDER ? 2 : 0; }
static void keys(bool focus)
{
	if (focus && pressed(VK_SNOW))
	{
		on = !on;
		save();
		message(on ? "Seasons: on (K) - the weather follows the calendar" : "Seasons: off (K) - clear skies, the snow melts off the ground", 3.0f);
		LOG("season: %s", on ? "on" : "off");
	}
	if (focus && pressed(VK_SNOWSTORM))
	{
		hold = (hold + 1) % 8;
		save();
		static const char *names[8] = {"Weather: by the season (Y)", "Weather: clear (Y)", "Weather: overcast (Y)", "Weather: rain (Y)",
			"Weather: thunderstorm (Y)", "Weather: light snow (Y)", "Weather: heavy snow (Y)", "Weather: blizzard (Y)"};
		message(names[hold], 2.5f);
		LOG("season: weather held %d", hold);
	}
}
static void tick(bool focus)
{
	if (!loaded) load();
	keys(focus);
	float days = glob(0x39);
	int month = (int)glob(0x36), day = (int)glob(0x37);
	if (days < 0 || month < 0) return;
	double h = days * 24.0;
	int s = seasonOf(month);
	double dh = lastH < 0 ? 0 : h - lastH;
	if (dh < 0 || dh > 24 * 30) { dh = 0; nextH = h; }
	if (dh > 24) dh = 24;
	lastH = h;
	static int lastSeason = -1;
	if (lastSeason >= 0 && s != lastSeason) { LOG("season: %s begins", kSName[s]); nextH = h; }
	lastSeason = s;
	if (on && (nextH < 0 || h >= nextH))
	{
		float tot = 0, wv[6];
		for (int w = 0; w < 6; w++) tot += wv[w] = weight(s, w, month, day);
		float r = rnd() * rt::max(tot, 0.001f);
		int pick = W_CLEAR;
		for (int w = 0; w < 6; w++) { if (r < wv[w]) { pick = w; break; } r -= wv[w]; }
		int was = weather;
		weather = pick;
		snowLevel = rnd() < TUNE(seasonHeavyShare) ? 2 : 1;
		float lo = TUNE(seasonRollMinHours), hi = rt::max(TUNE(seasonRollMaxHours), lo);
		nextH = h + lo + (hi - lo) * rnd();
		LOG("season: %s (month %d day %d) - %s%s for %.1f game hours (was %s)", kSName[s], month + 1, day, kWName[weather],
			weather == W_SNOW ? (snowLevel == 2 ? ", heavy" : ", light") : "", (float)(nextH - h), kWName[was]);
		save();
	}
	int w = effective();
	float gain = 0;
	if (w == W_SNOW) gain = effLevel() == 2 ? TUNE(groundHeavyPerHour) : TUNE(groundLightPerHour);
	else if (w == W_BLIZZARD) gain = TUNE(groundBlizzardPerHour);
	else gain = -(s == 0 ? TUNE(groundMeltWinter) : s == 2 ? TUNE(groundMeltSummer) : TUNE(groundMeltMild));
	if (!on) gain = -TUNE(groundMeltSummer);
	ground = rt::clamp(ground + gain * (float)dh, 0, 1);
	float t1 = TUNE(groundStage1), t2 = TUNE(groundStage2), t3 = TUNE(groundStage3);
	int st = ground >= t3 ? 3 : ground >= t2 ? 2 : ground >= t1 ? 1 : 0;
	if (st != stage)
	{
		float edge = st > stage ? 0 : 0.02f;
		float th = st > stage ? (st == 1 ? t1 : st == 2 ? t2 : t3) : (stage == 1 ? t1 : stage == 2 ? t2 : t3);
		if (st > stage || ground < th - edge)
		{
			LOG("season: the ground's snow %.2f - stage %d -> %d", ground, stage, st);
			stage = st;
			save();
		}
	}
	if (rt::now() - savedAt > 30) { savedAt = rt::now(); save(); }
	wintertex::tick(stage);
}
}
// ---- Rain and thunderstorms
namespace rainwx
{
static bool tried;
static u32 form, dataOff;
static u8 orig[12];
static int applied = -1;
static char hex[12];
static int modIdx(const char *name)
{
	u32 dh = ptr(0x11C3F2C);
	if (!dh) return -1;
	u32 n = rd<u32>(dh + 0x218);
	for (u32 i = 0; i < n && i < 255; i++)
	{
		u32 mi = ptr(dh + 0x21C + 4 * i);
		const char *m = mi ? (const char *)(mi + 0x20) : 0;
		if (!m) continue;
		const char *a = m, *b = name;
		while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
		if (!*a && !*b) return (int)i;
	}
	return -1;
}
static void find()
{
	tried = true;
	int hh = modIdx("HonestHearts.esm");
	if (hh < 0) { LOG("rain: Honest Hearts isn't loaded - rain shows as an overcast sky"); return; }
	u32 id = ((u32)hh << 24) | 0x010C78;
	TESForm *f = lookup(id);
	if (!f || formtype(f) != 0x35) { LOG("rain: %08x isn't Honest Hearts' rain weather - rain shows as an overcast sky", id); return; }
	form = (u32)f;
	rt::fmt(hex, sizeof hex, "%08x", id);
	for (u32 o = 0x18; o + 12 <= 0x300 && readable(form + o, 12); o++)
	{
		const u8 *d = (const u8 *)(form + o);
		if (d[0] == 64 && d[6] == 52 && d[7] == 128 && d[8] == 128 && d[9] == 128 && d[10] == 138 && (d[11] & 4)) { dataOff = o; break; }
	}
	if (dataOff) memcpy(orig, (void *)(form + dataOff), 12);
	LOG("rain: Honest Hearts' rain %s%s", hex, dataOff ? "" : " (its lightning not found in memory: plain rain has lightning too)");
	if (dataOff) LOG("rain: its weather data at +%x", dataOff);
}
const char *weatherFor(int kind)
{
	if (!tried) find();
	if (!form) return 0;
	int want = kind == 2 ? 1 : 0;
	if (dataOff && want != applied)
	{
		u8 *d = (u8 *)(form + dataOff);
		memcpy(d, orig, 12);
		if (!want) { d[8] = 0; d[9] = 0; d[10] = 255; }
		applied = want;
	}
	return hex;
}
double dumpAt;
static bool dumped;
static u32 texVt;
static bool isAv(u32 o) { return o && readable(o, 0xB0) && readable(ptr(o), 0x40) && ptr(ptr(o)) > 0x400000 && ptr(ptr(o)) < 0x1200000; }
static const char *nameOf(u32 o) { const char *n = (const char *)ptr(o + 8); return n && readable((u32)n, 4) ? n : "(no name)"; }
static void texturesIn(u32 o, int depth, char *out, u32 cap, int &n)
{
	if (!o || !readable(o, 0x100) || depth > 1) return;
	for (u32 off = 0; off < 0x100 && n < 6; off += 4)
	{
		u32 t = ptr(o + off);
		if (!t || !readable(t, 0x40)) continue;
		if (texVt && ptr(t) == texVt)
		{
			const char *pth = (const char *)ptr(t + 0x30);
			u32 len = (u32)strlen(out);
			if (pth && readable((u32)pth, 4) && len + 80 < cap) { rt::fmt(out + len, cap - len, " [+%x %s]", off, pth); n++; }
		}
		else if (depth == 0 && readable(ptr(t), 4)) texturesIn(t, depth + 1, out, cap, n);
	}
}
static void tree(u32 o, int depth, int &lines)
{
	if (lines >= 150 || depth > 10 || !isAv(o)) return;
	char ind[24]; int k = depth * 2 < 22 ? depth * 2 : 22;
	for (int i = 0; i < k; i++) ind[i] = ' ';
	ind[k] = 0;
	bool node = ptr(ptr(o) + 0x0C) == 0x6815C0;
	if (node)
	{
		u16 cnt = rd<u16>(o + OFF_NODE_CHILDCOUNT);
		LOG("precip: %s%s node, %d children, vtable %x, flags %x", ind, nameOf(o), cnt, ptr(o), rd<u32>(o + 0x30));
		lines++;
		u32 arr = ptr(o + OFF_NODE_CHILDREN);
		for (u16 i = 0; arr && i < cnt && i < 32; i++) tree(ptr(arr + 4 * i), depth + 1, lines);
		return;
	}
	u32 sh = ptr(o + OFF_GEOM_SHADER), mat = ptr(o + OFF_GEOM_MATERIAL);
	char tex[400] = "";
	int n = 0;
	texturesIn(sh, 0, tex, sizeof tex, n);
	LOG("precip: %s%s geometry, vtable %x, shader %x type %d%s", ind, nameOf(o), ptr(o), sh, sh && readable(sh, 0x20) ? rd<i32>(sh + OFF_SHADER_TYPE) : -1, tex);
	lines++;
	if (mat && readable(mat, 0x60))
	{
		const float *m = (const float *)(mat + 0x18);
		LOG("precip: %s  material %x: %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f", ind, mat,
			m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13]);
		lines++;
	}
}
void tickDump()
{
	if (dumped || !dumpAt || rt::now() < dumpAt) return;
	dumped = true;
	u32 anchor = loadTexture("textures\\mmw\\snow_depth.dds");
	texVt = anchor ? ptr(anchor) : 0;
	u32 sky = ptr(0x11DEA20);
	if (!sky || !readable(sky, 0x80)) { LOG("precip: no sky"); return; }
	char line[300]; int n = 0;
	for (u32 off = 0; off < 0x80; off += 4) n += rt::fmt(line + n, sizeof line - n, "%x ", ptr(sky + off));
	LOG("precip: sky %x: %s", sky, line);
	u32 p = ptr(sky + 0x38);
	if (!p || !readable(p, 0x40)) { LOG("precip: no precipitation at sky +38"); return; }
	n = 0;
	for (u32 off = 0; off < 0x40; off += 4) n += rt::fmt(line + n, sizeof line - n, "%x ", ptr(p + off));
	LOG("precip: precipitation %x (vtable %x): %s", p, ptr(p), line);
	int lines = 0;
	g_gThread = GetCurrentThreadId();
	for (u32 off = 4; off < 0x40; off += 4)
	{
		u32 o = ptr(p + off);
		if (isAv(o)) { LOG("precip: at +%x:", off); tree(o, 1, lines); }
	}
	if (form && dataOff) { const u8 *d = (const u8 *)(form + dataOff); LOG("precip: weather data %d %d %d %d %d %d %d %d %d %d %d %d", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11]); }
}
void restore()
{
	if (form && dataOff && applied != 1) { memcpy((void *)(form + dataOff), orig, 12); applied = 1; }
}
}

namespace snowfx
{
enum { BW = 256, GW = 128, GH = 64, MAXW = 3, NREF = 4, NLAYER = 6 };
static const u8 kRefs[NLAYER] = {18, 55, 91, 128, 164, 200};
static bool loaded, on, wipersOn;
static int mode;
static bool netSet;
static int level = 1;
static float fall;
static float storm = 1;
static float depth;
static float glass[GW * GH];
static u8 thr[BW * BW];
static u8 rate[GW * GH];
static u32 depthPx[BW * BW], glassPx[GW * GH];
static int setLevel = -1;
static double oldUntil;
static bool weatherForced;
static float lastX, lastY, vx, vy;
static bool havePos;
static double nextDepthUp, nextGlassUp;
static float depthShown = -1;
static float windHeading;
static u32 glassTex;
static u32 glassRoot;
struct Wiper { u32 node; float axis[3], sweep, px, py, rIn, rOut, a0, sign, prevA; };
static Wiper wip[MAXW];
static int nWip, wipCar = -2;
static u32 wipRoot;
static float glassW, glassH, phase;
static float relA[MAXW][GW * GH], rad[MAXW][GW * GH];

static float hash01(int x, int y, int s)
{
	u32 h = (u32)x * 374761393u + (u32)y * 668265263u + (u32)s * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float vnoise(float x, float y, int s)
{
	int ix = (int)(x + 1000) - 1000, iy = (int)(y + 1000) - 1000;
	float fx = x - (float)ix, fy = y - (float)iy;
	fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
	float a = hash01(ix, iy, s), b = hash01(ix + 1, iy, s), c = hash01(ix, iy + 1, s), d = hash01(ix + 1, iy + 1, s);
	return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
static const char *levelName(int l);
static void load(double t)
{
	loaded = true;
	on = season::snowing();
	mode = season::level();
	fall = on ? 1.0f : 0.0f;
	level = mode ? mode : 1;
	storm = (float)level;
	windHeading = 360.0f * hash01(7, 3, (int)(t * 10) & 0xFFFF);
	for (int y = 0; y < BW; y++)
		for (int x = 0; x < BW; x++)
		{
			float n = 0.55f * vnoise(x / 14.0f, y / 14.0f, 1) + 0.3f * vnoise(x / 6.0f, y / 6.0f, 2) + 0.15f * vnoise(x / 2.5f, y / 2.5f, 3);
			thr[y * BW + x] = (u8)rt::clamp(n * 255, 0, 255);
		}
	for (int y = 0; y < GH; y++)
		for (int x = 0; x < GW; x++)
			rate[y * GW + x] = (u8)rt::clamp((0.6f * vnoise(x / 7.0f, y / 7.0f, 4) + 0.4f * vnoise(x / 2.5f, y / 2.5f, 5)) * 255, 0, 255);
	LOG("snow: %s at start (%s)", on ? "falling" : "not falling", levelName(level));
}
static const char *levelName(int l) { return l == 1 ? "light snow" : l == 2 ? "heavy snow" : "a blizzard"; }
static u32 shapeTex(u32 shape)
{
	u32 sh = shape ? ptr(shape + OFF_GEOM_SHADER) : 0;
	if (!sh || rd<u32>(sh + OFF_SHADER_TYPE) != SHADER_PPLIGHTING) return 0;
	u32 slot = ptr(sh + OFF_PPL_TEXTURES), tex = slot ? ptr(slot) : 0, td = tex ? ptr(tex + 0x24) : 0;
	return td ? ptr(td + 0x64) : 0;
}
static u32 childNamed(u32 root, const char *pre)
{
	u32 arr = root ? ptr(root + OFF_NODE_CHILDREN) : 0;
	u16 n = root ? rd<u16>(root + OFF_NODE_CHILDCOUNT) : 0;
	if (!arr || n > 512) return 0;
	u32 len = (u32)strlen(pre);
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (nm && !memcmp(nm, pre, len)) return c;
	}
	return 0;
}
static bool upload(u32 d3d, int w, int h, const u32 *px)
{
	u32 desc[8] = {0};
	if (!d3d || vcall(d3d, 17, 0u, (void *)desc) < 0) return false;
	if ((desc[0] != 21 && desc[0] != 22) || desc[6] != (u32)w || desc[7] != (u32)h)
	{
		static u32 told;
		if (told != d3d) { told = d3d; LOG("snow: texture %x is format %u %ux%u, not %dx%d A8R8G8B8 - can't draw on it", d3d, desc[0], desc[6], desc[7], w, h); }
		return false;
	}
	struct { i32 pitch; u8 *bits; } lr = {0, 0};
	if (vcall(d3d, 19, 0u, (void *)&lr, (void *)0, 0u) < 0 || !lr.bits) return false;
	for (int y = 0; y < h; y++) memcpy(lr.bits + y * lr.pitch, px + y * w, w * 4);
	vcall(d3d, 20, 0u);
	return true;
}
static inline float spotDepth(float d, u8 t) { return rt::clamp(d * (0.65f + 0.7f * (t / 255.0f)), 0, 1); }
static void drawDepth()
{
	for (int i = 0; i < BW * BW; i++)
	{
		u32 a = (u32)(spotDepth(depth, thr[i]) * 255);
		u32 s = 238 + (thr[i] >> 4);
		depthPx[i] = (a << 24) | ((s - 6) << 16) | ((s - 3) << 8) | s;
	}
}
static void drawGlass()
{
	for (int i = 0; i < GW * GH; i++)
	{
		u32 a = (u32)(rt::clamp(glass[i], 0, 1) * 255), s = 240 + (rate[i] >> 5);
		glassPx[i] = (a << 24) | ((s - 6) << 16) | ((s - 3) << 8) | s;
	}
}
static void showLayers(u32 node, float deepest)
{
	u32 arr = node ? ptr(node + OFF_NODE_CHILDREN) : 0;
	u16 n = node ? rd<u16>(node + OFF_NODE_CHILDCOUNT) : 0;
	if (!arr || n > 32) return;
	for (u16 i = 0; i < n; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (!nm) continue;
		const char *colon = nm;
		while (*colon && *colon != ':') colon++;
		if (*colon != ':') continue;
		int k = colon[1] - '0';
		if (k < 0 || k >= NLAYER) continue;
		bool show = deepest * 255 > kRefs[k];
		u32 &f = *(u32 *)(c + OFF_NIAV_FLAGS);
		f = show ? (f & ~1u) : (f | 1u);
	}
}
static void wipersFind(u32 root, int car)
{
	wipCar = car; wipRoot = root; nWip = 0;
	u32 g = childNamed(root, "MMWSnowGlass ");
	if (!g) return;
	float v[3];
	const char *s = suspF((const char *)ptr(g + 8) + 13, v, 3);
	int n = (int)v[2];
	glassW = v[0]; glassH = v[1];
	for (int k = 0; k < n && k < MAXW && s; k++)
	{
		float w[6];
		s = suspF(s, w, 6);
		if (!s) break;
		char nm[16];
		rt::fmt(nm, sizeof nm, "MMWWiper%d ", k);
		u32 node = childNamed(root, nm);
		if (!node) continue;
		Wiper &W = wip[nWip];
		float a[4];
		if (!suspF((const char *)ptr(node + 8) + strlen(nm), a, 4)) continue;
		W.node = node; W.axis[0] = a[0]; W.axis[1] = a[1]; W.axis[2] = a[2]; W.sweep = a[3];
		W.px = w[0]; W.py = w[1]; W.rIn = w[2]; W.rOut = w[3]; W.a0 = w[4]; W.sign = w[5]; W.prevA = 0;
		for (int y = 0; y < GH; y++)
			for (int x = 0; x < GW; x++)
			{
				float ps = (x + 0.5f) / GW * glassW, pt = (1 - (y + 0.5f) / GH) * glassH;
				float dx = ps - W.px, dy = pt - W.py;
				float ang = W.sign * (rt::atan2(dy, dx) - W.a0);
				while (ang > rt::PI) ang -= 2 * rt::PI;
				while (ang < -rt::PI) ang += 2 * rt::PI;
				relA[nWip][y * GW + x] = ang;
				rad[nWip][y * GW + x] = rt::sqrt(dx * dx + dy * dy);
			}
		nWip++;
	}
	LOG("snow: %s windshield %.1f x %.1f, %d wipers", car >= 0 ? kCars[car].id : "?", glassW, glassH, nWip);
}
static bool wipersTick(float dt)
{
	if (!nWip) return false;
	bool moving = wipersOn || phase != (float)(int)phase;
	if (!moving) return false;
	float cyc = rt::max(TUNE(wiperCycleSeconds), 0.3f);
	float next = phase + dt / cyc;
	if (!wipersOn && (int)next != (int)phase) next = (float)(int)next;
	phase = next;
	if (phase > 1000) phase -= 1000;
	float f = phase - (float)(int)phase;
	float e = (1 - rt::cos(2 * rt::PI * f)) * 0.5f;
	static float sinceLog = 99;
	sinceLog += dt;
	if (sinceLog > 5)
	{
		sinceLog = 0;
		float *m = (float *)(wip[0].node + 0x34);
		LOG("snow: wipers at %.0f%% of their sweep (wiper 0 at %.0f deg, its matrix %.2f %.2f %.2f)", e * 100, wip[0].sweep * e * 180 / rt::PI, m[0], m[1], m[2]);
	}
	for (int k = 0; k < nWip; k++)
	{
		Wiper &W = wip[k];
		float a = W.sweep * e;
		setRotAxis(W.node, W.axis, a);
		updateNode(W.node);
		float lo = rt::min(a, W.prevA) - 0.03f, hi = rt::max(a, W.prevA) + 0.03f;
		for (int i = 0; i < GW * GH; i++)
		{
			float r = rad[k][i], ra = relA[k][i];
			if (r >= W.rIn - 0.6f && r <= W.rOut + 0.6f && ra >= lo && ra <= hi) glass[i] *= 0.04f;
		}
		W.prevA = a;
	}
	return true;
}
static u32 wForm;
static float wFog[6];
static u8 wCol[10][6][4];
static bool wSaved;
static float wApplied = -1;
static void weatherLook(float s)
{
	if (!wForm || !wSaved) return;
	if (rt::absf(s - wApplied) < 0.01f) return;
	wApplied = s;
	float heavy = rt::clamp(s - 1, 0, 1), bliz = rt::clamp(s - 2, 0, 1);
	float *fog = (float *)(wForm + 0xF0);
	float hFar = TUNE(snowHeavyFogFar), bFar = TUNE(snowBlizzardFogFar);
	for (int k = 0; k < 2; k++)
	{
		float nearW = wFog[2 * k], farW = wFog[2 * k + 1];
		float hf = k ? hFar * 0.67f : hFar, bf = k ? bFar * 0.75f : bFar;
		float far1 = rt::min(farW, hf), far2 = rt::min(far1, bf);
		float far = farW + (far1 - farW) * heavy;
		far = far + (far2 - far) * bliz;
		float nr = nearW * (1 - 0.8f * heavy);
		nr = nr * (1 - bliz);
		fog[2 * k] = nr;
		fog[2 * k + 1] = rt::max(far, nr + 200);
	}
	u8 *col = (u8 *)(wForm + 0x108);
	static const int types[4] = {1, 0, 7, 8};
	float white = TUNE(snowBlizzardWhiteout);
	for (int ti = 0; ti < 4; ti++)
		for (int t = 0; t < 6; t++)
		{
			bool night = t == 3 || t == 5;
			float gr[3] = {night ? 52.0f : 188.0f, night ? 57.0f : 196.0f, night ? 68.0f : 208.0f};
			if (night)
			{
				float dk = nights::darkness(), k = t == 5 ? 1 - 0.92f * dk : 1 - (1 - TUNE(nightKeep)) * dk;
				for (int c = 0; c < 3; c++) gr[c] *= k;
			}
			float amt = types[ti] == 1 ? 0.6f * heavy + 0.4f * bliz : 0.35f * heavy * white + (1 - 0.35f * heavy) * white * bliz;
			for (int c = 0; c < 3; c++)
			{
				float v = wCol[types[ti]][t][c];
				col[(types[ti] * 6 + t) * 4 + c] = (u8)rt::clamp(v + (gr[c] - v) * amt, 0, 255);
			}
		}
	static float logged = -1;
	if (rt::absf(s - logged) >= 0.25f)
	{
		logged = s;
		LOG("snow: sky look %.2f (heavy %.2f, blizzard %.2f): fog day %.0f..%.0f night %.0f..%.0f", s, heavy, bliz, fog[0], fog[1], fog[2], fog[3]);
	}
	float dim = TUNE(snowBlizzardSunDim) * (0.4f * heavy + 0.6f * bliz);
	for (int t = 0; t < 6; t++)
	{
		for (int c = 0; c < 3; c++)
		{
			for (int ty = 4; ty <= 5; ty++)
				col[(ty * 6 + t) * 4 + c] = (u8)rt::clamp(wCol[ty][t][c] * (1 - dim), 0, 255);
			float a = wCol[3][t][c], cool = c == 2 ? 1.12f : c == 1 ? 1.04f : 0.97f;
			col[(3 * 6 + t) * 4 + c] = (u8)rt::clamp(a + (a * cool - a) * (heavy * 0.5f + bliz * 0.5f), 0, 255);
		}
	}
}
static void weatherRestore()
{
	if (!wForm || !wSaved) return;
	memcpy((void *)(wForm + 0xF0), wFog, sizeof wFog);
	memcpy((void *)(wForm + 0x108), wCol, sizeof wCol);
	wApplied = -1;
}
static int forcedKind;
static int forcedRain = -1;
static void weather(int kind, int rainKind)
{
	char w[16];
	iniStr("snowWeather", w, sizeof w, "");
	rainwx::tickDump();
	const char *rainId = kind == 2 ? rainwx::weatherFor(rainKind) : 0;
	if (kind == 2 && !rainId) kind = 1;
	if (kind == 1 && !*w) kind = 0;
	if (kind == forcedKind && (kind != 2 || rainKind == forcedRain)) return;
	if (forcedKind == 1) weatherRestore();
	if (kind == 1)
	{
		u32 id = 0;
		for (const char *q = w; *q; q++)
		{
			char c = *q;
			id = id * 16 + (u32)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
		}
		TESForm *f = lookup(id);
		if (!f || formtype(f) != 0x35) LOG("snow: weather %s (%x) not found or not a weather", w, id);
		if (f && formtype(f) == 0x35)
		{
			wForm = (u32)f;
			if (!wSaved)
			{
				memcpy(wFog, (void *)(wForm + 0xF0), sizeof wFog);
				memcpy(wCol, (void *)(wForm + 0x108), sizeof wCol);
				wSaved = true;
				LOG("snow: sky %s fog day %.0f..%.0f night %.0f..%.0f", w, wFog[0], wFog[1], wFog[2], wFog[3]);
			}
		}
	}
	char cmd[64];
	if (kind == 0)
	{
		console("ReleaseWeatherOverride");
		rainwx::restore();
		LOG("weather: the world's own sky again");
	}
	else
	{
		rt::fmt(cmd, sizeof cmd, "ForceWeather %s 1", kind == 1 ? w : rainId);
		console(cmd);
		if (kind != 2) rainwx::restore();
		else if (!rainwx::dumpAt) rainwx::dumpAt = rt::now() + 4;
		LOG("weather: %s", kind == 1 ? "the snow / overcast sky" : rainKind == 2 ? "a thunderstorm" : "rain");
	}
	forcedKind = kind;
	forcedRain = kind == 2 ? rainKind : -1;
	weatherForced = kind == 1;
}
static float flakeAlpha;
static u32 exactChild(u32 root, const char *name)
{
	u32 arr = root ? ptr(root + OFF_NODE_CHILDREN) : 0;
	u16 n = root ? rd<u16>(root + OFF_NODE_CHILDCOUNT) : 0;
	for (u16 i = 0; arr && i < n && i < 64; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		if (nm && !rt::strcmp(nm, name)) return c;
	}
	return 0;
}
static int refCount(int l) { return l == 1 ? 1 : l == 2 ? 3 : 4; }
static void removeSet(int base)
{
	for (int i = 0; i < NREF; i++)
		if (refs[base + i]) { removeRef((TESObjectREFR *)lookup(refs[base + i])); refs[base + i] = 0; }
}
static void removeFall()
{
	removeSet(0); removeSet(NREF);
	setLevel = -1;
}
static void reset()
{
	for (int i = 0; i < 2 * NREF; i++) refs[i] = 0;
	for (int i = 0; i < 4; i++) rainRefs[i] = 0;
	setLevel = -1; weatherForced = false; forcedKind = 0; forcedRain = -1; havePos = false; wipCar = -2; glassRoot = 0; wApplied = -1;
}
static void rainFalls(int kind, float x, float y, float z)
{
	static const int model[4] = {0, 1, 0, 2};
	bool want[4] = {kind == 1 || kind == 2, kind == 1 || kind == 2, kind == 2, kind == 3};
	float lead = TUNE(snowLeadSeconds);
	float cx = x + vx * lead, cy = y + vy * lead;
	float a = windHeading * rt::PI / 180, wx = rt::sin(a), wy = rt::cos(a);
	static int logged;
	for (int i = 0; i < 4; i++)
	{
		TESObjectREFR *r = rainRefs[i] ? (TESObjectREFR *)lookup(rainRefs[i]) : 0;
		if (!want[i] || !iniI("rainfall", 0))
		{
			if (r) removeRef(r);
			rainRefs[i] = 0;
			continue;
		}
		float px = cx, py = cy, pz = z, ax = 0, az = 0;
		if (i == 2) { float sp = rt::sqrt(vx * vx + vy * vy); float mx = sp > 50 ? vx / sp : 0, my = sp > 50 ? vy / sp : 1; px += mx * TUNE(rainStormOffset); py += my * TUNE(rainStormOffset); }
		if (i == 3) { float up = TUNE(blizzardStreakUpwind); px = x - wx * up; py = y - wy * up; ax = TUNE(blizzardStreakTilt); az = windHeading; }
		if (!r)
		{
			r = placeRef(kFx_rain + (u32)model[i], px, py, pz, az);
			if (r) rainRefs[i] = refid(r);
			if (logged < 12) { logged++; LOG("rain: fall %d (model %d) %s at %.0f %.0f %.0f", i, model[i], r ? "placed" : "NOT placed", px, py, pz); }
		}
		if (r) moveRef(r, px, py, pz, ax, 0, az);
	}
}
static void placeFalls(double t, float x, float y, float z, float spd)
{
	float lead = TUNE(snowLeadSeconds);
	float cx = x + vx * lead, cy = y + vy * lead, h = TUNE(snowHeightUnits);
	float px[NREF], py[NREF], pz[NREF], head = 0;
	int n = refCount(level);
	if (level == 3)
	{
		float a = windHeading * rt::PI / 180, wx = rt::sin(a), wy = rt::cos(a);
		float up = TUNE(snowBlizzardUpwindUnits);
		for (int i = 0; i < n; i++)
		{
			float side = ((float)i - (n - 1) * 0.5f) * 1000;
			px[i] = cx - wx * up + wy * side; py[i] = cy - wy * up - wx * side; pz[i] = z + 300 - kFx_snow_up;
		}
		head = windHeading - 90;
	}
	else
	{
		float mx = 0, my = 1;
		if (spd > 50) { mx = vx / spd; my = vy / spd; }
		for (int i = 0; i < n; i++)
		{
			float side = ((float)i - (n - 1) * 0.5f) * 700;
			px[i] = cx + my * side; py[i] = cy - mx * side; pz[i] = z + h - kFx_snow_up;
		}
	}
	if (setLevel != level)
	{
		removeSet(NREF);
		for (int i = 0; i < NREF; i++) { refs[NREF + i] = refs[i]; refs[i] = 0; }
		oldUntil = t + 3;
		setLevel = level;
		LOG("snow: %s falling", levelName(level));
	}
	if (t > oldUntil) removeSet(NREF);
	for (int i = 0; i < NREF; i++)
	{
		TESObjectREFR *r = refs[i] ? (TESObjectREFR *)lookup(refs[i]) : 0;
		if (i >= n) { if (r) { removeRef(r); refs[i] = 0; } continue; }
		if (!r && iniI("snowfall", 0))
		{
			r = placeRef(kFx_snow + (u32)(level - 1), px[i], py[i], pz[i], head);
			if (r) refs[i] = refid(r);
			static int logged;
			if (logged < 12) { logged++; LOG("snow: %s fall %d %s at %.0f %.0f %.0f (you at %.0f %.0f %.0f)", levelName(level), i, r ? "placed" : "NOT placed", px[i], py[i], pz[i], x, y, z); }
		}
		else if (r) moveRef(r, px[i], py[i], pz[i], 0, 0, head);
	}
	float bright = rt::max(TUNE(nightFlakeMin), 1 - nights::now());
	for (int i = 0; i < 2 * NREF; i++)
	{
		TESObjectREFR *r = refs[i] ? (TESObjectREFR *)lookup(refs[i]) : 0;
		u32 root = r ? thirdPersonNode(r) : 0;
		u32 ps = exactChild(exactChild(root, "FXTireSmoke NonAccum"), "PCloud01");
		u32 mat = ps ? ptr(ps + OFF_GEOM_MATERIAL) : 0;
		if (!mat) continue;
		if (!flakeAlpha) flakeAlpha = rd<float>(mat + 0x3C) > 0 ? rd<float>(mat + 0x3C) : 1.0f;
		*(float *)(mat + 0x3C) = flakeAlpha * bright;
	}
}
static void tick(float dt, double t, bool focus, TESObjectREFR *pl)
{
	if (!loaded) load(t);
	(void)focus;
	if (!netSet)
	{
		bool was = on;
		int lv = was ? level : 0;
		on = season::snowing();
		if (on) level = season::level();
		if (on != was || (on && level != lv)) LOG("snow: %s", on ? levelName(level) : "stopped");
		if (on && level == 3 && lv != 3) windHeading += 60 * (hash01((int)t, 3, 13) - 0.5f) * 2;
	}
	windHeading += dt * 1.5f * (vnoise((float)t * 0.01f, 0, 21) - 0.5f) * 2;
	bool outside = worldspace() != 0;
	float fade = rt::max(TUNE(snowFadeSeconds), 1.0f);
	fall = rt::clamp(fall + (on ? dt : -dt) / fade, 0, 1);
	float blend = rt::max(TUNE(snowStormBlendSeconds), 1.0f);
	float want = on ? (float)level : 1.0f;
	storm += rt::clamp(want - storm, -dt / blend, dt / blend);
	float factor = level == 1 ? 1.0f : level == 2 ? TUNE(snowHeavyFactor) : TUNE(snowBlizzardFactor);
	if (outside && fall > 0)
		depth = rt::min(1.0f, depth + fall * factor * dt / (60 * rt::max(TUNE(snowBuildMinutes), 0.1f)));
	else if (!on)
		depth = rt::max(0.0f, depth - dt / (60 * rt::max(TUNE(snowMeltMinutes), 0.1f)));
	int rk = netSet ? 0 : season::rain();
	weather(!outside ? 0 : on || (!netSet && season::overcast()) ? 1 : rk ? 2 : 0, rk);
	if (weatherForced)
	{
		float s = fall > 0 ? 1 + (storm - 1) * fall : 1;
		weatherLook(s);
		static float forcedAt = 1;
		if (rt::absf(s - forcedAt) >= 0.4f)
		{
			forcedAt = s;
			char w[16], cmd[64];
			iniStr("snowWeather", w, sizeof w, "");
			rt::fmt(cmd, sizeof cmd, "ForceWeather %s 1", w);
			if (*w) console(cmd);
		}
	}
	{
		float s = outside ? fall * rt::clamp(storm - 1, 0, 2) : 0;
		float lvl = rt::clamp(s <= 1 ? s * 0.15f : 0.15f + (s - 1) * 0.85f, 0, 1) * TUNE(snowWindVolume);
		static float rainLvl;
		int rk2 = netSet || !rainwx::form ? 0 : season::rain();
		float rw = rk2 == 2 ? 1.0f : rk2 == 1 ? 0.55f : 0.0f;
		rainLvl += rt::clamp(rw - rainLvl, -dt / 6, dt / 6);
		float rv = rainLvl * TUNE(rainVolume) * (outside ? 1.0f : TUNE(rainIndoorVolume));
		audio::setRain(rv, g_driving ? 1.0f : outside ? 0.0f : 0.8f);
		if (rk2 == 2 && outside) lvl = rt::max(lvl, TUNE(thunderWindLevel) * TUNE(snowWindVolume));
		audio::setWind(lvl, g_driving ? 1.0f : 0.0f);
	}
	float *pp = pos(pl);
	float x = g_driving ? g_car.x : pp[0], y = g_driving ? g_car.y : pp[1], z = g_driving ? g_car.z : pp[2];
	if (havePos && dt > 0)
	{
		float k = rt::clamp(dt * 4, 0, 1);
		vx += ((x - lastX) / dt - vx) * k;
		vy += ((y - lastY) / dt - vy) * k;
		if (rt::absf(x - lastX) > 2000 || rt::absf(y - lastY) > 2000) vx = vy = 0;
	}
	lastX = x; lastY = y; havePos = true;
	{
		int rk = !outside ? 0 : forcedKind == 2 ? (forcedRain == 2 ? 2 : 1) : on && level == 3 && fall > 0.5f ? 3 : 0;
		rainFalls(rk, x, y, z);
	}
	if (outside && fall > 0.02f) placeFalls(t, x, y, z, rt::sqrt(vx * vx + vy * vy));
	else if (setLevel >= 0 || refs[0] || refs[NREF]) removeFall();
	u32 roots[10];
	int nr = 0;
	u32 drivenRoot = 0;
	if (g_driving) { TESObjectREFR *r = vref(g_car); if (r) roots[nr++] = drivenRoot = thirdPersonNode(r); }
	else if (g_parkedRef && g_activeCar >= 0) { TESObjectREFR *r = (TESObjectREFR *)lookup(g_parkedRef); if (r) roots[nr++] = thirdPersonNode(r); }
	for (int s = 0; s < 8; s++)
		if (g_display[s] && g_slotCar[s] >= 0) { TESObjectREFR *r = (TESObjectREFR *)lookup(g_display[s]); if (r) roots[nr++] = thirdPersonNode(r); }
	bool glassMoved = false;
	if (drivenRoot)
	{
		if (wipCar != g_activeCar || wipRoot != drivenRoot) wipersFind(drivenRoot, g_activeCar);
		if (glassRoot != drivenRoot)
		{
			glassRoot = drivenRoot;
			if (!glassTex) glassTex = loadTexture("textures\\mmw\\snow_glass_drive.dds");
			u32 g = childNamed(drivenRoot, "MMWSnowGlass ");
			u32 arr = g ? ptr(g + OFF_NODE_CHILDREN) : 0;
			u16 n = g ? rd<u16>(g + OFF_NODE_CHILDCOUNT) : 0;
			int swapped = 0;
			for (u16 i = 0; arr && i < n && i < 16; i++)
			{
				u32 c = ptr(arr + 4 * i);
				u32 sh = c ? ptr(c + OFF_GEOM_SHADER) : 0;
				if (sh && rd<u32>(sh + OFF_SHADER_TYPE) == SHADER_PPLIGHTING && swapTexture(sh, 0, glassTex)) swapped++;
			}
			for (int i = 0; i < GW * GH; i++) glass[i] = spotDepth(depth, (u8)(rate[i] ^ 0x5A));
			nextGlassUp = 0;
			LOG("snow: driving - its windshield texture on %d piles", swapped);
		}
		if (focus && pressed(VK_WIPERS))
		{
			if (nWip)
			{
				wipersOn = !wipersOn;
				message(wipersOn ? "Wipers on (V)" : "Wipers off (V)", 1.5f);
				LOG("snow: wipers %s (%d wipers, node %x sweeps %.0f deg)", wipersOn ? "on" : "off", nWip, wip[0].node, wip[0].sweep * 180 / rt::PI);
			}
			else
			{
				message("No wipers on this car - its defroster clears the windshield", 2.5f);
				LOG("snow: wipers pressed - this car has none (%s)", g_activeCar >= 0 ? kCars[g_activeCar].id : "?");
			}
		}
		glassMoved = wipersTick(dt);
		float g = outside ? fall * factor * dt / (60 * rt::max(TUNE(snowGlassBuildMinutes), 0.1f)) : 0;
		float defrost = nWip ? 0 : dt / rt::max(TUNE(snowDefrostSeconds), 1.0f);
		for (int i = 0; i < GW * GH; i++)
			glass[i] = rt::clamp(glass[i] + g * (0.55f + 0.9f * rate[i] / 255.0f) - defrost, 0, 1);
		if (defrost > 0) glassMoved = glassMoved || t >= nextGlassUp;
	}
	else glassRoot = 0;
	float deepest = rt::min(1.0f, depth * 1.35f);
	bool depthDue = t >= nextDepthUp && rt::absf(depth - depthShown) > 0.003f;
	u32 done[4];
	int nd = 0;
	for (int i = 0; i < nr; i++)
	{
		if (!roots[i]) continue;
		u32 body = childNamed(roots[i], "MMWSnowBody");
		u32 gl = childNamed(roots[i], "MMWSnowGlass ");
		showLayers(body, deepest);
		if (roots[i] != drivenRoot) showLayers(gl, deepest);
		else
		{
			float gmax = 0;
			for (int k = 0; k < GW * GH; k += 7) gmax = rt::max(gmax, glass[k]);
			showLayers(gl, gmax);
		}
		if (depthDue && body)
		{
			u32 d3d = shapeTex(ptr(ptr(body + OFF_NODE_CHILDREN)));
			bool seen = false;
			for (int k = 0; k < nd; k++) seen = seen || done[k] == d3d;
			if (d3d && !seen && nd < 4) { if (!nd) drawDepth(); if (upload(d3d, BW, BW, depthPx)) done[nd++] = d3d; }
		}
	}
	if (depthDue && nd) { depthShown = depth; nextDepthUp = t + 0.5; }
	if (drivenRoot && (t >= nextGlassUp || glassMoved))
	{
		u32 g = childNamed(drivenRoot, "MMWSnowGlass ");
		u32 d3d = g ? shapeTex(ptr(ptr(g + OFF_NODE_CHILDREN))) : 0;
		if (d3d) { drawGlass(); upload(d3d, GW, GH, glassPx); }
		nextGlassUp = t + 0.25;
	}
}
}

// Bodycam-style weapon handling on foot (weight and sway, recoil kick, free aim - free aim comes later).
namespace sprint { static int modeNow(); }
namespace body { static void frame(); }
namespace move { static int stateNow(); static void blindArmsOff(); static bool blindReachOk(const float *tb); static bool blindArms(TESObjectREFR *pl, float w, const float *E, const float *D, const float *Fv, float roll, const float *tb, float *moved); static bool blindQ(float *r, float *t); }
namespace handling
{
static bool on = true, active, haveLast;
static float O[2], lastW[2], lag[2], rec[2], recV[2], bake[2], blend;
static u32 weap;
static int lastClip = -1, shotsLogged;
static float kick, side, keep, hz, sway, lagSec;
static float wpow = 1, wheavy = 1;
static float wmass = 1;
static int shotsTotal;
static bool wAuto, wOneHand, wShotgun;
static float patPh;
struct GunAxis { float x, v; };
static GunAxis wbH, wbP, wbR;
static GunAxis flip;
static float pendRec[2], pendBake[2], pendT;
static u32 cam1N;
static int adsPending; static bool adsPendIn;
static float heat, adsT;
static int wtype = -1;
static bool hookShots;
static int shotLogs;
static int wfeed; static const char *wlabel;
static float lastPos[3], stepPhase, speedS, sinceLog, checkLeft;
static u32 rng = 0x2545F491;
static float rnd() { rng = rng * 1664525u + 1013904223u; return (float)(rng >> 8) / 16777216.0f; }
static float wrapPi(float a) { while (a > rt::PI) a -= 2 * rt::PI; while (a < -rt::PI) a += 2 * rt::PI; return a; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static u32 process(TESObjectREFR *pl)
{
	u32 p = ptr((u32)pl + 0x68);
	return p && rd<u32>(p + 0x28) <= 1 ? p : 0;
}
static u32 moveFlags(TESObjectREFR *pl)
{
	u32 mv = ptr((u32)pl + 0x190);
	return mv && ptr(mv) ? ((u32(__thiscall *)(u32))ptr(ptr(mv) + 0x20))(mv) : 0;
}
static void gunReset();
static void pivotIdentity();
static void rlRestore();
static void reset()
{
	for (int i = 0; i < 2; i++) O[i] = lag[i] = rec[i] = recV[i] = bake[i] = 0;
	blend = 0; haveLast = false;
}
static void tpCamIdentity();
static void release(TESObjectREFR *pl, const char *why)
{
	if (haveLast)
	{
		float *r = rot(pl);
		r[0] -= O[0]; r[2] -= O[1];
	}
	reset();
	gunReset(); pivotIdentity(); tpgun::identity(); rlRestore(); tpCamIdentity();
	if (active) LOG("handling: off (%s)", why);
	active = false;
}
static void readWeapon(u32 w)
{
	weap = w;
	shotsLogged = 0; shotLogs = 0;
	if (!w || formtype((void *)w) != 0x28) { wtype = -1; wpow = wheavy = wmass = 1; kick = side = 0; sway = 0.15f; lagSec = 0.03f; keep = 0; hz = 5; LOG("handling: no weapon (%x)", w ? refid((void *)w) : 0); return; }
	int ty = rd<u8>(w + 0xF4);
	wtype = ty > 13 ? 13 : ty;
	const HandType &H = kHandTypes[wtype];
	float weight = rd<float>(w + 0x90), dmg = (float)rd<u16>(w + 0xA0);
	int np = rd<u8>(w + 0x11E);
	if (np < 1) np = 1;
	if (!(weight > 0.01f && weight < 500)) weight = H.refWeight;
	float ks = 1, ss = 1;
	u32 id = refid((void *)w);
	wfeed = H.feed; wlabel = 0;
	for (int i = 0; i < kNumHandWeapons; i++)
	{
		const HandWeapon &HW = kHandWeapons[i];
		u32 mi = HW.plugin < 0 ? 0 : HW.plugin < kNumHandPlugins && HW.plugin < 8 ? g_handPlug[HW.plugin] : 255;
		if (mi < 255 && (id >> 24) == mi && (id & 0xFFFFFF) == HW.formId)
		{
			ks = HW.kickScale; ss = HW.swayScale; wlabel = HW.label;
			if (HW.feed >= 0) wfeed = HW.feed;
		}
	}
	u8 ra = rd<u8>(w + 0x103);
	if (ra >= 19 && ra <= 22) wfeed = 0;
	{
		static const char *kSn[5] = {"3D", "distant", "2D", "loop", "fail"};
		char b[600]; int at = 0; b[0] = 0;
		for (int i = 0; i < 5 && at < 540; i++)
		{
			u32 sn = ptr(w + 0x21C + 4 * i);
			if (!sn || formtype((void *)sn) != 0x0D) continue;
			const char *ed = (const char *)ptr(sn + 0x3C), *fp = (const char *)ptr(sn + 0x34);
			at += rt::fmt(b + at, sizeof b - at, " %s %s (%s)", kSn[i], ed ? ed : "?", fp ? fp : "?");
		}
		LOG("handling: weapon %x sounds:%s; reload anim %d, feed %s%s%s", id, at ? b : " none", ra, wfeed == 1 ? "box (+1, dropped rounds lost)" : wfeed == 2 ? "open bolt (dropped rounds lost)" : "fixed (reloads unchanged)", wlabel ? " - sheet: " : "", wlabel ? wlabel : "");
	}
	float power = clampf(rt::sqrt(dmg * np / rt::max(H.refDamage, 1.0f)), 0.5f, 2.2f);
	float heavy = clampf(rt::sqrt(weight / rt::max(H.refWeight, 0.1f)), 0.5f, 2.0f);
	kick = H.kickDeg * power / rt::sqrt(heavy) * ks;
	side = H.sideDeg * power / rt::sqrt(heavy) * ks;
	keep = H.keep; hz = rt::max(H.returnHz, 0.5f);
	sway = H.swayDeg * heavy * ss;
	lagSec = H.lagSec * heavy * ss;
	wpow = power; wheavy = heavy;
	wmass = clampf(weight / rt::max(TUNE(gunMassPounds), 0.5f), 0.4f, 3.5f);
	wAuto = (rd<u8>(w + 0x100) & 2) || ty == 6 || ty == 8;
	wOneHand = ty == 3 || ty == 4;
	wShotgun = np > 1;
	lagSec = H.lagSec * clampf(weight / rt::max(H.refWeight, 0.1f), 0.5f, 2.5f) * ss;
	LOG("handling: weapon %x type %d (%s) weight %.1f damage %.0f x%d, flags %x: kick %.2f deg side %.2f keep %.2f at %.1f Hz, sway %.2f deg, lag %.3f s",
		id, ty, H.label, weight, dmg, np, rd<u8>(w + 0x100), kick, side, keep, hz, sway, lagSec);
}

static GunAxis fk;
static GunAxis gh, gp, gr, gb;
static u32 pivRoot, pivot, wrist, pivParent;
static float pivR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, pivT[3];
static float turnH, turnP, lastAimH, lastAimP;
static bool haveAim, wasAim, gunLogged;
static float strafeS, strafeA;
static float extP, extR;
static float sprP, sprH, sprR, sprX, sprZ;
static float rlP, rlH, rlR, rlW;
static bool rlOn;
static int curAction = -1;
static GunAxis wfl;
static GunAxis wflH, wflR;
static GunAxis csP, csH, csR;
static u32 camRollN, camRollFor;
static u32 wristN, wristFor;
static bool isNodeObj(u32 o) { u32 vt = o ? ptr(o) : 0; return vt && ptr(vt + 0x0C) == 0x6815C0; }
static void dumpTree(u32 n, int depth, int &lines)
{
	if (lines >= 120 || depth > 14) return;
	char ind[40];
	int k = depth * 2 < 38 ? depth * 2 : 38;
	for (int i = 0; i < k; i++) ind[i] = ' ';
	ind[k] = 0;
	const char *nm = (const char *)ptr(n + 8);
	bool node = isNodeObj(n);
	u16 cnt = node ? rd<u16>(n + OFF_NODE_CHILDCOUNT) : 0;
	LOG("handling: 1st %s%s%s %d", ind, nm ? nm : "(no name)", node ? " node" : "", cnt);
	lines++;
	if (!node || (nm && (!memcmp(nm, "Bip01 L Finger", 14) || !memcmp(nm, "Bip01 R Finger", 14)))) return;
	u32 arr = ptr(n + OFF_NODE_CHILDREN);
	if (!arr || cnt > 64) return;
	for (u16 i = 0; i < cnt; i++) { u32 c = ptr(arr + 4 * i); if (c) dumpTree(c, depth + 1, lines); }
}
static bool rootHas(u32 root, u32 child)
{
	u32 arr = ptr(root + OFF_NODE_CHILDREN);
	u16 n = rd<u16>(root + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < n && i < 128; i++) if (ptr(arr + 4 * i) == child) return true;
	return false;
}
static void gunReset()
{
	gh.x = gh.v = gp.x = gp.v = gr.x = gr.v = gb.x = gb.v = 0;
	turnH = turnP = 0; haveAim = false;
}
static void sightRestore();
static void wristIdentity();
static void camRollIdentity();
static void coverIdentity();
static void pivotIdentity()
{
	sightRestore();
	wristIdentity(); wfl.x = wfl.v = 0; wflH.x = wflH.v = wflR.x = wflR.v = 0;
	camRollIdentity(); csP.x = csP.v = csH.x = csH.v = csR.x = csR.v = 0;
	coverIdentity();
	for (int i = 0; i < 9; i++) pivR[i] = (i % 4) ? 0.0f : 1.0f;
	pivT[0] = pivT[1] = pivT[2] = 0;
	if (pivot && pivParent && pivRoot == firstPersonNode(player()) && rootHas(pivParent, pivot))
	{
		memcpy((void *)(pivot + 0x34), pivR, 36);
		memcpy((void *)(pivot + 0x58), pivT, 12);
	}
}

static bool isAncestor(u32 a, u32 n) { for (int g = 0; n && g < 60; g++, n = ptr(n + 0x18)) if (n == a) return true; return false; }
static void insertPivot(u32 root)
{
	pivRoot = root; pivot = 0; pivParent = 0; gunLogged = false;
	for (int i = 0; i < 9; i++) pivR[i] = (i % 4) ? 0.0f : 1.0f;
	pivT[0] = pivT[1] = pivT[2] = 0;
	wrist = root ? findNamed(root, "Bip01 R Hand", 30) : 0;
	if (!wrist) { LOG("handling: no Bip01 R Hand under the first-person root %x - no gun inertia", root); return; }
	u32 cam1 = findNamed(root, "Camera1st", 30), parent = root;
	cam1N = cam1;
	if (cam1)
		for (u32 a = ptr(wrist + 0x18); a; a = ptr(a + 0x18))
		{
			if (isAncestor(a, cam1)) { parent = a; break; }
			if (a == root) break;
		}
	u32 target = wrist;
	for (int g = 0; target && ptr(target + 0x18) != parent && g < 40; g++) target = ptr(target + 0x18);
	u32 arr = ptr(parent + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(parent + OFF_NODE_CHILDCOUNT);
	int idx = -1;
	for (u16 i = 0; arr && i < cnt; i++) if (ptr(arr + 4 * i) == target) idx = i;
	if (!target || idx < 0 || !isNodeObj(target)) { LOG("handling: the wrist's branch isn't under %x - no gun inertia", parent); return; }
	u32 n = ((u32(__cdecl *)())0xA5F030)();
	if (!n) { LOG("handling: NiNode::Create failed"); return; }
	*(u32 *)(target + 4) += 1;
	((void(__thiscall *)(u32, u32, u32))ptr(ptr(parent) + 0xE0))(parent, (u32)idx, n);
	((void(__thiscall *)(u32, u32, bool))ptr(ptr(n) + 0xDC))(n, target, true);
	*(u32 *)(target + 4) -= 1;
	bool ok = ptr(n + 0x18) == parent && ptr(target + 0x18) == n && rootHas(parent, n) && !rootHas(parent, target);
	const char *tn = (const char *)ptr(target + 8), *pn = (const char *)ptr(parent + 8);
	LOG("handling: gun pivot %x inserted above %s, under %s (was child %d of %d) - %s; Camera1st %x %s", n, tn ? tn : "?", pn ? pn : "?", idx, cnt,
		ok ? "ok" : "NOT AS EXPECTED", cam1, cam1 && isAncestor(n, cam1) ? "TURNS WITH THE GUN" : "stays");
	pivot = ok ? n : 0;
	pivParent = ok ? parent : 0;
	u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
	if (camN)
	{
		char chain[200]; int at = 0; chain[0] = 0;
		for (u32 a = ptr(camN + 0x18); a && at < 180; a = ptr(a + 0x18))
		{
			const char *an = (const char *)ptr(a + 8);
			at += rt::fmt(chain + at, sizeof chain - at, "%s%s", at ? " < " : "", an ? an : "(no name)");
		}
		const float *cp = (const float *)(camN + 0x8C), *c1 = cam1 ? (const float *)(cam1 + 0x8C) : cp;
		LOG("handling: the camera %x hangs under: %s; it is %.1f units from Camera1st", camN, chain[0] ? chain : "(nothing)",
			rt::sqrt((cp[0] - c1[0]) * (cp[0] - c1[0]) + (cp[1] - c1[1]) * (cp[1] - c1[1]) + (cp[2] - c1[2]) * (cp[2] - c1[2])));
	}
	int lines = 0;
	static u32 dumped;
	if (dumped != root) { dumped = root; dumpTree(root, 0, lines); }
}
static void rotAbout(const float *a, float deg, float *m)
{
	float an = deg * rt::PI / 180, c = rt::cos(an), sn = rt::sin(an), t = 1 - c, x = a[0], y = a[1], z = a[2];
	m[0] = c + x * x * t;     m[1] = x * y * t - z * sn; m[2] = x * z * t + y * sn;
	m[3] = y * x * t + z * sn; m[4] = c + y * y * t;     m[5] = y * z * t - x * sn;
	m[6] = z * x * t - y * sn; m[7] = z * y * t + x * sn; m[8] = c + z * z * t;
}
static void mul3(const float *a, const float *b, float *o)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) o[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
static void toRoot(const float *W, const float *v, float *o)
{
	for (int i = 0; i < 3; i++) o[i] = W[i] * v[0] + W[3 + i] * v[1] + W[6 + i] * v[2];
}
struct Xf { float r[9], t[3], s; };
static Xf xfRead(u32 n, u32 off) { Xf x; memcpy(x.r, (void *)(n + off), 36); memcpy(x.t, (void *)(n + off + 0x24), 12); x.s = rd<float>(n + off + 0x30); if (!(x.s > 0.001f)) x.s = 1; return x; }
static Xf xfMul(const Xf &a, const Xf &b)
{
	Xf o; mul3(a.r, b.r, o.r);
	for (int i = 0; i < 3; i++) o.t[i] = a.s * (a.r[i * 3] * b.t[0] + a.r[i * 3 + 1] * b.t[1] + a.r[i * 3 + 2] * b.t[2]) + a.t[i];
	o.s = a.s * b.s; return o;
}
static Xf xfInv(const Xf &a)
{
	Xf o;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o.r[i * 3 + j] = a.r[j * 3 + i];
	o.s = 1 / a.s;
	for (int i = 0; i < 3; i++) o.t[i] = -(o.r[i * 3] * a.t[0] + o.r[i * 3 + 1] * a.t[1] + o.r[i * 3 + 2] * a.t[2]) * o.s;
	return o;
}
static u32 sightN, sightWritten;
static Xf sightOrig, sightLast;
static void sightRestore()
{
	if (sightN && sightWritten && pivRoot && pivRoot == firstPersonNode(player()) && pivot && isAncestor(pivot, sightN))
	{
		memcpy((void *)(sightN + 0x34), sightOrig.r, 36);
		memcpy((void *)(sightN + 0x58), sightOrig.t, 12);
	}
	sightN = 0; sightWritten = 0;
}
static void sightFix(TESObjectREFR *pl, bool aim)
{
	u32 sn = ptr((u32)pl + 0xE34);
	if (!aim || !sn || !pivot || !isAncestor(pivot, sn)) { if (sightN) sightRestore(); return; }
	if (sn != sightN) { sightRestore(); sightN = sn; sightOrig = xfRead(sn, 0x34); sightWritten = 0; }
	else
	{
		Xf now = xfRead(sn, 0x34);
		if (memcmp(now.r, sightLast.r, 36) || memcmp(now.t, sightLast.t, 12)) sightOrig = now;
	}
	u32 sp = ptr(sn + 0x18);
	if (!sp) return;
	Xf Wp = xfRead(pivot, 0x68), Wq = xfRead(sp, 0x68);
	Xf Pp = xfMul(xfInv(Wp), Wq);
	Xf G; memcpy(G.r, pivR, 36); memcpy(G.t, pivT, 12); G.s = 1;
	Xf Pp0 = Pp;
	{
		Xf Q;
		if (move::blindQ(Q.r, Q.t)) { Q.s = 1; Pp0 = xfMul(xfMul(xfMul(xfInv(Wp), Q), Wp), Pp); }
	}
	Xf L = xfMul(xfMul(xfMul(xfInv(Pp), xfInv(G)), Pp0), sightOrig);
	memcpy((void *)(sn + 0x34), L.r, 36);
	memcpy((void *)(sn + 0x58), L.t, 12);
	sightLast = L; sightLast.s = sightOrig.s; sightWritten = 1;
	static u32 told;
	if (told != sn) { told = sn; LOG("handling: sights node %x counter-turned while aiming (its parent %s)", sn, (const char *)ptr(sp + 8) ? (const char *)ptr(sp + 8) : "?"); }
}
static float wobHz(bool aim) { return TUNE(wobbleHz) * (aim ? 1.0f : clampf(1 / rt::sqrt(wmass), 0.55f, 1.0f)); }
static float camHz() { return TUNE(camShakeHz) / rt::sqrt(rt::sqrt(rt::max(wmass, 0.3f))); }
static float backHz(bool aim) { return aim ? 9.0f : clampf(9 / rt::sqrt(wmass), 4.0f, 12.0f); }
static void gunKick(float up, bool aim)
{
	float w = 2 * rt::PI * rt::max(TUNE(inertiaHz), 0.5f) / rt::sqrt(wmass);
	float hk = 1 + heat;
	heat = clampf(heat + TUNE(kickHeatPerShot), 0, TUNE(kickHeatMax));
	float g = aim ? TUNE(adsKickScale) * (wAuto ? 1.0f : TUNE(adsSemiKick)) : wOneHand ? TUNE(hipKickOneHand) : TUNE(hipKickTwoHand);
	float m = up * g / rt::sqrt(wmass);
	float tilt = 1;
	if (wShotgun) { m *= TUNE(shotgunLiftScale); tilt = TUNE(shotgunTiltScale) / rt::max(TUNE(shotgunLiftScale), 0.1f); }
	if (wAuto)
	{
		patPh += 0.7f + 0.5f * rnd();
		float sideDir = 0.8f * rt::sin(patPh) + 0.2f * (2 * rnd() - 1);
		gp.v += m * hk * TUNE(autoClimb) * w * 1.6f;
		gh.v += m * TUNE(autoSway) * sideDir * w * 1.6f;
		gr.v += m * 0.25f * sideDir * w * 1.6f;
	}
	else
	{
		float spread = aim ? 0.15f : wOneHand ? TUNE(hipKickSpread) : TUNE(hipKickSpread) * 0.4f;
		gp.v += m * hk * (0.8f + 0.4f * rnd()) * w * 1.6f;
		gh.v += m * spread * (2 * rnd() - 1) * w * 1.6f;
		gr.v += m * rt::max(spread, wShotgun ? 0.5f : 0.0f) * tilt * (wOneHand ? 1.0f : 0.4f) * (2 * rnd() - 1) * w * 1.6f;
	}
	if (aim)
	{
		float wf = 2 * rt::PI * TUNE(adsFlipHz);
		float semi = wAuto ? 1.0f : TUNE(adsSemiScale) * (wOneHand ? TUNE(adsOneHandFlip) : 1.0f) * (wShotgun ? TUNE(shotgunTiltScale) : 1.0f);
		flip.v += TUNE(adsFlipDeg) * semi * rt::min(2.0f, wpow) * hk / rt::sqrt(rt::max(wmass, 0.8f)) * (0.85f + 0.3f * rnd()) * wf * 1.6f;
	}
	float wb = TUNE(wobbleDeg) * (aim ? (wAuto ? 1.2f : TUNE(adsSemiWobble) * (wOneHand ? TUNE(adsOneHandWobble) : 1.0f)) : 1.4f) * (wShotgun ? TUNE(shotgunWobble) : 1.0f) * rt::min(2.0f, wpow) / rt::sqrt(wmass), ww = 2 * rt::PI * wobHz(aim);
	wbP.v += wb * (0.5f + 0.5f * rnd()) * ww; wbH.v += wb * (2 * rnd() - 1) * ww; wbR.v += wb * 1.5f * (2 * rnd() - 1) * ww;
	float back = aim ? 1 / rt::sqrt(wmass) : wOneHand ? TUNE(hipOneHandBack) / rt::sqrt(wmass) : rt::sqrt(wmass) * TUNE(hipHeavyBack);
	gb.v += TUNE(kickBackUnits) * rt::min(wpow, 1.4f) * hk * back * (wShotgun ? TUNE(shotgunBackScale) : 1.0f) * 2 * rt::PI * backHz(aim) * 1.6f;
	{
		float a = TUNE(camShakeDeg) * rt::min(2.0f, wpow) * (aim ? TUNE(camShakeAds) : 1.0f) * (wShotgun ? 1.6f : 1.0f) * rt::sqrt(rt::sqrt(rt::max(wmass, 0.3f)));
		float cw = 2 * rt::PI * camHz();
		{ float fw = 2 * rt::PI * TUNE(fovKickHz); fk.v -= TUNE(fovKickDeg) * rt::min(2.0f, wpow) * (aim ? TUNE(fovKickAds) : 1.0f) * fw * 1.7f; }
		csP.v += a * (0.6f + 0.4f * rnd()) * cw; csH.v += a * 0.5f * (2 * rnd() - 1) * cw; { float ra = TUNE(camRollDeg) * rt::min(2.0f, wpow) * (aim ? TUNE(camShakeAds) : 1.0f) * (wShotgun ? 1.6f : 1.0f) * rt::sqrt(rt::sqrt(rt::max(wmass, 0.3f))); float sd = 2 * rnd() - 1; sd = sd < 0 ? sd - 0.35f : sd + 0.35f; csR.v += ra * sd * cw; }
	}
	if (!aim && wOneHand)
	{
		float fw = 2 * rt::PI * TUNE(hipWristFlipHz) * 1.6f, fa = TUNE(hipWristFlipDeg) * rt::min(2.0f, wpow) * hk / rt::sqrt(rt::max(wmass, 0.5f));
		float side = 2 * rnd() - 1; side = side * rt::absf(side) * 0.6f + side * 0.4f;
		wfl.v += fa * (0.75f + 0.5f * rnd()) * fw;
		wflH.v += fa * TUNE(hipWristTwist) * side * fw;
		wflR.v += fa * TUNE(hipWristCant) * (side * 0.7f + 0.3f * (2 * rnd() - 1)) * fw;
	}
	shotsTotal++;
}
static u32 insertAboveH(u32 target)
{
	u32 parent = ptr(target + 0x18);
	if (!parent || !isNodeObj(parent) || !isNodeObj(target)) return 0;
	u32 arr = ptr(parent + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(parent + OFF_NODE_CHILDCOUNT);
	int idx = -1;
	for (u16 i = 0; arr && i < cnt; i++) if (ptr(arr + 4 * i) == target) idx = i;
	if (idx < 0) return 0;
	u32 n = ((u32(__cdecl *)())0xA5F030)();
	if (!n) return 0;
	*(u32 *)(target + 4) += 1;
	((void(__thiscall *)(u32, u32, u32))ptr(ptr(parent) + 0xE0))(parent, (u32)idx, n);
	((void(__thiscall *)(u32, u32, bool))ptr(ptr(n) + 0xDC))(n, target, true);
	*(u32 *)(target + 4) -= 1;
	return ptr(n + 0x18) == parent && ptr(target + 0x18) == n ? n : 0;
}
static void flagsLike(u32 n, u32 like)
{
	if (!n || !like) return;
	u32 &f = *(u32 *)(n + OFF_NIAV_FLAGS), lf = rd<u32>(like + OFF_NIAV_FLAGS);
	f = (f & ~0x7Eu) | (lf & 0x7Eu);
}
static void wristIdentity()
{
	static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z[3] = {0, 0, 0};
	if (wristN && wrist && pivRoot && pivRoot == firstPersonNode(player()) && ptr(wrist + 0x18) == wristN)
	{
		memcpy((void *)(wristN + 0x34), I, 36); memcpy((void *)(wristN + 0x58), Z, 12);
	}
}
static void wristTick(bool aim)
{
	if (!wrist || !pivot) return;
	u32 par = ptr(wrist + 0x18);
	if (wristFor != pivRoot || par != wristN || !wristN)
	{
		wristFor = pivRoot; wristN = 0;
		const char *nm = par ? (const char *)ptr(par + 8) : 0;
		bool kept = par && par != pivot && isNodeObj(par) && (!nm || !nm[0]);
		wristN = kept ? par : insertAboveH(wrist);
		const char *gn = wristN ? (const char *)ptr(ptr(wristN + 0x18) + 8) : 0;
		LOG("handling: wrist node %x %s above Bip01 R Hand (under %s) - pistols flip in the hand", wristN, kept ? "(ours already)" : wristN ? "put in" : "NOT put in", gn ? gn : "?");
		if (!wristN) return;
	}
	float a = wfl.x;
	static bool wasZero = true;
	if (rt::absf(a) + rt::absf(wflH.x) + rt::absf(wflR.x) < 0.005f) { if (!wasZero) { wristIdentity(); wasZero = true; } return; }
	wasZero = false;
	u32 fp = ptr(wristN + 0x18);
	if (!fp) return;
	const float *Wg = (const float *)(pivot + 0x68), *Wf = (const float *)(fp + 0x68);
	float rx[3] = {1, 0, 0}, ry[3] = {0, 1, 0}, rz[3] = {0, 0, 1}, Rv[9], WgT[9], WfT[9], T1[9], Rw[9], T2[9], Rl[9], Ra[9], Rb[9], Rc[9], Rab[9];
	rotAbout(rz, -wflH.x, Ra); rotAbout(rx, a, Rb); rotAbout(ry, wflR.x, Rc);
	mul3(Ra, Rb, Rab); mul3(Rab, Rc, Rv);
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { WgT[i * 3 + j] = Wg[j * 3 + i]; WfT[i * 3 + j] = Wf[j * 3 + i]; }
	mul3(Wg, Rv, T1); mul3(T1, WgT, Rw); mul3(WfT, Rw, T2); mul3(T2, Wf, Rl);
	const float *l = (const float *)(wrist + 0x58);
	float t[3];
	for (int i = 0; i < 3; i++) t[i] = l[i] - (Rl[i * 3] * l[0] + Rl[i * 3 + 1] * l[1] + Rl[i * 3 + 2] * l[2]);
	memcpy((void *)(wristN + 0x34), Rl, 36);
	memcpy((void *)(wristN + 0x58), t, 12);
}
static void camRollIdentity()
{
	static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z[3] = {0, 0, 0};
	if (camRollN && cam1N && pivRoot && pivRoot == firstPersonNode(player()) && ptr(cam1N + 0x18) == camRollN)
	{
		memcpy((void *)(camRollN + 0x34), I, 36); memcpy((void *)(camRollN + 0x58), Z, 12);
	}
}
static bool g_camOn, g_camSet;
static float g_camRoll, g_camSide, g_camKeep[13 + 12];
static float g_camRoll1;
static float g_camShift[3];
static u32 g_skyN[2]; static float g_skyKeep[2][3];
static bool under(u32 n, u32 top) { for (int d = 0; n && d < 64; d++, n = ptr(n + 0x18)) if (n == top) return true; return false; }
static void skyMove(const float *delta)
{
	u32 sky = ptr(0x11DEA20);
	const float *cp = g_camKeep + 13 + 9;
	g_skyN[0] = g_skyN[1] = 0;
	for (int k = 0; k < 2 && sky; k++)
	{
		u32 n = ptr(sky + 4 + 4 * k);
		if (!n || !isNodeObj(n) || (k == 1 && g_skyN[0] && (under(n, g_skyN[0]) || under(g_skyN[0], n)))) continue;
		const float *w = (const float *)(n + 0x8C);
		float dx = w[0] - cp[0], dy = w[1] - cp[1], dz = w[2] - cp[2];
		bool at = dx * dx + dy * dy + dz * dz < 300.0f * 300.0f;
		static int told[2];
		if (told[k] < 1) { told[k]++; LOG("handling: the sky's node +%x is %.0f units from the camera%s", 4 + 4 * k, rt::sqrt(dx * dx + dy * dy + dz * dz), at ? " - it goes with the camera's shoulder" : ""); }
		if (!at) continue;
		g_skyN[k] = n;
		memcpy(g_skyKeep[k], (void *)(n + 0x58), 12);
		u32 par = ptr(n + 0x18);
		float l[3] = {delta[0], delta[1], delta[2]};
		if (par) { Xf W = xfRead(par, 0x68); for (int i = 0; i < 3; i++) l[i] = (W.r[i] * delta[0] + W.r[3 + i] * delta[1] + W.r[6 + i] * delta[2]) / W.s; }
		for (int i = 0; i < 3; i++) *(float *)(n + 0x58 + 4 * i) += l[i];
		updateNode(n);
	}
}
static void tpCamIdentity() { g_camOn = false; }
static void tpCamRoll(float roll, float side) { g_camOn = true; g_camRoll = roll; g_camSide = side; }
static u32 sceneCam() { u32 sg = ptr(0x11DEB7C); return sg ? ptr(sg + 0xAC) : 0; }
static void camApply()
{
	u32 camN = sceneCam();
	bool shift = rt::absf(g_camShift[0]) + rt::absf(g_camShift[1]) + rt::absf(g_camShift[2]) > 0.01f || rt::absf(g_camRoll1) > 0.0005f;
	bool rs = g_camOn && (rt::absf(g_camRoll) >= 0.001f || rt::absf(g_camSide) >= 0.01f);
	if (!camN || g_camSet || (!rs && !shift)) return;
	TESObjectREFR *pl = player();
	if (rd<u32>(0x11F2250 + 8) || menumode() || !pl) { g_camOn = false; return; }
	if (rs && !thirdPerson(pl)) { g_camOn = false; rs = false; }
	if (!rs && !shift) return;
	memcpy(g_camKeep, (void *)(camN + 0x34), 13 * 4); memcpy(g_camKeep + 13, (void *)(camN + 0x68), 12 * 4);
	g_camSet = true;
	const float *cl = g_camKeep, *cp = g_camKeep + 9;
	float fw[3] = {cl[0], cl[3], cl[6]}, rg[3] = {cl[2], cl[5], cl[8]}, R[9], L[9], t[3];
	if (rs) { rotAbout(fw, g_camRoll, R); mul3(R, cl, L); }
	else if (rt::absf(g_camRoll1) > 0.0005f && !thirdPerson(pl)) { rotAbout(fw, g_camRoll1, R); mul3(R, cl, L); }
	else memcpy(L, cl, 36);
	for (int i = 0; i < 3; i++) t[i] = cp[i] + (rs ? rg[i] * g_camSide : 0) + (shift ? g_camShift[i] : 0);
	memcpy((void *)(camN + 0x34), L, 36); memcpy((void *)(camN + 0x58), t, 12);
	updateNode(camN);
	float dw[3] = {((float *)(camN + 0x8C))[0] - g_camKeep[13 + 9], ((float *)(camN + 0x8C))[1] - g_camKeep[13 + 10], ((float *)(camN + 0x8C))[2] - g_camKeep[13 + 11]};
	skyMove(dw);
	static int told;
	if (told < 2 && rt::absf(g_camSide) > 1)
	{
		told++;
		const float *w = (const float *)(camN + 0x8C);
		float dx = w[0] - g_camKeep[13 + 9], dy = w[1] - g_camKeep[13 + 10], dz = w[2] - g_camKeep[13 + 11];
		LOG("handling: third-person camera moved %.1f units sideways (wanted %.1f), rolled %.1f deg, as the frame drew", rt::sqrt(dx * dx + dy * dy + dz * dz), rt::absf(g_camSide), g_camRoll);
	}
}
static void camRestore()
{
	u32 camN = sceneCam();
	if (!g_camSet) return;
	g_camSet = false;
	if (!camN) return;
	memcpy((void *)(camN + 0x34), g_camKeep, 13 * 4); memcpy((void *)(camN + 0x68), g_camKeep + 13, 12 * 4);
	updateNode(camN);
	for (int k = 0; k < 2; k++) if (g_skyN[k]) { memcpy((void *)(g_skyN[k] + 0x58), g_skyKeep[k], 12); updateNode(g_skyN[k]); g_skyN[k] = 0; }
}
static bool toScreen(const float *p, float W, float H, float *sx, float *sy)
{
	u32 camN = sceneCam();
	if (!camN) return false;
	const float *m = (const float *)(camN + 0x9C), *vp = (const float *)(camN + 0x100);
	float hw = p[0] * m[12] + p[1] * m[13] + p[2] * m[14] + m[15];
	if (hw <= 0.001f) return false;
	float x = (p[0] * m[0] + p[1] * m[1] + p[2] * m[2] + m[3]) / hw, y = (p[0] * m[4] + p[1] * m[5] + p[2] * m[6] + m[7]) / hw;
	float l = vp[0], r = vp[1], t = vp[2], b = vp[3];
	if (!(r > l) || !(t > b)) { l = 0; r = 1; t = 1; b = 0; }
	*sx = (x * (r - l) * 0.5f + (l + r) * 0.5f) * W;
	*sy = (1 - (y * (t - b) * 0.5f + (t + b) * 0.5f)) * H;
	return true;
}
struct Tracer { float a[3], b[3]; double t0; float dur; };
static Tracer g_tr[24];
static int g_trN;
static void addTracer(const float *a, const float *b)
{
	if (TUNE(tracerOn) <= 0) return;
	if (g_trN == 24) { memmove(g_tr, g_tr + 1, sizeof(Tracer) * 23); g_trN--; }
	Tracer &t = g_tr[g_trN++];
	memcpy(t.a, a, 12); memcpy(t.b, b, 12); t.t0 = rt::now();
	float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
	t.dur = rt::clamp(rt::sqrt(dx * dx + dy * dy + dz * dz) / rt::max(TUNE(tracerSpeed), 1000.0f), 0.03f, 0.4f);
}
static int s_wpOff = -1;
static u32 weapProj(u32 w)
{
	if (!w || formtype((void *)w) != 0x28) return 0;
	if (s_wpOff < 0)
	{
		for (u32 o = 0x30; o < 0x400 && s_wpOff < 0; o += 4)
		{
			if (!readable(w + o, 4)) break;
			u32 v = ptr(w + o);
			if (v && readable(v, 0x10) && rd<u8>(v + 4) == 0x33 && (u32)lookup(rd<u32>(v + 0xC)) == v) s_wpOff = (int)o;
		}
		LOG("ghosts: a weapon's projectile is %s", s_wpOff >= 0 ? "found" : "NOT found - friends' shots show no flash");
		if (s_wpOff >= 0) LOG("ghosts: (at the weapon record's +%x)", s_wpOff);
		if (s_wpOff < 0) { s_wpOff = 0; return 0; }
	}
	if (!s_wpOff) return 0;
	u32 v = ptr(w + s_wpOff);
	return v && readable(v, 0xD0) && rd<u8>(v + 4) == 0x33 ? v : 0;
}
static const char *projFlash(u32 proj, float *sec)
{
	*sec = 0.06f;
	if (!proj) return 0;
	float d = rd<float>(proj + 0x8C);
	if (d > 0.01f && d < 1) *sec = rt::clamp(d, 0.03f, 0.2f);
	u32 p = ptr(proj + 0xB8);
	if (!p || !readable(p, 8)) return 0;
	const char *c = (const char *)p;
	int n = 0; while (n < 200 && c[n]) n++;
	if (n < 5 || n >= 200 || (c[n - 4] != '.') || (c[n - 3] | 32) != 'n' || (c[n - 2] | 32) != 'i' || (c[n - 1] | 32) != 'f') return 0;
	return c;
}
static void camRollTick()
{
	if (!cam1N || !pivRoot || TUNE(camRollDeg) <= 0) return;
	u32 par = ptr(cam1N + 0x18);
	if (camRollFor != pivRoot || par != camRollN || !camRollN)
	{
		camRollFor = pivRoot; camRollN = 0;
		const char *nm = par ? (const char *)ptr(par + 8) : 0;
		bool kept = par && par != pivot && isNodeObj(par) && (!nm || !nm[0]);
		camRollN = kept ? par : insertAboveH(cam1N);
		const char *gn = camRollN ? (const char *)ptr(ptr(camRollN + 0x18) + 8) : 0;
		LOG("handling: camera roll node %x %s above Camera1st (under %s)", camRollN, kept ? "(ours already)" : camRollN ? "put in" : "NOT put in", gn ? gn : "?");
		if (!camRollN) return;
	}
	float roll = csR.x + TUNE(strafeCantDeg) * strafeS;
	static bool wasZero = true;
	if (rt::absf(roll) < 0.003f) { if (!wasZero) { camRollIdentity(); wasZero = true; } return; }
	wasZero = false;
	float f[3] = {0, 1, 0}, R[9];
	rotAbout(f, roll, R);
	const float *l = (const float *)(cam1N + 0x58);
	float t[3];
	for (int i = 0; i < 3; i++) t[i] = l[i] - (R[i * 3] * l[0] + R[i * 3 + 1] * l[1] + R[i * 3 + 2] * l[2]);
	memcpy((void *)(camRollN + 0x34), R, 36);
	memcpy((void *)(camRollN + 0x58), t, 12);
}
static u32 coverN, coverFor;
static GunAxis cvS, cvU;
static float cvTS, cvTU;
static float cvFS, cvFU;
static float cvOff[3];
static bool cvOn;
static float cvClearT, cvSearchT, cvLoseT, cvLogT, cvNoPeekT;
static int cvLogs;
static bool blOn;
static GunAxis bl;
static float blTS, blTU, blS, blU;
static float blW[3];
static void coverIdentity()
{
	static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z[3] = {0, 0, 0};
	if (coverN && coverFor && coverFor == firstPersonNode(player()) && isNodeObj(coverN)) { memcpy((void *)(coverN + 0x34), I, 36); memcpy((void *)(coverN + 0x58), Z, 12); }
	cvS.x = cvS.v = cvU.x = cvU.v = cvTS = cvTU = cvFS = cvFU = 0; cvOff[0] = cvOff[1] = cvOff[2] = 0; cvOn = false; cvClearT = cvSearchT = cvLoseT = 0;
	blOn = false; bl.x = bl.v = 0; blTS = blTU = blS = blU = 0; blW[0] = blW[1] = blW[2] = 0;
	move::blindArmsOff();
}
static bool cvBlocked(const float *a, const float *b, float *fr)
{
	u32 o = 0; float f = 1;
	if (!rayHitObject(a, b, &f, &o)) { *fr = 1; return false; }
	if (TESObjectREFR *r = o ? refOfNode(o) : 0) { u8 ty = formtype((void *)r); if (ty == 0x3B || ty == 0x3C) { *fr = f; return false; } }
	*fr = f; return true;
}
static bool cvSpot(const float *E, const float *rt3, const float *D, float s, float u, float len, int &rays, float fwd = 0)
{
	float P[3], fr;
	for (int i = 0; i < 3; i++) P[i] = E[i] + rt3[i] * s + (i == 2 ? u : 0);
	float m = rt::sqrt(s * s + u * u);
	if (m > 0.5f)
	{
		float k = (m + TUNE(coverHeadUnits)) / m, H[3];
		for (int i = 0; i < 3; i++) H[i] = E[i] + (P[i] - E[i]) * k;
		rays++;
		if (cvBlocked(E, H, &fr)) return false;
	}
	if (fwd > 0) { float h = rt::sqrt(D[0] * D[0] + D[1] * D[1]); if (h > 0.1f) { P[0] += D[0] / h * fwd; P[1] += D[1] / h * fwd; } }
	float B[3] = {P[0] + D[0] * len, P[1] + D[1] * len, P[2] + D[2] * len};
	rays++;
	return !cvBlocked(P, B, &fr);
}
static bool cvSearch(const float *E, const float *rt3, const float *D, float len, bool crouch, float *bs, float *bu, int &rays)
{
	float st = rt::max(TUNE(coverStepUnits), 2.0f);
	float upMax = crouch ? TUNE(coverUpCrouch) : TUNE(coverUpStand), dnMax = crouch ? TUNE(coverDownCrouch) : TUNE(coverDownStand), sdMax = TUNE(coverSideUnits);
	struct C { float s, u, c; } c[160];
	int n = 0;
	auto add = [&](float s, float u) {
		if (n >= 160) return;
		float k = (u > 0 ? u * TUNE(coverCostUp) : -u * TUNE(coverCostDown)) + rt::absf(s) * TUNE(coverCostSide);
		if (cvOn) { float ds = s - cvTS, du = u - cvTU; k += TUNE(coverCostMove) * rt::sqrt(ds * ds + du * du); }
		if (cvOn && cvTS * s < 0) k += 2 * st;
		int i = n++;
		while (i > 0 && c[i - 1].c > k) { c[i] = c[i - 1]; i--; }
		c[i] = {s, u, k};
	};
	for (float u = st; u <= upMax + 0.01f; u += st) add(0, u);
	for (float s = st; s <= sdMax + 0.01f; s += st)
	{
		add(s, 0); add(-s, 0);
		for (float u = st; u <= upMax + 0.01f; u += st) { add(s, u); add(-s, u); }
		for (float u = st; u <= dnMax + 0.01f; u += st) { add(s, -u); add(-s, -u); }
	}
	for (float u = st; u <= dnMax + 0.01f; u += st) add(0, -u);
	int budget = (int)rt::max(TUNE(coverMaxRays), 8.0f);
	for (int i = 0; i < n && rays < budget; i++)
		if (cvSpot(E, rt3, D, c[i].s, c[i].u, len, rays))
		{
			float mg = TUNE(coverMarginUnits);
			float s = c[i].s, u = c[i].u;
			if (s != 0) s = rt::clamp(s + (s > 0 ? mg : -mg), -sdMax, sdMax);
			if (u > 0) u = rt::min(u + mg, upMax); else if (u < 0) u = rt::max(u - mg, -dnMax);
			*bs = s; *bu = u; return true;
		}
	return false;
}
static bool blSearch(const float *E, const float *rt3, const float *D, float len, bool crouch, float *bs, float *bu, int &rays)
{
	float st = rt::max(TUNE(coverStepUnits), 2.0f), fwd = TUNE(blindForwardUnits);
	float upMax = crouch ? TUNE(coverUpCrouch) : TUNE(coverUpStand), sdMax = TUNE(coverSideUnits);
	float h = rt::sqrt(D[0] * D[0] + D[1] * D[1]), fh[3] = {h > 0.1f ? D[0] / h : 0, h > 0.1f ? D[1] / h : 0, 0};
	auto reach = [&](float s, float u) { float tb[3]; for (int i = 0; i < 3; i++) tb[i] = rt3[i] * s + fh[i] * fwd + (i == 2 ? u : 0); return move::blindReachOk(tb); };
	float mg = TUNE(coverMarginUnits);
	for (float u = upMax + st; u <= TUNE(blindUpUnits) + 0.01f; u += st)
	{
		if (!reach(0, u)) break;
		if (cvSpot(E, rt3, D, 0, u, len, rays, fwd)) { *bs = 0; *bu = reach(0, u + mg) ? u + mg : u; return true; }
	}
	for (float s = sdMax + st; s <= TUNE(blindSideUnits) + 0.01f; s += st)
		for (int k = 0; k < 2; k++)
		{
			float ss = (k == 0) == (blTS >= 0) ? s : -s;
			if (!reach(ss, 0)) continue;
			if (cvSpot(E, rt3, D, ss, 0, len, rays, fwd)) { float s2 = ss + (ss > 0 ? mg : -mg); *bs = reach(s2, 0) ? s2 : ss; *bu = 0; return true; }
		}
	return false;
}
static void coverTick(float dt, bool aim, bool sneak, const float *aimT, TESObjectREFR *pl)
{
	u32 root = firstPersonNode(pl);
	u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
	if (!root || !camN || !cam1N || TUNE(coverAim) <= 0) { if (cvOn || blOn || rt::absf(cvS.x) + rt::absf(cvU.x) + bl.x > 0.01f) coverIdentity(); return; }
	u32 look = findNamed(root, "Bip01 Looking", 30);
	if (!look || !isAncestor(look, cam1N) || (wrist && !isAncestor(look, wrist))) return;
	u32 par = ptr(look + 0x18);
	if (coverFor != root || !coverN || par != coverN)
	{
		coverFor = root; coverN = 0;
		const char *nm = par ? (const char *)ptr(par + 8) : 0;
		bool kept = par && isNodeObj(par) && (!nm || !nm[0]) && par != pivot && par != camRollN && par != wristN;
		coverN = kept ? par : insertAboveH(look);
		coverIdentity();
		coverFor = root;
		const char *gn = coverN ? (const char *)ptr(ptr(coverN + 0x18) + 8) : 0;
		LOG("cover: node %x %s above Bip01 Looking (under %s)", coverN, kept ? "(ours already)" : coverN ? "put in" : "NOT put in", gn ? gn : "?");
		if (!coverN) return;
	}
	float cp = rt::cos(aimT[0]), D[3] = {rt::sin(aimT[1]) * cp, rt::cos(aimT[1]) * cp, -rt::sin(aimT[0])};
	float rt3[3] = {rt::cos(aimT[1]), -rt::sin(aimT[1]), 0}, fh[3] = {rt::sin(aimT[1]), rt::cos(aimT[1]), 0};
	const float *cw = (const float *)(camN + 0x8C);
	float E[3] = {cw[0] - cvOff[0], cw[1] - cvOff[1], cw[2] - cvOff[2]};
	bool want = (aim || TUNE(coverAimHip) > 0) && sprint::modeNow() == 0 && move::stateNow() == 0 && curAction != 0 && curAction != 1
		&& aimT[0] < TUNE(coverMaxDownDeg) * rt::PI / 180;
	int rays = 0;
	float probe = TUNE(coverProbeUnits), dHit = 0;
	if (want)
	{
		float fr, B[3] = {E[0] + D[0] * probe, E[1] + D[1] * probe, E[2] + D[2] * probe};
		rays++;
		bool blocked = cvBlocked(E, B, &fr);
		dHit = fr * probe;
		float len = dHit + TUNE(coverPastUnits);
		if (!blocked)
		{
			cvNoPeekT = 0;
			if ((cvOn || blOn) && (cvClearT += dt) > TUNE(coverReleaseSec)) { cvOn = blOn = false; cvTS = cvTU = 0; LOG("cover: clear from where you stand - back"); }
		}
		else
		{
			cvClearT = 0;
			if (cvOn) cvNoPeekT = 0; else cvNoPeekT += dt;
			bool keep = (cvOn && cvSpot(E, rt3, D, cvTS, cvTU, len, rays)) || (blOn && cvSpot(E, rt3, D, blTS, blTU, len, rays, TUNE(blindForwardUnits)));
			if (keep && blOn && (cvSearchT -= dt) <= 0)
			{
				cvSearchT = TUNE(coverRetrySec);
				float s = 0, u = 0;
				if (cvSearch(E, rt3, D, len, sneak, &s, &u, rays)) { blOn = false; cvOn = true; cvTS = s; cvTU = u; keep = true; LOG("cover: a spot to lean to - blind fire down (side %.0f up %.0f)", s, u); }
			}
			if (keep) cvLoseT = 0;
			else if ((cvSearchT -= dt) <= 0)
			{
				cvSearchT = TUNE(coverSearchSec);
				float s = 0, u = 0;
				bool wasC = cvOn, wasB = blOn;
				const char *what = 0;
				if (cvSearch(E, rt3, D, len, sneak, &s, &u, rays)) { cvOn = true; blOn = false; cvTS = s; cvTU = u; cvLoseT = 0; what = "spot"; }
				else if (aim && TUNE(blindFire) > 0 && dHit <= TUNE(blindMaxDistUnits) && !wasC && cvNoPeekT >= TUNE(blindDelaySec) && blSearch(E, rt3, D, len, sneak, &s, &u, rays))
				{
					blOn = true; cvOn = false; cvTS = cvTU = 0; blTS = s; blTU = u; cvLoseT = 0; what = s == 0 ? "blind fire over" : "blind fire round";
				}
				else
				{
					cvSearchT = TUNE(coverRetrySec);
					if ((wasC || wasB) && (cvLoseT += TUNE(coverRetrySec)) > TUNE(coverLoseSec)) { cvOn = blOn = false; cvTS = cvTU = 0; what = "nothing - back"; }
				}
				if (cvLogs < 60 && what && (what[0] != 's' || !wasC || cvTS != s || cvTU != u))
				{
					cvLogs++;
					LOG("cover: blocked %.0f units ahead%s -> %s side %.0f up %.0f (%d rays)%s", dHit, sneak ? " (crouched)" : "", what, blOn ? blTS : cvTS, blOn ? blTU : cvTU, rays, aim ? "" : " hip");
				}
			}
		}
	}
	else { cvNoPeekT = 0; if (cvOn || blOn) { cvOn = blOn = false; cvTS = cvTU = 0; } }
	float kf = clampf(dt * TUNE(coverSmoothHz) * 2 * rt::PI, 0, 1);
	cvFS += (cvTS - cvFS) * kf; cvFU += (cvTU - cvFU) * kf;
	float hz = rt::max(TUNE(coverHz), 0.5f), dm = TUNE(coverDamping);
	spring(cvS.x, cvS.v, cvFS, hz, dm, dt);
	spring(cvU.x, cvU.v, cvFU, hz, dm, dt);
	if (blOn) { blS += (blTS - blS) * kf; blU += (blTU - blU) * kf; }
	spring(bl.x, bl.v, blOn ? 1.0f : 0.0f, rt::max(TUNE(blindHz), 0.5f), 1.0f, dt);
	bl.x = clampf(bl.x, 0, 1.0f);
	blW[0] = blW[1] = blW[2] = 0;
	if (bl.x > 0.002f)
	{
		float tb[3];
		for (int i = 0; i < 3; i++) tb[i] = bl.x * (rt3[i] * blS + fh[i] * TUNE(blindForwardUnits) + (i == 2 ? blU : 0));
		Xf Lw = xfRead(look, 0x68);
		float Fv[3] = {Lw.r[1], Lw.r[4], Lw.r[7]};
		float side = blS > 0.5f ? 1.0f : blS < -0.5f ? -1.0f : 0.0f;
		float broll = bl.x * (side != 0 ? side * TUNE(blindSideRollDeg) : -TUNE(blindRollDeg));
		if (!move::blindArms(pl, bl.x, cw, D, Fv, broll, tb, blW)) blW[0] = blW[1] = blW[2] = 0;
	}
	else move::blindArmsOff();
	if (!cvOn && bl.x < 0.002f && rt::absf(cvS.x) + rt::absf(cvU.x) + rt::absf(cvS.v) + rt::absf(cvU.v) < 0.02f) { if (cvOff[0] != 0 || cvOff[1] != 0 || cvOff[2] != 0) { static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, Z[3] = {0, 0, 0}; memcpy((void *)(coverN + 0x34), I, 36); memcpy((void *)(coverN + 0x58), Z, 12); cvOff[0] = cvOff[1] = cvOff[2] = 0; cvS.x = cvS.v = cvU.x = cvU.v = 0; } return; }
	u32 P = ptr(coverN + 0x18);
	if (!P) return;
	Xf Wp = xfRead(P, 0x68), Cn = xfRead(coverN, 0x34);
	const float *c1 = (const float *)(cam1N + 0x8C);
	float q[3] = {c1[0] - Wp.t[0], c1[1] - Wp.t[1], c1[2] - Wp.t[2]}, xn[3], cl[3];
	toRoot(Wp.r, q, xn);
	for (int i = 0; i < 3; i++) xn[i] = xn[i] / Wp.s - Cn.t[i];
	toRoot(Cn.r, xn, cl);
	float off[3] = {rt3[0] * cvS.x, rt3[1] * cvS.x, cvU.x}, ol[3], fl[3];
	toRoot(Wp.r, off, ol);
	for (int i = 0; i < 3; i++) ol[i] /= Wp.s;
	toRoot(Wp.r, D, fl);
	float fn = rt::sqrt(fl[0] * fl[0] + fl[1] * fl[1] + fl[2] * fl[2]);
	if (fn < 0.5f || cl[0] * cl[0] + cl[1] * cl[1] + cl[2] * cl[2] > 300.0f * 300.0f) return;
	for (int i = 0; i < 3; i++) fl[i] /= fn;
	float sdMax = rt::max(TUNE(coverSideUnits), 1.0f);
	float roll = TUNE(coverLeanRollDeg) * clampf(cvS.x / sdMax, -1.2f, 1.2f), R[9], t[3];
	rotAbout(fl, roll, R);
	if (bl.x > 0.002f)
	{
		float rw[3] = {rt3[0], rt3[1], 0}, rl[3], Tl[9], TR[9];
		toRoot(Wp.r, rw, rl);
		rotAbout(rl, bl.x * TUNE(blindTiltDeg), Tl);
		mul3(Tl, R, TR); memcpy(R, TR, 36);
	}
	for (int i = 0; i < 3; i++) t[i] = cl[i] - (R[i * 3] * cl[0] + R[i * 3 + 1] * cl[1] + R[i * 3 + 2] * cl[2]) + ol[i];
	memcpy((void *)(coverN + 0x34), R, 36);
	memcpy((void *)(coverN + 0x58), t, 12);
	cvOff[0] = off[0]; cvOff[1] = off[1]; cvOff[2] = off[2];
	if ((cvLogT += dt) > 2 && (cvOn || blOn)) { cvLogT = 0; LOG("cover: at side %.1f up %.1f (heading to %.0f %.0f), roll %.1f deg; blind %.2f", cvS.x, cvU.x, cvTS, cvTU, roll, bl.x); }
}
static void gunTick(float dt, double t, const float *aimT, bool aim, bool sneak, float mv, float stepPh, TESObjectREFR *pl)
{
	bool tp = thirdPerson(pl);
	static bool wasTp;
	if (tp != wasTp)
	{
		wasTp = tp;
		if (tp) pivotIdentity(); else tpgun::identity();
		LOG("handling: now %s person", tp ? "third" : "first");
	}
	if (!tp)
	{
		u32 root = firstPersonNode(pl);
		if (!root) return;
		if (root != pivRoot || (pivot && !rootHas(pivParent, pivot))) insertPivot(root);
		if (!pivot || !pivParent) return;
	}
	const float R2D = 180 / rt::PI;
	float upSign = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f;
	float elev = upSign < 0 ? -aimT[0] : aimT[0];
	if (haveAim && dt > 0)
	{
		float rh = clampf(wrapPi(aimT[1] - lastAimH) * R2D / dt, -5000, 5000), rp = clampf((elev - lastAimP) * R2D / dt, -5000, 5000);
		float k = clampf(dt / rt::max(TUNE(inertiaRateSmoothSec), 0.005f), 0, 1);
		turnH += (rh - turnH) * k; turnP += (rp - turnP) * k;
	}
	lastAimH = aimT[1]; lastAimP = elev; haveAim = true;
	heat = rt::max(0.0f, heat - dt * TUNE(kickHeatDecay));
	if (aim != wasAim) { wasAim = aim; adsPending = 2; adsPendIn = aim; }
	if (adsPending > 0 && --adsPending == 0)
	{
		float a = (g_smoothAds ? TUNE(adsSettleDeg) : 0) * clampf(rt::sqrt(wmass), 0.8f, 1.4f);
		if (adsPendIn) { float w9 = 2 * rt::PI * 4; gp.v -= a * w9; gh.v += a * 0.4f * (2 * rnd() - 1) * w9; gr.v -= a * (0.8f + 0.4f * rnd()) * w9; }
		else { float w9 = 2 * rt::PI * 4; gp.v -= a * 0.5f * w9; gr.v += a * 0.6f * (2 * rnd() - 1) * w9; }
		adsT = g_smoothAds ? TUNE(adsTransitionSec) : 0;
		u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0, sight = ptr((u32)pl + 0xE34);
		if (!tp && camN && cam1N && pivParent)
		{
			const float *cp = (const float *)(camN + 0x8C), *c1 = (const float *)(cam1N + 0x8C);
			float dx = cp[0] - c1[0], dy = cp[1] - c1[1], dz = cp[2] - c1[2];
			const float *cr = (const float *)(camN + 0x68), *lr = (const float *)(pivParent + 0x68);
			float dotF = cr[0] * lr[1] + cr[3] * lr[4] + cr[6] * lr[7];
			const char *sn = sight ? (const char *)ptr(sight + 8) : 0;
			LOG("handling: sights %s: camera %.1f units from Camera1st; camera axis0 . Looking forward %.2f; sights node %s %s", adsPendIn ? "in" : "out",
				rt::sqrt(dx * dx + dy * dy + dz * dz), dotF, sn ? sn : "(none)", sight && pivot && isAncestor(pivot, sight) ? "under our pivot" : "not under our pivot");
		}
	}
	float S = TUNE(handlingStrength);
	float lagK = TUNE(inertiaTurnSec) * rt::sqrt(wmass) * S, mx = TUNE(inertiaMaxDeg);
	float st = (float)(t - 1000.0 * (int)(t / 1000.0));
	float br = sway * TUNE(inertiaSwayScale) * (aim ? TUNE(handlingAimSway) : 1) * (sneak ? TUNE(handlingSneakSway) : 1) * S;
	float mvA = TUNE(inertiaMoveDeg) * mv * (aim ? 0.35f : 1) * S;
	auto soft = [](float x, float m) { float y = x / m; return m * y / rt::sqrt(1 + y * y); };
	float tH = rlH + sprH + soft(-lagK * turnH, mx) + br * (0.7f * rt::sin(2 * rt::PI * 0.19f * st + 0.5f) + 0.3f * rt::sin(2 * rt::PI * 0.43f * st)) + mvA * rt::sin(stepPh);
	float tP = rlP + extP + sprP + soft(-lagK * turnP, mx) + br * (0.6f * rt::sin(2 * rt::PI * 0.27f * st + 2) + 0.4f * rt::sin(2 * rt::PI * 0.33f * st)) - mvA * 0.6f * rt::absf(rt::sin(stepPh));
	float tR = rlR + extR + sprR - TUNE(inertiaRoll) * gh.x + strafeS * 6.0f * S;
	float hzI = rt::max(TUNE(inertiaHz), 0.5f) / rt::sqrt(wmass), dmp = clampf(TUNE(inertiaDamping) + 0.08f * wmass, 0.2f, 0.9f);
	if (wAuto) dmp = rt::max(dmp, TUNE(autoDamping));
	if (adsT > 0) { adsT -= dt; hzI *= 0.6f; dmp = rt::max(dmp, 0.55f); }
	{
		float wh = wobHz(aim), wd = TUNE(wobbleDamping);
		spring(wbH.x, wbH.v, 0, wh, wd, dt); spring(wbP.x, wbP.v, 0, wh, wd, dt); spring(wbR.x, wbR.v, 0, wh * 0.8f, wd, dt);
	}
	spring(gh.x, gh.v, tH, hzI, dmp, dt);
	spring(gp.x, gp.v, tP, hzI, dmp, dt);
	spring(gr.x, gr.v, tR, hzI * 0.9f, dmp, dt);
	spring(gb.x, gb.v, 0, backHz(aim), 0.6f, dt);
	spring(wfl.x, wfl.v, 0, TUNE(hipWristFlipHz), TUNE(hipWristFlipDamping), dt);
	spring(fk.x, fk.v, 0, TUNE(fovKickHz), TUNE(fovKickDamping), dt); fk.x = clampf(fk.x, -6, 6); g_fovKick = fk.x;
	{ float ch = camHz(), cd = TUNE(camShakeDamping); spring(csP.x, csP.v, 0, ch, cd, dt); spring(csH.x, csH.v, 0, ch, cd, dt); spring(csR.x, csR.v, 0, ch * 0.8f, cd, dt); csP.x = clampf(csP.x, -6, 6); csH.x = clampf(csH.x, -6, 6); csR.x = clampf(csR.x, -8, 8); }
	wfl.x = clampf(wfl.x, -35, 35);
	spring(wflH.x, wflH.v, 0, TUNE(hipWristFlipHz) * 0.85f, TUNE(hipWristFlipDamping), dt); spring(wflR.x, wflR.v, 0, TUNE(hipWristFlipHz) * 0.8f, TUNE(hipWristFlipDamping), dt);
	wflH.x = clampf(wflH.x, -25, 25); wflR.x = clampf(wflR.x, -35, 35);
	gh.x = clampf(gh.x, -mx * 1.6f, mx * 1.6f); gp.x = clampf(gp.x, -mx * 1.6f, mx * 1.6f); gr.x = clampf(gr.x, -50, 50);
	gb.x = clampf(gb.x, -2, TUNE(kickBackMaxUnits));
	spring(flip.x, flip.v, 0, TUNE(adsFlipHz), TUNE(adsFlipDamping), dt);
	flip.x = clampf(flip.x, -25, 25);
	if (tp) { tpgun::apply(pl, aimT, dt, aim, sneak); return; }
	if (net::active()) tpgun::apply(pl, aimT, dt, aim, sneak, true);
	else tpgun::identity();
	const float *W = (const float *)(pivParent + 0x68), *WT = (const float *)(pivParent + 0x8C);
	float ws = rd<float>(pivParent + 0x98);
	if (!(ws > 0.01f)) ws = 1;
	float f[3] = {0, 1, 0}, r[3] = {1, 0, 0}, u[3] = {0, 0, 1};
	float A[9], B[9], C[9], AB[9], R[9];
	rotAbout(u, -(gh.x + wbH.x), A); rotAbout(r, gp.x + wbP.x, B); rotAbout(f, gr.x + wbR.x, C);
	mul3(A, B, AB); mul3(AB, C, R);
	float F[9], RF[9];
	rotAbout(r, flip.x, F);
	mul3(R, F, RF);
	float fwdP = TUNE(adsFlipPivotUnits) * (wOneHand ? 1.0f : TUNE(adsFlipLongArm));
	const float *pw = (const float *)(wrist + 0x8C);
	float d[3] = {pw[0] - WT[0], pw[1] - WT[1], pw[2] - WT[2]}, q[3], P[3];
	toRoot(W, d, q);
	for (int i = 0; i < 3; i++) q[i] = q[i] / ws - pivT[i];
	for (int i = 0; i < 3; i++) P[i] = pivR[i] * q[0] + pivR[3 + i] * q[1] + pivR[6 + i] * q[2];
	if (P[0] * P[0] + P[1] * P[1] + P[2] * P[2] > 200.0f * 200.0f) return;
	float P2[3] = {P[0] + f[0] * fwdP, P[1] + f[1] * fwdP, P[2] + f[2] * fwdP}, k[3];
	for (int i = 0; i < 3; i++) k[i] = P2[i] - (F[i * 3] * P2[0] + F[i * 3 + 1] * P2[1] + F[i * 3 + 2] * P2[2]) - P[i];
	for (int i = 0; i < 3; i++)
		pivT[i] = (R[i * 3] * k[0] + R[i * 3 + 1] * k[1] + R[i * 3 + 2] * k[2]) + P[i] - f[i] * gb.x;
	pivT[0] += sprX; pivT[2] += sprZ;
	memcpy(R, RF, 36);
	memcpy(pivR, R, 36);
	memcpy((void *)(pivot + 0x34), pivR, 36);
	memcpy((void *)(pivot + 0x58), pivT, 12);
	wristTick(aim);
	camRollTick();
	coverTick(dt, aim, sneak, aimT, pl);
	sightFix(pl, aim);
	if (!gunLogged)
	{
		gunLogged = true;
		LOG("handling: gun pivot at the wrist %.1f %.1f %.1f (parent frame), parent scale %.2f, view forward in parent frame %.2f %.2f %.2f", P[0], P[1], P[2], ws, f[0], f[1], f[2]);
	}
	static float sinceGun;
	if ((sinceGun += dt) > 3 && (rt::absf(gh.x) > 1 || rt::absf(gp.x) > 1))
	{
		sinceGun = 0;
		LOG("handling: gun heading %.1f pitch %.1f roll %.1f back %.1f (turning %.0f / %.0f deg/s), heat %.2f, %d shots so far%s", gh.x, gp.x, gr.x, gb.x, turnH, turnP, heat, shotsTotal, aim ? " aiming" : "");
	}
}
static bool rlApplied, rlEmpty, rlJam;
static int rlBefore, rlGroup;
static u32 rlAmmo;
static double rlStart;
static float rlEmptyBase[2], rlEmptyWrote[2];
static float rofBase[2], rofWrote[2];
static int rlLogged;
static u32 animDataOf(TESObjectREFR *pl, u32 proc, int i) { return i == 0 ? (proc ? ptr(proc + 0x1C0) : 0) : ptr((u32)pl + 0x690); }
static int weapGroup(TESObjectREFR *pl, u32 proc)
{
	u32 ad = animDataOf(pl, proc, thirdPerson(pl) ? 0 : 1);
	u32 seq = ad ? ptr(ad + 0xE0 + 4 * 4) : 0, grp = seq ? ptr(seq + 0x74) : 0;
	return grp ? rd<u16>(grp + 0x10) & 0xFF : -1;
}
static int ammoLeft(u32 ammo, int lost)
{
	if (!s_ammo || !ammo) return -1;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_ammo, player(), 0, &res, 2, (TESForm *)ammo, F((float)lost));
	return res.type == 1 ? (int)res.num : -1;
}
static int reloadTick(float dt, double t, TESObjectREFR *pl, u32 proc, int action, u32 ae, int clip, int prevClip)
{
	bool feel = TUNE(reloadFeel) > 0;
	int g = weapGroup(pl, proc);
	bool jam = g >= 0xCB && g <= 0xE1;
	bool rel = (action == 9 || (action >= 15 && action <= 17));
	u32 ammo = ae ? ptr(ae + 8) : 0;
	if (rel && !rlOn)
	{
		rlOn = true; rlApplied = false; rlJam = jam; rlGroup = g; rlStart = t;
		rlBefore = prevClip; rlAmmo = ammo; rlEmpty = prevClip == 0;
		if (feel && !jam)
		{
			float j = TUNE(reloadJoltDeg) * rt::min(1.5f, 1 / rt::sqrt(wmass)) * 2 * rt::PI * TUNE(reloadJoltHz);
			gp.v -= j * 0.9f; gr.v -= j * 1.1f; gh.v += j * 0.3f * (2 * rnd() - 1); gb.v += 8;
		}
		if (rlLogged < 30) { rlLogged++; LOG("reload: start - action %d, weapon group %x%s, clip %d, feed %d (%s)%s", action, g, jam ? " (a jam)" : "", prevClip, wfeed, wfeed == 1 ? "box" : wfeed == 2 ? "open bolt" : "fixed", rlEmpty ? ", empty" : ""); }
	}
	if (rlOn && !rlApplied && !rlJam && clip > rlBefore && clip >= 0)
	{
		rlApplied = true;
		int chamber = (feel && TUNE(chamberRound) > 0 && wfeed == 1 && rlBefore > 0) ? 1 : 0;
		int lost = (feel && TUNE(dropMagLosesRounds) > 0 && wfeed >= 1 && rlBefore > 0) ? rlBefore - chamber : 0;
		if (ammo != rlAmmo) { chamber = lost = 0; }
		int was = clip;
		if (chamber || lost)
		{
			int inv = ammoLeft(ammo, lost);
			if (inv >= 0)
			{
				int want = clip + chamber;
				if (want > inv) want = inv;
				if (want >= 0 && want != clip) { *(int *)(ae + 4) = want; clip = want; }
			}
			if (rlLogged < 30) { rlLogged++; LOG("reload: clip %d -> %d (the game filled it to %d): %d chambered, %d rounds lost with the old magazine, %d left in all", rlBefore, clip, was, chamber, lost, inv); }
		}
		else if (rlLogged < 30) { rlLogged++; LOG("reload: clip %d -> %d (the game's fill, nothing changed: feed %d%s)", rlBefore, clip, wfeed, ammo != rlAmmo ? ", another ammo" : ""); }
		if (feel)
		{
			float j = TUNE(reloadJoltDeg) * rt::min(1.5f, 1 / rt::sqrt(wmass)) * 2 * rt::PI * TUNE(reloadJoltHz);
			gp.v += j * 1.4f; gr.v += j * 0.6f; gb.v += 14; wbP.v += j * 0.8f;
		}
	}
	if (!rel && rlOn)
	{
		rlOn = false;
		if (rlLogged < 30) { rlLogged++; LOG("reload: done after %.2f s, clip %d%s", (float)(t - rlStart), clip, rlApplied ? "" : rlJam ? " (the jam's)" : " (no fill seen)"); }
		if (feel && !rlJam && rlEmpty) { float j = TUNE(reloadJoltDeg) * 2 * rt::PI * TUNE(reloadJoltHz); gb.v += 18; gp.v -= j * 0.8f; gr.v += j * 0.7f; wbP.v -= j * 0.6f; }
		else if (feel && !rlJam) { float j = TUNE(reloadJoltDeg) * 2 * rt::PI * TUNE(reloadJoltHz); gp.v += j * 0.3f; }
	}
	float wantW = (feel && rlOn && !rlJam) ? 1.0f : 0.0f;
	rlW += (wantW - rlW) * clampf(dt * TUNE(reloadPoseHz), 0, 1);
	float wm = clampf(rt::sqrt(wmass), 0.8f, 1.4f);
	rlP = rlW * TUNE(reloadGunPitch) * wm; rlR = rlW * TUNE(reloadGunRoll); rlH = rlW * TUNE(reloadGunHeading);
	bool slow = feel && wfeed >= 1 && TUNE(reloadEmptySpeed) > 0.05f && (clip == 0 || (rlOn && rlEmpty && !rlJam));
	for (int i = 0; i < 2; i++)
	{
		u32 ad = animDataOf(pl, proc, i);
		if (!ad) { rlEmptyWrote[i] = 0; continue; }
		float *rs = (float *)(ad + 0x118);
		if (slow)
		{
			if (*rs != rlEmptyWrote[i]) rlEmptyBase[i] = *rs;
			if (rlEmptyBase[i] > 0.05f && rlEmptyBase[i] < 10)
			{
				float v = rlEmptyBase[i] * TUNE(reloadEmptySpeed);
				if (*rs != v) { static int told; if (told++ < 4) LOG("reload: empty - %s reload speed %.2f -> %.2f", i ? "first-person" : "third-person", rlEmptyBase[i], v); }
				*rs = v; rlEmptyWrote[i] = v;
			}
		}
		else if (rlEmptyWrote[i] != 0)
		{
			if (*rs == rlEmptyWrote[i]) *rs = rlEmptyBase[i];
			rlEmptyWrote[i] = 0;
		}
	}
	return clip;
}

static void rlRestore()
{
	TESObjectREFR *pl = player();
	u32 proc = pl ? process(pl) : 0;
	for (int i = 0; i < 2; i++)
	{
		u32 ad = pl ? animDataOf(pl, proc, i) : 0;
		if (ad && rlEmptyWrote[i] != 0 && *(float *)(ad + 0x118) == rlEmptyWrote[i]) *(float *)(ad + 0x118) = rlEmptyBase[i];
		rlEmptyWrote[i] = 0;
	}
	rlOn = false; rlW = 0; rlP = rlH = rlR = 0;
	for (int i = 0; i < 2; i++)
	{
		u32 ad = pl ? animDataOf(pl, proc, i) : 0;
		if (ad && rofWrote[i] != 0 && *(float *)(ad + 0x110) == rofWrote[i]) *(float *)(ad + 0x110) = rofBase[i];
		rofWrote[i] = 0;
	}
}
static void fireRateTick(TESObjectREFR *pl, u32 proc)
{
	bool want = (wtype == 3 || wtype == 4) && !wAuto && TUNE(pistolFireAnimSpeed) > 1.001f;
	for (int i = 0; i < 2; i++)
	{
		u32 ad = animDataOf(pl, proc, i);
		if (!ad) { rofWrote[i] = 0; continue; }
		float *rf = (float *)(ad + 0x110);
		if (want)
		{
			if (*rf != rofWrote[i]) rofBase[i] = *rf;
			if (rofBase[i] > 0.05f && rofBase[i] < 20)
			{
				float v = rofBase[i] * TUNE(pistolFireAnimSpeed);
				if (*rf != v) { static int told; if (told++ < 6) LOG("handling: pistol - %s attack speed %.2f -> %.2f", i ? "first-person" : "third-person", rofBase[i], v); }
				*rf = v; rofWrote[i] = v;
			}
		}
		else if (rofWrote[i] != 0)
		{
			if (*rf == rofWrote[i]) *rf = rofBase[i];
			rofWrote[i] = 0;
		}
	}
}
static float cone, coneShotH, coneShotP;
static double lastShotAt = -9;
static void spreadTick(float dt, TESObjectREFR *pl, u32 proc)
{
	float mv = clampf(speedS / 350.0f, 0, 1.3f);
	u32 cc = proc ? ptr(proc + 0x138) : 0;
	int cs = cc ? rd<int>(cc + 0x3F0) : 0;
	bool air = cs == 1 || cs == 2;
	bool spr = sprint::modeNow() != 0;
	bool crouch = (moveFlags(pl) & 0x400) != 0;
	float want = TUNE(spreadWalkDeg) * mv + (spr ? TUNE(spreadSprintDeg) : 0) + (air ? TUNE(spreadAirDeg) : 0);
	if (crouch) want *= TUNE(spreadCrouchScale);
	if (want > cone) cone += (want - cone) * clampf(dt / 0.08f, 0, 1);
	else cone += (want - cone) * clampf(dt / rt::max(TUNE(spreadSettleSec), 0.05f), 0, 1);
}
static void kicks(int n, u32 proc, TESObjectREFR *pl, int clipA, int clipB)
{
	if (n <= 0 || kick <= 0) return;
	float *r = rot(pl);
	bool aim = rd<u8>(proc + 0x228) != 0, sneak = (moveFlags(pl) & 0x400) != 0;
	float vk = aim ? (wAuto ? TUNE(adsViewKickAuto) : TUNE(adsViewKick) * TUNE(adsSemiClimb)) : wOneHand ? TUNE(hipViewKickOneHand) : TUNE(hipViewKick);
	if (wShotgun) vk *= TUNE(shotgunViewScale);
	float m = TUNE(handlingStrength) * vk * (sneak ? TUNE(handlingSneakSway) : 1);
	float sideMul = (!aim && wOneHand) ? TUNE(hipSideOneHand) : 1.0f;
	float w0 = 2 * rt::PI * hz * 2.71828f;
	for (int i = 0; i < n; i++)
	{
		float hv = wAuto ? 1 + heat * TUNE(autoViewHeat) : 1.0f;
		float up = kick * m * hv * (0.85f + 0.3f * rnd()), sd = (side * m * (2 * rnd() - 1) + 0.15f * side * m) * sideMul;
		if (wAuto) sd = side * m * (0.9f * rt::sin(patPh + 0.7f) + 0.25f * (2 * rnd() - 1));
		if (aim) { pendRec[0] += up * (1 - keep) * w0; pendBake[0] += up * keep; pendRec[1] += sd * (1 - keep) * w0; pendBake[1] += sd * keep; pendT = TUNE(adsRiseDelaySec); }
		else { recV[0] += up * (1 - keep) * w0; bake[0] += up * keep; recV[1] += sd * (1 - keep) * w0; bake[1] += sd * keep; }
		gunKick(up / rt::max(vk * hv, 0.05f), aim);
	}
	if (shotsLogged < 12 || sinceLog > 5)
	{
		shotsLogged++; sinceLog = 0;
		LOG("handling: %d shot%s (clip %d -> %d)%s%s, view pitch %.2f deg, heat %.2f", n, n > 1 ? "s" : "", clipA, clipB, aim ? " aiming" : "", sneak ? " crouched" : "", r[0] * 180 / rt::PI, heat);
		checkLeft = 0.08f;
	}
}
struct Mz { Beam beam; double offAt = -1, seenAt = 0, findAt = 0; float pos[3], wrote[3]; bool want, check; u32 light, root, refSeen; };
static Mz g_mz[4];
static int g_mzFast = 1;
static const float mzRefReach = 1500;
static const u32 OFF_LIGHT_DIMMER = 0xC4, OFF_LIGHT_DIFFUSE = 0xD4;
static u32 mzFind(u32 n, int depth)
{
	if (!n || !readable(n, 0xE0) || !readable(ptr(n), 0x10)) return 0;
	const float *d = (const float *)(n + OFF_LIGHT_DIMMER), *c = (const float *)(n + OFF_LIGHT_DIFFUSE);
	if (rt::absf(d[0] - kLight_muzzle_fade) < 0.02f && rt::absf(c[0] - kLight_muzzle_rgb[0]) < 0.02f && rt::absf(c[1] - kLight_muzzle_rgb[1]) < 0.02f
		&& rt::absf(c[2] - kLight_muzzle_rgb[2]) < 0.02f) return n;
	if (depth <= 0 || !handling::isNodeObj(n)) return 0;
	u32 arr = ptr(n + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(n + OFF_NODE_CHILDCOUNT);
	if (!arr || cnt > 64 || !readable(arr, 4 * (cnt < 16 ? cnt : 16))) return 0;
	for (u16 i = 0; i < cnt && i < 16; i++) if (u32 f = mzFind(ptr(arr + 4 * i), depth - 1)) return f;
	return 0;
}
static TESObjectREFR *mzRef(Mz &m)
{
	if (!m.beam.ref) return 0;
	u32 fl = 0; TESForm *f = lookup(m.beam.ref);
	if (f) fl = rd<u32>((u32)f + 8);
	if (!f || (fl & 0x20) || (fl & 0x800)) return 0;
	TESForm *ours = lookup((g_mod << 24) | kLight_muzzle);
	return ours && ptr((u32)f + OFF_REF_BASE) == (u32)ours ? (TESObjectREFR *)f : 0;
}
static void mzReset()
{
	for (auto &m : g_mz) { memset(&m, 0, sizeof m); m.offAt = -1; }
	for (auto &r : g_mzRefs) r = 0;
}
static u32 mzLight(Mz &m)
{
	TESObjectREFR *r = mzRef(m);
	u32 root = r ? thirdPersonNode(r) : 0;
	if (!root) return m.light = m.root = 0;
	if (m.beam.ref != m.refSeen) { m.refSeen = m.beam.ref; m.seenAt = rt::now(); m.light = 0; m.root = 0; }
	if (root != m.root || !m.light)
	{
		if (rt::now() - m.seenAt < 0.5 || rt::now() - m.findAt < 1) return m.light = 0;
		m.findAt = rt::now();
		m.root = root;
		m.light = mzFind(root, 3);
		static int told;
		if (told++ < 4) LOG("muzzle light: %s", m.light ? "its light found in the reference's 3D - moved and dimmed directly" : "its light NOT found in the reference's 3D - the reference is moved");
	}
	return m.light;
}
static void mzPut(u32 L, const float *p)
{
	u32 par = ptr(L + 0x18);
	if (par)
	{
		Xf W = xfRead(par, 0x68);
		float d[3] = {p[0] - W.t[0], p[1] - W.t[1], p[2] - W.t[2]};
		for (int i = 0; i < 3; i++) *(float *)(L + 0x58 + 4 * i) = (W.r[i] * d[0] + W.r[3 + i] * d[1] + W.r[6 + i] * d[2]) / W.s;
	}
	memcpy((void *)(L + 0x8C), p, 12);
}
static void muzzleFlashAt(int k, const float *p)
{
	if (TUNE(muzzleLightOn) <= 0 || k < 0 || k > 3) return;
	memcpy(g_mz[k].pos, p, 12); g_mz[k].want = true;
}
static void muzzleLight(bool tp)
{
	float p[3]; const float *r = 0;
	if (tp && tpgun::muzzle(p, &r)) { muzzleFlashAt(0, p); return; }
	TESObjectREFR *pl = player();
	u32 root = pl ? firstPersonNode(pl) : 0, m = root ? findNamed(root, "ProjectileNode", 40) : 0;
	if (!m) return;
	muzzleFlashAt(0, (const float *)(m + 0x8C));
}
static void muzzleLightTick()
{
	for (int k = 0; k < 4; k++)
	{
		Mz &m = g_mz[k];
		if (!m.want && m.offAt < 0 && !m.check) continue;
		u32 L = g_mzFast ? mzLight(m) : 0;
		if (m.check)
		{
			m.check = false;
			if (L)
			{
				const float *w = (const float *)(L + 0x8C);
				float dx = w[0] - m.wrote[0], dy = w[1] - m.wrote[1], dz = w[2] - m.wrote[2];
				if (dx * dx + dy * dy + dz * dz > 25) { g_mzFast = 0; L = 0; LOG("muzzle light: the game put the light back by its reference - the reference is moved for each shot"); }
			}
		}
		if (m.want)
		{
			m.want = false;
			TESObjectREFR *ref = mzRef(m);
			float *rp = ref ? pos(ref) : 0;
			float dx = rp ? m.pos[0] - rp[0] : 1e9f, dy = rp ? m.pos[1] - rp[1] : 0;
			if (L && dx * dx + dy * dy < mzRefReach * mzRefReach)
			{
				mzPut(L, m.pos);
				*(float *)(L + OFF_LIGHT_DIMMER) = kLight_muzzle_fade;
				memcpy(m.wrote, m.pos, 12); m.check = true;
			}
			else
			{
				if (L) *(float *)(L + OFF_LIGHT_DIMMER) = kLight_muzzle_fade;
				beamFollow(m.beam, kLight_muzzle, m.pos[0], m.pos[1], m.pos[2]);
				g_mzRefs[2 * k] = m.beam.ref; g_mzRefs[2 * k + 1] = m.beam.old;
			}
			m.offAt = rt::now() + TUNE(muzzleLightSec);
			continue;
		}
		if (m.offAt < 0 || rt::now() < m.offAt) continue;
		m.offAt = -1;
		if (L) { *(float *)(L + OFF_LIGHT_DIMMER) = 0; continue; }
		TESObjectREFR *r = mzRef(m);
		if (r) { float *q = pos(r); moveRef(r, q[0], q[1], q[2] - 4000, 0, 0, 0); }
	}
}
static float g_shotFrom[3], g_shotH, g_shotP; static double g_shotT;
static void onShot(u32 proj, u32 weapF, float *px, float *py, float *pz, float *rz, float *rx)
{
	TESObjectREFR *pl = player();
	u32 proc = pl ? process(pl) : 0;
	g_shotFrom[0] = *px; g_shotFrom[1] = *py; g_shotFrom[2] = *pz; g_shotH = *rz; g_shotP = *rx; g_shotT = rt::now();
	if (!active || !proc) return;
	if (cvOff[0] != 0 || cvOff[1] != 0 || cvOff[2] != 0 || bl.x > 0.05f)
	{
		u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
		if (camN)
		{
			const float *c = (const float *)(camN + 0x8C);
			float a = 0, b = 0;
			for (int i = 0; i < 3; i++) { float g = i == 0 ? *px : i == 1 ? *py : *pz; a += (g - c[i]) * (g - c[i]); b += (g - c[i] + cvOff[i]) * (g - c[i] + cvOff[i]); }
			bool add = b < a, eye = rt::min(a, b) < 30 * 30;
			static int cvShotLogs;
			if (cvShotLogs < 8) { cvShotLogs++; LOG("cover: shot from %.0f %.0f %.0f, %.1f units from the camera (%.1f from where it would be without us) -> %s%s", *px, *py, *pz, rt::sqrt(a), rt::sqrt(b), add ? "moved with you" : "the game's own (already moved)", bl.x > 0.05f ? (eye ? ", blind: from the gun up there" : ", blind: the game's own") : ""); }
			if (add) { *px += cvOff[0]; *py += cvOff[1]; *pz += cvOff[2]; }
			if (eye && bl.x > 0.05f) { *px += blW[0]; *py += blW[1]; *pz += blW[2]; }
		}
	}
	float x = *px, y = *py, z = *pz;
	bool follow = TUNE(barrelFollow) > 0;
	bool aim = rd<u8>(proc + 0x228) != 0, tp = thirdPerson(pl);
	muzzleLight(tp);
	if (s_wpOff < 0 && weapF && proj) for (u32 o = 0x30; o < 0x400; o += 4) if (ptr(weapF + o) == proj) { s_wpOff = (int)o; LOG("ghosts: a weapon's projectile is at the weapon record's +%x (from your shot)", o); break; }
	if (tp && TUNE(tpBarrelShots) > 0)
	{
		float mp[3], d[3];
		float up = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f;
		float cx = rt::cos(up < 0 ? *rx : -*rx);
		float A[3] = {rt::sin(*rz) * cx, rt::cos(*rz) * cx, -rt::sin(up < 0 ? *rx : -*rx)};
		{
			if (tpgun::barrel(mp, d, A, true))
			{
				static int bLogs; static double bLogAt;
				if (bLogs < 10 || rt::now() - bLogAt > 4)
				{
					bLogs++; bLogAt = rt::now();
					float c = clampf(d[0] * A[0] + d[1] * A[1] + d[2] * A[2], -1, 1), m[3];
					for (int i = 0; i < 3; i++) m[i] = (mp[i] + d[i] * 1000) - ((i == 0 ? *px : i == 1 ? *py : *pz) + A[i] * 1000);
					LOG("shot: third person - from the muzzle (%.0f units from the game's start) along the barrel, %.2f deg off the game's aim: %.0f units from its line 1000 out%s",
						rt::sqrt((mp[0] - *px) * (mp[0] - *px) + (mp[1] - *py) * (mp[1] - *py) + (mp[2] - *pz) * (mp[2] - *pz)), rt::atan2(rt::sqrt(1 - c * c), c) * 180 / rt::PI,
						rt::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), aim ? " (aiming)" : "");
				}
				*px = mp[0]; *py = mp[1]; *pz = mp[2];
				*rz = rt::atan2(d[0], d[1]); if (*rz < 0) *rz += 2 * rt::PI;
				float pd = -rt::atan2(d[2], rt::sqrt(d[0] * d[0] + d[1] * d[1]));
				*rx = up < 0 ? pd : -pd;
				x = *px; y = *py; z = *pz;
				follow = false;
			}
		}
	}
	double now = rt::now();
	bool newShot = now - lastShotAt > 0.004;
	lastShotAt = now;
	if (newShot)
	{
		float c = cone * (aim ? TUNE(spreadAdsScale) : 1.0f);
		float a = rnd() * 2 * rt::PI, r = c * rt::sqrt(rnd());
		coneShotH = r * rt::cos(a); coneShotP = r * rt::sin(a);
	}
	float dH = 0, dP = 0;
	if (!aim && follow)
	{
		float S = TUNE(barrelFollowScale);
		dH = (gh.x + wbH.x + (tp ? 0 : wflH.x)) * S;
		dP = (gp.x * TUNE(hipShotPitchFollow) + wbP.x + (tp ? 0 : wfl.x)) * S;
		if (tp) { dH *= TUNE(tpShoulderScale); dP *= TUNE(tpShoulderScale) + TUNE(tpGunPitch); }
		float m = rt::sqrt(dH * dH + dP * dP), M = rt::max(TUNE(barrelFollowMaxDeg), 1.0f);
		if (m > 0.001f) { float k = (M * (m / M) / rt::sqrt(1 + (m / M) * (m / M))) / m; dH *= k; dP *= k; }
	}
	dH += coneShotH; dP += coneShotP;
	if (bl.x > 0.3f) { float bs = TUNE(blindSpreadDeg) * bl.x; dH += bs * (2 * rnd() - 1); dP += bs * (2 * rnd() - 1); }
	float upSign = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f;
	const float D = rt::PI / 180;
	float inZ = *rz, inX = *rx;
	*rz += dH * D;
	*rx += upSign * dP * D;
	if (shotLogs < 16 && newShot)
	{
		shotLogs++;
		float *vr = rot(pl);
		LOG("shot: weapon %x projectile %x (hitscan %d) from %.0f %.0f %.0f; the game's aim heading %.2f pitch %.2f deg, the view's %.2f %.2f; gun %.2f %.2f + cone %.2f (of %.2f) -> %.2f %.2f%s%s",
			weapF ? refid((void *)weapF) : 0, proj ? refid((void *)proj) : 0, proj ? (int)(rd<u32>(proj + 0x60) & 1) : -1, x, y, z,
			inZ / D, inX / D, vr[2] / D, vr[0] / D, dH - coneShotH, dP - coneShotP, rt::sqrt(coneShotH * coneShotH + coneShotP * coneShotP), cone, *rz / D, *rx / D, aim ? " aiming" : " hip", tp ? " third person" : "");
	}
	g_shotH = *rz; g_shotP = *rx;
	if (newShot && weapF == weap)
	{
		u32 ae = ptr(proc + 0x118);
		int c = ae ? rd<int>(ae + 4) : -1;
		kicks(1, proc, pl, c, c);
	}
}

static void tick(float dt, double t, bool focus, TESObjectREFR *pl)
{
	static int loadedFor = -1;
	char hp[320];
	rt::fmt(hp, sizeof hp, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\handling.ini", g_dir);
	const char *key = g_vanilla ? "vanilla=" : "mmw=";
	if (loadedFor != (int)g_vanilla)
	{
		loadedFor = g_vanilla;
		on = true;
		if (char *b = rt::readfile(hp, 0))
		{
			for (char *q = b; *q; q++)
				if ((q == b || q[-1] == '\n') && rt::startswith(q, key)) on = rt::parsei(q + strlen(key)) != 0;
			rt::free(b);
		}
		if (!on) LOG("handling: off (handling.ini)");
	}
	muzzleLightTick();
	bool u = pressed(VK_HANDLING);
	if (u && focus && !g_driving && !menumode())
	{
		on = !on;
		message(on ? "Weapon handling on: inertia, sway, recoil and contextual aiming. U turns it off."
		           : "Weapon handling off: the game's own guns. U turns it back on.", 3);
		LOG("handling: U - %s", on ? "on" : "off");
		int other = 1;
		const char *okey = g_vanilla ? "mmw=" : "vanilla=";
		if (char *b = rt::readfile(hp, 0))
		{
			for (char *q = b; *q; q++)
				if ((q == b || q[-1] == '\n') && rt::startswith(q, okey)) other = rt::parsei(q + strlen(okey)) != 0;
			rt::free(b);
		}
		char line[48];
		int n = rt::fmt(line, sizeof line, "mmw=%d\r\nvanilla=%d\r\n", g_vanilla ? other : (int)on, g_vanilla ? (int)on : other);
		rt::writefile(hp, line, (u32)n, false);
	}
	u32 proc = process(pl);
	const char *why = 0;
	if (!on || TUNE(handlingStrength) <= 0) why = "turned off";
	else if (g_driving) why = "driving";
	else if (rd<u32>(0x11F2250 + 8)) why = "VATS";
	else if (!proc) why = "no process";
	else if (!rd<u8>(proc + 0x135)) why = "weapon away";
	if (why) { curAction = -1; if (active || haveLast) release(pl, why); return; }
	float *r = rot(pl), *p = pos(pl);
	if (!active)
	{
		active = true; reset();
		lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2];
		LOG("handling: on (view pitch %.1f yaw %.1f deg)", r[0] * 180 / rt::PI, r[2] * 180 / rt::PI);
	}
	{
		float jx = p[0] - lastPos[0], jy = p[1] - lastPos[1], jz = p[2] - lastPos[2];
		if (jx * jx + jy * jy + jz * jz > 500.0f * 500.0f) { reset(); lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2]; }
	}
	float d[2] = {0, 0};
	if (haveLast)
	{
		d[0] = r[0] - lastW[0]; d[1] = wrapPi(r[2] - lastW[1]);
		if (rt::absf(d[0]) > 1.2f || rt::absf(d[1]) > 1.2f) { LOG("handling: the view jumped (%.0f, %.0f deg) - starting over", d[0] * 180 / rt::PI, d[1] * 180 / rt::PI); reset(); d[0] = d[1] = 0; }
	}
	float T[2] = {r[0] - O[0], wrapPi(r[2] - O[1])};
	u32 we = ptr(proc + 0x114), ae = ptr(proc + 0x118);
	u32 w = we ? ptr(we + 8) : 0;
	int clip = ae ? rd<int>(ae + 4) : -1;
	static double lastClipShot = -9, lastEdgeShot = -9;
	static int lastAction = -1;
	int action = (int)((short(__thiscall *)(u32))ptr(ptr(proc) + 0x3E4))(proc);
	curAction = action;
	bool edge = action == 2 && lastAction != 2;
	lastAction = action;
	int n = 0;
	if (w != weap) { g_gearChangedAt = rt::now(); readWeapon(w); lastClip = clip; LOG("handling: ammo entry %x, rounds %d", ae, clip); }
	else
	{
		if (clip >= 0 && lastClip > clip && lastClip - clip <= 40)
		{
			n = lastClip - clip;
			if (t - lastEdgeShot < 0.25) { n--; lastEdgeShot = -9; }
			lastClipShot = t;
		}
		if (edge)
		{
			if (t - lastClipShot >= 0.25) { n++; lastEdgeShot = t; if (shotsLogged < 12) LOG("handling: a shot from the attack starting (rounds %d)", clip); }
			else lastClipShot = -9;
		}
	}
	if (hookShots) n = 0;
	if (n > 0) kicks(n, proc, pl, lastClip, clip);
	clip = reloadTick(dt, t, pl, proc, action, ae, clip, lastClip);
	lastClip = clip;
	spreadTick(dt, pl, proc);
	fireRateTick(pl, proc);
	if (pendT > 0 && (pendT -= dt) <= 0)
		for (int i = 0; i < 2; i++) { recV[i] += pendRec[i]; bake[i] += pendBake[i]; pendRec[i] = pendBake[i] = 0; }
	float step0 = bake[0] * clampf(dt / 0.06f, 0, 1), step1 = bake[1] * clampf(dt / 0.06f, 0, 1);
	bake[0] -= step0; bake[1] -= step1;
	float upSign = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f;
	T[0] += upSign * step0 * rt::PI / 180; T[1] += step1 * rt::PI / 180;
	for (int i = 0; i < 2; i++) spring(rec[i], recV[i], 0, hz, 1.0f, dt);
	bool aim = rd<u8>(proc + 0x228) != 0, sneak = (moveFlags(pl) & 0x400) != 0;
	float tau = clampf(lagSec * TUNE(handlingViewLagScale) * (aim ? 1.15f : 1) * TUNE(handlingStrength), 0, TUNE(handlingMaxLagSec));
	for (int i = 0; i < 2; i++)
	{
		lag[i] += d[i];
		lag[i] -= lag[i] * (tau > 0.001f ? clampf(dt / (tau + dt), 0, 1) : 1);
	}
	float hx = p[0] - lastPos[0], hy = p[1] - lastPos[1];
	lastPos[0] = p[0]; lastPos[1] = p[1]; lastPos[2] = p[2];
	float sp = rt::sqrt(hx * hx + hy * hy) / rt::max(dt, 0.001f);
	if (sp > 2000) sp = 0;
	speedS += (sp - speedS) * clampf(dt * 6, 0, 1);
	float mv = clampf(speedS / 350.0f, 0, 1);
	stepPhase += dt * 2 * rt::PI * (0.9f + 0.9f * mv);
	if (stepPhase > 1000) stepPhase -= 1000;
	float st = (float)(t - 1000.0 * (int)(t / 1000.0));
	float tpv = thirdPerson(pl) ? TUNE(tpViewSway) : 1.0f;
	float a = sway * TUNE(inertiaViewSway) * (aim ? TUNE(handlingAimSway) : 1) * (sneak ? TUNE(handlingSneakSway) : 1) * TUNE(handlingStrength) * tpv;
	float mvA = TUNE(handlingMoveSwayDeg) * mv * (aim ? 0.5f : 1) * TUNE(handlingStrength) * tpv;
	float swP = a * (0.6f * rt::sin(2 * rt::PI * 0.23f * st) + 0.4f * rt::sin(2 * rt::PI * 0.37f * st + 1)) + mvA * 0.5f * rt::sin(2 * stepPhase);
	float swY = a * 0.8f * (0.7f * rt::sin(2 * rt::PI * 0.17f * st + 2) + 0.3f * rt::sin(2 * rt::PI * 0.41f * st)) + mvA * rt::sin(stepPhase);
	blend = clampf(blend + dt / 0.3f, 0, 1);
	{
		float sr = (hx * rt::cos(T[1]) - hy * rt::sin(T[1])) / rt::max(dt, 0.001f);
		if (rt::absf(sr) > 2000) sr = 0;
		strafeS += (clampf(sr / 300.0f, -1, 1) - strafeS) * clampf(dt * 5, 0, 1);
		strafeA += (clampf(sr / rt::max(TUNE(tpStrafeRockAdsSpeed), 20.0f), -1, 1) - strafeA) * clampf(dt * 5, 0, 1);
		gunTick(dt, t, T, aim, sneak, mv, stepPhase, pl);
	}
	const float D = rt::PI / 180;
	bool tpNow = thirdPerson(pl);
	float shk = tpNow ? TUNE(tpCamShake) : 1.0f;
	O[0] = blend * (upSign * (rec[0] + csP.x * shk) + swP) * D - lag[0];
	O[1] = blend * (rec[1] + csH.x * shk) * D - lag[1];
	if (tpNow) tpCamRoll(blend * (csR.x + TUNE(strafeCantDeg) * strafeS) * TUNE(tpCamRoll), g_tpCamSide);
	else tpCamIdentity();
	r[0] = clampf(T[0] + O[0], -1.55f, 1.55f);
	r[2] = T[1] + O[1];
	if (r[2] < 0) r[2] += 2 * rt::PI; else if (r[2] >= 2 * rt::PI) r[2] -= 2 * rt::PI;
	lastW[0] = r[0]; lastW[1] = r[2]; haveLast = true;
	sinceLog += dt;
	if (checkLeft > 0 && (checkLeft -= dt) <= 0)
		LOG("handling: a moment later: view pitch %.2f deg (aim %.2f, recoil %.2f up, kept %.2f to come), the game's turn this frame %.3f deg", r[0] * 180 / rt::PI, T[0] * 180 / rt::PI, rec[0], bake[0], d[0] * 180 / rt::PI);
	static float sinceState;
	if ((sinceState += dt) > 10)
	{
		sinceState = 0;
		LOG("handling: sway %.2f/%.2f deg, lag %.2f/%.2f deg, speed %.0f, aiming %d crouched %d", swP, swY, lag[0] / D, lag[1] / D, speedS, aim, sneak);
	}
}
}

// ---- The plugin's first code hook: the call to Projectile::Create (0x9BCA60) inside TESObjectWEAP::Fire
typedef u32(__cdecl *ProjCreateFn)(u32, u32, u32, u32, float, float, float, float, float, u32, u32, u32, u32, float, float, u32);
static ProjCreateFn s_projCreate;
namespace ghosts { static void npcShot(u32 actor); }
namespace coop { void leavingWorld(bool newGame); void saved(const char *name); void hostSession(u8 peer, const char *who); bool chatCommand(const char *line); void questItemsIn(u8 peer, const void *data, int len, const char *who); void questStepIn(u8 peer, const void *data, int len, const char *who); void sessionRefused(const void *data, int len); void xpIn(u8 peer, const void *data, int len, const char *who); void hostWorldChanged(const char *who); }
static u32 __cdecl projCreateHook(u32 proj, u32 actor, u32 cc, u32 weap, float x, float y, float z, float rz, float rx, u32 a10, u32 tgt, u32 hit, u32 grav, float amz, float amx, u32 cell)
{
	if (actor && actor == (u32)player()) handling::onShot(proj, weap, &x, &y, &z, &rz, &rx);
	else if (actor) ghosts::npcShot(actor);
	return s_projCreate(proj, actor, cc, weap, x, y, z, rz, rx, a10, tgt, hit, grav, amz, amx, cell);
}
static int(__stdcall *s_vprot)(void *, u32, u32, u32 *);
static bool hookCall(u32 at, u32 expect, void *fn, u32 *orig, const char *what)
{
	if (!s_vprot) { HANDLE k = LoadLibraryA("kernel32.dll"); if (k) s_vprot = (int(__stdcall *)(void *, u32, u32, u32 *))GetProcAddress(k, "VirtualProtect"); }
	u8 *p = (u8 *)at;
	if (p[0] != 0xE8 || !s_vprot) { LOG("hook %s at %x: not a call (%02x %02x %02x %02x %02x) - left alone", what, at, p[0], p[1], p[2], p[3], p[4]); return false; }
	u32 tgt = at + 5 + *(int *)(p + 1);
	if (tgt == (u32)fn) { LOG("hook %s at %x: already ours", what, at); return true; }
	u32 old = 0;
	if (!s_vprot(p, 5, 0x40, &old)) { LOG("hook %s at %x: VirtualProtect failed", what, at); return false; }
	*orig = tgt;
	*(int *)(p + 1) = (int)((u32)fn - (at + 5));
	s_vprot(p, 5, old, &old);
	LOG("hook %s at %x: it called %x%s - now ours first", what, at, tgt, tgt == expect ? " (the game's own)" : " (not the game's own - another plugin's hook, called on)");
	return true;
}
static char lcChar(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static const char *stristr(const char *s, const char *w)
{
	if (!s) return 0;
	for (; *s; s++) { const char *a = s, *b = w; while (*a && *b && lcChar(*a) == lcChar(*b)) { a++; b++; } if (!*b) return s; }
	return 0;
}
// ---- (plugin 164, the author: "allow for super fine tuning for friends, maybe a keybind that brings up a menu similar
#include "font5x7.h"
static double g_useT;
// ---- Listen-in: a friend's conversation, as subtitles (ghosts fills it, the frame's Present draws it)
struct MsgKind { char key[40]; double at; float secs; bool pend; float pendSecs; char pendText[900]; };
static MsgKind s_mk[8];
void game::message(const char *text, float secs, bool replace)
{
	if (!text || !text[0]) return;
	float t = rt::max(1.0f, secs * TUNE(notifyTimeScale));
	if (!replace) { gameMessage(text, t); return; }
	char key[40]; int n = 0;
	const char *colon = 0;
	for (int i = 0; text[i] && i < 32; i++) if (text[i] == ':') { colon = text + i; break; }
	if (colon) { n = (int)(colon - text); if (n > 39) n = 39; memcpy(key, text, n); }
	else { while (text[n] && text[n] != ' ' && n < 39) { key[n] = text[n]; n++; } }
	key[n] = 0;
	double now = rt::now();
	MsgKind *k = 0;
	for (auto &m : s_mk) if (m.key[0] && rt::streq(m.key, key)) k = &m;
	if (k && now - k->at < k->secs) { rt::strcpy(k->pendText, text, sizeof k->pendText); k->pendSecs = t; k->pend = true; return; }
	if (!k) { k = &s_mk[0]; for (auto &m : s_mk) { if (!m.key[0]) { k = &m; break; } if (!m.pend && m.at < k->at) k = &m; } if (k->pend) gameMessage(k->pendText, k->pendSecs); }
	gameMessage(text, t);
	rt::strcpy(k->key, key, sizeof k->key); k->at = now; k->secs = t; k->pend = false;
}
static void messageTick()
{
	double now = rt::now();
	for (auto &m : s_mk)
		if (m.pend && now - m.at >= m.secs) { m.pend = false; gameMessage(m.pendText, m.pendSecs); m.at = now; m.secs = m.pendSecs; }
}
struct Listen { char who[24], speaker[32], text[300]; double t; };
static Listen g_listen[3]; static int g_nListen;
// ---- The trade window: what it shows
struct TradeRow { char name[48]; u32 count; };
struct TradeView { bool on, myReady, theirReady, editing; char peer[24]; int nMine, nTheirs; TradeRow mine[10], theirs[10]; u32 capsMine, capsTheirs; char status[120]; };
static TradeView g_tv;
// ---- Shared XP: the last quest step this game did (XP that comes with it is a quest's), and XP friends gave you
struct LastStep { u32 quest; u8 stage; double t; };
static LastStep g_lastStep;
static double g_xpPending, g_xpPendingT;
// ---- (286, the author: "I also want a way to visualize multiple ranges in game so I can see what the 100m range is on
namespace ranges
{
static bool on;
struct Ring { float r; u32 rgb; const char *label; int n; bool follow; };
static Ring kRings[4] = {{7000, 0xE8B030, "shared XP + quest credit", 120, false}, {4200, 0x40C8C0, "the host's people synced round a guest", 96, false}, {160, 0x70E070, "E: get a downed friend up", 24, true}, {700, 0xB090F0, "listen in on a friend's conversation", 48, true}};
static float pts[120 + 96][3];
static float cx, cy, cz;
static int fill;
static bool centred;
static void tick(TESObjectREFR *pl)
{
	if (!on || !pl) return;
	static float lastR;
	kRings[0].r = TUNE(coopXpRangeUnits); kRings[3].r = TUNE(coopListenRangeUnits);
	if (kRings[0].r != lastR) { lastR = kRings[0].r; centred = false; }
	const float *p = pos(pl);
	float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
	if (!centred || dx * dx + dy * dy > 250.0f * 250.0f || dz * dz > 150.0f * 150.0f) { cx = p[0]; cy = p[1]; cz = p[2]; fill = 0; centred = true; }
	int total = kRings[0].n + kRings[1].n;
	for (int k = 0; k < 24 && fill < total; k++, fill++)
	{
		int ri = fill < kRings[0].n ? 0 : 1, i = ri ? fill - kRings[0].n : fill;
		float a = 2 * rt::PI * i / kRings[ri].n, x = cx + kRings[ri].r * rt::cos(a), y = cy + kRings[ri].r * rt::sin(a);
		pts[fill][0] = x; pts[fill][1] = y; pts[fill][2] = groundAt(x, y, cz) + 6;
	}
}
static void toggle()
{
	on = !on; centred = false; fill = 0;
	message(on ? "Ranges shown: gold 100 m shared XP, teal 60 m the host's people, violet 10 m listen in, green 2 m revive (/ranges hides them)" : "Ranges hidden", 4);
	LOG("ranges: %s", on ? "shown" : "hidden");
}
}
namespace feel
{
struct Group { const char *label; const char *ids; float lo; const char *was; };
static const Group kGroups[] = {
	{"WEAPON INERTIA", " inertiaTurnSec inertiaMaxDeg inertiaMoveDeg inertiaRoll handlingViewLagScale "},
	{"WEAPON SWAY", " inertiaSwayScale inertiaViewSway handlingMoveSwayDeg autoSway "},
	{"GUN KICK", " adsKickScale adsSemiKick hipKickOneHand hipKickTwoHand kickBackUnits adsFlipDeg wobbleDeg "},
	{"VIEW RECOIL", " adsViewKick adsViewKickAuto hipViewKick hipViewKickOneHand "},
	{"CAMERA SHAKE", " camShakeDeg "},
	{"CAMERA ROLL", " camRollDeg strafeCantDeg "},
	{"FOV KICK", " fovKickDeg "},
	{"CORNER LEAN SPEED", " coverHz coverSmoothHz tpLeanHz "},
	{"3RD PERSON BODY", " tpSpineInertia tpShoulderScale tpStrafeRockDeg "},
	{"FOOT PLACEMENT", " bodyIkWeight "},
	{"BODY LEAN", " bodyAccelLean bodyRunLeanDeg ", 0, "BODY LEAN & BANK"},
	{"BODY BANK", " bodyBankScale ", 0, "BODY LEAN & BANK"},
	{"HILL LEAN", " bodySlopeLeanUp bodySlopeLeanDown bodySlopeBodyUp bodySlopeBodyDown "},
	{"HILL SIDE LEAN", " bodySlopeSide "},
	{"ANIMATION BLEND TIME", " bodyBlendSec ", 0.2f},
	{"HEAD LOOK", " headLookScale "},
	{"BREATH, WEIGHT & ARMS", " lifeBreathDeg lifeBreathShoulder lifeShiftUnits lifeHipDeg lifeShrugUnits lifeArmSwing lifeArmIdleDeg lifeHeadDriftDeg "},
	{"MOVEMENT ACCELERATION", " moveAccelSec moveDecelSec ", 0.2f},
};
static const int N = sizeof kGroups / sizeof kGroups[0];
static float scale[N] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static bool open, loaded;
static int sel;
static float holdT;
static u32 s_presents, s_openCalls;
static double s_openAt;
static bool s_drawTold, s_waitTold;
static void presentWhere();
static bool has(const char *h, const char *n)
{
	u32 ln = (u32)strlen(n);
	for (; *h; h++) if (*h == *n && !memcmp(h, n, ln)) return true;
	return false;
}
static void path(char *p, u32 cap) { rt::fmt(p, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\feel.ini", g_dir); }
static void load()
{
	loaded = true;
	char p[320]; path(p, sizeof p);
	char *b = rt::readfile(p, 0);
	if (!b) return;
	for (int i = 0; i < N; i++)
		for (int pass = 0; pass < 2; pass++)
		{
			const char *lb = pass ? kGroups[i].was : kGroups[i].label;
			if (!lb) break;
			u32 n = (u32)strlen(lb);
			bool got = false;
			for (char *q = b; *q; q++)
				if ((q == b || q[-1] == '\n') && !memcmp(q, lb, n) && q[n] == '=') { scale[i] = rt::clamp(rt::parsef(q + n + 1), kGroups[i].lo, 3); got = true; }
			if (got) break;
		}
	rt::free(b);
	g_tuneGen++;
}
static void save()
{
	char p[320], buf[1024]; path(p, sizeof p);
	int n = rt::fmt(buf, sizeof buf, "; the feel menu (F8): each line x its tuning values\r\n");
	for (int i = 0; i < N; i++) n += rt::fmt(buf + n, sizeof buf - n, "%s=%.4f\r\n", kGroups[i].label, scale[i]);
	rt::writefile(p, buf, (u32)n, false);
}
static float of(const char *id)
{
	if (!loaded) load();
	char k[48];
	u32 n = (u32)strlen(id);
	if (n + 3 > sizeof k) return 1;
	k[0] = ' '; memcpy(k + 1, id, n); k[n + 1] = ' '; k[n + 2] = 0;
	for (int i = 0; i < N; i++) if (has(kGroups[i].ids, k)) return scale[i];
	return 1;
}
static void tick(float dt, bool focus)
{
	bool f8 = pressed(VK_FEEL);
	if (f8 && focus && !menumode())
	{
		open = !open; if (!open) save(); LOG("feel: menu %s", open ? "open" : "closed");
		s_openAt = rt::now(); s_openCalls = s_presents; s_drawTold = s_waitTold = false;
	}
	if (!open) return;
	if (!s_waitTold && s_presents == s_openCalls && rt::now() - s_openAt > 1.0) { s_waitTold = true; presentWhere(); }
	if (!focus) return;
	bool up = pressed(0x26), dn = pressed(0x28), r = pressed(0x08);
	if (up) sel = (sel + N - 1) % N;
	if (dn) sel = (sel + 1) % N;
	if (r) { scale[sel] = 1; g_tuneGen++; }
	bool L = held(0x25), R = held(0x27);
	if (L != R)
	{
		holdT += dt;
		float rate = rt::min(0.04f + holdT * holdT * 0.25f, 0.6f) * (held(0x10) ? 0.1f : 1.0f);
		scale[sel] = rt::clamp(scale[sel] + (R ? rate : -rate) * dt, kGroups[sel].lo, 3);
		g_tuneGen++;
	}
	else holdT = 0;
}
struct V { float x, y, z, w; u32 c; };
static V vb[6 * 4096];
static int nv;
static void quad(float x, float y, float w, float h, u32 c)
{
	if (nv + 6 > (int)(sizeof vb / sizeof vb[0])) return;
	V a = {x, y, 0, 1, c}, b = {x + w, y, 0, 1, c}, d = {x, y + h, 0, 1, c}, e = {x + w, y + h, 0, 1, c};
	vb[nv++] = a; vb[nv++] = b; vb[nv++] = d; vb[nv++] = b; vb[nv++] = e; vb[nv++] = d;
}
static void line(float x0, float y0, float x1, float y1, float th, u32 c)
{
	if (nv + 6 > (int)(sizeof vb / sizeof vb[0])) return;
	float dx = x1 - x0, dy = y1 - y0, L = rt::sqrt(dx * dx + dy * dy);
	if (L < 0.01f) return;
	float nx = -dy / L * th * 0.5f, ny = dx / L * th * 0.5f;
	V a = {x0 + nx, y0 + ny, 0, 1, c}, b = {x1 + nx, y1 + ny, 0, 1, c}, d = {x0 - nx, y0 - ny, 0, 1, c}, e = {x1 - nx, y1 - ny, 0, 1, c};
	vb[nv++] = a; vb[nv++] = b; vb[nv++] = d; vb[nv++] = b; vb[nv++] = e; vb[nv++] = d;
}
static float text(float x, float y, float px, const char *t, u32 c)
{
	for (; *t; t++, x += 6 * px)
	{
		const unsigned char *rows = 0;
		char ch = *t >= 'a' && *t <= 'z' ? *t - 32 : *t;
		for (int i = 0; i < kFont5x7Count; i++) if (kFont5x7[i].c == ch) rows = kFont5x7[i].rows;
		if (!rows) continue;
		for (int ry = 0; ry < 7; ry++)
			for (int rx = 0; rx < 5; rx++)
				if (rows[ry] & (0x10 >> rx)) quad(x + rx * px, y + ry * px, px, px, c);
	}
	return x;
}
static void blockMark(float W, float H)
{
	if (rt::now() - g_blkAt > 0.1 || menumode()) return;
	float x = W * 0.5f, y = H * 0.5f, r = rt::max(3.0f, H * TUNE(blockMarkSize)), th = rt::max(1.0f, (float)(int)(H / 720));
	u32 c = g_hudColor & 0x00FFFFFF;
	line(x - r, y - r, x + r, y + r, th * 3, 0x40000000 | c); line(x - r, y + r, x + r, y - r, th * 3, 0x40000000 | c);
	line(x - r, y - r, x + r, y + r, th, 0xF0000000 | c); line(x - r, y + r, x + r, y - r, th, 0xF0000000 | c);
}
static void tracers(float W, float H)
{
	double now = rt::now();
	for (int i = 0; i < handling::g_trN;)
	{
		const handling::Tracer &t = handling::g_tr[i];
		float u = (float)((now - t.t0) / t.dur);
		if (u > 1) { handling::g_tr[i] = handling::g_tr[--handling::g_trN]; continue; }
		i++;
		float u0 = rt::max(0.0f, u - 0.3f), h[3], tl[3], hx, hy, tx, ty;
		for (int k = 0; k < 3; k++) { h[k] = t.a[k] + (t.b[k] - t.a[k]) * u; tl[k] = t.a[k] + (t.b[k] - t.a[k]) * u0; }
		if (!handling::toScreen(h, W, H, &hx, &hy) || !handling::toScreen(tl, W, H, &tx, &ty)) continue;
		float th = rt::max(1.5f, H / 600);
		line(tx, ty, hx, hy, th * 3, 0x40FFC870);
		line(tx, ty, hx, hy, th, 0xE0FFF0C0);
	}
}
struct TV { float x, y, z, w; u32 c; float u, v; };
static TV tb[6 * 6144];
static int nt;
struct Glyph { u16 x, y, w; };
static Glyph s_gl[96];
static int s_cellH;
static void *s_tex, *s_texDev;
static bool s_fontTried;
static const int kAtlas = 512;
struct GGlyph { float u0, v0, u1, v1, w, h, kl, kr, top; };
struct GFont { GGlyph g[256]; float line, ascent; void *tex; bool ok; };
enum { F_HUD, F_TEXT, F_TITLE, kGF };
static GFont s_gf[kGF];
static TV tbf[kGF][6 * 4096]; static int ntf[kGF];
static int g_fnt = F_TEXT;
static void fontSel(int f) { g_fnt = f; }
static bool gameFontLoad(void *dev, int which, GFont &F)
{
	F.ok = false; F.tex = 0;
	char p[400];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\gamefont\\font%d.fnt", g_dir, which);
	u32 n = 0; u8 *fnt = (u8 *)rt::readfile(p, &n);
	if (!fnt) return false;
	if (n < 296 + 256 * 56) { rt::free(fnt); return false; }
	u32 pages = 0; memcpy(&pages, fnt + 4, 4);
	if (pages != 1) { rt::free(fnt); LOG("feel: the game's font has %u pages of letters - our own lettering stays", pages); return false; }
	memcpy(&F.line, fnt, 4);
	F.ascent = 0;
	for (int c = 0; c < 256; c++)
	{
		const float *q = (const float *)(fnt + 296 + c * 56);
		GGlyph &G = F.g[c];
		if (q[0] != 0) { rt::free(fnt); return false; }
		G.u0 = q[1]; G.v0 = q[2]; G.u1 = q[7]; G.v1 = q[8]; G.w = q[9]; G.h = q[10]; G.kl = q[11]; G.kr = q[12]; G.top = q[13];
		if (c >= 'A' && c <= 'Z' && G.top > F.ascent) F.ascent = G.top;
	}
	rt::free(fnt);
	if (!(F.line > 4 && F.line < 200)) return false;
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\gamefont\\font%d.tex", g_dir, which);
	u8 *tx = (u8 *)rt::readfile(p, &n);
	if (!tx) return false;
	u32 tw = 0, th = 0;
	if (n >= 8) { memcpy(&tw, tx, 4); memcpy(&th, tx + 4, 4); }
	if (!tw || !th || tw > 4096 || th > 4096 || n < 8 + tw * th * 4) { rt::free(tx); return false; }
	void **vt = *(void ***)dev;
	void *tex = 0;
	long hr = ((long(__stdcall *)(void *, u32, u32, u32, u32, u32, u32, void **, void *))vt[23])(dev, tw, th, 1, 0, 21, 1, &tex, 0);
	if (hr >= 0 && tex)
	{
		void **tv = *(void ***)tex;
		struct { int pitch; void *p; } lr = {0, 0};
		if (((long(__stdcall *)(void *, u32, void *, const void *, u32))tv[19])(tex, 0, &lr, 0, 0) >= 0 && lr.p)
		{
			for (u32 r = 0; r < th; r++)
			{
				const u8 *src = tx + 8 + r * tw * 4;
				u32 *dst = (u32 *)((u8 *)lr.p + r * lr.pitch);
				for (u32 c = 0; c < tw; c++) dst[c] = ((u32)src[c * 4 + 3] << 24) | 0xFFFFFF;
			}
			((long(__stdcall *)(void *, u32))tv[20])(tex, 0);
			F.tex = tex; F.ok = true;
		}
		else ((u32(__stdcall *)(void *))tv[2])(tex);
	}
	rt::free(tx);
	if (F.ok) LOG("feel: the game's own lettering (font %d: %ux%u, line %.0f) for our screens", which, tw, th, F.line);
	return F.ok;
}
static int s_useGF = -1;
static const GFont &gfNow() { if (s_gf[g_fnt].ok) return s_gf[g_fnt]; for (auto &f : s_gf) if (f.ok) return f; return s_gf[0]; }
static int gfSlot() { if (s_gf[g_fnt].ok) return g_fnt; for (int i = 0; i < kGF; i++) if (s_gf[i].ok) return i; return 0; }
static bool fontAtlas(void *dev)
{
	if ((s_tex || s_useGF >= 0) && s_texDev == dev) return true;
	if (s_fontTried && s_texDev == dev) return false;
	if (s_tex) { ((u32(__stdcall *)(void *))(*(void ***)s_tex)[2])(s_tex); }
	for (auto &f : s_gf) { if (f.tex) ((u32(__stdcall *)(void *))(*(void ***)f.tex)[2])(f.tex); f.tex = 0; f.ok = false; }
	s_useGF = -1;
	s_fontTried = true; s_texDev = dev; s_tex = 0;
	{
		static const int which[kGF] = {1, 2, 6};
		bool any = false;
		for (int i = 0; i < kGF; i++) any |= gameFontLoad(dev, which[i], s_gf[i]);
		if (any) { s_useGF = 0; return true; }
	}
	HANDLE g = LoadLibraryA("gdi32.dll");
	if (!g) return false;
	typedef void *P;
	auto mkFont = (P(__stdcall *)(int, int, int, int, int, u32, u32, u32, u32, u32, u32, u32, u32, const char *))GetProcAddress(g, "CreateFontA");
	auto mkDC = (P(__stdcall *)(P))GetProcAddress(g, "CreateCompatibleDC");
	auto mkDIB = (P(__stdcall *)(P, const void *, u32, P *, P, u32))GetProcAddress(g, "CreateDIBSection");
	auto sel = (P(__stdcall *)(P, P))GetProcAddress(g, "SelectObject");
	auto txtCol = (u32(__stdcall *)(P, u32))GetProcAddress(g, "SetTextColor");
	auto bkMode = (int(__stdcall *)(P, int))GetProcAddress(g, "SetBkMode");
	auto out = (int(__stdcall *)(P, int, int, const char *, int))GetProcAddress(g, "TextOutA");
	auto widths = (int(__stdcall *)(P, u32, u32, int *))GetProcAddress(g, "GetCharWidth32A");
	auto del = (int(__stdcall *)(P))GetProcAddress(g, "DeleteObject");
	auto delDC = (int(__stdcall *)(P))GetProcAddress(g, "DeleteDC");
	auto flush = (int(__stdcall *)())GetProcAddress(g, "GdiFlush");
	if (!mkFont || !mkDC || !mkDIB || !sel || !txtCol || !bkMode || !out || !widths || !del || !delDC || !flush) { LOG("feel: Windows' font drawing not found - block letters"); return false; }
	const int fh = 44;
	P font = mkFont(-fh, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 4 /*ANTIALIASED*/, 0, "Consolas");
	P dc = mkDC(0);
	struct { u32 size; int w, h; u16 planes, bits; u32 comp, img, xpm, ypm, used, imp; } bi = {40, kAtlas, -kAtlas, 1, 32, 0, 0, 0, 0, 0, 0};
	P bits = 0;
	P dib = dc ? mkDIB(dc, &bi, 0, &bits, 0, 0) : 0;
	if (!font || !dc || !dib || !bits) { LOG("feel: the menu's font atlas couldn't be made - block letters"); if (dib) del(dib); if (dc) delDC(dc); if (font) del(font); return false; }
	P oldB = sel(dc, dib), oldF = sel(dc, font);
	memset(bits, 0, kAtlas * kAtlas * 4);
	txtCol(dc, 0xFFFFFF); bkMode(dc, 1 /*TRANSPARENT*/);
	int wd[96] = {0};
	widths(dc, 32, 127, wd);
	s_cellH = fh + 10;
	int x = 2, y = 2;
	for (int i = 0; i < 95; i++)
	{
		int w = wd[i] > 0 && wd[i] < 120 ? wd[i] : fh / 2;
		if (x + w + 4 > kAtlas) { x = 2; y += s_cellH + 2; }
		if (y + s_cellH > kAtlas) break;
		char ch = (char)(32 + i);
		out(dc, x, y + 2, &ch, 1);
		s_gl[i].x = (u16)x; s_gl[i].y = (u16)y; s_gl[i].w = (u16)w;
		x += w + 6;
	}
	flush();
	void **vt = *(void ***)dev;
	void *tex = 0;
	long hr = ((long(__stdcall *)(void *, u32, u32, u32, u32, u32, u32, void **, void *))vt[23])(dev, kAtlas, kAtlas, 1, 0, 21, 1, &tex, 0);
	if (hr < 0 || !tex) LOG("feel: the menu's font texture couldn't be made (%x) - block letters", (u32)hr);
	else
	{
		void **tv = *(void ***)tex;
		struct { int pitch; void *p; } lr = {0, 0};
		if (((long(__stdcall *)(void *, u32, void *, const void *, u32))tv[19])(tex, 0, &lr, 0, 0) >= 0 && lr.p)
		{
			for (int r = 0; r < kAtlas; r++)
			{
				const u32 *src = (const u32 *)bits + r * kAtlas;
				u32 *dst = (u32 *)((u8 *)lr.p + r * lr.pitch);
				for (int c = 0; c < kAtlas; c++) { u32 a = (src[c] >> 8) & 0xFF; dst[c] = (a << 24) | 0xFFFFFF; }
			}
			((long(__stdcall *)(void *, u32))tv[20])(tex, 0);
			s_tex = tex;
		}
		else { ((u32(__stdcall *)(void *))tv[2])(tex); LOG("feel: the menu's font texture couldn't be filled - block letters"); }
	}
	sel(dc, oldF); sel(dc, oldB); del(dib); delDC(dc); del(font);
	if (s_tex) LOG("feel: the menu's lettering ready (Consolas)");
	return s_tex != 0;
}
static float gfK(const GFont &F, float h) { return h * 0.62f / rt::max(F.ascent, 1.0f); }
static float textW(const char *t, float h)
{
	if (s_useGF >= 0)
	{
		const GFont &F = gfNow();
		float k = gfK(F, h), w = 0;
		for (; *t; t++) { const GGlyph &G = F.g[(u8)*t]; w += (G.kl + G.w + G.kr) * k; }
		return w;
	}
	float k = h / (float)s_cellH, w = 0;
	for (; *t; t++) { int i = (u8)*t - 32; if (i >= 0 && i < 95) w += (s_gl[i].w + 1) * k; }
	return w;
}
static void textT(float x, float y, float h, const char *t, u32 c)
{
	if (s_useGF >= 0)
	{
		int sl = gfSlot();
		const GFont &F = s_gf[sl];
		TV *B = tbf[sl]; int &N = ntf[sl];
		float k = gfK(F, h), base = y + h * 0.80f;
		for (; *t; t++)
		{
			const GGlyph &G = F.g[(u8)*t];
			if (G.w > 0 && G.h > 0 && N + 6 <= 6 * 4096)
			{
				float X = x + G.kl * k - 0.5f, Y = base - G.top * k - 0.5f, w = G.w * k, gh = G.h * k;
				TV a = {X, Y, 0, 1, c, G.u0, G.v0}, b = {X + w, Y, 0, 1, c, G.u1, G.v0}, d = {X, Y + gh, 0, 1, c, G.u0, G.v1}, e = {X + w, Y + gh, 0, 1, c, G.u1, G.v1};
				B[N++] = a; B[N++] = b; B[N++] = d; B[N++] = b; B[N++] = e; B[N++] = d;
			}
			x += (G.kl + G.w + G.kr) * k;
		}
		return;
	}
	float k = h / (float)s_cellH;
	for (; *t; t++)
	{
		int i = (u8)*t - 32;
		if (i < 0 || i >= 95) continue;
		const Glyph &g = s_gl[i];
		if (*t != ' ' && nt + 6 <= (int)(sizeof tb / sizeof tb[0]))
		{
			float w = g.w * k, u0 = g.x / (float)kAtlas, v0 = g.y / (float)kAtlas, u1 = (g.x + g.w) / (float)kAtlas, v1 = (g.y + s_cellH) / (float)kAtlas;
			float X = x - 0.5f, Y = y - 0.5f;
			TV a = {X, Y, 0, 1, c, u0, v0}, b = {X + w, Y, 0, 1, c, u1, v0}, d = {X, Y + h, 0, 1, c, u0, v1}, e = {X + w, Y + h, 0, 1, c, u1, v1};
			tb[nt++] = a; tb[nt++] = b; tb[nt++] = d; tb[nt++] = b; tb[nt++] = e; tb[nt++] = d;
		}
		x += (g.w + 1) * k;
	}
}
static float nvS(float H) { return H / 720.0f; }
static u32 nvLine(u32 a) { u32 h = g_hudColor; u32 r = ((h >> 16) & 0xFF) * 200 / 255, g = ((h >> 8) & 0xFF) * 200 / 255, b = (h & 0xFF) * 200 / 255; return (a << 24) | (r << 16) | (g << 8) | b; }
static u32 nvText(u32 a) { return (a << 24) | (g_hudColor & 0xFFFFFF); }
static void quad2(float x, float y, float w, float h, u32 top, u32 bot)
{
	if (nv + 6 > (int)(sizeof vb / sizeof vb[0])) return;
	V a = {x, y, 0, 1, top}, b = {x + w, y, 0, 1, top}, d = {x, y + h, 0, 1, bot}, e = {x + w, y + h, 0, 1, bot};
	vb[nv++] = a; vb[nv++] = b; vb[nv++] = d; vb[nv++] = b; vb[nv++] = e; vb[nv++] = d;
}
static void nvBackground(float x, float y, float w, float h) { quad(x, y, w, h, 0xB4000000); }
static void nvTopBracket(float x, float y, float w, float s)
{
	float t = rt::max(1.0f, s);
	quad(x, y, w, t, nvLine(255));
	quad2(x, y, t, 50 * s, nvLine(255), nvLine(0)); quad2(x + w - t, y, t, 50 * s, nvLine(255), nvLine(0));
}
static void nvBottomBracket(float x, float y, float w, float s)
{
	float t = rt::max(1.0f, s);
	quad(x, y, w, t, nvLine(255));
	quad2(x, y - 50 * s + t, t, 50 * s, nvLine(0), nvLine(255)); quad2(x + w - t, y - 50 * s + t, t, 50 * s, nvLine(0), nvLine(255));
}
static float nvH(int font, float s) { fontSel(font); return s * (s_useGF >= 0 ? gfNow().ascent : 22.0f) / 0.62f; }
static void nvPut(int font, float x, float y, float s, const char *t, u32 c) { float h = nvH(font, s); textT(x, y, h, t, c); }
static float nvW(int font, float s, const char *t) { float h = nvH(font, s); return textW(t, h); }
static void nvHeader(float x, float y, float w, float s, const char *title)
{
	float t = rt::max(1.0f, s), tx = x + 60 * s, tw = nvW(F_TITLE, s, title);
	quad2(x, y, t, 40 * s, nvLine(255), nvLine(0));
	quad(x, y, tx - 10 * s - x, t, nvLine(255));
	nvPut(F_TITLE, tx, y - 10 * s, s, title, nvText(255));
	quad(tx + tw + 10 * s, y, x + w - (tx + tw + 10 * s), t, nvLine(255));
	quad2(x + w - t, y, t, 40 * s, nvLine(255), nvLine(0));
}
static void nvItem(float x, float y, float w, float s, const char *t, bool picked)
{
	float h = nvH(F_TEXT, s);
	if (picked)
	{
		float bt = rt::max(1.0f, s), by = y - 6 * s, bh = h + 8 * s;
		quad(x - 10 * s, by, w + 20 * s, bt, nvLine(255)); quad(x - 10 * s, by + bh - bt, w + 20 * s, bt, nvLine(255));
		quad(x - 10 * s, by, bt, bh, nvLine(255)); quad(x + w + 10 * s - bt, by, bt, bh, nvLine(255));
	}
	textT(x, y, h, t, nvText(picked ? 255 : 128));
}
static void build(float W, float H, bool font)
{
	u32 hud = g_hudColor & 0xFFFFFF, A = 0xFF000000 | hud;
	float s = nvS(H), u = rt::max(H / 720.0f, 0.75f);
	if (!font)
	{
		float w = 600 * u, lh = 30 * u, h = 112 * u + lh * N;
		float x0 = W * 0.5f - w * 0.5f, y0 = H * 0.18f;
		quad(x0, y0, w, h, 0xB4000000);
		float px = rt::max(2.0f, (float)(int)(H / 320));
		text(x0 + 20 * u, y0 + 20 * u, px, "WEAPON FEEL", A);
		for (int i = 0; i < N; i++)
		{
			float y = y0 + 80 * u + i * lh;
			char v[16]; rt::fmt(v, sizeof v, "%.0f%%", scale[i] * 100);
			text(x0 + 28 * u, y, px, kGroups[i].label, i == sel ? 0xFFFFFFFF : A);
			text(x0 + w - 96 * u, y, px, v, i == sel ? 0xFFFFFFFF : A);
		}
		return;
	}
	float th = nvH(F_TEXT, s), lh = rt::min(th + 20 * s, (H * 0.9f - 220 * s) / N);
	float w = 640 * s, h = 70 * s + lh * N + 70 * s;
	float x0 = W * 0.5f - w * 0.5f, y0 = rt::max(H * 0.05f, (H - h) * 0.5f);
	nvBackground(x0 - w * 0.04f, y0 - h * 0.08f, w * 1.08f, h * 1.16f);
	nvHeader(x0, y0, w, s, "WEAPON FEEL");
	float bx = x0 + 330 * s, bw = 190 * s, t = rt::max(1.0f, s);
	for (int i = 0; i < N; i++)
	{
		float y = y0 + 50 * s + i * lh;
		bool on = i == sel;
		nvItem(x0 + 40 * s, y, w - 80 * s, s, kGroups[i].label, on);
		float ty = y + th * 0.5f, full = bw * scale[i] / 3;
		quad(bx, ty, bw, t, nvLine(on ? 255 : 128));
		quad(bx + bw / 3 - t * 0.5f, ty - 6 * s, t, 12 * s + t, nvLine(on ? 255 : 128));
		quad(bx + full - 4 * s, ty - 7 * s, 8 * s, 14 * s + t, nvText(on ? 255 : 128));
		char v[16]; rt::fmt(v, sizeof v, "%.0f%%", scale[i] * 100);
		nvPut(F_TEXT, x0 + w - 40 * s - nvW(F_TEXT, s, v), y, s, v, nvText(on ? 255 : 128));
	}
	float by = y0 + 50 * s + lh * N + 24 * s;
	nvBottomBracket(x0, by + 40 * s, w, s);
	const char *k = "Up/Down) Select    Left/Right) Adjust    Shift) Fine    Backspace) Reset    F8) Close";
	float ks = s * 0.85f, kw = nvW(F_TEXT, ks, k);
	if (kw > w - 40 * s) { ks *= (w - 40 * s) / kw; kw = w - 40 * s; }
	nvPut(F_TEXT, x0 + w * 0.5f - kw * 0.5f, by, ks, k, nvText(128));
}
static void chatBox(float W, float H, bool font)
{
	u32 A = 0xFF000000 | (g_hudColor & 0xFFFFFF);
	float s = nvS(H), u = rt::max(H / 720.0f, 0.75f);
	char line[140];
	bool blink = ((int)(rt::now() * 2.5)) & 1;
	rt::fmt(line, sizeof line, "%s%s", g_chatBuf, blink ? "_" : " ");
	if (!font)
	{
		float x0 = 40 * u, y0 = H * 0.72f;
		quad(x0, y0, 520 * u, 64 * u, 0xB4000000);
		float px = rt::max(2.0f, (float)(int)(H / 360));
		text(x0 + 12 * u, y0 + 8 * u, px, "SAY:", A);
		text(x0 + 12 * u, y0 + 34 * u, px, line, A);
		return;
	}
	float w = 560 * s, h = 120 * s, x0 = 50 * s, y0 = H - h - 140 * s;
	nvBackground(x0 - 25 * s, y0 - 25 * s, w + 50 * s, h + 50 * s);
	nvTopBracket(x0, y0, w, s);
	nvBottomBracket(x0, y0 + h, w, s);
	const char *p = "Say something to your friends";
	nvPut(F_HUD, x0 + (w - nvW(F_HUD, s, p)) * 0.5f, y0 + 20 * s, s, p, nvText(255));
	const char *shown = line;
	while (*shown && nvW(F_HUD, s, shown) > w - 60 * s) shown++;
	nvPut(F_HUD, x0 + (w - nvW(F_HUD, s, shown)) * 0.5f, y0 + 58 * s, s, shown, nvText(255));
	const char *ok = "Enter) Send     Esc) Close";
	nvPut(F_TEXT, x0 + w - 16 * s - nvW(F_TEXT, s * 0.85f, ok), y0 + h - 34 * s, s * 0.85f, ok, nvText(128));
}
static void rangesDraw(float W, float H, bool font)
{
	fontSel(F_HUD);
	if (!ranges::on || menumode()) return;
	TESObjectREFR *pl = player();
	if (!pl) return;
	const float *p = pos(pl);
	float th = rt::max(2.0f, H / 450), yaw = rot(pl)[2];
	float face = rt::atan2(rt::cos(yaw), rt::sin(yaw));
	int base = 0;
	for (int ri = 0; ri < 4; ri++)
	{
		const ranges::Ring &R = ranges::kRings[ri];
		u32 glow = 0x50000000 | R.rgb, col = 0xE0000000 | R.rgb;
		float px = 0, py = 0, lx = 0, ly = 0; bool prev = false, lab = false;
		int li = (int)((face < 0 ? face + 2 * rt::PI : face) / (2 * rt::PI) * R.n + 0.5f) % R.n;
		for (int i = 0; i <= R.n; i++)
		{
			int k = i % R.n;
			float w[3];
			if (R.follow) { float a = 2 * rt::PI * k / R.n; w[0] = p[0] + R.r * rt::cos(a); w[1] = p[1] + R.r * rt::sin(a); w[2] = p[2] + 4; }
			else { if (base + k >= ranges::fill) { prev = false; continue; } memcpy(w, ranges::pts[base + k], 12); }
			float sx, sy;
			bool ok = handling::toScreen(w, W, H, &sx, &sy) && sx > -W && sx < 2 * W && sy > -H && sy < 2 * H;
			if (ok && prev) { line(px, py, sx, sy, th * 3, glow); line(px, py, sx, sy, th, col); }
			if (ok && k == li && sx > 0 && sx < W && sy > 0 && sy < H) { lx = sx; ly = sy; lab = true; }
			prev = ok; px = sx; py = sy;
		}
		if (!R.follow) base += R.n;
		if (lab)
		{
			char m[96]; rt::fmt(m, sizeof m, "%.0f m: %s", R.r / 70, R.label);
			if (font) textT(lx + 8, ly - H * 0.035f, H * 0.022f, m, col); else text(lx + 8, ly - H * 0.03f, rt::max(2.0f, H / 540), m, col);
		}
	}
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	for (int i = 0; i < n; i++)
	{
		const net::Ghost &g = gs[i];
		if (!g.fresh || ((g.s.world ^ world) & 0xFFFFFF) || ((g.s.cell ^ cid) & 0xFFFFFF) || (!g.s.cell) != (!cid)) continue;
		float dx = g.s.pos[0] - p[0], dy = g.s.pos[1] - p[1], dz = g.s.pos[2] - p[2], d = rt::sqrt(dx * dx + dy * dy + dz * dz);
		float h[3] = {g.s.pos[0], g.s.pos[1], g.s.pos[2] + 140}, sx, sy;
		if (!handling::toScreen(h, W, H, &sx, &sy) || sx < 0 || sx > W || sy < 0 || sy > H) continue;
		bool in = d <= ranges::kRings[0].r;
		char m[64]; rt::fmt(m, sizeof m, "%s %.0f m%s", g.name, d / 70, in ? " - shares XP" : "");
		u32 c = in ? 0xF0E8B030 : 0xC0B0B0B0;
		if (font) textT(sx - textW(m, H * 0.022f) * 0.5f, sy, H * 0.022f, m, c); else text(sx - 30, sy, rt::max(2.0f, H / 540), m, c);
	}
}
static void listenDraw(float W, float H, bool font)
{
	double now = rt::now();
	while (g_nListen && now - g_listen[0].t > 8) { memmove(g_listen, g_listen + 1, sizeof(Listen) * (g_nListen - 1)); g_nListen--; }
	if (!g_nListen || menumode() || !font) return;
	u32 hud = g_hudColor & 0xFFFFFF;
	float s = nvS(H), sz = nvH(F_HUD, s), lh = sz * 1.15f, maxW = rt::min(W * 0.9f, 800 * s);
	char lines[3][5][120]; int nl[3]; int total = 0;
	for (int i = 0; i < g_nListen; i++)
	{
		char all[480]; rt::fmt(all, sizeof all, "%s: %s", g_listen[i].speaker, g_listen[i].text);
		nl[i] = 0;
		const char *q = all;
		while (*q && nl[i] < 5)
		{
			int n = 0, cut = 0;
			char tmp[120];
			while (q[n] && n < 118)
			{
				if (q[n] == ' ') { memcpy(tmp, q, n); tmp[n] = 0; if (textW(tmp, sz) > maxW) break; cut = n; }
				n++;
			}
			{ memcpy(tmp, q, n); tmp[n] = 0; if (textW(tmp, sz) > maxW && cut > 0) n = cut; }
			memcpy(lines[i][nl[i]], q, n); lines[i][nl[i]][n] = 0; nl[i]++;
			q += n; while (*q == ' ') q++;
		}
		total += nl[i];
	}
	float y = H * 0.82f - total * lh;
	for (int i = 0; i < g_nListen; i++)
	{
		float age = (float)(now - g_listen[i].t);
		u32 a = age > 7 ? (u32)(255 * (8 - age)) : 255; if (a > 255) a = 255;
		for (int k = 0; k < nl[i]; k++, y += lh)
		{
			float tw = textW(lines[i][k], sz), x = (W - tw) * 0.5f;
			textT(x, y, sz, lines[i][k], (a << 24) | hud);
		}
	}
}
typedef long(__stdcall *PresentFn)(void *, const void *, const void *, void *, const void *);
static PresentFn s_present;
static void *s_sb;
typedef long(__stdcall *BeginFn)(void *);
static BeginFn s_begin;
static bool s_began;
static long __stdcall beginScene(void *dev)
{
	if (!s_began) { s_began = true; handling::camApply(); body::frame(); }
	return s_begin(dev);
}
static long __stdcall present(void *dev, const void *a, const void *b, void *c, const void *d)
{
	nv = 0;
	s_presents++;
	bool mark = rt::now() - g_blkAt < 0.1, tr = handling::g_trN > 0;
	bool rings = ranges::on && !menumode(), listen = g_nListen > 0 && !menumode();
	if (open || mark || tr || g_chatTyping || rings || listen)
	{
		void **vt = *(void ***)dev;
		struct VP { u32 x, y, w, h; float mn, mx; } vp;
		((long(__stdcall *)(void *, VP *))vt[48])(dev, &vp);
		void *bb = 0, *rt0 = 0;
		((long(__stdcall *)(void *, u32, u32, u32, void **))vt[18])(dev, 0, 0, 0, &bb);
		((long(__stdcall *)(void *, u32, void **))vt[38])(dev, 0, &rt0);
		u32 W = vp.w, H = vp.h;
		if (bb) { u32 desc[8] = {0}; if (((long(__stdcall *)(void *, u32 *))(*(void ***)bb)[12])(bb, desc) >= 0 && desc[6] && desc[7]) { W = desc[6]; H = desc[7]; } }
		if (mark) blockMark((float)W, (float)H);
		if (tr && !menumode()) tracers((float)W, (float)H);
		nt = 0; for (int &q : ntf) q = 0; fontSel(F_TEXT);
		bool font = (open || g_chatTyping || rings || listen) && fontAtlas(dev);
		if (rings) rangesDraw((float)W, (float)H, font);
		if (listen) listenDraw((float)W, (float)H, font);
		if (open) build((float)W, (float)H, font);
		else if (g_chatTyping) chatBox((float)W, (float)H, font);
		s_sb = 0; ((long(__stdcall *)(void *, u32, void **))vt[59])(dev, 1, &s_sb);
		if (open && !s_drawTold)
		{
			s_drawTold = true;
			LOG("feel: drawing the menu (%d vertices, %d of lettering) on the %ux%u back buffer; the game had left a %ux%u viewport at %u,%u on %s%s",
				nv, nt, W, H, vp.w, vp.h, vp.x, vp.y, rt0 == bb ? "the back buffer" : "another render target", s_sb ? "" : " - no state block: not drawn");
		}
		int ntAll = nt; for (int q : ntf) ntAll += q;
		if (s_sb && (nv || ntAll))
		{
			void **sv = *(void ***)s_sb;
			((long(__stdcall *)(void *))sv[4])(s_sb);
			if (bb && rt0 != bb) ((long(__stdcall *)(void *, u32, void *))vt[37])(dev, 0, bb);
			else { VP full = {0, 0, W, H, 0, 1}; ((long(__stdcall *)(void *, const VP *))vt[47])(dev, &full); }
			auto rs = (long(__stdcall *)(void *, u32, u32))vt[57];
			auto ts = (long(__stdcall *)(void *, u32, u32, u32))vt[67];
			(s_begin ? s_begin : (BeginFn)vt[41])(dev);
			rs(dev, 7, 0); rs(dev, 14, 0); rs(dev, 15, 0); rs(dev, 22, 1); rs(dev, 27, 1); rs(dev, 19, 5); rs(dev, 20, 6);
			rs(dev, 28, 0); rs(dev, 52, 0); rs(dev, 137, 0); rs(dev, 168, 0xF); rs(dev, 174, 0); rs(dev, 194, 0);
			((long(__stdcall *)(void *, u32, void *))vt[65])(dev, 0, 0);
			((long(__stdcall *)(void *, void *))vt[107])(dev, 0);
			((long(__stdcall *)(void *, void *))vt[92])(dev, 0);
			ts(dev, 0, 1, 2); ts(dev, 0, 2, 0); ts(dev, 0, 4, 2); ts(dev, 0, 5, 0); ts(dev, 1, 1, 1); ts(dev, 1, 4, 1);
			((long(__stdcall *)(void *, u32))vt[89])(dev, 0x004 | 0x040);
			if (nv) ((long(__stdcall *)(void *, u32, u32, const void *, u32))vt[83])(dev, 4, nv / 3, vb, sizeof(V));
			for (int q = -1; q < kGF; q++)
			{
				void *ftex = q < 0 ? s_tex : s_gf[q].tex;
				int cnt = q < 0 ? nt : ntf[q];
				const TV *buf = q < 0 ? tb : tbf[q];
				if (!cnt || !ftex) continue;
				auto ss = (long(__stdcall *)(void *, u32, u32, u32))vt[69];
				((long(__stdcall *)(void *, u32, void *))vt[65])(dev, 0, ftex);
				ts(dev, 0, 1, 4); ts(dev, 0, 2, 2); ts(dev, 0, 3, 0); ts(dev, 0, 4, 4); ts(dev, 0, 5, 2); ts(dev, 0, 6, 0);
				ss(dev, 0, 5, 2); ss(dev, 0, 6, 2); ss(dev, 0, 7, 0); ss(dev, 0, 1, 3); ss(dev, 0, 2, 3);
				((long(__stdcall *)(void *, u32))vt[89])(dev, 0x004 | 0x040 | 0x100);
				((long(__stdcall *)(void *, u32, u32, const void *, u32))vt[83])(dev, 4, cnt / 3, buf, sizeof(TV));
			}
			((long(__stdcall *)(void *))vt[42])(dev);
			if (bb && rt0 != bb && rt0) ((long(__stdcall *)(void *, u32, void *))vt[37])(dev, 0, rt0);
			((long(__stdcall *)(void *))sv[5])(s_sb);
		}
		if (s_sb) { ((u32(__stdcall *)(void *))(*(void ***)s_sb)[2])(s_sb); s_sb = 0; }
		if (bb) ((u32(__stdcall *)(void *))(*(void ***)bb)[2])(bb);
		if (rt0) ((u32(__stdcall *)(void *))(*(void ***)rt0)[2])(rt0);
	}
	handling::camRestore(); s_began = false;
	return s_present(dev, a, b, c, d);
}
static void hook()
{
	static bool tried;
	if (tried) return;
	tried = true;
	u32 rnd = ptr(0x11C73B4), dev = rnd ? ptr(rnd + 0x288) : 0;
	void **vt = dev ? *(void ***)dev : 0;
	char where[96] = "?";
	if (!vt || !addrInfo((u32)vt[17], where, sizeof where, true))
	{
		LOG("feel: no Direct3D 9 device found (renderer %x device %x, Present at %s) - the menu can't draw", rnd, dev, where);
		if (!vt) return;
	}
	if (!s_vprot) { HANDLE k = LoadLibraryA("kernel32.dll"); if (k) s_vprot = (int(__stdcall *)(void *, u32, u32, u32 *))GetProcAddress(k, "VirtualProtect"); }
	u32 old = 0;
	if (!s_vprot || !s_vprot(&vt[17], 4, 0x04, &old)) { LOG("feel: couldn't hook Present"); return; }
	s_present = (PresentFn)vt[17];
	vt[17] = (void *)&present;
	s_vprot(&vt[17], 4, old, &old);
	LOG("feel: Present hooked (it was at %s)", where);
	if (s_vprot(&vt[41], 4, 0x04, &old)) { s_begin = (BeginFn)vt[41]; vt[41] = (void *)&beginScene; s_vprot(&vt[41], 4, old, &old); LOG("handling: BeginScene hooked (the third-person camera's roll and shoulder)"); }
}
static void rehook()
{
	static double at;
	double t = rt::now();
	if (!s_present || t - at < 0.5) return;
	at = t;
	u32 rnd = ptr(0x11C73B4), dev = rnd ? ptr(rnd + 0x288) : 0;
	void **vt = dev ? *(void ***)dev : 0;
	if (!vt || !s_vprot) return;
	static int told;
	u32 old = 0;
	if (vt[17] == (void *)s_present && s_vprot(&vt[17], 4, 0x04, &old))
	{
		vt[17] = (void *)&present; s_vprot(&vt[17], 4, old, &old);
		if (told++ < 10) LOG("feel: the device's own Present was back in its table - ours put back (the menu, tracers, the shoulder camera)");
	}
	if (s_begin && vt[41] == (void *)s_begin && s_vprot(&vt[41], 4, 0x04, &old))
	{
		vt[41] = (void *)&beginScene; s_vprot(&vt[41], 4, old, &old);
		if (told++ < 10) LOG("handling: the device's own BeginScene was back in its table - ours put back");
	}
}
static void presentWhere()
{
	u32 rnd = ptr(0x11C73B4), dev = rnd ? ptr(rnd + 0x288) : 0;
	void **vt = dev ? *(void ***)dev : 0;
	char where[96] = "?";
	if (vt) addrInfo((u32)vt[17], where, sizeof where, true);
	LOG("feel: the menu is open but the game's Present hasn't reached ours for 1 s (%u calls so far): device %x, its Present now %s%s",
		s_presents, dev, vt && vt[17] == (void *)&present ? "ours" : where, s_present ? "" : " (never hooked)");
}
}
static float feelScale(const char *id) { return feel::of(id); }
static void installHooks()
{
	static bool done;
	if (done) return;
	done = true;
	{
		u8 *z = (u8 *)(0x11E097C + 4);
		LOG("handling: bIronSightsZoomEnable %d%s", *z, TUNE(adsKeepFov) > 0 ? " -> 0 (the sights keep your field of view)" : "");
		if (TUNE(adsKeepFov) > 0) *z = 0;
	}
	u32 o = 0;
	if (hookCall(0x5245BD, 0x9BCA60, (void *)projCreateHook, &o, "shots (Projectile::Create in TESObjectWEAP::Fire)")) { s_projCreate = (ProjCreateFn)o; handling::hookShots = true; }
}

namespace sprint { static float climbScale(); }
// ---- movement: mantling, climbing and wall kicks (the author, 2026-10-13: "wall jumping to where I can walk up to
namespace move
{
static const float kHavok = 0.142857f;
struct Ledge { bool ok; float z, h, wall, edge[3], stand[3]; };
static int state;
static int stateNow() { return state; }
static int climbKind;
static float A[3], B[3], C[3], dur, el, handT[3], vel[3], flyT;
static bool ctrlOff, grip;
static int kicks, upKicks;
static float reachW, camOff, lastCamOff, afterLog;
static float hintT[3]; static bool hintOn, hintHave; static float sinceProbeLog;
static bool dropOn, dropHit, dropUsed;
static bool dropGround; static float dropG, dropL[3], dropGlide;
static double sprintSeenT = -9;
static float dropJolt;
static float dropLand = -1;
static bool dropSlowed; static double dropSlowUntil;
static void dropStopEnd() { if (dropSlowed) { dropSlowed = false; console("sgtm 1"); LOG("move: dropkick - the hit's freeze over, the world's time back to normal"); } }
static void slowTick() { if (dropSlowed && rt::now() >= dropSlowUntil) dropStopEnd(); }
static float joltAt(float tau) { if (tau < 0) return 0; float x = tau / 0.06f, y = 1 - x; if (y < -12) return 0; float e = 1 + y / 32; for (int i = 0; i < 5; i++) e *= e; return x * e; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float ease(float u) { u = clampf(u, 0, 1); return u * u * (3 - 2 * u); }
static Script *s_setPos;
static const char *kSetPos3 = "Begin Function {ref rObj, float fx, float fy, float fz}\nrObj.SetPos X fx\nrObj.SetPos Y fy\nrObj.SetPos Z fz\nEnd";

static u32 ctrl(TESObjectREFR *pl)
{
	u32 p = ptr((u32)pl + 0x68);
	return p && rd<u8>(p + 0x28) <= 1 ? ptr(p + 0x138) : 0;
}
static int hkState(u32 cc) { return cc ? (int)rd<u32>(cc + 0x3F0) : -1; }
static void setVel(u32 cc, const float *v)
{
	u32 px = cc ? ptr(cc + 8) : 0;
	if (!px) return;
	float *pv = (float *)(px + 0x10);
	pv[0] = v[0] * kHavok; pv[1] = v[1] * kHavok; pv[2] = v[2] * kHavok; pv[3] = 0;
}
static void noFall(u32 cc, float z) { if (cc) { *(float *)(cc + 0x548) = 0; *(float *)(cc + 0x544) = z; } }
static float radius(u32 cc)
{
	float r = cc ? rd<float>(cc + 0x5F8) : 0;
	if (r > 1 && r < 2) r /= kHavok;
	return r > 5 && r < 60 ? r : TUNE(moveBodyRadius);
}
static void setPos(TESObjectREFR *pl, const float *p)
{
	if (s_setPos) g_script->CallFunctionAlt(s_setPos, player(), 4, pl, F(p[0]), F(p[1]), F(p[2]));
}
static bool cast(const float *a, const float *b, float *fr, bool *actor)
{
	u32 o; *actor = false;
	if (!rayHitObject(a, b, fr, &o)) return false;
	if (o) if (TESObjectREFR *r = refOfNode(o)) { u8 ty = formtype((void *)r); *actor = ty == 0x3B || ty == 0x3C; }
	return true;
}
static bool clear(const float *a, const float *b) { float fr; bool ac; return !cast(a, b, &fr, &ac); }
static void at(const float *p, const float *f, float d, float z, float *o) { o[0] = p[0] + f[0] * d; o[1] = p[1] + f[1] * d; o[2] = z; }

static bool findLedge(TESObjectREFR *pl, const float *f, float reach, float rad, Ledge &L)
{
	L.ok = false;
	const float *p = pos(pl);
	float probe = rt::max(TUNE(moveProbeUnits), TUNE(moveHintUnits)) + rad, wall = 1e9f, fr, a[3], b[3];
	bool ac;
	const float hs[3] = {25, 55, 90};
	for (int i = 0; i < 3; i++)
	{
		at(p, f, 0, p[2] + hs[i], a); at(p, f, probe, p[2] + hs[i], b);
		if (cast(a, b, &fr, &ac) && !ac && fr * probe < wall) wall = fr * probe;
	}
	if (wall > probe) return false;
	float lo = TUNE(moveMinLedge);
	float dIn = wall + 12;
	at(p, f, dIn, p[2] + reach + 15, a); at(p, f, dIn, p[2] + lo - 5, b);
	if (!cast(a, b, &fr, &ac) || ac) return false;
	float zl = a[2] + (b[2] - a[2]) * fr, h = zl - p[2];
	if (h < lo || h > reach) return false;
	float a2[3], b2[3];
	at(p, f, dIn + 18, zl + 25, a2); at(p, f, dIn + 18, zl - 25, b2);
	if (!cast(a2, b2, &fr, &ac) || ac || rt::absf(a2[2] + (b2[2] - a2[2]) * fr - zl) > 18) return false;
	at(p, f, 0, zl + 30, a); at(p, f, dIn + 24, zl + 30, b);
	if (!clear(a, b)) return false;
	float st[3];
	at(p, f, wall + rad + 14, zl + 3, st);
	float up[3] = {st[0], st[1], zl + TUNE(moveClearUnits)};
	if (!clear(st, up)) return false;
	L.ok = true; L.z = zl; L.h = h; L.wall = wall;
	at(p, f, wall, zl + 2, L.edge);
	L.stand[0] = st[0]; L.stand[1] = st[1]; L.stand[2] = zl + 2;
	return true;
}

static void sub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float norm(float *v) { float l = rt::sqrt(dot(v, v)); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } return l; }
static void apply(const float *m, const float *v, float *o) { for (int i = 0; i < 3; i++) o[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2]; }
static void tr(const float *m, float *o) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o[i * 3 + j] = m[j * 3 + i]; }
static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static void between(const float *a, const float *b, float k, float *m)
{
	float ax[3]; cross(a, b, ax);
	float s = norm(ax), c = dot(a, b);
	if (s < 1e-5f || k <= 0) { memcpy(m, kI, 36); return; }
	handling::rotAbout(ax, rt::atan2(s, c) * 180 / rt::PI * k, m);
}
static void writeNode(u32 n, const float *Wr, const float *D, const float *l)
{
	float WT[9], X[9], R[9], Rl[3], t[3];
	tr(Wr, WT); handling::mul3(WT, D, X); handling::mul3(X, Wr, R);
	apply(R, l, Rl);
	for (int i = 0; i < 3; i++) t[i] = l[i] - Rl[i];
	memcpy((void *)(n + 0x34), R, 36); memcpy((void *)(n + 0x58), t, 12);
}
static void nodeIdentity(u32 n) { const float Z[3] = {0, 0, 0}; memcpy((void *)(n + 0x34), kI, 36); memcpy((void *)(n + 0x58), Z, 12); }

enum { S_UP, S_FORE, S_HAND, S_F0, S_F1, S_F2, S_F3, S_F4, S_RUP, NSLOT };
static const char *kSlotName[NSLOT] = {"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 L Finger0", "Bip01 L Finger1",
	"Bip01 L Finger2", "Bip01 L Finger3", "Bip01 L Finger4", "Bip01 R UpperArm"};
static const char *kTipName[5] = {"Bip01 L Finger01", "Bip01 L Finger11", "Bip01 L Finger21", "Bip01 L Finger31", "Bip01 L Finger41"};
struct Slot { u32 bone, node, tip; };
static Slot sl[NSLOT];
static u32 aRoot;
static bool aWritten, aLogged;
static double armFailT = -99;
static bool unnamedNode(u32 n) { const char *nm = n ? (const char *)ptr(n + 8) : 0; return n && handling::isNodeObj(n) && (!nm || !nm[0]); }
static u32 insertAbove(u32 target)
{
	u32 parent = ptr(target + 0x18);
	if (!parent || !handling::isNodeObj(parent) || !handling::isNodeObj(target)) return 0;
	u32 arr = ptr(parent + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(parent + OFF_NODE_CHILDCOUNT);
	int idx = -1;
	for (u16 i = 0; arr && i < cnt; i++) if (ptr(arr + 4 * i) == target) idx = i;
	if (idx < 0) return 0;
	u32 n = ((u32(__cdecl *)())0xA5F030)();
	if (!n) return 0;
	*(u32 *)(target + 4) += 1;
	((void(__thiscall *)(u32, u32, u32))ptr(ptr(parent) + 0xE0))(parent, (u32)idx, n);
	((void(__thiscall *)(u32, u32, bool))ptr(ptr(n) + 0xDC))(n, target, true);
	*(u32 *)(target + 4) -= 1;
	return ptr(n + 0x18) == parent && ptr(target + 0x18) == n ? n : 0;
}
static bool armValid(u32 root)
{
	if (!root || root != aRoot) return false;
	for (int i = 0; i < NSLOT; i++)
	{
		if (i >= S_F0 && i <= S_F4 && !sl[i].bone) continue;
		if (!sl[i].node || ptr(sl[i].bone + 0x18) != sl[i].node || !ptr(sl[i].node + 0x18)) return false;
	}
	return true;
}
static void armIdentity()
{
	if (!aWritten) return;
	aWritten = false;
	TESObjectREFR *pl = player();
	if (!aRoot || (aRoot != firstPersonNode(pl) && aRoot != thirdPersonNode(pl)) || !armValid(aRoot)) return;
	for (int i = 0; i < NSLOT; i++) if (sl[i].node) nodeIdentity(sl[i].node);
}
static void armNeutral() { armIdentity(); }
static bool armFind(u32 root)
{
	aRoot = root; aWritten = false;
	memset(sl, 0, sizeof sl);
	int found = 0, made = 0, kept = 0;
	for (int i = 0; i < NSLOT; i++)
	{
		u32 b = findNamed(root, kSlotName[i], 40);
		sl[i].bone = b;
		if (!b) continue;
		found++;
		u32 p = ptr(b + 0x18);
		if (unnamedNode(p) && ptr(p + 0x18)) { sl[i].node = p; kept++; }
		else if ((sl[i].node = insertAbove(b))) made++;
		if (i >= S_F0 && i <= S_F4) sl[i].tip = findNamed(b, kTipName[i - S_F0], 1);
	}
	bool ok = true;
	for (int i = 0; i < NSLOT; i++) if (!(i >= S_F0 && i <= S_F4) && !sl[i].node) ok = false;
	if (ok && (ptr(sl[S_FORE].node + 0x18) != sl[S_UP].bone || ptr(sl[S_HAND].node + 0x18) != sl[S_FORE].bone)) ok = false;
	int fingers = 0, tips = 0;
	char fp[160]; int fat = 0; fp[0] = 0;
	for (int i = S_F0; i <= S_F4; i++)
	{
		u32 par = sl[i].node ? ptr(sl[i].node + 0x18) : 0;
		if (sl[i].node && par && handling::isAncestor(sl[S_HAND].bone, par)) fingers++; else sl[i].bone = sl[i].node = 0;
		if (sl[i].tip) tips++;
		const char *pn = par ? (const char *)ptr(par + 8) : 0;
		if (fat < 140) fat += rt::fmt(fp + fat, sizeof fp - fat, "%s%s", fat ? ", " : "", pn && pn[0] ? pn : "(ours/none)");
	}
	double t = rt::now();
	if (!ok)
	{
		if (t - armFailT > 5)
		{
			armFailT = t;
			LOG("move: arms not as expected (%d of %d bones, %d nodes ours already, %d put in; upper %x/%x fore %x/%x hand %x/%x) - no reach",
				found, NSLOT, kept, made, sl[S_UP].bone, sl[S_UP].node, sl[S_FORE].bone, sl[S_FORE].node, sl[S_HAND].bone, sl[S_HAND].node);
		}
		aRoot = 0;
		return false;
	}
	LOG("move: arm nodes ready - %d put in, %d ours already; %d fingers (%d with a tip joint; parents %s), right upper arm %x", made, kept, fingers, tips, fp, sl[S_RUP].bone);
	aLogged = false;
	return true;
}

static u32 shown[24]; static int nShown;
static bool forced;
static void showMark(u32 n)
{
	if (!n || nShown >= 24) return;
	u32 &f = *(u32 *)(n + OFF_NIAV_FLAGS);
	if (f & 1) { f &= ~1u; shown[nShown++] = n; }
}
static void showArms(u32 root, bool on)
{
	if (on == forced) return;
	if (on)
	{
		nShown = 0;
		for (u32 n = sl[S_HAND].bone; n; n = ptr(n + 0x18)) { showMark(n); if (n == root) break; }
		u32 arr = ptr(root + OFF_NODE_CHILDREN);
		u16 cnt = rd<u16>(root + OFF_NODE_CHILDCOUNT);
		for (u16 i = 0; arr && i < cnt && i < 32; i++)
		{
			u32 c = ptr(arr + 4 * i);
			const char *nm = c ? (const char *)ptr(c + 8) : 0;
			if (!nm || memcmp(nm, "UpperBody", 9)) continue;
			showMark(c);
			u32 a2 = ptr(c + OFF_NODE_CHILDREN);
			u16 c2 = handling::isNodeObj(c) ? rd<u16>(c + OFF_NODE_CHILDCOUNT) : 0;
			for (u16 k = 0; a2 && k < c2 && k < 32; k++) { u32 g = ptr(a2 + 4 * k); const char *gn = g ? (const char *)ptr(g + 8) : 0; if (gn && !rt::strcmp(gn, "Arms")) showMark(g); }
		}
		LOG("move: no weapon out - showing the arms for the reach (%d parts were hidden)", nShown);
	}
	else
		for (int i = 0; i < nShown; i++) *(u32 *)(shown[i] + OFF_NIAV_FLAGS) |= 1;
	if (!on) nShown = 0;
	forced = on;
}

enum { B_LUP, B_LFO, B_LHA, B_RUP, B_RFO, B_RHA, NBA };
static const char *kBName[NBA] = {"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand"};
static u32 bBone[NBA], bNode[NBA], bRoot;
static bool bWritten, bQOn, bLogged;
static handling::Xf bQx;
static double bFailT = -99;
static bool bValid(u32 root)
{
	if (!root || root != bRoot) return false;
	for (int i = 0; i < NBA; i++) if (!bNode[i] || ptr(bBone[i] + 0x18) != bNode[i] || !ptr(bNode[i] + 0x18)) return false;
	return true;
}
static bool bFind(u32 root)
{
	bRoot = 0; bWritten = false;
	int made = 0, kept = 0;
	for (int i = 0; i < NBA; i++)
	{
		bBone[i] = findNamed(root, kBName[i], 40); bNode[i] = 0;
		if (!bBone[i]) break;
		u32 p = ptr(bBone[i] + 0x18);
		if (unnamedNode(p) && ptr(p + 0x18)) { bNode[i] = p; kept++; }
		else if ((bNode[i] = insertAbove(bBone[i]))) made++;
	}
	bool ok = true;
	for (int i = 0; i < NBA; i++) if (!bNode[i]) ok = false;
	for (int k = 0; ok && k < NBA; k += 3)
		if (ptr(bNode[k + 1] + 0x18) != bBone[k] || ptr(bNode[k + 2] + 0x18) != bBone[k + 1]) ok = false;
	if (!ok)
	{
		if (rt::now() - bFailT > 5) { bFailT = rt::now(); LOG("cover: blind fire - the arms aren't as expected (nodes %x %x %x / %x %x %x) - no blind fire", bNode[0], bNode[1], bNode[2], bNode[3], bNode[4], bNode[5]); }
		return false;
	}
	bRoot = root; bLogged = false;
	LOG("cover: blind fire arm nodes ready (%d put in, %d ours already)", made, kept);
	return true;
}
static void blindArmsOff()
{
	bQOn = false;
	if (!bWritten) return;
	bWritten = false;
	u32 root = firstPersonNode(player());
	if (bValid(root)) for (int i = 0; i < NBA; i++) nodeIdentity(bNode[i]);
}
static bool blindQ(float *r, float *t) { if (!bQOn) return false; memcpy(r, bQx.r, 36); memcpy(t, bQx.t, 12); return true; }
struct ArmP { handling::Xf C, Lu, Lf, Lh, U0, F0, H0; float L1, L2; };
static bool armP(int k, ArmP &a)
{
	using handling::xfRead; using handling::xfMul;
	u32 clav = ptr(bNode[k] + 0x18);
	if (!clav) return false;
	a.C = xfRead(clav, 0x68); a.Lu = xfRead(bBone[k], 0x34); a.Lf = xfRead(bBone[k + 1], 0x34); a.Lh = xfRead(bBone[k + 2], 0x34);
	a.U0 = xfMul(a.C, a.Lu); a.F0 = xfMul(a.U0, a.Lf); a.H0 = xfMul(a.F0, a.Lh);
	float se[3], eh[3]; sub(a.F0.t, a.U0.t, se); sub(a.H0.t, a.F0.t, eh);
	a.L1 = rt::sqrt(dot(se, se)); a.L2 = rt::sqrt(dot(eh, eh));
	return a.L1 > 1 && a.L2 > 1 && a.L1 < 200 && a.L2 < 200;
}
static void armTo(int k, const ArmP &a, const handling::Xf &Ht, const float *pole, float *got)
{
	using handling::Xf; using handling::xfMul;
	const float *S = a.U0.t;
	float se[3], v[3]; sub(a.F0.t, S, se); sub(Ht.t, S, v);
	float d = norm(v);
	d = clampf(d, rt::absf(a.L1 - a.L2) + 0.5f, (a.L1 + a.L2) * clampf(TUNE(blindArmExtend), 0.5f, 0.995f));
	float p[3] = {pole[0], pole[1], pole[2]}, k0 = dot(p, v);
	for (int i = 0; i < 3; i++) p[i] -= v[i] * k0;
	if (norm(p) < 0.1f) { float k1 = dot(se, v); for (int i = 0; i < 3; i++) p[i] = se[i] - v[i] * k1; norm(p); }
	float ca = clampf((a.L1 * a.L1 + d * d - a.L2 * a.L2) / (2 * a.L1 * d), -1, 1), sa = rt::sqrt(1 - ca * ca);
	float u0[3] = {se[0] / a.L1, se[1] / a.L1, se[2] / a.L1}, u1[3], E2[3];
	for (int i = 0; i < 3; i++) { u1[i] = v[i] * ca + p[i] * sa; E2[i] = S[i] + a.L1 * u1[i]; }
	float D1[9]; between(u0, u1, 1, D1);
	float sh[3], r1[3], H1[3]; sub(a.H0.t, S, sh); apply(D1, sh, r1);
	for (int i = 0; i < 3; i++) H1[i] = S[i] + r1[i];
	float a2[3], b2[3]; sub(H1, E2, a2); norm(a2);
	for (int i = 0; i < 3; i++) b2[i] = S[i] + v[i] * d - E2[i];
	norm(b2);
	float D2[9]; between(a2, b2, 1, D2);
	writeNode(bNode[k], a.C.r, D1, a.Lu.t);
	Xf Un = a.U0; handling::mul3(D1, a.U0.r, Un.r);
	writeNode(bNode[k + 1], Un.r, D2, a.Lf.t);
	Xf Fn = a.F0; float D21[9]; handling::mul3(D2, D1, D21); handling::mul3(D21, a.F0.r, Fn.r); memcpy(Fn.t, E2, 12);
	Xf Hn0 = xfMul(Fn, a.Lh);
	float HT[9], D3[9]; tr(Hn0.r, HT); handling::mul3(Ht.r, HT, D3);
	writeNode(bNode[k + 2], Fn.r, D3, a.Lh.t);
	memcpy(got, Hn0.t, 12);
}
static bool bArms(ArmP &L, ArmP &R)
{
	u32 root = firstPersonNode(player());
	if (!root || (!bValid(root) && !bFind(root))) return false;
	return armP(0, L) && armP(3, R);
}
static bool blindReachOk(const float *tb)
{
	ArmP L, R;
	if (!bArms(L, R)) return false;
	float m = clampf(TUNE(blindArmExtend), 0.5f, 0.995f) * 0.97f;
	for (int k = 0; k < 2; k++)
	{
		const ArmP &a = k ? R : L;
		float T[3] = {a.H0.t[0] + tb[0] - a.U0.t[0], a.H0.t[1] + tb[1] - a.U0.t[1], a.H0.t[2] + tb[2] - a.U0.t[2]};
		if (rt::sqrt(dot(T, T)) > (a.L1 + a.L2) * m) return false;
	}
	return true;
}
static bool blindArms(TESObjectREFR *pl, float w, const float *E, const float *D, const float *Fv, float roll, const float *tb, float *moved)
{
	moved[0] = moved[1] = moved[2] = 0;
	if (thirdPerson(pl) || w < 0.002f) { blindArmsOff(); return false; }
	ArmP L, R;
	if (!bArms(L, R)) { blindArmsOff(); return false; }
	using handling::Xf;
	float A[9], Rr[9], Rm[9], d0[3] = {D[0], D[1], D[2]}, fv[3] = {Fv[0], Fv[1], Fv[2]};
	norm(d0); norm(fv);
	between(fv, d0, 1, A);
	handling::rotAbout(d0, roll, Rr);
	handling::mul3(Rr, A, Rm);
	auto mv = [&](const Xf &H, Xf &o) {
		o = H; handling::mul3(Rm, H.r, o.r);
		float q[3] = {H.t[0] - E[0], H.t[1] - E[1], H.t[2] - E[2]}, rq[3]; apply(Rm, q, rq);
		for (int i = 0; i < 3; i++) o.t[i] = E[i] + rq[i] + tb[i];
	};
	Xf HR, HL; mv(R.H0, HR); mv(L.H0, HL);
	float rw[3] = {d0[1], -d0[0], 0}; norm(rw);
	float pR[3], pL[3], aR[3], aL[3];
	sub(R.F0.t, R.U0.t, aR); norm(aR); sub(L.F0.t, L.U0.t, aL); norm(aL);
	for (int i = 0; i < 3; i++)
	{
		float dn = i == 2 ? -TUNE(blindElbowDown) : 0;
		pR[i] = aR[i] * (1 - w) + (rw[i] + dn) * w;
		pL[i] = aL[i] * (1 - w) + (-rw[i] + dn) * w;
	}
	float gotR[3], gotL[3];
	armTo(3, R, HR, pR, gotR);
	armTo(0, L, HL, pL, gotL);
	bWritten = true;
	Xf Hach = HR; memcpy(Hach.t, gotR, 12);
	bQx = handling::xfMul(R.H0, handling::xfInv(Hach)); bQOn = true;
	for (int i = 0; i < 3; i++) moved[i] = tb[i] + (gotR[i] - HR.t[i]);
	if (!bLogged && w > 0.9f)
	{
		bLogged = true;
		float sR[3], sL[3], mR[3], mL[3]; sub(R.U0.t, E, sR); sub(L.U0.t, E, sL); sub(gotR, HR.t, mR); sub(gotL, HL.t, mL);
		LOG("cover: blind fire arms - upper %.1f/%.1f forearm %.1f/%.1f (left/right); shoulders from the eye L %.0f %.0f %.0f R %.0f %.0f %.0f; hands short of the target by L %.1f R %.1f; the gun moved %.0f %.0f %.0f",
			L.L1, R.L1, L.L2, R.L2, sL[0], sL[1], sL[2], sR[0], sR[1], sR[2], rt::sqrt(dot(mL, mL)), rt::sqrt(dot(mR, mR)), moved[0], moved[1], moved[2]);
	}
	return true;
}

static void armTick(TESObjectREFR *pl, const float *target, float w, bool gripping, bool weaponOut)
{
	if (bWritten) { aWritten = false; return; }
	bool tp = thirdPerson(pl);
	u32 root = tp ? thirdPersonNode(pl) : firstPersonNode(pl);
	if (!root)
	{
		static double told;
		if (w > 0.5f && rt::now() - told > 5) { told = rt::now(); LOG("move: no reach drawn: no %s model", tp ? "third-person" : "first-person"); }
		aWritten = false; forced = false; nShown = 0; return;
	}
	if (aRoot && root != aRoot)
	{
		armIdentity();
		if (forced) { u32 r1 = firstPersonNode(pl); if (r1 && aRoot == r1) showArms(r1, false); else { forced = false; nShown = 0; } }
		LOG("move: the reaching arm is now your %s-person one", tp ? "third" : "first");
	}
	if (tp) weaponOut = true;
	if (!armValid(root) && !armFind(root)) return;
	if (w < 0.002f) { armIdentity(); if (forced) showArms(root, false); return; }
	bool holstered = !weaponOut && TUNE(moveShowArmsHolstered) > 0;
	if (!weaponOut && !holstered) { armIdentity(); return; }
	if (holstered != forced) showArms(root, holstered);
	using handling::Xf; using handling::xfRead; using handling::xfMul;
	u32 clav = ptr(sl[S_UP].node + 0x18);
	Xf Cw = xfRead(clav, 0x68);
	Xf Lu = xfRead(sl[S_UP].bone, 0x34), Lf = xfRead(sl[S_FORE].bone, 0x34), Lh = xfRead(sl[S_HAND].bone, 0x34);
	Xf U0 = xfMul(Cw, Lu), F0 = xfMul(U0, Lf), H0 = xfMul(F0, Lh);
	const float *S = U0.t, *E = F0.t, *H = H0.t;
	float se[3], eh[3];
	sub(E, S, se); sub(H, E, eh);
	float L1 = rt::sqrt(dot(se, se)), L2 = rt::sqrt(dot(eh, eh));
	if (L1 < 1 || L2 < 1 || L1 > 200 || L2 > 200)
	{
		static double told;
		if (rt::now() - told > 5) { told = rt::now(); LOG("move: left arm lengths %.1f / %.1f out of range (clavicle scale %.2f) - no reach", L1, L2, Cw.s); }
		return;
	}
	float yaw = rot(pl)[2], fw[3] = {rt::sin(yaw), rt::cos(yaw), 0}, lf[3] = {-fw[1], fw[0], 0}, up[3] = {0, 0, 1}, dn[3] = {0, 0, -1};
	float T[3];
	for (int i = 0; i < 3; i++) T[i] = H[i] + (target[i] - H[i]) * w;
	float v[3]; sub(T, S, v);
	float d = norm(v);
	float ext = gripping ? TUNE(moveArmGripExtend) : TUNE(moveArmReachExtend);
	d = clampf(d, rt::absf(L1 - L2) + 0.5f, (L1 + L2) * clampf(ext, 0.5f, 0.995f));
	float pa[3] = {se[0] - v[0] * dot(se, v), se[1] - v[1] * dot(se, v), se[2] - v[2] * dot(se, v)};
	norm(pa);
	float ph[3] = {dn[0] + lf[0] * 0.7f, dn[1] + lf[1] * 0.7f, dn[2]};
	float k0 = dot(ph, v); for (int i = 0; i < 3; i++) ph[i] -= v[i] * k0;
	norm(ph);
	float pole[3]; for (int i = 0; i < 3; i++) pole[i] = pa[i] * (1 - w) + ph[i] * w;
	if (norm(pole) < 0.1f) memcpy(pole, ph, 12);
	float ca = clampf((L1 * L1 + d * d - L2 * L2) / (2 * L1 * d), -1, 1), sa = rt::sqrt(1 - ca * ca);
	float E2[3], u0[3] = {se[0] / L1, se[1] / L1, se[2] / L1}, u1[3];
	for (int i = 0; i < 3; i++) { u1[i] = v[i] * ca + pole[i] * sa; E2[i] = S[i] + L1 * u1[i]; }
	float D1[9]; between(u0, u1, 1, D1);
	float H1[3], sh[3], r1[3]; sub(H, S, sh); apply(D1, sh, r1);
	for (int i = 0; i < 3; i++) H1[i] = S[i] + r1[i];
	float a2[3], b2[3]; sub(H1, E2, a2); norm(a2);
	for (int i = 0; i < 3; i++) b2[i] = S[i] + v[i] * d - E2[i];
	norm(b2);
	float D2[9]; between(a2, b2, 1, D2);
	writeNode(sl[S_UP].node, Cw.r, D1, Lu.t);
	Xf Un = U0; handling::mul3(D1, U0.r, Un.r);
	writeNode(sl[S_FORE].node, Un.r, D2, Lf.t);
	Xf Fn = F0; float D21[9]; handling::mul3(D2, D1, D21); handling::mul3(D21, F0.r, Fn.r); memcpy(Fn.t, E2, 12);
	Xf Hn0 = xfMul(Fn, Lh);
	float fd[3]; sub(Hn0.t, E2, fd); norm(fd);
	u32 mid = sl[S_F2].bone ? sl[S_F2].bone : sl[S_F1].bone;
	float hd[3] = {fd[0], fd[1], fd[2]};
	if (mid) { Xf m0 = xfMul(Hn0, xfRead(mid, 0x34)); sub(m0.t, Hn0.t, hd); if (norm(hd) < 0.01f) memcpy(hd, fd, 12); }
	float want[3];
	if (gripping) { for (int i = 0; i < 3; i++) want[i] = fw[i] + dn[i] * TUNE(moveWristHook); }
	else { for (int i = 0; i < 3; i++) want[i] = fd[i] + up[i] * TUNE(moveWristUp); }
	norm(want);
	float D3[9]; between(hd, want, w, D3);
	writeNode(sl[S_HAND].node, Fn.r, D3, Lh.t);
	Xf Hn = Hn0; handling::mul3(D3, Hn0.r, Hn.r);
	float hdn[3]; apply(D3, hd, hdn); norm(hdn);
	int fingersDone = 0;
	for (int i = S_F0; i <= S_F4; i++)
	{
		if (!sl[i].bone || !sl[i].node) continue;
		u32 par = ptr(sl[i].node + 0x18);
		Xf Pw = par == sl[S_HAND].bone ? Hn : xfRead(par, 0x68);
		Xf Lb = xfRead(sl[i].bone, 0x34), B0 = xfMul(Pw, Lb);
		float fdir[3];
		if (sl[i].tip) { Xf T0 = xfMul(B0, xfRead(sl[i].tip, 0x34)); sub(T0.t, B0.t, fdir); }
		else { fdir[0] = B0.r[0]; fdir[1] = B0.r[3]; fdir[2] = B0.r[6]; }
		if (norm(fdir) < 0.01f) continue;
		float fw2[3];
		if (gripping) { for (int k = 0; k < 3; k++) fw2[k] = hdn[k] + dn[k] * TUNE(moveFingerCurl); }
		else memcpy(fw2, hdn, 12);
		norm(fw2);
		float Df[9]; between(fdir, fw2, w * TUNE(moveFingerAmount) * (i == S_F0 ? 0.5f : 1.0f), Df);
		writeNode(sl[i].node, Pw.r, Df, Lb.t);
		fingersDone++;
	}
	if (forced && sl[S_RUP].node)
	{
		u32 rc = ptr(sl[S_RUP].node + 0x18);
		Xf RC = xfRead(rc, 0x68), Lr = xfRead(sl[S_RUP].bone, 0x34), R0 = xfMul(RC, Lr);
		u32 rf = findNamed(sl[S_RUP].bone, "Bip01 R Forearm", 2);
		float rd2[3];
		if (rf) { Xf RF = xfMul(R0, xfRead(rf, 0x34)); sub(RF.t, R0.t, rd2); } else { rd2[0] = R0.r[0]; rd2[1] = R0.r[3]; rd2[2] = R0.r[6]; }
		norm(rd2);
		float Dr[9]; between(rd2, dn, w, Dr);
		writeNode(sl[S_RUP].node, RC.r, Dr, Lr.t);
	}
	else if (sl[S_RUP].node) nodeIdentity(sl[S_RUP].node);
	aWritten = true;
	if (forced)
	{
		static u8 upd[64];
		((void(__thiscall *)(u32, void *, u32))0xA5DD70)(root, upd, 0);
		static bool told;
		if (!told)
		{
			told = true;
			u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
			const float *rp = (const float *)(root + 0x8C), *cp = camN ? (const float *)(camN + 0x8C) : rp;
			LOG("move: arms shown with no weapon: the first-person model is %.0f units from the camera", rt::sqrt((rp[0] - cp[0]) * (rp[0] - cp[0]) + (rp[1] - cp[1]) * (rp[1] - cp[1]) + (rp[2] - cp[2]) * (rp[2] - cp[2])));
		}
	}
	if (!aLogged && w > 0.9f)
	{
		aLogged = true;
		LOG("move: left arm upper %.1f forearm %.1f units, reach %.0f%% of its length; hand %.1f units from the target, %d fingers %s%s", L1, L2, ext * 100,
			rt::sqrt((target[0] - H[0]) * (target[0] - H[0]) + (target[1] - H[1]) * (target[1] - H[1]) + (target[2] - H[2]) * (target[2] - H[2])),
			fingersDone, gripping ? "curled over the edge" : "open", forced ? ", arms shown (no weapon out)" : "");
	}
}

static rider::Body pk;
static u32 pkHolder, pkParent, pkKidRoot[2];
static HiddenSet pkKids[2];
static float pkO[3], pkX[3], pkFw[3], pkW, pkH, pkEyeH, pkT, pkStandY;
static float moveWorstDt, moveStartMs;
static float pkLookDown, pkLookW, pkGrip[2][3]; static bool pkGripOk;
static int pkKind;
static bool pkOn, pkThird;
static u32 pkSig;
static bool pkDirty = true;
static u32 pkSeenSig;
static bool pkBroken;
static u32 sig3ptr(TESObjectREFR *pl)
{
	u32 n3 = thirdPersonNode(pl);
	if (!n3) return 0;
	u32 h = n3 * 2654435761u;
	u32 arr = ptr(n3 + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(n3 + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < cnt && i < 128; i++) h = (h ^ ptr(arr + 4 * i)) * 16777619u;
	return h ^ cnt;
}
static u32 sig3(TESObjectREFR *pl)
{
	u32 n3 = thirdPersonNode(pl);
	if (!n3) return 0;
	u32 h = 2166136261u;
	u32 arr = ptr(n3 + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(n3 + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < cnt && i < 128; i++)
	{
		u32 c = ptr(arr + 4 * i);
		const char *nm = c ? (const char *)ptr(c + 8) : 0;
		h = (h ^ (c ? 1u : 0u)) * 16777619u;
		for (int k = 0; nm && k < 64 && nm[k]; k++) h = (h ^ (u8)nm[k]) * 16777619u;
		h = (h ^ 0x2Fu) * 16777619u;
	}
	return h ^ cnt;
}
static const char *kBlendName[] = {"Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck", "Bip01 Neck1", "Bip01 Head",
	"Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
	"Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 R Toe0"};
enum { NBN = sizeof kBlendName / sizeof kBlendName[0], NBF = 30, NB = NBN + NBF };
static u32 bD[NB], bL[NB];
static float bSave[NB][9];
static float bRootSave[13];
static float bLiveR[9], bLiveT[3], bLiveS = 1, bLiveYaw, bLivePos[3];
static bool pkFading; static float pkFadeT;
static float bLiveAt0[3][9];
static void blendName(int i, char *nm, int cap)
{
	if (i < NBN) { rt::fmt(nm, cap, "%s", kBlendName[i]); return; }
	int k = i - NBN, h = k / 15, f = (k % 15) / 3, j = k % 3;
	rt::fmt(nm, cap, j ? "Bip01 %c Finger%d%d" : "Bip01 %c Finger%d", h ? 'R' : 'L', f, j);
}
static void q4(const float *m, float *q)
{
	float tr = m[0] + m[4] + m[8];
	if (tr > 0) { float s = rt::sqrt(tr + 1) * 2; q[0] = 0.25f * s; q[1] = (m[7] - m[5]) / s; q[2] = (m[2] - m[6]) / s; q[3] = (m[3] - m[1]) / s; }
	else if (m[0] > m[4] && m[0] > m[8]) { float s = rt::sqrt(1 + m[0] - m[4] - m[8]) * 2; q[0] = (m[7] - m[5]) / s; q[1] = 0.25f * s; q[2] = (m[1] + m[3]) / s; q[3] = (m[2] + m[6]) / s; }
	else if (m[4] > m[8]) { float s = rt::sqrt(1 + m[4] - m[0] - m[8]) * 2; q[0] = (m[2] - m[6]) / s; q[1] = (m[1] + m[3]) / s; q[2] = 0.25f * s; q[3] = (m[5] + m[7]) / s; }
	else { float s = rt::sqrt(1 + m[8] - m[0] - m[4]) * 2; q[0] = (m[3] - m[1]) / s; q[1] = (m[2] + m[6]) / s; q[2] = (m[5] + m[7]) / s; q[3] = 0.25f * s; }
}
static void m9(const float *q, float *m)
{
	float w = q[0], x = q[1], y = q[2], z = q[3];
	m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z); m[2] = 2 * (x * z + w * y);
	m[3] = 2 * (x * y + w * z); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
	m[6] = 2 * (x * z - w * y); m[7] = 2 * (y * z + w * x); m[8] = 1 - 2 * (x * x + y * y);
}
static void rotBlend(const float *a, const float *b, float k, float *o)
{
	float qa[4], qb[4], q[4];
	q4(a, qa); q4(b, qb);
	float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3], sg = d < 0 ? -1.0f : 1.0f, l = 0;
	for (int i = 0; i < 4; i++) { q[i] = qa[i] * (1 - k) + qb[i] * sg * k; l += q[i] * q[i]; }
	l = rt::sqrt(l);
	if (l < 1e-6f) { memcpy(o, b, 36); return; }
	for (int i = 0; i < 4; i++) q[i] /= l;
	m9(q, o);
}
static void liveRootInHolder(TESObjectREFR *pl, float *R, float *T, float *S)
{
	float dy = rot(pl)[2] - bLiveYaw, c = rt::cos(dy), sn = rt::sin(dy);
	float Z[9] = {c, sn, 0, -sn, c, 0, 0, 0, 1}, Rw[9], off[3] = {bLiveT[0] - bLivePos[0], bLiveT[1] - bLivePos[1], bLiveT[2] - bLivePos[2]}, ow[3];
	handling::mul3(Z, bLiveR, Rw);
	for (int i = 0; i < 3; i++) ow[i] = Z[i * 3] * off[0] + Z[i * 3 + 1] * off[1] + Z[i * 3 + 2] * off[2];
	const float *p = pos(pl);
	float tw[3] = {p[0] + ow[0], p[1] + ow[1], p[2] + ow[2]};
	float H[9] = {pkX[0], pkFw[0], 0, pkX[1], pkFw[1], 0, 0, 0, 1}, HT[9];
	tr(H, HT);
	handling::mul3(HT, Rw, R);
	float d[3] = {tw[0] - pkO[0], tw[1] - pkO[1], tw[2] - pkO[2]};
	apply(HT, d, T);
	*S = bLiveS;
}
static u32 slGun, slFrom, slBone;
static float slOrig[13], slAxB[3], slAxU[3], slWS = 1;
static bool slHip;
static void sling(TESObjectREFR *pl)
{
	if (slGun || TUNE(moveSlingGun) <= 0 || !pk.root) return;
	u32 proc = handling::process(pl);
	if (!proc || !rd<u8>(proc + 0x135)) return;
	u32 n3 = thirdPersonNode(pl);
	u32 hand = n3 ? findNamed(n3, "Bip01 R Hand", 40) : 0, wb = hand ? findNamed(hand, "Weapon", 3) : 0;
	if (!wb && n3) wb = findNamed(n3, "Weapon", 40);
	u32 g = 0;
	if (wb)
	{
		u32 arr = ptr(wb + OFF_NODE_CHILDREN);
		u16 cnt = rd<u16>(wb + OFF_NODE_CHILDCOUNT);
		for (u16 i = 0; arr && i < cnt && i < 16 && !g; i++) { u32 c = ptr(arr + 4 * i); if (c && handling::isNodeObj(c)) g = c; }
	}
	static double told;
	if (!g) { if (rt::now() - told > 10) { told = rt::now(); LOG("move: no gun to carry (Weapon node %x%s)", wb, wb ? " has no gun under it" : " not found"); } return; }
	slBone = rider::findByName(pk.root, "Bip01 Spine2");
	if (!slBone) slBone = rider::findByName(pk.root, "Bip01 Spine1");
	if (!slBone) return;
	tpgun::identity();
	memcpy(slOrig, (void *)(g + 0x34), 52);
	using handling::Xf; using handling::xfRead;
	Xf Wg = xfRead(g, 0x68);
	slWS = Wg.s;
	float b[3] = {0, 1, 0};
	u32 pn = rider::findByName(g, "ProjectileNode");
	bool fromNode = false;
	if (pn)
	{
		Xf Wp = xfRead(pn, 0x68);
		float d[3] = {Wp.t[0] - Wg.t[0], Wp.t[1] - Wg.t[1], Wp.t[2] - Wg.t[2]}, l[3], RT[9];
		tr(Wg.r, RT); apply(RT, d, l);
		if (norm(l) > 1e-3f && rt::sqrt(dot(d, d)) > 3) { memcpy(b, l, 12); fromNode = true; }
	}
	memcpy(slAxB, b, 12);
	float u[3] = {0, 0, 1}, k = dot(u, b);
	for (int i = 0; i < 3; i++) u[i] -= b[i] * k;
	if (norm(u) < 0.1f) { u[0] = 1; u[1] = u[2] = 0; }
	memcpy(slAxU, u, 12);
	int wt = handling::wtype;
	slHip = !(wt >= 5 && wt <= 9);
	rider::hold(g); rider::hold(wb);
	rider::detach(wb, g);
	rider::attach(slBone, g);
	slGun = g; slFrom = wb;
	const char *gn = (const char *)ptr(g + 8);
	LOG("move: your gun %s carried at your %s through the move (barrel %.2f %.2f %.2f in its own frame%s)", gn ? gn : "?", slHip ? "hip" : "back", b[0], b[1], b[2], fromNode ? ", from its ProjectileNode" : ", assumed");
}
static void slingPlace()
{
	if (!slGun || !slBone || !pk.root) return;
	using handling::Xf; using handling::xfRead;
	Xf S2 = xfRead(slBone, 0x68);
	float nk[3], sp[3], pv[3], lu[3], ru[3];
	memcpy(nk, (void *)(pk.bone[rider::NECK] + 0x8C), 12); memcpy(sp, (void *)(pk.bone[rider::SPINE] + 0x8C), 12);
	memcpy(pv, (void *)(pk.bone[rider::PELVIS] + 0x8C), 12);
	memcpy(lu, (void *)(pk.bone[rider::L_UPPER] + 0x8C), 12); memcpy(ru, (void *)(pk.bone[rider::R_UPPER] + 0x8C), 12);
	float up[3], rgt[3], fwd[3];
	sub(nk, sp, up); if (norm(up) < 0.1f) return;
	sub(ru, lu, rgt); { float k = dot(rgt, up); for (int i = 0; i < 3; i++) rgt[i] -= up[i] * k; } if (norm(rgt) < 0.1f) return;
	cross(up, rgt, fwd);
	float d[3], top[3], g[3];
	if (!slHip)
	{
		for (int i = 0; i < 3; i++) { d[i] = -0.5f * rgt[i] - up[i]; top[i] = -fwd[i]; g[i] = S2.t[i] - fwd[i] * TUNE(moveSlingBack) + rgt[i] * 7 + up[i] * 5; }
	}
	else
	{
		for (int i = 0; i < 3; i++) { d[i] = -up[i] + 0.1f * fwd[i]; top[i] = fwd[i]; g[i] = pv[i] + rgt[i] * 12 - up[i] * 2; }
	}
	norm(d);
	{ float k = dot(top, d); for (int i = 0; i < 3; i++) top[i] -= d[i] * k; } norm(top);
	float sd[3], sb[3]; cross(d, top, sd); cross(slAxB, slAxU, sb);
	float M[9], RT[9], Lr[9], dl[3], Lt[3];
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i * 3 + j] = d[i] * slAxB[j] + top[i] * slAxU[j] + sd[i] * sb[j];
	tr(S2.r, RT); handling::mul3(RT, M, Lr);
	sub(g, S2.t, dl); apply(RT, dl, Lt);
	for (int i = 0; i < 3; i++) Lt[i] /= S2.s;
	memcpy((void *)(slGun + 0x34), Lr, 36); memcpy((void *)(slGun + 0x58), Lt, 12); *(float *)(slGun + 0x64) = slWS / S2.s;
	updateNode(slGun);
}
static void unsling()
{
	if (!slGun) return;
	u32 g = slGun, wb = slFrom; slGun = slFrom = 0;
	if (slBone && ptr(g + 0x18) == slBone) rider::detach(slBone, g);
	TESObjectREFR *pl = player();
	u32 n3 = pl ? thirdPersonNode(pl) : 0;
	bool back = wb && n3 && handling::isAncestor(n3, wb);
	if (back) { memcpy((void *)(g + 0x34), slOrig, 52); rider::attach(wb, g); }
	rider::release(g); rider::release(wb);
	LOG("move: your gun back in your hand%s", back ? "" : " - not put back: your body was rebuilt meanwhile");
}
static void pkDestroy()
{
	unsling();
	pkFading = false;
	if (pkHolder)
	{
		if (pkParent) rider::detach(pkParent, pkHolder);
		if (pk.root) { rider::detach(pkHolder, pk.root); rider::release(pk.root); }
		rider::release(pkHolder);
	}
	if (pkParent) rider::release(pkParent);
	pkHolder = pkParent = 0; pk.root = 0;
}
static void pkVisible(bool on);
static void pkPark()
{
	unsling();
	pkFading = false;
	if (!pkHolder) return;
	pkVisible(false);
	if (pkParent) { rider::detach(pkParent, pkHolder); rider::release(pkParent); pkParent = 0; }
}
static void pkVisible(bool on)
{
	if (!pkHolder) return;
	u32 &f = *(u32 *)(pkHolder + OFF_NIAV_FLAGS);
	if (on) f &= ~1u; else f |= 1u;
}
static bool pkBuild(TESObjectREFR *pl, const char *why)
{
	if (TUNE(moveFullBody) <= 0) return false;
	double t0 = rt::now();
	{
		u32 sg = sig3ptr(pl);
		if (sg != pkSeenSig) { pkSeenSig = sg; g_gearChangedAt = t0; }
		if (t0 - g_gearChangedAt < TUNE(moveCopySettleSec) || handling::curAction == 0 || handling::curAction == 1)
		{
			static double told;
			if (t0 - told > 2) { told = t0; LOG("move: full body not copied yet (%s) - your 3D changed %.1f s ago%s; next quiet moment", why, (float)(t0 - g_gearChangedAt), handling::curAction == 0 || handling::curAction == 1 ? ", a weapon going in/out" : ""); }
			pkDirty = true;
			return false;
		}
	}
	pkDestroy();
	u32 n3 = thirdPersonNode(pl);
	u32 par = n3 ? ptr(n3 + 0x18) : 0;
	if (!n3 || !par)
	{
		static double toldNo;
		if (t0 - toldNo > 2) { toldNo = t0; LOG("move: full body: no third-person model to copy (%s)", why); }
		return false;
	}
	if (pkBroken) return false;
	neutral3rd();
	double tCopy = rt::now();
	u32 c = rider::copy(n3);
	double tCopied = rt::now();
	if (rider::lastCopyFaulted())
	{
		pkBroken = true;
		LOG("move: full body: the game faulted inside its copy of your body (at %08x) - caught; the full-body double is off for this session (climbs go on with your arms)", g_gFaultEip);
		return false;
	}
	if (!c) { LOG("move: full body: the copy failed"); return false; }
	rider::hold(c);
	if (!rider::bind(pk, c)) { rider::release(c); pk.root = 0; LOG("move: full body: the copy has no human skeleton"); return false; }
	pkHolder = ((u32(__cdecl *)())0xA5F030)();
	if (!pkHolder) { rider::release(c); pk.root = 0; return false; }
	rider::hold(pkHolder);
	rider::attach(pkHolder, c);
	pkParent = par;
	rider::hold(pkParent);
	pkVisible(false);
	rider::attach(pkParent, pkHolder);
	pkSig = sig3(pl); pkDirty = false;
	{ char nm[40]; for (int i = 0; i < NB; i++) { blendName(i, nm, sizeof nm); bD[i] = rider::findByName(c, nm); } }
	double tBound = rt::now();
	int out = 0, sk = rider::skinCheck(c, &out);
	int pr[4]; rider::lastPruned(pr);
	double tEnd = rt::now();
	float cms[5]; rider::lastCopyMs(cms);
	LOG("move: full body made ahead (%s) in %.0f ms (pose %.0f, copy %.0f [lift %.0f, clone %.0f, fix-up %.0f, strip %.0f, prune %.0f], bones %.0f, check %.0f) - copy %x, %d skinned meshes (%d on outside bones), left out %d lights %d particles %d flashes %d guns; %d parts lifted off your body for the copy",
		why, (tEnd - t0) * 1000, (tCopy - t0) * 1000, (tCopied - tCopy) * 1000, cms[0], cms[1], cms[2], cms[3], cms[4], (tBound - tCopied) * 1000, (tEnd - tBound) * 1000, c, sk, out, pr[0], pr[1], pr[2], pr[3], rider::lastLifted());
	return true;
}
static void pkReparent(TESObjectREFR *pl);
static void pkPrepare(TESObjectREFR *pl, const char *why)
{
	if (TUNE(moveFullBody) <= 0 || TUNE(moveOn) <= 0 || pkOn || pkFading || g_driving || !pl) return;
	if (!pkHolder || pkDirty || sig3(pl) != pkSig) pkBuild(pl, why);
	else pkReparent(pl);
}
static void pkShowYou(const char *why)
{
	for (int i = 0; i < 2; i++) { showChildren(pkKidRoot[i], pkKids[i]); pkKidRoot[i] = 0; }
	pkVisible(false);
	handling::g_camShift[0] = handling::g_camShift[1] = handling::g_camShift[2] = 0;
	unsling();
	LOG("move: full body hidden (%s)", why);
}
static void pkFadeStop(const char *why) { if (!pkFading) return; pkFading = false; pkShowYou(why); }
static bool pkBlendingIn;
static void pkEnd(const char *why, bool blend = false)
{
	if (pkFading) pkFadeStop(why);
	if (!pkOn) return;
	pkOn = false; pkBlendingIn = false;
	if (blend && pkThird && TUNE(moveBlendOutSec) > 0.01f && pk.root && pkHolder)
	{
		for (int i = 0; i < NB; i++) if (bD[i]) memcpy(bSave[i], (void *)(bD[i] + 0x34), 36);
		memcpy(bRootSave, (void *)(pk.root + 0x34), 52);
		pkFading = true; pkFadeT = 0;
		float worst = 0;
		for (int k = 0; k < 3; k++)
		{
			int i = k == 0 ? 0 : k == 1 ? 15 : 12;
			if (!bL[i]) continue;
			const float *a = bLiveAt0[k], *b = (const float *)(bL[i] + 0x34);
			float tr3 = 0; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) tr3 += a[r * 3 + c] * b[r * 3 + c];
			float cs = rt::clamp((tr3 - 1) * 0.5f, -1.0f, 1.0f), ang = rt::atan2(rt::sqrt(rt::max(0.0f, 1 - cs * cs)), cs) * 180 / rt::PI;
			if (ang > worst) worst = ang;
		}
		LOG("move: blending back into your body (%s) - your hidden body's pelvis/thigh/arm turned up to %.1f deg meanwhile (%s)", why, worst, worst > 0.5f ? "it kept animating" : "it was frozen");
		return;
	}
	pkShowYou(why);
}
static void pkFadeTick(TESObjectREFR *pl, float dt)
{
	if (!pkFading) return;
	if (!pk.root || !pkHolder) { pkFadeStop("nothing to blend"); return; }
	pkFadeT += dt;
	float k = ease(pkFadeT / rt::max(TUNE(moveBlendOutSec), 0.02f)), o[9];
	for (int i = 0; i < NB; i++)
		if (bD[i] && bL[i]) { rotBlend(bSave[i], (const float *)(bL[i] + 0x34), k, o); memcpy((void *)(bD[i] + 0x34), o, 36); }
	float R[9], T[3], S;
	liveRootInHolder(pl, R, T, &S);
	float *rl = (float *)(pk.root + 0x34);
	rotBlend(bRootSave, R, k, o); memcpy(rl, o, 36);
	for (int i = 0; i < 3; i++) rl[9 + i] = bRootSave[9 + i] + (T[i] - bRootSave[9 + i]) * k;
	rl[12] = bRootSave[12] + (S - bRootSave[12]) * k;
	updateNode(pkHolder);
	slingPlace();
	if (k >= 1) pkFadeStop("blended back into yours");
}
static void pkHide(int i, u32 n)
{
	if (pkKidRoot[i] != n) { pkKids[i].n = 0; pkKidRoot[i] = n; }
	hideChildren(n, pkKids[i]);
}
static void pkFrame(TESObjectREFR *pl, const float *f)
{
	const float *p = pos(pl);
	memcpy(pkO, p, 12);
	pkFw[0] = f[0]; pkFw[1] = f[1]; pkFw[2] = 0;
	pkX[0] = f[1]; pkX[1] = -f[0]; pkX[2] = 0;
	pkT = 0;
}
static void pkReparent(TESObjectREFR *pl)
{
	u32 n3 = thirdPersonNode(pl), np = n3 ? ptr(n3 + 0x18) : 0;
	if (!pkHolder || !np || np == pkParent) return;
	rider::hold(np);
	if (pkParent) { rider::detach(pkParent, pkHolder); rider::release(pkParent); }
	pkParent = np;
	rider::attach(pkParent, pkHolder);
	LOG("move: full body moved to the new cell's node (no new copy)");
}
static bool pkBegin(TESObjectREFR *pl, int kind, const float *f, float wall, float h, float standY)
{
	if (TUNE(moveFullBody) <= 0) return false;
	pkKind = kind; pkW = wall; pkH = h; pkStandY = standY;
	if (pkOn) { pkFrame(pl, f); return true; }
	if (pkFading) pkFadeStop("a new move");
	u32 n3 = thirdPersonNode(pl);
	if (!pkHolder || !pk.root) { if (!pkBuild(pl, "none ready - made now")) return false; }
	else pkReparent(pl);
	pkThird = thirdPerson(pl);
	showChildren(pkKidRoot[0], pkKids[0]); showChildren(pkKidRoot[1], pkKids[1]); pkKidRoot[0] = pkKidRoot[1] = 0;
	pkHide(0, n3); pkHide(1, firstPersonNode(pl));
	u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
	pkEyeH = camN ? rd<float>(camN + 0x8C + 8) - pos(pl)[2] : 120;
	if (pkThird || !(pkEyeH > 60 && pkEyeH < 170)) pkEyeH = 120;
	pkFrame(pl, f);
	pkOn = true;
	pkVisible(true);
	if (pkThird)
	{
		char nm[40];
		for (int i = 0; i < NB; i++) { blendName(i, nm, sizeof nm); bL[i] = n3 ? findNamed(n3, nm, 40) : 0; }
		using handling::Xf; using handling::xfRead;
		Xf W = xfRead(n3, 0x68);
		memcpy(bLiveR, W.r, 36); memcpy(bLiveT, W.t, 12); bLiveS = W.s;
		bLiveYaw = rot(pl)[2]; memcpy(bLivePos, pos(pl), 12);
		pkBlendingIn = TUNE(moveBlendInSec) > 0.01f;
		for (int k = 0; k < 3; k++) { int i = k == 0 ? 0 : k == 1 ? 15 : 12; if (bL[i]) memcpy(bLiveAt0[k], (void *)(bL[i] + 0x34), 36); }
		sling(pl);
	}
	LOG("move: full body shown (%s) - eye %.0f over the feet, %s", kind == 1 ? "vault" : kind == 2 ? "mantle" : kind == 3 ? "climb" : kind == 4 ? "kick up" : "kick off", pkEyeH, pkThird ? "third person" : "first person");
	return true;
}
static void fr(const float *w, float *o) { float d[3] = {w[0] - pkO[0], w[1] - pkO[1], w[2] - pkO[2]}; o[0] = d[0] * pkX[0] + d[1] * pkX[1]; o[1] = d[0] * pkFw[0] + d[1] * pkFw[1]; o[2] = d[2]; }
static u32 poseBody() { return (pkOn || pkFading) && pk.root && pkHolder ? pk.root : 0; }
static void pkTick(TESObjectREFR *pl, float u, float dt)
{
	if (!pkOn || !pk.root || !pkHolder) return;
	pkT += dt;
	pkHide(0, thirdPersonNode(pl)); pkHide(1, firstPersonNode(pl));
	{
		using handling::Xf; using handling::xfRead; using handling::xfMul; using handling::xfInv;
		Xf Pw = xfRead(pkParent, 0x68), D;
		float R[9] = {pkX[0], pkFw[0], 0, pkX[1], pkFw[1], 0, 0, 0, 1};
		memcpy(D.r, R, 36); memcpy(D.t, pkO, 12); D.s = 1;
		Xf L = xfMul(xfInv(Pw), D);
		memcpy((void *)(pkHolder + 0x34), L.r, 36); memcpy((void *)(pkHolder + 0x58), L.t, 12); *(float *)(pkHolder + 0x64) = L.s;
	}
	rider::Free F;
	float pw[3] = {pos(pl)[0], pos(pl)[1], pos(pl)[2] + pkEyeH}, eye[3];
	fr(pw, eye);
	float anc[3]; bool anchored = pkKind == 6 && dropGround;
	if (anchored) fr(dropL, anc);
	rider::parkour(F, pkKind, u, pkT, eye, pkEyeH, pkW, pkH, pkStandY, pkThird, pkGripOk && (pkKind <= 3) ? pkGrip : 0, anchored ? anc : 0);
	if (pkKind == 6)
	{
		float d[3] = {0, 0, 0};
		if (!pkThird && !thirdPerson(pl)) { float m[3] = {F.eye[0] - eye[0], F.eye[1] - eye[1], F.eye[2] - eye[2]}; d[0] = pkX[0] * m[0] + pkFw[0] * m[1]; d[1] = pkX[1] * m[0] + pkFw[1] * m[1]; d[2] = m[2]; }
		memcpy(handling::g_camShift, d, 12);
	}
	const float *E = F.eye; float W = pkW, H = pkH;
	rider::poseFree(pk, F);
	if (pkBlendingIn)
	{
		float k = ease(pkT / rt::max(TUNE(moveBlendInSec), 0.02f)), o[9];
		for (int i = 0; i < NB; i++)
			if (bD[i] && bL[i]) { rotBlend((const float *)(bL[i] + 0x34), (const float *)(bD[i] + 0x34), k, o); memcpy((void *)(bD[i] + 0x34), o, 36); }
		float R[9], T[3], S;
		liveRootInHolder(pl, R, T, &S);
		float *rl = (float *)(pk.root + 0x34);
		rotBlend(R, rl, k, o); memcpy(rl, o, 36);
		for (int i = 0; i < 3; i++) rl[9 + i] = T[i] + (rl[9 + i] - T[i]) * k;
		rl[12] = S + (rl[12] - S) * k;
		if (k >= 1) pkBlendingIn = false;
	}
	updateNode(pkHolder);
	slingPlace();
	{
		float tg[3], w;
		rider::parkourLook(F, pkKind, u, tg, &w);
		float dx = tg[0] - E[0], dy = tg[1] - E[1], dz = tg[2] - E[2];
		pkLookDown = rt::clamp(rt::atan2(-dz, rt::max(rt::sqrt(dx * dx + dy * dy), 1.0f)), -0.6f, 1.35f);
		pkLookW = w * TUNE(moveLookAt);
	}
	static int logged;
	if (logged < 3 && u > 0.5f)
	{
		logged++;
		float hp[3]; rider::bonePos(pk, rider::L_HAND, hp);
		LOG("move: full body at %.0f%%: eye %.0f %.0f %.0f (frame), left hand at %.0f %.0f %.0f (grip %.0f %.0f %.0f)", u * 100, E[0], E[1], E[2], hp[0], hp[1], hp[2], -11.0f, W + 0.5f, H + 5);
	}
}

static float pathGap, pathW, pathH, pathStand, pathEyeH = 120;
static bool noclipOn;
static void noclip(bool on)
{
	if (on == noclipOn || g_tcl) return;
	noclipOn = on;
	console("tcl");
}
static void controls(bool off)
{
	if (off == ctrlOff) return;
	ctrlOff = off;
	console(off ? "DisablePlayerControls 1 0 0 0 0 0 0" : "EnablePlayerControls");
}
static void finish(TESObjectREFR *pl, const char *why, bool keepVel = false)
{
	if (!state) return;
	u32 cc = ctrl(pl);
	if (state == 1) setPos(pl, C);
	noclip(false);
	float z[3] = {0, 0, 0};
	setVel(cc, keepVel ? vel : z);
	noFall(cc, pos(pl)[2]);
	controls(false);
	LOG("move: %s done (%s) at %.0f %.0f %.0f - its start took %.1f ms, the worst frame during it %.0f ms", state == 1 ? (climbKind == 0 ? "vault" : climbKind == 1 ? "mantle" : "climb") : "kick flight", why, pos(pl)[0], pos(pl)[1], pos(pl)[2], moveStartMs, moveWorstDt * 1000);
	moveWorstDt = 0;
	state = 0; afterLog = 0.15f; grip = false; dropOn = false; dropGround = false;
	dropStopEnd(); dropLand = -1; handling::g_camRoll1 = 0;
	handling::g_camShift[0] = handling::g_camShift[1] = handling::g_camShift[2] = 0;
	handling::extP = handling::extR = 0;
	pkEnd(why, true);
}
static void unview(TESObjectREFR *pl)
{
	if (lastCamOff != 0 && pl) rot(pl)[0] -= lastCamOff;
	lastCamOff = 0;
}
static void view(TESObjectREFR *pl)
{
	if (camOff != 0) { rot(pl)[0] += camOff; lastCamOff = camOff; }
}
static void reset() { dropOn = dropHit = dropGround = false; dropJolt = -1; dropStopEnd(); dropLand = -1; handling::g_camRoll1 = 0; handling::g_camShift[0] = handling::g_camShift[1] = handling::g_camShift[2] = 0; if (noclipOn) noclip(false); pkOn = false; pkBlendingIn = false; pkPark(); pkKids[0].n = pkKids[1].n = 0; pkKidRoot[0] = pkKidRoot[1] = 0; state = 0; aRoot = 0; memset(sl, 0, sizeof sl); aWritten = false; forced = false; nShown = 0; reachW = 0; camOff = lastCamOff = 0; kicks = upKicks = 0; hintHave = false; if (ctrlOff) controls(false); }
static void menuPrepare(TESObjectREFR *pl)
{
	if (pkHolder || !pl || g_driving || rt::now() - g_loadedAt > 600) return;
	pkPrepare(pl, "in a menu");
}
static void menu(TESObjectREFR *pl)
{
	if (state) finish(pl, "a menu opened");
	pkEnd("a menu opened");
	unview(pl); camOff = 0; armIdentity(); reachW = 0;
	if (forced) { u32 r = firstPersonNode(pl); if (r) showArms(r, false); }
}

static void startClimb(TESObjectREFR *pl, const Ledge &L, const float *f, bool air, float rad)
{
	double st0 = rt::now();
	const float *p = pos(pl);
	if (state == 2) state = 0;
	climbKind = air ? 2 : L.h <= TUNE(moveVaultMax) ? 0 : 1;
	dur = climbKind == 0 ? TUNE(moveVaultSec) : climbKind == 1 ? TUNE(moveMantleSec) : TUNE(moveClimbSec);
	dur *= 0.8f + 0.2f * clampf(L.h / rt::max(TUNE(moveMantleMax), 1.0f), 0, 1.5f);
	dur *= sprint::climbScale();
	float gap = rt::max(0.0f, L.wall - rad - 2);
	A[0] = p[0]; A[1] = p[1]; A[2] = p[2];
	at(p, f, gap, L.z + 4, B);
	C[0] = L.stand[0]; C[1] = L.stand[1]; C[2] = L.stand[2];
	float lf[3] = {-f[1], f[0], 0};
	float hl = TUNE(moveHandLeftUnits);
	handT[0] = L.edge[0] + lf[0] * hl; handT[1] = L.edge[1] + lf[1] * hl; handT[2] = L.edge[2] + 1;
	el = 0; state = 1; grip = true;
	controls(true);
	pathGap = gap; pathW = L.wall; pathH = L.h; pathStand = rt::sqrt((C[0] - A[0]) * (C[0] - A[0]) + (C[1] - A[1]) * (C[1] - A[1]));
	pkFrame(pl, f);
	{
		u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
		pathEyeH = camN && !thirdPerson(pl) ? rd<float>(camN + 0x8C + 8) - p[2] : 120;
		if (!(pathEyeH > 60 && pathEyeH < 170)) pathEyeH = 120;
	}
	noclip(true);
	pkGripOk = true;
	{
		float xr[3] = {f[1], -f[0], 0};
		for (int k = 0; k < 2; k++)
		{
			float sx = k ? 10.0f : -10.0f, a[3], b[3], frc; bool ac;
			float base[3] = {p[0] + xr[0] * sx, p[1] + xr[1] * sx, p[2]};
			float reachY = L.wall + 40, d = L.wall;
			at(base, f, 0, L.z - 4, a); at(base, f, reachY, L.z - 4, b);
			if (cast(a, b, &frc, &ac) && !ac) d = frc * reachY;
			float top = L.z;
			at(base, f, d + 3, L.z + 25, a); at(base, f, d + 3, L.z - 25, b);
			if (cast(a, b, &frc, &ac) && !ac) top = a[2] + (b[2] - a[2]) * frc; else pkGripOk = false;
			pkGrip[k][0] = sx; pkGrip[k][1] = d; pkGrip[k][2] = top - p[2];
		}
		LOG("move: grips - left %.0f ahead at %.0f, right %.0f ahead at %.0f (the body's wall %.0f, top %.0f)%s", pkGrip[0][1], pkGrip[0][2], pkGrip[1][1], pkGrip[1][2], L.wall, L.h, pkGripOk ? "" : " - an edge not found, the plain one used");
	}
	pkBegin(pl, climbKind == 0 ? 1 : climbKind == 1 ? 2 : 3, f, L.wall, L.h, pathStand);
	u32 cc = ctrl(pl);
	moveStartMs = (float)((rt::now() - st0) * 1000); moveWorstDt = 0;
	LOG("move: %s onto a ledge %.0f above your feet (wall %.0f away, body radius %.0f, state %d): %.2f s, from %.0f %.0f %.0f to %.0f %.0f %.0f",
		climbKind == 0 ? "vault" : climbKind == 1 ? "mantle" : "climb", L.h, L.wall, rad, hkState(cc), dur, A[0], A[1], A[2], C[0], C[1], C[2]);
}
static void startKick(TESObjectREFR *pl, const float *f, const float *wallDir, float wallDist, bool upWall)
{
	double st0 = rt::now();
	const float *p = pos(pl);
	float g = rt::max(TUNE(moveGravity), 50.0f);
	if (upWall)
	{
		float vz = rt::sqrt(2 * g * rt::max(TUNE(moveKickUpUnits), 1.0f));
		vel[0] = wallDir[0] * 40; vel[1] = wallDir[1] * 40; vel[2] = vz;
		upKicks++;
	}
	else
	{
		float d[3] = {-wallDir[0] + 0.6f * f[0], -wallDir[1] + 0.6f * f[1], 0};
		norm(d);
		float vz = rt::sqrt(2 * g * rt::max(TUNE(moveKickAwayUp), 1.0f));
		vel[0] = d[0] * TUNE(moveKickSpeed); vel[1] = d[1] * TUNE(moveKickSpeed); vel[2] = vz;
	}
	kicks++;
	flyT = 0; state = 2; grip = false;
	controls(true);
	pkBegin(pl, upWall ? 4 : 5, f, wallDist + radius(ctrl(pl)), 0, 0);
	moveStartMs = (float)((rt::now() - st0) * 1000); moveWorstDt = 0;
	LOG("move: kick %s (kick %d, %d up) - wall %.0f away, at %.0f %.0f %.0f, velocity %.0f %.0f %.0f", upWall ? "up the wall" : "off the wall", kicks, upKicks, wallDist, p[0], p[1], p[2], vel[0], vel[1], vel[2]);
}
static float s5(float x) { x = clampf(x, 0, 1); return x * x * x * (x * (x * 6 - 15) + 10); }
static void startDropkick(TESObjectREFR *pl, const float *f, const float *v0)
{
	double st0 = rt::now();
	float hs = rt::sqrt(v0[0] * v0[0] + v0[1] * v0[1]), sp = hs + TUNE(dropkickBoost);
	vel[0] = f[0] * sp; vel[1] = f[1] * sp; vel[2] = rt::max(v0[2], 0.0f) + TUNE(dropkickLift);
	flyT = 0; state = 2; grip = false;
	dropOn = true; dropHit = false; dropUsed = true; dropJolt = -1; dropGround = false; dropG = 0;
	controls(true);
	if (!pkBegin(pl, 6, f, TUNE(dropkickGetUpSec), TUNE(dropkickTiltDeg) * rt::PI / 180, 0)) LOG("move: the dropkick's body couldn't be shown (flying without it)");
	moveStartMs = (float)((rt::now() - st0) * 1000); moveWorstDt = 0;
	dropLand = -1;
	{
		static u32 snd; static bool looked;
		if (!looked)
		{
			looked = true;
			u32 map = ptr(ADDR_FORM_MAP), buckets = map ? rd<u32>(map + 4) : 0, table = map ? ptr(map + 8) : 0;
			u32 any = 0;
			for (u32 bk = 0; table && bk < buckets && buckets < (1u << 24) && !snd; bk++)
				for (u32 e = ptr(table + 4 * bk); e && !snd; e = ptr(e))
				{
					u32 f = ptr(e + 8), id = rd<u32>(e + 4);
					if (!f || (id >> 24) != 0 || formtype((void *)f) != 0x28 || rd<u8>(f + 0xF4) != 0) continue;
					u32 sn = ptr(f + 0x21C);
					if (!sn || formtype((void *)sn) != 0x0D) continue;
					const char *nm = (const char *)ptr(f + 0x34);
					if (nm && readable((u32)nm, 7) && !memcmp(nm, "Unarmed", 7)) snd = sn; else if (!any) any = sn;
				}
			if (!snd) snd = any;
			LOG("move: dropkick whoosh %s", snd ? "- the game's unarmed attack sound" : "- none found");
		}
		if (snd && s_sound) g_script->CallFunctionAlt(s_sound, player(), 2, pl, (TESForm *)snd);
	}
	const float *p = pos(pl);
	LOG("move: dropkick - from %.0f units a second, at %.0f %.0f %.0f, velocity %.0f %.0f %.0f", hs, p[0], p[1], p[2], vel[0], vel[1], vel[2]);
}
static void dropHitTick(TESObjectREFR *pl, float rad)
{
	if (!dropOn || dropHit || dropGround || flyT < 0.14f) return;
	const float *p = pos(pl);
	float hv = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
	float yaw = rot(pl)[2], fw[3] = {rt::sin(yaw), rt::cos(yaw), 0};
	if (hv > 50) { fw[0] = vel[0] / hv; fw[1] = vel[1] / hv; }
	float rgt[3] = {fw[1], -fw[0], 0}, reach = rad + TUNE(dropkickReach);
	u32 best = 0; float bestF = 1e9f;
	forEachActor([&](TESObjectREFR *a) {
		u8 ft = formtype(a);
		if (a == pl || (ft != 0x3B && ft != 0x3C) || rd<u32>((u32)a + OFF_ACTOR_LIFESTATE) != 0) return;
		const float *ap = pos(a);
		float d[3] = {ap[0] - p[0], ap[1] - p[1], ap[2] - p[2]};
		float fo = d[0] * fw[0] + d[1] * fw[1], la = rt::absf(d[0] * rgt[0] + d[1] * rgt[1]);
		if (fo < 0 || fo > reach + 30 || la > 45 || d[2] < -120 || d[2] > 90) return;
		if (fo < bestF) { bestF = fo; best = refid(a); }
	});
	if (!best) return;
	TESObjectREFR *a = (TESObjectREFR *)lookup(best);
	if (!a || !s_dropHit) return;
	NVSEElement res; memset(&res, 0, sizeof res);
	g_script->CallFunction(s_dropHit, player(), 0, &res, 3, a, F(TUNE(dropkickPush)), F(TUNE(dropkickDamagePct)));
	dropHit = true;
	if (res.type == 1 && res.num != 0)
	{
		float k = -TUNE(dropkickBounce);
		vel[0] *= k; vel[1] *= k; vel[2] = rt::max(vel[2], 0.0f) + 100;
		dropJolt = 0;
		if (TUNE(dropkickHitstopSec) > 0.005f) { char c[32]; rt::fmt(c, sizeof c, "sgtm %.2f", rt::clamp(TUNE(dropkickHitstopScale), 0.02f, 1.0f)); console(c); dropSlowed = true; dropSlowUntil = rt::now() + TUNE(dropkickHitstopSec); }
		LOG("move: dropkick - %08x kicked flying (%.0f units ahead, at %.2f s)", best, bestF, flyT);
	}
	else LOG("move: dropkick - %08x is your companion: not kicked", best);
}
static void dropGroundTick(TESObjectREFR *pl, u32 cc, float dt, float rad)
{
	flyT += dt; dropG += dt;
	float G = rt::max(TUNE(dropkickGetUpSec), 0.4f);
	float hv = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
	if (hv > 1)
	{
		float k = rt::max(0.0f, 1 - 5 * dt);
		vel[0] *= k; vel[1] *= k;
		float st[3] = {vel[0] * dt, vel[1] * dt, 0}, a[3] = {dropL[0], dropL[1], dropL[2] + 30}, b[3] = {dropL[0] + st[0] + vel[0] / hv * rad, dropL[1] + st[1] + vel[1] / hv * rad, dropL[2] + 30};
		if (clear(a, b)) { dropL[0] += st[0]; dropL[1] += st[1]; } else vel[0] = vel[1] = 0;
		float g0[3] = {dropL[0], dropL[1], dropL[2] + 25}, g1[3] = {dropL[0], dropL[1], dropL[2] - 40}, fr; bool ac;
		if (cast(g0, g1, &fr, &ac)) dropL[2] = g0[2] + (g1[2] - g0[2]) * fr + 1;
	}
	float s3 = s5((dropG - 0.7f * G) / (0.3f * G)), gl = rt::min(dropGlide, rider::dropkickStandY()) * s3;
	float P[3] = {dropL[0] + pkFw[0] * gl, dropL[1] + pkFw[1] * gl, dropL[2]};
	setPos(pl, P);
	float z0[3] = {0, 0, 0}; setVel(cc, z0); noFall(cc, P[2]);
	if (dropG >= G) finish(pl, "got up");
}
static void fly(TESObjectREFR *pl, u32 cc, float dt, bool focus, float yaw, float rad)
{
	const float *p = pos(pl);
	float g = rt::max(TUNE(moveGravity), 50.0f);
	flyT += dt;
	if (focus)
	{
		float fw[3] = {rt::sin(yaw), rt::cos(yaw), 0}, rt3[3] = {fw[1], -fw[0], 0}, wish[2] = {0, 0};
		if (held(0x57)) { wish[0] += fw[0]; wish[1] += fw[1]; }
		if (held(0x53)) { wish[0] -= fw[0]; wish[1] -= fw[1]; }
		if (held(0x44)) { wish[0] += rt3[0]; wish[1] += rt3[1]; }
		if (held(0x41)) { wish[0] -= rt3[0]; wish[1] -= rt3[1]; }
		float wl = rt::sqrt(wish[0] * wish[0] + wish[1] * wish[1]);
		if (wl > 0.01f)
		{
			float before = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
			vel[0] += wish[0] / wl * TUNE(moveAirControl) * dt; vel[1] += wish[1] / wl * TUNE(moveAirControl) * dt;
			float after = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]), cap = rt::max(before, TUNE(moveAirMaxSpeed));
			if (after > cap) { vel[0] *= cap / after; vel[1] *= cap / after; }
		}
	}
	vel[2] -= g * dt;
	float N[3] = {p[0] + vel[0] * dt, p[1] + vel[1] * dt, p[2] + vel[2] * dt};
	float hv = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
	if (hv > 1)
	{
		float hd[3] = {vel[0] / hv, vel[1] / hv, 0};
		const float hs[3] = {30, 70, 110};
		for (int i = 0; i < 3; i++)
		{
			float a[3] = {p[0], p[1], p[2] + hs[i]}, b[3] = {N[0] + hd[0] * rad, N[1] + hd[1] * rad, N[2] + hs[i]};
			if (!clear(a, b)) { vel[0] = vel[1] = 0; N[0] = p[0]; N[1] = p[1]; break; }
		}
	}
	if (vel[2] > 0)
	{
		float a[3] = {N[0], N[1], p[2] + 118}, b[3] = {N[0], N[1], N[2] + 124};
		if (!clear(a, b)) { vel[2] = 0; N[2] = p[2]; }
	}
	if (vel[2] <= 0)
	{
		float a[3] = {N[0], N[1], p[2] + 20}, b[3] = {N[0], N[1], N[2] - 3}, fr; bool ac;
		if (cast(a, b, &fr, &ac))
		{
			N[2] = a[2] + (b[2] - a[2]) * fr + 1;
			setPos(pl, N);
			vel[2] = 0;
			if (dropOn)
			{
				dropGround = true; dropG = 0; memcpy(dropL, N, 12); dropLand = 0;
				float want = rider::dropkickStandY(), a2[3] = {N[0], N[1], N[2] + 50}, b2[3] = {N[0] + pkFw[0] * (want + rad), N[1] + pkFw[1] * (want + rad), N[2] + 50}, fr2; bool ac2;
				dropGlide = cast(a2, b2, &fr2, &ac2) ? rt::max(0.0f, fr2 * (want + rad) - rad) : want;
				float z0[3] = {0, 0, 0}; setVel(cc, z0); noFall(cc, N[2]);
				LOG("move: dropkick - down on your back after %.2f s at %.0f %.0f %.0f (room to stand %.0f of %.0f ahead)", flyT, N[0], N[1], N[2], dropGlide, want);
				return;
			}
			finish(pl, "landed");
			return;
		}
	}
	setPos(pl, N);
	float z0[3] = {0, 0, 0}; setVel(cc, z0); noFall(cc, N[2]);
	if (flyT > 4) finish(pl, "too long in the air", true);
}

static void tick(float dt, double t, bool focus, TESObjectREFR *pl, bool quiet)
{
	{ u32 sg = sig3ptr(pl); if (sg != pkSeenSig) { pkSeenSig = sg; g_gearChangedAt = rt::now(); } }
	if (handling::curAction == 0 || handling::curAction == 1) g_gearChangedAt = rt::now();
	float target[3];
	bool reach = false;
	const char *why = 0;
	if (TUNE(moveOn) <= 0) why = "turned off";
	else if (g_driving) why = "driving";
	else if (quiet) why = "loading";
	else if (rd<u32>(0x11F2250 + 8)) why = "VATS";
	u32 cc = ctrl(pl);
	if (!cc) why = "no character controller";
	{
		static float lastP[3]; static double stillSince;
		const float *p = pos(pl);
		float dx = p[0] - lastP[0], dy = p[1] - lastP[1], dz = p[2] - lastP[2];
		if (dx * dx + dy * dy + dz * dz > 4) stillSince = rt::now();
		memcpy(lastP, p, 12);
		if (!g_driving && cc && pkHolder && !pkParent) pkPrepare(pl, "after a loading screen");
		else if (!g_driving && cc && !pkHolder && rt::now() - g_loadedAt < 120 && rt::now() - stillSince > 2) pkPrepare(pl, "standing still after a loading screen");
	}
	if (why)
	{
		if (state) finish(pl, why);
		pkEnd(why);
		camOff = 0; reachW = 0; armIdentity();
		if (forced) { u32 r = firstPersonNode(pl); if (r) showArms(r, false); else { forced = false; nShown = 0; } }
		return;
	}
	pkFadeTick(pl, dt);
	int hs = hkState(cc);
	if (!s_setPos) { s_setPos = g_script->CompileScript(kSetPos3); LOG("move: SetPos function %x", (u32)s_setPos); }
	float yaw = rot(pl)[2], f[3] = {rt::sin(yaw), rt::cos(yaw), 0};
	float rad = radius(cc);
	u32 proc = handling::process(pl);
	bool weaponOut = proc && rd<u8>(proc + 0x135);
	if (afterLog > 0 && (afterLog -= dt) <= 0)
	{
		float *pv = (float *)(ptr(cc + 8) + 0x10);
		LOG("move: a moment later: state %d at %.0f %.0f %.0f, velocity %.0f %.0f %.0f", hs, pos(pl)[0], pos(pl)[1], pos(pl)[2], pv[0] / kHavok, pv[1] / kHavok, pv[2] / kHavok);
	}
	bool ground = hs == 0 && state != 2;
	if (ground && !state) kicks = upKicks = 0;
	if (ground && !state) { dropUsed = false; if (sprint::modeNow() > 0) sprintSeenT = rt::now(); }
	bool rmb = focus && pressed(0x02);
	bool space = focus && pressed(VK_CLIMB);
	if (state && dt > moveWorstDt) moveWorstDt = dt;
	if (state == 1)
	{
		el += dt;
		float u = clampf(el / dur, 0, 1), P[3], ey, ez;
		rider::parkourEye(climbKind == 0 ? 1 : 2, u, pathEyeH, pathGap, pathW, pathH, pathStand, &ey, &ez);
		P[0] = pkO[0] + pkFw[0] * ey; P[1] = pkO[1] + pkFw[1] * ey; P[2] = pkO[2] + ez - pathEyeH;
		if (u >= 1) memcpy(P, C, 12);
		setPos(pl, P);
		float z0[3] = {0, 0, 0}; setVel(cc, z0); noFall(cc, P[2]);
		float s = rt::sin(rt::PI * u);
		camOff = -(TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f) * TUNE(moveCamDipDeg) * s * (climbKind == 0 ? 0.5f : 1.0f) * rt::PI / 180;
		handling::extP = -TUNE(moveGunDropDeg) * s; handling::extR = TUNE(moveGunDropDeg) * 0.4f * s;
		reach = u < 0.8f && !pkOn;
		memcpy(target, handT, 12);
		pkLookW = 0;
		pkTick(pl, u, dt);
		if (pkOn && pkLookW > 0)
		{
			float sgn = TUNE(handlingUpSign) < 0 ? 1.0f : -1.0f, now = rot(pl)[0] * sgn;
			camOff = sgn * pkLookW * (pkLookDown - now);
		}
		if (pkThird || thirdPerson(pl)) camOff = 0;
		if (u >= 1) finish(pl, "arrived");
	}
	else
	{
		camOff = 0;
		handling::extP = handling::extR = 0;
		bool air = state == 2 || (!ground && hs != 3 && hs != 4 && hs != 5);
		Ledge L; L.ok = false;
		float climbMax = air ? TUNE(moveAirReach) : TUNE(moveMantleMax);
		if (hs != 5 && hs != 4) findLedge(pl, f, rt::max(climbMax, TUNE(moveHintMax)), rad, L);
		bool climbable = L.ok && L.h <= climbMax;
		if (L.ok)
		{
			if (L.wall - rad <= TUNE(moveHintUnits))
			{
				reach = true; memcpy(target, L.edge, 12);
				float lf[3] = {-f[1], f[0], 0}; target[0] += lf[0] * TUNE(moveHandLeftUnits); target[1] += lf[1] * TUNE(moveHandLeftUnits);
			}
			if (!hintOn) { hintOn = true; if (sinceProbeLog > 15) { sinceProbeLog = 0; LOG("move: ledge %.0f above your feet, wall %.0f away%s%s - the left hand reaches", L.h, L.wall, air ? " (in the air)" : "", climbable ? "" : " (too high to climb from here)"); } }
		}
		else hintOn = false;
		sinceProbeLog += dt;
		if (climbable && (!air ? space : (space || (focus && (held(0x57) || held(VK_CLIMB))))))
			startClimb(pl, L, f, air, rad);
		else
		{
			if (air && space && kicks < (int)TUNE(moveKicksMax))
			{
				float best = 1e9f, bd[3] = {0, 0, 0};
				float reachW2 = rad + TUNE(moveWallUnits);
				const float *p = pos(pl);
				for (int k = 0; k < 8; k++)
				{
					float ang = yaw + k * rt::PI / 4, d[3] = {rt::sin(ang), rt::cos(ang), 0}, a[3], b[3], fr; bool ac;
					at(p, d, 0, p[2] + 50, a); at(p, d, reachW2, p[2] + 50, b);
					if (cast(a, b, &fr, &ac) && !ac && fr * reachW2 < best) { best = fr * reachW2; memcpy(bd, d, 12); }
				}
				if (best >= reachW2) LOG("move: Space in the air, no wall within %.0f units - no kick", reachW2 - rad);
				if (best < reachW2)
				{
					bool facing = dot(bd, f) > 0.64f;
					if (facing && upKicks >= (int)TUNE(moveUpKicksMax)) LOG("move: no more kicks up this wall (%d)", upKicks);
					else startKick(pl, f, bd, best - rad, facing);
				}
			}
			if (rmb && !state && air && TUNE(dropkickOn) > 0 && hs != 3 && hs != 4 && hs != 5)
			{
				float *pv = (float *)(ptr(cc + 8) + 0x10), v0[3] = {pv[0] / kHavok, pv[1] / kHavok, pv[2] / kHavok};
				float sp = rt::sqrt(v0[0] * v0[0] + v0[1] * v0[1]);
				const char *no = weaponOut ? "your weapon is out" : dropUsed ? "one per jump" : rt::now() - sprintSeenT > TUNE(dropkickSprintSec) ? "you weren't sprinting" : sp < TUNE(dropkickMinSpeed) ? "too slow" : 0;
				if (no) LOG("move: right-click in the air - no dropkick (%s; %.0f units a second)", no, sp);
				else startDropkick(pl, f, v0);
			}
			if (state == 2)
			{
				float mdt = dt;
				slowTick();
				if (dropSlowed) mdt = dt * rt::clamp(TUNE(dropkickHitstopScale), 0.02f, 1.0f);
				if (dropOn && dropGround) dropGroundTick(pl, cc, mdt, rad); else fly(pl, cc, mdt, focus, yaw, rad);
				if (dropOn) pkStandY = dropJolt;
				if (state == 2) { dropHitTick(pl, rad); pkTick(pl, dropOn ? (dropGround ? dropG : -1.0f) : 0, mdt); }
				if (state == 2 && dropOn && !thirdPerson(pl))
				{
					if (dropJolt >= 0) dropJolt += mdt;
					if (dropLand >= 0) dropLand += mdt;
					float sg = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f, lie = rider::dropkickLie(flyT - (dropGround ? dropG : 0), dropGround ? dropG : -1.0f, TUNE(dropkickGetUpSec));
					float j = joltAt(dropJolt), l = joltAt(dropLand * 0.8f);
					camOff = -sg * (TUNE(dropkickCamDeg) * lie + TUNE(dropkickJoltDeg) * j + TUNE(dropkickLandDeg) * l) * rt::PI / 180;
					handling::g_camRoll1 = (TUNE(dropkickRollDeg) * j - TUNE(dropkickRollDeg) * 0.6f * l) * rt::PI / 180;
				}
				else handling::g_camRoll1 = 0;
			}
		}
	}
	float rs = rt::max(TUNE(moveReachInSec), 0.02f);
	reachW = clampf(reachW + (reach ? dt : -dt) / rs, 0, 1);
	if (reach)
	{
		float st = (float)(t - 1000.0 * (int)(t / 1000.0)), sw = TUNE(moveHandSway);
		target[0] += sw * rt::sin(1.3f * st); target[1] += sw * rt::sin(1.7f * st + 1); target[2] += sw * rt::sin(1.1f * st + 2);
		if (!hintHave || state == 1) { memcpy(hintT, target, 12); hintHave = true; }
		else { float k = clampf(dt * 14, 0, 1); for (int i = 0; i < 3; i++) hintT[i] += (target[i] - hintT[i]) * k; }
	}
	else if (reachW <= 0) hintHave = false;
	armTick(pl, hintT, ease(reachW), grip && state == 1, weaponOut);
	static float sinceArm = 99; static bool lastReach;
	sinceArm += dt;
	bool changed = reach != lastReach;
	lastReach = reach;
	if (changed && sinceArm > 15)
	{
		u32 r1 = firstPersonNode(pl);
		LOG("move: arm %s - third person %d, weapon out %d, first-person root %x (valid %d, flags %x), reach %.2f, written %d, arms shown by us %d", reach ? "reaching" : "resting",
			thirdPerson(pl), weaponOut, r1, armValid(r1), r1 ? rd<u32>(r1 + OFF_NIAV_FLAGS) : 0, reachW, aWritten, forced);
		sinceArm = 0;
	}
}
}

// ---- The gun handling in third person (plugin 118, the author 2026-10-14: "get back to perfecting weapons and
// parkour,
namespace tpgun
{
using handling::Xf; using handling::xfRead;
static u32 root, node[2], clav[2], wbone, gun, gunPar;
static u32 spine, leanN, head;
static float lS, lSv, lU, lUv, tgtS, tgtU, clearT, searchT, loseT;
static float rock, rockV;
static bool leanOn;
static Xf gunOrig, gunLast;
static bool written, gunWritten;
static float sendQ[4] = {1, 0, 0, 0};
static void gunQ(float q[4]) { memcpy(q, sendQ, 16); }
static double failT = -99;
static bool ours(u32 n) { return move::unnamedNode(n) && rd<u16>(n + OFF_NODE_CHILDCOUNT) == 1 && ptr(n + 0x18); }
static bool valid(TESObjectREFR *pl)
{
	u32 r = pl ? thirdPersonNode(pl) : 0;
	if (!r || r != root) return false;
	for (int k = 0; k < 2; k++)
		if (!node[k] || ptr(clav[k] + 0x18) != node[k] || !ptr(node[k] + 0x18) || !handling::isAncestor(r, node[k])) return false;
	if (leanN && (ptr(spine + 0x18) != leanN || !handling::isAncestor(r, leanN))) return false;
	return true;
}
static bool find(TESObjectREFR *pl)
{
	u32 r = thirdPersonNode(pl);
	if (gun) { rider::release(gun); gun = gunPar = 0; }
	root = 0; node[0] = node[1] = 0; wbone = 0; gunWritten = written = false;
	if (!r) return false;
	int made = 0, kept = 0;
	const char *pn[2] = {0, 0};
	for (int k = 0; k < 2; k++)
	{
		clav[k] = findNamed(r, k ? "Bip01 R Clavicle" : "Bip01 L Clavicle", 40);
		if (!clav[k]) break;
		u32 p = ptr(clav[k] + 0x18);
		if (ours(p)) { node[k] = p; kept++; }
		else if ((node[k] = move::insertAbove(clav[k]))) made++;
		handling::flagsLike(node[k], clav[k]);
		u32 pp = node[k] ? ptr(node[k] + 0x18) : 0;
		pn[k] = pp ? (const char *)ptr(pp + 8) : 0;
	}
	if (!node[0] || !node[1])
	{
		if (rt::now() - failT > 5) { failT = rt::now(); LOG("handling: third person - no shoulder nodes (clavicles %x %x, nodes %x %x) - the body's gun stays still", clav[0], clav[1], node[0], node[1]); }
		return false;
	}
	root = r;
	spine = findNamed(r, "Bip01 Spine", 40);
	head = findNamed(r, "Bip01 Head", 60);
	leanN = 0;
	if (spine && head)
	{
		u32 p = ptr(spine + 0x18);
		leanN = ours(p) ? p : move::insertAbove(spine);
		handling::flagsLike(leanN, spine);
	}
	lS = lSv = lU = lUv = 0; rock = rockV = 0; leanOn = false;
	LOG("handling: third person - lean node %x above Bip01 Spine %x (head %x), flags %x (the spine's %x)", leanN, spine, head, leanN ? rd<u32>(leanN + OFF_NIAV_FLAGS) : 0, spine ? rd<u32>(spine + OFF_NIAV_FLAGS) : 0);
	u32 hand = findNamed(r, "Bip01 R Hand", 40);
	wbone = hand ? findNamed(hand, "Weapon", 3) : 0;
	if (!wbone) wbone = findNamed(r, "Weapon", 40);
	u32 wp = wbone ? ptr(wbone + 0x18) : 0;
	LOG("handling: third person - shoulder nodes ready (%d put in, %d ours already) under %s / %s%s; the hand's Weapon node %x (under %s)", made, kept,
		pn[0] ? pn[0] : "?", pn[1] ? pn[1] : "?", ptr(node[0] + 0x18) == ptr(node[1] + 0x18) ? "" : " - NOT THE SAME PARENT", wbone, wp && ptr(wp + 8) ? (const char *)ptr(wp + 8) : "?");
	return true;
}
static u32 gunNow()
{
	if (!wbone) return 0;
	u32 arr = ptr(wbone + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(wbone + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < cnt && i < 16; i++) { u32 c = ptr(arr + 4 * i); if (c && handling::isNodeObj(c)) return c; }
	return 0;
}
static void gunPut(const Xf &x) { memcpy((void *)(gun + 0x34), x.r, 36); memcpy((void *)(gun + 0x58), x.t, 12); }
static void gunDrop() { sendQ[0] = 1; sendQ[1] = sendQ[2] = sendQ[3] = 0; if (gun) { if (gunWritten && ptr(gun + 0x18) == gunPar && gunPar == wbone) gunPut(gunOrig); rider::release(gun); } gun = gunPar = 0; gunWritten = false; }
static void identity()
{
	if (!written) return;
	written = false;
	TESObjectREFR *pl = player();
	if (!valid(pl)) { if (gun) rider::release(gun); root = 0; gun = gunPar = 0; gunWritten = false; return; }
	for (int k = 0; k < 2; k++) move::nodeIdentity(node[k]);
	if (leanN) move::nodeIdentity(leanN);
	lS = lSv = lU = lUv = 0; rock = rockV = 0; leanOn = false;
	gunDrop();
}
static void leanTick(const float *aimT, float dt, bool aim, bool sneak, bool fromFirst = false)
{
	using namespace handling;
	if (!leanN || !head) return;
	float y = aimT[1], cp = rt::cos(aimT[0]);
	float D[3] = {rt::sin(y) * cp, rt::cos(y) * cp, -rt::sin(aimT[0])}, rt3[3] = {rt::cos(y), -rt::sin(y), 0}, fh[3] = {rt::sin(y), rt::cos(y), 0};
	const float *hw = (const float *)(head + 0x8C);
	float E[3] = {hw[0] - rt3[0] * lS, hw[1] - rt3[1] * lS, hw[2] - lU};
	bool want = TUNE(tpLeanOn) > 0 && TUNE(coverAim) > 0 && (aim || TUNE(coverAimHip) > 0) && sprint::modeNow() == 0 && move::stateNow() == 0
		&& aimT[0] < TUNE(coverMaxDownDeg) * rt::PI / 180;
	int rays = 0;
	if (fromFirst)
	{
		leanOn = rt::absf(cvS.x) + rt::absf(cvU.x) > 0.5f;
		tgtS = cvS.x; tgtU = cvU.x;
	}
	else if (want)
	{
		float probe = TUNE(coverProbeUnits), fr, B[3] = {E[0] + D[0] * probe, E[1] + D[1] * probe, E[2] + D[2] * probe};
		rays++;
		bool blocked = cvBlocked(E, B, &fr);
		float len = fr * probe + TUNE(coverPastUnits);
		if (!blocked) { if (leanOn && (clearT += dt) > TUNE(coverReleaseSec)) { leanOn = false; LOG("handling: third person - clear, lean back"); } }
		else
		{
			clearT = 0;
			if (leanOn && cvSpot(E, rt3, D, tgtS, tgtU, len, rays)) loseT = 0;
			else if ((searchT -= dt) <= 0)
			{
				searchT = TUNE(coverSearchSec);
				float sS = 0, sU = 0;
				if (cvSearch(E, rt3, D, len, sneak, &sS, &sU, rays))
				{
					if (!leanOn || sS != tgtS || sU != tgtU) LOG("handling: third person - lean to side %.0f up %.0f", sS, sU);
					leanOn = true; tgtS = sS; tgtU = sU; loseT = 0;
				}
				else { searchT = TUNE(coverRetrySec); if (leanOn && (loseT += TUNE(coverRetrySec)) > TUNE(coverLoseSec)) { leanOn = false; LOG("handling: third person - no spot, lean back"); } }
			}
		}
	}
	else leanOn = false;
	float w = 2 * rt::PI * rt::max(TUNE(tpLeanHz), 0.1f);
	float ts = leanOn ? tgtS : 0, tu = leanOn ? clampf(tgtU, -TUNE(tpLeanDownMax), TUNE(tpLeanUpMax)) : 0;
	float h = rt::min(dt, 0.05f);
	lSv += (w * w * (ts - lS) - 2 * w * lSv) * h; lS += lSv * h;
	lUv += (w * w * (tu - lU) - 2 * w * lUv) * h; lU += lUv * h;
	Xf W = xfRead(ptr(leanN + 0x18), 0x68);
	const float *sw = (const float *)(spine + 0x8C);
	float hx = hw[0] - sw[0], hy = hw[1] - sw[1], hz = hw[2] - sw[2], L = rt::max(rt::sqrt(hx * hx + hy * hy + hz * hz), 20.0f);
	float ang = clampf(rt::atan2(lS, L) * 180 / rt::PI * (TUNE(tpLeanSign) < 0 ? -1.0f : 1.0f), -TUNE(tpLeanMaxDeg), TUNE(tpLeanMaxDeg));
	float Dm[9], WT[9], X[9], R[9], P[3], RP[3], up[3] = {0, 0, lU}, ul[3], t[3];
	float k = TUNE(tpSpineInertia), A[9], Bm[9], AB[9], Rl[9], upv[3] = {0, 0, 1};
	rotAbout(upv, -gh.x * k, A); rotAbout(rt3, gp.x * k, Bm); mul3(A, Bm, AB);
	float rockTo = aim ? strafeA * TUNE(tpStrafeRockAds) : strafeS;
	spring(rock, rockV, rockTo * TUNE(tpStrafeRockDeg), TUNE(tpStrafeRockHz), TUNE(tpStrafeRockDamping), dt);
	rock = clampf(rock, -2 * TUNE(tpStrafeRockDeg) - 1, 2 * TUNE(tpStrafeRockDeg) + 1);
	rotAbout(fh, ang + rock * (TUNE(tpLeanSign) < 0 ? -1.0f : 1.0f), Rl); mul3(AB, Rl, Dm);
	move::tr(W.r, WT); mul3(WT, Dm, X); mul3(X, W.r, R);
	for (int i = 0; i < 3; i++) P[i] = rd<float>(spine + 0x58 + 4 * i);
	move::apply(R, P, RP); move::apply(WT, up, ul);
	for (int i = 0; i < 3; i++) t[i] = P[i] - RP[i] + ul[i] / W.s;
	memcpy((void *)(leanN + 0x34), R, 36); memcpy((void *)(leanN + 0x58), t, 12);
	(void)rays;
}
static int shoulderMine = 1;
static float shoulderNow = 1;
static void shoulderTick(float dt)
{
	bool x = pressed(VK_SHOULDER);
	if (x && gameFocused() && !menumode()) { shoulderMine = -shoulderMine; LOG("handling: third person - camera over the %s shoulder", shoulderMine > 0 ? "right" : "left"); }
	const float kMaxSide = 60;
	u32 sg = ptr(0x11DEB7C), camN = sg ? ptr(sg + 0xAC) : 0;
	static float gameSide = 0; static bool measured; static double lastT, steadyT; static int refused;
	double now = rt::now();
	if (now - lastT > 0.25) steadyT = now;
	lastT = now;
	if (camN && head && shoulderNow >= 0.999f && rt::absf(lS) < 0.5f && rt::absf(lU) < 0.5f && now - steadyT > 1.0)
	{
		const float *W = (const float *)(camN + 0x68), *T = (const float *)(camN + 0x8C), *h = (const float *)(head + 0x8C);
		float side = (T[0] - h[0]) * W[2] + (T[1] - h[1]) * W[5] + (T[2] - h[2]) * W[8];
		if (rt::absf(side) > kMaxSide) { if (refused++ < 3) LOG("handling: third person - the camera %.1f units to the side of your head: not the game's shoulder, left out", side); }
		else if (!measured) { gameSide = side; measured = true; LOG("handling: third person - the game's camera is %.1f units to the %s of your head", rt::absf(side), side >= 0 ? "right" : "left"); }
		else gameSide += (side - gameSide) * rt::min(dt * 0.5f, 1.0f);
	}
	float step = dt * 2 / rt::max(TUNE(tpShoulderSec), 0.05f);
	shoulderNow += handling::clampf((float)shoulderMine - shoulderNow, -step, step);
	float e = 0.5f - shoulderNow * 0.5f; e = e * e * (3 - 2 * e);
	g_tpCamSide = -(2 * gameSide + (gameSide >= 0 ? 1 : -1) * TUNE(tpShoulderLeftExtraUnits)) * e;
}
static bool muzzle(float *pos, const float **rot)
{
	TESObjectREFR *pl = player();
	if (!valid(pl)) return false;
	u32 g = gunNow();
	if (!g) return false;
	u32 m = findNamed(g, "ProjectileNode", 8);
	if (!m) m = g;
	memcpy(pos, (const void *)(m + 0x8C), 12);
	*rot = (const float *)(m + 0x68);
	return true;
}
static u32 axisFor; static int axis = -1;
static bool barrel(float *pos, float *dir, const float *A, bool tell)
{
	const float *mr = 0;
	if (!muzzle(pos, &mr)) return false;
	u32 g = gunNow();
	if (axisFor != g)
	{
		axisFor = g; axis = -1;
		float best = rt::cos(25 * rt::PI / 180);
		for (int k = 0; k < 6; k++)
		{
			float sg = (k & 1) ? -1.0f : 1.0f, c = sg * (mr[k / 2] * A[0] + mr[3 + k / 2] * A[1] + mr[6 + k / 2] * A[2]);
			if (c > best) { best = c; axis = k; }
		}
		if (tell || axis >= 0) LOG("shot: third person - the barrel is the muzzle's %s%c axis%s", axis < 0 ? "" : (axis & 1) ? "-" : "+", axis < 0 ? '?' : "xyz"[axis / 2], axis < 0 ? " - none near the aim, the game's aim kept" : "");
		else axisFor = 0;
	}
	if (axis < 0) return false;
	float sg = (axis & 1) ? -1.0f : 1.0f;
	dir[0] = sg * mr[axis / 2]; dir[1] = sg * mr[3 + axis / 2]; dir[2] = sg * mr[6 + axis / 2];
	return true;
}
static void blockTick(bool aim)
{
	using namespace handling;
	if (TUNE(blockMarkOn) <= 0) return;
	hudColorTick();
	u32 camN = sceneCam();
	if (!camN || !aim) return;
	const float *cw = (const float *)(camN + 0x68), *c0 = (const float *)(camN + 0x8C);
	float fw[3] = {cw[0], cw[3], cw[6]}, M[3], d[3], fr, sd = g_camOn ? g_camSide : 0;
	float cp[3] = {c0[0] + cw[2] * sd, c0[1] + cw[5] * sd, c0[2] + cw[8] * sd};
	if (!barrel(M, d, fw, false)) return;
	float far = TUNE(blockMaxUnits), C[3] = {cp[0] + fw[0] * far, cp[1] + fw[1] * far, cp[2] + fw[2] * far};
	u32 o = 0; float fc = 1;
	if (rayHitObject(cp, C, &fc, &o)) for (int i = 0; i < 3; i++) C[i] = cp[i] + fw[i] * far * fc;
	float tx = C[0] - M[0], ty = C[1] - M[1], tz = C[2] - M[2], td = rt::sqrt(tx * tx + ty * ty + tz * tz);
	float len = rt::min(td, TUNE(blockNearMetres) * kUnitsPerMetre);
	if (len < 1) return;
	float B[3] = {M[0] + d[0] * len, M[1] + d[1] * len, M[2] + d[2] * len};
	if (!cvBlocked(M, B, &fr) || fr * len > td - TUNE(blockMarginUnits)) return;
	g_blkAt = rt::now();
	static int told;
	if (told < 6) { told++; LOG("handling: third person - the barrel's line is blocked %.0f units out, the crosshair's %.0f: X shown", fr * len, td); }
}
static void apply(TESObjectREFR *pl, const float *aimT, float dt, bool aim, bool sneak, bool forFriends)
{
	if (!valid(pl) && !find(pl)) return;
	leanTick(aimT, dt, aim, sneak, forFriends);
	if (!forFriends) { shoulderTick(dt); blockTick(aim); }
	using namespace handling;
	float upSign = TUNE(handlingUpSign) < 0 ? -1.0f : 1.0f;
	float e = upSign < 0 ? -aimT[0] : aimT[0], y = aimT[1];
	float f[3] = {rt::sin(y) * rt::cos(e), rt::cos(y) * rt::cos(e), rt::sin(e)}, r[3] = {rt::cos(y), -rt::sin(y), 0}, u[3];
	move::cross(r, f, u);
	float sg = TUNE(tpShoulderScale), A[9], B[9], C[9], AB[9], D[9];
	rotAbout(u, -gh.x * sg, A); rotAbout(r, gp.x * sg, B); rotAbout(f, gr.x * sg, C);
	mul3(A, B, AB); mul3(AB, C, D);
	float back = gb.x * TUNE(tpBackScale);
	bool same = ptr(node[0] + 0x18) == ptr(node[1] + 0x18);
	float Pm[3];
	for (int i = 0; i < 3; i++) Pm[i] = (rd<float>(clav[0] + 0x58 + 4 * i) + rd<float>(clav[1] + 0x58 + 4 * i)) * 0.5f;
	for (int k = 0; k < 2; k++)
	{
		u32 par = ptr(node[k] + 0x18);
		Xf W = xfRead(par, 0x68);
		float P[3]; if (same) memcpy(P, Pm, 12); else for (int i = 0; i < 3; i++) P[i] = rd<float>(clav[k] + 0x58 + 4 * i);
		float WT[9], X[9], R[9], RP[3], bw[3] = {-f[0] * back, -f[1] * back, -f[2] * back}, bl[3], t[3];
		move::tr(W.r, WT); mul3(WT, D, X); mul3(X, W.r, R);
		move::apply(R, P, RP); move::apply(WT, bw, bl);
		for (int i = 0; i < 3; i++) t[i] = P[i] - RP[i] + bl[i] / W.s;
		memcpy((void *)(node[k] + 0x34), R, 36); memcpy((void *)(node[k] + 0x58), t, 12);
	}
	u32 g = gunNow();
	if (g != gun)
	{
		gunDrop();
		gun = g; gunPar = g ? ptr(g + 0x18) : 0; gunWritten = false;
		if (g) { rider::hold(g); gunOrig = xfRead(g, 0x34); static u32 told; if (told != g) { told = g; const char *gn = (const char *)ptr(g + 8); LOG("handling: third person - the gun %s turns about the grip", gn ? gn : "?"); } }
	}
	if (gun)
	{
		Xf now = xfRead(gun, 0x34);
		if (gunWritten && (memcmp(now.r, gunLast.r, 36) || memcmp(now.t, gunLast.t, 12))) gunOrig = now;
		float kw = TUNE(tpGunWobble), G[9], Wb[9], Rg[9];
		rotAbout(u, -(wbH.x * kw), A);
		rotAbout(r, flip.x * TUNE(tpGunFlip) + wbP.x * kw + gp.x * TUNE(tpGunPitch), B);
		rotAbout(f, wbR.x * kw, C);
		mul3(A, B, AB); mul3(AB, C, G);
		Xf W = xfRead(gunPar, 0x68);
		float WT[9]; move::tr(W.r, WT); mul3(WT, G, Wb); mul3(Wb, W.r, Rg);
		Xf L = gunOrig; mul3(Rg, gunOrig.r, L.r);
		gunPut(L); gunLast = L; gunWritten = true;
		move::q4(Rg, sendQ);
	}
	written = true;
	static double lastLog;
	if (rt::now() - lastLog > 6 && (rt::absf(gh.x) > 1 || rt::absf(gp.x) > 1))
	{
		lastLog = rt::now();
		LOG("handling: third person - shoulders heading %.1f pitch %.1f roll %.1f deg, back %.1f; gun flip %.1f wobble %.1f", -gh.x * sg, gp.x * sg, gr.x * sg, back, flip.x * TUNE(tpGunFlip), wbP.x * TUNE(tpGunWobble));
	}
}
}

// ---- Sprint (hold Shift to
namespace sprint
{
static int mode;
static int modeNow() { return mode; }
static float blend[2], tacLeft = -1, tacWait, phase, fovNow;
static double lastPress = -9;
static bool overriding, wasDown;
static float baseWalk, baseRun;
static bool ours, haveBase, accMoving; static int accLogs; static float wroteW, wroteR, accX, accV, accT = -1, accAng;
static float wrapAng(float a) { while (a > rt::PI) a -= 2 * rt::PI; while (a < -rt::PI) a += 2 * rt::PI; return a; }
static float animWrote[2], animBase[2];
static int arSaved = -1;
static int animRecomputed[2], animKept[2];
static u32 animData(TESObjectREFR *pl, int i) { u32 p = handling::process(pl); return i == 0 ? (p ? ptr(p + 0x1C0) : 0) : ptr((u32)pl + 0x690); }
static void cadence(TESObjectREFR *pl, float speedFactor)
{
	for (int i = 0; i < 2; i++)
	{
		u32 ad = animData(pl, i);
		if (!ad) continue;
		float &m = *(float *)(ad + 0x10C);
		if (animWrote[i] > 0 && rt::absf(m - animWrote[i]) < 1e-5f) animKept[i]++;
		else { animBase[i] = m; animRecomputed[i]++; }
		if (!(animBase[i] > 0.01f && animBase[i] < 20)) continue;
		float w = animBase[i] * rt::clamp(TUNE(sprintCadence) / rt::max(speedFactor, 0.1f), 0.3f, 1.0f);
		m = w; animWrote[i] = w;
	}
}
static void cadenceOff(TESObjectREFR *pl)
{
	for (int i = 0; i < 2; i++)
	{
		u32 ad = pl ? animData(pl, i) : 0;
		if (ad && animWrote[i] > 0 && rt::absf(*(float *)(ad + 0x10C) - animWrote[i]) < 1e-5f) *(float *)(ad + 0x10C) = animBase[i];
		animWrote[i] = 0;
	}
}
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static u32 cached(TESObjectREFR *pl) { u32 p = handling::process(pl); return p ? ptr(p + 0x2C) : 0; }
static void speedOff(TESObjectREFR *pl)
{
	if (overriding)
	{
		overriding = false;
		cadenceOff(pl);
		if (arSaved >= 0 && pl) { *(u8 *)((u32)pl + 0x651) = (u8)arSaved; arSaved = -1; }
	}
	if (u32 cv = pl ? cached(pl) : 0) *(u32 *)(cv + 0x44) &= ~0x3000u;
	ours = false;
}
static void reset() { mode = 0; overriding = false; ours = haveBase = accMoving = false; accX = accV = 0; accT = -1; animWrote[0] = animWrote[1] = 0; blend[0] = blend[1] = 0; tacLeft = -1; fovNow = 0; }
static void stop(TESObjectREFR *pl, const char *why)
{
	if (mode) LOG("sprint: off (%s)", why);
	mode = 0;
	speedOff(pl);
}
static void tick(float dt, double t, bool focus, TESObjectREFR *pl)
{
	if (tacLeft < 0) tacLeft = TUNE(tacSprintSec);
	const char *why = 0;
	u32 proc = handling::process(pl);
	if (TUNE(sprintOn) <= 0) why = "turned off";
	else if (g_driving) why = "driving";
	else if (!proc) why = "no process";
	else if (rd<u32>(0x11F2250 + 8)) why = "VATS";
	bool down = focus && held(VK_SPRINT), fwd = focus && held(0x57);
	bool edge = down && !wasDown;
	wasDown = down;
	if (!why)
	{
		bool aim = rd<u8>(proc + 0x228) != 0, sneak = (handling::moveFlags(pl) & 0x400) != 0;
		int action = (int)((short(__thiscall *)(u32))ptr(ptr(proc) + 0x3E4))(proc);
		if (aim) why = "aiming";
		else if (action == 2) why = "firing";
		else if (sneak) why = "crouched";
		else if (!fwd) why = "not going forward";
		else if (!down) why = "Shift let go";
	}
	if (edge)
	{
		if (t - lastPress < TUNE(sprintDoubleTapSec) && tacLeft > 0.3f && !why) { if (mode != 2) LOG("sprint: tac sprint (%.1f s left)", tacLeft); mode = 2; }
		lastPress = t;
	}
	if (why) { if (mode) stop(pl, why); }
	else if (!mode) { mode = 1; LOG("sprint: on"); }
	if (mode == 2)
	{
		tacLeft -= dt;
		if (tacLeft <= 0) { tacLeft = 0; mode = 1; tacWait = 0; LOG("sprint: tac sprint ran out - sprinting"); }
	}
	else if (tacLeft < TUNE(tacSprintSec))
	{
		tacWait += dt;
		if (tacWait > TUNE(tacSprintRechargeSec)) { tacLeft = rt::min(TUNE(tacSprintSec), tacLeft + dt * TUNE(tacSprintSec) / 1.5f); }
	}
	if (mode != 2 && tacLeft >= TUNE(tacSprintSec)) tacWait = 0;
	u32 cv = proc ? ptr(proc + 0x2C) : 0;
	bool sprinting = cv && mode && !move::state;
	bool accOn = TUNE(moveAccelOn) > 0 && cv && !move::state && !g_driving && !rd<u32>(0x11F2250 + 8);
	if (cv && (sprinting || accOn))
	{
		u32 &fl = *(u32 *)(cv + 0x44);
		float cw = rd<float>(cv + 0x38), cr = rd<float>(cv + 0x3C);
		bool fresh = (fl & 0x3000) == 0x3000;
		if (!fresh) { ours = false; fl &= ~0x3000u; }
		else if (!ours || rt::absf(cw - wroteW) > 0.01f || rt::absf(cr - wroteR) > 0.01f)
		{
			if (!(cw > 1 && cw < 5000 && cr > 1 && cr < 5000)) fresh = false;
			else { baseWalk = cw; baseRun = cr; haveBase = true; ours = false; }
		}
		if (fresh && haveBase)
		{
			if (sprinting && !overriding)
			{
				overriding = true;
				u8 &ar = *(u8 *)((u32)pl + 0x651);
				if (arSaved < 0) { arSaved = ar; ar = 0; }
				LOG("sprint: the game's speeds walk %.1f run %.1f; always-run was %d (off while sprinting)", baseWalk, baseRun, arSaved);
			}
			float sp = baseRun * (mode == 2 ? TUNE(tacSprintSpeedMult) : TUNE(sprintSpeedMult));
			float tW = sprinting ? sp : baseWalk, tR = sprinting ? sp : baseRun;
			u32 mf = handling::moveFlags(pl), dirB = mf & 0xF;
			float T = (mf & 0x200) || !(mf & 0x100) ? tR : tW, m = 1;
			if (accOn && dirB)
			{
				float ang = rt::atan2((float)((dirB & 8 ? 1 : 0) - (dirB & 4 ? 1 : 0)), (float)((dirB & 1 ? 1 : 0) - (dirB & 2 ? 1 : 0)));
				if (!accMoving) { accX = -(1 - rt::clamp(TUNE(moveAccelStart), 0.1f, 1)) * T; accV = 0; if (accLogs < 30) { accLogs++; LOG("accel: setting off - %s %.0f (walk %.0f run %.0f), keys %x, flags %x", sprinting ? "sprint" : (mf & 0x200) || !(mf & 0x100) ? "run" : "walk", T, tW, tR, dirB, mf); } }
				else
				{
					if (accT > 0) accX += accT - T;
					float th = rt::absf(wrapAng(ang - accAng));
					if (th > 0.35f)
					{
						float S = (T + accX) * (1 - (1 - rt::cos(th)) * 0.5f * TUNE(moveTurnLoss));
						S = rt::max(S, T * TUNE(moveAccelStart));
						if (S < T + accX) { accX = S - T; accV = 0; }
					}
				}
				accMoving = true; accAng = ang; accT = T;
				float sec = accX < 0 ? TUNE(moveAccelSec) : TUNE(moveDecelSec);
				decay(accX, accV, 4.74f / rt::max(sec, 0.05f), dt);
				accX = rt::clamp(accX, -0.9f * T, 1.5f * T);
				m = 1 + accX / rt::max(T, 1.0f);
			}
			else { accMoving = false; accX = accV = 0; accT = -1; }
			if (sprinting || rt::absf(m - 1) > 0.002f)
			{
				wroteW = tW * m; wroteR = tR * m;
				*(float *)(cv + 0x38) = wroteW; *(float *)(cv + 0x3C) = wroteR;
				fl |= 0x3000; ours = true;
			}
			else if (ours) { *(float *)(cv + 0x38) = baseWalk; *(float *)(cv + 0x3C) = baseRun; ours = false; }
			if (sprinting) cadence(pl, mode == 2 ? TUNE(tacSprintSpeedMult) / TUNE(tacCadence) * TUNE(sprintCadence) : TUNE(sprintSpeedMult));
		}
		if (overriding && !sprinting) speedOff(pl);
	}
	else if (overriding || ours) speedOff(pl);
	for (int i = 0; i < 2; i++) blend[i] = clampf(blend[i] + ((mode == i + 1 && !move::state) ? dt : -dt) * TUNE(sprintPoseHz), 0, 1);
	float b0 = blend[0] * blend[0] * (3 - 2 * blend[0]), b1 = blend[1] * blend[1] * (3 - 2 * blend[1]), b = rt::max(b0, b1);
	phase += dt * 2 * rt::PI * (mode == 2 ? 1.75f : 1.5f);
	if (phase > 1000) phase -= 1000;
	float bobP = rt::sin(2 * phase) * TUNE(sprintBobDeg), bobH = rt::sin(phase) * TUNE(sprintBobDeg) * 1.3f;
	bool heavy = !handling::wOneHand && handling::wmass > 1.4f;
	if (heavy)
	{
		float hb = rt::max(b0, b1), hp = phase * 0.8f;
		handling::sprP = hb * (TUNE(sprintHeavyPitch) + rt::sin(2 * hp) * TUNE(sprintBobDeg) * 1.2f);
		handling::sprH = hb * (TUNE(sprintHeavyHeading) + rt::sin(hp) * TUNE(sprintBobDeg) * 0.8f);
		handling::sprR = hb * TUNE(sprintHeavyRoll);
		handling::sprX = hb * TUNE(sprintHeavyX);
		handling::sprZ = hb * (TUNE(sprintHeavyZ) + 0.9f * rt::sin(2 * hp + 1));
	}
	else
	{
	handling::sprP = b0 * TUNE(sprintGunPitch) + b1 * TUNE(tacGunPitch) + b * bobP;
	handling::sprH = b0 * TUNE(sprintGunHeading) + b1 * TUNE(tacGunHeading) + b * bobH;
	handling::sprR = b0 * TUNE(sprintGunRoll) + b1 * TUNE(tacGunRoll);
	handling::sprX = b0 * TUNE(sprintGunX) + b1 * TUNE(tacGunX);
	handling::sprZ = b0 * TUNE(sprintGunZ) + b1 * TUNE(tacGunZ) + b * 0.6f * rt::sin(2 * phase + 1);
	}
	float fovWant = b0 * TUNE(sprintFovAdd) + b1 * TUNE(tacFovAdd);
	static bool kicked;
	float kick = g_driving ? 0 : g_fovKick;
	if (g_driving) fovNow = 0;
	else if (rt::absf(fovWant - fovNow) > 0.01f || (fovNow != 0 && fovWant == 0) || rt::absf(kick) > 0.01f || kicked)
	{
		fovNow += (fovWant - fovNow) * clampf(dt * 6, 0, 1);
		kicked = rt::absf(kick) > 0.01f;
		if (rt::absf(fovNow) < 0.02f && fovWant == 0 && !kicked) { fovNow = 0; driftFovOff(pl); }
		else driftFov(pl, fovNow + kick);
	}
	static float sinceLog;
	if (mode && (sinceLog += dt) > 3)
	{
		sinceLog = 0;
		LOG("sprint: %s, speed %.0f%s, tac left %.1f s; steps: third-person animation %.2f -> %.2f (the game set it %d times, left ours %d), first-person %.2f -> %.2f (%d/%d)",
			mode == 2 ? "tac" : "on", overriding ? rd<float>(ptr(proc + 0x2C) + 0x3C) : -1.0f, overriding ? "" : " (not yet set)", tacLeft,
			animBase[0], animWrote[0], animRecomputed[0], animKept[0], animBase[1], animWrote[1], animRecomputed[1], animKept[1]);
		animRecomputed[0] = animRecomputed[1] = animKept[0] = animKept[1] = 0;
	}
}
static float climbScale() { return mode == 2 ? TUNE(sprintClimbScale) * 0.92f : mode == 1 ? TUNE(sprintClimbScale) : 1.0f; }
}

// ---- The whole body in third person.
namespace body
{
static inline bool fin(float x) { return x == x && x < 1e30f && x > -1e30f; }
using handling::Xf; using handling::xfRead; using handling::mul3; using handling::rotAbout; using handling::clampf;
using move::tr; using move::apply; using move::cross; using move::dot; using move::norm; using move::between;
static const float kId[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
static const float R2D = 180 / rt::PI, D2R = rt::PI / 180;
static float acosf(float x) { x = clampf(x, -1, 1); return rt::atan2(rt::sqrt(1 - x * x), x); }
static float wrapA(float a) { while (a > rt::PI) a -= 2 * rt::PI; while (a < -rt::PI) a += 2 * rt::PI; return a; }
static float smooth01(float x) { x = clampf(x, 0, 1); return x * x * (3 - 2 * x); }
static float len3(const float *v) { return rt::sqrt(dot(v, v)); }
static void toVec(const float *m, float *v)
{
	float a = acosf((m[0] + m[4] + m[8] - 1) * 0.5f);
	float x = m[7] - m[5], y = m[2] - m[6], z = m[3] - m[1], s = rt::sqrt(x * x + y * y + z * z);
	if (s < 1e-6f) { v[0] = v[1] = v[2] = 0; return; }
	v[0] = x / s * a; v[1] = y / s * a; v[2] = z / s * a;
}
static void fromVec(const float *v, float *m)
{
	float a = len3(v);
	if (a < 1e-6f) { memcpy(m, kId, 36); return; }
	float ax[3] = {v[0] / a, v[1] / a, v[2] / a};
	rotAbout(ax, a * R2D, m);
}
static void motionLocal(const Xf &P, const float *M, const float *c, const float *mv, float *Xr, float *Xt)
{
	float PT[9], A[9], d[3] = {P.t[0] - c[0], P.t[1] - c[1], P.t[2] - c[2]}, Md[3], w[3];
	tr(P.r, PT); mul3(PT, M, A); mul3(A, P.r, Xr);
	apply(M, d, Md);
	for (int i = 0; i < 3; i++) w[i] = Md[i] + c[i] + (mv ? mv[i] : 0) - P.t[i];
	apply(PT, w, Xt);
	for (int i = 0; i < 3; i++) Xt[i] /= P.s;
}
static void preMul(u32 n, const float *Xr, const float *Xt)
{
	float Or[9], Ot[3], R[9], t[3];
	memcpy(Or, (void *)(n + 0x34), 36); memcpy(Ot, (void *)(n + 0x58), 12);
	mul3(Xr, Or, R); apply(Xr, Ot, t);
	for (int i = 0; i < 3; i++) t[i] += Xt[i];
	memcpy((void *)(n + 0x34), R, 36); memcpy((void *)(n + 0x58), t, 12);
}
static void ident(u32 n) { if (n) { const float Z[3] = {0, 0, 0}; memcpy((void *)(n + 0x34), kId, 36); memcpy((void *)(n + 0x58), Z, 12); } }
static Xf W(u32 n) { return xfRead(n, 0x68); }
static void worldDown(u32 n, int depth = 0)
{
	u32 p = n ? ptr(n + 0x18) : 0;
	if (!p) return;
	Xf P = W(p);
	const float *Lr = (const float *)(n + 0x34), *Lt = (const float *)(n + 0x58);
	float Ls = rd<float>(n + 0x64), R[9], t[3];
	if (!(Ls > 0.001f)) Ls = 1;
	mul3(P.r, Lr, R); apply(P.r, Lt, t);
	for (int i = 0; i < 3; i++) t[i] = t[i] * P.s + P.t[i];
	memcpy((void *)(n + 0x68), R, 36); memcpy((void *)(n + 0x8C), t, 12); *(float *)(n + 0x98) = P.s * Ls;
	if (depth > 40 || !handling::isNodeObj(n)) return;
	u32 arr = ptr(n + OFF_NODE_CHILDREN);
	u16 cnt = rd<u16>(n + OFF_NODE_CHILDCOUNT);
	for (u16 i = 0; arr && i < cnt && i < 64; i++) { u32 c = ptr(arr + 4 * i); if (c) worldDown(c, depth + 1); }
}

enum { PELV, THL, CAL, FTL, THR, CAR, FTR, SPN, NCK, HED, SP2, UAL, UAR, CLL, CLR, FAL, NB };
static const char *kName[NB] = {"Bip01 Pelvis", "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 R Thigh", "Bip01 R Calf",
	"Bip01 R Foot", "Bip01 Spine1", "Bip01 Neck1", "Bip01 Head", "Bip01 Spine2", "Bip01 L UpperArm", "Bip01 R UpperArm",
	"Bip01 L Clavicle", "Bip01 R Clavicle", "Bip01 L Forearm"};
static const bool kNeeded[NB] = {true, true, true, true, true, true, true, false, false, false, false, false, false, false, false, false};
static const bool kBlend[NB] = {true, true, true, true, true, true, true, false, false, false, false, false, false, false, false, false};
static const bool kWatch[NB] = {true, true, true, false, true, true, false, false, false, false, false, false, false, false, false, false};
struct Bone
{
	u32 bone, node, child;
	float lr[9], lt[3];
	float pd[9], pm[3]; bool pdOk;
	float q[3], qv[3], d[3], dv[3];
	float dr[9], dt[3];
	bool have;
};
static Bone b[NB];
static u32 root, toe[2], nac, nacN;
static bool written;
static double findT = -99, lockUntil;
static int headAx = -1;
static float hint[3][3]; static bool hintOk[3];

static float exert;
static float brPh;
static float ws, wsV, wsT, wsNext = 3;
static float idleT;
static float fidT = 7, fidU; static int fid = -1; static float fidS = 1;
static float armF[2], armFV[2], armO[2], armOV[2];
static float lastH, yawB; static bool lastHok;
static float landD, landDV;
static float airVz; static bool wasAir;
static float hpNow = -1, hpMax = 1, hurtT;
static float flin, flinV;
static float hideCmd, kneel, kneelV, holdLowT, lastHeadH = -1, kneelLift; static bool kneeling;
static float handW, handWV, wallL, wallLV; static float handT[3];
static u32 glRef; static float glT, glCool = 3, glW;
static u32 rngS = 0x9E3779B9u;
static float rnd01() { rngS ^= rngS << 13; rngS ^= rngS >> 17; rngS ^= rngS << 5; return (rngS & 0xFFFFFF) / 16777216.0f; }
static float drift(float t, float s) { return 0.5f * rt::sin(t * 0.71f + s) + 0.3f * rt::sin(t * 1.37f + s * 1.7f) + 0.2f * rt::sin(t * 2.29f + s * 2.3f); }
static float lifeClock;
static float env(float u, float a, float h, float r) { if (u < 0) return 0; if (u < a) return smooth01(u / a); if (u < a + h) return 1; return 1 - smooth01((u - a - h) / r); }
static const float kFidDur[4] = {1.3f, 1.7f, 2.4f, 2.2f};
static bool okTick;
static float vel[3], spd, accS, yawRate, lastFs;
static bool havePrev, haveVh; static float pp[3], lastVh;
static bool aiming, weaponOut, sneaking, onGround;
static int extra = 0;
static Script *s_state;
static float stateT;
static const char *kState = "Begin Function {}\nint i\nif player.GetKnockedState != 0\nlet i := 1\nendif\nif player.GetSitting != 0\nlet i := i + 2\nendif\nif player.IsSwimming\nlet i := i + 4\nendif\nSetFunctionValue i\nEnd";

static double lastT;
static float wOn, wIk;
static float drop, dropV, stairZ, stairV; static bool stairHave;
static float lean, leanV, bank, bankV, spP, spV;
static bool hillReset;
static float stairT, stairK;
static float hy, hyV, hp, hpV, hr, hrV;
static float bias; static bool biasHave;
static float gz[2], gzV[2]; static bool gzHave;
static float fpH[2][2], fpT[2][2]; static bool fpOk[2];
static float gAcc[2], gPend[2], gPendT[2], pitchS[2], nS[2][3]; static bool gAccHave[2];
static bool idleLatch; static float idleIn;
static float rest = 9;
static int jumps; static float logT;
static double evUntil; static int evLast = -1;
static float plantPrev[2] = {1, 1};
static float tw, twV;
struct Lock { bool on; float p[2], from[2], st; };
static Lock lk[2]; static float lockW, stepGap;
static bool flOn; static float flYaw, flV, pinned;

static bool valid(u32 r)
{
	if (!r || r != root) return false;
	for (int i = 0; i < NB; i++)
		if (b[i].node && (ptr(b[i].child + 0x18) != b[i].node || !ptr(b[i].node + 0x18) || !handling::isAncestor(r, b[i].node))) return false;
	return b[PELV].node != 0 && nacN && ptr(nac + 0x18) == nacN && ptr(nacN + 0x18);
}
static bool ours(u32 n) { return move::unnamedNode(n) && rd<u16>(n + OFF_NODE_CHILDCOUNT) == 1; }
static bool find(u32 r)
{
	if (rt::now() - findT < 2) return false;
	findT = rt::now();
	root = 0; written = false; headAx = -1; hintOk[0] = hintOk[1] = hintOk[2] = false; gzHave = stairHave = false; fpOk[0] = fpOk[1] = gAccHave[0] = gAccHave[1] = false; pitchS[0] = pitchS[1] = 0; nS[0][0] = nS[0][1] = nS[1][0] = nS[1][1] = 0; nS[0][2] = nS[1][2] = 1; idleLatch = false; idleIn = 0;
	u32 was[NB]; for (int i = 0; i < NB; i++) was[i] = b[i].node;
	memset(b, 0, sizeof b);
	int made = 0, kept = 0;
	for (int i = 0; i < NB; i++)
	{
		u32 bn = findNamed(r, kName[i], 60);
		if (!bn && i == NCK)
		{
			u32 h = findNamed(r, "Bip01 Head", 60), p = h ? ptr(h + 0x18) : 0;
			const char *pn = p ? (const char *)ptr(p + 8) : 0;
			if (pn && rt::startswith(pn, "Bip01 Neck") && !findNamed(p, "Bip01 L Clavicle", 2) && !findNamed(p, "Bip01 R Clavicle", 2)) bn = p;
		}
		if (!bn) { if (kNeeded[i]) { LOG("body: no %s in your skeleton - the body's IK is off", kName[i]); return false; } continue; }
		if (i == NCK && (findNamed(bn, "Bip01 L Clavicle", 1) || findNamed(bn, "Bip01 R Clavicle", 1))) { LOG("body: %s carries the arms - the head turns on its own", kName[i]); continue; }
		u32 tgt = bn, p = ptr(bn + 0x18);
		if ((i == CLL || i == CLR) && p != was[i] && move::unnamedNode(p) && rd<u16>(p + OFF_NODE_CHILDCOUNT) == 1) { tgt = p; p = ptr(p + 0x18); }
		u32 n = ours(p) && p == was[i] ? p : 0;
		if (n) kept++;
		else if ((n = move::insertAbove(tgt))) made++;
		if (!n) { LOG("body: couldn't put a node above %s", kName[i]); if (kNeeded[i]) return false; continue; }
		handling::flagsLike(n, bn);
		b[i].bone = bn; b[i].node = n; b[i].child = tgt;
	}
	nac = findNamed(r, "Bip01 NonAccum", 20);
	if (!nac) nacN = 0;
	if (nac) { u32 p = ptr(nac + 0x18), wasN = nacN; nacN = ours(p) && p == wasN ? p : move::insertAbove(nac); if (nacN) handling::flagsLike(nacN, nac); }
	if (!nacN) { LOG("body: no node above Bip01 NonAccum - the body's IK is off"); return false; }
	toe[0] = findNamed(b[FTL].bone, "Bip01 L Toe0", 4);
	toe[1] = findNamed(b[FTR].bone, "Bip01 R Toe0", 4);
	root = r; lockUntil = rt::now() + 0.5;
	auto pn = [](u32 n) { u32 p = n ? ptr(ptr(n + 0x18) + 0x18) : 0; const char *s = p ? (const char *)ptr(p + 8) : 0; return s ? s : "?"; };
	LOG("body: %d node(s) put in, %d ours already - pelvis under %s, thighs under %s / %s, spine1 under %s, neck %s under %s, toes %x %x",
		made, kept, pn(b[PELV].node), pn(b[THL].node), pn(b[THR].node), b[SPN].node ? pn(b[SPN].node) : "-", b[NCK].bone ? (const char *)ptr(b[NCK].bone + 8) : "-", b[NCK].node ? pn(b[NCK].node) : "-", toe[0], toe[1]);
	return true;
}
static void identity()
{
	if (!written) return;
	written = false;
	TESObjectREFR *pl = player();
	if (pl && valid(thirdPersonNode(pl))) { for (int i = 0; i < NB; i++) ident(b[i].node); ident(nacN); }
	for (int i = 0; i < NB; i++) b[i].have = b[i].pdOk = false;
	lk[0].on = lk[1].on = false; lockW = 0;
	drop = dropV = lean = leanV = bank = bankV = spP = spV = hy = hyV = hp = hpV = hr = hrV = tw = twV = 0;
	for (int k = 0; k < 2; k++) armF[k] = armFV[k] = armO[k] = armOV[k] = 0; landD = landDV = 0; ws = wsV = 0; fid = -1; stairHave = gzHave = false; fpOk[0] = fpOk[1] = gAccHave[0] = gAccHave[1] = false;
}
static void reset() { identity(); hillReset = true; root = 0; glRef = 0; hpNow = -1; flOn = false; flYaw = flV = 0; havePrev = false; wOn = wIk = 0; findT = -99; }

static bool groundAt(float x, float y, float z0, float *gzOut, float *n, bool *nok)
{
	float a[3] = {x, y, z0 + TUNE(bodyRayUp)}, c[3] = {x, y, z0 - TUNE(bodyRayDown)}, fr;
	u32 o;
	for (int k = 0; k < 3; k++)
	{
		if (!rayHitNormal(a, c, &fr, &o, n, nok)) return false;
		float hz = a[2] + (c[2] - a[2]) * fr;
		TESObjectREFR *r = o ? refOfNode(o) : 0;
		u8 ty = r ? formtype((void *)r) : 0;
		if (ty == 0x3B || ty == 0x3C) { a[2] = hz - 3; if (a[2] <= c[2]) return false; continue; }
		if (hz > z0 + TUNE(bodyStepUpMax) + 2) { a[2] = hz - 2; if (a[2] <= c[2]) return false; continue; }
		if (fr < 0.002f) return false;
		*gzOut = hz;
		return true;
	}
	return false;
}

struct Probe { bool have, n0ok, okF[2], cov, stepped, nHok, hgHave; float nH[3], hg[2]; float n0[3], gF[2], nF[2][3], pitchF[2], toeLen[2], covD, covH, wallD[2], g0, backD; };
static Probe pr;
static void probe(TESObjectREFR *pl, float dt)
{
	pr.have = false;
	u32 r = thirdPersonNode(pl);
	if (!valid(r)) return;
	const float *P = pos(pl);
	float *n0 = pr.n0; bool &n0ok = pr.n0ok; n0[0] = n0[1] = 0; n0[2] = 1; n0ok = false;
	float g0;
	bool have0 = groundAt(P[0], P[1], P[2], &g0, n0, &n0ok);
	static int hillTick; static bool stepLast;
	if (hillReset) { hillReset = false; pr.hgHave = pr.nHok = false; stepLast = false; }
	if (have0 && !n0ok && (++hillTick & 1))
	{
		float h = flOn ? pinned : rot(pl)[2], fx = rt::sin(h), fy = rt::cos(h), rx = rt::cos(h), ry = -rt::sin(h);
		const float off[9][2] = {{0, 0}, {32, 0}, {-32, 0}, {0, 32}, {0, -32}, {16, 0}, {-16, 0}, {0, 16}, {0, -16}};
		float gq[9], nq[3]; bool kq, okq[9];
		gq[0] = g0; okq[0] = true;
		for (int i = 1; i < 9; i++)
		{
			okq[i] = groundAt(P[0] + fx * off[i][0] + rx * off[i][1], P[1] + fy * off[i][0] + ry * off[i][1], P[2], &gq[i], nq, &kq);
			if (okq[i] && rt::absf(gq[i] - g0) > rt::absf(off[i][0] + off[i][1]) * 1.5f) okq[i] = false;
		}
		auto fit = [&](float &a, float &sF, float &sR) -> bool
		{
			double S[3][4] = {};
			int n = 0;
			for (int i = 0; i < 9; i++)
			{
				if (!okq[i]) continue;
				double v[3] = {1, off[i][0], off[i][1]};
				for (int r = 0; r < 3; r++) { for (int c = 0; c < 3; c++) S[r][c] += v[r] * v[c]; S[r][3] += v[r] * gq[i]; }
				n++;
			}
			if (n < 4) return false;
			for (int c = 0; c < 3; c++)
			{
				int pv = c; for (int r = c + 1; r < 3; r++) if ((S[r][c] < 0 ? -S[r][c] : S[r][c]) > (S[pv][c] < 0 ? -S[pv][c] : S[pv][c])) pv = r;
				if ((S[pv][c] < 0 ? -S[pv][c] : S[pv][c]) < 1e-6) return false;
				for (int k2 = 0; k2 < 4; k2++) { double t2 = S[c][k2]; S[c][k2] = S[pv][k2]; S[pv][k2] = t2; }
				for (int r = 0; r < 3; r++) if (r != c) { double m2 = S[r][c] / S[c][c]; for (int k2 = 0; k2 < 4; k2++) S[r][k2] -= m2 * S[c][k2]; }
			}
			a = (float)(S[0][3] / S[0][0]); sF = (float)(S[1][3] / S[1][1]); sR = (float)(S[2][3] / S[2][2]);
			return true;
		};
		float a, sF, sR, lim = TUNE(bodySlopeStepUnits);
		if (fit(a, sF, sR))
		{
			int worst = -1; float wr = 0;
			for (int i = 0; i < 9; i++) if (okq[i]) { float r2 = rt::absf(gq[i] - (a + sF * off[i][0] + sR * off[i][1])); if (r2 > wr) { wr = r2; worst = i; } }
			if (wr > lim && worst >= 0) { okq[worst] = false; fit(a, sF, sR); }
			int offN = 0;
			for (int i = 0; i < 9; i++) if (okq[i] && rt::absf(gq[i] - (a + sF * off[i][0] + sR * off[i][1])) > lim) offN++;
			stepLast = offN >= 2;
			float gx = fx * sF + rx * sR, gy = fy * sF + ry * sR;
			float c[3] = {-gx, -gy, 1};
			if (norm(c) > 1e-3f) { memcpy(pr.nH, c, 12); pr.nHok = true; }
			float k2 = clampf(2 * dt / rt::max(TUNE(bodySlopeSmoothSec), 0.02f), 0, 1);
			if (!pr.hgHave) { pr.hg[0] = gx; pr.hg[1] = gy; pr.hgHave = true; }
			else { pr.hg[0] += (gx - pr.hg[0]) * k2; pr.hg[1] += (gy - pr.hg[1]) * k2; }
		}
	}
	if (!have0) { pr.nHok = false; }
	pr.stepped = stepLast;
	if (have0 && !n0ok && pr.nHok) { memcpy(n0, pr.nH, 12); n0ok = true; }
	if (have0 && n0ok && n0[2] > 0.985f && onGround && spd < 200)
	{
		if (!biasHave) { bias = g0 - P[2]; biasHave = true; }
		else bias += (clampf(g0 - P[2], -6, 6) - bias) * clampf(dt * 0.5f, 0, 1);
	}
	float *gF = pr.gF, (*nF)[3] = pr.nF, *pitchF = pr.pitchF, *toeLen = pr.toeLen;
	bool *okF = pr.okF;
	for (int k = 0; k < 2; k++)
	{
		u32 ft = b[k ? FTR : FTL].bone;
		const float *Ab = (const float *)(ft + 0x8C);
		if (fpOk[k] && (fpH[k][0] - Ab[0]) * (fpH[k][0] - Ab[0]) + (fpH[k][1] - Ab[1]) * (fpH[k][1] - Ab[1]) > 30 * 30) fpOk[k] = false;
		const float *A = fpOk[k] ? fpH[k] : Ab;
		float gh = 0, gt = 0, nh[3] = {0, 0, 1}, nt[3]; bool nhok = false, ntok = false;
		okF[k] = groundAt(A[0], A[1], P[2], &gh, nh, &nhok);
		bool tOk = false; toeLen[k] = 0; pitchF[k] = 0;
		if (toe[k])
		{
			const float *Tp = fpOk[k] ? fpT[k] : (const float *)(toe[k] + 0x8C);
			float dx = Tp[0] - A[0], dy = Tp[1] - A[1];
			toeLen[k] = rt::sqrt(dx * dx + dy * dy);
			if (toeLen[k] > 3) tOk = groundAt(Tp[0], Tp[1], P[2], &gt, nt, &ntok);
		}
		if (!okF[k] && tOk) { gh = gt; okF[k] = true; }
		float g = gh;
		if (okF[k] && tOk)
		{
			float maxRise = toeLen[k] * rt::sin(TUNE(bodyFootPitchDeg) * D2R) / rt::max(rt::cos(TUNE(bodyFootPitchDeg) * D2R), 0.2f);
			float rise = gt - gh;
			if (rise > maxRise) g = gh + (rise - maxRise);
			pitchF[k] = rt::atan2(clampf(rise, -maxRise, maxRise), toeLen[k]);
		}
		gF[k] = g;
		if (nhok) memcpy(nF[k], nh, 12); else { nF[k][0] = nF[k][1] = 0; nF[k][2] = 1; }
		if (!nhok && n0ok) memcpy(nF[k], n0, 12);
	}
	pr.cov = false;
	if (sneaking && onGround && TUNE(coverPoseOn) > 0)
	{
		float hb = flOn ? pinned : rot(pl)[2], fx = rt::sin(hb), fy = rt::cos(hb), g = have0 ? g0 : P[2] + bias, reach = TUNE(coverReachUnits), d = 1e9f;
		const float hs[3] = {16, 28, 40};
		for (int i = 0; i < 3; i++)
		{
			float a[3] = {P[0], P[1], g + hs[i]}, c[3] = {P[0] + fx * reach, P[1] + fy * reach, g + hs[i]}, fr; u32 o;
			if (!rayHitObject(a, c, &fr, &o)) continue;
			TESObjectREFR *r2 = o ? refOfNode(o) : 0; u8 ty = r2 ? formtype((void *)r2) : 0;
			if (ty == 0x3B || ty == 0x3C || fr < 0.05f) continue;
			if (fr * reach < d) d = fr * reach;
		}
		if (d < reach)
		{
			float top = -1e9f;
			for (int j = 0; j < 2; j++)
			{
				float x = P[0] + fx * (d + 5 + 9 * j), y = P[1] + fy * (d + 5 + 9 * j), a[3] = {x, y, g + 140}, c[3] = {x, y, g + 4}, fr; u32 o;
				if (rayHitObject(a, c, &fr, &o) && fr > 0.01f) { float z = a[2] + (c[2] - a[2]) * fr; if (z > top) top = z; }
			}
			if (top > g + 14 && top < g + 110) { pr.cov = true; pr.covD = d; pr.covH = top - g; }
		}
		pr.backD = 1e9f;
		if (pr.cov)
		{
			float a[3] = {P[0], P[1], g + 9}, c[3] = {P[0] - fx * 40, P[1] - fy * 40, g + 9}, fr; u32 o;
			if (rayHitObject(a, c, &fr, &o)) pr.backD = fr * 40;
		}
	}
	pr.wallD[0] = pr.wallD[1] = 1e9f;
	if (!sneaking && onGround && spd < 10 && TUNE(coverWallLeanDeg) > 0)
	{
		float hb = flOn ? pinned : rot(pl)[2], rx = rt::cos(hb), ry = -rt::sin(hb), g = have0 ? g0 : P[2] + bias, reach = TUNE(coverWallReach);
		for (int k = 0; k < 2; k++)
		{
			float sd = k ? 1.0f : -1.0f, hits = 0, dsum = 0;
			for (int j = 0; j < 2; j++)
			{
				float h2 = g + (j ? 62 : 44), a[3] = {P[0], P[1], h2}, c[3] = {P[0] + rx * sd * reach, P[1] + ry * sd * reach, h2}, fr; u32 o;
				if (!rayHitObject(a, c, &fr, &o) || fr < 0.05f) continue;
				TESObjectREFR *r2 = o ? refOfNode(o) : 0; u8 ty = r2 ? formtype((void *)r2) : 0;
				if (ty == 0x3B || ty == 0x3C) continue;
				hits++; dsum += fr * reach;
			}
			if (hits == 2) pr.wallD[k] = dsum / 2;
		}
	}
	pr.g0 = have0 ? g0 : P[2] + bias;
	pr.have = true;
}
static void tick(float dt, TESObjectREFR *pl, bool focus, bool free)
{
	if (!pl || dt <= 0) return;
	const float *p = pos(pl);
	if (havePrev && dt < 0.25f)
	{
		float v[3] = {(p[0] - pp[0]) / dt, (p[1] - pp[1]) / dt, (p[2] - pp[2]) / dt};
		if (v[0] * v[0] + v[1] * v[1] > 2500.0f * 2500.0f) { vel[0] = vel[1] = vel[2] = 0; }
		else { float k = clampf(dt / 0.08f, 0, 1); for (int i = 0; i < 3; i++) vel[i] += (v[i] - vel[i]) * k; }
	}
	memcpy(pp, p, 12); havePrev = true;
	float h = flOn ? pinned : rot(pl)[2];
	float f[3] = {rt::sin(h), rt::cos(h), 0};
	spd = rt::sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
	float fs = vel[0] * f[0] + vel[1] * f[1];
	float a = (fs - lastFs) / dt; lastFs = fs;
	accS += (clampf(a, -6000, 6000) - accS) * clampf(dt / 0.12f, 0, 1);
	if (spd > 60)
	{
		float vh = rt::atan2(vel[0], vel[1]);
		if (haveVh) yawRate += (clampf(wrapA(vh - lastVh) / dt, -8, 8) - yawRate) * clampf(dt / 0.15f, 0, 1);
		lastVh = vh; haveVh = true;
	}
	else { haveVh = false; yawRate *= 1 - clampf(dt / 0.2f, 0, 1); }
	u32 proc = ptr((u32)pl + 0x68);
	aiming = proc && rd<u8>(proc + 0x228); weaponOut = proc && rd<u8>(proc + 0x135);
	sneaking = (handling::moveFlags(pl) & 0x400) != 0;
	onGround = move::hkState(move::ctrl(pl)) == 0;
	if (!onGround) { wasAir = true; airVz = rt::min(airVz, vel[2]); }
	else if (wasAir)
	{
		wasAir = false;
		float imp = clampf((-airVz - 180) / 700, 0, 1);
		if (imp > 0.02f) { landDV -= imp * TUNE(lifeLandDip) * 12; leanV += imp * 35; for (int k = 0; k < 2; k++) armFV[k] += imp * 120; exert = rt::min(1.0f, exert + imp * 0.15f); }
		airVz = 0;
	}
	{
		int st = (sneaking ? 1 : 0) | (onGround ? 2 : 0) | (spd > 40 ? 4 : 0) | (weaponOut ? 8 : 0) | (aiming ? 16 : 0) | (spd > 260 ? 32 : 0);
		if (evLast >= 0 && st != evLast) evUntil = rt::now() + TUNE(bodyBlendEventSec);
		evLast = st;
	}
	if ((stateT -= dt) <= 0)
	{
		stateT = 0.2f;
		if (!s_state) { static bool tried; if (!tried) { tried = true; s_state = g_script->CompileScript(kState); LOG("body: the knocked-down / sitting / swimming check %x", (u32)s_state); } }
		double v; extra = s_state && callNum(s_state, &v) ? (int)v : 0;
	}
	{
		static float hpT;
		if ((hpT -= dt) <= 0 && s_healthNow && TUNE(lifeOn) > 0)
		{
			hpT = 0.1f;
			double v;
			if (callNum(s_healthNow, &v) && v > 0)
			{
				float hv = (float)v;
				if (hv > hpMax) hpMax = hv;
				if (hpNow > 0 && hv < hpNow - 0.5f)
				{
					float imp = clampf((hpNow - hv) / hpMax * 6 + 0.25f, 0.25f, 1);
					flinV += imp * TUNE(lifeFlinchDeg) * 14; hpV += imp * 90; for (int k = 0; k < 2; k++) armFV[k] -= imp * 90;
				}
				hpNow = hv;
				hurtT = clampf((0.45f - hv / hpMax) / 0.35f, 0, 1);
			}
		}
	}
	glT -= dt;
	if (glT <= 0) glRef = 0;
	if (!glRef && (glCool -= dt) <= 0 && TUNE(lifeOn) > 0 && !aiming && spd < 250)
	{
		glCool = 0.5f;
		float hb = flOn ? pinned : rot(pl)[2], fx = rt::sin(hb), fy = rt::cos(hb), best = TUNE(lifeGlanceUnits);
		u32 pick = 0;
		forEachActor([&](TESObjectREFR *a) {
			if (!a || a == pl || rd<u32>((u32)a + OFF_ACTOR_LIFESTATE) != 0 || !thirdPersonNode(a)) return;
			const float *q = pos(a);
			float dx = q[0] - p[0], dy = q[1] - p[1], d = rt::sqrt(dx * dx + dy * dy);
			if (d < 40 || d > best || (dx * fx + dy * fy) < -0.2f * d) return;
			best = d; pick = (u32)a;
		});
		if (pick && rnd01() < 0.4f) { glRef = refid((void *)pick); glT = 1.5f + 1.8f * rnd01(); glCool = 4 + 6 * rnd01(); static int told; if (told++ < 8) LOG("body: a look at %x, %.0f units away", glRef, best); }
	}
	u32 life = rd<u32>((u32)pl + OFF_ACTOR_LIFESTATE);
	okTick = free && life == 0 && move::stateNow() == 0;
	if (okTick && TUNE(bodyOn) > 0 && !(extra & 7) && rt::now() >= g_quietUntil) probe(pl, dt); else pr.have = false;
	bool tp = thirdPerson(pl);
	bool want = TUNE(freeLookOn) > 0 && TUNE(bodyOn) > 0 && okTick && tp && !(extra & 7) && !rd<u32>(0x11F2250 + 8);
	float hNow = rot(pl)[2];
	if (!want) { if (flOn) { flOn = false; flYaw = flV = 0; } return; }
	if (!flOn) { flOn = true; pinned = hNow; flYaw = flV = 0; }
	flYaw = wrapA(hNow - pinned);
	bool fire = weaponOut && focus && held(0x01);
	bool moving = spd > 30 || (focus && (held(VK_ACCELERATE) || held(VK_BRAKE) || held(VK_STEERLEFT) || held(VK_STEERRIGHT)));
	float lim = rt::max(TUNE(freeLookMaxDeg), 10.0f) * D2R, tgt = flYaw;
	if (moving || aiming || fire || sprint::modeNow() || menumode()) tgt = 0;
	else if (flYaw > lim) tgt = lim - 25 * D2R;
	else if (flYaw < -lim) tgt = -lim + 25 * D2R;
	if (rt::absf(tgt - flYaw) > 0.0005f)
	{
		float cHz = aiming ? 4.74f / (2 * rt::PI * rt::max(TUNE(freeLookAimSec), 0.05f)) : fire ? TUNE(freeLookCatchFastHz) : TUNE(freeLookCatchHz);
		spring(flYaw, flV, tgt, cHz, 1, dt);
		pinned = wrapA(hNow - flYaw);
	}
	else { flYaw = tgt; flV = 0; }
}

static void solve(int leg, const float *H, const float *K, const float *A, const float *T, float *Rh, float *Rk)
{
	float u[3], l[3], c[3], d[3];
	for (int i = 0; i < 3; i++) { u[i] = K[i] - H[i]; l[i] = A[i] - K[i]; c[i] = A[i] - H[i]; d[i] = T[i] - H[i]; }
	float la = len3(u), lb = len3(l), lc = len3(c), ld = len3(d);
	if (la < 1 || lb < 1) { memcpy(Rh, kId, 36); memcpy(Rk, kId, 36); return; }
	float lo = rt::absf(la - lb) + 0.5f, hi = (la + lb) * 0.999f;
	ld = clampf(ld, lo, hi); lc = clampf(lc, lo, hi);
	float th0 = acosf((la * la + lb * lb - lc * lc) / (2 * la * lb)), th1 = acosf((la * la + lb * lb - ld * ld) / (2 * la * lb));
	float n[3]; cross(u, l, n);
	if (norm(n) > 0.05f * la * lb) { memcpy(hint[leg], n, 12); hintOk[leg] = true; }
	else if (hintOk[leg]) memcpy(n, hint[leg], 12);
	else { memcpy(Rh, kId, 36); memcpy(Rk, kId, 36); return; }
	rotAbout(n, (th0 - th1) * R2D, Rk);
	float l2[3], e[3]; apply(Rk, l, l2);
	for (int i = 0; i < 3; i++) e[i] = K[i] + l2[i] - H[i];
	norm(e); norm(d);
	between(e, d, 1, Rh);
}

static void frame()
{
	double now = rt::now();
	float dt = (float)clampf((float)(now - lastT), 0, 0.05f); lastT = now;
	TESObjectREFR *pl = player();
	if (!pl) return;
	u32 r = thirdPersonNode(pl);
	if (!r) return;
	bool tp = thirdPerson(pl);
	bool hard = !okTick || (extra & 7) || rd<u32>((u32)pl + OFF_ACTOR_LIFESTATE) != 0;
	bool want = TUNE(bodyOn) > 0 && !hard && (tp || net::active()) && !menumode();
	if (hard || TUNE(bodyOn) <= 0) { wOn = wIk = 0; identity(); return; }
	if (menumode()) return;
	if (!valid(r) && !find(r)) return;
	{
		bool bad = !fin(drop) || !fin(dropV) || !fin(lean) || !fin(leanV) || !fin(bank) || !fin(bankV) || !fin(spP) || !fin(spV)
			|| !fin(hy) || !fin(hyV) || !fin(hp) || !fin(hpV) || !fin(hr) || !fin(hrV) || !fin(tw) || !fin(twV) || !fin(stairZ) || !fin(stairV)
			|| !fin(ws) || !fin(wsV) || !fin(landD) || !fin(landDV) || !fin(flin) || !fin(flinV) || !fin(kneel) || !fin(kneelV) || !fin(hideCmd)
			|| !fin(flYaw) || !fin(flV) || !fin(pinned) || !fin(glW) || !fin(kneelLift);
		for (int k = 0; k < 2 && !bad; k++) bad = !fin(gz[k]) || !fin(gzV[k]) || !fin(armF[k]) || !fin(armFV[k]) || !fin(armO[k]) || !fin(armOV[k]) || !fin(pitchS[k]) || !fin(nS[k][0]) || !fin(nS[k][1]) || !fin(nS[k][2]) || !fin(plantPrev[k]);
		for (int i = 0; i < NB && !bad; i++) for (int k = 0; k < 3 && !bad; k++) bad = !fin(b[i].q[k]) || !fin(b[i].qv[k]) || !fin(b[i].d[k]) || !fin(b[i].dv[k]);
		if (bad)
		{
			static int told; if (told++ < 10) LOG("body: a value went NaN - the whole body put back to rest (drop %.1f lean %.1f head %.1f/%.1f torso %.1f feet %.1f/%.1f pelvis offset %.2f %.1f, free look %.2f, life state %u)",
				drop, lean, hy, hp, tw, gz[0], gz[1], b[PELV].q[0], b[PELV].d[2], flYaw, rd<u32>((u32)pl + OFF_ACTOR_LIFESTATE));
			identity();
			for (int i = 0; i < NB; i++) for (int k = 0; k < 3; k++) b[i].q[k] = b[i].qv[k] = b[i].d[k] = b[i].dv[k] = 0;
			flOn = false; flYaw = flV = 0; pinned = rot(pl)[2]; glW = 0; hideCmd = kneel = kneelV = kneelLift = 0; kneeling = false; flin = flinV = 0;
			for (int k = 0; k < 2; k++) { pitchS[k] = 0; nS[k][0] = nS[k][1] = 0; nS[k][2] = 1; plantPrev[k] = 0; }
			gAccHave[0] = gAccHave[1] = false;
			return;
		}
	}
	float fd = flOn && tp ? wrapA(rot(pl)[2] - pinned) : 0;
	wOn = clampf(wOn + (want ? dt : -dt) * 4, 0, 1);
	wIk = clampf(wIk + (onGround ? dt * 6 : -dt * 10), 0, 1);
	if (wOn <= 0) { identity(); return; }
	written = true;
	const float *P = pos(pl);
	float h = flOn ? pinned : rot(pl)[2];
	float f[3] = {rt::sin(h), rt::cos(h), 0}, rr[3] = {rt::cos(h), -rt::sin(h), 0}, up[3] = {0, 0, 1};
	float aimK = aiming ? TUNE(bodyAimScale) : 1;

	if (!pr.have) return;
	float n0[3] = {pr.n0[0], pr.n0[1], pr.n0[2]}; bool n0ok = pr.n0ok;
	float gF[2] = {pr.gF[0], pr.gF[1]}, nF[2][3], pitchF[2] = {pr.pitchF[0], pr.pitchF[1]}, toeLen[2] = {pr.toeLen[0], pr.toeLen[1]};
	bool okF[2] = {pr.okF[0], pr.okF[1]};
	memcpy(nF, pr.nF, sizeof nF);
	if (!gzHave) { for (int k = 0; k < 2; k++) { gz[k] = okF[k] ? gF[k] : P[2] + bias; gzV[k] = 0; } gzHave = true; }
	for (int k = 0; k < 2; k++)
	{
		float tg = okF[k] ? gF[k] : P[2] + bias;
		bool onPlane = false;
		if (n0ok && n0[2] > 0.3f && !pr.stepped && stairT <= 0)
		{
			const float *Q = fpOk[k] ? fpH[k] : (const float *)(b[k ? FTR : FTL].bone + 0x8C);
			float zp = P[2] + bias - (n0[0] * (Q[0] - P[0]) + n0[1] * (Q[1] - P[1])) / n0[2];
			onPlane = rt::absf(tg - zp) < 2.5f;
		}
		if (!gAccHave[k] || rt::absf(tg - gAcc[k]) < 2 || onPlane) { gAcc[k] = tg; gAccHave[k] = true; gPendT[k] = 0; }
		else
		{
			if (rt::absf(tg - gPend[k]) < 2) gPendT[k] += dt; else { gPend[k] = tg; gPendT[k] = 0; }
			if (gPendT[k] >= TUNE(bodyFootSettleSec)) { gAcc[k] = tg; gPendT[k] = 0; }
		}
		spring(gz[k], gzV[k], gAcc[k], TUNE(bodyFootHz), 1, dt);
		float ke = clampf(dt / rt::max(TUNE(bodyFootSettleSec), 0.02f), 0, 1);
		pitchS[k] += (pitchF[k] - pitchS[k]) * ke;
		for (int i = 0; i < 3; i++) nS[k][i] += (nF[k][i] - nS[k][i]) * ke;
		if (norm(nS[k]) < 0.5f) { nS[k][0] = nS[k][1] = 0; nS[k][2] = 1; }
		pitchF[k] = pitchS[k]; memcpy(nF[k], nS[k], 12);
	}
	float dz[2] = {gz[0] - (P[2] + bias), gz[1] - (P[2] + bias)};
	for (int k = 0; k < 2; k++) dz[k] = clampf(dz[k], -TUNE(bodyDropMaxUnits) * 1.5f, TUNE(bodyRayUp) - 5);
	float slope = 0, slopeR = 0;
	if (pr.hgHave) { slope = pr.hg[0] * f[0] + pr.hg[1] * f[1]; slopeR = pr.hg[0] * rr[0] + pr.hg[1] * rr[1]; }
	slope = clampf(slope, -1.2f, 1.2f); slopeR = clampf(slopeR, -1.2f, 1.2f);

	float accM = accS / kUnitsPerMetre, vM = spd / kUnitsPerMetre;
	float runK = clampf((spd - 180) / 300, 0, 1);
	float slopeDeg = rt::atan2(slope, 1) * R2D, sideDeg = rt::atan2(slopeR, 1) * R2D, moveK = 0.5f + 0.5f * clampf(spd / 150, 0, 1);
	float cv0 = rt::max(TUNE(bodySlopeCurveDeg), 5.0f);
	float hillF = slopeDeg * (1 + rt::absf(slopeDeg) / cv0), hillS = sideDeg * (1 + rt::absf(sideDeg) / cv0);
	stairT = pr.stepped ? 0.6f : rt::max(0.0f, stairT - dt);
	stairK = clampf(stairK + (stairT > 0 ? dt : -dt) * 3, 0, 1);
	float gradeDeg = rt::atan2(rt::sqrt(slope * slope + slopeR * slopeR), 1) * R2D;
	float hillK = moveK * (0.5f + 0.5f * aimK) * (1 - smooth01(stairK)) * smooth01((gradeDeg - TUNE(bodySlopeMinDeg)) / 4);
	float hillLean = clampf(hillF * (hillF > 0 ? TUNE(bodySlopeBodyUp) : TUNE(bodySlopeBodyDown)) * hillK, -12, 12);
	float hillBank = clampf(hillS * TUNE(bodySlopeSide) * hillK, -10, 10);
	float leanT = rt::atan2(accM, 9.81f) * R2D * TUNE(bodyAccelLean) + runK * TUNE(bodyRunLeanDeg);
	leanT = clampf(leanT * aimK, -TUNE(bodyLeanMaxDeg), TUNE(bodyLeanMaxDeg)) + hillLean;
	spring(lean, leanV, onGround ? leanT : 0, TUNE(bodyLeanHz), TUNE(bodyLeanDamping), dt);
	float bankT = spd > 80 && onGround ? rt::atan2(vM * yawRate, 9.81f) * R2D * TUNE(bodyBankScale) : 0;
	bankT = clampf(bankT * aimK, -TUNE(bodyBankMaxDeg), TUNE(bodyBankMaxDeg)) + (onGround ? hillBank : 0);
	spring(bank, bankV, bankT, TUNE(bodyBankHz), TUNE(bodyBankDamping), dt);
	float spT = hillF * (hillF > 0 ? TUNE(bodySlopeLeanUp) : TUNE(bodySlopeLeanDown)) * hillK;
	spring(spP, spV, onGround ? clampf(spT, -12, 24) : 0, 2.0f, 1, dt);
	float dbd = TUNE(bodyDropDeadband), dzL = rt::min(0.0f, dz[0] + dbd), dzR = rt::min(0.0f, dz[1] + dbd);
	float dropT = rt::min(0.0f, rt::min(dzL * plantPrev[0], dzR * plantPrev[1])) - rt::absf(slopeDeg) / 20 * TUNE(bodySlopeCrouchUnits);
	dropT = clampf(dropT, -TUNE(bodyDropMaxUnits), 0) * TUNE(bodyIkWeight);
	spring(drop, dropV, onGround ? dropT : 0, TUNE(bodyDropHz), 1, dt);
	if (!stairHave || !onGround) { stairZ = P[2]; stairV = 0; stairHave = true; }
	else
	{
		spring(stairZ, stairV, P[2], TUNE(bodyStairHz), 1, dt);
		float m = TUNE(bodyStairMaxUnits);
		if (stairZ - P[2] > m) stairZ = P[2] + m; else if (stairZ - P[2] < -m) stairZ = P[2] - m;
	}
	float wl = wOn * wIk;
	float vz = (drop + (stairZ - P[2]) * TUNE(bodyIkWeight)) * wl;

	float lifeK = TUNE(lifeOn) > 0 ? wOn : 0;
	lifeClock += dt;
	bool still = spd < 12 && onGround;
	idleT = still ? idleT + dt : 0;
	float idleK = smooth01((50 - spd) / 50) * (onGround ? 1.0f : 0.0f) * (aiming ? 0.0f : 1.0f);
	if (sprint::modeNow()) exert += dt / 9; else if (spd > 300) exert += dt / 40; else exert -= dt / 22;
	exert = clampf(exert, 0, 1);
	brPh += 2 * rt::PI * (TUNE(lifeBreathHz) + exert * 0.45f) * dt;
	if (brPh > 2 * rt::PI) brPh -= 2 * rt::PI;
	float inh = 0.5f - 0.5f * rt::cos(brPh);
	float brAmp = 1 + exert * 2 + hurtT;
	float chestB = inh * TUNE(lifeBreathDeg) * brAmp;
	float shLift = inh * TUNE(lifeBreathShoulder) * brAmp;
	float tiredHunch = (exert * idleK + hurtT) * TUNE(lifeTiredHunchDeg);
	spring(flin, flinV, 0, 3.5f, 0.45f, dt); flin = clampf(flin, -4, 18);
	if (idleT > 1.5f) { if ((wsNext -= dt) <= 0) { wsT = (wsT > 0 ? -1.0f : 1.0f) * (0.55f + 0.45f * rnd01()) * (weaponOut ? 0.5f : 1.0f); wsNext = 4 + 6 * rnd01(); } }
	else if (!still) wsT = 0;
	spring(ws, wsV, wsT * idleK, TUNE(lifeShiftHz), 0.9f, dt);
	float fidLift = 0, fidLiftS = 0, fidYaw = 0, fidPitch = 0, fidRoll = 0, fidChestRoll = 0;
	if (fid < 0)
	{
		if (idleT > 3 && !weaponOut && idleK > 0.8f && (fidT -= dt) <= 0) { fid = (int)(rnd01() * 4) & 3; fidU = 0; fidS = rnd01() < 0.5f ? -1.0f : 1.0f; fidT = 9 + 12 * rnd01(); static int told; if (told++ < 12) LOG("body: a %s", fid == 0 ? "shrug" : fid == 1 ? "shoulder rolled" : fid == 2 ? "neck stretch" : "look round"); }
	}
	else
	{
		fidU += dt * ((still && !weaponOut) ? 1.0f : 3.0f);
		float D = kFidDur[fid], ev = 0;
		switch (fid)
		{
		case 0: ev = env(fidU, 0.35f, 0.25f, 0.7f); fidLift = ev * TUNE(lifeShrugUnits); fidPitch = -ev * 4; break;
		case 1: ev = env(fidU, 0.5f, 0.3f, 0.9f); fidLiftS = ev * TUNE(lifeShrugUnits) * 0.8f * fidS; fidChestRoll = ev * 3 * fidS; break;
		case 2: ev = env(fidU, 0.7f, 0.6f, 1.1f); fidRoll = ev * 14 * fidS; fidPitch = ev * 4; break;
		case 3: ev = env(fidU, 0.45f, 1.0f, 0.75f); fidYaw = ev * 28 * fidS; fidPitch = -ev * 5; break;
		}
		if (fidU >= D) fid = -1;
	}
	float hNow = h;
	float yb = lastHok && dt > 0 ? clampf(wrapA(hNow - lastH) / dt * R2D, -720, 720) : 0;
	lastH = hNow; lastHok = true;
	float ybA = dt > 0 ? (yb - yawB) / dt : 0;
	yawB += (yb - yawB) * clampf(dt / 0.05f, 0, 1);
	{
		float wA = TUNE(lifeArmSwing) * lifeK * (aiming ? 0.0f : weaponOut ? 0.35f : 1.0f);
		float w = 2 * rt::PI * TUNE(lifeArmHz), z = TUNE(lifeArmDamping);
		float fwdA = -clampf(accM, -6, 6) / 0.6f * R2D;
		float turnA = clampf(ybA, -1500, 1500) * 0.35f;
		for (int k = 0; k < 2; k++)
		{
			float side = k ? -1.0f : 1.0f;
			float idA = TUNE(lifeArmIdleDeg) * idleK * wA;
			float restF = idA * drift(lifeClock * 0.35f, 1.7f * k + 0.4f);
			float restO = idA * (0.35f + 0.25f * drift(lifeClock * 0.3f, 2.9f * k + 1.1f) + 0.4f * inh) + rt::max(0.0f, ws * side) * idA * 0.8f;
			int n = (int)(dt / 0.008f) + 1; float hs = dt / n;
			for (int j = 0; j < n; j++)
			{
				armFV[k] += (w * w * (restF - armF[k]) - 2 * z * w * armFV[k] + fwdA * wA) * hs; armF[k] += armFV[k] * hs;
				armOV[k] += (w * w * (restO - armO[k]) - 2 * z * w * armOV[k] + side * turnA * wA) * hs; armO[k] += armOV[k] * hs;
			}
			armF[k] = clampf(armF[k], -35, 35); if (armO[k] < 0) { armO[k] = 0; if (armOV[k] < 0) armOV[k] = 0; } armO[k] = rt::min(armO[k], 25.0f);
		}
	}
	spring(landD, landDV, 0, TUNE(lifeLandHz), 0.55f, dt);
	landD = clampf(landD, -TUNE(lifeLandDip), 4);
	float shiftU = ws * TUNE(lifeShiftUnits) * lifeK, hipRoll = -ws * TUNE(lifeHipDeg) * lifeK;
	bool inCover = pr.cov && sneaking && !aiming && spd < 70 && onGround && TUNE(coverPoseOn) > 0;
	float squatMax = TUNE(coverSquatMaxUnits), hideMax = squatMax + TUNE(coverKneelDropUnits) + 6;
	if (inCover && lastHeadH > 0)
	{
		float err = lastHeadH - (pr.covH - TUNE(coverHeadMargin) - 9);
		hideCmd = clampf(hideCmd + clampf(err, -20, 20) * clampf(dt * 5, 0, 1), 0, hideMax);
	}
	else hideCmd = rt::max(0.0f, hideCmd - dt * 40);
	holdLowT = inCover && hideCmd > 3 ? holdLowT + dt : 0;
	if (!inCover) kneeling = false;
	else if (kneeling && pr.backD < 30) kneeling = false;
	else if (!kneeling && pr.backD >= 30 && (hideCmd > squatMax + 1 || holdLowT > TUNE(coverKneelSec)))
	{
		kneeling = true;
		static int told; if (told++ < 10) LOG("body: cover %.0f high, %.0f away - %s", pr.covH, pr.covD, hideCmd > squatMax + 1 ? "too low to crouch behind: down on a knee" : "held low 10 s: down on a knee");
	}
	spring(kneel, kneelV, kneeling ? 1.0f : 0.0f, TUNE(coverKneelHz), 1, dt);
	kneel = clampf(kneel, 0, 1);
	float squat = rt::min(hideCmd, squatMax) * (1 - 0.6f * kneel);
	float coverHunch = squat * TUNE(coverHunchPerUnit) * (1 - 0.5f * kneel) + kneel * 6;
	if (pr.cov) coverHunch = rt::min(coverHunch, rt::atan2(clampf((pr.covD - 22) / 40, 0, 1), rt::sqrt(1 - clampf((pr.covD - 22) / 40, 0, 1) * clampf((pr.covD - 22) / 40, 0, 1))) * R2D);
	kneelLift = rt::max(0.0f, kneelLift - dt * 4);
	float coverHead = -squat * 0.8f;
	vz -= (squat * 0.55f + kneel * TUNE(coverKneelDropUnits) - kneelLift) * wOn;
	static double covLogT;
	if (inCover && now - covLogT > 3) { covLogT = now; LOG("body: cover %.0f high %.0f away - head %.0f over the ground, lowered %.1f (kneel %.2f)", pr.covH, pr.covD, lastHeadH, hideCmd, kneel); }
	vz += landD * wOn;

	for (int i = SPN; i < NB; i++) ident(b[i].node);
	bool jump = false; float worst = 0; int worstI = -1;
	float fr60 = rt::max(dt / 0.0167f, 0.5f), lim = TUNE(bodyBlendJumpDeg) * rt::max(1.0f, fr60 * fr60);
	bool keen = now < evUntil;
	if (keen) lim *= TUNE(bodyBlendEventKeen);
	for (int i = 0; i < NB; i++)
	{
		if (!kBlend[i] || !b[i].bone || !b[i].have) continue;
		const float *Lr = (const float *)(b[i].bone + 0x34), *Lt = (const float *)(b[i].bone + 0x58);
		bool watch = kWatch[i];
		float LT[9], D[9], PT[9], E[9]; tr(b[i].lr, LT); mul3(Lr, LT, D);
		float a = acosf((D[0] + D[4] + D[8] - 1) * 0.5f) * R2D;
		tr(b[i].pd, PT); mul3(D, PT, E);
		float acc = acosf((E[0] + E[4] + E[8] - 1) * 0.5f) * R2D;
		float mv[3], mvU = 0; for (int k = 0; k < 3; k++) { mv[k] = Lt[k] - b[i].lt[k]; float q = mv[k] - b[i].pm[k]; mvU += q * q; }
		mvU = rt::sqrt(mvU);
		if (watch && b[i].pdOk && a > (keen ? 1.0f : TUNE(bodyBlendJumpDeg) * 0.6f) && acc > lim) { jump = true; if (acc > worst) { worst = acc; worstI = i; } }
		if (i == PELV && b[i].pdOk && mvU > TUNE(bodyBlendJumpUnits) * rt::max(1.0f, fr60 * fr60)) { jump = true; if (worstI < 0) worst = mvU; }
		memcpy(b[i].pd, D, 36); memcpy(b[i].pm, mv, 12); b[i].pdOk = true;
	}
	if (now < lockUntil || dt <= 0 || dt >= 0.05f || wOn < 0.5f) jump = false;
	if (jump)
	{
		jumps++;
		static int told;
		if (told < 25) { told++; LOG("body: the animation jumped (%s turned %.1f deg in a frame) - blending it over %.2f s", worstI >= 0 ? kName[worstI] : "the pelvis moved", worst, TUNE(bodyBlendSec)); }
	}
	float bw = 4.74f / rt::max(TUNE(bodyBlendSec), 0.05f);
	float pelvR[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, pelvT[3] = {0, 0, 0}, pelvD[3] = {0, 0, 0};
	for (int i = 0; i < NB; i++)
	{
		if (!kBlend[i] || !b[i].bone) continue;
		const float *Lr = (const float *)(b[i].bone + 0x34), *Lt = (const float *)(b[i].bone + 0x58);
		Bone &B = b[i];
		if (jump && B.have)
		{
			float LT[9], Q[9]; tr(Lr, LT); mul3(B.dr, LT, Q);
			toVec(Q, B.q);
			for (int k = 0; k < 3; k++) { B.d[k] = clampf(B.dt[k] - Lt[k], -30, 30); B.qv[k] = B.dv[k] = 0; }
		}
		if (!B.have) { for (int k = 0; k < 3; k++) B.q[k] = B.qv[k] = B.d[k] = B.dv[k] = 0; }
		for (int k = 0; k < 3; k++) { decay(B.q[k], B.qv[k], bw, dt); decay(B.d[k], B.dv[k], bw, dt); }
		float Qm[9], Ql[3], Ot[3];
		fromVec(B.q, Qm);
		apply(Qm, Lt, Ql);
		for (int k = 0; k < 3; k++) Ot[k] = Lt[k] - Ql[k] + B.d[k];
		mul3(Qm, Lr, B.dr);
		for (int k = 0; k < 3; k++) B.dt[k] = Lt[k] + B.d[k];
		memcpy(B.lr, Lr, 36); memcpy(B.lt, Lt, 12); B.have = true;
		if (i == PELV) { memcpy(pelvR, Qm, 36); for (int q = 0; q < 3; q++) { pelvT[q] = Lt[q] - Ql[q]; pelvD[q] = B.d[q]; } }
		else { memcpy((void *)(B.node + 0x34), Qm, 36); memcpy((void *)(B.node + 0x58), Ot, 12); }
	}
	float Rl[9], Rb[9], M[9], c[3] = {P[0], P[1], P[2] + bias}, mv[3] = {rr[0] * shiftU, rr[1] * shiftU, vz};
	float Rv[9], Rt[9]; rotAbout(up, fd * R2D, Rv);
	{
		float wt = 0;
		if (idleT > TUNE(coverWallSec) && !aiming && pr.have)
		{
			for (int k = 0; k < 2; k++)
				if (pr.wallD[k] < TUNE(coverWallReach))
				{
					float a = rt::atan2(rt::max(pr.wallD[k] - 12, 0.0f), 62) * R2D;
					if (a <= TUNE(coverWallLeanDeg) && rt::absf(a) > rt::absf(wt)) wt = k ? a : -a;
				}
		}
		spring(wallL, wallLV, wt * lifeK, 1.2f, 1, dt);
	}
	rotAbout(rr, -lean * wOn, Rl); rotAbout(f, (bank + wallL) * wOn, Rb); mul3(Rb, Rl, Rt); mul3(Rt, Rv, M);
	{
		u32 pn = b[PELV].node;
		memcpy((void *)(pn + 0x34), pelvR, 36); memcpy((void *)(pn + 0x58), pelvT, 12);
		ident(nacN);
		Xf Pw = W(ptr(nacN + 0x18));
		float Xr[9], Xt[3];
		{
			float Rn[9], dw[3]; mul3(Pw.r, (const float *)(nac + 0x34), Rn); apply(Rn, pelvD, dw);
			for (int q = 0; q < 3; q++) mv[q] += dw[q] * Pw.s;
		}
		motionLocal(Pw, M, c, mv, Xr, Xt);
		preMul(nacN, Xr, Xt);
		worldDown(nacN);
		if (rt::absf(hipRoll) > 0.01f)
		{
			float Rh2[9]; rotAbout(f, hipRoll, Rh2);
			motionLocal(W(ptr(pn + 0x18)), Rh2, (const float *)(b[PELV].bone + 0x8C), 0, Xr, Xt);
			preMul(pn, Xr, Xt);
			worldDown(pn);
		}
	}
	float twT = 0;
	{
		u32 cn = handling::sceneCam(), hb0 = b[HED].bone;
		if (cn && hb0 && headAx >= 0 && TUNE(headLookOn) > 0 && !rd<u32>(0x11F2250 + 8))
		{
			Xf H0 = W(hb0);
			float sg = (headAx & 1) ? -1.0f : 1.0f, hf[3] = {sg * H0.r[headAx / 2], sg * H0.r[3 + headAx / 2], sg * H0.r[6 + headAx / 2]};
			const float *cw = (const float *)(cn + 0x68);
			float cf[3] = {cw[0], cw[3], cw[6]};
			float Y = wrapA(rt::atan2(dot(cf, rr), dot(cf, f)) - rt::atan2(dot(hf, rr), dot(hf, f))) * R2D;
			float share = TUNE(headTorsoShare) * (aiming ? 0 : weaponOut ? 0.5f : 1);
			twT = clampf(Y * share, -TUNE(headTorsoMaxDeg), TUNE(headTorsoMaxDeg)) * wOn * TUNE(headLookScale);
		}
	}
	spring(tw, twV, twT, TUNE(headLookHz) * 0.7f, 1, dt);
	float spRoll = (ws * 0.8f * TUNE(lifeHipDeg) / 3 + fidChestRoll) * lifeK;
	if (b[SPN].node && (rt::absf(spP) + rt::absf(tw) + rt::absf(spRoll) + tiredHunch + coverHunch + rt::absf(flin) > 0.01f))
	{
		u32 sn = b[SPN].node;
		Xf Pw = W(ptr(sn + 0x18));
		const float *sc = (const float *)(b[SPN].bone + 0x8C);
		float S[9], Sl[9], St[9], Sr[9], S1[9], Xr[9], Xt[3];
		rotAbout(rr, (-(spP + tiredHunch * lifeK + coverHunch) + flin * lifeK) * wOn, Sl); rotAbout(up, -tw, St); rotAbout(f, spRoll, Sr);
		mul3(St, Sl, S1); mul3(Sr, S1, S);
		motionLocal(Pw, S, sc, 0, Xr, Xt);
		preMul(sn, Xr, Xt);
		worldDown(sn);
	}
	if (lifeK > 0.001f)
	{
		float Xr[9], Xt[3];
		if (b[SP2].node && rt::absf(chestB) > 0.005f)
		{
			float S[9]; rotAbout(rr, chestB * lifeK, S);
			motionLocal(W(ptr(b[SP2].node + 0x18)), S, (const float *)(b[SP2].bone + 0x8C), 0, Xr, Xt);
			preMul(b[SP2].node, Xr, Xt);
			worldDown(b[SP2].node);
		}
		for (int k = 0; k < 2; k++)
		{
			Bone &U = b[k ? UAR : UAL];
			if (!U.node) continue;
			float side = k ? -1.0f : 1.0f;
			float lift = (shLift + fidLift + (fidS * side > 0 ? fidLiftS * fidS * side : 0) - (k ? 0 : TUNE(lifeShoulderDropUnits))) * lifeK;
			Bone &C = b[k ? CLR : CLL];
			if (C.node && rt::absf(lift) > 0.01f)
			{
				const float *c0 = (const float *)(C.bone + 0x8C), *u0 = (const float *)(U.bone + 0x8C);
				float dx = u0[0] - c0[0], dy = u0[1] - c0[1], dz2 = u0[2] - c0[2], cl = rt::max(rt::sqrt(dx * dx + dy * dy + dz2 * dz2), 6.0f);
				float Ce[9]; rotAbout(f, side * clampf(lift / cl, -0.35f, 0.5f) * R2D, Ce);
				motionLocal(W(ptr(C.node + 0x18)), Ce, c0, 0, Xr, Xt);
				preMul(C.node, Xr, Xt);
				worldDown(C.node);
			}
			float Rf[9], Ro[9], R[9];
			rotAbout(rr, armF[k], Rf); rotAbout(f, side * armO[k], Ro); mul3(Ro, Rf, R);
			motionLocal(W(ptr(U.node + 0x18)), R, (const float *)(U.bone + 0x8C), 0, Xr, Xt);
			preMul(U.node, Xr, Xt);
			worldDown(U.node);
		}
	}
	{
		Bone &U = b[UAL], &Fo = b[FAL];
		bool want = inCover && !weaponOut && U.node && Fo.node && hideCmd >= 0;
		float hT = 0;
		if (want)
		{
			const float *S = (const float *)(U.bone + 0x8C), *E = (const float *)(Fo.bone + 0x8C);
			u32 hand = findNamed(Fo.bone, "Bip01 L Hand", 3);
			const float *Wr = hand ? (const float *)(hand + 0x8C) : 0;
			if (Wr)
			{
				float g = P[2] + bias, topZ = g + pr.covH;
				float x = P[0] + f[0] * (pr.covD + 3) - rr[0] * 9, y = P[1] + f[1] * (pr.covD + 3) - rr[1] * 9;
				float tz = topZ + 2;
				if (topZ > S[2] - 2) { x = P[0] + f[0] * (pr.covD - 3) - rr[0] * 9; y = P[1] + f[1] * (pr.covD - 3) - rr[1] * 9; tz = S[2] - 8; }
				float la = 0, lb = 0, dd;
				{ float u[3] = {E[0] - S[0], E[1] - S[1], E[2] - S[2]}, l[3] = {Wr[0] - E[0], Wr[1] - E[1], Wr[2] - E[2]}; la = len3(u); lb = len3(l); }
				float dv[3] = {x - S[0], y - S[1], tz - S[2]}; dd = len3(dv);
				if (dd < (la + lb) * 0.97f) { handT[0] = x; handT[1] = y; handT[2] = tz; hT = 1; }
			}
		}
		spring(handW, handWV, hT * lifeK, 2.5f, 1, dt);
		handW = clampf(handW, 0, 1);
		if (handW > 0.01f && U.node && Fo.node)
		{
			u32 hand = findNamed(Fo.bone, "Bip01 L Hand", 3);
			if (hand)
			{
				const float *S = (const float *)(U.bone + 0x8C), *E = (const float *)(Fo.bone + 0x8C), *Wr = (const float *)(hand + 0x8C);
				float Tg[3]; for (int i = 0; i < 3; i++) Tg[i] = Wr[i] + (handT[i] - Wr[i]) * handW;
				float Ru[9], Rf[9], Xr[9], Xt[3];
				solve(2, S, E, Wr, Tg, Ru, Rf);
				Xf Uw = W(ptr(U.node + 0x18)), Ew = W(ptr(Fo.node + 0x18));
				motionLocal(Uw, Ru, S, 0, Xr, Xt); preMul(U.node, Xr, Xt);
				motionLocal(Ew, Rf, E, 0, Xr, Xt); preMul(Fo.node, Xr, Xt);
				worldDown(U.node);
			}
		}
	}
	float plant[2] = {0, 0}, hgt[2] = {0, 0};
	if (TUNE(bodyIkOn) > 0 && wl > 0.001f)
	{
		float Mi[9]; tr(M, Mi);
		idleIn = spd < 10 && onGround ? idleIn + dt : 0;
		if (spd > 24 || !onGround) idleLatch = false; else if (idleIn > 0.12f) idleLatch = true;
		bool idle = idleLatch && TUNE(bodyStepUnits) > 0 && kneel < 0.05f;
		lockW = clampf(lockW + (idle ? dt : -dt) * 6, 0, 1);
		stepGap -= dt;
		for (int k = 0; k < 2; k++)
		{
			Bone &Th = b[k ? THR : THL], &Ca = b[k ? CAR : CAL], &Ft = b[k ? FTR : FTL];
			Xf Tw = W(Th.bone), Cw = W(Ca.bone), Fw = W(Ft.bone);
			float H[3], K[3], A[3]; memcpy(H, Tw.t, 12); memcpy(K, Cw.t, 12); memcpy(A, Fw.t, 12);
			float y[3] = {A[0] - c[0] - mv[0], A[1] - c[1] - mv[1], A[2] - c[2] - mv[2]}, a0[3];
			apply(Mi, y, a0);
			for (int i = 0; i < 3; i++) a0[i] += c[i];
			hgt[k] = a0[2] - (P[2] + bias);
			plant[k] = 1 - smooth01((hgt[k] - rest - 2) / 12);
			float tz = a0[2] + dz[k];
			float T[3] = {A[0], A[1], A[2] + (tz - A[2]) * wl * (tz > A[2] ? 1.0f : TUNE(bodyIkWeight))};
			Lock &L = lk[k];
			if (!idle && L.on && lockW <= 0) { L.on = false; L.st = -1; }
			if (idle && !L.on) { L.on = true; L.p[0] = A[0]; L.p[1] = A[1]; L.st = -1; }
			if (L.on)
			{
				float ex = A[0] - L.p[0], ey = A[1] - L.p[1];
				if (idle && L.st < 0 && ex * ex + ey * ey > TUNE(bodyStepUnits) * TUNE(bodyStepUnits) && lk[1 - k].st < 0 && stepGap <= 0)
				{
					L.st = 0; L.from[0] = L.p[0]; L.from[1] = L.p[1]; stepGap = TUNE(bodyStepSec) * 0.6f;
				}
				float cx = L.p[0], cy = L.p[1], lift = 0;
				if (L.st >= 0)
				{
					L.st += dt / rt::max(TUNE(bodyStepSec), 0.05f);
					float u = smooth01(L.st);
					cx = L.from[0] + (A[0] - L.from[0]) * u; cy = L.from[1] + (A[1] - L.from[1]) * u;
					lift = rt::sin(rt::PI * clampf(L.st, 0, 1)) * TUNE(bodyStepLift);
					if (L.st >= 1) { L.p[0] = A[0]; L.p[1] = A[1]; L.st = -1; }
				}
				T[0] += (cx - A[0]) * lockW * wl; T[1] += (cy - A[1]) * lockW * wl; T[2] += lift * lockW * wl;
				if (lift > 0.5f) plant[k] *= 1 - clampf(lift / rt::max(TUNE(bodyStepLift), 1.0f), 0, 1);
			}
			bool rearLeg = (k == 1) == (TUNE(coverKneelRight) > 0);
			if (kneel > 0.001f)
			{
				float la = 0, lb = 0;
				{ float u[3], l2[3]; for (int i = 0; i < 3; i++) { u[i] = K[i] - H[i]; l2[i] = A[i] - K[i]; } la = len3(u); lb = len3(l2); }
				float gk = pr.g0, Tk[3];
				if (rearLeg) { Tk[0] = H[0] - f[0] * lb * 0.9f; Tk[1] = H[1] - f[1] * lb * 0.9f; Tk[2] = gk + 7; }
				else
				{
					float fw = rt::min(la * 0.6f, pr.cov ? rt::max(pr.covD - 14, 0.0f) : la * 0.6f), out = (la * 0.6f - fw) * 0.8f;
					float sd = k ? 1.0f : -1.0f;
					Tk[0] = H[0] + f[0] * fw + rr[0] * sd * out; Tk[1] = H[1] + f[1] * fw + rr[1] * sd * out; Tk[2] = gk + rest;
				}
				for (int i = 0; i < 3; i++) T[i] += (Tk[i] - T[i]) * kneel * wOn;
				if (rearLeg) plant[k] *= 1 - kneel;
			}
			fpH[k][0] = T[0]; fpH[k][1] = T[1];
			if (toe[k]) { const float *Tq = (const float *)(toe[k] + 0x8C); fpT[k][0] = T[0] + Tq[0] - A[0]; fpT[k][1] = T[1] + Tq[1] - A[1]; }
			else { fpT[k][0] = T[0]; fpT[k][1] = T[1]; }
			fpOk[k] = true;
			float Rh[9], Rk[9];
			solve(k, H, K, A, T, Rh, Rk);
			if (kneel > 0.05f && rearLeg)
			{
				float kv[3] = {K[0] - H[0], K[1] - H[1], K[2] - H[2]}, kn[3]; apply(Rh, kv, kn);
				float kz = H[2] + kn[2], floor = gz[k] + 3;
				if (kz < floor) kneelLift = rt::min(kneelLift + (floor - kz) * 0.5f, TUNE(coverKneelDropUnits) + 10);
			}
			float G[9]; memcpy(G, kId, 36);
			float ng[3] = {nF[k][0], nF[k][1], nF[k][2]};
			if (toeLen[k] > 3 && rt::absf(pitchF[k]) > 0.001f)
			{
				const float *Tp = (const float *)(toe[k] + 0x8C);
				float fd[3] = {Tp[0] - A[0], Tp[1] - A[1], 0}; norm(fd);
				float al = dot(ng, fd), ns[3] = {ng[0] - fd[0] * al, ng[1] - fd[1] * al, ng[2]};
				float tn = rt::sin(pitchF[k]) / rt::max(rt::cos(pitchF[k]), 0.2f) * ns[2];
				for (int i = 0; i < 3; i++) ns[i] -= fd[i] * tn;
				norm(ns); memcpy(ng, ns, 12);
			}
			float ang = acosf(ng[2]) * R2D;
			if (ang > 0.5f)
			{
				float kk = plant[k] * wl * rt::min(1.0f, TUNE(bodyFootMaxDeg) / ang);
				between(up, ng, kk, G);
			}
			if (kneel > 0.001f && rearLeg) { float Gk[9], G2[9]; rotAbout(rr, -TUNE(coverKneelToeDeg) * kneel, Gk); mul3(Gk, G, G2); memcpy(G, G2, 36); }
			float RhRk[9], RT[9], Mf[9];
			mul3(Rh, Rk, RhRk); tr(RhRk, RT); mul3(RT, G, Mf);
			float Xr[9], Xt[3];
			motionLocal(W(ptr(Th.node + 0x18)), Rh, H, 0, Xr, Xt); preMul(Th.node, Xr, Xt);
			motionLocal(Tw, Rk, K, 0, Xr, Xt); preMul(Ca.node, Xr, Xt);
			motionLocal(Cw, Mf, A, 0, Xr, Xt); preMul(Ft.node, Xr, Xt);
			worldDown(Th.node);
		}
		plantPrev[0] = plant[0]; plantPrev[1] = plant[1];
		if (spd < 5 && onGround && n0ok && n0[2] > 0.985f) rest += (clampf(rt::min(hgt[0], hgt[1]), 2, 20) - rest) * clampf(dt * 0.3f, 0, 1);
	}
	u32 camN = handling::sceneCam();
	u32 hb = b[HED].bone;
	if (hb && camN && TUNE(headLookOn) > 0)
	{
		Xf Hw = W(hb);
		if (headAx < 0 && spd < 5 && !aiming)
		{
			float best = 0.6f;
			for (int k = 0; k < 6; k++)
			{
				float sg = (k & 1) ? -1.0f : 1.0f, d = sg * (Hw.r[k / 2] * f[0] + Hw.r[3 + k / 2] * f[1] + Hw.r[6 + k / 2] * f[2]);
				if (d > best) { best = d; headAx = k; }
			}
			if (headAx >= 0) LOG("body: the head looks down its %c%c axis (%.2f with where you face)", (headAx & 1) ? '-' : '+', "xyz"[headAx / 2], best);
		}
		bool vats = rd<u32>(0x11F2250 + 8) != 0;
		float wH = wOn * TUNE(headLookScale) * (aiming ? TUNE(headAiming) : weaponOut ? TUNE(headWeaponOut) : 1) * (vats ? 0 : 1);
		float ty = 0, tp2 = 0;
		if (headAx >= 0)
		{
			float sg = (headAx & 1) ? -1.0f : 1.0f;
			float hf[3] = {sg * Hw.r[headAx / 2], sg * Hw.r[3 + headAx / 2], sg * Hw.r[6 + headAx / 2]};
			const float *cw = (const float *)(camN + 0x68);
			float cf[3] = {cw[0], cw[3], cw[6]};
			auto yawOf = [&](const float *v) { return rt::atan2(dot(v, rr), dot(v, f)); };
			auto pitchOf = [&](const float *v) { return rt::atan2(v[2], rt::sqrt(v[0] * v[0] + v[1] * v[1])); };
			float tgl = glRef && glT > 0 ? 1.0f : 0.0f;
			glW += clampf(tgl - glW, -dt * 2.5f, dt * 2.5f);
			if (glW > 0.001f && glRef)
			{
				TESForm *gf = lookup(glRef);
				TESObjectREFR *ga = gf && (formtype((void *)gf) == 0x3B || formtype((void *)gf) == 0x3C) ? (TESObjectREFR *)gf : 0;
				if (ga && thirdPersonNode(ga) && rd<u32>((u32)ga + OFF_ACTOR_LIFESTATE) == 0)
				{
					const float *q = pos(ga);
					float to[3] = {q[0] - Hw.t[0], q[1] - Hw.t[1], q[2] + 112 - Hw.t[2]};
					norm(to);
					for (int i = 0; i < 3; i++) cf[i] += (to[i] - cf[i]) * glW * lifeK * (weaponOut ? 0.5f : 1.0f);
					norm(cf);
				}
				else glRef = 0;
			}
			float dr = TUNE(lifeHeadDriftDeg) * idleK * lifeK;
			ty = clampf(wrapA(yawOf(cf) - yawOf(hf)) * R2D + (fidYaw + dr * drift(lifeClock, 0.3f)) * lifeK, -TUNE(headYawMaxDeg), TUNE(headYawMaxDeg));
			tp2 = clampf((pitchOf(cf) - pitchOf(hf)) * R2D + (fidPitch + dr * 0.6f * drift(lifeClock * 0.8f, 2.1f)) * lifeK + coverHead, -TUNE(headDownMaxDeg), TUNE(headUpMaxDeg));
		}
		spring(hy, hyV, ty * wH, TUNE(headLookHz), 1, dt);
		spring(hp, hpV, tp2 * wH, TUNE(headLookHz), 1, dt);
		float rlT = (fidRoll + TUNE(lifeHeadTiltDeg)) * lifeK * (aiming ? 0.2f : 1.0f);
		spring(hr, hrV, rlT, TUNE(headLookHz) * 0.6f, 1, dt);
		if (rt::absf(hy) + rt::absf(hp) + rt::absf(hr) > 0.02f)
		{
			float s = b[NCK].node ? clampf(TUNE(headNeckShare), 0, 1) : 0;
			auto turn = [&](float yd, float pd, float rd, float *D) { float Ry[9], Rp[9], Rr[9], r2[3], f2[3], T[9]; rotAbout(up, -yd, Ry); apply(Ry, rr, r2); apply(Ry, f, f2); rotAbout(r2, pd, Rp); mul3(Rp, Ry, T); rotAbout(f2, rd, Rr); mul3(Rr, T, D); };
			float Dt[9], Dn[9]; turn(hy, hp, hr, Dt); turn(hy * s, hp * s, hr * s, Dn);
			float Xr[9], Xt[3];
			if (b[NCK].node)
			{
				const float *np = (const float *)(b[NCK].bone + 0x8C);
				motionLocal(W(ptr(b[NCK].node + 0x18)), Dn, np, 0, Xr, Xt); preMul(b[NCK].node, Xr, Xt);
			}
			float DnT[9], Dh[9]; tr(Dn, DnT); mul3(DnT, Dt, Dh);
			motionLocal(W(ptr(b[HED].node + 0x18)), Dh, Hw.t, 0, Xr, Xt); preMul(b[HED].node, Xr, Xt);
			worldDown(b[NCK].node ? b[NCK].node : b[HED].node);
		}
		lastHeadH = ((const float *)(hb + 0x8C))[2] - (P[2] + bias);
	}
	if ((logT += dt) > 6)
	{
		logT = 0;
		LOG("body: on %.2f ik %.2f - feet' ground %+.1f %+.1f (ankles %.1f %.1f over it, planted %.2f %.2f, rest %.1f), hips %+.1f (stairs %+.1f), lean %.1f bank %.1f torso %.1f, slope %.0f deg (grade %.0f, stairs %.2f, hill lean x%.2f); head %.0f right %.0f up; %d jump(s) blended; free look %.0f deg; normal %s %.2f",
			wOn, wIk, dz[0], dz[1], hgt[0], hgt[1], plant[0], plant[1], rest, drop, stairZ - P[2], lean, bank, spP, slopeDeg, gradeDeg, stairK, hillK, hy, hp, jumps, flYaw * R2D, n0ok ? "known" : "NOT known", n0[2]);
	}
}
}
// The playing-together tick's parts, each timed: every 10 s in the perf line while they cost something, and in a slow
// tick's line part by part
namespace gprof
{
enum { NET, EVENTS, FIGHTS, MIRRORS, DOORS, CHAT, SKY, PROXIES, DOWNED, SCALE, FRIENDS, N };
static const char *kName[N] = {"link", "events", "your fights", "friends' fights", "doors", "chat", "clock/sky", "hit bodies", "downed", "scaling", "friends"};
static double acc[N], mx[N], cur[N], mark;
static void begin() { mark = rt::now(); for (auto &c : cur) c = 0; }
static void lap(int i) { double n = rt::now(), d = n - mark; mark = n; cur[i] += d; acc[i] += d; if (d > mx[i]) mx[i] = d; }
}
// ---- The other players (net.cpp): each drawn where the link says they are - on foot a posed copy of a body (the
namespace ghosts
{
static u32 modHash[256];
static int modCount = -1;
static void modsBuild()
{
	u32 dh = ptr(0x11C3F2C);
	if (!dh) return;
	u32 n = rd<u32>(dh + 0x218);
	if (n > 255) n = 255;
	for (u32 i = 0; i < n; i++) { u32 mi = ptr(dh + 0x21C + 4 * i); modHash[i] = 0; if (mi) net::hashName((const char *)(mi + 0x20), &modHash[i]); }
	modCount = (int)n;
	LOG("ghosts: %d mod files hashed (friends' forms are matched by file name, not load order)", modCount);
}
static u32 modsSum[4];
static int modsState[4];
static const char *modName(int i)
{
	u32 dh = ptr(0x11C3F2C), mi = dh ? ptr(dh + 0x21C + 4 * i) : 0;
	return mi ? (const char *)(mi + 0x20) : "?";
}
static void modsCompare(const char *who, int id)
{
	static u32 theirs[net::kMaxMods];
	int n = net::peerMods(id, theirs, net::kMaxMods);
	if (n <= 0) { modsState[id] = 0; modsSum[id] = 0; return; }
	u32 sum = 2166136261u;
	for (int i = 0; i < n; i++) sum = (sum ^ theirs[i]) * 16777619u;
	if (sum == modsSum[id]) return;
	modsSum[id] = sum;
	int onlyMine = 0, onlyTheirs = 0;
	char names[600] = "";
	for (int i = 0; i < modCount; i++)
	{
		bool found = false;
		for (int k = 0; k < n && !found; k++) found = theirs[k] == modHash[i];
		if (found) continue;
		onlyMine++;
		LOG("mods:   %s has no %s", who, modName(i));
		if (strlen(names) < 400) { if (names[0]) rt::strcat(names, ", ", sizeof names); rt::strcat(names, modName(i), sizeof names); }
	}
	for (int k = 0; k < n; k++)
	{
		bool found = false;
		for (int i = 0; i < modCount && !found; i++) found = theirs[k] == modHash[i];
		if (!found) onlyTheirs++;
	}
	bool sameOrder = n == modCount;
	for (int i = 0; i < n && sameOrder; i++) sameOrder = theirs[i] == modHash[i];
	modsState[id] = !onlyMine && !onlyTheirs ? 1 : 2;
	if (modsState[id] == 1)
	{
		LOG("mods: %s has the same %d mod files as you%s", who, n, sameOrder ? ", in the same order" : " (in a different order)");
		if (!sameOrder) message("Your friend has the same mods, in a different order.", 4);
		return;
	}
	LOG("mods: %s's mods differ - you have %d they don't, they have %d you don't (their game names those)", who, onlyMine, onlyTheirs);
	char m[700];
	if (onlyMine) rt::fmt(m, sizeof m, "Mods differ from %s's: you have %s%s%s", who, names, onlyTheirs ? "; they have more you don't" : "", "");
	else rt::fmt(m, sizeof m, "Mods differ from %s's: they have %d you don't (their screen names them).", who, onlyTheirs);
	message(m, 6);
}
int modsMatch(int id) { return id >= 0 && id < 4 ? modsState[id] : 0; }
static net::FormKey formKey(u32 id)
{
	net::FormKey k = {0, 0};
	if (modCount < 0) modsBuild();
	u32 mi = id >> 24;
	if (!id || mi == 0xFF || (int)mi >= modCount) return k;
	k.low = id & 0xFFFFFF; k.modHash = modHash[mi];
	return k;
}
static TESForm *formOf(const net::FormKey &k)
{
	if (!k.low) return 0;
	if (modCount < 0) modsBuild();
	for (int i = 0; i < modCount; i++) if (modHash[i] == k.modHash) return lookup(((u32)i << 24) | k.low);
	return 0;
}

static u16 boneHash(const char *nm) { u32 h; net::hashName(nm, &h); return (u16)(h ^ (h >> 16)); }
static bool isBoneName(const char *nm)
{
	return nm && ((nm[0] == 'B' && nm[1] == 'i' && nm[2] == 'p' && nm[3] == '0') || !rt::strcmp(nm, "Weapon"));
}
struct BoneList { int n; u32 node[net::kPoseBones]; u16 hash[net::kPoseBones]; u8 depth[net::kPoseBones]; int named[net::kPoseBones]; };
static void collectBones(u32 root, BoneList &L)
{
	L.n = 0;
	u32 q[512]; int qp[512]; u8 qd[512];
	int head = 0, tail = 0;
	q[tail] = root; qp[tail] = -1; qd[tail] = 0; tail++;
	while (head < tail && L.n < net::kPoseBones)
	{
		u32 n = q[head]; int par = qp[head]; u8 d = qd[head]; head++;
		const char *nm = n != root ? (const char *)ptr(n + 8) : 0;
		int me = par;
		if (n != root && isBoneName(nm))
		{
			u16 h = boneHash(nm);
			bool dup = false;
			for (int i = 0; i < L.n; i++) if (L.hash[i] == h) dup = true;
			if (!dup)
			{
				me = L.n;
				L.node[L.n] = n; L.hash[L.n] = h; L.depth[L.n] = d; L.named[L.n] = par; L.n++;
			}
			if (!rt::strcmp(nm, "Weapon")) continue;
		}
		if (!handling::isNodeObj(n)) continue;
		u32 arr = ptr(n + OFF_NODE_CHILDREN);
		u16 c = rd<u16>(n + OFF_NODE_CHILDCOUNT);
		u8 nd = (u8)(d + (n != root && isBoneName(nm) ? 1 : 0));
		for (u16 j = 0; arr && j < c && j < 128 && tail < 512; j++) { u32 ch = ptr(arr + 4 * j); if (ch) { q[tail] = ch; qp[tail] = me; qd[tail] = nd; tail++; } }
	}
}
using handling::Xf; using handling::xfRead; using handling::xfMul; using handling::xfInv;
static Xf xfId() { Xf x; float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(x.r, I, 36); x.t[0] = x.t[1] = x.t[2] = 0; x.s = 1; return x; }
static void xfWriteLocal(u32 n, const Xf &x) { memcpy((void *)(n + 0x34), x.r, 36); memcpy((void *)(n + 0x58), x.t, 12); *(float *)(n + 0x64) = x.s; }
static void xfApply(const Xf &M, const float *v, float *o) { for (int i = 0; i < 3; i++) o[i] = M.s * (M.r[i * 3] * v[0] + M.r[i * 3 + 1] * v[1] + M.r[i * 3 + 2] * v[2]) + M.t[i]; }
static bool fromRoot(u32 node, u32 root, Xf &M, const BoneList *L = 0, const Xf *done = 0, const bool *have = 0)
{
	u32 chain[40];
	int nc = 0;
	u32 x = node;
	M = xfId();
	while (x && x != root && nc < 40)
	{
		if (L)
		{
			int bi = -1;
			for (int i = 0; i < L->n; i++) if (have[i] && L->node[i] == x) { bi = i; break; }
			if (bi >= 0) { M = done[bi]; break; }
		}
		chain[nc++] = x;
		x = ptr(x + 0x18);
	}
	if (!x || nc >= 40) return false;
	for (int k = nc - 1; k >= 0; k--) M = xfMul(M, xfRead(chain[k], 0x34));
	return true;
}
static bool buildPose(u32 n3, net::Pose &P, net::Skel *sk, const Xf *frame = 0)
{
	if (!n3) return false;
	static BoneList L;
	collectBones(n3, L);
	static Xf M[net::kPoseBones];
	P.n = 0;
	if (sk) sk->n = 0;
	int nt = 0;
	Xf Fi = frame ? xfInv(*frame) : xfId();
	for (int i = 0; i < L.n; i++)
	{
		if (frame) M[i] = xfMul(Fi, xfRead(L.node[i], 0x68));
		else if (!fromRoot(L.node[i], n3, M[i])) { M[i] = xfId(); continue; }
		int k = P.n++;
		P.hash[k] = L.hash[i];
		move::q4(M[i].r, P.q[k]);
		P.hasT[k] = L.depth[i] <= 4 && nt < 8;
		if (P.hasT[k]) { memcpy(P.t[k], M[i].t, 12); nt++; }
		if (sk)
		{
			Xf Pm = L.named[i] >= 0 ? M[L.named[i]] : xfId(), Pi = xfInv(Pm);
			int j = sk->n++;
			sk->hash[j] = L.hash[i];
			xfApply(Pi, M[i].t, sk->t[j]);
		}
	}
	return P.n > 0;
}
static void applyPose(u32 R, BoneList &L, const net::Pose &P, const net::Skel *sk)
{
	for (int i = 0; i < P.n; i++)
	{
		bool ok = true;
		for (int k = 0; k < 4; k++) ok = ok && P.q[i][k] == P.q[i][k] && P.q[i][k] < 2 && P.q[i][k] > -2;
		if (P.hasT[i]) for (int k = 0; k < 3; k++) ok = ok && P.t[i][k] == P.t[i][k] && P.t[i][k] < 1e6f && P.t[i][k] > -1e6f;
		if (!ok) { static int told; if (told++ < 10) LOG("ghosts: a friend's pose with a bad number in it (bone %d) - not drawn", i); return; }
	}
	static Xf done[net::kPoseBones];
	static bool have[net::kPoseBones];
	memset(have, 0, sizeof have);
	int hint = 0;
	for (int i = 0; i < P.n; i++)
	{
		int bi = -1;
		if (hint < L.n && L.hash[hint] == P.hash[i]) bi = hint;
		for (int k = 0; bi < 0 && k < L.n; k++) if (L.hash[k] == P.hash[i]) bi = k;
		if (bi < 0) continue;
		hint = bi + 1;
		u32 b = L.node[bi];
		Xf Mp;
		if (!fromRoot(ptr(b + 0x18), R, Mp, &L, done, have)) continue;
		float Rt[9], MpT[9];
		move::m9(P.q[i], Rt);
		move::tr(Mp.r, MpT);
		Xf Lc = xfRead(b, 0x34);
		handling::mul3(MpT, Rt, Lc.r);
		const float *want = 0;
		float w[3];
		if (P.hasT[i]) want = P.t[i];
		else if (sk)
		{
			int si = -1;
			if (i < sk->n && sk->hash[i] == P.hash[i]) si = i;
			for (int k = 0; si < 0 && k < sk->n; k++) if (sk->hash[k] == P.hash[i]) si = k;
			if (si >= 0)
			{
				int np = L.named[bi];
				Xf Mn = np >= 0 && have[np] ? done[np] : (np < 0 ? xfId() : Mp);
				xfApply(Mn, sk->t[si], w);
				want = w;
			}
		}
		if (want)
		{
			float d[3] = {want[0] - Mp.t[0], want[1] - Mp.t[1], want[2] - Mp.t[2]};
			move::apply(MpT, d, Lc.t);
			for (int k = 0; k < 3; k++) Lc.t[k] /= Mp.s;
		}
		memcpy((void *)(b + 0x34), Lc.r, 36);
		memcpy((void *)(b + 0x58), Lc.t, 12);
		done[bi] = xfMul(Mp, Lc); have[bi] = true;
	}
}

static u32 markRef, markCell;
static void shotSound(TESForm *weap, const float *at)
{
	u32 sn = weap ? ptr((u32)weap + 0x21C) : 0;
	if (!sn || formtype((void *)sn) != 0x0D || !s_shot) return;
	u32 cell = ptr((u32)player() + OFF_REF_CELL);
	TESObjectREFR *m = markRef ? (TESObjectREFR *)lookup(markRef) : 0;
	if (!m || markCell != cell)
	{
		if (m) removeRef(m);
		TESForm *xm = lookup(0x3B);
		static int okMarker = -1;
		if (okMarker < 0)
		{
			const char *mp = xm && formtype(xm) == 0x20 ? (const char *)ptr((u32)xm + 0x34) : 0;
			okMarker = 0;
			for (const char *c = mp; c && *c && !okMarker; c++) if ((c[0] | 32) == 'm' && (c[1] | 32) == 'a' && (c[2] | 32) == 'r' && (c[3] | 32) == 'k') okMarker = 1;
			LOG("ghosts: friends' shots heard from a marker (%s): %s", mp ? mp : "no model", okMarker ? "yes" : "no - shots stay silent");
		}
		m = xm && okMarker ? placeForm(xm, at[0], at[1], at[2], 0) : 0;
		markRef = m ? refid(m) : 0; markCell = cell;
		if (!m) return;
	}
	g_script->CallFunctionAlt(s_shot, player(), 5, m, (TESForm *)sn, F(at[0]), F(at[1]), F(at[2]));
}

static u32 __cdecl loadThunk(u32 path, u32)
{
	u32 ml = ptr(0x11C3B3C);
	if (!ml) return 0;
	return ((u32(__thiscall *)(u32, const char *, u32, u32, u32, u32, u32))0x447080)(ml, (const char *)path, 0, 1, 0, 0, 1);
}
static u32 loadModel(const char *path)
{
	g_gThread = GetCurrentThreadId();
	u32 n = guardedCall2((u32)&loadThunk, (u32)path, 0);
	if (g_gFault) { LOG("ghosts: loading %s faulted (at %08x) - no gun model", path, g_gFaultEip); return 0; }
	return n;
}

static Xf s_weapLocal; static int s_weapKnown = -1;
static bool weaponDefault()
{
	if (s_weapKnown >= 0) return s_weapKnown > 0;
	s_weapKnown = 0;
	u32 sk = loadModel("Characters\\_Male\\skeleton.nif");
	u32 w = sk ? rider::findByName(sk, "Weapon") : 0, wp = w ? ptr(w + 0x18) : 0;
	const char *pn = wp ? (const char *)ptr(wp + 8) : 0;
	if (w && pn && !rt::strcmp(pn, "Bip01 R Hand")) { s_weapLocal = xfRead(w, 0x34); s_weapKnown = 1; }
	LOG("ghosts: the hand's Weapon place from the skeleton: %s (%.1f %.1f %.1f)", s_weapKnown ? "found" : pn ? pn : "not found",
		s_weapLocal.t[0], s_weapLocal.t[1], s_weapLocal.t[2]);
	return s_weapKnown > 0;
}
static u32 weaponToHand(u32 root)
{
	u32 hand = root ? rider::findByName(root, "Bip01 R Hand") : 0, w = 0;
	if (!hand) return 0;
	static BoneList L; collectBones(root, L);
	for (int i = 0; i < L.n && !w; i++) if (!rt::strcmp((const char *)ptr(L.node[i] + 8), "Weapon")) w = L.node[i];
	if (!w || !weaponDefault()) return w;
	u32 par = ptr(w + 0x18);
	if (par != hand)
	{
		rider::hold(w);
		if (par) rider::detach(par, w);
		rider::attach(hand, w);
		rider::release(w);
	}
	xfWriteLocal(w, s_weapLocal);
	return w;
}
struct Fx { u32 flash, flashPar, flashProj; double offAt; };
static void fxOff(Fx &f) { if (f.flash && f.flashPar) { rider::detach(f.flashPar, f.flash); rider::release(f.flash); } memset(&f, 0, sizeof f); }
static void fxTick(Fx &f) { if (f.flash && f.offAt > 0 && rt::now() > f.offAt) { *(u32 *)(f.flash + OFF_NIAV_FLAGS) |= 1u; f.offAt = 0; } }
static void shotFx(int slot, u32 gun, TESForm *w, const float *eye, const float *dir, Fx &fx)
{
	u32 pn = gun ? findNamed(gun, "ProjectileNode", 8) : 0, mn = pn ? pn : gun;
	float mp[3];
	if (mn) memcpy(mp, (const void *)(mn + 0x8C), 12); else memcpy(mp, eye, 12);
	handling::muzzleFlashAt(slot, mp);
	u32 proj = handling::weapProj((u32)w);
	float sec; const char *fp = handling::projFlash(proj, &sec);
	if (fp && pn)
	{
		if (fx.flashProj != proj || fx.flashPar != pn)
		{
			fxOff(fx);
			u32 shared = loadModel(fp);
			u32 c = shared ? rider::copy(shared, false) : 0;
			if (c)
			{
				rider::hold(c); rider::attach(pn, c);
				fx.flash = c; fx.flashPar = pn; fx.flashProj = proj;
				static int told; if (told++ < 4) LOG("ghosts: a friend's muzzle flash %s", fp);
			}
			else fx.flashProj = proj;
		}
		if (fx.flash) { *(u32 *)(fx.flash + OFF_NIAV_FLAGS) &= ~1u; fx.offAt = rt::now() + sec; }
	}
	float far = 8000, E[3] = {eye[0] + dir[0] * far, eye[1] + dir[1] * far, eye[2] + dir[2] * far}, f = 1; u32 o = 0;
	if (rayHitObject(eye, E, &f, &o)) for (int i = 0; i < 3; i++) E[i] = eye[i] + dir[i] * far * f;
	handling::addTracer(mp, E);
}
static void aimDir(float yaw, float pitch, float *d) { float cp = rt::cos(pitch); d[0] = rt::sin(yaw) * cp; d[1] = rt::cos(yaw) * cp; d[2] = -rt::sin(pitch); }
struct Vis
{
	rider::Body body;
	u32 holder, parent;
	bool shown, made, seen;
	TESObjectREFR *carRef;
	int carIdx;
	float phase, eyeH;
	char name[24];
	u32 carCell;
	TESObjectREFR *oldCar;
	int oldWait;
	float carAt[6];
	BoneList bones; bool bonesFound;
	int shotsDue; double nextShot;
	u32 gun, gunBone;
	u32 weap;
	Fx fx;
	net::FormKey gunKey;
	u16 shots; bool shotsKnown;
	double inviteTold;
	u32 lookSum;
	double here;
	Vehicle cv; int cvCar; bool cvRested;
	Xf carW; bool carWOk;
	Xf gunOrig;
};
static Vis vis[4];
static float sinceHour;
static bool netInited;
static net::Self lastMe;
static net::Ghost s_gs[4];
static int s_n;
static void hideAll();
void netStopCheck()
{
	static double t;
	if (!g_dir[0] || rt::now() - t < 0.5) return;
	t = rt::now();
	char p[400];
	rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\net.stop", g_dir);
	if (GetFileAttributesA(p) == 0xFFFFFFFFu) return;
	u32 n = 0;
	char *b = rt::readfile(p, &n);
	bool panic = b && n >= 5 && !memcmp(b, "panic", 5);
	rt::free(b);
	DeleteFileA(p);
	bool was = net::active();
	if (net::soloTesting()) net::soloTest(false);
	if (was) net::stopNow(panic ? "the panic key" : "setup: stop hosting / leave");
	if (netInited) hideAll();
	char a[400];
	rt::fmt(a, sizeof a, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\net.stopped", g_dir);
	rt::writefile(a, "stopped\r\n", 9, false);
	if (was) message(panic ? "Panic key: playing together is off - everyone dropped, nobody can get in." : "Playing together is off (setup).", 6);
	LOG("net.stop (%s): %s", panic ? "panic" : "setup", was ? "playing together off" : "nothing was on");
}
static void askTick(bool focus)
{
	bool yes = pressed(VK_JOINACCEPT), no = pressed(VK_JOINREFUSE);
	char who[24]; u32 build = 0;
	if (!net::hosting() || !net::asking(who, sizeof who, &build, 0, 0)) return;
	static double shownT; static char shownWho[24];
	if (focus && (yes || no))
	{
		net::answer(yes);
		char m[120];
		rt::fmt(m, sizeof m, yes ? "%s is coming in." : "%s was refused (for this session).", who);
		message(m, 3);
		shownT = 0;
		return;
	}
	if (rt::now() - shownT > 4 || !rt::streq(shownWho, who))
	{
		shownT = rt::now(); rt::strcpy(shownWho, who, sizeof shownWho);
		char m[160];
		rt::fmt(m, sizeof m, "%s wants to join your game%s - F6 let them in, F7 refuse", who, build && build != kPluginBuild ? " (another plugin build)" : "");
		message(m, 4.5f);
	}
}
static void pump()
{
	if (netInited && net::active()) net::tick(rt::now(), lastMe);
}

static bool riding;
static void gunOff(Vis &v)
{
	fxOff(v.fx);
	if (v.gun && v.gunBone) rider::detach(v.gunBone, v.gun);
	v.gun = v.gunBone = 0; v.gunKey.low = v.gunKey.modHash = 0;
}
static void hideBody(Vis &v)
{
	if (v.holder && v.shown) *(u32 *)(v.holder + OFF_NIAV_FLAGS) |= 1u;
	v.shown = false;
}
static void carOff(Vis &v)
{
	if (v.carRef) { removeRef(v.carRef); v.carRef = 0; v.carIdx = -1; }
	if (v.oldCar) { removeRef(v.oldCar); v.oldCar = 0; }
}
static void hide(Vis &v) { hideBody(v); carOff(v); }
static void hideAll() { for (auto &v : vis) hide(v); }
static void destroy(Vis &v)
{
	hide(v);
	gunOff(v);
	if (v.holder)
	{
		if (v.parent) { rider::detach(v.parent, v.holder); rider::release(v.parent); }
		if (v.body.root) { rider::detach(v.holder, v.body.root); rider::release(v.body.root); }
		rider::release(v.holder);
	}
	v.holder = v.parent = 0; v.body.root = 0; v.made = false; v.bonesFound = false; v.weap = 0;
}
static bool skinnedGeom(u32 o)
{
	if (!o || handling::isNodeObj(o) || !readable(o, 0xC0)) return false;
	u32 si = ptr(o + 0xBC);
	if (!si || !readable(si, 0x18)) return false;
	u32 bl = ptr(si + 0x14);
	if (!bl || !readable(bl, 4)) return false;
	u32 b0 = ptr(bl);
	return b0 && readable(b0, 0xA8) && handling::isNodeObj(b0);
}
static bool hasSkinned(u32 n, int depth)
{
	if (skinnedGeom(n)) return true;
	if (!n || depth > 24 || !readable(n, 0xA8) || !handling::isNodeObj(n)) return false;
	u32 arr = ptr(n + OFF_NODE_CHILDREN); u16 cnt = rd<u16>(n + OFF_NODE_CHILDCOUNT);
	for (int i = 0; arr && i < cnt; i++) if (hasSkinned(ptr(arr + 4 * i), depth + 1)) return true;
	return false;
}
static int pruneButSkin(u32 n, int depth)
{
	if (!n || depth > 24 || !readable(n, 0xA8) || !handling::isNodeObj(n)) return 0;
	u32 arr = ptr(n + OFF_NODE_CHILDREN); u16 cnt = rd<u16>(n + OFF_NODE_CHILDCOUNT);
	int gone = 0;
	for (int i = (int)cnt - 1; arr && i >= 0; i--)
	{
		u32 c = ptr(arr + 4 * i);
		if (!c || skinnedGeom(c)) continue;
		if (handling::isNodeObj(c) && hasSkinned(c, depth + 1)) { gone += pruneButSkin(c, depth + 1); continue; }
		rider::detach(n, c); gone++;
	}
	return gone;
}
static int prunePipBoy(u32 n, int depth)
{
	if (!n || depth > 24 || !readable(n, 0xA8) || !handling::isNodeObj(n)) return 0;
	u32 arr = ptr(n + OFF_NODE_CHILDREN); u16 cnt = rd<u16>(n + OFF_NODE_CHILDCOUNT);
	int gone = 0;
	for (int i = (int)cnt - 1; arr && i >= 0; i--)
	{
		u32 c = ptr(arr + 4 * i);
		if (!c) continue;
		const char *nm = (const char *)ptr(c + 8);
		if (nm && readable((u32)nm, 6) && stristr(nm, "pipboy"))
		{
			if (skinnedGeom(c)) continue;
			if (handling::isNodeObj(c) && hasSkinned(c, depth + 1)) { gone += pruneButSkin(c, depth + 1); continue; }
			rider::detach(n, c); gone++; continue;
		}
		gone += prunePipBoy(c, depth + 1);
	}
	return gone;
}
static bool makeFrom(Vis &v, TESObjectREFR *pl, u32 src, const char *what)
{
	u32 n3 = thirdPersonNode(pl), par = n3 ? ptr(n3 + 0x18) : 0;
	if (!src || !par || riding) return false;
	double t0 = rt::now();
	u32 c = rider::copy(src);
	if (!c) return false;
	*(u32 *)(c + OFF_NIAV_FLAGS) &= ~1u;
	int pb = prunePipBoy(c, 0);
	if (pb) LOG("ghosts: %d Pip-Boy parts taken off the body copy", pb);
	rider::hold(c);
	if (!rider::bind(v.body, c)) { rider::release(c); v.body.root = 0; return false; }
	v.holder = ((u32(__cdecl *)())0xA5F030)();
	if (!v.holder) { rider::release(c); v.body.root = 0; return false; }
	rider::hold(v.holder);
	rider::attach(v.holder, c);
	v.parent = par; rider::hold(par);
	*(u32 *)(v.holder + OFF_NIAV_FLAGS) |= 1u;
	rider::attach(par, v.holder);
	v.made = true; v.shown = false; v.eyeH = 120; v.bonesFound = false;
	v.weap = weaponToHand(c);
	LOG("ghosts: a body made in %.0f ms from %s (its gun hand %s)", (rt::now() - t0) * 1000, what, v.weap ? "ready" : "NOT found");
	return true;
}
static bool make(Vis &v, TESObjectREFR *pl)
{
	u32 n3 = thirdPersonNode(pl);
	if (!n3 || riding) return false;
	neutral3rd();
	return makeFrom(v, pl, n3, "a copy of you");
}
static void bodyOff(Vis &v)
{
	hideBody(v);
	gunOff(v);
	if (v.holder)
	{
		if (v.parent) { rider::detach(v.parent, v.holder); rider::release(v.parent); }
		if (v.body.root) { rider::detach(v.holder, v.body.root); rider::release(v.body.root); }
		rider::release(v.holder);
	}
	v.holder = v.parent = 0; v.body.root = 0; v.made = false; v.bonesFound = false; v.weap = 0;
}

#pragma pack(push, 1)
struct LookPk
{
	u32 sum;
	net::FormKey race, hair, eyes;
	u8 female, nParts, nArmor, nFace;
	float hairLen, height;
	u32 hairColor;
	net::FormKey parts[4];
	net::FormKey armor[12];
	float face[130];
};
#pragma pack(pop)
static const int kFaceN[3] = {50, 30, 50};
static u32 lookSum(const LookPk &L) { u32 h = 2166136261u; const u8 *b = (const u8 *)&L + 4; for (u32 i = 0; i < sizeof L - 4; i++) h = (h ^ b[i]) * 16777619u; return h ? h : 1; }
static float *faceVals(u32 npc, int k) { u32 v = ptr(npc + 0x134 + 0x20 * k + 0xC); return v && readable(v, kFaceN[k] * 4) ? (float *)v : 0; }
static void lookBuild(TESObjectREFR *pl, LookPk &L)
{
	memset(&L, 0, sizeof L);
	u32 npc = ptr((u32)pl + OFF_REF_BASE);
	if (!npc || formtype((void *)npc) != 0x2A) return;
	auto key = [](u32 f) { return f ? formKey(refid((void *)f)) : net::FormKey{0, 0}; };
	L.race = key(ptr(npc + 0x110));
	L.female = rd<u8>(npc + 0x34) & 1;
	L.hair = key(ptr(npc + 0x1B8)); L.hairLen = rd<float>(npc + 0x1BC);
	L.eyes = key(ptr(npc + 0x1C0));
	L.hairColor = rd<u32>(npc + 0x1D8);
	L.height = rd<float>(npc + 0x1F4);
	for (u32 n = npc + 0x1DC, i = 0; n && i < 16 && L.nParts < 4; n = ptr(n + 4), i++)
		if (u32 hp = ptr(n)) { net::FormKey k = key(hp); if (k.low) L.parts[L.nParts++] = k; }
	int at = 0;
	for (int k = 0; k < 3; k++) { if (const float *f = faceVals(npc, k)) memcpy(L.face + at, f, kFaceN[k] * 4); at += kFaceN[k]; }
	L.nFace = 130;
	u32 bip = ptr((u32)pl + 0x1B4);
	if (bip && readable(bip, 0x16C))
		for (int i = 0; i < 20 && L.nArmor < 12; i++)
		{
			u32 it = ptr(bip + 0x2C + 0x10 * i);
			if (!it || formtype((void *)it) != 0x18) continue;
			net::FormKey k = key(it);
			bool dup = !k.low;
			for (int j = 0; j < L.nArmor && !dup; j++) dup = L.armor[j].low == k.low && L.armor[j].modHash == k.modHash;
			if (!dup) L.armor[L.nArmor++] = k;
		}
	L.sum = lookSum(L);
}
static TESForm *dressBase;
static bool dressNoClone;
static bool proxyNoClone;
struct Dressing { int state, slot; u32 sum; TESObjectREFR *ref; double t0, built; };
static Dressing dr;
static bool dressing() { return dr.state != 0; }
static bool canDress() { return !dressNoClone && s_strip && s_dressItem; }
static u32 __cdecl cloneNpcThunk(u32 src, u32)
{
	u32 f = ((u32(__cdecl *)(u32))0x465110)(0x2A);
	if (!f) return 0;
	void **vt = *(void ***)f;
	((void(__thiscall *)(u32, u32))vt[0x108 / 4])(f, src);
	memcpy((void *)(f + 0x24), (const void *)(src + 0x24), 12);
	u32 dh = ptr(0x11C3F2C), id = dh ? ((u32(__thiscall *)(u32))0x469800)(dh) : 0;
	if (id) ((void(__thiscall *)(u32, u32, bool))vt[0x128 / 4])(f, id, true);
	((void(__thiscall *)(u32))0x484490)(f);
	return f;
}
static void dressStop(const char *why)
{
	if (dr.ref) removeRef(dr.ref);
	if (why) LOG("ghosts: the stand-in for %s removed (%s)", vis[dr.slot].name, why);
	memset(&dr, 0, sizeof dr);
}
static bool dressLook(const LookPk &L)
{
	TESObjectREFR *pl = player();
	u32 me = pl ? ptr((u32)pl + OFF_REF_BASE) : 0;
	if (!dressBase)
	{
		if (!me || formtype((void *)me) != 0x2A) return false;
		g_gThread = GetCurrentThreadId();
		u32 c = guardedCall2((u32)&cloneNpcThunk, me, 0);
		if (g_gFault) { LOG("ghosts: copying your record faulted (at %08x) - friends stay copies of you", g_gFaultEip); c = 0; }
		dressBase = c ? (TESForm *)c : 0;
		if (!dressBase || formtype(dressBase) != 0x2A) { dressBase = 0; dressNoClone = true; LOG("ghosts: no copy of your record could be made - friends stay copies of you"); return false; }
		u32 b = (u32)dressBase;
		bool shared = false;
		for (int k = 0; k < 3; k++) { u32 a = ptr(b + 0x134 + 0x20 * k + 0xC), m = me ? ptr(me + 0x134 + 0x20 * k + 0xC) : 0; shared = shared || !a || a == m; }
		if (shared) { dressBase = 0; dressNoClone = true; LOG("ghosts: the copy of your record shares your face's data - not used, friends stay copies of you"); return false; }
		LOG("ghosts: a copy of your record made for friends' stand-ins (%08x)", refid(dressBase));
	}
	u32 b = (u32)dressBase;
	TESForm *race = formOf(L.race), *hair = formOf(L.hair), *eyes = formOf(L.eyes);
	if (race && formtype(race) == 0x0C) *(u32 *)(b + 0x110) = (u32)race;
	bool fem = (rd<u8>(b + 0x34) & 1) != 0;
	if (fem != (L.female != 0)) ((void(__thiscall *)(u32, u32, u32, u32))0x47DD50)(b + 0x30, 1, L.female ? 1 : 0, 1);
	if (hair && formtype(hair) == 0x0A) *(u32 *)(b + 0x1B8) = (u32)hair;
	if (L.hairLen >= 0 && L.hairLen <= 10) *(float *)(b + 0x1BC) = L.hairLen;
	if (eyes && formtype(eyes) == 0x0B) *(u32 *)(b + 0x1C0) = (u32)eyes;
	*(u32 *)(b + 0x1D8) = L.hairColor;
	if (L.height > 0.5f && L.height < 2) *(float *)(b + 0x1F4) = L.height;
	*(u32 *)(b + 0x1F0) = 0;
	if (L.nFace == 130) { int at = 0; for (int k = 0; k < 3; k++) { if (float *f = faceVals(b, k)) memcpy(f, L.face + at, kFaceN[k] * 4); at += kFaceN[k]; } }
	u32 parts[4]; int np = 0;
	for (int i = 0; i < L.nParts && i < 4; i++) { TESForm *f = formOf(L.parts[i]); if (f && formtype(f) == 0x09) parts[np++] = (u32)f; }
	*(u32 *)(b + 0x1DC) = np ? parts[0] : 0;
	u32 next = 0;
	for (int i = np - 1; i >= 1; i--)
	{
		u32 n = ((u32(__thiscall *)(u32, u32))0xAA3E40)(0x11F6238, 8);
		if (!n) break;
		*(u32 *)n = parts[i]; *(u32 *)(n + 4) = next; next = n;
	}
	*(u32 *)(b + 0x1E0) = next;
	return true;
}
static u32 __cdecl placeNativeThunk(u32 base, u32)
{
	return ((u32(__cdecl *)(u32, u32, u32, u32, u32, float))0x5C4B30)((u32)player(), base, 1, 0, 0, 1.0f);
}
static TESObjectREFR *placeNative(TESForm *base, float x, float y, float z)
{
	if (!base || !player()) return 0;
	g_gThread = GetCurrentThreadId();
	u32 r = guardedCall2((u32)&placeNativeThunk, (u32)base, 0);
	if (g_gFault) { LOG("ghosts: placing %08x natively faulted (at %08x)", refid(base), g_gFaultEip); return 0; }
	TESObjectREFR *ref = r ? (TESObjectREFR *)r : 0;
	if (ref) { float v[3] = {x, y, z}; ((void(__thiscall *)(u32, const float *))0x575830)((u32)ref, v); }
	return ref;
}
static void dressStart(int slot, const LookPk &L, TESObjectREFR *pl)
{
	memset(&dr, 0, sizeof dr);
	dr.slot = slot; dr.sum = L.sum;
	if (!dressLook(L)) return;
	const float *p = pos(pl);
	dr.ref = placeNative(dressBase, p[0], p[1], p[2] + 3000);
	if (!dr.ref)
	{
		u32 b = (u32)dressBase;
		LOG("ghosts: the stand-in for %s couldn't be placed (the copy %08x: type %x, flags %08x, race %08x, in the form map %d)", vis[slot].name,
			refid(dressBase), formtype(dressBase), rd<u32>(b + 8), ptr(b + 0x110) ? refid((void *)ptr(b + 0x110)) : 0, lookup(refid(dressBase)) == dressBase);
		vis[slot].lookSum = L.sum;
		return;
	}
	g_script->CallFunctionAlt(s_strip, player(), 1, dr.ref);
	int worn = 0;
	for (int i = 0; i < L.nArmor && i < 12; i++)
	{
		TESForm *a = formOf(L.armor[i]);
		if (a && formtype(a) == 0x18) { g_script->CallFunctionAlt(s_dressItem, player(), 2, dr.ref, a); worn++; }
	}
	dr.state = 1; dr.t0 = rt::now();
	LOG("ghosts: dressing a stand-in as %s (%s, %d of %d worn things found here)", vis[slot].name, L.female ? "female" : "male", worn, (int)L.nArmor);
}
static void dressTick(TESObjectREFR *pl)
{
	if (!dr.state) return;
	Vis &v = vis[dr.slot];
	if (!v.seen) { dressStop("they left"); return; }
	if ((rd<u32>((u32)dr.ref + 8) & 0x20)) { dr.ref = 0; dressStop("it was deleted"); return; }
	u32 n = thirdPersonNode(dr.ref);
	if (n) *(u32 *)(n + OFF_NIAV_FLAGS) |= 1u;
	double now = rt::now();
	if (n && !dr.built) dr.built = now;
	if (n && now - dr.built > 1.2)
	{
		if (riding) return;
		bodyOff(v);
		bool ok = makeFrom(v, pl, n, "a stand-in dressed as them");
		v.lookSum = dr.sum;
		dressStop(ok ? 0 : "its body couldn't be copied");
		if (ok) LOG("ghosts: %s is drawn as themselves", v.name);
		return;
	}
	if (now - dr.t0 > 8) { v.lookSum = dr.sum; dressStop("its 3D never came"); }
}
static TESForm *proxyBase[4];
static char proxyName[4][24];
struct Proxy { TESObjectREFR *ref; double made; bool set; float hp; float at[3]; double hpT; double goneT, unwantedT; };
static Proxy proxies[4];
static double myAttackT;
static int lastShots;
static void proxyOff(Proxy &x) { if (x.ref) removeRef(x.ref); bool had = x.ref != 0; double g = x.goneT; memset(&x, 0, sizeof x); x.goneT = had ? rt::now() : g; }
static void proxiesOff() { for (auto &x : proxies) proxyOff(x); }
static float proxyHp(TESObjectREFR *r)
{
	NVSEElement e; memset(&e, 0, sizeof e);
	if (!s_proxyHp || !g_script->CallFunction(s_proxyHp, player(), 0, &e, 1, r) || e.type != 1) return -1;
	return (float)e.num;
}
static u8 hostRules();
static bool refGone(u32 id);
static void downedTick(TESObjectREFR *pl, bool focus);
float downFacing();
static void hurtSend(int slot, float amount, bool fromYou, const char *who)
{
	net::EvHurt h; h.to = (u8)slot; h.fromPlayer = fromYou ? 1 : 0; h.amount = amount;
	if (slot == 3 && net::soloTesting()) { LOG("hits: your echo hit for %.0f (%s) - the solo test: you take it", amount, fromYou ? "by you" : "by the world"); void hurtMe(float amount, bool fromPlayer, const char *who); hurtMe(amount, fromYou, "your 2-s-ago self"); return; }
	net::addEvent(net::EV_HURT, &h, sizeof h);
	LOG("hits: %s hit for %.0f (%s)", who, amount, fromYou ? "by you - friendly fire" : "by your game's people");
}
static u8 hostRules()
{
	if (net::hosting() || !net::active()) return g_friendlyFire ? 1 : 0;
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	for (int i = 0; i < n; i++) if (gs[i].id == 0) return gs[i].s.rules;
	return g_friendlyFire ? 1 : 0;
}
static void proxiesTick(TESObjectREFR *pl)
{
	if (handling::shotsTotal != lastShots)
	{
		lastShots = handling::shotsTotal; myAttackT = rt::now();
		static int told;
		bool any = false; for (auto &x : proxies) any = any || x.ref;
		if (any && told < 25 && s_crossRef)
		{
			told++;
			NVSEElement r; memset(&r, 0, sizeof r);
			g_script->CallFunction(s_crossRef, player(), 0, &r, 0);
			TESObjectREFR *cr = r.type == 2 ? (TESObjectREFR *)r.form : 0;
			char m[300]; int k = rt::fmt(m, sizeof m, "hits: you fired - under the crosshair %08x;", cr ? refid(cr) : 0);
			const float *pp = pos(pl);
			for (auto &x : proxies) if (x.ref && x.set && !refGone(refid(x.ref)) && k < 260)
			{
				const float *q = pos(x.ref);
				float dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2];
				k += rt::fmt(m + k, sizeof m - k, " body %08x %.0f units away, health %.0f%s;", refid(x.ref), rt::sqrt(dx * dx + dy * dy + dz * dz), proxyHp(x.ref), cr == x.ref ? " (aimed at)" : "");
			}
			LOG("%s", m);
		}
	}
	if (held(0x01) && gameFocused() && !menumode()) myAttackT = rt::now();
	if (!s_proxySet || !s_proxyHp || proxyNoClone) return;
	if (rd<u32>(0x11F2250 + 8)) return;
	net::Ghost gs[4];
	int n = net::ghosts(gs, 4);
	bool want[4] = {false, false, false, false};
	float gp[4][3]; static char pname[4][24];
	u32 myWorld = worldspace(), myCell = interior() ? refid((void *)ptr((u32)pl + OFF_REF_CELL)) : 0;
	const float *pp = pos(pl);
	for (int i = 0; i < n; i++)
	{
		const net::Ghost &g = gs[i];
		int slot = g.id < 4 ? g.id : 3;
		if (!g.fresh || g.s.mode != 0 || g.s.ride || (g.s.status & 1)) continue;
		if (((g.s.world ^ myWorld) & 0xFFFFFF) || ((g.s.cell ^ myCell) & 0xFFFFFF) || (!g.s.cell) != (!myCell)) continue;
		float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1], dz = g.s.pos[2] - pp[2];
		if (dx * dx + dy * dy > 10000.0f * 10000.0f) continue;
		bool echo = slot == 3 && net::soloTesting();
		if (!echo && dx * dx + dy * dy + dz * dz < 80.0f * 80.0f) continue;
		want[slot] = true; memcpy(gp[slot], g.s.pos, 12); rt::strcpy(pname[slot], g.name, sizeof pname[slot]);
		if (echo) { gp[slot][0] += 200; gp[slot][2] += 0; }
	}
	for (int s = 0; s < 4; s++)
	{
		Proxy &x = proxies[s];
		double now = rt::now();
		if (!want[s])
		{
			if (x.ref && refGone(refid(x.ref))) { memset(&x, 0, sizeof x); x.goneT = now; continue; }
			if (x.ref && !x.unwantedT)
			{
				if (x.set) { float away[3] = {x.at[0], x.at[1], x.at[2] - 10000}; ((void(__thiscall *)(u32, const float *))0x575830)((u32)x.ref, away); }
				x.unwantedT = now; x.set = false;
			}
			if (x.ref && now - x.unwantedT > 3) proxyOff(x);
			continue;
		}
		x.unwantedT = 0;
		if (x.ref && refGone(refid(x.ref))) { memset(&x, 0, sizeof x); x.goneT = now; }
		if (x.ref && interior() && ptr((u32)x.ref + OFF_REF_CELL) != ptr((u32)pl + OFF_REF_CELL)) proxyOff(x);
		if (!x.ref && now - x.goneT < 3) continue;
		if (!x.ref)
		{
			if (!proxyBase[s])
			{
				u32 me = ptr((u32)pl + OFF_REF_BASE);
				if (!me || formtype((void *)me) != 0x2A) return;
				g_gThread = GetCurrentThreadId();
				u32 c = guardedCall2((u32)&cloneNpcThunk, me, 0);
				if (g_gFault || !c || formtype((void *)c) != 0x2A) { LOG("hits: no copy of your record for friends' bodies - they can't be hit"); proxyNoClone = true; return; }
				proxyBase[s] = (TESForm *)c;
				proxyName[s][0] = 0;
			}
			if (!rt::streq(proxyName[s], pname[s]))
			{
				rt::strcpy(proxyName[s], pname[s], sizeof proxyName[s]);
				char nm[24], line[80]; int k = 0;
				for (const char *q = pname[s]; *q && k < 23; q++) if (*q != '"') nm[k++] = *q;
				nm[k] = 0;
				rt::fmt(line, sizeof line, "SetName \"%s\" %08X", nm, refid(proxyBase[s]));
				console(line);
			}
			x.ref = placeNative(proxyBase[s], gp[s][0], gp[s][1], gp[s][2]);
			x.made = rt::now(); x.set = false;
			if (x.ref) LOG("hits: a body for %s to be hit (%08x)", pname[s], refid(x.ref));
			continue;
		}
		float v[3] = {gp[s][0], gp[s][1], gp[s][2]};
		if (!x.set)
		{
			bool ready = thirdPersonNode(x.ref) && handling::process(x.ref);
			if (thirdPersonNode(x.ref)) setHidden(x.ref, true);
			if (!ready && now - x.made > 6) { LOG("hits: %s's body never got its 3D - taken away, tried again in a moment", pname[s]); proxyOff(x); continue; }
			if (!ready || now - x.made < 0.5) continue;
			((void(__thiscall *)(u32, const float *))0x575830)((u32)x.ref, v); memcpy(x.at, v, 12);
			setHidden(x.ref, true);
			g_script->CallFunctionAlt(s_proxySet, player(), 1, x.ref);
			x.set = true; x.hp = proxyHp(x.ref);
			LOG("hits: %s's body ready (health %.0f)", pname[s], x.hp);
			continue;
		}
		float mx = v[0] - x.at[0], my = v[1] - x.at[1], mz = v[2] - x.at[2];
		if (mx * mx + my * my + mz * mz > 4) { ((void(__thiscall *)(u32, const float *))0x575830)((u32)x.ref, v); memcpy(x.at, v, 12); }
		setHidden(x.ref, true);
		if (rt::now() - x.hpT < 0.1) continue;
		x.hpT = rt::now();
		float hp = proxyHp(x.ref);
		if (hp < 0) { static double said; if (rt::now() - said > 30) { said = rt::now(); LOG("hits: %s's body - its health can't be read", pname[s]); } continue; }
		float lost = x.hp - hp;
		if (lost > 0.5f)
		{
			bool mine = rt::now() - myAttackT < 0.6;
			if (!mine) hurtSend(s, lost, false, pname[s]);
			else if (hostRules() & 1) hurtSend(s, lost, true, pname[s]);
			else LOG("hits: you hit %s for %.0f - friendly fire is off", pname[s], lost);
			g_script->CallFunctionAlt(s_proxyTop, player(), 1, x.ref);
			hp = proxyHp(x.ref);
		}
		x.hp = hp > 0 ? hp : x.hp;
	}
}
static void reparent(Vis &v, TESObjectREFR *pl)
{
	u32 n3 = thirdPersonNode(pl), np = n3 ? ptr(n3 + 0x18) : 0;
	if (!v.holder || !np || np == v.parent) return;
	rider::hold(np);
	if (v.parent) { rider::detach(v.parent, v.holder); rider::release(v.parent); }
	v.parent = np;
	rider::attach(v.parent, v.holder);
}
static void rideEnd(TESObjectREFR *pl, const char *why, bool place);
void scaleLoaded();
static void reset()
{
	scaleLoaded();
	if (riding) rideEnd(player(), "a game was loaded", false);
	for (auto &v : vis) { v.holder = v.parent = 0; v.body.root = 0; v.made = v.shown = false; v.carRef = 0; v.carIdx = -1; v.oldCar = 0; v.carCell = 0; v.bonesFound = false; v.gun = v.gunBone = 0; v.weap = 0; memset(&v.fx, 0, sizeof v.fx); v.gunKey.low = 0; v.shotsKnown = false; v.shotsDue = 0; v.lookSum = 0; v.here = 0; }
	markRef = 0;
	memset(&dr, 0, sizeof dr); dressBase = 0;
	memset(proxies, 0, sizeof proxies); memset(proxyBase, 0, sizeof proxyBase); memset(proxyName, 0, sizeof proxyName);
}

static void holderAt(Vis &v, const Xf &W)
{
	Xf L = xfMul(xfInv(xfRead(v.parent, 0x68)), W);
	xfWriteLocal(v.holder, L);
}
static Xf feetFrame(const float *p, float yaw)
{
	Xf D;
	float f[3] = {rt::sin(yaw), rt::cos(yaw), 0}, x[3] = {f[1], -f[0], 0};
	float R[9] = {x[0], f[0], 0, x[1], f[1], 0, 0, 0, 1};
	memcpy(D.r, R, 36); memcpy(D.t, p, 12); D.s = 1;
	return D;
}
static Xf lyingFrame(const float *p, float yaw)
{
	Xf D;
	float f[3] = {rt::sin(yaw), rt::cos(yaw), 0}, x[3] = {f[1], -f[0], 0};
	float R[9] = {x[0], 0, -f[0], x[1], 0, -f[1], 0, 1, 0};
	memcpy(D.r, R, 36); memcpy(D.t, p, 12); D.t[2] += 12; D.s = 1;
	return D;
}
static Xf bodyFrame(const net::Ghost &g) { return (g.s.status & 1) && !g.s.hasPose ? lyingFrame(g.s.pos, g.s.yaw) : feetFrame(g.s.pos, g.s.yaw); }
static Xf carFrame(const float *p, float headingDeg, float pitchDeg, float rollDeg)
{
	float rr[3], f[3], u[3];
	carAxes(headingDeg, pitchDeg, rollDeg, rr, f, u);
	Xf W;
	for (int i = 0; i < 3; i++) { W.r[i * 3] = rr[i]; W.r[i * 3 + 1] = f[i]; W.r[i * 3 + 2] = u[i]; W.t[i] = p[i]; }
	W.s = 1;
	return W;
}
static Xf vehFrame(const Vehicle &v)
{
	float lift = v.wheelie > 0 ? v.halfBase * rt::sin(v.wheelie * rt::PI / 180.0f) : 0;
	float h = v.heading * rt::PI / 180.0f;
	float back = v.wheelie > 0 ? v.halfBase * (1 - rt::cos(v.wheelie * rt::PI / 180.0f)) : 0;
	float o[3] = {v.x - rt::sin(h) * back, v.y - rt::cos(h) * back, v.z + lift + v.shakeZ};
	return carFrame(o, v.heading, (v.pitch + v.wheelie + v.shakePitch) * g_pitchSign, (v.roll + v.shakeRoll) * g_rollSign);
}

static void poseWalk(Vis &v, const net::Ghost &g, float dt)
{
	holderAt(v, bodyFrame(g));
	float sp = rt::sqrt(g.vel[0] * g.vel[0] + g.vel[1] * g.vel[1]);
	bool crouch = (g.s.flags & 2) != 0;
	float eyeH = v.eyeH * (crouch ? 0.72f : 1.0f);
	float A = rt::clamp(sp * 0.16f, 0, (g.s.flags & 1) ? 34.0f : 26.0f);
	v.phase += dt * rt::clamp(sp / 75.0f, 0, 3.4f) * rt::PI;
	if (v.phase > 1000) v.phase -= 1000;
	float s = rt::sin(v.phase), c = rt::cos(v.phase);
	rider::Free F;
	memset(&F, 0, sizeof F);
	F.eye[0] = 0; F.eye[1] = 0; F.eye[2] = eyeH + rt::absf(s) * A * 0.08f;
	F.lean = rt::clamp(sp / 900.0f, 0, 0.35f);
	auto set = [](float *o, float a, float b, float cc) { o[0] = a; o[1] = b; o[2] = cc; };
	set(F.foot[0], -7, s * A + 2, rt::max(0.0f, c) * 11 + 2);
	set(F.foot[1], 7, -s * A + 2, rt::max(0.0f, -c) * 11 + 2);
	set(F.kneePole[0], -0.1f, 1, 0.2f); set(F.kneePole[1], 0.1f, 1, 0.2f);
	for (int k = 0; k < 2; k++)
	{
		float sx = k ? 1.0f : -1.0f, sw = (k ? s : -s) * A * 0.7f;
		set(F.hand[k], sx * 18, sw + 4, eyeH - 72 + rt::absf(sw) * 0.15f);
		set(F.handDir[k], 0, 0.15f, -1); set(F.handSide[k], 0, 1, 0); F.curl[k] = 30;
	}
	rider::poseFree(v.body, F);
	updateNode(v.holder);
}
static void poseStream(Vis &v, const net::Ghost &g, const net::Skel *sk)
{
	holderAt(v, bodyFrame(g));
	u32 R = v.body.root;
	if (!v.bonesFound)
	{
		collectBones(R, v.bones);
		v.bonesFound = true;
		LOG("ghosts: %s's body copy - %d bones", v.name, v.bones.n);
	}
	Xf root = xfId();
	root.s = v.body.scale > 0.01f ? v.body.scale : 1;
	xfWriteLocal(R, root);
	applyPose(R, v.bones, g.s.pose, sk);
	if (v.weap && s_weapKnown > 0) xfWriteLocal(v.weap, s_weapLocal);
	updateNode(v.holder);
}
static float carIniFor(int ci, const char *what, float def)
{
	char k[64];
	rt::fmt(k, sizeof k, "car.%s.%s", kCars[ci].id, what);
	return iniF(k, def);
}
static void seatPoseFor(int ci, rider::Pose &p, int side)
{
	memset(&p, 0, sizeof p);
	const CarDef &cd = kCars[ci];
	const CarState &cs = g_cars[ci];
	if (cd.cockpit) { p.eye[0] = cd.eye[0]; p.eye[1] = cd.eye[1]; p.eye[2] = cd.eye[2]; }
	else { p.eye[0] = -cs.halfTrack * iniF("seatSide", 0.42f); p.eye[1] = -cs.halfBase * iniF("seatBack", 0.12f); p.eye[2] = TUNE(seatEye); }
	p.eye[0] = carIniFor(ci, "eyeX", p.eye[0]); p.eye[1] = carIniFor(ci, "eyeY", p.eye[1]); p.eye[2] = carIniFor(ci, "eyeZ", p.eye[2]);
	if (side > 0) p.eye[0] = -p.eye[0];
	p.side = side;
	p.floorZ = carIniFor(ci, "floor", cd.cockpit ? cd.floor : cs.clearance + TUNE(ridersFloorAboveClearance));
	p.recline = carIniFor(ci, "recline", cd.cockpit && cd.recline > 0 ? cd.recline : TUNE(ridersReclineDegrees));
	p.curl = TUNE(ridersFingerCurlDegrees);
	if (cd.cockpit && cd.lever[2] > 0) { p.lever[0] = cd.lever[0]; p.lever[1] = cd.lever[1]; p.lever[2] = cd.lever[2]; }
	else { p.lever[0] = p.eye[0] * 0.15f; p.lever[1] = p.eye[1] + 22; p.lever[2] = p.floorZ + 26; }
}
static void seat(Vis &v, const Xf &carW, int ci, int side)
{
	holderAt(v, carW);
	rider::Pose p;
	seatPoseFor(ci, p, side);
	rider::pose(v.body, p);
	updateNode(v.holder);
}
static Xf myCarFrame() { return vehFrame(g_car); }
static void gunTick(Vis &v, const net::Ghost &g, bool show)
{
	bool want = show && (g.s.flags & 4) && g.s.weapon.low;
	if (!want) { if (v.gun) *(u32 *)(v.gun + OFF_NIAV_FLAGS) |= 1u; return; }
	if (!v.bonesFound) { collectBones(v.body.root, v.bones); v.bonesFound = true; }
	if (v.gunKey.low != g.s.weapon.low || v.gunKey.modHash != g.s.weapon.modHash)
	{
		gunOff(v);
		v.gunKey = g.s.weapon;
		TESForm *w = formOf(g.s.weapon);
		const char *path = w && formtype(w) == 0x28 ? (const char *)ptr((u32)w + 0x40) : 0;
		u32 bone = v.weap;
		for (int i = 0; i < v.bones.n && !bone; i++) if (!rt::strcmp((const char *)ptr(v.bones.node[i] + 8), "Weapon")) bone = v.bones.node[i];
		if (!path || !path[0] || !bone) { LOG("ghosts: %s's weapon %06x - %s", v.name, g.s.weapon.low, !w ? "not on this PC" : !bone ? "no hand to put it in" : "no model"); return; }
		u32 shared = loadModel(path);
		u32 c = shared ? rider::copy(shared) : 0;
		if (!c) { LOG("ghosts: %s's weapon model %s couldn't be loaded", v.name, path); return; }
		rider::attach(bone, c);
		v.gun = c; v.gunBone = bone; v.gunOrig = xfRead(c, 0x34);
		const float *gt = (const float *)(c + 0x58);
		const char *pn = (const char *)ptr(ptr(bone + 0x18) + 8);
		LOG("ghosts: %s holds %s (under %s; the model's own offset %.1f %.1f %.1f)", v.name, path, pn ? pn : "?", gt[0], gt[1], gt[2]);
	}
	if (v.gun)
	{
		*(u32 *)(v.gun + OFF_NIAV_FLAGS) &= ~1u;
		float G[9], R[9];
		move::m9(g.s.gunQ, G);
		handling::mul3(G, v.gunOrig.r, R);
		memcpy((void *)(v.gun + 0x34), R, 36);
	}
}

static int rideWith = -1, rideCar = -1;
static double inviteUntil;
static float rideYaw, ridePitch;
static HiddenSet rideHid1, rideHid3;
static bool rideTcl;
static Xf rideCarW;
static void inviteNow() { inviteUntil = rt::now() + 15; message("Offering a ride for 15 s - a friend presses G to get in", 3); LOG("ghosts: offering a ride"); }
static const net::Ghost *ghostById(int id) { for (int i = 0; i < s_n; i++) if (s_gs[i].id == id) return &s_gs[i]; return 0; }
static int rideCandidate(TESObjectREFR *pl, float *d2out)
{
	if (riding || g_driving) return -1;
	const float *pp = pos(pl);
	int best = -1; float bestD = 1e18f;
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	for (int i = 0; i < s_n; i++)
	{
		const net::Ghost &g = s_gs[i];
		if (!g.fresh || g.s.mode != 1 || g.s.car < 0 || g.s.car >= kNumCars || g.s.world != world || g.s.cell != cid) continue;
		if (!g_cars[g.s.car].available || kCars[g.s.car].seats < 2) continue;
		bool taken = false;
		for (int j = 0; j < s_n; j++) if (j != i && s_gs[j].s.ride == g.id + 1) taken = true;
		if (taken) continue;
		float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1], d2 = dx * dx + dy * dy;
		float reach = g.s.invite ? 1800.0f : g_cars[g.s.car].halfLength + 260.0f;
		if (d2 < reach * reach && d2 < bestD) { bestD = d2; best = g.id; }
	}
	if (d2out) *d2out = bestD;
	return best;
}
static void rideBegin(TESObjectREFR *pl, int id)
{
	const net::Ghost *g = ghostById(id);
	if (!g) return;
	riding = true; rideWith = id; rideCar = g->s.car;
	handling::release(pl, "riding with a friend"); handling::active = false;
	move::reset(); sprint::reset();
	console("DisablePlayerControls 1 0 1 1 1 0 1");
	if (!g_tcl) { console("tcl"); g_tcl = true; rideTcl = true; }
	setFirstPerson(true);
	rideYaw = 0; ridePitch = 0;
	char m[96]; rt::fmt(m, sizeof m, "Riding with %s - G to get out", g->name); message(m, 4);
	LOG("ghosts: riding in %s's %s", g->name, kCars[rideCar].id);
}
static void rideEnd(TESObjectREFR *pl, const char *why, bool place)
{
	if (!riding) return;
	riding = false;
	showChildren(firstPersonNode(pl), rideHid1); showChildren(thirdPersonNode(pl), rideHid3);
	if (place)
	{
		float side = rideCar >= 0 ? g_cars[rideCar].halfTrack + 70 : 120;
		float px = rideCarW.t[0] + rideCarW.r[0] * side, py = rideCarW.t[1] + rideCarW.r[3] * side;
		float pz = groundAt(px, py, rideCarW.t[2]) + 10;
		float hd = rt::atan2(rideCarW.r[1], rideCarW.r[4]) * 180.0f / rt::PI;
		moveRef(pl, px, py, pz, 0, 0, hd);
	}
	console("EnablePlayerControls");
	if (rideTcl && g_tcl) { console("tcl"); g_tcl = false; }
	rideTcl = false;
	LOG("ghosts: out of the friend's car (%s)", why);
	rideWith = -1; rideCar = -1;
}
static void rideTick(TESObjectREFR *pl, float dt, bool focus)
{
	const net::Ghost *g = ghostById(rideWith);
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	if (!g || !g->fresh || g->s.mode != 1 || g->s.car != rideCar || g->s.world != world || g->s.cell != cid)
	{
		rideEnd(pl, !g || !g->fresh ? "the friend is gone" : g->s.mode != 1 ? "the friend got out" : "the car changed", true);
		message("Out of the car", 2);
		return;
	}
	if (focus && !menumode() && pressed(VK_USE)) { rideEnd(pl, "you got out", true); return; }
	hideChildren(firstPersonNode(pl), rideHid1);
	hideChildren(thirdPersonNode(pl), rideHid3);
	u32 in = ptr(0x11F35CC);
	int mx = in ? rd<int>(in + 0x1B24) : 0, my = in ? rd<int>(in + 0x1B28) : 0;
	if (rd<int>(0x11E0A5C + 4)) my = -my;
	float sens = rd<float>(0x11E0A6C + 4);
	if (!(sens > 0.0001f && sens < 1.0f)) sens = 0.002f;
	float k = sens * 180.0f / rt::PI * TUNE(mouseLookScale);
	rideYaw = rt::clamp(wrap180(rideYaw + mx * k), -165, 165);
	ridePitch = rt::clamp(ridePitch + my * k, -40, 55);
	{
		const Vis &dv = vis[g->id < 4 ? g->id : 3];
		rideCarW = dv.carWOk ? dv.carW : carFrame(g->s.pos, g->s.yaw, g->s.pitch, g->s.roll);
	}
	rider::Pose p;
	seatPoseFor(rideCar, p, 1);
	float e[3], ew[3];
	e[0] = p.eye[0]; e[1] = p.eye[1]; e[2] = p.eye[2] + TUNE(seatCameraLiftUnits);
	move::apply(rideCarW.r, e, ew);
	float eye = iniF("playerEye", 120);
	float hd = g->s.yaw + rideYaw;
	while (hd >= 360) hd -= 360;
	while (hd < 0) hd += 360;
	moveRef(pl, rideCarW.t[0] + ew[0], rideCarW.t[1] + ew[1], rideCarW.t[2] + ew[2] - eye, rt::clamp(-g->s.pitch + 2 + ridePitch, -60, 70), 0, hd);
	(void)dt;
}

enum { kMaxInv = 1000 };
struct Inv { int n; u32 form[kMaxInv]; i32 count[kMaxInv]; };
static void readInv(TESObjectREFR *r, Inv &I)
{
	I.n = 0;
	for (u32 x = ptr((u32)r + 0x48), k = 0; x && k < 64; x = ptr(x + 8), k++)
	{
		if (rd<u8>(x + 4) != 0x15) continue;
		u32 data = ptr(x + 0xC), list = data ? ptr(data) : 0;
		for (u32 nd = list, m = 0; nd && m < 4000; nd = ptr(nd + 4), m++)
		{
			u32 e = ptr(nd);
			u32 f = e ? ptr(e + 8) : 0;
			if (!f) continue;
			if (I.n == kMaxInv)
			{
				static double said;
				if (rt::now() - said > 60) { said = rt::now(); LOG("inventory: more than %d kinds of things carried - the rest isn't counted", (int)kMaxInv); }
				break;
			}
			I.form[I.n] = f; I.count[I.n] = rd<i32>(e + 4); I.n++;
		}
		break;
	}
}
static i32 invCount(const Inv &I, u32 f) { for (int i = 0; i < I.n; i++) if (I.form[i] == f) return I.count[i]; return 0; }
static bool invHas(const Inv &I, u32 f) { for (int i = 0; i < I.n; i++) if (I.form[i] == f) return true; return false; }
static Inv invLast;
static bool invHave;

static u32 contRef;
struct MyDrop { u32 id, ref; net::FormKey item; u16 count; float pos[3]; u32 world, cell; };
static MyDrop myDrops[net::kMaxDrops];
static int nMy;
static u32 dropSeq;
struct Copy { u8 owner; u32 id, ref; };
static Copy copies[24];
static int nCopies;
static net::Taken takenL[net::kMaxTaken];
static double takenT[net::kMaxTaken];
static int nTaken;
static void cellKeys(u32 cell, u32 *world, u32 *cid)
{
	*world = *cid = 0;
	if (!cell) return;
	if (rd<u8>(cell + OFF_CELL_FLAGS) & 1) *cid = refid((void *)cell);
	else *world = refid((void *)ptr(cell + OFF_CELL_WORLD));
}
static bool refGone(u32 id)
{
	TESForm *f = id ? lookup(id) : 0;
	if (!f) return true;
	u32 fl = rd<u32>((u32)f + 8);
	return (fl & 0x20) || (fl & 0x800);
}
static u16 refCount(u32 r)
{
	for (u32 e = ptr(r + 0x48), n = 0; e && n < 64; e = ptr(e + 8), n++)
		if (rd<u8>(e + 4) == 0x24) { i16 c = rd<i16>(e + 0xC); return c > 0 ? (u16)c : 1; }
	return 1;
}
static bool isMine(u8 owner) { return owner == net::myId() || (net::soloTesting() && owner == 3); }
static void lootTick(TESObjectREFR *pl, const net::Self &me)
{
	{
		static Inv cur;
		static struct Cand { u32 form; i32 n; double t; } cand[8];
		static int nc;
		readInv(pl, cur);
		if (invHave && !contRef)
			for (int i = 0; i < invLast.n; i++)
			{
				i32 d = invLast.count[i] - invCount(cur, invLast.form[i]);
				if (d > 0 && nc < 8) { cand[nc].form = invLast.form[i]; cand[nc].n = d; cand[nc].t = rt::now(); nc++; }
			}
		invLast = cur; invHave = true;
		u32 cell = ptr((u32)pl + OFF_REF_CELL);
		const float *pp = pos(pl);
		for (int c = 0; c < nc;)
		{
			if (rt::now() - cand[c].t > 3) { cand[c] = cand[--nc]; continue; }
			u32 found = 0;
			for (u32 nd = cell ? cell + 0xAC : 0, k = 0; nd && k < 6000 && !found; nd = ptr(nd + 4), k++)
			{
				u32 r = ptr(nd);
				if (!r || (refid((void *)r) >> 24) != 0xFF || ptr(r + OFF_REF_BASE) != cand[c].form) continue;
				u32 id = refid((void *)r);
				bool known = refGone(id);
				for (int i = 0; i < nMy && !known; i++) known = myDrops[i].ref == id;
				for (int i = 0; i < nCopies && !known; i++) known = copies[i].ref == id;
				const float *rp = pos((void *)r);
				float dx = rp[0] - pp[0], dy = rp[1] - pp[1];
				if (!known && dx * dx + dy * dy < 450.0f * 450.0f) found = id;
			}
			if (found)
			{
				if (nMy == net::kMaxDrops) { memmove(myDrops, myDrops + 1, sizeof(MyDrop) * (net::kMaxDrops - 1)); nMy--; }
				MyDrop &d = myDrops[nMy++];
				d.id = ++dropSeq; d.ref = found; d.item = formKey(refid((void *)cand[c].form)); d.count = refCount((u32)lookup(found));
				LOG("loot: you dropped %08x x%u (shared as #%u)", refid((void *)cand[c].form), d.count, d.id);
				cand[c] = cand[--nc];
				continue;
			}
			c++;
		}
	}
	for (int i = 0; i < nMy;)
	{
		MyDrop &d = myDrops[i];
		TESObjectREFR *r = refGone(d.ref) ? 0 : (TESObjectREFR *)lookup(d.ref);
		if (!r) { LOG("loot: #%u picked up or gone", d.id); myDrops[i] = myDrops[--nMy]; continue; }
		memcpy(d.pos, pos(r), 12);
		cellKeys(ptr((u32)r + OFF_REF_CELL), &d.world, &d.cell);
		i++;
	}
	for (int p = 0; p <= 3; p++)
	{
		net::Drop dd[net::kMaxDrops]; net::Taken tk[net::kMaxTaken]; int nt = 0;
		if (net::peerLoot(p, dd, net::kMaxDrops, tk, &nt, net::kMaxTaken) < 0) continue;
		for (int t = 0; t < nt; t++)
		{
			if (!isMine(tk[t].owner)) continue;
			for (int i = 0; i < nMy; i++)
				if (myDrops[i].id == tk[t].id)
				{
					LOG("loot: #%u was picked up by a friend - yours removed", myDrops[i].id);
					removeRef((TESObjectREFR *)lookup(myDrops[i].ref));
					myDrops[i] = myDrops[--nMy];
					break;
				}
		}
	}
	for (int p = 0; p <= 3; p++)
	{
		net::Drop dd[net::kMaxDrops]; net::Taken tk[net::kMaxTaken]; int nt = 0;
		int nd = net::peerLoot(p, dd, net::kMaxDrops, tk, &nt, net::kMaxTaken);
		if (nd < 0) continue;
		for (int c = 0; c < nCopies;)
		{
			Copy &cp = copies[c];
			if (cp.owner != p) { c++; continue; }
			bool listed = false;
			for (int i = 0; i < nd; i++) if (dd[i].id == cp.id) listed = true;
			if (refGone(cp.ref))
			{
				if (listed && nTaken < net::kMaxTaken) { takenL[nTaken].owner = (u8)p; takenL[nTaken].id = cp.id; takenT[nTaken] = rt::now(); nTaken++; LOG("loot: you picked up a friend's #%u", cp.id); }
				copies[c] = copies[--nCopies];
				continue;
			}
			if (!listed) { removeRef((TESObjectREFR *)lookup(cp.ref)); LOG("loot: a friend's #%u picked up - its copy removed", cp.id); copies[c] = copies[--nCopies]; continue; }
			c++;
		}
		for (int i = 0; i < nd; i++)
		{
			const net::Drop &d = dd[i];
			bool have = false;
			for (int c = 0; c < nCopies; c++) if (copies[c].owner == p && copies[c].id == d.id) have = true;
			for (int t = 0; t < nTaken; t++) if (takenL[t].owner == p && takenL[t].id == d.id) have = true;
			if (have || nCopies == 24 || d.world != me.world || d.cell != me.cell) continue;
			float dx = d.pos[0] - me.pos[0], dy = d.pos[1] - me.pos[1];
			if (dx * dx + dy * dy > 5000.0f * 5000.0f) continue;
			TESForm *base = formOf(d.item);
			if (!base) { static u32 told; if (told != d.id) { told = d.id; LOG("loot: a friend's #%u (%06x) isn't on this PC", d.id, d.item.low); } continue; }
			TESObjectREFR *r = placeCount(base, d.count, d.pos[0], d.pos[1], d.pos[2] + 4);
			if (!r) continue;
			copies[nCopies].owner = (u8)p; copies[nCopies].id = d.id; copies[nCopies].ref = refid(r); nCopies++;
			LOG("loot: a friend's #%u (%08x x%u) copied in front of you", d.id, refid(base), d.count);
		}
	}
	for (int t = 0; t < nTaken;) { if (rt::now() - takenT[t] > 15) { takenL[t] = takenL[nTaken - 1]; takenT[t] = takenT[nTaken - 1]; nTaken--; } else t++; }
	net::Drop out[net::kMaxDrops];
	for (int i = 0; i < nMy; i++)
	{
		out[i].id = myDrops[i].id; out[i].item = myDrops[i].item; out[i].count = myDrops[i].count;
		memcpy(out[i].pos, myDrops[i].pos, 12); out[i].world = myDrops[i].world; out[i].cell = myDrops[i].cell;
	}
	net::setLoot(out, nMy, takenL, nTaken);
}

static void tradeIn(u8 peer, const net::Event &ev, const char *who);
static void vendorIn(u8 peer, const net::Event &ev, const char *who);
static void objIn(u8 peer, const net::Event &ev, const char *who);
struct PeerAt;
static void talkIn(u8 peer, const net::Event &ev, const char *who, const PeerAt *at);
void vendorTick(TESObjectREFR *pl);
static void contDiff(TESObjectREFR *pl)
{
	static Inv cur;
	readInv(pl, cur);
	TESObjectREFR *c = contRef ? (TESObjectREFR *)lookup(contRef) : 0;
	net::FormKey ck = formKey(contRef);
	u8 ft = c ? formtype(c) : 0;
	bool actor = ft == 0x3B || ft == 0x3C;
	bool share = c && ck.low && !actor;
	auto one = [&](u32 f, i32 before, i32 now) {
		if (before == now || !share) return;
		net::EvContainer e;
		e.cont = ck; e.item = formKey(refid((void *)f)); e.delta = before - now;
		if (!e.item.low) return;
		net::addEvent(net::EV_CONTAINER, &e, sizeof e);
		LOG("share: container %08x %s %d of %08x", contRef, e.delta < 0 ? "lost" : "got", e.delta < 0 ? -e.delta : e.delta, refid((void *)f));
	};
	for (int i = 0; i < cur.n; i++) one(cur.form[i], invCount(invLast, cur.form[i]), cur.count[i]);
	for (int i = 0; i < invLast.n; i++) if (!invHas(cur, invLast.form[i]) && invLast.count[i]) one(invLast.form[i], invLast.count[i], 0);
	invLast = cur; invHave = true;
}
static void menuTick(TESObjectREFR *pl)
{
	if (!netInited || !net::active()) return;
	u32 cm = ptr(0x11D93F8), c = cm ? ptr(cm + 0x74) : 0;
	u32 cid = c ? refid((void *)c) : 0;
	if (cid != contRef)
	{
		if (contRef) contDiff(pl);
		contRef = cid;
		if (cid) { void lootRollNow(u32 id); lootRollNow(cid); readInv(pl, invLast); invHave = true; }
		return;
	}
	if (contRef) contDiff(pl);
	vendorTick(pl);
	void talkGuard(TESObjectREFR *pl); talkGuard(pl);
}

struct Pending { u8 peer; net::Event ev; double t; bool havePos; float pos[3]; u32 world, cell; };
enum { kPend = 256 };
static Pending pend[kPend];
static int nPend;
struct PeerAt { float pos[3]; u32 world, cell; };
static bool applyEvent(u8 peer, const net::Event &e, const char *who, const PeerAt *peerAt);
bool barterOpen();
static void eventsTick()
{
	bool hold = menumode() && (contRef || barterOpen());
	for (int p = 0; p <= 3; p++)
	{
		const char *who = vis[p].name[0] ? vis[p].name : "a friend";
		PeerAt at;
		bool havePos = net::peerPos(p, at.pos, &at.world, &at.cell);
		for (int i = 0; i < 64; i++)
		{
			net::Event ev;
			if (!net::peekEvent(p, &ev)) break;
			bool moves = ev.type == net::EV_CONTAINER || ev.type == net::EV_VENDOR ||
				(ev.type == net::EV_TRADE && ev.data[0] != net::TR_OFFER && ev.data[0] != net::TR_UPDATE && ev.data[0] != net::TR_READY);
			if (hold && moves) break;
			net::takeEvents(p, &ev, 1);
			if (!applyEvent((u8)p, ev, who, havePos ? &at : 0))
			{
				if (nPend == kPend)
				{
					static int told; if (told++ < 5) LOG("share: 256 friends' changes waiting for places not loaded here - the oldest (type %d) let go", (int)pend[0].ev.type);
					memmove(pend, pend + 1, sizeof(Pending) * (kPend - 1)); nPend--;
				}
				pend[nPend].peer = (u8)p; pend[nPend].ev = ev; pend[nPend].t = rt::now();
				pend[nPend].havePos = havePos;
				if (havePos) { memcpy(pend[nPend].pos, at.pos, 12); pend[nPend].world = at.world; pend[nPend].cell = at.cell; }
				nPend++;
			}
		}
	}
	static double retryT;
	if (nPend && !hold && rt::now() - retryT > 2)
	{
		retryT = rt::now();
		for (int i = 0; i < nPend;)
		{
			PeerAt pa; memcpy(pa.pos, pend[i].pos, 12); pa.world = pend[i].world; pa.cell = pend[i].cell;
			if (applyEvent(pend[i].peer, pend[i].ev, "a friend", pend[i].havePos ? &pa : 0) || rt::now() - pend[i].t > 600) { pend[i] = pend[--nPend]; continue; }
			i++;
		}
	}
}
static TESObjectREFR *refOf(const net::FormKey &k)
{
	TESForm *f = formOf(k);
	if (!f || formtype(f) < 0x3A || formtype(f) > 0x3C || (TESObjectREFR *)f == player() || (rd<u32>((u32)f + 8) & 0x20)) return 0;
	return (TESObjectREFR *)f;
}
static void fightDamaged(const net::FormKey &k, float amount);
static bool shareAllowed(u8 peer, int kind, TESObjectREFR *r, const PeerAt *peerAt, const char *who)
{
	static double since[4][2]; static int count[4][2]; static double toldT[4];
	const char *what = kind ? "container change" : "kill";
	float lim = kind ? TUNE(shareContainersPer30s) : TUNE(shareKillsPer30s);
	double now = rt::now();
	if (peer < 4)
	{
		if (now - since[peer][kind] > 30) { since[peer][kind] = now; count[peer][kind] = 0; }
		if (++count[peer][kind] > (int)lim)
		{
			if (now - toldT[peer] > 10) { toldT[peer] = now; LOG("share: %s's %ss dropped - more than %.0f in 30 s", who, what, lim); }
			return false;
		}
	}
	float *rp = r ? pos(r) : 0;
	if (!peerAt || !rp) { LOG("share: %s's %s of %08x dropped - where they are isn't known", who, what, r ? refid(r) : 0); return false; }
	u32 cell = ptr((u32)r + OFF_REF_CELL);
	bool inside = cell && (rd<u8>(cell + OFF_CELL_FLAGS) & 1);
	u32 here = inside ? refid((void *)cell) : (cell ? refid((void *)ptr(cell + OFF_CELL_WORLD)) : 0);
	u32 there = inside ? peerAt->cell : peerAt->world;
	if (!here || ((here ^ there) & 0xFFFFFF) || (inside != (peerAt->cell != 0)))
	{
		LOG("share: %s's %s of %08x dropped - not where they are (%s %08x, they're in %08x/%08x)", who, what, refid(r), inside ? "cell" : "world", here, peerAt->world, peerAt->cell);
		return false;
	}
	float dx = rp[0] - peerAt->pos[0], dy = rp[1] - peerAt->pos[1], dz = rp[2] - peerAt->pos[2], reach = TUNE(shareReachUnits);
	if (dx * dx + dy * dy + dz * dz > reach * reach)
	{
		LOG("share: %s's %s of %08x dropped - %.0f units from them (more than %.0f)", who, what, refid(r), rt::sqrt(dx * dx + dy * dy + dz * dz), reach);
		return false;
	}
	return true;
}
struct DoorWatch { u32 ref; int state; double until; };
static DoorWatch doorW[6];
static int doorState(TESObjectREFR *d)
{
	NVSEElement r; memset(&r, 0, sizeof r);
	if (!s_doorGet || !g_script->CallFunction(s_doorGet, player(), 0, &r, 1, d) || r.type != 1) return -1;
	int v = (int)r.num, o = v % 10;
	return (o == 1 || o == 2 ? 1 : 0) + (v >= 10 ? 10 : 0);
}
static void doorsTick(bool focus)
{
	static bool eWas;
	bool e = focus && held('E') && !menumode();
	if (e && !eWas && s_crossRef)
	{
		NVSEElement r; memset(&r, 0, sizeof r);
		g_script->CallFunction(s_crossRef, player(), 0, &r, 0);
		TESObjectREFR *d = r.type == 2 ? (TESObjectREFR *)r.form : 0;
		u32 base = d ? ptr((u32)d + OFF_REF_BASE) : 0;
		if (base && formtype((void *)base) == 0x1C && formKey(refid(d)).low)
		{
			DoorWatch *w = 0;
			for (auto &x : doorW) if (x.ref == refid(d)) w = &x;
			if (!w) { w = &doorW[0]; for (auto &x : doorW) if (x.until < w->until) w = &x; w->ref = refid(d); w->state = doorState(d); }
			w->until = rt::now() + 30;
		}
	}
	eWas = e;
	static double pollT;
	if (rt::now() - pollT < 0.25) return;
	pollT = rt::now();
	for (auto &w : doorW)
	{
		if (!w.ref || rt::now() > w.until) { w.ref = 0; continue; }
		TESObjectREFR *d = refGone(w.ref) ? 0 : (TESObjectREFR *)lookup(w.ref);
		int st = d ? doorState(d) : -1;
		if (st < 0 || st == w.state) continue;
		w.state = st;
		net::EvDoor ev; ev.ref = formKey(w.ref); ev.open = st % 10 ? 1 : 0; ev.locked = st >= 10 ? 1 : 0;
		net::addEvent(net::EV_DOOR, &ev, sizeof ev);
		LOG("share: door %08x %s%s - friends' games told", w.ref, ev.open ? "open" : "closed", ev.locked ? ", locked" : "");
	}
}
static bool applyEvent(u8 peer, const net::Event &e, const char *who, const PeerAt *peerAt)
{
	if (peer == 3 && net::soloTesting() && e.type != net::EV_CHAT) return true;
	if (e.type == net::EV_SESSIONASK) { if (peer >= 1 && peer <= 2 && net::hosting()) coop::hostSession(peer, who); return true; }
	if (e.type == net::EV_QUESTITEMS) { if (peer >= 1 && peer <= 2 && net::hosting()) coop::questItemsIn(peer, e.data, e.len, who); return true; }
	if (e.type == net::EV_QUESTSTEP) { if (peer <= 2) coop::questStepIn(peer, e.data, e.len, who); return true; }
	if (e.type == net::EV_SESSIONNO) { if (peer == 0) coop::sessionRefused(e.data, e.len); return true; }
	if (e.type == net::EV_CHAT)
	{
		char t[130]; int n = e.len < 124 ? e.len : 124;
		memcpy(t, e.data, n); t[n] = 0;
		char m[180]; rt::fmt(m, sizeof m, "%s: %s", who, t);
		message(m, 7, false);
		LOG("chat: %s", m);
		return true;
	}
	if (e.type == net::EV_CONTAINER && e.len == sizeof(net::EvContainer))
	{
		net::EvContainer c; memcpy(&c, e.data, sizeof c);
		TESObjectREFR *cr = refOf(c.cont);
		TESForm *it = formOf(c.item);
		if (!it) return true;
		if (!cr) return false;
		if (formtype(cr) == 0x3B || formtype(cr) == 0x3C) return true;
		if (c.delta > 100 || c.delta < -100) { LOG("share: %s's change of %+d to container %08x dropped (more than 100)", who, c.delta, refid(cr)); return true; }
		if (peer != 3 && !shareAllowed(peer, 1, cr, peerAt, who)) return true;
		g_script->CallFunctionAlt(s_contAdj, player(), 3, cr, it, F((float)c.delta));
		LOG("share: %s's change to container %08x applied here (%+d of %08x)", who, refid(cr), c.delta, refid(it));
		return true;
	}
	if (e.type == net::EV_KILL && e.len == sizeof(net::EvKill))
	{
		net::EvKill k; memcpy(&k, e.data, sizeof k);
		TESObjectREFR *a = refOf(k.ref);
		if (!a) return false;
		if ((formtype(a) == 0x3B || formtype(a) == 0x3C) && !deadNow(a) && (peer == 3 || shareAllowed(peer, 0, a, peerAt, who)))
		{
			g_script->CallFunctionAlt(s_kill, player(), 1, a);
			LOG("share: %08x killed by %s - dead here too", refid(a), who);
		}
		return true;
	}
	if (e.type == net::EV_XP && e.len == sizeof(net::EvXp)) { if (peer <= 2) coop::xpIn(peer, e.data, e.len, who); return true; }
	if (e.type == net::EV_HOSTWORLD) { if (peer == 0) coop::hostWorldChanged(who); return true; }
	if (e.type == net::EV_OBJ && e.len == sizeof(net::EvObj)) { if (peer <= 2) objIn(peer, e, who); return true; }
	if (e.type == net::EV_TALK && e.len == sizeof(net::EvTalk)) { if (peer <= 2) talkIn(peer, e, who, peerAt); return true; }
	if (e.type == net::EV_TRADE && e.len == sizeof(net::EvTrade)) { if (peer <= 2) tradeIn(peer, e, who); return true; }
	if (e.type == net::EV_VENDOR && e.len == sizeof(net::EvVendor)) { if (peer <= 2) vendorIn(peer, e, who); return true; }
	if (e.type == net::EV_HURT && e.len == sizeof(net::EvHurt))
	{
		net::EvHurt h; memcpy(&h, e.data, sizeof h);
		if (h.to != net::myId() || !(h.amount > 0) || h.amount > 2000) return true;
		if (h.fromPlayer && !(hostRules() & 1)) return true;
		void hurtMe(float amount, bool fromPlayer, const char *who);
		hurtMe(h.amount, h.fromPlayer != 0, who);
		return true;
	}
	if (e.type == net::EV_REVIVE && e.len == sizeof(net::EvRevive))
	{
		net::EvRevive r; memcpy(&r, e.data, sizeof r);
		if (r.to != net::myId()) return true;
		void revivedBy(const char *who, int how);
		revivedBy(who, r.how);
		return true;
	}
	if (e.type == net::EV_CHARGEN && e.len == 2)
	{
		if (peer != 0 || !coop::inSession()) return true;
		void charGenIn(int menu);
		charGenIn(e.data[0] | (e.data[1] << 8));
		return true;
	}
	if (e.type == net::EV_DOOR && e.len == sizeof(net::EvDoor))
	{
		net::EvDoor d; memcpy(&d, e.data, sizeof d);
		TESObjectREFR *r = refOf(d.ref);
		if (!r) return true;
		int st = doorState(r);
		if (st < 0) return true;
		int want = (d.open ? 1 : 0) + (d.locked ? 10 : 0);
		if (st != want) { g_script->CallFunctionAlt(s_doorSet, player(), 3, r, F(d.open ? 1.0f : 0.0f), F(d.locked ? 1.0f : 0.0f)); LOG("share: %s's door %08x %s%s here too", who, refid(r), d.open ? "opened" : "closed", d.locked ? ", locked" : ""); }
		for (auto &w : doorW) if (w.ref == refid(r)) w.state = want;
		return true;
	}
	if (e.type == net::EV_DAMAGE && e.len == sizeof(net::EvDamage))
	{
		net::EvDamage d; memcpy(&d, e.data, sizeof d);
		fightDamaged(d.ref, d.amount);
		return true;
	}
	return true;
}

static u32 fightRef[net::kMaxEnemies];
static double fightDeadT[net::kMaxEnemies];
static int nFight;
static u32 shotActor[16]; static u16 shotCount[16];
static void npcShot(u32 actor)
{
	for (int i = 0; i < 16; i++) if (shotActor[i] == actor) { shotCount[i]++; return; }
	for (int i = 0; i < 16; i++) if (!shotActor[i] || refGone(refid((void *)shotActor[i]))) { shotActor[i] = actor; shotCount[i] = 1; return; }
}
static u16 shotsOf(u32 actor) { for (int i = 0; i < 16; i++) if (shotActor[i] == actor) return shotCount[i]; return 0; }
static float healthPct(TESObjectREFR *a) { double v = 1; NVSEElement r; memset(&r, 0, sizeof r); g_script->CallFunction(s_healthPct, player(), 0, &r, 1, a); if (r.type == 1) v = r.num; return (float)v; }
static bool mirroredHere(u32 id);
static bool sendingHere(u32 id);
static void fightsTick(TESObjectREFR *pl)
{
	static double pickT, skelT;
	double now = rt::now();
	if (now - pickT > 0.3)
	{
		pickT = now;
		u32 keep[net::kMaxEnemies]; double keepT[net::kMaxEnemies]; int nk = 0;
		static u32 fightLoad; if (fightLoad != g_loads) { fightLoad = g_loads; nFight = 0; }
		for (int i = 0; i < nFight; i++)
		{
			TESObjectREFR *a = refGone(fightRef[i]) ? 0 : (TESObjectREFR *)lookup(fightRef[i]);
			if (!a) continue;
			bool dead = deadNow(a);
			if (dead && fightDeadT[i] == 0)
			{
				fightDeadT[i] = now;
				net::EvKill k; k.ref = formKey(fightRef[i]);
				net::addEvent(net::EV_KILL, &k, sizeof k);
				LOG("share: %08x died in your fight", fightRef[i]);
			}
			if (dead && now - fightDeadT[i] < 3) { keep[nk] = fightRef[i]; keepT[nk] = fightDeadT[i]; nk++; }
		}
		const float *pp = pos(pl);
		{
			static u32 alive[96]; static int nAlive; static u32 aliveLoad; static double aliveSince;
			if (aliveLoad != g_loads) { aliveLoad = g_loads; nAlive = 0; aliveSince = now; }
			u32 nowAlive[96]; int na = 0;
			forEachActor([&](TESObjectREFR *a) {
				u8 ft = formtype(a);
				if (a == pl || (ft != 0x3B && ft != 0x3C) || na >= 96) return;
				u32 id = refid(a);
				if (!formKey(id).low || deadNow(a)) return;
				nowAlive[na++] = id;
			});
			for (int k = 0; k < nAlive; k++)
			{
				u32 id = alive[k];
				bool still = false;
				for (int j = 0; j < na && !still; j++) still = nowAlive[j] == id;
				if (still || refGone(id)) continue;
				TESObjectREFR *a = (TESObjectREFR *)lookup(id);
				if (!a || !deadNow(a) || mirroredHere(id) || sendingHere(id)) continue;
				if (now - aliveSince < 5) continue;
				net::EvKill ev; ev.ref = formKey(id);
				net::addEvent(net::EV_KILL, &ev, sizeof ev);
				LOG("share: %08x died near you - friends' games told", id);
			}
			memcpy(alive, nowAlive, 4 * na); nAlive = na;
		}
		net::Ghost gs[4]; int ngs = net::hosting() ? net::ghosts(gs, 4) : 0;
		u32 myWorld = worldspace(), myCell = interior() ? refid((void *)ptr((u32)pl + OFF_REF_CELL)) : 0;
		struct C { u32 id; float d; } cand[48]; int nc = 0;
		forEachActor([&](TESObjectREFR *a) {
			u8 ft = formtype(a);
			if (a == pl || (ft != 0x3B && ft != 0x3C) || deadNow(a) || nc >= 48) return;
			u32 id = refid(a);
			if (!formKey(id).low) return;
			if (mirroredHere(id)) return;
			float *ap = pos(a), dx = ap[0] - pp[0], dy = ap[1] - pp[1], d = dx * dx + dy * dy;
			if (ptr((u32)a + 0x128) == (u32)pl) { if (d > 7000.0f * 7000.0f) return; cand[nc].id = id; cand[nc].d = d; nc++; return; }
			float best = 1e30f;
			for (int g = 0; g < ngs; g++)
			{
				if (!gs[g].fresh || gs[g].id == 3 || ((gs[g].s.world ^ myWorld) & 0xFFFFFF) || ((gs[g].s.cell ^ myCell) & 0xFFFFFF) || (!gs[g].s.cell) != (!myCell)) continue;
				float gx = ap[0] - gs[g].s.pos[0], gy = ap[1] - gs[g].s.pos[1], gd = gx * gx + gy * gy;
				if (gd < best) best = gd;
			}
			if (best > 4200.0f * 4200.0f) return;
			cand[nc].id = id; cand[nc].d = 1e12f + best; nc++;
		});
		for (int i = 0; i < nc; i++) for (int j = i + 1; j < nc; j++) if (cand[j].d < cand[i].d) { C t = cand[i]; cand[i] = cand[j]; cand[j] = t; }
		for (int i = 0; i < nc && nk < net::kMaxEnemies; i++) { bool dup = false; for (int k = 0; k < nk; k++) dup = dup || keep[k] == cand[i].id; if (!dup) { keep[nk] = cand[i].id; keepT[nk] = 0; nk++; } }
		if (nk != nFight) LOG("share: %d fighting you or near a friend (from your game)", nk);
		memcpy(fightRef, keep, 4 * nk); memcpy(fightDeadT, keepT, 8 * nk); nFight = nk;
	}
	static net::Enemy E[net::kMaxEnemies];
	bool wantSkel = now - skelT > 2;
	if (wantSkel) skelT = now;
	int n = 0;
	for (int i = 0; i < nFight; i++)
	{
		TESObjectREFR *a = refGone(fightRef[i]) ? 0 : (TESObjectREFR *)lookup(fightRef[i]);
		if (!a) continue;
		net::Enemy &e = E[n];
		e.ref = formKey(fightRef[i]);
		memcpy(e.pos, pos(a), 12); e.yaw = rot(a)[2];
		bool dead = deadNow(a);
		e.health = dead ? 0 : healthPct(a);
		u32 proc = handling::process(a);
		u32 we = proc ? ptr(proc + 0x114) : 0, w = we ? ptr(we + 8) : 0;
		e.weapon = w && formtype((void *)w) == 0x28 ? formKey(refid((void *)w)) : net::FormKey{0, 0};
		e.flags = (dead ? 1 : 0) | (proc && rd<u8>(proc + 0x135) ? 2 : 0);
		e.shots = shotsOf((u32)a);
		static net::Skel sk;
		e.hasPose = buildPose(thirdPersonNode(a), e.pose, wantSkel ? &sk : 0);
		if (wantSkel && e.hasPose) net::setEnemySkel(n, sk);
		n++;
	}
	net::setEnemies(E, n);
}
static void fightDamaged(const net::FormKey &k, float amount)
{
	TESObjectREFR *a = refOf(k);
	if (!a || amount <= 0 || amount > 1) return;
	for (int i = 0; i < nFight; i++) if (fightRef[i] == refid(a))
	{
		g_script->CallFunctionAlt(s_dmgPct, player(), 2, a, F(amount));
		LOG("share: a friend hit %08x for %.0f%% of its health", refid(a), amount * 100);
		return;
	}
}

struct Mirror
{
	bool used; u8 peer; u32 refId; net::FormKey key;
	u32 copy, holder, parent;
	BoneList bones; bool bonesFound;
	u32 gun, gunBone, weap; net::FormKey gunKey;
	Fx fx;
	u16 shots; bool shotsKnown; int shotsDue; double nextShot;
	double seen, moveT, dmgT, hpT;
	float lastAt[3]; bool lastHave;
	bool dead;
};
static Mirror mirrors[16];
static bool mirroredHere(u32 id) { for (auto &m : mirrors) if (m.used && m.refId == id) return true; return false; }
static bool sendingHere(u32 id) { for (int i = 0; i < nFight; i++) if (fightRef[i] == id) return true; return false; }
struct MirCache { u32 refId, base, keyLow, root, copy; double t; };
static MirCache mcache[8];
static void mcachePut(u32 id, u32 base, u32 keyLow, u32 root, u32 copy)
{
	MirCache *e = &mcache[0];
	for (auto &x : mcache) { if (!x.copy) { e = &x; break; } if (x.t < e->t) e = &x; }
	if (e->copy) rider::release(e->copy);
	e->refId = id; e->base = base; e->keyLow = keyLow; e->root = root; e->copy = copy; e->t = rt::now();
}
static u32 mcacheTake(u32 id, u32 base, u32 keyLow, u32 root)
{
	for (auto &x : mcache)
		if (x.copy && x.refId == id)
		{
			u32 c = x.copy; bool good = rt::now() - x.t < 60 && x.base == base && x.keyLow == keyLow && x.root == root;
			memset(&x, 0, sizeof x);
			if (good) return c;
			rider::release(c); return 0;
		}
	return 0;
}
static void mcachePrune() { for (auto &x : mcache) if (x.copy && rt::now() - x.t > 60) { rider::release(x.copy); memset(&x, 0, sizeof x); } }
static void mcacheClear() { for (auto &x : mcache) { if (x.copy) rider::release(x.copy); memset(&x, 0, sizeof x); } }
static void mirrorOff(Mirror &m, const char *why, bool keep = false)
{
	TESObjectREFR *a = refGone(m.refId) ? 0 : (TESObjectREFR *)lookup(m.refId);
	if (a) { setHidden(a, false); g_script->CallFunctionAlt(s_restrain, player(), 2, a, F(0)); }
	fxOff(m.fx);
	if (m.gun && m.gunBone) rider::detach(m.gunBone, m.gun);
	if (m.holder)
	{
		if (m.parent) { rider::detach(m.parent, m.holder); rider::release(m.parent); }
		if (m.copy) { rider::detach(m.holder, m.copy); if (keep && a) mcachePut(m.refId, ptr((u32)a + OFF_REF_BASE), m.key.low, thirdPersonNode(a), m.copy); else rider::release(m.copy); }
		rider::release(m.holder);
	}
	LOG("share: a friend's fighter %08x drawn by its own game again (%s)", m.refId, why);
	memset(&m, 0, sizeof m);
}
static void mirrorsTick(TESObjectREFR *pl)
{
	double now = rt::now();
	mcachePrune();
	bool madeThisTick = false;
	u32 n3 = thirdPersonNode(pl), par = n3 ? ptr(n3 + 0x18) : 0;
	const float *pp = pos(pl);
	for (int p = 0; p <= 3; p++)
	{
		static net::Enemy es[net::kMaxEnemies];
		int ne = net::peerEnemies(p, es, net::kMaxEnemies);
		for (int i = 0; i < ne; i++)
		{
			const net::Enemy &e = es[i];
			TESObjectREFR *a = refOf(e.ref);
			if (a && p == 0 && !net::hosting() && coop::inSession() && s_refOn && (rd<u32>((u32)a + 8) & 0x800) && !deadNow(a) && (formtype(a) == 0x3B || formtype(a) == 0x3C))
			{
				static u32 onDone[32]; static int nOn;
				u32 id = refid(a); bool done = false;
				for (int k = 0; k < 32; k++) done = done || onDone[k] == id;
				if (!done) { onDone[nOn++ % 32] = id; g_script->CallFunctionAlt(s_refOn, player(), 1, a); LOG("share: %08x is out in the host's world - out here too", id); }
				continue;
			}
			if (!a || !thirdPersonNode(a) || !par || (formtype(a) != 0x3B && formtype(a) != 0x3C)) continue;
			float dx = e.pos[0] - pp[0], dy = e.pos[1] - pp[1];
			if (dx * dx + dy * dy > 9000.0f * 9000.0f) continue;
			u32 id = refid(a);
			if (net::hosting() && sendingHere(id)) continue;
			Mirror *m = 0;
			for (auto &x : mirrors) if (x.used && x.refId == id) m = &x;
			bool localDead = deadNow(a);
			if (m && localDead && !m->dead && !(e.flags & 1) && rt::now() - myAttackT > 1.5) { m->dead = true; LOG("share: %08x died here, not by you - their game not told", id); }
			if (m && localDead && !m->dead && !(e.flags & 1))
			{
				m->dead = true;
				net::EvKill k; k.ref = e.ref;
				net::addEvent(net::EV_KILL, &k, sizeof k);
				LOG("share: you killed %08x (in %s's fight) - their game told", id, vis[p].name);
			}
			if (ptr((u32)a + 0x128) == (u32)pl && !(e.flags & 1) && !localDead) { if (m) mirrorOff(*m, "it's fighting you"); continue; }
			if (!m && deadNow(a)) continue;
			if (!m)
			{
				const float *ap = pos(a);
				float cx = ap[0] - e.pos[0], cy = ap[1] - e.pos[1], cz = ap[2] - e.pos[2];
				if (cx * cx + cy * cy + cz * cz > 1000.0f * 1000.0f) continue;
			}
			if (!m && madeThisTick) continue;
			if (!m)
			{
				madeThisTick = true;
				for (auto &x : mirrors) if (!x.used) { m = &x; break; }
				if (!m) continue;
				u32 c = mcacheTake(id, ptr((u32)a + OFF_REF_BASE), e.ref.low, thirdPersonNode(a)); bool kept = c != 0;
				if (!c) c = rider::copy(thirdPersonNode(a));
				u32 h = c ? ((u32(__cdecl *)())0xA5F030)() : 0;
				if (!h) { if (c) { if (!kept) rider::hold(c); rider::release(c); } continue; }
				memset(m, 0, sizeof *m);
				m->used = true; m->peer = (u8)p; m->refId = id; m->key = e.ref;
				if (!kept) rider::hold(c);
				rider::hold(h);
				rider::attach(h, c);
				m->copy = c; m->holder = h; m->parent = par; rider::hold(par);
				rider::attach(par, h);
				m->weap = weaponToHand(c);
				g_script->CallFunctionAlt(s_restrain, player(), 2, a, F(1));
				LOG("share: %08x (fighting %s) drawn from their game", id, vis[p].name);
			}
			if (m->parent != par) { rider::hold(par); rider::detach(m->parent, m->holder); rider::release(m->parent); m->parent = par; rider::attach(par, m->holder); }
			m->seen = now;
			setHidden(a, true);
			Xf W = feetFrame(e.pos, e.yaw);
			xfWriteLocal(m->holder, xfMul(xfInv(xfRead(m->parent, 0x68)), W));
			if (!m->bonesFound) { collectBones(m->copy, m->bones); m->bonesFound = true; }
			Xf root = xfId();
			xfWriteLocal(m->copy, root);
			static net::Skel sk;
			bool hs = net::peerEnemySkel(p, i, &sk);
			if (e.hasPose) applyPose(m->copy, m->bones, e.pose, hs ? &sk : 0);
			if (m->weap && s_weapKnown > 0) xfWriteLocal(m->weap, s_weapLocal);
			bool wantGun = (e.flags & 2) && e.weapon.low;
			if (wantGun && (m->gunKey.low != e.weapon.low || m->gunKey.modHash != e.weapon.modHash))
			{
				fxOff(m->fx);
				if (m->gun && m->gunBone) rider::detach(m->gunBone, m->gun);
				m->gun = m->gunBone = 0; m->gunKey = e.weapon;
				TESForm *w = formOf(e.weapon);
				const char *path = w && formtype(w) == 0x28 ? (const char *)ptr((u32)w + 0x40) : 0;
				u32 bone = m->weap;
				for (int k = 0; k < m->bones.n && !bone; k++) if (!rt::strcmp((const char *)ptr(m->bones.node[k] + 8), "Weapon")) bone = m->bones.node[k];
				u32 shared = path && path[0] && bone ? loadModel(path) : 0;
				u32 gc = shared ? rider::copy(shared) : 0;
				if (gc) { rider::attach(bone, gc); m->gun = gc; m->gunBone = bone; }
			}
			if (m->gun) { u32 &f = *(u32 *)(m->gun + OFF_NIAV_FLAGS); f = wantGun ? (f & ~1u) : (f | 1u); }
			updateNode(m->holder);
			if (m->lastHave)
			{
				float jx = e.pos[0] - m->lastAt[0], jy = e.pos[1] - m->lastAt[1], jz = e.pos[2] - m->lastAt[2];
				if (jx * jx + jy * jy + jz * jz > 1000.0f * 1000.0f) { memcpy(m->lastAt, e.pos, 12); mirrorOff(*m, "it went through a door or was moved there"); continue; }
			}
			memcpy(m->lastAt, e.pos, 12); m->lastHave = true;
			if (now - m->moveT > 0.5 && !rd<u32>(0x11F2250 + 8)) { m->moveT = now; moveRef(a, e.pos[0], e.pos[1], e.pos[2], 0, 0, e.yaw * 180.0f / rt::PI); }
			if (!(e.flags & 1) && now - m->hpT < 0.1) {}
			else if (e.flags & 1)
			{
				if (!m->dead && !deadNow(a)) { g_script->CallFunctionAlt(s_kill, player(), 1, a); LOG("share: %08x died in %s's fight", id, vis[p].name); }
				m->dead = true;
			}
			else
			{
				m->hpT = now;
				float mine = healthPct(a);
				if (mine > e.health + 0.03f) g_script->CallFunctionAlt(s_dmgPct, player(), 2, a, F(mine - e.health));
				else if (mine < e.health - 0.03f && now - m->dmgT > 0.5)
				{
					m->dmgT = now;
					net::EvDamage d; d.ref = e.ref; d.amount = e.health - mine;
					net::addEvent(net::EV_DAMAGE, &d, sizeof d);
					LOG("share: you hit %08x (in %s's fight) for %.0f%%", id, vis[p].name, d.amount * 100);
				}
			}
			if (!m->shotsKnown) { m->shots = e.shots; m->shotsKnown = true; }
			else if (e.shots != m->shots) { u16 k = (u16)(e.shots - m->shots); m->shots = e.shots; if (k < 50) { if (m->shotsDue <= 0) m->nextShot = now; m->shotsDue = rt::min(m->shotsDue + (int)k, 30); } }
			fxTick(m->fx);
			if (m->shotsDue > 0 && now >= m->nextShot)
			{
				TESForm *w = formOf(e.weapon);
				float rate = w ? rd<float>((u32)w + 0x134) : 0;
				float at[3] = {e.pos[0], e.pos[1], e.pos[2] + 100};
				shotSound(w, at);
				{
					float d[3]; aimDir(e.yaw, 0, d);
					shotFx(1 + (int)(m - mirrors) % 3, m->gun, w, at, d, m->fx);
				}
				m->shotsDue--;
				m->nextShot = now + rt::clamp(rate > 0.5f ? 1.0f / rate : 0.15f, 0.06f, 0.6f);
			}
		}
	}
	for (auto &m : mirrors) if (m.used && now - m.seen > 3) mirrorOff(m, "the friend's fight moved on", true);
}

static void unload()
{
	TESObjectREFR *pl = player();
	if (riding && pl) rideEnd(pl, "a game is being loaded", false);
	for (auto &v : vis) { if (v.made || v.carRef || v.oldCar) destroy(v); v.seen = false; }
	for (auto &m : mirrors) if (m.used) mirrorOff(m, "a game is being loaded");
	mcacheClear();
	if (dr.state) dressStop("a game is being loaded");
	proxiesOff();
	if (markRef) { TESObjectREFR *m = (TESObjectREFR *)lookup(markRef); if (m) removeRef(m); markRef = 0; }
	LOG("ghosts: everything of the friends' let go before the load");
}
void tradeAbort(const char *why);
void beforeSave()
{
	tradeAbort("the game is saving");
	int n = 0;
	for (auto &m : mirrors) if (m.used) { mirrorOff(m, "a save is being written", true); n++; }
	if (dr.ref) dressStop("a save is being written");
	proxiesOff();
	for (auto &v : vis) if (v.carRef || v.oldCar) carOff(v);
	if (markRef) { TESObjectREFR *m = (TESObjectREFR *)lookup(markRef); if (m) removeRef(m); markRef = 0; }
	if (n) LOG("share: %d fighter(s) of the friends' let go before the save", n);
}
static void quickLoadKey(bool session)
{
	static int set = -1;
	if (set == (int)session) return;
	set = session;
	console(session ? "DisableKey 67" : "EnableKey 67");
	LOG("net: quick load (F9) %s", session ? "off while playing together" : "on again");
}
static bool raw(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static u8 s_keysOffByUs[264 / 8 + 1];
static void gameKeys(bool off)
{
	if (!s_keyOff || !s_keyOn) return;
	for (int k = 1; k < 264; k++)
	{
		if (k > 221 && k < 256) continue;
		if (k == 256 + 2) continue;
		if (off)
		{
			NVSEElement e; memset(&e, 0, sizeof e);
			g_script->CallFunction(s_keyOff, player(), 0, &e, 1, F((float)k));
			bool was = e.type == 1 && e.num != 0;
			if (!was) s_keysOffByUs[k >> 3] |= (u8)(1 << (k & 7)); else s_keysOffByUs[k >> 3] &= (u8)~(1 << (k & 7));
		}
		else if (s_keysOffByUs[k >> 3] & (1 << (k & 7)))
		{
			g_script->CallFunctionAlt(s_keyOn, player(), 1, F((float)k));
			s_keysOffByUs[k >> 3] &= (u8)~(1 << (k & 7));
		}
	}
}
static void chatLine(const char *line);
static void chatClose(const char *why)
{
	if (!g_chatTyping) return;
	g_chatTyping = false;
	gameKeys(false);
	LOG("chat: the box closed (%s)", why);
}
static void chatTick(TESObjectREFR *pl, bool focus)
{
	static bool armed, prevK[256]; static double backT;
	bool menu = menumode();
	if (!g_chatTyping)
	{
		bool d = raw(VK_CHAT);
		if (menu || !focus) armed = false;
		else if (d && !prevK[VK_CHAT]) armed = true;
		else if (!d && armed)
		{
			armed = false;
			g_chatTyping = true; g_chatLen = 0; g_chatBuf[0] = 0;
			for (int k = 0; k < 256; k++) prevK[k] = raw(k);
			gameKeys(true);
			LOG("chat: the box opened");
		}
		prevK[VK_CHAT] = d;
		return;
	}
	if (menu || !focus) { chatClose(menu ? "a menu opened" : "the game lost focus"); return; }
	u8 ks[256]; memset(ks, 0, sizeof ks);
	if (raw(0x10)) ks[0x10] = 0x80;
	if (raw(0x11) && raw(0x12)) { ks[0x11] = 0x80; ks[0x12] = 0x80; }
	if (GetKeyState(0x14) & 1) ks[0x14] = 1;
	for (int k = 8; k < 256; k++)
	{
		bool d = raw(k), edge = d && !prevK[k];
		prevK[k] = d;
		if (k == 0x10 || k == 0x11 || k == 0x12 || (k >= 0xA0 && k <= 0xA5) || k == 0x14) continue;
		if (k == 0x08)
		{
			if (edge) { backT = rt::now() + 0.45; if (g_chatLen) g_chatBuf[--g_chatLen] = 0; }
			else if (d && rt::now() > backT && g_chatLen) { backT = rt::now() + 0.04; g_chatBuf[--g_chatLen] = 0; }
			continue;
		}
		if (!edge) continue;
		if (k == 0x1B) { chatClose("Esc"); return; }
		if (k == 0x0D)
		{
			char line[100]; rt::strcpy(line, g_chatBuf, sizeof line);
			chatClose("sent");
			prevK[VK_CHAT] = true;
			chatLine(line);
			return;
		}
		u16 out[2] = {0, 0};
		int n = ToAscii((u32)k, MapVirtualKeyA((u32)k, 0), ks, out, 0);
		char c = n == 1 ? (char)(out[0] & 0xFF) : 0;
		if (c >= 32 && c < 127 && g_chatLen < (int)sizeof g_chatBuf - 1) { g_chatBuf[g_chatLen++] = c; g_chatBuf[g_chatLen] = 0; }
	}
	(void)pl;
}
static void chatLine(const char *line)
{
	int s0 = 0; while (line[s0] == ' ') s0++;
	if (!line[s0]) { LOG("chat: an empty line"); return; }
	LOG("chat: typed \"%s\"", line + s0);
	char low[100]; int li = 0;
	for (; line[s0 + li] && li < 99; li++) { char c = line[s0 + li]; low[li] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
	low[li] = 0;
	if (low[0] == '/' && coop::chatCommand(low)) return;
	if (rt::streq(low, "/ranges")) { ranges::toggle(); return; }
	if (line[s0] == '/' && (line[s0 + 1] | 32) == 'f' && (line[s0 + 2] | 32) == 'f')
	{
		const char *a = line + s0 + 3; while (*a == ' ') a++;
		if ((a[0] | 32) == 'o' && (a[1] | 32) == 'n') g_friendlyFire = 1; else if ((a[0] | 32) == 'o' && (a[1] | 32) == 'f') g_friendlyFire = 0;
		radioKnobSave();
		char m[120]; rt::fmt(m, sizeof m, "Friendly fire %s%s", g_friendlyFire ? "on" : "off", net::hosting() || !net::active() ? " (your games' rule when you host)" : " when you host - your host's rule counts now");
		message(m, 4);
		return;
	}
	if (!memcmp(low, "/name ", 6))
	{
		const char *nm = line + s0 + 6;
		while (*nm == ' ') nm++;
		if (*nm)
		{
			rt::strcpy(g_playerName, nm, sizeof g_playerName);
			radioKnobSave();
			net::setName(g_playerName);
			char m[80]; rt::fmt(m, sizeof m, "You're %s to your friends now", g_playerName); message(m, 4);
		}
		return;
	}
	net::addEvent(net::EV_CHAT, line + s0, (u8)strlen(line + s0));
	char m[160]; rt::fmt(m, sizeof m, "You: %s", line + s0);
	message(m, 5, false);
	LOG("chat: %s", m);
}

static void carShow(Vis &v, int ci, const float *p, float yaw, float pitch, float roll, u32 cell, const char *who, const net::Self *drv, float dt)
{
	v.carWOk = false;
	if (v.carRef && refGone(refid(v.carRef))) { v.carRef = 0; v.carIdx = -1; }
	if (v.oldCar && refGone(refid(v.oldCar))) v.oldCar = 0;
	if (ci < 0) { carOff(v); return; }
	if (v.carRef && v.carIdx != ci) { removeRef(v.carRef); v.carRef = 0; }
	if (v.oldCar)
	{
		u32 nr = v.carRef ? thirdPersonNode(v.carRef) : 0;
		if ((nr && ptr(nr + 0x18)) || ++v.oldWait >= 30) { removeRef(v.oldCar); v.oldCar = 0; v.oldWait = 0; }
	}
	bool moved = false;
	if (v.carRef && cell != v.carCell && !v.oldCar)
	{
		TESObjectREFR *n = placeRef(kCars[ci].parked, p[0], p[1], p[2], yaw);
		if (n) { v.oldCar = v.carRef; v.oldWait = 0; v.carRef = n; v.carCell = cell; moved = true; LOG("ghosts: %s's car placed again in your new cell %x", who, cell ? refid((void *)cell) : 0); }
	}
	if (!v.carRef) { v.carRef = placeRef(kCars[ci].parked, p[0], p[1], p[2], yaw); v.carIdx = ci; v.carCell = cell; moved = true; }
	if (!v.carRef) return;
	if (drv)
	{
		Vehicle &V = v.cv;
		if (v.cvCar != ci) { memset(&V, 0, sizeof V); v.cvCar = ci; }
		if (V.refID != refid(v.carRef)) { V.refID = refid(v.carRef); V.wheelRestOK = false; }
		const CarState &cs = g_cars[ci];
		V.halfTrack = cs.halfTrack; V.halfBase = cs.halfBase; V.halfLength = cs.halfLength; V.wheelRadius = cs.wheelRadius;
		memcpy(V.steerAxis, cs.steerAxis, 12);
		V.x = p[0]; V.y = p[1]; V.z = p[2]; V.heading = yaw; V.speed = drv->speed;
		V.wheelie = drv->wheelie; V.steer = drv->steer;
		V.pitch = pitch * g_pitchSign - V.wheelie; V.roll = roll * g_rollSign;
		V.shakeZ = drv->shakeZ / 8.0f; V.shakePitch = drv->shakePitch / 20.0f; V.shakeRoll = drv->shakeRoll / 20.0f;
		for (int k = 0; k < 4; k++) V.wheelOff[k] = drv->wheelOff[k];
		pushRef(v.carRef, V);
		if (v.oldCar) pushRef(v.oldCar, V);
		turnWheels(V, dt); v.cvRested = false;
		v.carW = vehFrame(V); v.carWOk = true;
		memset(v.carAt, 0, sizeof v.carAt);
		return;
	}
	if (v.cvCar == ci && v.cv.refID == refid(v.carRef) && v.cv.wheelRestOK && !v.cvRested)
	{
		Vehicle &V = v.cv;
		memset(V.wheelOff, 0, sizeof V.wheelOff); V.steer = V.speed = V.shakeZ = V.shakePitch = V.shakeRoll = V.wheelie = 0;
		turnWheels(V, 0);
		v.cvRested = true;
	}
	float ax, ay, az;
	carAngles(yaw, pitch, roll, &ax, &ay, &az);
	float at[6] = {p[0], p[1], p[2], ax, ay, az};
	for (int k = 0; k < 6 && !moved; k++) if (rt::absf(at[k] - v.carAt[k]) > 0.05f) moved = true;
	if (!moved) return;
	memcpy(v.carAt, at, sizeof at);
	moveRef(v.carRef, p[0], p[1], p[2], ax, ay, az);
	if (v.oldCar) moveRef(v.oldCar, p[0], p[1], p[2], ax, ay, az);
}

static void xpTick(TESObjectREFR *pl);
static void tradeTick(TESObjectREFR *pl, bool focus);
static void objTick(TESObjectREFR *pl);
static void lootRollTick(TESObjectREFR *pl);
static void tick(float dt, TESObjectREFR *pl, bool quiet)
{
	if (!netInited)
	{
		netInited = true;
		net::logFn = logs;
		net::setBuild(kPluginBuild);
		char p[400];
		rt::fmt(p, sizeof p, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\net.ini", g_dir);
		net::init(p);
		if (g_playerName[0]) net::setName(g_playerName);
	}
	gprof::begin();
	bool focus = gameFocused();
	if (focus && pressed(VK_NETTEST))
	{
		bool on = !net::soloTesting();
		net::soloTest(on);
		message(on ? "Multiplayer test: you, 2 s ago (F10 to stop)" : "Multiplayer test off", 2);
		if (!on) { for (auto &v : vis) hide(v); if (riding) rideEnd(pl, "test off", true); }
	}
	g_friendSeat = false;
	askTick(focus);
	quickLoadKey(net::active() && !net::soloTesting());
	if (!net::active()) { chatClose("not playing together"); { void tradeNetGone(); tradeNetGone(); } if (riding) rideEnd(pl, "no game", true); { void downNetGone(TESObjectREFR *pl); downNetGone(pl); } return; }
	net::Self me;
	memset(&me, 0, sizeof me);
	u32 cell = ptr((u32)pl + OFF_REF_CELL);
	me.world = worldspace();
	me.cell = interior() && cell ? refid((void *)cell) : 0;
	if (TESForm *gh = lookup(0x38)) me.hour = rd<float>((u32)gh + 0x24);
	{
		u32 sky = ptr(0x11DEA20), w = sky ? ptr(sky + 0x10) : 0;
		u32 wid = w ? refid((void *)w) : 0;
		me.weather = (wid >> 24) == 0 ? wid : 0;
		me.snow = snowfx::on ? (u8)snowfx::level : 0;
	}
	me.parkCar = -1;
	if (g_driving)
	{
		me.mode = 1; me.car = (i16)g_activeCar;
		me.pos[0] = g_car.x; me.pos[1] = g_car.y; me.pos[2] = g_car.z;
		me.yaw = g_car.heading; me.pitch = (g_car.pitch + g_car.wheelie) * g_pitchSign; me.roll = g_car.roll * g_rollSign;
		me.speed = g_car.speed;
		auto i8c = [](float x) { return (i8)rt::clamp(x < 0 ? x - 0.5f : x + 0.5f, -127, 127); };
		for (int k = 0; k < 4; k++) me.wheelOff[k] = i8c(g_car.wheelOff[k]);
		me.steer = i8c(g_car.steer); me.wheelie = i8c(g_car.wheelie);
		me.shakeZ = i8c(g_car.shakeZ * 8); me.shakePitch = i8c(g_car.shakePitch * 20); me.shakeRoll = i8c(g_car.shakeRoll * 20);
		me.invite = rt::now() < inviteUntil ? 1 : 0;
	}
	else
	{
		const float *p = pos(pl), *r = rot(pl);
		memcpy(me.pos, p, 12);
		me.yaw = r[2]; me.pitch = r[0];
		u32 proc = handling::process(pl);
		me.flags = (sprint::mode ? 1 : 0) | ((handling::moveFlags(pl) & 0x400) ? 2 : 0) | (proc && rd<u8>(proc + 0x135) ? 4 : 0) | (move::state ? 8 : 0);
		me.car = -1;
		me.ride = riding ? (u8)(rideWith + 1) : 0;
		tpgun::gunQ(me.gunQ);
		static net::Self park;
		static u32 parkFor;
		if (g_parkedRef && g_activeCar >= 0)
		{
			TESObjectREFR *pr = (TESObjectREFR *)lookup(g_parkedRef);
			if (pr && !refGone(g_parkedRef))
			{
				const float *cp = pos(pr);
				memcpy(park.parkPos, cp, 12);
				bool pose = g_parkedPoseRef == g_parkedRef;
				park.parkYaw = pose ? g_parkedPose.heading : rot(pr)[2] * 180.0f / rt::PI;
				park.parkPitch = pose ? g_parkedPose.pitch * g_pitchSign : 0; park.parkRoll = pose ? g_parkedPose.roll * g_rollSign : 0;
				cellKeys(ptr((u32)pr + OFF_REF_CELL), &park.parkWorld, &park.parkCell);
				parkFor = g_parkedRef;
			}
			if (parkFor == g_parkedRef)
			{
				me.parkCar = (i16)g_activeCar;
				memcpy(me.parkPos, park.parkPos, 12); me.parkYaw = park.parkYaw; me.parkPitch = park.parkPitch; me.parkRoll = park.parkRoll;
				me.parkWorld = park.parkWorld; me.parkCell = park.parkCell;
			}
		}
		u32 we = proc ? ptr(proc + 0x114) : 0, w = we ? ptr(we + 8) : 0;
		if (w && formtype((void *)w) == 0x28) me.weapon = formKey(refid((void *)w));
		me.shots = (u16)handling::shotsTotal;
		{
			void downedState(u8 *health, u8 *status, u8 *left);
			downedState(&me.health, &me.status, &me.downLeft);
			if (me.status & 1) me.yaw = downFacing();
			void scaleFill(net::Self &me);
			scaleFill(me);
		}
		static double poseT, skelT;
		static net::Pose pose;
		static net::Skel skel;
		static bool poseOk, told;
		if (!riding && rt::now() - poseT > 0.9f / rt::max(TUNE(netSendHz), 5.0f))
		{
			poseT = rt::now();
			bool wantSkel = rt::now() - skelT > 2.0;
			u32 pkb = move::poseBody();
			if (me.status & 1)
			{
				Xf F = feetFrame(pos(pl), me.yaw);
				u32 n3 = thirdPersonNode(pl);
				if (n3) { float sc = xfRead(n3, 0x68).s; if (sc > 0.01f && sc < 100) F.s = sc; }
				wantSkel = false; poseOk = buildPose(n3, pose, 0, &F);
			}
			else if (pkb)
			{
				Xf F = feetFrame(pos(pl), rot(pl)[2]);
				u32 n3 = thirdPersonNode(pl);
				if (n3) { float sc = xfRead(n3, 0x68).s; if (sc > 0.01f && sc < 100) F.s = sc; }
				wantSkel = false; poseOk = buildPose(pkb, pose, 0, &F);
			}
			else poseOk = buildPose(thirdPersonNode(pl), pose, wantSkel ? &skel : 0);
			static bool pkTold; if (pkb && !pkTold && poseOk) { pkTold = true; LOG("ghosts: friends see your vault/climb/kick (the move's body, %d bones)", pose.n); }
			if (wantSkel && poseOk) { skelT = rt::now(); net::setSkel(skel); }
			if (!told && poseOk) { told = true; LOG("ghosts: your body - %d bones sent", pose.n); }
		}
		if (poseOk) { me.hasPose = true; me.pose = pose; }
		static double lookT; static u32 lookWas;
		if (rt::now() - lookT > 2.0)
		{
			lookT = rt::now();
			static LookPk L;
			lookBuild(pl, L);
			if (L.nFace) { net::setLook(&L, sizeof L); if (L.sum != lookWas) { lookWas = L.sum; LOG("ghosts: your look for friends - %s, %d worn things, %d head parts", L.female ? "female" : "male", (int)L.nArmor, (int)L.nParts); } }
		}
		static double modsT;
		if (rt::now() - modsT > 3.0)
		{
			modsT = rt::now();
			if (modCount < 0) modsBuild();
			if (modCount > 0) net::setMods(modHash, modCount);
			net::Ghost gs[4];
			int ng = net::ghosts(gs, 4);
			for (int i = 0; i < ng; i++) if (gs[i].id < 4) modsCompare(gs[i].name, gs[i].id);
		}
	}
	me.rules = g_friendlyFire ? 1 : 0;
	net::setSendHz(TUNE(netSendHz));
	net::tick(rt::now(), me);
	lastMe = me;
	gprof::lap(gprof::NET);
	{
		static double lootT;
		if (rt::now() - lootT > 0.5) { lootT = rt::now(); lootTick(pl, me); }
	}
	CRUMB("playing together: friends' events");
	{ void tradeLoadCheck(); tradeLoadCheck(); }
	eventsTick();
	gprof::lap(gprof::EVENTS);
	{
		static double fightT;
		CRUMB("playing together: your fights");
		if (!riding && rt::now() - fightT > 1.0 / 15) { fightT = rt::now(); fightsTick(pl); }
	}
	xpTick(pl);
	lootRollTick(pl);
	gprof::lap(gprof::FIGHTS);
	CRUMB("playing together: friends' fights");
	mirrorsTick(pl);
	gprof::lap(gprof::MIRRORS);
	CRUMB("playing together: doors");
	doorsTick(focus);
	tradeTick(pl, focus);
	objTick(pl);
	gprof::lap(gprof::DOORS);
	CRUMB("playing together: chat");
	chatTick(pl, focus);
	gprof::lap(gprof::CHAT);
	CRUMB("playing together: friends");
	float hh;
	if ((sinceHour += dt) > 5 && net::hostHour(&hh))
	{
		sinceHour = 0;
		if (TESForm *gh = lookup(0x38))
		{
			float &mine = *(float *)((u32)gh + 0x24), d = hh - mine;
			if (d > 12) d -= 24; else if (d < -12) d += 24;
			if (rt::absf(d) > 0.05f) { LOG("net: game hour %.2f -> the host's %.2f", mine, hh); mine = hh; }
		}
	}
	static float sinceSky;
	u32 hw; u8 hs;
	if ((sinceSky += dt) > 4 && net::hostSky(&hw, &hs))
	{
		sinceSky = 0;
		if (hs && (!snowfx::on || snowfx::level != hs))
		{
			snowfx::on = true; snowfx::mode = hs; snowfx::level = hs; snowfx::netSet = true;
			LOG("net: snow as the host's (%d)", (int)hs);
		}
		else if (!hs && snowfx::on && snowfx::netSet) { snowfx::on = false; snowfx::netSet = false; LOG("net: snow off as the host's"); }
		u32 sky = ptr(0x11DEA20), w = sky ? ptr(sky + 0x10) : 0, wid = w ? refid((void *)w) : 0;
		if (!hs && hw && wid != hw && worldspace())
		{
			char cmd[48];
			rt::fmt(cmd, sizeof cmd, "ForceWeather %08X 0", hw);
			console(cmd);
			LOG("net: weather %08X -> the host's %08X", wid, hw);
		}
	}
	dressTick(pl);
	gprof::lap(gprof::SKY);
	CRUMB("playing together: friends' bodies for hits");
	proxiesTick(pl);
	gprof::lap(gprof::PROXIES);
	CRUMB("playing together: downed");
	downedTick(pl, focus);
	gprof::lap(gprof::DOWNED);
	CRUMB("playing together: the guest's scaling");
	{ void scaleTickG(TESObjectREFR *pl); scaleTickG(pl); }
	CRUMB("playing together: character creation");
	{ void charGenTick(); charGenTick(); }
	gprof::lap(gprof::SCALE);
	s_n = net::ghosts(s_gs, 4);
	bool seen[4] = {false, false, false, false};
	u8 mine = net::myId();
	for (int i = 0; i < s_n; i++)
	{
		const net::Ghost &g = s_gs[i];
		int slot = g.id < 4 ? g.id : 3;
		Vis &v = vis[slot];
		seen[slot] = true;
		if (!v.seen)
		{
			v.seen = true; rt::strcpy(v.name, g.name, sizeof v.name);
			u32 b = net::peerBuild(g.id);
			char m[120];
			if (b && b != kPluginBuild) rt::fmt(m, sizeof m, "%s is here - but on Mojave Enhanced plugin %u (you: %u): update in setup", g.name, b, kPluginBuild);
			else rt::fmt(m, sizeof m, "%s is here", g.name);
			message(m, b && b != kPluginBuild ? 8.0f : 3.0f);
			LOG("ghosts: %s appears (plugin %u)", g.name, b);
		}
		if (!rt::streq(v.name, g.name)) { char m[80]; rt::fmt(m, sizeof m, "%s is now %s", v.name, g.name); message(m, 3); rt::strcpy(v.name, g.name, sizeof v.name); }
		bool here = g.fresh && g.s.world == me.world && g.s.cell == me.cell;
		int ci = -1; float cp[3] = {0, 0, 0}, cy = 0, cpitch = 0, croll = 0;
		if (g.s.mode == 1 && g.s.car >= 0 && g.s.car < kNumCars) { if (here) { ci = g.s.car; memcpy(cp, g.s.pos, 12); cy = g.s.yaw; cpitch = g.s.pitch; croll = g.s.roll; } }
		else if (g.s.parkCar >= 0 && g.s.parkCar < kNumCars && g.fresh && g.s.parkWorld == me.world && g.s.parkCell == me.cell)
		{
			ci = g.s.parkCar; memcpy(cp, g.s.parkPos, 12); cy = g.s.parkYaw; cpitch = g.s.parkPitch; croll = g.s.parkRoll;
		}
		if (ci >= 0 && !g_cars[ci].available)
		{
			static u32 told;
			if (told != (u32)ci + 1) { told = (u32)ci + 1; LOG("ghosts: %s's car %s isn't on this PC - drawn as themselves", g.name, kCars[ci].id); }
			ci = -1;
		}
		carShow(v, ci, cp, cy, cpitch, croll, cell, g.name, ci >= 0 && g.s.mode == 1 && here ? &g.s : 0, dt);
		if (g.s.mode == 1 && g.s.invite && here && !riding && !g_driving && rt::now() - v.inviteTold > 16)
		{
			const float *pp = pos(pl);
			float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1];
			if (dx * dx + dy * dy < 1800.0f * 1800.0f) { v.inviteTold = rt::now(); char m[80]; rt::fmt(m, sizeof m, "%s offers you a ride - press G", g.name); message(m, 5); }
		}
		if (!here) { hideBody(v); gunTick(v, g, false); v.here = 0; continue; }
		{
			static u8 lb[net::kMaxLook];
			int ln = net::peerLook(g.id, lb, sizeof lb);
			const LookPk *L = ln == (int)sizeof(LookPk) ? (const LookPk *)lb : 0;
			if (L && L->sum != lookSum(*L)) L = 0;
			if (!v.here) v.here = rt::now();
			if (L && L->sum != v.lookSum && !dressing() && canDress() && !riding) dressStart(slot, *L, pl);
			if (!v.made)
			{
				bool wait = canDress() && rt::now() - v.here < 12 && ((dressing() && dr.slot == slot) || !L || L->sum != v.lookSum);
				if (wait) { gunTick(v, g, false); continue; }
				if (!make(v, pl)) continue;
				LOG("ghosts: %s drawn as a copy of you - %s", g.name, !canDress() ? "stand-ins can't be made here" : L ? "their stand-in couldn't be dressed" : "their look never came (an older plugin?)");
			}
		}
		reparent(v, pl);
		bool seated = false;
		if (g.s.mode == 1 && ci >= 0 && v.carRef) { seat(v, v.carWOk ? v.carW : carFrame(cp, cy, cpitch, croll), ci, -1); seated = true; }
		else if (g.s.mode == 1) { hideBody(v); gunTick(v, g, false); continue; }
		else if (g.s.ride)
		{
			int t = g.s.ride - 1;
			if (t == mine && g_driving && g_activeCar >= 0) { seat(v, myCarFrame(), g_activeCar, 1); seated = true; g_friendSeat = true; }
			else
				for (int j = 0; j < s_n; j++)
				{
					const net::Ghost &d = s_gs[j];
					Vis &dv = vis[d.id < 4 ? d.id : 3];
					if (d.id != t || d.s.mode != 1 || !dv.carRef || dv.carIdx < 0) continue;
					seat(v, dv.carWOk ? dv.carW : carFrame(d.s.pos, d.s.yaw, d.s.pitch, d.s.roll), dv.carIdx, 1);
					seated = true;
				}
		}
		if (!seated)
		{
			static net::Skel sk;
			bool hs = net::peerSkel(g.id, &sk);
			if (g.s.hasPose) poseStream(v, g, hs ? &sk : 0);
			else poseWalk(v, g, dt);
		}
		if (!v.shown) { *(u32 *)(v.holder + OFF_NIAV_FLAGS) &= ~1u; v.shown = true; }
		gunTick(v, g, !seated);
		if (!v.shotsKnown) { v.shots = g.s.shots; v.shotsKnown = true; }
		else if (g.s.shots != v.shots)
		{
			u16 n = (u16)(g.s.shots - v.shots);
			v.shots = g.s.shots;
			if (!seated && n < 50) { if (v.shotsDue <= 0) v.nextShot = rt::now(); v.shotsDue = rt::min(v.shotsDue + (int)n, 30); }
		}
		fxTick(v.fx);
		if (v.shotsDue > 0 && rt::now() >= v.nextShot)
		{
			TESForm *w = formOf(g.s.weapon);
			float rate = w ? rd<float>((u32)w + 0x134) : 0;
			float at[3] = {g.s.pos[0], g.s.pos[1], g.s.pos[2] + 110};
			shotSound(w, at);
			{
				float eye[3] = {g.s.pos[0], g.s.pos[1], g.s.pos[2] + ((g.s.flags & 2) ? 80.0f : 115.0f)}, d[3];
				aimDir(g.s.yaw, g.s.pitch, d);
				shotFx(slot, v.gun, w, eye, d, v.fx);
			}
			v.shotsDue--;
			v.nextShot = rt::now() + rt::clamp(rate > 0.5f ? 1.0f / rate : 0.12f, 0.06f, 0.5f);
		}
	}
	for (int s = 0; s < 4; s++)
		if (!seen[s] && vis[s].seen)
		{
			vis[s].seen = false;
			hide(vis[s]);
			char m[64]; rt::fmt(m, sizeof m, "%s left", vis[s].name); message(m, 3);
			LOG("ghosts: %s gone", vis[s].name);
		}
	for (int s = 0; s < 4; s++) { g_ghostCars[2 * s] = vis[s].carRef ? refid(vis[s].carRef) : 0; g_ghostCars[2 * s + 1] = vis[s].oldCar ? refid(vis[s].oldCar) : 0; }
	if (riding) rideTick(pl, dt, focus);
	gprof::lap(gprof::FRIENDS);
	CRUMB("the game (after ours)");
}
static const u32 kStimpak = 0x15169, kCaps = 0xF;
struct Down { bool on; double t0; float healthMax; bool controlsOff; double shieldUntil, knockT; float facing; };
static const int kDownSecs = 20;
static Down down;
static bool coopNow()
{
	if (!net::active()) return false;
	if (net::soloTesting()) return true;
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	for (int i = 0; i < n; i++) if (gs[i].fresh) return true;
	return false;
}
static float myHealthPct(TESObjectREFR *pl) { return healthPct(pl); }
static int myCount(u32 id)
{
	TESForm *f = lookup(id);
	NVSEElement e; memset(&e, 0, sizeof e);
	if (!f || !s_itemCount || !g_script->CallFunction(s_itemCount, player(), 0, &e, 1, f) || e.type != 1) return 0;
	return (int)e.num;
}
static float myHealth()
{
	NVSEElement e; memset(&e, 0, sizeof e);
	if (!s_healthNow || !g_script->CallFunction(s_healthNow, player(), 0, &e, 0) || e.type != 1) return -1;
	return (float)e.num;
}
static void reviveTick(TESObjectREFR *pl, bool focus);
static void toOne()
{
	float h = myHealth();
	if (h <= 1) return;
	char line[80]; rt::fmt(line, sizeof line, "player.DamageAV Health %d", (int)(h - 1));
	console(line);
}
static void healTo(TESObjectREFR *pl, float pct)
{
	float now = myHealthPct(pl);
	if (now >= pct) return;
	char line[80];
	rt::fmt(line, sizeof line, "player.RestoreAV Health %d", (int)((pct - now) * rt::max(down.healthMax, 50.0f)) + 1);
	console(line);
}
static int guardMode = -1, guardWas;
static void guardSet(int mode)
{
	if (!s_immortal || mode == guardMode) return;
	NVSEElement e; memset(&e, 0, sizeof e);
	g_script->CallFunction(s_immortal, player(), 0, &e, 1, F((float)mode));
	if (guardMode < 0) guardWas = e.type == 1 ? (int)e.num : 0;
	guardMode = mode;
	LOG("downed: immortal mode %d (%s)", mode, mode == 1 ? "playing together: no death, downed instead" : mode == 2 ? "downed: no more hits" : "off");
}
static void guardOff() { if (guardMode >= 0 && s_immortal) { NVSEElement e; memset(&e, 0, sizeof e); g_script->CallFunction(s_immortal, player(), 0, &e, 1, F((float)guardWas)); LOG("downed: immortal mode back to %d", guardWas); } guardMode = -1; }
static void standUp(TESObjectREFR *pl, float pct, const char *how)
{
	if (!down.on) return;
	down.on = false;
	healTo(pl, pct);
	if (down.controlsOff) { console("EnablePlayerControls"); down.controlsOff = false; }
	if (guardMode == 2) guardSet(1);
	char m[120]; rt::fmt(m, sizeof m, "You're up (%s).", how);
	message(m, 4);
	LOG("downed: up - %s, health %.0f%%", how, myHealthPct(pl) * 100);
}
static void goDown(TESObjectREFR *pl, const char *why)
{
	if (down.on || g_driving) return;
	if (riding) rideEnd(pl, "downed", true);
	down.on = true; down.t0 = rt::now(); down.facing = rot(pl)[2];
	console("DisablePlayerControls 1 0 1 0 1 0 1");
	down.controlsOff = true;
	guardSet(2);
	if (s_knockDown && TUNE(coopDownRagdoll) > 0) { g_script->CallFunctionAlt(s_knockDown, player(), 1, F(TUNE(coopDownRagdoll))); down.knockT = rt::now(); }
	char m[200];
	rt::fmt(m, sizeof m, "You're down! %d s for a friend to get you up (E) - or 1: a stimpak (%d)", kDownSecs, myCount(kStimpak));
	message(m, 6);
	LOG("downed: %s", why);
}
void deathGuardTick(TESObjectREFR *pl)
{
	if (!coopNow()) { guardOff(); return; }
	if (s_immortal)
	{
		guardSet(1);
		float h = myHealth();
		if (h >= 0 && h < 1.5f) goDown(pl, "a hit that would have killed you");
		return;
	}
	float p = myHealthPct(pl);
	if (p > 0 && p <= 0.02f) { toOne(); goDown(pl, "nearly killed"); }
}
void hurtMe(float amount, bool fromPlayer, const char *who)
{
	TESObjectREFR *pl = player();
	if (!pl || down.on) return;
	float cur = myHealth();
	if (cur < 0) cur = amount + 1;
	if (amount >= cur && coopNow()) { char why[120]; rt::fmt(why, sizeof why, "%s by %s", fromPlayer ? "shot" : "hurt", who); toOne(); goDown(pl, why); return; }
	if (s_hurtMortal) g_script->CallFunctionAlt(s_hurtMortal, player(), 1, F(amount));
	else { char line[80]; rt::fmt(line, sizeof line, "player.DamageAV Health %d", (int)(amount + 0.5f)); console(line); }
	LOG("hits: you were hit for %.0f by %s (%s) - health %.0f -> %.0f", amount, who, fromPlayer ? "friendly fire" : "their game's people", cur, myHealth());
	if (fromPlayer) { static double told; if (rt::now() - told > 4) { told = rt::now(); char m[80]; rt::fmt(m, sizeof m, "%s hit you!", who); message(m, 2); } }
}
void revivedBy(const char *who, int how)
{
	if (!down.on) return;
	char h[80]; rt::fmt(h, sizeof h, how == 2 ? "%s used a stimpak on you" : "%s got you up", who);
	standUp(player(), how == 2 ? 0.5f : 0.25f, h);
}
float downFacing() { return down.facing; }
void downNetGone(TESObjectREFR *pl)
{
	if (down.on) standUp(pl, 0.25f, "not playing together any more");
	guardOff();
}
void downedState(u8 *health, u8 *status, u8 *left)
{
	TESObjectREFR *pl = player();
	float pct = pl ? myHealthPct(pl) : 1;
	*health = (u8)rt::clamp(pct * 255.0f, 0, 255);
	*status = down.on ? 1 : 0;
	*left = down.on ? (u8)rt::clamp(kDownSecs - (float)(rt::now() - down.t0), 0, 60) : 0;
}
struct Spot { bool have; float p[3]; u32 world, cell; };
static Spot safeSpot, entrySpot;
static void spotsTick(TESObjectREFR *pl)
{
	u32 c = ptr((u32)pl + OFF_REF_CELL), cid = c ? refid((void *)c) : 0, w = worldspace();
	static u32 lastCell;
	const float *pp = pos(pl);
	if (cid != lastCell) { lastCell = cid; entrySpot.have = true; memcpy(entrySpot.p, pp, 12); entrySpot.world = w; entrySpot.cell = cid; safeSpot.have = false; }
	static double t;
	if (rt::now() - t < 3) return;
	t = rt::now();
	if (myHealthPct(pl) > 0.5f && nFight == 0 && !g_driving) { safeSpot.have = true; memcpy(safeSpot.p, pp, 12); safeSpot.world = w; safeSpot.cell = cid; }
}
static void die(TESObjectREFR *pl)
{
	int caps = myCount(kCaps), lose = caps < 20 ? caps : 20;
	if (lose > 0) { char line[64]; rt::fmt(line, sizeof line, "player.RemoveItem 0000000F %d 1", lose); console(line); }
	float to[3]; const float *pp = pos(pl); memcpy(to, pp, 12);
	const char *where = "where you fell";
	u32 cid = ptr((u32)pl + OFF_REF_CELL) ? refid((void *)ptr((u32)pl + OFF_REF_CELL)) : 0;
	net::Ghost gs[4]; int n = net::ghosts(gs, 4); bool atFriend = false;
	u32 myWorld = worldspace(), myCell = interior() ? cid : 0;
	for (int i = 0; i < n && !atFriend; i++)
	{
		const net::Ghost &g = gs[i];
		if (!g.fresh || (g.s.status & 1) || g.s.mode != 0 || ((g.s.world ^ myWorld) & 0xFFFFFF) || ((g.s.cell ^ myCell) & 0xFFFFFF) || (!g.s.cell) != (!myCell)) continue;
		if (g.id == 3 && net::soloTesting()) continue;
		to[0] = g.s.pos[0] + 90 * rt::cos(g.s.yaw); to[1] = g.s.pos[1] - 90 * rt::sin(g.s.yaw); to[2] = g.s.pos[2] + 10;
		atFriend = true; where = g.name;
	}
	if (!atFriend && safeSpot.have && safeSpot.cell == cid) { memcpy(to, safeSpot.p, 12); to[2] += 10; where = "the last safe place you stood"; }
	else if (!atFriend && entrySpot.have && entrySpot.cell == cid) { memcpy(to, entrySpot.p, 12); to[2] += 10; where = "where you came in"; }
	down.on = false;
	if (down.controlsOff) { console("EnablePlayerControls"); down.controlsOff = false; }
	((void(__thiscall *)(u32, const float *))0x575830)((u32)pl, to);
	healTo(pl, 0.5f);
	down.shieldUntil = rt::now() + 3;
	char m[200];
	if (atFriend) rt::fmt(m, sizeof m, "You died. %d caps lost%s. Back on your feet next to %s.", lose, caps ? "" : " (you had none)", where);
	else rt::fmt(m, sizeof m, "You died. %d caps lost%s. Back on your feet at %s.", lose, caps ? "" : " (you had none)", where);
	message(m, 6);
	LOG("downed: died (nobody came in %d s) - %d caps lost, back at %s (%.0f %.0f %.0f)", kDownSecs, lose, where, to[0], to[1], to[2]);
}
static void downedTick(TESObjectREFR *pl, bool focus)
{
	if (!down.on)
	{
		static double readT;
		if (rt::now() - readT > 2) { readT = rt::now(); float p = myHealthPct(pl), hp = myHealth(); if (p > 0.05f && hp > 0) down.healthMax = hp / p; }
		spotsTick(pl);
		if (down.shieldUntil && rt::now() > down.shieldUntil) { down.shieldUntil = 0; if (guardMode == 2) guardSet(1); }
		void deathGuardTick(TESObjectREFR *pl);
		if (!down.shieldUntil) deathGuardTick(pl);
		reviveTick(pl, focus);
		return;
	}
	if (!coopNow()) { standUp(pl, 0.25f, "not playing together any more"); return; }
	double t = rt::now() - down.t0;
	static double checkT;
	if (s_knockDown && s_knocked && TUNE(coopDownRagdoll) > 0 && rt::now() - down.knockT > 1.5 * (t < 2) && rt::now() - checkT > 0.25)
	{
		checkT = rt::now();
		NVSEElement e; memset(&e, 0, sizeof e);
		g_script->CallFunction(s_knocked, player(), 0, &e, 0);
		int ks = e.type == 1 ? (int)e.num : -1;
		if (ks == 0 || ks == 2) { g_script->CallFunctionAlt(s_knockDown, player(), 1, F(TUNE(coopDownRagdoll) * 0.3f)); down.knockT = rt::now(); static int told; if (told++ < 10) LOG("downed: knocked over again (state %d)", ks); }
	}
	static bool k1;
	bool d1 = focus && held('1') && !menumode();
	if (d1 && !k1)
	{
		if (myCount(kStimpak) > 0) { console("player.RemoveItem 00015169 1 1"); standUp(pl, 0.6f, "a stimpak"); }
		else message("No stimpak.", 2);
	}
	k1 = d1;
	if (!down.on) return;
	if (myHealthPct(pl) > 0.2f) { standUp(pl, 0.2f, "healed"); return; }
	if (t > kDownSecs) { die(pl); return; }
	static int said;
	int left = kDownSecs - (int)t;
	if ((left == 10 || left == 5) && left != said) { said = left; char m[120]; rt::fmt(m, sizeof m, "Down - %d s left. 1: a stimpak, or a friend's E", left); message(m, 3); }
}
static void reviveTick(TESObjectREFR *pl, bool focus)
{
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	const float *pp = pos(pl);
	static bool eWas; bool e = focus && held('E') && !menumode();
	static double toldT[4];
	for (int i = 0; i < n; i++)
	{
		const net::Ghost &g = gs[i];
		int slot = g.id < 4 ? g.id : 3;
		if (!g.fresh || !(g.s.status & 1)) continue;
		float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1], d = rt::sqrt(dx * dx + dy * dy);
		bool here = !((g.s.world ^ worldspace()) & 0xFFFFFF);
		if (rt::now() - toldT[slot] > 5)
		{
			toldT[slot] = rt::now();
			float yawTo = rt::atan2(dx, dy), rel = yawTo - rot(pl)[2];
			while (rel > rt::PI) rel -= 2 * rt::PI; while (rel < -rt::PI) rel += 2 * rt::PI;
			const char *side = rel > 0.8f ? "to your right" : rel < -0.8f ? "to your left" : (rel > 2.4f || rel < -2.4f) ? "behind you" : "ahead";
			if (rel > 2.4f || rel < -2.4f) side = "behind you";
			char m[160];
			if (here) rt::fmt(m, sizeof m, "%s is down %.0f m %s - %d s left%s", g.name, d / 70, side, (int)g.s.downLeft, d < 160 ? " - press E" : "");
			else rt::fmt(m, sizeof m, "%s is down - %d s left", g.name, (int)g.s.downLeft);
			message(m, 3);
		}
		if (e && !eWas && here && d < 160 && !down.on)
		{
			bool free = true;
			net::EvRevive r; r.to = g.id; r.how = 1;
			if (slot == 3 && net::soloTesting()) { message("(test) you'd get your echo up", 2); }
			else net::addEvent(net::EV_REVIVE, &r, sizeof r);
			char m[100]; rt::fmt(m, sizeof m, free ? "You get %s up." : "You use a stimpak on %s.", g.name);
			message(m, 3);
			LOG("downed: you got %s up (%s)", g.name, free ? "free" : "a stimpak");
			toldT[slot] = rt::now() + 10;
		}
	}
	eWas = e;
}
static int myLevel() { NVSEElement e; memset(&e, 0, sizeof e); return s_myLevel && g_script->CallFunction(s_myLevel, player(), 0, &e, 0) && e.type == 1 ? (int)e.num : 0; }
static float gunRatio(TESForm *w)
{
	if (!w || formtype(w) != 0x28 || !s_calcDmg || !s_baseDmg) return 0;
	NVSEElement a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
	if (!g_script->CallFunction(s_calcDmg, player(), 0, &a, 1, w) || a.type != 1) return 0;
	if (!g_script->CallFunction(s_baseDmg, player(), 0, &b, 1, w) || b.type != 1 || b.num < 0.5) return 0;
	return (float)(a.num / b.num);
}
static float healthBoostOn, dmgMult = 1;
static float myMaxNow; static int myLevelNow;
void scaleFill(net::Self &me)
{
	static double t; static u16 dmgFor[4];
	if (rt::now() - t > 2)
	{
		t = rt::now();
		myLevelNow = myLevel();
		if (down.healthMax > 0) myMaxNow = down.healthMax - healthBoostOn;
		static bool told;
		if (!told && player())
		{
			u32 proc = handling::process(player()), we = proc ? ptr(proc + 0x114) : 0, w = we ? ptr(we + 8) : 0;
			if (w) { told = true; LOG("scaling: you are level %d, %.0f health; your gun %08x does %.2fx its own damage", myLevelNow, myMaxNow, refid((void *)w), gunRatio((TESForm *)w)); }
		}
		memset(dmgFor, 0, sizeof dmgFor);
		if (net::hosting())
		{
			net::Ghost gs[4]; int n = net::ghosts(gs, 4);
			for (int i = 0; i < n; i++)
			{
				if (!gs[i].fresh || gs[i].id > 3 || !gs[i].s.weapon.low) continue;
				float r = gunRatio(formOf(gs[i].s.weapon));
				dmgFor[gs[i].id] = (u16)rt::clamp(r * 1000, 0, 65000);
			}
		}
	}
	me.level = (u8)rt::clamp((float)myLevelNow, 0, 255);
	me.maxHealth = (u16)rt::clamp(myMaxNow, 0, 65000);
	memcpy(me.dmgFor, dmgFor, sizeof dmgFor);
}
static void scaleOff(const char *why)
{
	if (healthBoostOn > 0.5f) { char line[64]; rt::fmt(line, sizeof line, "player.ModAV Health %d", -(int)(healthBoostOn + 0.5f)); console(line); }
	if (healthBoostOn > 0.5f || dmgMult > 1.01f) LOG("scaling: off (%s)", why);
	healthBoostOn = 0; dmgMult = 1;
}
void scaleLoaded() { healthBoostOn = 0; dmgMult = 1; }
static void scaleTick(TESObjectREFR *pl)
{
	static double t;
	if (rt::now() - t < 2) return;
	t = rt::now();
	net::Ghost host; bool have = false;
	if (coop::inSession() && !net::hosting())
	{
		net::Ghost gs[4]; int n = net::ghosts(gs, 4);
		for (int i = 0; i < n; i++) if (gs[i].id == 0 && gs[i].fresh) { host = gs[i]; have = true; }
	}
	if (!have || !host.s.level || host.s.level <= myLevelNow) { scaleOff(have ? "you're not below the host's level" : "not in a friend's world"); return; }
	float want = rt::max(0.0f, (float)host.s.maxHealth - myMaxNow);
	if (rt::absf(want - healthBoostOn) >= 1)
	{
		int d = (int)(want - healthBoostOn + (want > healthBoostOn ? 0.5f : -0.5f));
		char line[64]; rt::fmt(line, sizeof line, "player.ModAV Health %d", d); console(line);
		healthBoostOn += (float)d;
		LOG("scaling: the host (level %d) has %u health, you %.0f of your own - %+d (boost now %.0f)", (int)host.s.level, (unsigned)host.s.maxHealth, myMaxNow, d, healthBoostOn);
	}
	u32 proc = handling::process(pl);
	u32 we = proc ? ptr(proc + 0x114) : 0, w = we ? ptr(we + 8) : 0;
	float mine = w ? gunRatio((TESForm *)w) : 0, theirs = host.s.dmgFor[net::myId() & 3] / 1000.0f;
	float m = (mine > 0.05f && theirs > 0.05f) ? rt::max(1.0f, theirs / mine) : 1.0f;
	if (rt::absf(m - dmgMult) > 0.02f) { LOG("scaling: your gun does %.2fx its own, the host's would %.2fx - your hits x%.2f", mine, theirs, m); dmgMult = m; }
}
static void hitBonusTick(TESObjectREFR *pl)
{
	struct H { u32 id; float pct; };
	static H hs[32]; static int nh;
	if (dmgMult <= 1.01f) { nh = 0; return; }
	static double hbT;
	if (rt::now() - hbT < 0.1) return;
	hbT = rt::now();
	H now[32]; int n = 0;
	const float *pp = pos(pl);
	forEachActor([&](TESObjectREFR *a) {
		u8 ft = formtype(a);
		if (a == pl || (ft != 0x3B && ft != 0x3C) || n >= 32 || deadNow(a)) return;
		u32 id = refid(a);
		if (!formKey(id).low) return;
		if (mirroredHere(id)) return;
		float *ap = pos(a), dx = ap[0] - pp[0], dy = ap[1] - pp[1];
		if (dx * dx + dy * dy > 5000.0f * 5000.0f) return;
		now[n].id = id; now[n].pct = healthPct(a); n++;
	});
	bool mineNow = rt::now() - myAttackT < 0.4;
	for (int i = 0; i < n; i++)
		for (int k = 0; k < nh; k++)
		{
			if (hs[k].id != now[i].id) continue;
			float drop = hs[k].pct - now[i].pct;
			if (drop > 0.001f && mineNow)
			{
				float extra = drop * (dmgMult - 1);
				TESObjectREFR *a = (TESObjectREFR *)lookup(now[i].id);
				if (a) { g_script->CallFunctionAlt(s_dmgPct, player(), 2, a, F(extra)); now[i].pct -= extra; }
				static double said; if (rt::now() - said > 3) { said = rt::now(); LOG("scaling: your hit on %08x %.0f%% + %.0f%% (x%.2f)", now[i].id, drop * 100, extra * 100, dmgMult); }
			}
		}
	memcpy(hs, now, sizeof(H) * n); nh = n;
}
void scaleTickG(TESObjectREFR *pl) { scaleTick(pl); hitBonusTick(pl); }
static int charMenuNow()
{
	NVSEElement e; memset(&e, 0, sizeof e);
	return s_charMenu && g_script->CallFunction(s_charMenu, player(), 0, &e, 0) && e.type == 1 ? (int)e.num : 0;
}
static bool charDone()
{
	NVSEElement e; memset(&e, 0, sizeof e);
	return !s_charDone || !g_script->CallFunction(s_charDone, player(), 0, &e, 0) || e.type != 1 || e.num != 0;
}
static const char *charCommand(int menu)
{
	switch (menu)
	{
	case 1036: return "ShowRaceMenu";
	case 1051: return "ShowNameMenu";
	case 1074: return "ShowLoveTesterMenu";
	case 1060: return "ShowSPECIALBookMenu";
	case 1076: return "ShowTraitMenu";
	case 1027: return "AddTagSkills 3";
	}
	return 0;
}
static int charWant; static double charWantAt;
void charGenIn(int menu) { if (charCommand(menu)) { charWant = menu; charWantAt = rt::now() + 1; LOG("chargen: the host has menu %d - yours opens in a moment", menu); } }
void charGenTick()
{
	static double t; static int was;
	if (rt::now() - t < 0.25) return;
	t = rt::now();
	if (net::hosting() && !net::soloTesting())
	{
		int m = menumode() ? charMenuNow() : 0;
		if (m && m != was && !charDone())
		{
			u8 b[2] = {(u8)(m & 255), (u8)(m >> 8)};
			net::addEvent(net::EV_CHARGEN, b, 2);
			LOG("chargen: menu %d for the guests", m);
		}
		was = m;
	}
	if (charWant && rt::now() > charWantAt && !menumode())
	{
		const char *c = charCommand(charWant);
		charWant = 0;
		if (c) { console(c); LOG("chargen: %s (as the host's)", c); }
	}
}
static void xpTick(TESObjectREFR *pl)
{
	static double t, last = -1, quietUntil, held, heldAt;
	static u32 loadsSeen;
	double now = rt::now();
	if (now - t < 0.1) return;
	t = now;
	if (loadsSeen != g_loads) { loadsSeen = g_loads; last = -1; quietUntil = now + 20; held = 0; }
	NVSEElement r; memset(&r, 0, sizeof r);
	if (!s_xpNow || !g_script->CallFunction(s_xpNow, player(), 0, &r, 0) || r.type != 1) return;
	double xp = r.num, d = last < 0 ? 0 : xp - last;
	last = xp;
	if (g_xpPending > 0 && now - g_xpPendingT > 10) g_xpPending = 0;
	if (d > 0)
	{
		double cap = now - g_xpPendingT < 0.6 ? g_xpPending * 1.3 + 1 : g_xpPending;
		double f = d < cap ? d : cap;
		g_xpPending = g_xpPending > f ? g_xpPending - f : 0; d -= f;
		if (d > 0 && now >= quietUntil) { if (held <= 0) heldAt = now; held += d; }
	}
	if (held <= 0 || now - heldAt < 0.7) return;
	int amt = (int)(held + 0.5);
	held = 0;
	if (amt < 1 || TUNE(coopXpShare) <= 0 || !coopNow() || net::soloTesting()) return;
	if (amt > 20000) { LOG("xp: +%d at once - a value put in, not earned: not shared", amt); return; }
	net::EvXp e; memset(&e, 0, sizeof e);
	e.amount = (u16)amt;
	if (g_lastStep.quest && g_lastStep.t > heldAt - 0.2 && g_lastStep.t < heldAt + 0.7) { e.fromQuest = 1; e.quest = formKey(g_lastStep.quest); e.stage = g_lastStep.stage; }
	memcpy(e.pos, pos(pl), 12);
	u32 cell = ptr((u32)pl + OFF_REF_CELL);
	e.world = worldspace(); e.cell = interior() && cell ? refid((void *)cell) : 0;
	net::addEvent(net::EV_XP, &e, sizeof e);
	LOG("xp: +%d here%s - to friends within %.0f m", amt, e.fromQuest ? " (with a quest step)" : "", TUNE(coopXpRangeUnits) / 70);
}
static bool rolledAlready(u32 id);
static bool baseCarry(u32 base, u32 *fixed, int &nf, u32 *lists, i32 *lc, int &nl, u32 &death)
{
	nf = nl = 0; death = 0;
	for (int depth = 0; depth < 4 && base; depth++)
	{
		if (!readable(base, 0x70)) return false;
		u8 ty = formtype((void *)base);
		if (ty != 0x2A && ty != 0x2B) return false;
		u32 di = ptr(base + 0x4C);
		if (!death && di && readable(di, 0x10) && formtype((void *)di) == 0x34) death = di;
		u16 tf = rd<u16>(base + 0x4A);
		u32 tpl = ptr(base + 0x54);
		if ((tf & 0x100) && tpl) { base = tpl; continue; }
		for (u32 node = base + 0x68, g = 0; node && g < 256; node = ptr(node + 4), g++)
		{
			if (!readable(node, 8)) return false;
			u32 co = ptr(node);
			if (!co) continue;
			if (!readable(co, 8)) return false;
			i32 n = rd<i32>(co);
			u32 f = ptr(co + 4);
			if (n < -1000 || n > 1000) return false;
			if (!f || !readable(f, 0x10)) continue;
			if (formtype((void *)f) == 0x34) { if (nl < 16) { lists[nl] = f; lc[nl] = n > 0 ? n : 1; nl++; } }
			else if (nf < 64) fixed[nf++] = f;
		}
		return true;
	}
	return false;
}
static void listGives(u32 list, u32 *out, int &n, int cap, int depth)
{
	if (depth > 6 || !readable(list, 0x40)) return;
	for (u32 node = list + 0x34, g = 0; node && g < 256 && readable(node, 8); node = ptr(node + 4), g++)
	{
		u32 d = ptr(node);
		if (!d || !readable(d, 4)) continue;
		u32 f = ptr(d);
		if (!f || !readable(f, 0x10)) continue;
		if (formtype((void *)f) == 0x34) { listGives(f, out, n, cap, depth + 1); continue; }
		bool have = false;
		for (int i = 0; i < n && !have; i++) have = out[i] == f;
		if (!have && n < cap) out[n++] = f;
	}
}
static u32 s_rolled[64]; static int s_nRolled;
static bool rolledAlready(u32 id) { for (int i = 0; i < 64; i++) if (s_rolled[i] == id) return true; return false; }
static void lootRoll(TESObjectREFR *a)
{
	u32 id = refid(a);
	if (rolledAlready(id)) return;
	s_rolled[s_nRolled++ % 64] = id;
	NVSEElement tm; memset(&tm, 0, sizeof tm);
	if (!s_teammate || !g_script->CallFunction(s_teammate, player(), 0, &tm, 1, a) || tm.type != 1 || tm.num != 0) { LOG("loot: %08x is a companion (or that couldn't be checked) - its things left as they are", id); return; }
	u32 fixed[64], lists[16], death; i32 lc[16]; int nf, nl;
	if (!baseCarry(ptr((u32)a + OFF_REF_BASE), fixed, nf, lists, lc, nl, death)) { LOG("loot: %08x - its record couldn't be read: its loot is the same for everyone", id); return; }
	if (!nl && !death) { LOG("loot: %08x carries nothing random: the same for everyone", id); return; }
	static u32 gives[256]; int ng = 0;
	for (int i = 0; i < nl; i++) listGives(lists[i], gives, ng, 256, 0);
	if (death) listGives(death, gives, ng, 256, 0);
	static Inv inv;
	readInv(a, inv);
	int kinds = 0, things = 0;
	for (int i = 0; i < inv.n; i++)
	{
		u32 f = inv.form[i];
		i32 n = inv.count[i];
		if (n <= 0 || !readable(f, 0x10)) continue;
		if (rd<u32>(f + 8) & 0x400) continue;
		bool can = false;
		for (int k = 0; k < ng && !can; k++) can = gives[k] == f;
		if (!can) continue;
		bool named = false;
		for (int k = 0; k < nf && !named; k++) named = fixed[k] == f;
		if (named) continue;
		NVSEElement w; memset(&w, 0, sizeof w);
		if (g_script->CallFunction(s_wornBy, player(), 0, &w, 2, a, (TESForm *)f) && w.type == 1 && w.num != 0) continue;
		g_script->CallFunctionAlt(s_contAdj, player(), 3, a, (TESForm *)f, F(-(float)n));
		kinds++; things += n;
	}
	for (int i = 0; i < nl; i++) g_script->CallFunctionAlt(s_contAdj, player(), 3, a, (TESForm *)lists[i], F((float)lc[i]));
	if (death) g_script->CallFunctionAlt(s_contAdj, player(), 3, a, (TESForm *)death, F(1));
	LOG("loot: your own roll on %08x - %d kinds (%d things) out, %d list(s)%s rolled again (%d things they can give)", id, kinds, things, nl, death ? " and its death item" : "", ng);
}
struct RollQ { u32 id; double at; };
static RollQ s_rollQ[32];
static int s_nRollQ;
static bool lootRollOn() { return TUNE(coopLootRoll) > 0 && s_contAdj && s_wornBy && coopNow() && !net::soloTesting(); }
static void lootRollTick(TESObjectREFR *pl)
{
	static double t;
	static u32 alive[128];
	static int na;
	static u32 loadsSeen;
	double now = rt::now();
	if (now - t < 0.1) return;
	t = now;
	if (loadsSeen != g_loads) { loadsSeen = g_loads; na = 0; s_nRollQ = 0; memset(s_rolled, 0, sizeof s_rolled); }
	bool on = lootRollOn();
	u32 cur[128]; int nc = 0;
	const float *pp = pos(pl);
	forEachActor([&](TESObjectREFR *a) {
		u8 ft = formtype(a);
		if (a == pl || (ft != 0x3B && ft != 0x3C) || nc >= 128 || deadNow(a)) return;
		const float *ap = pos(a); float dx = ap[0] - pp[0], dy = ap[1] - pp[1];
		if (dx * dx + dy * dy > 8000.0f * 8000.0f) return;
		cur[nc++] = refid(a);
	});
	if (on)
		for (int k = 0; k < na; k++)
		{
			u32 id = alive[k];
			bool still = false;
			for (int j = 0; j < nc && !still; j++) still = cur[j] == id;
			if (still || refGone(id) || rolledAlready(id) || s_nRollQ == 32) continue;
			TESObjectREFR *a = (TESObjectREFR *)lookup(id);
			if (!a || (formtype(a) != 0x3B && formtype(a) != 0x3C) || !deadNow(a)) continue;
			bool queued = false;
			for (int q = 0; q < s_nRollQ && !queued; q++) queued = s_rollQ[q].id == id;
			if (!queued) { s_rollQ[s_nRollQ].id = id; s_rollQ[s_nRollQ].at = now + 0.25; s_nRollQ++; }
		}
	memcpy(alive, cur, 4 * nc); na = nc;
	for (int q = 0; q < s_nRollQ;)
	{
		if (now < s_rollQ[q].at) { q++; continue; }
		u32 id = s_rollQ[q].id;
		s_rollQ[q] = s_rollQ[--s_nRollQ];
		TESObjectREFR *a = refGone(id) ? 0 : (TESObjectREFR *)lookup(id);
		if (a && (formtype(a) == 0x3B || formtype(a) == 0x3C) && deadNow(a) && on) lootRoll(a);
	}
}
void lootRollNow(u32 id)
{
	for (int q = 0; q < s_nRollQ; q++)
		if (s_rollQ[q].id == id)
		{
			s_rollQ[q] = s_rollQ[--s_nRollQ];
			s_rolled[s_nRolled++ % 64] = id;
			LOG("loot: %08x opened before its roll - left as it was", id);
			return;
		}
}
enum { T_IDLE, T_OPEN, T_COMMIT, T_GIFTWAIT };
struct Trade
{
	int st; int peer; bool starter; u32 id; u32 box; u32 loads;
	bool editing, opened, owed, myReady, theirReady;
	bool wantBox, boxOpen, boxReadyTo; int boxAct[6]; char boxText[1600]; double boxAt;
	double openAt, t, seenAt, sentAt;
	net::EvTrade mine, theirs, theirsAtAccept;
};
struct TradeEnded { u32 id; u8 peer, how; };
static TradeEnded s_trEnded[8]; static int s_nTrEnded;
static int tradeEndedHow(int peer, u32 id) { for (auto &x : s_trEnded) if (x.id == id && x.peer == peer) return x.how; return 0; }
static Trade s_tr;
static bool tradeKind(u8 ty) { return ty == 0x18 || ty == 0x19 || ty == 0x1F || ty == 0x28 || ty == 0x29 || ty == 0x2E || ty == 0x2F || ty == 0x31; }
static const char *itemName(u32 f)
{
	static char nm[64];
	NVSEElement r; memset(&r, 0, sizeof r);
	if (s_itemName && f && g_script->CallFunction(s_itemName, player(), 0, &r, 1, (TESForm *)f) && r.type == 3 && r.str && r.str[0]) rt::strcpy(nm, r.str, sizeof nm);
	else rt::fmt(nm, sizeof nm, "%08X", f ? refid((void *)f) : 0);
	return nm;
}
static void tradeList(const net::EvTrade &e, char *out, int cap)
{
	int at = 0; out[0] = 0;
	for (int i = 0; i < e.n && i < 10; i++)
	{
		TESForm *f = formOf(e.item[i]);
		at += rt::fmt(out + at, cap - at, "%u x %s\n", (unsigned)e.count[i], f ? itemName((u32)f) : "(not on this PC)");
	}
	if (!e.n) rt::fmt(out, cap, "(nothing)\n");
}
static const char *peerName(int p) { return p >= 0 && p < 4 && vis[p].name[0] ? vis[p].name : "your friend"; }
static void tradeViewItems(const net::EvTrade &e, TradeRow *rows, int &n, u32 &caps)
{
	n = 0; caps = 0;
	for (int i = 0; i < e.n && i < 10; i++)
	{
		TESForm *f = formOf(e.item[i]);
		if (f && refid(f) == 0xF) { caps += e.count[i]; continue; }
		rt::strcpy(rows[n].name, f ? itemName((u32)f) : "(not on this PC)", sizeof rows[n].name);
		rows[n].count = e.count[i]; n++;
	}
}
static void tradeView(const char *status)
{
	g_tv.on = s_tr.st != T_IDLE;
	if (!g_tv.on) return;
	g_tv.myReady = s_tr.myReady; g_tv.theirReady = s_tr.theirReady; g_tv.editing = s_tr.editing;
	rt::strcpy(g_tv.peer, peerName(s_tr.peer), sizeof g_tv.peer);
	tradeViewItems(s_tr.mine, g_tv.mine, g_tv.nMine, g_tv.capsMine);
	tradeViewItems(s_tr.theirs, g_tv.theirs, g_tv.nTheirs, g_tv.capsTheirs);
	if (status) rt::strcpy(g_tv.status, status, sizeof g_tv.status);
}
static TESObjectREFR *tradeBox()
{
	if (!s_tr.box || refGone(s_tr.box)) return 0;
	TESObjectREFR *b = (TESObjectREFR *)lookup(s_tr.box);
	TESForm *ours = lookup((g_mod << 24) | kCont_tradeBox);
	return b && ours && ptr((u32)b + OFF_REF_BASE) == (u32)ours ? b : 0;
}
static void tradeSendTo(int peer, u32 id, int op, const net::EvTrade *items)
{
	net::EvTrade e; memset(&e, 0, sizeof e);
	if (items) e = *items;
	e.op = (u8)op; e.to = (u8)peer; e.id = id;
	if (!items) e.n = 0;
	net::addEvent(net::EV_TRADE, &e, sizeof e);
}
static void tradeSend(int op, const net::EvTrade *items) { tradeSendTo(s_tr.peer, s_tr.id, op, items); }
static void tradeSendReady(bool r)
{
	net::EvTrade e; memset(&e, 0, sizeof e);
	e.ready = r ? 1 : 0; e.ver = s_tr.mine.ver; e.seen = s_tr.theirs.ver;
	tradeSend(net::TR_READY, &e);
}
static bool tradeKnown(const net::EvTrade &e)
{
	for (int i = 0; i < e.n && i < 10; i++) { TESForm *f = formOf(e.item[i]); if (!f || !tradeKind(formtype(f))) return false; }
	return true;
}
static bool tradeCancelled(int peer, u32 id)
{
	static net::Event ev[net::kPeerEv];
	int n = net::peerEvents(peer, ev, net::kPeerEv);
	for (int i = 0; i < n; i++)
	{
		if (ev[i].type != net::EV_TRADE || ev[i].len != sizeof(net::EvTrade)) continue;
		net::EvTrade e; memcpy(&e, ev[i].data, sizeof e);
		if (e.id == id && e.to == net::myId() && e.op == net::TR_CANCEL) return true;
	}
	return false;
}
static void tradeTakeMine()
{
	TESObjectREFR *pl = player();
	for (int i = 0; i < s_tr.mine.n && i < 10; i++) { TESForm *f = formOf(s_tr.mine.item[i]); if (f) g_script->CallFunctionAlt(s_contAdj, player(), 3, pl, f, F(-(float)s_tr.mine.count[i])); }
}
static u32 s_boxGone;
static void tradeEnd(int how, const char *why)
{
	TESObjectREFR *b = tradeBox();
	bool later = false;
	if (b && ptr(ptr(0x11D93F8) + 0x74) == (u32)b)
	{
		console("CloseAllMenus");
		if (how == 1) { s_boxGone = refid(b); g_script->CallFunctionAlt(s_boxBack, player(), 1, b); later = true; LOG("trade: its box was open - closed, taken away once the menu's gone"); }
	}
	if (!later)
	{
		if (b) { g_script->CallFunctionAlt(how == 1 ? s_boxBack : s_boxEmpty, player(), 1, b); removeRef(b); }
		else if (how == 1 && !s_tr.owed && s_tr.loads == g_loads && s_tr.mine.n && s_tr.box)
		{
			TESObjectREFR *pl = player();
			for (int i = 0; i < s_tr.mine.n && i < 10; i++) { TESForm *f = formOf(s_tr.mine.item[i]); if (f) g_script->CallFunctionAlt(s_contAdj, player(), 3, pl, f, F((float)s_tr.mine.count[i])); }
			LOG("trade: the box was gone - its things given back from the offer");
		}
		else if (how == 2 && s_tr.owed) tradeTakeMine();
	}
	LOG("trade: over (%s)%s", why, b ? (how == 1 ? " - your things back" : " - your things went to your friend") : "");
	if (s_tr.id) { TradeEnded &x = s_trEnded[s_nTrEnded++ % 8]; x.id = s_tr.id; x.peer = (u8)s_tr.peer; x.how = (u8)(how == 2 ? 2 : 1); }
	bool shown = s_tr.boxOpen;
	memset(&s_tr, 0, sizeof s_tr);
	g_tv.on = false;
	if (shown) console("CloseAllMenus");
}
void tradeNetGone() { if (s_tr.st != T_IDLE && s_tr.st != T_COMMIT && s_tr.st != T_GIFTWAIT) tradeEnd(1, "not playing together any more"); else if (s_tr.st != T_IDLE) g_tv.on = false; }
static bool tradeBoxHolds(const net::EvTrade &o)
{
	if (!o.n) return true;
	TESObjectREFR *b = tradeBox();
	if (!b) return false;
	static Inv in;
	readInv(b, in);
	for (int i = 0; i < o.n; i++)
	{
		TESForm *f = formOf(o.item[i]);
		if (!f || invCount(in, (u32)f) < (i32)o.count[i]) return false;
	}
	return true;
}
static void tradeGet(const net::EvTrade &e, const char *who)
{
	TESObjectREFR *pl = player();
	for (int i = 0; i < e.n && i < 10; i++)
	{
		TESForm *f = formOf(e.item[i]);
		if (!f || !e.count[i] || !tradeKind(formtype(f))) { LOG("trade: %s's %06x isn't a thing on this PC - not received", who, e.item[i].low); continue; }
		g_script->CallFunctionAlt(s_contAdj, player(), 3, pl, f, F((float)e.count[i]));
	}
}
static void tradeOpenWindow(int peer, bool starter, u32 id)
{
	memset(&s_tr, 0, sizeof s_tr);
	s_tr.st = T_OPEN; s_tr.peer = peer; s_tr.starter = starter; s_tr.id = id; s_tr.loads = g_loads; s_tr.t = rt::now();
	s_tr.wantBox = starter;
}
static void tradeStart(int slot)
{
	if (s_tr.st != T_IDLE) { message("A trade is already going.", 3); return; }
	if (net::soloTesting()) { message("No trades in the solo test.", 3); return; }
	tradeOpenWindow(slot, true, (u32)(rt::now() * 1000) ^ ((u32)slot << 28));
	tradeSend(net::TR_OFFER, &s_tr.mine);
	char st[120]; rt::fmt(st, sizeof st, "Waiting for %s.", peerName(slot));
	tradeView(st);
	LOG("trade: started with %s", peerName(slot));
}
void tradeAbort(const char *why)
{
	if (s_tr.st == T_IDLE) return;
	if ((s_tr.st == T_COMMIT || s_tr.st == T_GIFTWAIT) && !s_tr.owed)
	{
		TESObjectREFR *b = tradeBox();
		if (b) { g_script->CallFunctionAlt(s_boxBack, player(), 1, b); removeRef(b); s_tr.owed = true; }
		s_tr.box = 0;
		LOG("trade: %s - your offer back with you for the save (it goes back in its box after)", why);
		return;
	}
	if (s_tr.owed) return;
	tradeSend(net::TR_CANCEL, 0);
	char m[96]; rt::fmt(m, sizeof m, "Trade with %s called off (%s).", peerName(s_tr.peer), why); message(m, 4);
	tradeEnd(1, why);
}
static void tradeIn(u8 peer, const net::Event &ev, const char *who)
{
	net::EvTrade e; memcpy(&e, ev.data, sizeof e);
	if (e.to != net::myId() || e.n > 10) return;
	char list[700], m[900];
	if (e.op == net::TR_GIFT)
	{
		static u32 got[8]; static int ngot;
		for (int i = 0; i < 8; i++) if (got[i] == e.id) { tradeSendTo(peer, e.id, net::TR_DONE, 0); return; }
		if (s_tr.st == T_IDLE || s_tr.peer != peer || s_tr.id != e.id) { tradeSendTo(peer, e.id, net::TR_DECLINE, 0); LOG("trade: %s's gift for a trade that's over - sent back", who); return; }
		if (tradeCancelled(peer, e.id)) { LOG("trade: %s's gift was called off before it got here", who); return; }
		got[ngot++ % 8] = e.id;
		if (!tradeKnown(e)) { tradeSendTo(peer, e.id, net::TR_DECLINE, 0); message("A friend's gift has things this PC doesn't know (a mod) - sent back.", 5); LOG("trade: %s's gift refused - things not on this PC", who); tradeEnd(1, "their gift couldn't be taken"); return; }
		tradeGet(e, who);
		tradeSendTo(peer, e.id, net::TR_DONE, 0);
		tradeList(e, list, sizeof list);
		rt::fmt(m, sizeof m, "%s gave you:\n%s", who, list); message(m, 6);
		LOG("trade: a gift from %s (%d kinds)", who, (int)e.n);
		if (s_tr.st != T_IDLE && s_tr.id == e.id) tradeEnd(1, "a gift came instead");
		return;
	}
	if (e.op == net::TR_OFFER)
	{
		if (s_tr.st != T_IDLE) { tradeSendTo(peer, e.id, net::TR_DECLINE, 0); LOG("trade: %s's invitation declined - busy", who); return; }
		tradeOpenWindow(peer, false, e.id);
		char st[120]; rt::fmt(st, sizeof st, "%s wants to trade - look at them and press E.", who);
		tradeView(st);
		message(st, 5);
		LOG("trade: %s opened a trade with you", who);
		return;
	}
	if (s_tr.st == T_IDLE || peer != s_tr.peer || e.id != s_tr.id)
	{
		if (e.op == net::TR_ACCEPT || e.op == net::TR_UPDATE || e.op == net::TR_READY)
		{
			int how = tradeEndedHow(peer, e.id);
			tradeSendTo(peer, e.id, how == 2 ? net::TR_DONE : net::TR_CANCEL, 0);
		}
		return;
	}
	s_tr.t = rt::now();
	bool committing = s_tr.st == T_COMMIT || s_tr.st == T_GIFTWAIT;
	if (e.op == net::TR_UPDATE)
	{
		if (committing) { s_tr.theirs = e; s_tr.theirReady = false; tradeView("Finishing - your friend's game decides."); return; }
		if (!tradeKnown(e)) { tradeSend(net::TR_CANCEL, 0); rt::fmt(m, sizeof m, "%s offered things this PC doesn't know (a mod) - trade off, your things are back.", who); message(m, 5); tradeEnd(1, "unknown things"); return; }
		s_tr.theirs = e; s_tr.theirReady = false;
		if (s_tr.myReady) { s_tr.myReady = false; tradeSendReady(false); }
		rt::fmt(m, sizeof m, "%s changed their offer.", who);
		tradeView(m);
		return;
	}
	if (e.op == net::TR_READY)
	{
		s_tr.theirReady = e.ready && e.ver == s_tr.theirs.ver && e.seen == s_tr.mine.ver;
		rt::fmt(m, sizeof m, s_tr.theirReady ? "%s is ready." : "%s isn't ready.", who);
		tradeView(m);
		return;
	}
	if (e.op == net::TR_ACCEPT && !s_tr.starter && s_tr.st == T_OPEN)
	{
		bool same = s_tr.myReady && !s_tr.editing && e.ver == s_tr.theirs.ver && e.seen == s_tr.mine.ver;
		if (tradeCancelled(peer, e.id)) { message("The trade was called off - your things are back.", 4); tradeEnd(1, "called off before the finish got here"); return; }
		if (!same || !tradeBoxHolds(s_tr.mine)) { s_tr.myReady = false; tradeSend(net::TR_REFUSE, 0); tradeSendReady(false); tradeView("Something changed - ready again when you're happy."); return; }
		tradeGet(s_tr.theirs, who);
		tradeSend(net::TR_DONE, 0);
		rt::fmt(m, sizeof m, "Trade with %s done.", who); message(m, 4);
		tradeEnd(2, "both ready");
		return;
	}
	if (e.op == net::TR_REFUSE && s_tr.st == T_COMMIT)
	{
		s_tr.st = T_OPEN; s_tr.theirReady = false; s_tr.myReady = false;
		tradeView("Something changed - ready again when you're happy.");
		message("Something changed in the trade - look at it again.", 4);
		if (s_tr.starter) s_tr.wantBox = true;
		return;
	}
	if (e.op == net::TR_DONE && s_tr.st == T_COMMIT)
	{
		tradeGet(s_tr.theirsAtAccept, who);
		rt::fmt(m, sizeof m, "Trade with %s done.", who); message(m, 4);
		tradeEnd(2, "both ready");
		return;
	}
	if (e.op == net::TR_DONE && s_tr.st == T_GIFTWAIT)
	{
		rt::fmt(m, sizeof m, "%s has your gift.", who); message(m, 4);
		tradeEnd(2, "a gift");
		return;
	}
	if (e.op == net::TR_DECLINE || e.op == net::TR_CANCEL)
	{
		rt::fmt(m, sizeof m, "%s %s the trade - your things are back.", who, e.op == net::TR_DECLINE ? "declined" : "called off"); message(m, 4);
		tradeEnd(1, e.op == net::TR_DECLINE ? "they declined" : "they called it off");
	}
}
void tradeLoadCheck()
{
	if (s_tr.st != T_IDLE && s_tr.loads != g_loads) { tradeSend(net::TR_CANCEL, 0); LOG("trade: called off - a game was loaded"); memset(&s_tr, 0, sizeof s_tr); g_tv.on = false; }
}
static bool tradeReadBox(TESObjectREFR *b)
{
	static Inv in;
	readInv(b, in);
	net::EvTrade o; memset(&o, 0, sizeof o);
	int kinds = 0;
	const char *bad = 0;
	for (int i = 0; i < in.n && !bad; i++)
	{
		if (in.count[i] <= 0) continue;
		net::FormKey k = formKey(refid((void *)in.form[i]));
		if (kinds == 10) bad = "At most 10 kinds of things in one offer - take some out.";
		else if (in.count[i] > 65535) bad = "At most 65535 of one thing in an offer - take some out.";
		else if (!k.low || !tradeKind(formtype((void *)in.form[i]))) bad = "Something in there can't be traded (made in the game, or not an item) - take it out.";
		else { o.item[kinds] = k; o.count[kinds] = (u16)in.count[i]; kinds++; }
	}
	if (bad) { message(bad, 5); return false; }
	o.n = (u8)kinds; o.ver = (u16)(s_tr.mine.ver + 1);
	s_tr.mine = o; s_tr.theirReady = false;
	tradeSend(net::TR_UPDATE, &s_tr.mine);
	return true;
}
static void tradeAct(int a, TESObjectREFR *pl, double now);
static void tradeBoxWrite(char *o, int cap)
{
	int k = 0;
	auto put = [&](const char *t) { int n = (int)strlen(t); if (k + n >= cap) n = cap - 1 - k; if (n > 0) { memcpy(o + k, t, n); k += n; } o[k] = 0; };
	char line[120];
	rt::fmt(line, sizeof line, "BARTER WITH %s\n\n", g_tv.peer); put(line);
	for (int c = 0; c < 2; c++)
	{
		const TradeRow *R = c ? g_tv.theirs : g_tv.mine;
		int n = c ? g_tv.nTheirs : g_tv.nMine;
		u32 caps = c ? g_tv.capsTheirs : g_tv.capsMine;
		bool ready = c ? g_tv.theirReady : g_tv.myReady;
		if (c) rt::fmt(line, sizeof line, "%s's offer%s\n", g_tv.peer, ready ? " (ready)" : "");
		else rt::fmt(line, sizeof line, "Your offer%s\n", ready ? " (ready)" : "");
		put(line);
		if (!n && !caps) put(!c && g_tv.editing ? "   choosing...\n" : "   nothing yet\n");
		for (int i = 0; i < n; i++) { if (R[i].count > 1) rt::fmt(line, sizeof line, "   %s (%u)\n", R[i].name, R[i].count); else rt::fmt(line, sizeof line, "   %s\n", R[i].name); put(line); }
		if (caps) { rt::fmt(line, sizeof line, "   %u caps\n", caps); put(line); }
		put("\n");
	}
	put(g_tv.status);
}
static void tradeBoxShow()
{
	static const char *b[6];
	int n = 0;
	s_tr.boxAct[n] = 1; b[n++] = "Change Offer";
	s_tr.boxAct[n] = 2; b[n++] = s_tr.myReady ? "Not Ready" : "Ready"; s_tr.boxReadyTo = !s_tr.myReady;
	if (s_tr.starter && s_tr.mine.n) { s_tr.boxAct[n] = 3; b[n++] = "Give as a Gift"; }
	s_tr.boxAct[n] = 4; b[n++] = s_tr.starter ? "Call Off" : "No Thanks";
	s_tr.boxAct[n] = 5; b[n++] = "Back";
	tradeBoxWrite(s_tr.boxText, sizeof s_tr.boxText);
	if (showMessageBox(s_tr.boxText, b, n)) { s_tr.boxOpen = true; s_tr.wantBox = false; s_tr.boxAt = rt::now(); }
}
static void tradeBoxLive()
{
	static char now[1600];
	tradeBoxWrite(now, sizeof now);
	if (rt::streq(now, s_tr.boxText) || !s_boxText) return;
	rt::strcpy(s_tr.boxText, now, sizeof s_tr.boxText);
	NVSEElement r; memset(&r, 0, sizeof r);
	g_script->CallFunction(s_boxText, player(), 0, &r, 1, (void *)s_tr.boxText);
	static bool told;
	if (!told && s_boxTextGet)
	{
		told = true;
		NVSEElement g; memset(&g, 0, sizeof g);
		bool ok = g_script->CallFunction(s_boxTextGet, player(), 0, &g, 0) && g.type == 3 && g.str && rt::streq(g.str, s_tr.boxText);
		LOG("trade: the window's text %s while it's open", ok ? "changes" : "didn't change - it shows what it opened with");
	}
}
static void tradeTick(TESObjectREFR *pl, bool focus)
{
	double now = rt::now();
	if (s_boxGone && ptr(ptr(0x11D93F8) + 0x74) != s_boxGone)
	{
		TESObjectREFR *gb = (TESObjectREFR *)lookup(s_boxGone);
		if (gb) { g_script->CallFunctionAlt(s_boxBack, player(), 1, gb); removeRef(gb); }
		s_boxGone = 0;
	}
	static bool eWas;
	bool e = focus && held('E') && !menumode();
	if (e && !eWas) g_useT = now;
	if (e && !eWas && s_crossRef && (s_tr.st == T_IDLE || !s_tr.boxOpen) && !g_menu)
	{
		NVSEElement r; memset(&r, 0, sizeof r);
		g_script->CallFunction(s_crossRef, player(), 0, &r, 0);
		TESObjectREFR *cr = r.type == 2 ? (TESObjectREFR *)r.form : 0;
		int hit = -1;
		for (int sl = 0; sl < 3 && cr; sl++) if (proxies[sl].ref == cr) hit = sl;
		int near = -1; float nd = 1e9f;
		{
			const float *pp = pos(pl); float yaw = rot(pl)[2], fx = rt::sin(yaw), fy = rt::cos(yaw);
			net::Ghost gs[4]; int n = net::ghosts(gs, 4);
			for (int i = 0; i < n; i++)
			{
				const net::Ghost &g = gs[i];
				if (!g.fresh || g.id > 2) continue;
				float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1], d = rt::sqrt(dx * dx + dy * dy);
				if (d < 1 || d > 175 || (dx * fx + dy * fy) / d < 0.85f) continue;
				if (d < nd) { nd = d; near = g.id; }
			}
		}
		bool other = cr && hit < 0 && (formtype(cr) != 0x3B && formtype(cr) != 0x3C ? true : refid(cr) >> 24 != 0xFF);
		if (hit < 0 && near >= 0 && !other) hit = near;
		static int told;
		if ((near >= 0 || hit >= 0) && told < 10) { told++; LOG("trade: E with %08x under the crosshair, a friend %.0f units in front - %s", cr ? refid(cr) : 0, near >= 0 ? nd : -1.0f, hit >= 0 ? "a trade" : "not a trade (something else is there)"); }
		if (hit >= 0 && s_tr.st == T_IDLE) tradeStart(hit);
		else if (hit >= 0 && hit == s_tr.peer && s_tr.st == T_OPEN && !s_tr.editing) s_tr.wantBox = true;
	}
	eWas = e;
	if (s_tr.st == T_IDLE) return;
	bool here = false;
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	for (int i = 0; i < n; i++) if (gs[i].id == s_tr.peer && gs[i].since < 5) here = true;
	if (here || !s_tr.seenAt) s_tr.seenAt = now;
	bool committing = s_tr.st == T_COMMIT || s_tr.st == T_GIFTWAIT;
	if (now - s_tr.seenAt > (committing ? 60 : 15)) { tradeSend(net::TR_CANCEL, 0); message("Your friend is gone - the trade is off, your things are back.", 4); tradeEnd(1, "the friend is gone"); return; }
	if (now - s_tr.t > (committing ? 600 : 300)) { tradeSend(net::TR_CANCEL, 0); message("Nothing happened for a while - the trade is off, your things are back.", 4); tradeEnd(1, "nothing happened"); return; }
	if (committing)
	{
		if (s_tr.boxOpen) { s_tr.boxOpen = false; console("CloseAllMenus"); }
		if (s_tr.owed && !s_tr.box)
		{
			const float *p = pos(pl);
			TESObjectREFR *b = placeRef(kCont_tradeBox, p[0], p[1], p[2] - 3000, 0);
			if (b)
			{
				for (int i = 0; i < s_tr.mine.n && i < 10; i++)
				{
					TESForm *f = formOf(s_tr.mine.item[i]);
					if (!f) continue;
					g_script->CallFunctionAlt(s_contAdj, player(), 3, pl, f, F(-(float)s_tr.mine.count[i]));
					g_script->CallFunctionAlt(s_contAdj, player(), 3, b, f, F((float)s_tr.mine.count[i]));
				}
				s_tr.box = refid(b); s_tr.owed = false;
				LOG("trade: your offer back in a box after the save");
			}
		}
		if (now - s_tr.sentAt > 10)
		{
			s_tr.sentAt = now;
			if (s_tr.st == T_GIFTWAIT) tradeSend(net::TR_GIFT, &s_tr.mine);
			else { net::EvTrade a; memset(&a, 0, sizeof a); a.ver = s_tr.mine.ver; a.seen = s_tr.theirsAtAccept.ver; tradeSend(net::TR_ACCEPT, &a); }
		}
		return;
	}
	if (s_tr.editing)
	{
		TESObjectREFR *b = tradeBox();
		if (!b) { s_tr.editing = false; s_tr.wantBox = true; tradeView("The trade box went - Change Offer to try again."); return; }
		if (!s_tr.opened)
		{
			if (now < s_tr.openAt) return;
			setHidden(b, true);
			g_script->CallFunctionAlt(s_boxOpen, player(), 1, b);
			s_tr.opened = true; s_tr.t = now;
			return;
		}
		u32 cm = ptr(0x11D93F8), open = cm ? ptr(cm + 0x74) : 0;
		if (open == (u32)b || now - s_tr.t < 0.5) return;
		if (!tradeReadBox(b)) { s_tr.opened = false; s_tr.openAt = now + 0.2; return; }
		s_tr.editing = false; s_tr.t = now; s_tr.wantBox = true;
		tradeView("Your offer is sent - Ready when you are.");
		return;
	}
	if (s_tr.boxOpen)
	{
		int b = messageBoxButton();
		if (b >= 0) { s_tr.boxOpen = false; s_tr.wantBox = false; tradeAct(b < 6 ? s_tr.boxAct[b] : 0, pl, now); }
		else if (!menumode() && now - s_tr.boxAt > 0.3) s_tr.boxOpen = false;
		else tradeBoxLive();
	}
	else if (s_tr.wantBox && !menumode() && !g_menu && !g_chatTyping) tradeBoxShow();
	if (s_tr.st == T_OPEN && s_tr.starter && s_tr.myReady && s_tr.theirReady && (s_tr.mine.n || s_tr.theirs.n))
	{
		if (!tradeBoxHolds(s_tr.mine)) { s_tr.myReady = false; tradeSendReady(false); tradeView("Your offer changed - look at it again (Change Offer)."); return; }
		net::EvTrade a; memset(&a, 0, sizeof a); a.ver = s_tr.mine.ver; a.seen = s_tr.theirs.ver;
		tradeSend(net::TR_ACCEPT, &a);
		s_tr.theirsAtAccept = s_tr.theirs;
		s_tr.st = T_COMMIT; s_tr.t = now; s_tr.sentAt = now;
		tradeView("Both ready - finishing...");
		message("Both ready - finishing the trade...", 3);
		LOG("trade: both ready - finishing");
	}
}
static void tradeAct(int a, TESObjectREFR *pl, double now)
{
	char m[160];
	if (a == 5) { message("The trade stays open - look at your friend and press E to see it again.", 4); return; }
	if (a == 1)
	{
		if (!tradeBox() && s_tr.box && s_tr.mine.n)
		{
			for (int i = 0; i < s_tr.mine.n && i < 10; i++) { TESForm *f = formOf(s_tr.mine.item[i]); if (f) g_script->CallFunctionAlt(s_contAdj, player(), 3, pl, f, F((float)s_tr.mine.count[i])); }
			s_tr.mine.n = 0; s_tr.mine.ver++; tradeSend(net::TR_UPDATE, &s_tr.mine); s_tr.box = 0;
		}
		if (!tradeBox())
		{
			const float *p = pos(pl);
			TESObjectREFR *b = placeRef(kCont_tradeBox, p[0], p[1], p[2] - 3000, 0);
			if (!b) { tradeView("The trade box couldn't be made."); s_tr.wantBox = true; return; }
			s_tr.box = refid(b);
		}
		if (s_tr.myReady) { s_tr.myReady = false; tradeSendReady(false); }
		s_tr.editing = true; s_tr.opened = false; s_tr.openAt = now + 0.4; s_tr.t = now;
		tradeView("Put in what you offer (or take things out), then close the box.");
		return;
	}
	if (a == 2)
	{
		s_tr.myReady = s_tr.boxReadyTo; s_tr.t = now;
		tradeSendReady(s_tr.myReady);
		tradeView(s_tr.myReady ? "You're ready." : "You're not ready.");
		s_tr.wantBox = true;
		return;
	}
	if (a == 3 && s_tr.starter)
	{
		if (!s_tr.mine.n) { tradeView("Put something in your offer first (Change Offer)."); s_tr.wantBox = true; return; }
		if (!tradeBoxHolds(s_tr.mine)) { tradeView("Your offer changed - look at it again (Change Offer)."); s_tr.wantBox = true; return; }
		tradeSend(net::TR_GIFT, &s_tr.mine);
		s_tr.st = T_GIFTWAIT; s_tr.t = now; s_tr.sentAt = now;
		rt::fmt(m, sizeof m, "Giving it to %s...", peerName(s_tr.peer)); tradeView(m);
		LOG("trade: gift sent");
		return;
	}
	if (a == 4)
	{
		tradeSend(s_tr.starter ? net::TR_CANCEL : net::TR_DECLINE, 0);
		message("Trade called off - your things are back.", 3);
		tradeEnd(1, "you called it off");
		return;
	}
}
static bool hostNearMe(TESObjectREFR *pl)
{
	net::Ghost gs[4]; int n = net::ghosts(gs, 4);
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	const float *pp = pos(pl);
	for (int i = 0; i < n; i++)
	{
		const net::Ghost &g = gs[i];
		if (g.id != 0 || !g.fresh || ((g.s.world ^ world) & 0xFFFFFF) || ((g.s.cell ^ cid) & 0xFFFFFF) || (!g.s.cell) != (!cid)) continue;
		float dx = g.s.pos[0] - pp[0], dy = g.s.pos[1] - pp[1], dz = g.s.pos[2] - pp[2];
		if (dx * dx + dy * dy + dz * dz < 7000.0f * 7000.0f) return true;
	}
	return false;
}
static void talkCast(TESObjectREFR *pl)
{
	if (!s_talkText || !s_talkName || !net::active() || net::soloTesting()) return;
	static double t; static char last[300];
	double now = rt::now();
	if (now - t < 0.15) return;
	t = now;
	NVSEElement r; memset(&r, 0, sizeof r);
	TESObjectREFR *a = s_talkRef && g_script->CallFunction(s_talkRef, player(), 0, &r, 0) && r.type == 2 ? (TESObjectREFR *)r.form : 0;
	if (!a) { last[0] = 0; return; }
	NVSEElement tx; memset(&tx, 0, sizeof tx);
	if (!g_script->CallFunction(s_talkText, player(), 0, &tx, 0) || tx.type != 3 || !tx.str || !tx.str[0])
	{
		static int quiet; if (quiet++ == 40) LOG("listen-in: in a conversation but its text can't be read (type %d) - friends won't see it", tx.type);
		return;
	}
	{
		int k = 0; while (k < (int)sizeof last - 1 && tx.str[k] && tx.str[k] == last[k]) k++;
		if (k == (int)sizeof last - 1 || (!tx.str[k] && !last[k])) return;
	}
	rt::strcpy(last, tx.str, sizeof last);
	NVSEElement nm; memset(&nm, 0, sizeof nm);
	char speaker[32];
	if (g_script->CallFunction(s_talkName, player(), 0, &nm, 0) && nm.type == 3 && nm.str && nm.str[0]) rt::strcpy(speaker, nm.str, sizeof speaker);
	else rt::strcpy(speaker, itemName(ptr((u32)a + OFF_REF_BASE)), sizeof speaker);
	static u32 lineId;
	lineId++;
	int len = (int)strlen(last), parts = (len + 92) / 93;
	if (parts > 3) parts = 3;
	for (int k = 0; k < parts; k++)
	{
		net::EvTalk e; memset(&e, 0, sizeof e);
		e.line = lineId; e.part = (u8)k; e.parts = (u8)parts;
		rt::strcpy(e.speaker, speaker, sizeof e.speaker);
		int at = k * 93, n = len - at < 93 ? len - at : 93;
		memcpy(e.text, last + at, n);
		net::addEvent(net::EV_TALK, &e, sizeof e);
	}
	static int told; if (told++ < 10) LOG("listen-in: %s said a line - sent to friends", speaker);
}
static void talkIn(u8 peer, const net::Event &ev, const char *who, const PeerAt *at)
{
	net::EvTalk e; memcpy(&e, ev.data, sizeof e);
	e.speaker[sizeof e.speaker - 1] = 0; e.text[sizeof e.text - 1] = 0;
	static u32 curLine[4]; static char buf[4][300];
	if (peer > 3) return;
	if (e.part == 0 || curLine[peer] != e.line) { curLine[peer] = e.line; buf[peer][0] = 0; }
	int n = (int)strlen(buf[peer]);
	rt::strcpy(buf[peer] + n, e.text, sizeof buf[peer] - n);
	if (e.part + 1 < e.parts) return;
	TESObjectREFR *pl = player();
	if (!pl || !at) return;
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	if (((at->world ^ world) & 0xFFFFFF) || ((at->cell ^ cid) & 0xFFFFFF) || (!at->cell) != (!cid)) return;
	const float *pp = pos(pl);
	float dx = at->pos[0] - pp[0], dy = at->pos[1] - pp[1], dz = at->pos[2] - pp[2];
	if (dx * dx + dy * dy + dz * dz > TUNE(coopListenRangeUnits) * TUNE(coopListenRangeUnits)) return;
	if (g_nListen == 3) { memmove(g_listen, g_listen + 1, sizeof(Listen) * 2); g_nListen = 2; }
	Listen &L = g_listen[g_nListen++];
	rt::strcpy(L.who, who, sizeof L.who); rt::strcpy(L.speaker, e.speaker, sizeof L.speaker); rt::strcpy(L.text, buf[peer], sizeof L.text); L.t = rt::now();
}
void talkGuard(TESObjectREFR *pl)
{
	talkCast(pl);
	if (!s_talkRef || net::hosting() || !coop::inSession()) return;
	static double t, closedAt, toldAt;
	static u32 mine;
	double now = rt::now();
	if (now - t > 0.3) mine = 0;
	if (now - t < 0.05) return;
	t = now;
	NVSEElement r; memset(&r, 0, sizeof r);
	TESObjectREFR *a = g_script->CallFunction(s_talkRef, player(), 0, &r, 0) && r.type == 2 ? (TESObjectREFR *)r.form : 0;
	if (!a) { mine = 0; return; }
	if ((formtype(a) != 0x3B && formtype(a) != 0x3C) || refid(a) >> 24 == 0xFF) return;
	if (refid(a) == mine) return;
	if (now - g_useT < 1.5) { mine = refid(a); return; }
	if (now - closedAt < 0.5 || !hostNearMe(pl)) return;
	closedAt = now;
	console("CloseAllMenus");
	if (now - toldAt > 20)
	{
		toldAt = now;
		char m[140]; rt::fmt(m, sizeof m, "%s is talking with %s - their conversation counts for you both.", itemName(ptr((u32)a + OFF_REF_BASE)), peerName(0));
		message(m, 5);
	}
	LOG("coop: a conversation %08x started on you was closed - the host is with you, theirs counts", refid(a));
}
static u32 merchantChest(u32 actor)
{
	for (u32 x = ptr(actor + 0x48), k = 0; x && k < 64 && readable(x, 0x10); x = ptr(x + 8), k++)
	{
		if (rd<u8>(x + 4) != 0x3C) continue;
		u32 c = ptr(x + 0xC);
		if (c && readable(c, 0x30) && formtype((void *)c) == 0x3A) { u32 b = ptr(c + OFF_REF_BASE); if (b && formtype((void *)b) == 0x1B) return c; }
		break;
	}
	return 0;
}
static u32 s_venChest; static Inv s_venInv;
static void vendorDiff(TESObjectREFR *pl, u32 chest)
{
	net::FormKey ck = formKey(refid((void *)chest));
	static Inv cur;
	readInv(pl, cur);
	if (!ck.low) { s_venInv = cur; return; }
	net::EvVendor e; memset(&e, 0, sizeof e); e.cont = ck;
	auto flush = [&]() { if (e.n) { net::addEvent(net::EV_VENDOR, &e, sizeof e); LOG("vendor: chest %08x - %d change(s) sent to friends", refid((void *)chest), (int)e.n); } e.n = 0; };
	auto one = [&](u32 f, i32 before, i32 now) {
		if (before == now) return;
		net::FormKey k = formKey(refid((void *)f));
		if (!k.low) return;
		e.item[e.n] = k; e.delta[e.n] = before - now; e.n++;
		if (e.n == 8) flush();
	};
	for (int i = 0; i < cur.n; i++) one(cur.form[i], invCount(s_venInv, cur.form[i]), cur.count[i]);
	for (int i = 0; i < s_venInv.n; i++) if (!invHas(cur, s_venInv.form[i]) && s_venInv.count[i]) one(s_venInv.form[i], s_venInv.count[i], 0);
	flush();
	s_venInv = cur;
}
void vendorTick(TESObjectREFR *pl)
{
	if (!s_barterRef) return;
	static double last;
	double now = rt::now();
	if (now - last > 0.5) s_venChest = 0;
	last = now;
	NVSEElement r; memset(&r, 0, sizeof r);
	u32 actor = g_script->CallFunction(s_barterRef, player(), 0, &r, 0) && r.type == 2 && r.form ? (u32)r.form : 0;
	u32 chest = actor ? merchantChest(actor) : 0;
	if (chest != s_venChest)
	{
		if (s_venChest) vendorDiff(pl, s_venChest);
		s_venChest = chest;
		if (chest) { readInv(pl, s_venInv); LOG("vendor: bartering with %08x - its chest %08x (%s)", refid((void *)actor), refid((void *)chest), formKey(refid((void *)chest)).low ? "kept the same for friends" : "made in game: not shared"); }
		else if (actor) { static u32 told; if (told != actor) { told = actor; LOG("vendor: %08x's merchant chest not found - this barter isn't shared", refid((void *)actor)); } }
		return;
	}
	if (chest) vendorDiff(pl, chest);
}
bool barterOpen() { return s_venChest != 0; }
static void vendorIn(u8 peer, const net::Event &ev, const char *who)
{
	net::EvVendor c; memcpy(&c, ev.data, sizeof c);
	TESObjectREFR *cr = refOf(c.cont);
	if (!cr || formtype(cr) != 0x3A || c.n > 8) return;
	u32 b = ptr((u32)cr + OFF_REF_BASE);
	if (!b || formtype((void *)b) != 0x1B) { LOG("vendor: %s's change to %08x dropped - not a container", who, refid(cr)); return; }
	static double since[4]; static int count[4];
	if (peer < 4) { if (rt::now() - since[peer] > 30) { since[peer] = rt::now(); count[peer] = 0; } if (++count[peer] > 300) return; }
	int done = 0;
	for (int i = 0; i < c.n; i++)
	{
		TESForm *it = formOf(c.item[i]);
		if (!it || !tradeKind(formtype(it)) || c.delta[i] > 100000 || c.delta[i] < -100000) continue;
		g_script->CallFunctionAlt(s_contAdj, player(), 3, cr, it, F((float)c.delta[i]));
		done++;
	}
	LOG("vendor: %s's barter - chest %08x, %d change(s) here too", who, refid(cr), done);
}
static void objSend(TESObjectREFR *pl, TESObjectREFR *r, int op, float dmg)
{
	net::EvObj e; memset(&e, 0, sizeof e);
	e.ref = formKey(refid(r)); e.op = (u8)op; e.dmg = dmg;
	memcpy(e.pos, pos(pl), 12);
	u32 cell = ptr((u32)pl + OFF_REF_CELL);
	e.world = worldspace(); e.cell = interior() && cell ? refid((void *)cell) : 0;
	net::addEvent(net::EV_OBJ, &e, sizeof e);
}
static void objTick(TESObjectREFR *pl)
{
	static double seen;
	struct W { u32 id; double at; bool was; };
	static W w[8]; static int nw;
	double now = rt::now();
	if (handling::g_shotT != seen)
	{
		seen = handling::g_shotT;
		if (coopNow() && !net::soloTesting())
		{
			float d[3]; aimDir(handling::g_shotH, handling::g_shotP, d);
			const float *f0 = handling::g_shotFrom;
			float a[3] = {f0[0] + d[0] * 20, f0[1] + d[1] * 20, f0[2] + d[2] * 20}, b[3] = {f0[0] + d[0] * 8000, f0[1] + d[1] * 8000, f0[2] + d[2] * 8000}, fr = 1;
			u32 o = 0;
			TESObjectREFR *r = rayHitObject(a, b, &fr, &o) && o ? refOfNode(o) : 0;
			if (r && formtype(r) == 0x3A && formKey(refid(r)).low)
			{
				u32 proc = handling::process(pl), we = proc ? ptr(proc + 0x114) : 0, wf = we ? ptr(we + 8) : 0;
				float dmg = 10;
				NVSEElement res; memset(&res, 0, sizeof res);
				if (wf && s_baseDmg && g_script->CallFunction(s_baseDmg, player(), 0, &res, 1, (TESForm *)wf) && res.type == 1 && res.num > 0) dmg = (float)res.num;
				objSend(pl, r, 1, dmg);
				static int told; if (told++ < 20) LOG("share: your shot hit %08x (%.0f damage) - friends' worlds too", refid(r), dmg);
				if (nw < 8) { w[nw].id = refid(r); w[nw].at = now + 0.4; w[nw].was = (rd<u32>((u32)r + 8) & 0x800) != 0; nw++; }
			}
		}
	}
	for (int i = 0; i < nw;)
	{
		if (now < w[i].at) { i++; continue; }
		TESObjectREFR *r = refGone(w[i].id) ? 0 : (TESObjectREFR *)lookup(w[i].id);
		if (r && formtype(r) == 0x3A && !w[i].was && (rd<u32>((u32)r + 8) & 0x800)) { objSend(pl, r, 2, 0); LOG("share: %08x went when you shot it - in friends' worlds too", w[i].id); }
		w[i] = w[--nw];
	}
}
static void objIn(u8 peer, const net::Event &ev, const char *who)
{
	net::EvObj e; memcpy(&e, ev.data, sizeof e);
	TESObjectREFR *r = refOf(e.ref), *pl = player();
	if (!r || !pl || formtype(r) != 0x3A) return;
	static double since[4]; static int count[4];
	if (peer < 4) { if (rt::now() - since[peer] > 30) { since[peer] = rt::now(); count[peer] = 0; } if (++count[peer] > 120) return; }
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	if (((e.world ^ world) & 0xFFFFFF) || ((e.cell ^ cid) & 0xFFFFFF) || (!e.cell) != (!cid)) return;
	const float *rp = pos(r);
	float dx = rp[0] - e.pos[0], dy = rp[1] - e.pos[1], dz = rp[2] - e.pos[2];
	if (dx * dx + dy * dy + dz * dz > 8500.0f * 8500.0f) return;
	if (e.op == 1 && e.dmg > 0 && e.dmg <= 10000 && s_objDamage) { g_script->CallFunctionAlt(s_objDamage, player(), 2, r, F(e.dmg)); LOG("share: %s's shot hit %08x here too (%.0f)", who, refid(r), e.dmg); }
	else if (e.op == 2 && s_objOff && !(rd<u32>((u32)r + 8) & 0x800)) { g_script->CallFunctionAlt(s_objOff, player(), 1, r); LOG("share: %08x went in %s's world - here too", refid(r), who); }
}
}

// ---- The guest's side of joining a host's
namespace coop
{
static const char *const kAV[] = {"Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck", "Barter",
	"EnergyWeapons", "Explosives", "Guns", "Lockpick", "Medicine", "MeleeWeapons", "Repair", "Science", "Sneak", "Speech",
	"Survival", "Unarmed", "Karma", "XP"};
enum { kNAV = sizeof kAV / sizeof kAV[0], kXP = kNAV - 1, kKarma = kNAV - 2, kMaxPerk = 400, kMaxItem = ghosts::kMaxInv, kMaxWorn = 24,
	kMaxRep = 64, kMaxPlace = 2000 };
static const u32 kPlayerBase = 0x7;
struct Sheet
{
	char name[64];
	int level;
	int av[kNAV];
	int nPerk; u32 perk[kMaxPerk]; int rank[kMaxPerk];
	int nItem; u32 item[kMaxItem]; int count[kMaxItem];
	int nWorn; u32 worn[kMaxWorn];
	int nRep; u32 rep[kMaxRep]; int fame[kMaxRep], infamy[kMaxRep];
	int nPlace; u32 place[kMaxPlace]; u8 placeFlags[kMaxPlace];
	bool hasLook;
	u32 race, hair, eyes, hairColor, heightBits, hairLenBits;
	int female, nParts;
	u32 parts[4];
	u32 face[130];
};
static Script *sAV[kNAV], *sLevel, *sPerk, *sCount, *sWorn;
static bool compiled;
static Script *cmp(const char *src)
{
	Script *s = g_script->CompileScript(src);
	if (!s) LOG("coop: couldn't compile \"%s\"", src);
	return s;
}
static void compile()
{
	if (compiled) return;
	compiled = true;
	char src[200];
	for (int i = 0; i < kNAV; i++)
	{
		rt::fmt(src, sizeof src, "Begin Function {}\nSetFunctionValue (player.%s %s)\nEnd", i == kXP || i == kKarma ? "GetAV" : "GetBaseAV", kAV[i]);
		sAV[i] = cmp(src);
	}
	sLevel = cmp("Begin Function {}\nSetFunctionValue (player.GetLevel)\nEnd");
	sPerk = cmp("Begin Function {ref rP}\nSetFunctionValue (player.HasPerkRank rP)\nEnd");
	sCount = cmp("Begin Function {ref rI}\nSetFunctionValue (player.GetItemCount rI)\nEnd");
	sWorn = cmp("Begin Function {ref rI}\nSetFunctionValue (player.GetEquipped rI)\nEnd");
}
static bool num(Script *s, TESForm *arg, double *out)
{
	if (!s) return false;
	NVSEElement r; memset(&r, 0, sizeof r);
	if (arg) g_script->CallFunction(s, player(), 0, &r, 1, arg);
	else g_script->CallFunction(s, player(), 0, &r, 0);
	if (r.type != 1) return false;
	*out = r.num;
	return true;
}
static int iround(double v) { return (int)(v + (v < 0 ? -0.5 : 0.5)); }
static int formsOf(u8 type, u32 *ids, u32 *forms, int cap)
{
	u32 map = ptr(ADDR_FORM_MAP);
	if (!map) return 0;
	u32 buckets = rd<u32>(map + 4), table = ptr(map + 8);
	if (!buckets || !table || buckets > (1u << 24)) return 0;
	int n = 0;
	for (u32 b = 0; b < buckets && n < cap; b++)
		for (u32 e = ptr(table + 4 * b); e && n < cap; e = ptr(e))
		{
			u32 f = ptr(e + 8);
			if (!f || formtype((void *)f) != type) continue;
			ids[n] = rd<u32>(e + 4); forms[n] = f; n++;
		}
	return n;
}
static u32 s_ids[60000], s_forms[60000];
static bool perkCarried(u32 f) { return !rd<u8>(f + 0x3C); }
static u32 markerData(u32 ref)
{
	for (u32 x = ptr(ref + 0x48), k = 0; x && k < 64; x = ptr(x + 8), k++)
		if (rd<u8>(x + 4) == 0x2C) { u32 d = ptr(x + 0xC); return d && readable(d, 0x14) ? d : 0; }
	return 0;
}
static u32 fbits(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static float bitsf(u32 b) { float f; memcpy(&f, &b, 4); return f; }

static void capture(Sheet &S)
{
	compile();
	memset(&S, 0, sizeof S);
	TESObjectREFR *pl = player();
	u32 base = (u32)lookup(kPlayerBase);
	if (!pl || !base) return;
	u32 np = ptr(base + 0xD4);
	rt::strcpy(S.name, np && readable(np, 4) ? (const char *)np : "", sizeof S.name);
	double v = 0;
	if (num(sLevel, 0, &v)) S.level = iround(v);
	for (int i = 0; i < kNAV; i++) if (num(sAV[i], 0, &v)) S.av[i] = iround(v);
	int n = formsOf(0x56, s_ids, s_forms, 60000);
	for (int i = 0; i < n && S.nPerk < kMaxPerk; i++)
		if (perkCarried(s_forms[i]) && num(sPerk, (TESForm *)s_forms[i], &v) && v > 0) { S.perk[S.nPerk] = s_ids[i]; S.rank[S.nPerk++] = iround(v); }
	static ghosts::Inv I;
	ghosts::readInv(pl, I);
	for (int i = 0; i < I.n && S.nItem < kMaxItem; i++)
	{
		bool dup = false;
		for (int k = 0; k < i && !dup; k++) dup = I.form[k] == I.form[i];
		if (dup || !num(sCount, (TESForm *)I.form[i], &v) || v <= 0) continue;
		S.item[S.nItem] = refid((void *)I.form[i]); S.count[S.nItem++] = iround(v);
		if (S.nWorn < kMaxWorn && (formtype((void *)I.form[i]) == 0x18 || formtype((void *)I.form[i]) == 0x28) && num(sWorn, (TESForm *)I.form[i], &v) && v > 0)
			S.worn[S.nWorn++] = refid((void *)I.form[i]);
	}
	n = formsOf(0x68, s_ids, s_forms, 60000);
	for (int i = 0; i < n && S.nRep < kMaxRep; i++)
	{
		int fa = (int)rd<float>(s_forms[i] + 0x44), in = (int)rd<float>(s_forms[i] + 0x48);
		if (fa || in) { S.rep[S.nRep] = s_ids[i]; S.fame[S.nRep] = fa; S.infamy[S.nRep++] = in; }
	}
	if (u32 map = ptr(ADDR_FORM_MAP))
	{
		u32 buckets = rd<u32>(map + 4), table = ptr(map + 8);
		if (table && buckets && buckets < (1u << 24))
			for (u32 bk = 0; bk < buckets && S.nPlace < kMaxPlace; bk++)
				for (u32 e = ptr(table + 4 * bk); e && S.nPlace < kMaxPlace; e = ptr(e))
				{
					u32 f = ptr(e + 8);
					if (!f || formtype((void *)f) != 0x3A) continue;
					if (u32 d = markerData(f)) { u8 fl = rd<u8>(d + 0xC) & 3; if (fl) { S.place[S.nPlace] = rd<u32>(e + 4); S.placeFlags[S.nPlace++] = fl; } }
				}
	}
	S.hasLook = true;
	u32 race = ptr(base + 0x110), hair = ptr(base + 0x1B8), eyes = ptr(base + 0x1C0);
	S.race = race ? refid((void *)race) : 0; S.hair = hair ? refid((void *)hair) : 0; S.eyes = eyes ? refid((void *)eyes) : 0;
	S.female = rd<u32>(base + 0x34) & 1;
	S.hairLenBits = rd<u32>(base + 0x1BC); S.hairColor = rd<u32>(base + 0x1D8); S.heightBits = rd<u32>(base + 0x1F4);
	for (u32 nd = base + 0x1DC, k = 0; nd && k < 16 && S.nParts < 4; nd = ptr(nd + 4), k++) if (u32 hp = ptr(nd)) S.parts[S.nParts++] = refid((void *)hp);
	int at = 0;
	for (int k = 0; k < 3; k++) { if (const float *f = ghosts::faceVals(base, k)) memcpy(S.face + at, f, ghosts::kFaceN[k] * 4); at += ghosts::kFaceN[k]; }
}

static int toText(const Sheet &S, char *buf, int cap)
{
	int len = 0;
	len += rt::fmt(buf + len, cap - len, "name\t%s\r\nlevel\t%d\r\n", S.name, S.level);
	for (int i = 0; i < kNAV; i++) len += rt::fmt(buf + len, cap - len, "av\t%s\t%d\r\n", kAV[i], S.av[i]);
	for (int i = 0; i < S.nPerk && len < cap - 100; i++) len += rt::fmt(buf + len, cap - len, "perk\t%08x\t%d\r\n", S.perk[i], S.rank[i]);
	for (int i = 0; i < S.nItem && len < cap - 100; i++) len += rt::fmt(buf + len, cap - len, "item\t%08x\t%d\r\n", S.item[i], S.count[i]);
	for (int i = 0; i < S.nWorn && len < cap - 100; i++) len += rt::fmt(buf + len, cap - len, "worn\t%08x\r\n", S.worn[i]);
	for (int i = 0; i < S.nRep && len < cap - 100; i++) len += rt::fmt(buf + len, cap - len, "rep\t%08x\t%d\t%d\r\n", S.rep[i], S.fame[i], S.infamy[i]);
	for (int i = 0; i < S.nPlace && len < cap - 100; i++) len += rt::fmt(buf + len, cap - len, "place\t%08x\t%d\r\n", S.place[i], S.placeFlags[i]);
	if (S.hasLook)
	{
		len += rt::fmt(buf + len, cap - len, "look\trace\t%08x\r\nlook\tfemale\t%d\r\nlook\thair\t%08x\r\nlook\thairlength\t%08x\r\nlook\teyes\t%08x\r\nlook\thaircolor\t%08x\r\nlook\theight\t%08x\r\n",
			S.race, S.female, S.hair, S.hairLenBits, S.eyes, S.hairColor, S.heightBits);
		for (int i = 0; i < S.nParts; i++) len += rt::fmt(buf + len, cap - len, "look\tpart\t%08x\r\n", S.parts[i]);
		for (int k = 0; k < 130 && len < cap - 40; k += 10)
		{
			len += rt::fmt(buf + len, cap - len, "look\tface%d", k);
			for (int j = k; j < k + 10 && j < 130; j++) len += rt::fmt(buf + len, cap - len, "\t%08x", S.face[j]);
			len += rt::fmt(buf + len, cap - len, "\r\n");
		}
	}
	return len;
}
static bool field(const char *s, int k, char *out, int cap)
{
	for (int i = 0; i < k; i++) { while (*s && *s != '\t' && *s != '\r' && *s != '\n') s++; if (*s != '\t') return false; s++; }
	int n = 0;
	while (s[n] && s[n] != '\t' && s[n] != '\r' && s[n] != '\n' && n < cap - 1) { out[n] = s[n]; n++; }
	out[n] = 0;
	return true;
}
static bool fromText(const char *txt, Sheet &S)
{
	memset(&S, 0, sizeof S);
	if (!txt) return false;
	char a[64], b[64], c[64], d[64];
	for (const char *s = txt; *s; )
	{
		if (field(s, 0, a, sizeof a))
		{
			if (rt::streq(a, "name")) field(s, 1, S.name, sizeof S.name);
			else if (rt::streq(a, "level") && field(s, 1, b, sizeof b)) S.level = rt::parsei(b);
			else if (rt::streq(a, "av") && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c)) { for (int i = 0; i < kNAV; i++) if (rt::streq(kAV[i], b)) S.av[i] = rt::parsei(c); }
			else if (rt::streq(a, "perk") && S.nPerk < kMaxPerk && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c)) { S.perk[S.nPerk] = rt::parsehex(b); S.rank[S.nPerk++] = rt::parsei(c); }
			else if (rt::streq(a, "item") && S.nItem < kMaxItem && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c)) { S.item[S.nItem] = rt::parsehex(b); S.count[S.nItem++] = rt::parsei(c); }
			else if (rt::streq(a, "worn") && S.nWorn < kMaxWorn && field(s, 1, b, sizeof b)) S.worn[S.nWorn++] = rt::parsehex(b);
			else if (rt::streq(a, "rep") && S.nRep < kMaxRep && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c) && field(s, 3, d, sizeof d)) { S.rep[S.nRep] = rt::parsehex(b); S.fame[S.nRep] = rt::parsei(c); S.infamy[S.nRep++] = rt::parsei(d); }
			else if (rt::streq(a, "place") && S.nPlace < kMaxPlace && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c)) { S.place[S.nPlace] = rt::parsehex(b); S.placeFlags[S.nPlace++] = (u8)rt::parsei(c); }
			else if (rt::streq(a, "look") && field(s, 1, b, sizeof b))
			{
				S.hasLook = true;
				field(s, 2, c, sizeof c);
				u32 h = rt::parsehex(c);
				if (rt::streq(b, "race")) S.race = h;
				else if (rt::streq(b, "female")) S.female = rt::parsei(c);
				else if (rt::streq(b, "hair")) S.hair = h;
				else if (rt::streq(b, "hairlength")) S.hairLenBits = h;
				else if (rt::streq(b, "eyes")) S.eyes = h;
				else if (rt::streq(b, "haircolor")) S.hairColor = h;
				else if (rt::streq(b, "height")) S.heightBits = h;
				else if (rt::streq(b, "part") && S.nParts < 4) S.parts[S.nParts++] = h;
				else if (rt::startswith(b, "face"))
				{
					int k0 = rt::parsei(b + 4);
					if (k0 >= 0) for (int j = 0; j < 10 && k0 + j < 130; j++) if (field(s, 2 + j, c, sizeof c)) S.face[k0 + j] = rt::parsehex(c);
				}
			}
		}
		while (*s && *s != '\n') s++;
		if (*s) s++;
	}
	return true;
}
static void path(char *out, int cap, const char *file) { rt::fmt(out, cap, "%sMojaveMostWanted\\coop\\%s", g_dir, file); }
static bool save(const Sheet &S, const char *file)
{
	int cap = 1 << 20;
	char *buf = (char *)rt::alloc(cap), p[400];
	int len = toText(S, buf, cap);
	path(p, sizeof p, file);
	bool ok = rt::writefile(p, buf, (u32)len, false);
	rt::free(buf);
	return ok;
}
static bool load(const char *file, Sheet &S)
{
	char p[400];
	path(p, sizeof p, file);
	u32 ln = 0;
	char *txt = rt::readfile(p, &ln);
	if (!txt) return false;
	fromText(txt, S);
	rt::free(txt);
	return true;
}
static bool exists(const char *file) { char p[400]; path(p, sizeof p, file); return GetFileAttributesA(p) != 0xFFFFFFFF; }

static int findIn(const u32 *ids, int n, u32 id) { for (int i = 0; i < n; i++) if (ids[i] == id) return i; return -1; }
static void cmd(const char *line) { console(line); }
static u32 *s_qRef; static int s_nQRef = -1;
static void questRefsScan()
{
	s_nQRef = 0;
	u32 map = ptr(ADDR_FORM_MAP);
	if (!map || !readable(map, 12)) return;
	u32 nb = rd<u32>(map + 4), bk = ptr(map + 8);
	if (!nb || !readable(bk, nb * 4)) return;
	static u32 buf[4096]; s_qRef = buf;
	int quests = 0;
	for (u32 i = 0; i < nb; i++)
		for (u32 e = ptr(bk + 4 * i), g = 0; e && g < 4096; e = ptr(e), g++)
		{
			u32 fm = ptr(e + 8);
			if (!fm || formtype((void *)fm) != 0x47) continue;
			u32 sc = ptr(fm + 0x1C);
			if (!sc || !readable(sc, 0x50) || formtype((void *)sc) != 0x11) continue;
			quests++;
			for (u32 nd = sc + 0x44, k = 0; nd && k < 512 && readable(nd, 8); nd = ptr(nd + 4), k++)
			{
				u32 rv = ptr(nd);
				if (!rv || !readable(rv, 0x10)) continue;
				u32 f = ptr(rv + 8);
				if (!f || !readable(f, 0x10)) continue;
				u8 ty = formtype((void *)f);
				if ((ty == 0x1F || ty == 0x2E || ty == 0x31) && s_nQRef < 4096) buf[s_nQRef++] = refid((void *)f);
			}
		}
	LOG("coop: quest items - %d misc items, keys and notes named by %d quests' scripts (never taken home)", s_nQRef, quests);
}
static bool questItem(u32 id)
{
	TESForm *f = lookup(id);
	if (!f) return false;
	if (rd<u32>((u32)f + 8) & 0x400) return true;
	if (s_nQRef < 0) questRefsScan();
	for (int i = 0; i < s_nQRef; i++) if (s_qRef[i] == id) return true;
	return false;
}
struct Counts { int values, perks, items, worn, reps, places; };
static Counts put(const Sheet &T, bool look, bool removeOthers)
{
	compile();
	Counts C = {};
	TESObjectREFR *pl = player();
	u32 base = (u32)lookup(kPlayerBase);
	if (!pl || !base) return C;
	char line[200];
	double v = 0;
	if (look && T.name[0]) { rt::fmt(line, sizeof line, "player.SetName \"%s\"", T.name); cmd(line); }
	if (num(sLevel, 0, &v) && iround(v) != T.level && T.level > 0) { rt::fmt(line, sizeof line, "player.SetLevel %d", T.level); cmd(line); C.values++; }
	for (int i = 0; i < kNAV; i++)
	{
		if (!num(sAV[i], 0, &v) || iround(v) == T.av[i]) continue;
		C.values++;
		if (i == kXP)
		{
			void **vt = *(void ***)pl;
			((void(__thiscall *)(void *, u32, int, u32))vt[0x3A8 / 4])(pl, 24, T.av[i] - iround(v), 0);
			continue;
		}
		if (i == kKarma) { rt::fmt(line, sizeof line, "player.RewardKarma %d", T.av[i] - iround(v)); cmd(line); continue; }
		double left = 0;
		rt::fmt(line, sizeof line, "player.SetAV %s 0", kAV[i]); cmd(line);
		num(sAV[i], 0, &left);
		rt::fmt(line, sizeof line, "player.SetAV %s %d", kAV[i], T.av[i] - iround(left)); cmd(line);
	}
	int n = formsOf(0x56, s_ids, s_forms, 60000);
	for (int i = 0; i < n; i++)
	{
		if (!perkCarried(s_forms[i])) continue;
		int k = findIn(T.perk, T.nPerk, s_ids[i]), want = k >= 0 ? T.rank[k] : 0;
		int have = num(sPerk, (TESForm *)s_forms[i], &v) ? iround(v) : 0;
		if (!removeOthers && k < 0) continue;
		for (int g = 0; g < 12 && have > want; g++) { rt::fmt(line, sizeof line, "player.RemovePerk %08x", s_ids[i]); cmd(line); C.perks++; have = num(sPerk, (TESForm *)s_forms[i], &v) ? iround(v) : 0; }
		for (int g = 0; g < 12 && have < want; g++) { rt::fmt(line, sizeof line, "player.AddPerk %08x", s_ids[i]); cmd(line); C.perks++; int was = have; have = num(sPerk, (TESForm *)s_forms[i], &v) ? iround(v) : 0; if (have == was) break; }
	}
	static ghosts::Inv I;
	ghosts::readInv(pl, I);
	if (removeOthers)
		for (int i = 0; i < I.n; i++)
		{
			u32 id = refid((void *)I.form[i]);
			if (findIn(T.item, T.nItem, id) >= 0 || !num(sCount, (TESForm *)I.form[i], &v) || v <= 0) continue;
			rt::fmt(line, sizeof line, "player.RemoveItem %08x %d 1", id, iround(v)); cmd(line); C.items++;
		}
	for (int i = 0; i < T.nItem; i++)
	{
		TESForm *f = lookup(T.item[i]);
		if (!f) continue;
		int have = num(sCount, f, &v) ? iround(v) : 0, d = T.count[i] - have;
		if (d > 0) { rt::fmt(line, sizeof line, "player.AddItem %08x %d 1", T.item[i], d); cmd(line); C.items++; }
		if (d < 0) { rt::fmt(line, sizeof line, "player.RemoveItem %08x %d 1", T.item[i], -d); cmd(line); C.items++; }
	}
	for (int i = 0; i < T.nWorn; i++)
	{
		TESForm *f = lookup(T.worn[i]);
		if (!f || (num(sWorn, f, &v) && v > 0)) continue;
		rt::fmt(line, sizeof line, "player.EquipItem %08x 0 1", T.worn[i]); cmd(line); C.worn++;
	}
	n = formsOf(0x68, s_ids, s_forms, 60000);
	for (int i = 0; i < n; i++)
	{
		int k = findIn(T.rep, T.nRep, s_ids[i]);
		if (k < 0 && !removeOthers) continue;
		float fa = k >= 0 ? (float)T.fame[k] : 0, in = k >= 0 ? (float)T.infamy[k] : 0;
		float *pf = (float *)(s_forms[i] + 0x44), *pi = (float *)(s_forms[i] + 0x48);
		if ((int)*pf != (int)fa || (int)*pi != (int)in) { *pf = fa; *pi = in; C.reps++; }
	}
	for (int i = 0; i < T.nPlace; i++)
	{
		TESForm *r = lookup(T.place[i]);
		u32 d = r ? markerData((u32)r) : 0;
		if (!d) continue;
		u8 have = rd<u8>(d + 0xC) & 3, want = T.placeFlags[i] & 3;
		if ((have | want) == have) continue;
		rt::fmt(line, sizeof line, (want & 2) ? "ShowMap %08x 1" : "ShowMap %08x", T.place[i]); cmd(line); C.places++;
	}
	if (look && T.hasLook)
	{
		TESForm *race = lookup(T.race), *hair = lookup(T.hair), *eyes = lookup(T.eyes);
		if (race && formtype(race) == 0x0C) *(u32 *)(base + 0x110) = (u32)race;
		if ((int)(rd<u8>(base + 0x34) & 1) != T.female) ((void(__thiscall *)(u32, u32, u32, u32))0x47DD50)(base + 0x30, 1, T.female ? 1 : 0, 1);
		if (hair && formtype(hair) == 0x0A) *(u32 *)(base + 0x1B8) = (u32)hair;
		float hl = bitsf(T.hairLenBits), ht = bitsf(T.heightBits);
		if (hl >= 0 && hl <= 10) *(float *)(base + 0x1BC) = hl;
		if (eyes && formtype(eyes) == 0x0B) *(u32 *)(base + 0x1C0) = (u32)eyes;
		*(u32 *)(base + 0x1D8) = T.hairColor;
		if (ht > 0.5f && ht < 2) *(float *)(base + 0x1F4) = ht;
		int at = 0;
		for (int k = 0; k < 3; k++) { if (float *f = ghosts::faceVals(base, k)) memcpy(f, (const float *)T.face + at, ghosts::kFaceN[k] * 4); at += ghosts::kFaceN[k]; }
		u32 parts[4]; int np = 0;
		for (int i = 0; i < T.nParts; i++) { TESForm *f = lookup(T.parts[i]); if (f && formtype(f) == 0x09) parts[np++] = (u32)f; }
		*(u32 *)(base + 0x1DC) = np ? parts[0] : 0;
		u32 next = 0;
		for (int i = np - 1; i >= 1; i--)
		{
			u32 nd = ((u32(__thiscall *)(u32, u32))0xAA3E40)(0x11F6238, 8);
			if (!nd) break;
			*(u32 *)nd = parts[i]; *(u32 *)(nd + 4) = next; next = nd;
		}
		*(u32 *)(base + 0x1E0) = next;
		cmd("player.Update3D");
	}
	return C;
}
static int compare(const Sheet &want, const Sheet &got, const char *what, bool lookToo)
{
	int cap = 1 << 20;
	char *a = (char *)rt::alloc(cap), *b = (char *)rt::alloc(cap), l[300];
	static Sheet w, g;
	w = want; g = got;
	if (!lookToo) { w.hasLook = g.hasLook = false; w.name[0] = g.name[0] = 0; }
	toText(w, a, cap); toText(g, b, cap);
	int lines = 0, bad = 0;
	for (const char *s = a; *s; )
	{
		int k = 0;
		while (s[k] && s[k] != '\r' && s[k] != '\n' && k < 299) { l[k] = s[k]; k++; }
		l[k] = 0;
		if (k)
		{
			lines++;
			bool found = false;
			u32 L = strlen(l);
			for (const char *t = b; *t && !found; )
			{
				if (!memcmp(t, l, L) && (t[L] == '\r' || t[L] == '\n' || !t[L])) found = true;
				while (*t && *t != '\n') t++;
				if (*t) t++;
			}
			if (!found && ++bad <= 30) LOG("coop:   not as wanted (%s): %s", what, l);
		}
		s += k;
		while (*s == '\r' || *s == '\n') s++;
	}
	LOG("coop: %s - %d of %d lines as wanted", what, lines - bad, lines);
	rt::free(a); rt::free(b);
	return bad;
}

enum { IDLE, WAIT_SESSION, IN_SESSION, GOING_HOME };
static int state;
static double nextCheckpoint, nextCmd, pendingAt;
static int pending;
static char loadedSave[300];
static const char *kStateName[] = {"idle", "waiting for the session", "in a session", "going home"};
static void setState(int s)
{
	state = s;
	if (s != WAIT_SESSION) net::setFileWanted(false);
	char p[400];
	path(p, sizeof p, "state.txt");
	rt::writefile(p, kStateName[s], strlen(kStateName[s]), false);
	LOG("coop: now %s", kStateName[s]);
}
static void readState()
{
	char p[400];
	path(p, sizeof p, "state.txt");
	u32 n = 0;
	char *t = rt::readfile(p, &n);
	state = IDLE;
	if (t) { for (int i = 0; i < 4; i++) if (rt::streq(t, kStateName[i])) state = i; rt::free(t); }
}
static bool has(const char *s, const char *what)
{
	for (; *s; s++)
	{
		int k = 0;
		while (what[k] && s[k] && (s[k] | 0x20) == (what[k] | 0x20)) k++;
		if (!what[k]) return true;
	}
	return false;
}
static bool away;
static bool live() { return state == IN_SESSION && !away; }
static void join()
{
	if (state != IDLE) { message("Co-op: you're already joining or in a session (/leave first).", 4); LOG("coop: join refused - %s", kStateName[state]); return; }
	if (has(loadedSave, "MMW-Session")) { message("Co-op: this is a session save - load your own game to join.", 5); LOG("coop: join refused - MMW-Session is loaded"); return; }
	static Sheet S;
	capture(S);
	save(S, "home-sheet.txt");
	cmd("save MMW-Home");
	setState(WAIT_SESSION);
	LOG("coop: joining - your character written (%d perks, %d items, %d places) and your game saved as MMW-Home", S.nPerk, S.nItem, S.nPlace);
	message("Co-op: your game is saved (MMW-Home). Joining...", 3);
}
static u32 fileLen(const char *name)
{
	char p[400]; path(p, sizeof p, name);
	u32 n = 0; char *t = rt::readfile(p, &n);
	if (!t) return 0;
	rt::free(t);
	return n;
}
static void markLedger()
{
	char b[48], p[400];
	int n = rt::fmt(b, sizeof b, "%u %u", fileLen("ledger.txt"), fileLen("excluded.txt"));
	path(p, sizeof p, "ledger-at.txt");
	rt::writefile(p, b, (u32)n, false);
}
static void cutLedger()
{
	char p[400]; path(p, sizeof p, "ledger-at.txt");
	u32 n = 0; char *t = rt::readfile(p, &n);
	u32 keep[2] = {0, 0};
	if (t)
	{
		const char *c = t;
		for (int k = 0; k < 2; k++) { while (*c == ' ') c++; keep[k] = (u32)rt::parsei(c); while (*c && *c != ' ') c++; }
		rt::free(t);
	}
	static const char *kName[2] = {"ledger.txt", "excluded.txt"};
	for (int k = 0; k < 2; k++)
	{
		char q[400]; path(q, sizeof q, kName[k]);
		u32 len = 0; char *d = rt::readfile(q, &len);
		if (!d) continue;
		if (len > keep[k])
		{
			if (keep[k]) rt::writefile(q, d, keep[k], false); else DeleteFileA(q);
			LOG("coop: %s cut back to the last checkpoint (%u of %u bytes)", kName[k], keep[k], len);
		}
		rt::free(d);
	}
}
static const char *savesDir();
static void sessionFilesOff(const char *why)
{
	int n = 0;
	static const char *ext[2] = {".fos", ".nvse"};
	for (int i = 0; i < 2; i++) { char p[400]; rt::fmt(p, sizeof p, "%sMMW-Session%s", savesDir(), ext[i]); if (DeleteFileA(p)) n++; }
	LOG("coop: the session save taken away (%s: %d files)", why, n);
}
static void checkpoint(const char *why)
{
	static Sheet S;
	capture(S);
	save(S, "session-now.txt");
	markLedger();
	LOG("coop: checkpoint (%s)", why);
}
#pragma pack(push, 1)
struct QItem { net::FormKey k; u16 n; };
#pragma pack(pop)
static bool netSession;
static void handQuestItems()
{
	static Sheet start, now;
	if (!netSession || !load("session-start.txt", start) || !load("session-now.txt", now)) return;
	QItem q[10]; int n = 0, total = 0;
	for (int i = 0; i < now.nItem; i++)
	{
		if (!questItem(now.item[i])) continue;
		int ks = findIn(start.item, start.nItem, now.item[i]), d = now.count[i] - (ks >= 0 ? start.count[ks] : 0);
		if (d <= 0) continue;
		q[n].k = ghosts::formKey(now.item[i]); q[n].n = (u16)d;
		if (!q[n].k.low) continue;
		n++; total++;
		if (n == 10) { net::addEvent(net::EV_QUESTITEMS, q, (u8)(n * sizeof(QItem))); n = 0; }
	}
	if (n) net::addEvent(net::EV_QUESTITEMS, q, (u8)(n * sizeof(QItem)));
	if (total) LOG("coop: %d quest item(s) handed to the host", total);
}
static u8 sessionPeers;
void questItemsIn(u8 peer, const void *data, int len, const char *who)
{
	if (!(sessionPeers & (1 << peer))) { LOG("coop: quest items from %s - not in your world, not taken", who); return; }
	int n = len / (int)sizeof(QItem), got = 0;
	char line[100];
	for (int i = 0; i < n; i++)
	{
		QItem q; memcpy(&q, (const u8 *)data + i * sizeof(QItem), sizeof q);
		TESForm *f = ghosts::formOf(q.k);
		if (!f || !q.n || !questItem(refid(f))) continue;
		rt::fmt(line, sizeof line, "player.AddItem %08x %d", refid(f), q.n > 20 ? 20 : (int)q.n); cmd(line); got++;
	}
	if (got) { char m[120]; rt::fmt(m, sizeof m, "%s handed you %d quest item%s they picked up.", who, got, got == 1 ? "" : "s"); message(m, 5); LOG("coop: %s", m); }
}
static void leave()
{
	if (state == WAIT_SESSION) { setState(IDLE); message("Co-op: stopped joining - you're in your own game.", 4); return; }
	if (state != IN_SESSION) { LOG("coop: leave - not in a session (%s)", kStateName[state]); return; }
	if (!away) checkpoint("leaving");
	handQuestItems();
	setState(GOING_HOME);
	message("Co-op: going home with what you earned...", 3);
	cmd("load MMW-Home");
}
static void doSwap()
{
	static Sheet home, before;
	if (!load("home-sheet.txt", home)) { LOG("coop: no home-sheet.txt - the session save stays the host's character"); return; }
	capture(before);
	save(before, "session-host-player.txt");
	double t0 = rt::now();
	Counts C = put(home, true, true);
	LOG("coop: your character put in (%d values, %d perk ranks, %d items, %d worn, %d reputations, %d places) in %.0f ms",
		C.values, C.perks, C.items, C.worn, C.reps, C.places, (rt::now() - t0) * 1000);
	pending = 4; pendingAt = rt::now() + 2;
}
static void swapDone()
{
	static Sheet home, after, w, g;
	load("home-sheet.txt", home);
	capture(after);
	w = home; w.nPlace = 0;
	g = after; g.nPlace = 0;
	compare(w, g, "the swap", true);
	save(after, "session-start.txt");
	save(after, "session-now.txt");
	if (TESObjectREFR *pl = player())
	{
		const float *pp = pos(pl); float yaw = rot(pl)[2];
		float to[3] = {pp[0] + 85 * rt::cos(yaw), pp[1] - 85 * rt::sin(yaw), pp[2] + 5};
		((void(__thiscall *)(u32, const float *))0x575830)((u32)pl, to);
		LOG("coop: you stand beside the host's spot");
	}
	{ char lp[400]; path(lp, sizeof lp, "ledger.txt"); DeleteFileA(lp); path(lp, sizeof lp, "excluded.txt"); DeleteFileA(lp); }
	markLedger();
	away = false;
	setState(IN_SESSION);
	nextCheckpoint = rt::now() + 180;
	message("Co-op: you're in, as yourself.", 3);
	sessionFilesOff("you're in");
}
static Sheet homeTarget, homeCur;
static void homeFinish();
bool inSession() { return live(); }
static bool friendNear()
{
	TESObjectREFR *pl = player();
	if (!pl) return false;
	const float *pp = pos(pl);
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = 0, cid = 0;
	if (cell) { if (rd<u8>(cell + OFF_CELL_FLAGS) & 1) cid = refid((void *)cell); else world = refid((void *)ptr(cell + OFF_CELL_WORLD)); }
	net::Ghost g[4];
	int n = net::ghosts(g, 4);
	for (int i = 0; i < n; i++)
	{
		if (!g[i].fresh || ((g[i].s.world ^ world) & 0xFFFFFF) || ((g[i].s.cell ^ cid) & 0xFFFFFF) || (!g[i].s.cell) != (!cid)) continue;
		float dx = g[i].s.pos[0] - pp[0], dy = g[i].s.pos[1] - pp[1], dz = g[i].s.pos[2] - pp[2];
		if (dx * dx + dy * dy + dz * dz < 7000.0f * 7000.0f) return true;
	}
	return false;
}
#pragma pack(push, 1)
struct QStep { net::FormKey quest; u8 from, to, near; };
#pragma pack(pop)
static u32 fromOther[32][2]; static int nFromOther;
static bool stageDone(u32 q, int st);
bool sharing() { return live() || net::hosting(); }
struct Wallet { bool have; int xp, karma; ghosts::Inv inv; };
static Wallet wPrev;
static void walletTake(Wallet &w)
{
	compile();
	double v = 0;
	w.have = true;
	w.xp = num(sAV[kXP], 0, &v) ? iround(v) : 0;
	w.karma = num(sAV[kKarma], 0, &v) ? iround(v) : 0;
	if (TESObjectREFR *pl = player()) ghosts::readInv(pl, w.inv); else w.inv.n = 0;
}
void walletRoll() { if (live()) walletTake(wPrev); else wPrev.have = false; }
static void excludeReward(const char *edid, int to)
{
	if (!wPrev.have) return;
	static Wallet now;
	walletTake(now);
	char buf[2000], p[400];
	int n = 0, items = 0;
	if (now.xp != wPrev.xp) n += rt::fmt(buf + n, sizeof buf - n, "xp\t%d\r\n", now.xp - wPrev.xp);
	if (now.karma != wPrev.karma) n += rt::fmt(buf + n, sizeof buf - n, "karma\t%d\r\n", now.karma - wPrev.karma);
	for (int i = 0; i < now.inv.n && n < (int)sizeof buf - 40; i++)
	{
		int d = now.inv.count[i] - ghosts::invCount(wPrev.inv, now.inv.form[i]);
		if (d && !questItem(refid((void *)now.inv.form[i]))) { n += rt::fmt(buf + n, sizeof buf - n, "item\t%08x\t%d\r\n", refid((void *)now.inv.form[i]), d); items++; }
	}
	for (int i = 0; i < wPrev.inv.n && n < (int)sizeof buf - 40; i++)
		if (!ghosts::invHas(now.inv, wPrev.inv.form[i]) && wPrev.inv.count[i] && !questItem(refid((void *)wPrev.inv.form[i]))) { n += rt::fmt(buf + n, sizeof buf - n, "item\t%08x\t%d\r\n", refid((void *)wPrev.inv.form[i]), -wPrev.inv.count[i]); items++; }
	if (n)
	{
		path(p, sizeof p, "excluded.txt");
		rt::writefile(p, buf, (u32)n, true);
		LOG("coop:   %s stage %d isn't credited - what it gave stays here (XP %+d, karma %+d, %d items)", edid, to, now.xp - wPrev.xp, now.karma - wPrev.karma, items);
	}
	wPrev = now;
}
struct LocalStep { u32 quest; u8 stage; double t; bool seen; };
static LocalStep s_local[16]; static int s_nLocal;
static bool s_outOfSync; static char s_oosWhat[96];
void questStepIn(u8 peer, const void *data, int len, const char *who)
{
	if (len < (int)sizeof(QStep)) return;
	QStep q; memcpy(&q, data, sizeof q);
	if (peer == 0 && !net::hosting()) { TESForm *qf0 = ghosts::formOf(q.quest); for (int i = 0; i < s_nLocal; i++) if (qf0 && s_local[i].quest == refid(qf0) && s_local[i].stage == q.to) s_local[i].seen = true; }
	if (net::hosting() ? !(sessionPeers & (1 << peer)) : !(live() && netSession && peer == 0)) return;
	TESForm *f = ghosts::formOf(q.quest);
	if (!f || formtype(f) != 0x47) { LOG("coop: %s's quest step (stage %d) - the quest isn't in this game", who, q.to); return; }
	u32 qf = (u32)f;
	const char *ed = ptr(qf + 0x64) && readable(ptr(qf + 0x64), 4) ? (const char *)ptr(qf + 0x64) : "";
	int at = rd<u8>(qf + 0x60);
	if (stageDone(qf, q.to)) { LOG("coop: %s's step %s %d - done here already", who, ed, q.to); return; }
	if (at != q.from && net::hosting()) { LOG("coop: %s's step %s %d - not set here: this world is at stage %d, theirs was at %d", who, ed, q.to, at, q.from); return; }
	if (at != q.from) LOG("coop: the host's step %s %d - your copy of the quest was at %d (theirs at %d): the host's world counts", ed, q.to, at, q.from);
	fromOther[nFromOther % 32][0] = refid(f); fromOther[nFromOther % 32][1] = q.to; nFromOther++;
	char line[100];
	rt::fmt(line, sizeof line, "SetStage %08x %d", refid(f), q.to); cmd(line);
	int nEvp = 0;
	if (s_evp)
		if (TESObjectREFR *pl = player())
		{
			const float *pp = pos(pl);
			forEachActor([&](TESObjectREFR *a) {
				u8 ft = formtype(a);
				if (a == pl || (ft != 0x3B && ft != 0x3C) || deadNow(a) || nEvp >= 24) return;
				const float *ap = pos(a); float dx = ap[0] - pp[0], dy = ap[1] - pp[1];
				if (dx * dx + dy * dy > 6000.0f * 6000.0f) return;
				g_script->CallFunctionAlt(s_evp, player(), 1, a); nEvp++;
			});
		}
	u32 nm = ptr(qf + 0x34);
	char m[160]; rt::fmt(m, sizeof m, "%s: %s - a step done", who, nm && readable(nm, 4) ? (const char *)nm : ed);
	message(m, 4);
	LOG("coop: %s's step %s %d set here too (from %d); %d people round you look at what to do again", who, ed, q.to, q.from, nEvp);
}
void questStep(u32 quest, const char *edid, const char *name, int from, int to, u8 flags)
{
	if (!(flags & 0x20)) return;
	g_lastStep.quest = quest; g_lastStep.stage = (u8)to; g_lastStep.t = rt::now();
	bool near = friendNear();
	bool echo = false;
	for (int i = 0; i < 32 && i < nFromOther; i++) if (fromOther[i][0] == quest && (int)fromOther[i][1] == to) { echo = true; fromOther[i][0] = 0; }
	bool guestNear = !net::hosting() && near && state == IN_SESSION && netSession;
	if (guestNear && !echo)
	{
		LOG("coop: your step %s %d stays in your world - your host is with you, their conversation counts", edid, to);
		if (s_nLocal < 16) { LocalStep &L = s_local[s_nLocal++]; L.quest = quest; L.stage = (u8)to; L.t = rt::now(); L.seen = false; }
		if (!s_outOfSync) rt::fmt(s_oosWhat, sizeof s_oosWhat, "%s stage %d", edid, to);
	}
	if (!echo && !guestNear && net::active() && !net::soloTesting() && (net::hosting() || (state == IN_SESSION && netSession)))
	{
		QStep q = {ghosts::formKey(quest), (u8)from, (u8)to, (u8)(near ? 1 : 0)};
		if (q.quest.low) net::addEvent(net::EV_QUESTSTEP, &q, sizeof q);
	}
	if (!live()) return;
	char line[200], p[400];
	int n = rt::fmt(line, sizeof line, "step\t%08x\t%d\t%d\t%d\t%s\r\n", quest, from, to, near ? 1 : 0, edid);
	path(p, sizeof p, "ledger.txt");
	rt::writefile(p, line, (u32)n, true);
	LOG("coop: quest step - %s \"%s\" stage %d done (from %d), %s", edid, name, to, from, near ? "a friend within 100 m: it counts" : "no friend within 100 m");
	if (!near) excludeReward(edid, to);
}
static bool stageDone(u32 q, int st);
void xpIn(u8 peer, const void *data, int len, const char *who)
{
	if (len < (int)sizeof(net::EvXp) || TUNE(coopXpShare) <= 0) return;
	net::EvXp x; memcpy(&x, data, sizeof x);
	TESObjectREFR *pl = player();
	if (!pl || !x.amount || x.amount > 20000) return;
	static double since[4]; static int count[4];
	double now = rt::now();
	if (peer < 4) { if (now - since[peer] > 30) { since[peer] = now; count[peer] = 0; } if (++count[peer] > 120) { LOG("xp: %s's +%u dropped - more than 120 in 30 s", who, (unsigned)x.amount); return; } }
	u32 cell = ptr((u32)pl + OFF_REF_CELL), world = worldspace(), cid = interior() && cell ? refid((void *)cell) : 0;
	if (((x.world ^ world) & 0xFFFFFF) || ((x.cell ^ cid) & 0xFFFFFF) || (!x.cell) != (!cid)) { LOG("xp: %s's +%u - not where you are: not shared", who, (unsigned)x.amount); return; }
	const float *pp = pos(pl);
	float dx = x.pos[0] - pp[0], dy = x.pos[1] - pp[1], dz = x.pos[2] - pp[2], d = rt::sqrt(dx * dx + dy * dy + dz * dz);
	if (d > TUNE(coopXpRangeUnits)) { LOG("xp: %s's +%u - %.0f m away: not shared", who, (unsigned)x.amount, d / 70); return; }
	if (x.fromQuest && peer != 0 && !net::hosting()) { LOG("xp: %s's +%u came with a quest step - a guest takes quest XP from the host's world only", who, (unsigned)x.amount); return; }
	if (x.fromQuest)
	{
		TESForm *q = ghosts::formOf(x.quest);
		if (q && formtype(q) == 0x47 && stageDone((u32)q, x.stage)) { LOG("xp: %s's +%u came with a quest step your world did too - its XP came with it", who, (unsigned)x.amount); return; }
	}
	if (!s_giveXp) return;
	g_xpPending += x.amount; g_xpPendingT = now;
	g_script->CallFunctionAlt(s_giveXp, player(), 1, F((float)x.amount));
	LOG("xp: +%u from %s (%.0f m away)%s", (unsigned)x.amount, who, d / 70, x.fromQuest ? " - a quest step" : "");
}
static bool stageDone(u32 q, int st)
{
	int n = 0;
	for (u32 node = q + 0x44; node && n < 300; node = ptr(node + 4), n++)
	{
		u32 si = ptr(node);
		if (si && rd<u8>(si) == st) return rd<u8>(si + 1) != 0;
	}
	return false;
}
static int creditQuests()
{
	char p[400];
	path(p, sizeof p, "ledger.txt");
	u32 ln = 0;
	char *t = rt::readfile(p, &ln);
	if (!t) return 0;
	int credited = 0;
	char a[64], b[64], c[64], d[64], e[64], line[160];
	for (const char *s = t; *s; )
	{
		if (field(s, 0, a, sizeof a) && rt::streq(a, "step") && field(s, 1, b, sizeof b) && field(s, 2, c, sizeof c) && field(s, 3, d, sizeof d) && field(s, 4, e, sizeof e))
		{
			u32 id = rt::parsehex(b);
			int from = rt::parsei(c), to = rt::parsei(d), near = rt::parsei(e);
			char ed[64] = ""; field(s, 5, ed, sizeof ed);
			TESForm *q = lookup(id);
			if (!q || formtype(q) != 0x47) LOG("coop:   %s stage %d - no such quest here", ed, to);
			else
			{
				u32 f = (u32)q;
				int at = rd<u8>(f + 0x60);
				bool running = (rd<u8>(f + 0x3C) & 1) != 0;
				if (!near) LOG("coop:   %s stage %d - not credited: no friend was within 100 m", ed, to);
				else if (stageDone(f, to)) LOG("coop:   %s stage %d - already done in your game", ed, to);
				else if (!running && at == 0 && from == 0)
				{
					u32 nm = ptr(f + 0x34);
					char note[200];
					int n = rt::fmt(note, sizeof note, "found with your friend: %s (%s) - go there to start it\r\n", nm && readable(nm, 4) ? (const char *)nm : ed, ed);
					char np[400]; path(np, sizeof np, "notes.txt");
					rt::writefile(np, note, (u32)n, true);
					rt::fmt(note, sizeof note, "Found with your friend: %s - start it yourself.", nm && readable(nm, 4) ? (const char *)nm : ed);
					message(note, 6);
					LOG("coop:   %s stage %d - only begun together: a note, not started", ed, to);
				}
				else if (at != from) LOG("coop:   %s stage %d - not credited: your game is at stage %d, the session's was at %d", ed, to, at, from);
				else { rt::fmt(line, sizeof line, "SetStage %08x %d", id, to); cmd(line); credited++; LOG("coop:   %s stage %d - credited (SetStage, by id: an editor ID can be missing)", ed, to); }
			}
		}
		while (*s && *s != '\n') s++;
		if (*s) s++;
	}
	rt::free(t);
	return credited;
}
static void goHome(bool crash)
{
	static Sheet start, now, cur, target;
	if (!load("session-start.txt", start) || !load("session-now.txt", now)) { LOG("coop: home - no session sheets"); setState(IDLE); return; }
	cutLedger();
	capture(cur);
	target = cur;
	target.hasLook = false; target.name[0] = 0;
	{
		bool lookChanged = now.hasLook && start.hasLook && (now.race != start.race || now.hair != start.hair || now.eyes != start.eyes || now.female != start.female || memcmp(now.face, start.face, sizeof now.face));
		if (lookChanged) { target.hasLook = true; target.race = now.race; target.hair = now.hair; target.eyes = now.eyes; target.hairColor = now.hairColor; target.heightBits = now.heightBits; target.hairLenBits = now.hairLenBits; target.female = now.female; target.nParts = now.nParts; memcpy(target.parts, now.parts, sizeof target.parts); memcpy(target.face, now.face, sizeof target.face); }
		if (!rt::streq(now.name, start.name)) rt::strcpy(target.name, now.name, sizeof target.name);
		if (lookChanged || target.name[0]) LOG("coop: home - %s%s%s as made in the session", lookChanged ? "your look" : "", lookChanged && target.name[0] ? " and " : "", target.name[0] ? "your name" : "");
	}
	target.level = cur.level + (now.level - start.level);
	for (int i = 0; i < kNAV; i++) target.av[i] = cur.av[i] + (now.av[i] - start.av[i]);
	for (int i = 0; i < now.nPerk; i++)
	{
		int ks = findIn(start.perk, start.nPerk, now.perk[i]), d = now.rank[i] - (ks >= 0 ? start.rank[ks] : 0);
		if (!d) continue;
		int kc = findIn(target.perk, target.nPerk, now.perk[i]);
		if (kc < 0 && target.nPerk < kMaxPerk) { kc = target.nPerk++; target.perk[kc] = now.perk[i]; target.rank[kc] = 0; }
		if (kc >= 0) target.rank[kc] += d;
	}
	for (int i = 0; i < start.nPerk; i++)
		if (findIn(now.perk, now.nPerk, start.perk[i]) < 0) { int kc = findIn(target.perk, target.nPerk, start.perk[i]); if (kc >= 0) target.rank[kc] -= start.rank[i]; }
	int gained = 0, lost = 0;
	auto itemDelta = [&](u32 id, int d) {
		if (!d || questItem(id)) return;
		int kc = findIn(target.item, target.nItem, id);
		if (kc < 0 && target.nItem < kMaxItem) { kc = target.nItem++; target.item[kc] = id; target.count[kc] = 0; }
		if (kc >= 0) { target.count[kc] += d; if (target.count[kc] < 0) target.count[kc] = 0; }
		if (d > 0) gained++; else lost++;
	};
	for (int i = 0; i < now.nItem; i++) { int ks = findIn(start.item, start.nItem, now.item[i]); itemDelta(now.item[i], now.count[i] - (ks >= 0 ? start.count[ks] : 0)); }
	for (int i = 0; i < start.nItem; i++) if (findIn(now.item, now.nItem, start.item[i]) < 0) itemDelta(start.item[i], -start.count[i]);
	target.nWorn = 0;
	for (int i = 0; i < now.nRep; i++)
	{
		int ks = findIn(start.rep, start.nRep, now.rep[i]);
		int df = now.fame[i] - (ks >= 0 ? start.fame[ks] : 0), di = now.infamy[i] - (ks >= 0 ? start.infamy[ks] : 0);
		if (!df && !di) continue;
		int kc = findIn(target.rep, target.nRep, now.rep[i]);
		if (kc < 0 && target.nRep < kMaxRep) { kc = target.nRep++; target.rep[kc] = now.rep[i]; target.fame[kc] = target.infamy[kc] = 0; }
		if (kc >= 0) { target.fame[kc] += df; target.infamy[kc] += di; }
	}
	target.nPlace = 0;
	for (int i = 0; i < now.nPlace && target.nPlace < kMaxPlace; i++)
	{
		int ks = findIn(start.place, start.nPlace, now.place[i]);
		u8 add = now.placeFlags[i] & ~(ks >= 0 ? start.placeFlags[ks] : 0);
		if (add) { target.place[target.nPlace] = now.place[i]; target.placeFlags[target.nPlace++] = now.placeFlags[i]; }
	}
	{
		char p[400]; path(p, sizeof p, "excluded.txt");
		u32 ln = 0; char *t = rt::readfile(p, &ln);
		if (t)
		{
			char a[32], b[32], c[32];
			int nx = 0;
			for (const char *x = t; *x; )
			{
				if (field(x, 0, a, sizeof a) && field(x, 1, b, sizeof b))
				{
					if (rt::streq(a, "xp")) target.av[kXP] -= rt::parsei(b);
					else if (rt::streq(a, "karma")) target.av[kKarma] -= rt::parsei(b);
					else if (rt::streq(a, "item") && field(x, 2, c, sizeof c))
					{
						u32 id = rt::parsehex(b); int d = rt::parsei(c);
						int kc = findIn(target.item, target.nItem, id);
						if (kc >= 0) { target.count[kc] -= d; if (target.count[kc] < 0) target.count[kc] = 0; }
					}
					nx++;
				}
				while (*x && *x != '\n') x++;
				if (*x) x++;
			}
			rt::free(t);
			LOG("coop: %d reward line(s) of uncredited quest steps kept out", nx);
		}
	}
	LOG("coop: going home%s - XP %+d, level %+d, karma %+d, %d items more, %d fewer, %d new places",
		crash ? " (after a crash: the last checkpoint)" : "", now.av[kXP] - start.av[kXP], now.level - start.level,
		now.av[kKarma] - start.av[kKarma], gained, lost, target.nPlace);
	homeTarget = target; homeCur = cur;
	if (creditQuests() > 0) { pending = 5; pendingAt = rt::now() + 2; return; }
	homeFinish();
}
static void homeFinish()
{
	static Sheet after, now2;
	Sheet &target = homeTarget;
	capture(now2);
	for (int i = 0; i < now2.nItem; i++)
		if (findIn(target.item, target.nItem, now2.item[i]) < 0 && findIn(homeCur.item, homeCur.nItem, now2.item[i]) < 0 && target.nItem < kMaxItem && !questItem(now2.item[i]))
			{ target.item[target.nItem] = now2.item[i]; target.count[target.nItem++] = 0; }
	Counts C = put(target, target.hasLook || target.name[0], false);
	capture(after);
	LOG("coop: home - %d values, %d perk ranks, %d items, %d reputations, %d places put in", C.values, C.perks, C.items, C.reps, C.places);
	static Sheet w, g, gp;
	w = target; g = after;
	w.nWorn = g.nWorn = 0;
	gp = g; gp.nPlace = 0;
	for (int i = 0; i < w.nPlace; i++) { int k = findIn(g.place, g.nPlace, w.place[i]); if (k >= 0 && gp.nPlace < kMaxPlace) { gp.place[gp.nPlace] = g.place[k]; gp.placeFlags[gp.nPlace++] = g.placeFlags[k]; } }
	compare(w, gp, "going home", false);
	save(after, "home-after.txt");
	{ char lp[400]; path(lp, sizeof lp, "ledger.txt"); DeleteFileA(lp); path(lp, sizeof lp, "excluded.txt"); DeleteFileA(lp); path(lp, sizeof lp, "ledger-at.txt"); DeleteFileA(lp); }
	cmd("save MMW-Home");
	setState(IDLE);
	message("Co-op: home, with what you earned.", 4);
}
static char s_saves[300];
static const char *savesDir()
{
	if (s_saves[0]) return s_saves;
	typedef long(__stdcall * GetFolder)(void *, int, void *, u32, char *);
	char docs[260] = "";
	if (HANDLE sh = LoadLibraryA("shell32.dll")) if (GetFolder f = (GetFolder)GetProcAddress(sh, "SHGetFolderPathA")) f(0, 5, 0, 0, docs);
	char sub[200] = "Saves\\";
	if (docs[0])
	{
		static const char *kIni[] = {"Fallout.ini", "FalloutPrefs.ini", "FalloutCustom.ini"};
		for (int f = 0; f < 3; f++)
		{
			char p[400]; rt::fmt(p, sizeof p, "%s\\My Games\\FalloutNV\\%s", docs, kIni[f]);
			u32 n = 0; char *t = rt::readfile(p, &n);
			if (!t) continue;
			for (const char *c = t; *c; )
			{
				while (*c == ' ' || *c == '\t') c++;
				const char *k = "SLocalSavePath";
				int i = 0; while (k[i] && (c[i] | 0x20) == (k[i] | 0x20)) i++;
				if (!k[i])
				{
					const char *e = c + i; while (*e == ' ') e++;
					if (*e == '=')
					{
						e++; while (*e == ' ') e++;
						int m = 0; while (e[m] && e[m] != '\r' && e[m] != '\n' && m < 190) { sub[m] = e[m]; m++; }
						while (m > 0 && sub[m - 1] == ' ') m--;
						if (m && sub[m - 1] != '\\') sub[m++] = '\\';
						sub[m] = 0;
						if (!m) rt::strcpy(sub, "Saves\\", sizeof sub);
					}
				}
				while (*c && *c != '\n') c++;
				if (*c) c++;
			}
			rt::free(t);
		}
		rt::fmt(s_saves, sizeof s_saves, "%s\\My Games\\FalloutNV\\%s", docs, sub);
	}
	LOG("coop: saves are in %s", s_saves[0] ? s_saves : "(not found)");
	return s_saves;
}
static int hostPhase; static u8 hostPeer; static double hostAt; static u64 hostWas;
static char hostWho[24];
static u64 fileStamp(const char *path)
{
	struct { u32 attr, c[2], a[2], w[2], hi, lo; } d;
	typedef int(__stdcall * GetAttrEx)(const char *, int, void *);
	static GetAttrEx f;
	if (!f) f = (GetAttrEx)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetFileAttributesExA");
	if (!f || !f(path, 0, &d)) return 0;
	return ((u64)d.w[1] << 32 | d.w[0]) ^ ((u64)d.lo << 7);
}
enum { NO_BUSY = 1, NO_GATE, NO_MODS, NO_SAVE, NO_SEND };
static void refuse(u8 peer, u8 why) { u8 d[2] = {peer, why}; net::addEvent(net::EV_SESSIONNO, d, 2); }
void sessionRefused(const void *data, int len)
{
	if (len < 2 || state != WAIT_SESSION) return;
	const u8 *d = (const u8 *)data;
	if (d[0] != net::myId()) return;
	static const char *kWhy[] = {"", "it's already sending its world to someone - /join again in a minute", "(no longer used)",
		"your mods differ", "it couldn't save (in combat or a menu?) - /join again", "sending it failed - /join again"};
	char m[160]; rt::fmt(m, sizeof m, "Co-op: the host's game said no: %s.", d[1] <= NO_SEND ? kWhy[d[1]] : "?");
	message(m, 6);
	LOG("coop: the host refused (%d)", d[1]);
	setState(IDLE);
}
void hostSession(u8 peer, const char *who)
{
	char m[160];
	if (hostPhase) { LOG("coop: %s asked for your world - already sending one", who); refuse(peer, NO_BUSY); return; }

	int mm = ghosts::modsMatch(peer);
	if (mm != 1)
	{
		rt::fmt(m, sizeof m, "%s can't join your world: %s.", who, mm == 2 ? "your mods differ (see the message before)" : "their mod list hasn't come (an older plugin?)");
		message(m, 6);
		LOG("coop: %s asked for your world - refused (mods %s)", who, mm == 2 ? "differ" : "not heard");
		refuse(peer, NO_MODS);
		return;
	}
	char p[400];
	rt::fmt(p, sizeof p, "%sMMW-Session.fos", savesDir());
	hostWas = fileStamp(p);
	cmd("save MMW-Session");
	hostPhase = 1; hostPeer = peer; hostAt = rt::now();
	rt::strcpy(hostWho, who, sizeof hostWho);
	rt::fmt(m, sizeof m, "%s is joining your world - saving it for them...", who);
	message(m, 4);
	LOG("coop: %s asked for your world - saving MMW-Session", who);
}
static void hostTick(double t)
{
	if (!hostPhase) return;
	char p[400], m[160];
	if (hostPhase == 1)
	{
		rt::fmt(p, sizeof p, "%sMMW-Session.fos", savesDir());
		u64 st = fileStamp(p);
		static u64 seen; static double seenAt;
		if (st && st != hostWas && t - hostAt > 2)
		{
			if (st != seen) { seen = st; seenAt = t; }
			else if (t - seenAt >= 0.5)
			{
				seen = 0;
				if (net::sendFile(hostPeer, p, "MMW-Session.fos")) { hostPhase = 2; hostAt = t; LOG("coop: MMW-Session saved - sending it to %s", hostWho); }
				else { hostPhase = 0; message("Couldn't start sending your world.", 5); refuse(hostPeer, NO_SEND); }
			}
		}
		else if (t - hostAt > 15) { hostPhase = 0; rt::fmt(m, sizeof m, "Couldn't save your world for %s (in combat or a menu?) - they can /join again.", hostWho); message(m, 6); LOG("coop: MMW-Session wasn't saved"); refuse(hostPeer, NO_SAVE); }
		return;
	}
	u32 have = 0, total = 0;
	if (net::sending(&have, &total))
	{
		static double told;
		if (t - told > 5) { told = t; rt::fmt(m, sizeof m, "Sending your world to %s: %u%%", hostWho, total ? have * 100 / total : 0); message(m, 2); }
		return;
	}
	if (net::sendResult() != 1) { hostPhase = 0; rt::fmt(m, sizeof m, "Sending your world to %s failed - they can /join again.", hostWho); message(m, 6); refuse(hostPeer, NO_SEND); return; }
	if (hostPhase == 2)
	{
		rt::fmt(p, sizeof p, "%sMMW-Session.nvse", savesDir());
		if (net::sendFile(hostPeer, p, "MMW-Session.nvse")) { hostPhase = 3; return; }
		hostPhase = 0; message("Couldn't send the rest of your world.", 5); refuse(hostPeer, NO_SEND); return;
	}
	hostPhase = 0;
	sessionPeers |= (u8)(1 << hostPeer);
	rt::fmt(m, sizeof m, "%s has your world and is coming in.", hostWho);
	message(m, 4);
	LOG("coop: your world went to %s", hostWho);
	sessionFilesOff("sent");
}
static bool gotFos, gotNvse;
static double askAt;
static void guestTick(double t)
{
	if (state != WAIT_SESSION) return;
	char nm[48];
	if (netSession)
	{
		u32 h = 0, tot = 0;
		if (net::receiving(nm, sizeof nm, &h, &tot)) askAt = t;
		else if (t - askAt > 120) { message("Co-op: no answer from the host - you're in your own game (/join again).", 6); LOG("coop: no session came in 2 minutes - idle"); setState(IDLE); return; }
	}
	u8 st = net::lastFile(nm, sizeof nm);
	if (!st) return;
	askAt = t;
	net::clearLastFile();
	if (st != 1) { message("Your friend's world didn't come through - /join again.", 6); setState(IDLE); gotFos = gotNvse = false; return; }
	if (has(nm, ".fos")) gotFos = true;
	if (has(nm, ".nvse")) gotNvse = true;
	if (gotFos && gotNvse)
	{
		gotFos = gotNvse = false;
		LOG("coop: the host's world is here - loading MMW-Session");
		message("Your friend's world is here - loading it...", 3);
		cmd("load MMW-Session");
	}
}
bool chatCommand(const char *line)
{
	if (rt::startswith(line, "/join"))
	{
		if (!net::active() || net::hosting() || net::soloTesting()) { message("/join is for joining a friend's game (start joining in setup first).", 5); return true; }
		join();
		if (state == WAIT_SESSION)
		{
			gotFos = gotNvse = false; net::clearLastFile();
			net::setFileWanted(true); askAt = rt::now();
			u8 one = 1; net::addEvent(net::EV_SESSIONASK, &one, 1); netSession = true;
		}
		return true;
	}
	if (rt::startswith(line, "/leave")) { leave(); return true; }
	return false;
}
static int iniValue(const char *key, int dflt)
{
	int v = dflt;
	static const char *kFile[] = {"Fallout.ini", "FalloutPrefs.ini"};
	char dir[300]; rt::strcpy(dir, savesDir(), sizeof dir);
	int L = (int)strlen(dir);
	if (L > 6) dir[L - 6] = 0;
	for (int f = 0; f < 2; f++)
	{
		char p[400]; rt::fmt(p, sizeof p, "%s%s", dir, kFile[f]);
		u32 n = 0; char *t = rt::readfile(p, &n);
		if (!t) continue;
		int kl = (int)strlen(key);
		for (const char *s = t; *s; )
		{
			while (*s == ' ' || *s == '\t') s++;
			bool match = true;
			for (int i = 0; i < kl && match; i++) match = (s[i] | 0x20) == (key[i] | 0x20);
			if (match)
			{
				const char *e = s + kl;
				while (*e == ' ') e++;
				if (*e == '=') { e++; while (*e == ' ') e++; v = rt::parsei(e); }
			}
			while (*s && *s != '\n') s++;
			if (*s) s++;
		}
		rt::free(t);
	}
	return v;
}
static void autosaves(bool on)
{
	static bool off; static int was[4];
	static const char *kKey[4] = {"bSaveOnTravel", "bSaveOnWait", "bSaveOnRest", "bSaveOnInteriorExteriorSwitch"};
	if (on != off) return;
	char line[120];
	for (int i = 0; i < 4; i++)
	{
		if (!on) was[i] = iniValue(kKey[i], 1);
		rt::fmt(line, sizeof line, "SetNumericINISetting \"%s:GamePlay\" %d", kKey[i], on ? was[i] : 0);
		cmd(line);
	}
	cmd(on ? "EnableKey 63" : "DisableKey 63");
	off = !on;
	LOG("coop: the game's autosaves and quicksave %s", on ? "back as they were" : "off while you're in the session");
}
static void doCommand()
{
	char p[400];
	path(p, sizeof p, "command.txt");
	u32 n = 0;
	char *t = rt::readfile(p, &n);
	if (!t) return;
	DeleteFileA(p);
	LOG("coop: command \"%s\"", t);
	if (rt::startswith(t, "join")) join();
	else if (rt::startswith(t, "leave")) leave();
	else if (rt::startswith(t, "export")) { static Sheet S; capture(S); save(S, "export.txt"); LOG("coop: your character written to coop\\export.txt"); }
	rt::free(t);
}
static double loadedAt;
static double renameAt; static char renameName[200];
void preload(const char *path)
{
	rt::strcpy(loadedSave, path ? path : "", sizeof loadedSave);
	bool ours = has(loadedSave, "MMW-Session") || has(loadedSave, "MMW-Home");
	if (live() && !has(loadedSave, "MMW-Home")) { checkpoint("a load while in your friend's world - what you earned kept"); away = true; }
	if (net::hosting() && sessionPeers && !ours)
	{
		u8 d = 1;
		net::addEvent(net::EV_HOSTWORLD, &d, 1);
		LOG("coop: you loaded another save while hosting - your friends go home with what they earned and come into this world");
		sessionPeers = 0;
	}
}
void hostWorldChanged(const char *who)
{
	if (state != IN_SESSION || !netSession) return;
	char m[120]; rt::fmt(m, sizeof m, "Co-op: %s loaded another save - going home with what you earned, then into their world again.", who);
	message(m, 6);
	LOG("coop: the host loaded another save - going home, then joining again");
	leave();
}
void leavingWorld(bool newGame)
{
	if (live()) { checkpoint(newGame ? "a new game while in your friend's world - what you earned kept" : "the main menu while in your friend's world - what you earned kept"); away = true; }
}
void saved(const char *name)
{
	if (state != IN_SESSION || !name || !*name) return;
	const char *b = name;
	for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
	if (has(b, "MMW-") || has(b, "autosave") || has(b, "quicksave")) return;
	rt::strcpy(renameName, b, sizeof renameName);
	int n = (int)strlen(renameName);
	if (n > 4 && has(renameName + n - 4, ".fos")) renameName[n - 4] = 0;
	renameAt = rt::now() + 3;
	LOG("coop: a save made in your friend's world (%s) - renamed in 3 s", renameName);
}
static void renameTick()
{
	if (!renameAt || rt::now() < renameAt) return;
	renameAt = 0;
	static const char *ext[2] = {".fos", ".nvse"};
	int moved = 0;
	for (int i = 0; i < 2; i++)
	{
		char a[400], c[460];
		rt::fmt(a, sizeof a, "%s%s%s", savesDir(), renameName, ext[i]);
		rt::fmt(c, sizeof c, "%sMMW-Friend world - %s%s", savesDir(), renameName, ext[i]);
		if (MoveFileA(a, c)) moved++;
	}
	LOG("coop: \"%s\" renamed \"MMW-Friend world - %s\" (%d files) - your own saves stay yours", renameName, renameName, moved);
	if (moved) message("Co-op: that save is your friend's world - it's kept as \"MMW-Friend world - ...\".", 5);
}
void loaded()
{
	char p[400];
	rt::fmt(p, sizeof p, "%sMojaveMostWanted\\coop", g_dir);
	CreateDirectoryA(p, 0);
	readState();
	pending = 0;
	if (state == IN_SESSION) { away = true; nextCheckpoint = 0; }
	if (has(loadedSave, "MMW-Session") && state == WAIT_SESSION) pending = 1;
	else if (has(loadedSave, "MMW-Session") && state == IN_SESSION) pending = 6;
	else if (has(loadedSave, "MMW-Home") && (state == GOING_HOME || state == IN_SESSION)) pending = state == IN_SESSION ? 3 : 2;
	else if (state == IN_SESSION) LOG("coop: a session isn't over (%s loaded) - your gains wait for MMW-Home", loadedSave);
	pendingAt = rt::now() + 4;
	loadedAt = rt::now();
	if (state != IDLE) LOG("coop: %s loaded, %s%s", loadedSave, kStateName[state], pending ? " - acting in 4 s" : "");
}
void tick(double t)
{
	renameTick();
	if (pending && t >= pendingAt)
	{
		int what = pending;
		pending = 0;
		if (what == 1) doSwap();
		else if (what == 4) swapDone();
		else if (what == 5) homeFinish();
		else if (what == 6)
		{
			LOG("coop: MMW-Session loaded again - an old copy of the host's world: going home, then a fresh copy");
			message("Co-op: that was an old copy of your friend's world - going home with what you earned, then into their world as it is now.", 7);
			leave();
		}
		else goHome(what == 3);
	}
	if (live() && t >= nextCheckpoint && nextCheckpoint) { nextCheckpoint = t + 180; checkpoint("every 3 minutes"); }
	if (t >= nextCmd) { nextCmd = t + 1; doCommand(); autosaves(!(live() || (state == IN_SESSION && pending == 6))); }
	static bool dirSet;
	if (!dirSet) { dirSet = true; net::setFileDir(savesDir()); }
	hostTick(t);
	guestTick(t);
	{
		static u32 loadsSeen; static double lastTick, menuAt, resyncAt = -1000;
		if (loadsSeen != g_loads) { loadsSeen = g_loads; s_nLocal = 0; s_outOfSync = false; s_oosWhat[0] = 0; }
		if (t - lastTick > 0.5) menuAt = t;
		lastTick = t;
		for (int i = 0; i < s_nLocal;)
		{
			if (t - s_local[i].t < 10) { i++; continue; }
			if (!s_local[i].seen) { s_outOfSync = true; LOG("coop: your copy of the host's world did a step theirs didn't (%s) - it's gone its own way", s_oosWhat); }
			s_local[i] = s_local[--s_nLocal];
		}
		bool guestIn = !net::hosting() && state == IN_SESSION && netSession;
		if (!guestIn) s_outOfSync = false;
		if (s_outOfSync && t - menuAt > 3 && !g_tv.on && !ghosts::down.on && t - resyncAt > 120)
		{
			resyncAt = t; s_outOfSync = false;
			message("Co-op: your copy of the quest went its own way - a quick trip home, then straight back into your friend's world as it is.", 7);
			LOG("coop: out of step with the host's world (%s) - going home with what you earned, then joining again", s_oosWhat);
			s_oosWhat[0] = 0;
			leave();
		}
	}
	{
		static double heardAt, nextTry; static bool finishing, toldBuild, toldMods;
		bool guest = net::active() && !net::hosting() && !net::soloTesting();
		if (!guest) { heardAt = 0; nextTry = 0; finishing = false; toldBuild = toldMods = false; }
		else if (TUNE(coopAutoJoin) > 0 && !live())
		{
			net::Ghost g[4]; int n = net::ghosts(g, 4); bool hostHere = false;
			for (int i = 0; i < n; i++) if (g[i].id == 0 && g[i].since < 5) hostHere = true;
			if (hostHere && !heardAt) heardAt = t;
			bool ready = heardAt && t - heardAt > 5 && !menumode() && !pending && !has(loadedSave, "MMW-Session");
			if (ready && !finishing && (state == IN_SESSION || state == GOING_HOME))
			{
				finishing = true;
				LOG("coop: auto-join - your last session together isn't finished (%s): home with its gains first", kStateName[state]);
				message("Co-op: your last session together wasn't finished - taking what you earned home first, then joining.", 6);
				if (state == IN_SESSION) leave(); else cmd("load MMW-Home");
			}
			else if (ready && state == WAIT_SESSION && !netSession) setState(IDLE);
			if (ready && state == IDLE && t >= nextTry)
			{
				u32 hb = net::peerBuild(0);
				int mm = ghosts::modsMatch(0);
				if (hb && hb != kPluginBuild)
				{
					if (!toldBuild) { toldBuild = true; char m[160]; rt::fmt(m, sizeof m, "Co-op: your friend is on Mojave Enhanced plugin %u, you're on %u - update in setup (both) to play in their world.", hb, kPluginBuild); message(m, 8); LOG("coop: auto-join - the host's plugin %u, ours %u: not joining", hb, kPluginBuild); }
				}
				else if (mm == 2)
				{
					if (!toldMods) { toldMods = true; LOG("coop: auto-join - mods differ, staying in your own world"); message("Co-op: your mods differ from your friend's, so you play in your own world side by side (their world needs the same mods).", 8); }
				}
				else if (hb && mm == 1)
				{
					finishing = false; toldBuild = toldMods = false; nextTry = t + 30;
					LOG("coop: auto-join - the same plugin (%u) and mods: into the host's world", hb);
					chatCommand("/join");
				}
			}
		}
	}
	static double hostGoneAt;
	if (live() && netSession && !net::hosting() && t - loadedAt > 20)
	{
		net::Ghost g[4];
		int n = net::ghosts(g, 4);
		bool here = false;
		for (int i = 0; i < n; i++) if (g[i].id == 0 && g[i].since < 5) here = true;
		if (here) { if (hostGoneAt) { hostGoneAt = 0; message("Your friend is back.", 3); LOG("coop: the host is back"); } }
		else
		{
			if (!hostGoneAt) { hostGoneAt = t; message("Your friend's game is gone - going home in 60 s.", 5); LOG("coop: the host is gone - home in 60 s"); }
			static int lastSaid;
			int left = 60 - (int)(t - hostGoneAt);
			if ((left == 30 || left == 10) && left != lastSaid) { lastSaid = left; char m[80]; rt::fmt(m, sizeof m, "Going home in %d s.", left); message(m, 3); }
			if (left <= 0) { hostGoneAt = 0; leave(); }
		}
	}
	else hostGoneAt = 0;
}
}

// Frame profiler: each part of the tick is timed and every 10 s the log gets the game's frame rate and what each part
// cost, so cuts follow measurements
namespace prof
{
enum { HANDLING, MOVE, COOP, SNOW, DRIVE, FOOT, HITS, MEDIA, TOTAL, N };
static const char *kName[N] = {"handling", "moves", "playing together", "snow", "driving", "on foot", "run-overs", "music/screen/paint", "plugin total"};
static double acc[N], mx[N], frameAcc, frameMax, since;
static int frames;
static void add(int i, double s) { acc[i] += s; if (s > mx[i]) mx[i] = s; }
static void frame(float dt)
{
	frames++; frameAcc += dt; if (dt > frameMax) frameMax = dt;
	since += dt;
	if (since < 10 || frames < 1) return;
	char b[480]; int at = rt::fmt(b, sizeof b, "perf: %.1f fps (worst frame %.1f ms); per frame, average (worst) ms:", frames / frameAcc, frameMax * 1000);
	for (int i = 0; i < N; i++) at += rt::fmt(b + at, sizeof b - at, " %s %.3f (%.2f)%s", kName[i], acc[i] * 1000 / frames, mx[i] * 1000, i + 1 < N ? "," : "");
	LOG("%s", b);
	if (acc[COOP] * 1000 / frames > 2 || mx[COOP] > 0.05)
	{
		at = rt::fmt(b, sizeof b, "perf: playing together, per frame average (worst) ms:");
		for (int i = 0; i < gprof::N; i++) if (gprof::acc[i] * 1000 / frames > 0.05 || gprof::mx[i] > 0.005) at += rt::fmt(b + at, sizeof b - at, " %s %.2f (%.1f)", gprof::kName[i], gprof::acc[i] * 1000 / frames, gprof::mx[i] * 1000);
		LOG("%s", b);
	}
	for (int i = 0; i < gprof::N; i++) gprof::acc[i] = gprof::mx[i] = 0;
	for (int i = 0; i < N; i++) acc[i] = mx[i] = 0;
	frames = 0; frameAcc = frameMax = since = 0;
}
}
#define PROF(i, ...) do { double p0_ = rt::now(); __VA_ARGS__; prof::add(prof::i, rt::now() - p0_); } while (0)
// A tick of ours over 50 ms says where its time went, part by part (the first 40)
static double s_lapT0, s_lapAt[24]; static const char *s_lapN[24]; static int s_laps, s_slowLogs;
#define LAP(nm) do { if (s_laps < 24) { s_lapN[s_laps] = nm; s_lapAt[s_laps++] = rt::now(); } } while (0)
static void lapReport(const char *end)
{
	double now = rt::now(), tot = now - s_lapT0;
	static u32 slowLoad; if (slowLoad != g_loads) { slowLoad = g_loads; s_slowLogs = 0; }
	if (tot > 0.05 && s_slowLogs < 20)
	{
		s_slowLogs++;
		char b[700]; int n = rt::fmt(b, sizeof b, "slow tick: %.0f ms (%s) -", tot * 1000, end);
		double prev = s_lapT0;
		for (int i = 0; i < s_laps && n < 650; i++) { double d = s_lapAt[i] - prev; prev = s_lapAt[i]; if (d > 0.002) n += rt::fmt(b + n, sizeof b - n, " %s %.0f", s_lapN[i], d * 1000); }
		if (now - prev > 0.002) n += rt::fmt(b + n, sizeof b - n, " rest %.0f", (now - prev) * 1000);
		double co = 0; for (int i = 0; i < gprof::N; i++) co += gprof::cur[i];
		if (co > 0.01) { n += rt::fmt(b + n, sizeof b - n, " (playing together:"); for (int i = 0; i < gprof::N && n < 680; i++) if (gprof::cur[i] > 0.002) n += rt::fmt(b + n, sizeof b - n, " %s %.0f", gprof::kName[i], gprof::cur[i] * 1000); n += rt::fmt(b + n, sizeof b - n, ")"); }
		LOG("%s", b);
	}
	s_laps = 0;
}

// Smooth ADS on O, kept per game in smoothads.ini (mmw=, vanilla=, ask=); in vanilla stable it starts off, and the
// first load of a session asks once
namespace smoothads
{
static const int kMenu = 9;
static bool askPending, asked;
static void path(char *p, u32 cap) { rt::fmt(p, cap, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\smoothads.ini", g_dir); }
static int read(const char *key, int def)
{
	char p[320];
	path(p, sizeof p);
	char *b = rt::readfile(p, 0);
	int v = def;
	u32 n = (u32)strlen(key);
	if (b)
	{
		for (char *q = b; *q; q++)
			if ((q == b || q[-1] == '\n') && rt::startswith(q, key) && q[n] == '=') v = rt::parsei(q + n + 1);
		rt::free(b);
	}
	return v;
}
static void save(int ask)
{
	int mm = g_vanilla ? read("mmw", 1) : (g_smoothAds ? 1 : 0), va = g_vanilla ? (g_smoothAds ? 1 : 0) : read("vanilla", 0);
	char p[320], line[64];
	path(p, sizeof p);
	int n = rt::fmt(line, sizeof line, "mmw=%d\r\nvanilla=%d\r\nask=%d\r\n", mm, va, ask);
	rt::writefile(p, line, (u32)n, false);
}
static void apply()
{
	char cmd[64];
	rt::fmt(cmd, sizeof cmd, "SetSmoothIronsightsValue 1 %.3f", g_smoothAds ? rt::max(iniF("smoothAdsSec", 0.45f), 0.001f) : 0.001f);
	console(cmd);
}
static void set(bool on)
{
	g_smoothAds = on;
	save(read("ask", 1));
	apply();
	LOG("smooth ADS %s", on ? "on" : "off");
	message(on ? "Smooth ADS on: the camera eases onto the sights and the gun settles in. O turns it off."
	           : "Smooth ADS off: the game's own aiming. O turns it back on.", 4);
}
static void afterLoad()
{
	apply();
	if (g_vanilla && !g_smoothAds && !asked && read("ask", 1)) askPending = true;
}
static void tick(bool focus)
{
	bool o = pressed(VK_SMOOTHADS);
	if (o && focus && !menumode() && !g_driving) set(!g_smoothAds);
	if (askPending && focus && !g_menu)
	{
		askPending = false;
		asked = true;
		static const char *btn[] = {"Turn it on", "Keep it off", "Keep it off, don't ask again"};
		if (showMessageBox("VANILLA STABLE\n\nSmooth ADS is off: aiming down the sights is the game's own.\nTurn it on for an eased camera and the gun settling onto the sights.\n\nPress O any time to switch it.\nU switches all of our gun handling (inertia, sway, recoil, contextual aiming).", btn, 3))
			g_menu = kMenu;
	}
}
static void chosen(int button)
{
	if (button == 0) set(true);
	if (button == 2) { save(0); message("Smooth ADS stays off. O turns it on.", 4); }
}
}

static void tick()
{
	if (g_logLen && rt::now() - g_logAt > 1.0) logFlush();
	ghosts::netStopCheck();
	borderlessTick();
	TESObjectREFR *pl = player();
	if (!pl || !g_ready) return;
	double t = rt::now();
	float dt = (float)(t - g_lastTime);
	g_lastTime = t;
	if (dt > 0.5f) g_loadedAt = t;
	{
		static float lp[3]; static bool have; static u32 lcell;
		const float *pp = pos(pl);
		u32 cell = ptr((u32)pl + OFF_REF_CELL);
		bool sane = pp[0] > -4e6f && pp[0] < 4e6f && pp[1] > -4e6f && pp[1] < 4e6f && pp[2] > -1e6f && pp[2] < 1e6f;
		if (!sane && have && dt < 0.5f && cell == lcell)
		{
			LOG("you were thrown out of the world (%.0f %.0f %.0f) - put back at %.0f %.0f %.0f (ours last: %s)", pp[0], pp[1], pp[2], lp[0], lp[1], lp[2], g_crumb);
			float *w = pos(pl); w[0] = lp[0]; w[1] = lp[1]; w[2] = lp[2];
			moveRef(pl, lp[0], lp[1], lp[2], rot(pl)[0] * 180 / rt::PI, 0, rot(pl)[2] * 180 / rt::PI);
			pp = pos(pl);
		}
		float dx = pp[0] - lp[0], dy = pp[1] - lp[1], dz = pp[2] - lp[2];
		if (have && dt < 0.5f && dx * dx + dy * dy + dz * dz > 1500.0f * 1500.0f)
			LOG("you were moved %.0f units in one frame (%.0f %.0f %.0f -> %.0f %.0f %.0f, cell %x -> %x) - ours last: %s%s", rt::sqrt(dx * dx + dy * dy + dz * dz),
				lp[0], lp[1], lp[2], pp[0], pp[1], pp[2], lcell ? refid((void *)lcell) : 0, cell ? refid((void *)cell) : 0, g_crumb, g_driving ? " (driving)" : "");
		if (pp[0] > -4e6f && pp[0] < 4e6f && pp[1] > -4e6f && pp[1] < 4e6f && pp[2] > -1e6f && pp[2] < 1e6f) { memcpy(lp, pp, 12); have = true; lcell = cell; }
	}
	if (dt > 0.1f) dt = 0.1f;
	if (dt <= 0) return;
	move::slowTick();
	if (g_radioKnob < 0) radioKnobLoad();
	if (g_menu)
	{
		int b = messageBoxButton();
		if (b >= 0) { g_pendingMenu = g_menu; g_menu = 0; g_pendingChoice = b; }
		ghosts::pump();
		paintTick();
		previewTick(dt);
		return;
	}
	float *vol = gameVolumes();
	subApply();
	audio::setRadioOutside(g_driving ? 0.0f : 1.0f);
	audio::setMaster(vol[0] * vol[3], vol[0] * g_radioKnob * g_radioFall);
	hudColorTick();
	messageTick();
	static bool wasMenu;
	if (menumode())
	{
		if (!wasMenu) { wasMenu = true; move::menu(pl); audio::setEngine(false, g_rpm, 0, 0); audio::setLimiter(false, 13, 0, 0); audio::setSkid(0, 1, 0); if (g_radioOn) audio::radioPause(true); }
		if (g_worldUp) ghosts::menuTick(pl);
		move::menuPrepare(pl);
		if (g_worldUp) PROF(COOP, ghosts::tick(dt, pl, false));
		else ghosts::pump();
		return;
	}
	if (wasMenu) { wasMenu = false; if (g_radioOn) audio::radioPause(false); if (!g_fovFixAt) g_fovFixAt = rt::now() + 1.0; }
	{
		u32 life = rd<u32>((u32)pl + 0x108);
		static bool wasDead;
		if (life == 1 || life == 2)
		{
			if (!wasDead)
			{
				wasDead = true;
				LOG("you died (life state %u)%s - letting go of you", life, g_driving ? " in the car" : "");
				if (g_driving) { g_exitDead = true; exitCar(); g_exitDead = false; }
				if (ghosts::riding) ghosts::rideEnd(pl, "you died", false);
				handling::release(pl, "you died"); handling::active = false;
				move::reset(); sprint::reset();
			}
			ghosts::pump();
			return;
		}
		if (wasDead) { wasDead = false; LOG("alive again"); }
	}
	double pTick = rt::now();
	s_lapT0 = pTick; s_laps = 0;
	PROF(MEDIA, musicTick(); screenTick(t); gradioIdle(t); gaugeTick(t));
	LAP("media");
	g_worldUp = true;
	if (g_afterLoadPending) { g_afterLoadPending = false; g_fovFixAt = t + 1.0; snowfx::reset(); handling::reset(); handling::mzReset(); handling::active = false; move::reset(); sprint::reset(); ghosts::reset(); body::reset(); afterLoad(); smoothads::afterLoad(); }
	LAP("after-load");
	if (g_pendingChoice >= 0) { int b = g_pendingChoice; g_pendingChoice = -1; if (g_pendingMenu == smoothads::kMenu) smoothads::chosen(b); else garageChosen(g_pendingMenu, b); }
	paintTick();
	previewTick(dt);
	bool focus = gameFocused();
	if (g_liveEdit) { liveTick(dt, focus, t); return; }
	if (g_reopenPaintAt > 0 && t >= g_reopenPaintAt && !held(VK_PAINTKEEP) && !held(VK_USE) && !held(VK_PAINTUNDO))
	{
		g_reopenPaintAt = 0;
		if (g_paintCar >= 0) showPaintMain();
	}
	float *pp = pos(pl);
	bool inMojave = worldspace() == kGarageWorld;
	static u32 lastCell;
	u32 cell = ptr((u32)pl + OFF_REF_CELL);
	if (cell != lastCell) { lastCell = cell; g_quietUntil = t + 2.0; LOG("cell %x (Mojave %d), waiting", cell ? refid((void *)cell) : 0, inMojave); }
	fovWatch(pl);
	fovDefault(pl, t);
	LAP("paint/fov");
	if (!ghosts::riding)
	{
		PROF(HANDLING, move::unview(pl); handling::tick(dt, t, focus, pl)); LAP("handling");
		PROF(MOVE, move::tick(dt, t, focus, pl, t < g_quietUntil)); LAP("move");
		PROF(HANDLING, sprint::tick(dt, t, focus, pl); move::view(pl)); LAP("sprint");
	}
	PROF(COOP, ghosts::tick(dt, pl, t < g_quietUntil); ranges::tick(pl)); LAP("playing together");
	body::tick(dt, pl, focus, !g_driving && !ghosts::riding);
	LAP("body");
	if (t < g_quietUntil && !g_driving) { lapReport("settling after a load"); return; }
	smoothads::tick(focus);
	feel::hook();
	feel::rehook();
	feel::tick(dt, focus);
	if (focus && pressed(VK_RTX)) rtx::toggle();
	LAP("feel");
	lootcheck::tick(t);
	LAP("loot");
	questlog::tick(t);
	LAP("quests");
	coop::tick(t);
	LAP("coop");
	if (!g_vanilla)
	{
		nights::once();
		if (focus && pressed(VK_LIGHTING)) nights::toggle();
	}
	if (!g_vanilla || iniI("weather", 0)) PROF(SNOW, season::tick(focus); snowfx::tick(dt, t, focus, pl));
	LAP("seasons");
	if (ghosts::riding) { lapReport("riding"); return; }
	double pBlock = rt::now();
	bool pDriving = g_driving;

	if (g_driving)
	{
		if (!inMojave) { exitCar(); return; }
		float topMs = rt::clamp(g_cars[g_activeCar].topSpeed * iniF("topSpeedScale", 0.25f), 20, iniF("maxSpeedMs", 55));
		float accel = rt::clamp(topMs / 7.0f, 4.5f, 9.0f);
		bool w = focus && held(VK_ACCELERATE), s = focus && held(VK_BRAKE);
		bool a = focus && held(VK_STEERLEFT), d = focus && held(VK_STEERRIGHT), hb = focus && held(VK_HANDBRAKE);
		leanTick(focus && held(VK_LEAN), dt);
		float &v = g_car.speed;
		bool burn = w && hb && !s && (g_burning || rt::absf(v) < TUNE(burnoutMaxSpeedMs));
		if (!burn && g_burning)
		{
			if (w && !hb) { g_launchT = TUNE(launchSeconds); LOG("burnout: launch"); }
			else g_launchT = 0;
		}
		if (burn && !g_burning) LOG("burnout");
		bool fKey = pressed(VK_SMOKESTYLE);
		if (burn && focus && fKey && iniI("smoke", 0) > 0)
		{
			if (g_smokeVar < 0) smokeStyleLoad();
			for (int k = 0; k < kFx_tiresmoke_variants; k++)
			{
				g_smokeVar = (g_smokeVar + 1) % kFx_tiresmoke_variants;
				if (smokeHave(g_smokeVar)) break;
			}
			smokeStyleSave();
			smokeRemove();
			char m[64];
			rt::fmt(m, sizeof m, "Tyre smoke style %d of %d", g_smokeVar + 1, iniI("smoke", 0));
			message(m, 1.5f);
		}
		g_burning = burn;
		float boost = g_launchT > 0 ? 1 + (TUNE(launchBoost) - 1) * g_launchT / TUNE(launchSeconds) : 1;
		if (g_launchT > 0) g_launchT -= dt;
		if (burn) v = 0;
		else if (w) v += accel * boost * (1 - rt::clamp(v / topMs, 0, 1) * 0.6f) * dt;
		if (s) v -= (v > 0.5f ? 14.0f : 4.0f) * dt;
		if (hb && !burn) v -= (v > 0 ? 1 : -1) * rt::clamp(rt::absf(v), 0, (rt::absf(v) > TUNE(driftMinSpeedMs) ? TUNE(driftHandbrakeDecel) : 16.0f) * dt);
		if (!w && !s) v -= (v > 0 ? 1 : -1) * rt::clamp(rt::absf(v), 0, (1.2f + rt::absf(v) * 0.03f) * dt);
		v = rt::clamp(v, -8, topMs);
		float steer = ((d ? 1.0f : 0.0f) - (a ? 1.0f : 0.0f)) * (1 - TUNE(leanSteerCut) * leanEase());
		float grip = rt::clamp(rt::absf(v) / 6.0f, 0, 1) * (1 - 0.45f * rt::clamp(rt::absf(v) / 45.0f, 0, 1));
		{
			float &yawRate = g_yawRate;
			float slip = wrap180(g_car.heading - g_car.course);
			float aslip = rt::absf(slip), sg = slip >= 0 ? 1.0f : -1.0f;
			bool fast = v > TUNE(driftMinSpeedMs), hbFast = hb && fast;
			if (hbFast || (fast && aslip > TUNE(driftSlideDegrees))) g_sliding = true;
			float gripWant = 1;
			if (hbFast) gripWant = TUNE(driftHandbrakeGrip);
			else if (g_sliding)
			{
				if (w && fast) gripWant = TUNE(driftThrottleGrip);
				else if (aslip > TUNE(driftPeakDegrees)) gripWant = TUNE(driftSlideGrip);
			}
			if (aslip > TUNE(driftSpinDegrees)) gripWant = rt::min(gripWant, 0.12f);
			if (gripWant < g_rearGrip) g_rearGrip += (gripWant - g_rearGrip) * rt::clamp(dt * 12, 0, 1);
			else g_rearGrip = rt::min(gripWant, g_rearGrip + dt * TUNE(driftGripReturn));
			if (g_sliding && aslip < 3 && g_rearGrip > 0.9f && !hbFast) g_sliding = false;
			if (v < 0.5f || burn) { g_sliding = false; g_rearGrip = 1; }
			float want = steer * kCars[g_activeCar].handling * grip * (v >= 0 ? 1 : -1) * (hbFast ? 1.25f : 1.0f);
			if (g_sliding && v > 1)
			{
				float swing = TUNE(driftOversteer) * (1 - g_rearGrip) * rt::clamp((aslip + 6) / 30.0f, 0, 1.5f);
				float straighten = TUNE(driftStraighten) * g_rearGrip * rt::clamp(aslip / 30.0f, 0, 1.2f);
				want += sg * (swing - straighten);
			}
			yawRate += (want - yawRate) * rt::clamp(dt * TUNE(steerResponse) * (g_sliding ? 0.8f : 1.0f), 0, 1);
			if (rt::absf(v) < 0.5f) yawRate = 0;
			g_car.heading += yawRate * dt;
			if (v < 0.5f || burn) g_car.course = g_car.heading;
			else if (!g_sliding) g_car.course += wrap180(g_car.heading - g_car.course) * rt::clamp(dt * TUNE(gripRateNormal), 0, 1);
			else
			{
				float turn = wrap180(g_car.heading - g_car.course) * TUNE(driftGripRate) * rt::max(g_rearGrip, 0.3f);
				float cap = TUNE(driftSlideG) * 9.81f * (0.45f + 0.55f * g_rearGrip) / rt::max(v, 3.0f) * 180.0f / rt::PI;
				g_car.course += rt::clamp(turn, -cap, cap) * dt;
			}
			{
				float a2 = rt::absf(wrap180(g_car.heading - g_car.course));
				float side = rt::clamp((a2 - 45) / 45.0f, 0, 0.85f);
				if (v > 0.5f) v -= v * TUNE(driftScrub) * rt::sin(a2 * rt::PI / 180.0f) * (1 + 2 * side) * dt;
			}
			while (g_car.course >= 360) g_car.course -= 360;
			while (g_car.course < 0) g_car.course += 360;
			g_driftSlip = wrap180(g_car.heading - g_car.course);
		}
		if (burn && steer != 0)
		{
			float hb0 = g_car.halfBase, h0 = g_car.heading * rt::PI / 180.0f;
			float fx = g_car.x + rt::sin(h0) * hb0, fy = g_car.y + rt::cos(h0) * hb0;
			g_car.heading += steer * TUNE(burnoutPivotDegPerSec) * dt;
			float h1 = g_car.heading * rt::PI / 180.0f;
			g_car.x = fx - rt::sin(h1) * hb0; g_car.y = fy - rt::cos(h1) * hb0;
		}
		{
			float want = burn ? TUNE(burnoutWheelMs) : g_launchT > 0 ? TUNE(burnoutWheelMs) * 0.6f * g_launchT / TUNE(launchSeconds) : 0;
			g_car.slipMs += (want - g_car.slipMs) * rt::clamp(dt * (burn ? 3.0f : 5.0f), 0, 1);
			float slip = rt::clamp(g_car.slipMs / TUNE(burnoutWheelMs), 0, 1);
			float drift = rt::clamp((rt::absf(g_driftSlip) - 6) / 30.0f, 0, 1) * rt::clamp(v / 8.0f, 0, 1);
			if (drift > 0.45f && v > 6) g_smokeT = rt::max(g_smokeT, 0.35f);
			float d = slip + drift > 0.001f ? drift / (slip + drift) : 0;
			auto mix = [d](float a, float b) { return a + (b - a) * d; };
			audio::setSkidLayers(mix(TUNE(skidLayerBody), TUNE(driftLayerBody)), mix(TUNE(skidLayerMid), TUNE(driftLayerMid)), mix(TUNE(skidLayerHigh), TUNE(driftLayerHigh)));
			audio::setSkid(rt::max(slip, drift), mix(TUNE(skidPitch) * (0.92f + 0.12f * slip), TUNE(driftSkidPitch) * (0.9f + 0.15f * drift)), mix(TUNE(skidVolume), TUNE(driftSkidVolume)));
			g_revsOverride = burn ? TUNE(burnoutRevs) + 0.035f * rt::sin((float)t * 2 * rt::PI * 6.5f) : -1;
			if (burn) g_smokeT = TUNE(smokeLingerSeconds) + (g_launchT > 0 ? g_launchT : 0);
			else if (g_smokeT > 0) g_smokeT -= dt;
			if (g_launchT > 0 && g_smokeT < g_launchT) g_smokeT = g_launchT;
			bool lifts = g_activeCar >= 0 && (rt::streq(kClasses[kCars[g_activeCar].cls].id, "muscle") || rt::streq(kClasses[kCars[g_activeCar].cls].id, "classic"));
			float wt = 0;
			if (v < 0.8f && !w) g_standArmed = true;
			if (w && g_standArmed && v < 1.5f && !burn) { g_standLaunchT = TUNE(wheelieStartSeconds); g_standArmed = false; }
			if (g_standLaunchT > 0) g_standLaunchT -= dt;
			if (!w) g_standLaunchT = 0;
			if (lifts && w && !burn)
			{
				if (g_launchT > 0) wt = TUNE(wheelieLaunchDegrees) * rt::clamp(g_launchT / TUNE(launchSeconds) * TUNE(wheelieHold), 0, 1);
				else if (g_standLaunchT > 0) wt = TUNE(wheelieStartDegrees) * rt::clamp(g_standLaunchT / TUNE(wheelieStartSeconds) * 1.5f, 0, 1);
			}
			if (wt > g_wheelieAim) g_wheelieAim = rt::min(wt, g_wheelieAim + TUNE(wheelieRiseDegPerSec) * dt);
			else g_wheelieAim = wt;
			spring(g_car.wheelie, g_car.wheelieVel, g_wheelieAim, TUNE(wheelieHz), g_car.wheelieVel < 0 ? TUNE(wheelieDropDamping) : TUNE(wheelieDamping), dt);
			if (g_car.wheelie < 0) { g_car.wheelie = 0; if (g_car.wheelieVel < 0) g_car.wheelieVel = -g_car.wheelieVel * TUNE(wheelieBounce); }
			if (g_wheelieAim <= 0 && g_car.wheelie < 0.05f && rt::absf(g_car.wheelieVel) < 0.5f) g_car.wheelie = g_car.wheelieVel = 0;
		}
		if (g_car.heading >= 360) g_car.heading -= 360;
		if (g_car.heading < 0) g_car.heading += 360;
		{
			float want = steer * TUNE(maxSteerDegrees) * (1 - 0.6f * rt::clamp(rt::absf(v) / 45.0f, 0, 1));
			g_car.steer += (want - g_car.steer) * rt::clamp(dt * 10, 0, 1);
		}
		advanceCar(dt, pl);
		settle(g_car, dt, false);
		{
			static double rideLogT;
			if (rt::absf(v) > 1 && t - rideLogT > 0.5)
			{
				rideLogT = t;
				float gc = groundAt(g_car.x, g_car.y, g_car.z), tc;
				bool ht = terrainHeight(g_car.x, g_car.y, &tc);
				LOG("ride: %.1f m/s acc %.1f | body %.0f ride %.0f floor %.0f over ground (landscape %.0f) | vz %.0f pitch %.1f roll %.1f | wheels %.0f %.0f %.0f %.0f",
					v, g_car.accSm, g_car.z - gc, g_car.dbgRide - gc, g_car.dbgFloor - gc, ht ? tc - gc : 0.0f, g_car.vz, g_car.pitch, g_car.roll,
					g_car.wheelOff[0], g_car.wheelOff[1], g_car.wheelOff[2], g_car.wheelOff[3]);
			}
		}
		respawnIfFar(g_car);
		audio::setExternalBass(g_musicOn, g_musicOn ? music::level() : 0);
		bassShake(g_car, g_radioOn || g_musicOn, dt);
		pushVehicle(g_car);
		turnWheels(g_car, dt);
		if (g_smokeT > 0) smokeFollow(g_car);
		else if (g_smokeRef[0] || g_smokeRef[1]) smokeRemove();
		{
			bool lk = pressed(VK_HEADLIGHTS), hk = pressed(VK_HIGHBEAMS);
			static const char *headNames[3] = {"Headlights off (L: on)", "Low beams (H: high beams, L: off)", "High beams (H: low beams, L: off)"};
			if (focus && (lk || hk))
			{
				if (lk) g_headMode = g_headMode ? 0 : g_headOnMode;
				else g_headMode = g_headMode == 2 ? 1 : 2;
				if (g_headMode) g_headOnMode = g_headMode;
				click(radio::CLICK_BUTTON);
				message(headNames[g_headMode], 1.5f);
				radioKnobSave();
			}
		}
		carLights(s && v > 0.5f, v < -0.5f);
		if (focus && pressed(VK_CAMERA))
		{
			static const char *names[3] = {"Chase camera", "Hood camera", "Driver seat camera"};
			g_camMode = (g_camMode + 1) % 3;
			message(names[g_camMode], 1.5f);
		}
		placeRider(pl, dt);
		updateEngine(dt, v, topMs, w);
		shiftLights(g_car, true);
		ridersTick(dt);
		{
			bool srcK = pressed(VK_MUSICSOURCE), nextK = pressed(VK_NEXTTRACK), prevK = pressed(VK_PREVTRACK);
			if (focus && srcK)
			{
				click(radio::CLICK_SWITCH);
				g_musicMode = nextSource(g_musicMode);
				radioKnobSave();
				LOG("radio source: %s", sourceName(g_musicMode));
				g_screenDark = false;
				if (g_musicMode == 1) musicOn(true);
				else
				{
					musicOff(true);
					if (g_musicMode == 0) radioNext();
					else
					{
						radioOff();
						char m[160];
						rt::fmt(m, sizeof m, "Radio: the game's stations - tune them on your Pip-Boy (M: %s)", sourceName(nextSource(2)));
						message(m, 4);
					}
				}
			}
			bool scrDown = pressed(VK_SCREEN), jHeld = held(VK_SCREEN);
			bool tl = pressed(VK_SCREENTURNLEFT), tr = pressed(VK_SCREENTURNRIGHT), su = pressed(VK_SCREENSLANTUP), sd = pressed(VK_SCREENSLANTDOWN);
			static bool jDown, jTuned;
			if (scrDown) { jDown = true; jTuned = false; }
			if (jDown && jHeld && focus && g_screenKind && (tl || tr || su || sd))
			{
				if (g_scrCar != g_activeCar) screenTurnLoad(g_activeCar);
				g_scrTurn = rt::clamp(g_scrTurn + (tl ? 0.5f : 0) - (tr ? 0.5f : 0), -45, 45);
				g_scrSlant = rt::clamp(g_scrSlant + (su ? 0.5f : 0) - (sd ? 0.5f : 0), -45, 45);
				jTuned = true;
				screenTurnSave();
				g_screenDark = false;
				char m[160];
				rt::fmt(m, sizeof m, "Screen picture: turned %.1f, slanted %.1f (hold J: left / right turn it, up / down slant it)", g_scrTurn, g_scrSlant);
				message(m, 2.5f);
				LOG("screen picture for %s: turned %.1f, slanted %.1f", g_activeCar >= 0 ? kCars[g_activeCar].id : "?", g_scrTurn, g_scrSlant);
			}
			bool scrK = false;
			if (jDown && !jHeld) { jDown = false; scrK = !jTuned; }
			if (focus && scrK)
			{
				click(radio::CLICK_BUTTON);
				g_screenDark = false;
				if (g_screenKind == 2)
				{
					g_screenMode = g_screenMode ? 0 : 1;
					radioKnobSave();
					message(g_screenMode ? "Radio display: equalizer (J: off)" : "Radio display: off (J: equalizer)", 1.5f);
				}
				else
				{
					g_screenMode = g_screenMode == 1 ? 2 : g_screenMode == 2 ? 0 : 1;
					radioKnobSave();
					static const char *kModes[3] = {"Screen: off (J: equalizer)", "Screen: equalizer (J: video)", "Screen: video from your browser (J: off)"};
					message(kModes[g_screenMode], 1.5f);
				}
			}
			if (focus && (nextK || prevK) && g_musicMode == 2 && !g_musicOn && !g_radioOn) message("The game's radio: change the station on your Pip-Boy (M: another source)", 2.5f);
			else if (focus && nextK) { click(radio::CLICK_BUTTON); if (g_musicOn) music::command(music::CMD_NEXT); else radioNext(); }
			else if (focus && prevK) { click(radio::CLICK_BUTTON); if (g_musicOn) music::command(music::CMD_PREV); else radioPrev(); }
		}
		{
			bool u1 = pressed(VK_RADIOUP), u2 = pressed(VK_RADIOUPPAD), d1 = pressed(VK_RADIODOWN), d2 = pressed(VK_RADIODOWNPAD);
			bool upK = focus && (u1 || u2), dnK = focus && (d1 || d2);
			if (upK || dnK) click(radio::CLICK_DETENT);
			if ((upK || dnK) && g_musicOn) music::command(upK ? music::CMD_VOLUP : music::CMD_VOLDOWN);
			else if ((upK || dnK) && g_musicMode == 2 && !g_radioOn)
			{
				float *gv = gameVolumes();
				int step = (int)rt::clamp((float)(int)(gv[5] * 10 + 0.5f) + (upK ? 1 : -1), 0, 10);
				gv[5] = step / 10.0f;
				char m[48];
				rt::fmt(m, sizeof m, "Game radio volume: %d%%", step * 10);
				message(m, 1.2f);
			}
			else if (upK || dnK)
			{
				int step = (int)(g_radioKnob * 10 + 0.5f) + (upK ? 1 : -1);
				if (step < 0) step = 0;
				if (step > 15) step = 15;
				g_radioKnob = step / 10.0f;
				radioKnobSave();
				char m[48];
				rt::fmt(m, sizeof m, step > 10 ? "Car radio volume: %d%% (boost)" : "Car radio volume: %d%%", step * 10);
				message(m, 1.2f);
				u32 lvl, res; audio::radioVolumeState(&lvl, &res);
				LOG("radio knob %d%% (radio on %d; last set %u, MCI %u)", step * 10, g_radioOn, lvl, res);
			}
		}
		{
			bool su = pressed(VK_SUBUP), sd = pressed(VK_SUBDOWN);
			if (focus && (su || sd))
			{
				click(radio::CLICK_DETENT_LOW);
				g_sub = (int)rt::clamp((float)(g_sub + (su ? 1 : -1)), 0, 10);
				radioKnobSave();
				subApply();
				char m[96];
				const char *what = g_sub == 0 ? " (off)" : g_sub == 10 ? " - MAX" : g_sub > (int)TUNE(subRattleFromLevel) && !g_musicOn ? " (rattling)" : "";
				const char *why = g_musicOn ? (g_sub > (int)TUNE(subShakeFromLevel) ? " - your music shakes the car" : " - your music shakes the car from 3") :
					!g_radioOn ? " - turn the radio on with N" :
					!audio::radioDecoded() ? " - this song can't use it" : "";
				rt::fmt(m, sizeof m, "Subwoofer: %d/10%s%s", g_sub, what, why);
				message(m, 1.5f);
				LOG("subwoofer %d (radio on %d, decoded %d)", g_sub, g_radioOn, audio::radioDecoded());
			}
		}
		if (g_radioOn && audio::radioFinished()) radioNext();
		if (focus && pressed(VK_INVITE) && net::active()) ghosts::inviteNow();
		if (focus && pressed(VK_USE) && rt::absf(v) < 6 && t - g_enterT > 0.6) exitCar();
	}
	else
	{
		if (g_playerHidden) { showChildren(thirdPersonNode(pl), g_hidden3rd); showChildren(firstPersonNode(pl), g_hidden1st); g_playerHidden = false; }
		restoreCamera();
		if (g_radioOn)
		{
			TESObjectREFR *car = g_parkedRef ? (TESObjectREFR *)lookup(g_parkedRef) : 0;
			float *cp = car ? pos(car) : 0;
			float u = kUnitsPerMetre, dx = cp ? cp[0] - pp[0] : 0, dy = cp ? cp[1] - pp[1] : 0;
			float d = rt::sqrt(dx * dx + dy * dy) / u;
			if (!cp || !inMojave || d > TUNE(radioOutsideOffMetres))
			{
				LOG("radio off (%s)", !cp ? "no car" : !inMojave ? "left the Mojave" : "walked away");
				radioOff();
				g_radioFall = 1; audio::setRadioPan(0);
			}
			else
			{
				float nearM = TUNE(radioOutsideFullMetres), farM = TUNE(radioOutsideFadeMetres);
				float k = rt::clamp(1 - (d - nearM) / rt::max(farM - nearM, 1), 0, 1);
				g_radioFall = TUNE(radioOutsideVolume) * k * k;
				float rel = rt::atan2(dx, dy) - rot(pl)[2];
				audio::setRadioPan(d < 1.5f ? 0 : rt::sin(rel) * 0.8f);
				if (audio::radioFinished()) radioNext();
			}
		}
		{
			TESObjectREFR *car = g_parkedRef && g_parkedRef == g_parkedPoseRef ? (TESObjectREFR *)lookup(g_parkedRef) : 0;
			Vehicle &pv = g_parkedPose;
			bool was = pv.shakePitch != 0 || pv.shakeRoll != 0 || pv.shakeZ != 0;
			if (car)
			{
				bassShake(pv, g_radioOn, dt);
				if (was || pv.shakePitch != 0 || pv.shakeRoll != 0 || pv.shakeZ != 0) pushRef(car, pv);
			}
		}
		if (inMojave)
		{
			float dx = pp[0] - kGarage[0], dy = pp[1] - kGarage[1];
			float d2 = dx * dx + dy * dy;
			float r = kGarageRadius * kUnitsPerMetre;
			static bool atGarage;
			if (d2 < (80.0f * kUnitsPerMetre) * (80.0f * kUnitsPerMetre))
			{
				if (!atGarage) { atGarage = true; strayCleanup("garage"); }
				ensureDisplay();
				for (int s = 0; s < kNumSlots && s < 8; s++)
					if (g_display[s] && g_slotCar[s] >= 0) keepOut(pl, (TESObjectREFR *)lookup(g_display[s]), g_slotCar[s]);
			}
			if (g_parkedRef) keepOut(pl, (TESObjectREFR *)lookup(g_parkedRef), g_activeCar);
			else atGarage = false;
			bool nearGarage = d2 < r * r && !g_vanilla;
			if (nearGarage && !g_hinted) { message("Mojave Enhanced garage: press G to pick a car or paint one. Hold G anywhere to send your car home.", 6); g_hinted = true; }
			float &holdG = g_holdG;
			if (!(focus && held(VK_USE))) holdG = 0;
			else if (holdG >= 0) holdG += dt;
			if (holdG > 1.0f && g_parkedRef) { holdG = -1; sendCarHome(); if (nearGarage) ensureDisplay(); }
			if (!nearGarage && d2 > 4 * r * r) g_hinted = false;
			{
				worldCarsScan(pl);
				static u32 hintedRef;
				int wc = -1;
				TESObjectREFR *wr = worldCarNear(pl, &wc, 0);
				if (wr && refid(wr) != hintedRef) { hintedRef = refid(wr); char m[96]; rt::fmt(m, sizeof m, "%s: press G to drive it", kCars[wc].label); message(m, 4); }
				if (!wr) hintedRef = 0;
			}
			if (focus && pressed(VK_USE))
			{
				TESObjectREFR *parked = g_parkedRef ? (TESObjectREFR *)lookup(g_parkedRef) : 0;
				float *cp = parked ? pos(parked) : 0;
				float px = cp ? cp[0] - pp[0] : 1e9f, py = cp ? cp[1] - pp[1] : 1e9f;
				float rideD2 = 1e18f;
				int ride = ghosts::rideCandidate(pl, &rideD2);
				bool ownNear = parked && g_activeCar >= 0 && px * px + py * py < 420.0f * 420.0f;
				int wcar = -1; float wd2 = 1e18f;
				TESObjectREFR *wref = worldCarNear(pl, &wcar, &wd2);
				if (wref && (!ownNear || wd2 < px * px + py * py) && (ride < 0 || wd2 < rideD2)) { g_holdG = -1; worldTake(wref, wcar); }
				else if (ride >= 0 && (!ownNear || rideD2 < px * px + py * py)) { g_holdG = -1; ghosts::rideBegin(pl, ride); }
				else if (ownNear)
				{
					float x = cp[0], y = cp[1], z = cp[2];
					float hd = rot(parked)[2] * 180.0f / rt::PI;
					removeRef(parked);
					g_parkedRef = 0;
					g_holdG = -1;
					enterCar(g_activeCar, x, y, z, hd);
				}
				else if (nearGarage) { g_holdG = -1; openGarage(); }
			}
		}
	}
	prof::add(pDriving ? prof::DRIVE : prof::FOOT, rt::now() - pBlock);
	PROF(HITS, hitTick(dt, pl));
	prof::add(prof::TOTAL, rt::now() - pTick);
	lapReport("the whole tick");
	prof::frame(dt);
}

// ---- save/load
struct SaveRec2
{
	u32 version;
	i32 activeCar;
	u32 parkedRef;
	u32 display[8];
	u32 driving, driveRef;
	float x, y, z, heading, heat;
	u32 cops[6];
};
struct SaveRec
{
	u32 version;
	i32 activeCar;
	u32 parkedRef;
	u32 display[8];
	i32 slotCar[8];
	u32 slotAge[8], slotClock;
	u32 driving, driveRef;
	float x, y, z, heading, heat;
	u32 cops[6];
	u32 beams[4];
};
static void onSave(void *)
{
	SaveRec r; memset(&r, 0, sizeof r);
	r.version = 3; r.activeCar = g_activeCar; r.parkedRef = g_parkedRef;
	for (int i = 0; i < 8; i++) { r.display[i] = g_display[i]; r.slotCar[i] = g_slotCar[i]; r.slotAge[i] = g_slotAge[i]; }
	r.slotClock = g_slotClock;
	r.driving = g_driving; r.driveRef = g_driving ? g_car.refID : 0;
	r.x = g_car.x; r.y = g_car.y; r.z = g_car.z; r.heading = g_car.heading; r.heat = 0;
	for (int i = 0; i < 6; i++) r.cops[i] = 0;
	r.beams[0] = g_headBeam.ref; r.beams[1] = g_headBeam.old; r.beams[2] = g_brakeBeam.ref; r.beams[3] = g_brakeBeam.old;
	g_serial->WriteRecord('MMW3', 3, &r, sizeof r);
	if (g_previewRef) { u32 pv[2] = {g_previewRef, g_liveEdit}; g_serial->WriteRecord('MMWP', 1, pv, sizeof pv); }
	if (g_driving && g_passRef) g_serial->WriteRecord('MMWC', 1, &g_passRef, 4);
	if (g_takenRef) g_serial->WriteRecord('MMWT', 1, &g_takenRef, 4);
}
static u32 resolve(u32 id) { u32 out = 0; return id && g_serial->ResolveRefID(id, &out) ? out : 0; }
static void onLoad(void *)
{
	u32 type, ver, len;
	while (g_serial->GetNextRecordInfo(&type, &ver, &len))
	{
		bool resumeDrive = false; u32 driveRef = 0; float heading = 0;
		if (type == 'MMW2' && len == sizeof(SaveRec2))
		{
			SaveRec2 r; g_serial->ReadRecordData(&r, sizeof r);
			g_activeCar = r.activeCar >= 0 && r.activeCar < kNumCars ? r.activeCar : -1;
			g_parkedRef = resolve(r.parkedRef);
			for (int i = 0; i < 8; i++) g_loadedOldDisplay[i] = resolve(r.display[i]);
			for (int i = 0; i < 6; i++) g_loadedCops[i] = resolve(r.cops[i]);
			resumeDrive = r.driving && g_activeCar >= 0; driveRef = r.driveRef; heading = r.heading;
		}
		else if (type == 'MMW3' && len == sizeof(SaveRec))
		{
			SaveRec r; g_serial->ReadRecordData(&r, sizeof r);
			g_activeCar = r.activeCar >= 0 && r.activeCar < kNumCars ? r.activeCar : -1;
			g_parkedRef = resolve(r.parkedRef);
			for (int i = 0; i < 8; i++)
			{
				g_display[i] = resolve(r.display[i]);
				g_slotCar[i] = r.slotCar[i] >= 0 && r.slotCar[i] < kNumCars ? r.slotCar[i] : -1;
				g_slotAge[i] = r.slotAge[i];
			}
			g_slotClock = r.slotClock;
			for (int i = 0; i < 6; i++) g_loadedCops[i] = resolve(r.cops[i]);
			for (int i = 0; i < 4; i++) g_loadedBeams[i] = resolve(r.beams[i]);
			resumeDrive = r.driving && g_activeCar >= 0; driveRef = r.driveRef; heading = r.heading;
		}
		else if (type == 'MMWT' && len == 4) { u32 t = 0; g_serial->ReadRecordData(&t, 4); g_takenRef = resolve(t); }
		else if (type == 'MMWC' && len == 4)
		{
			u32 c; g_serial->ReadRecordData(&c, 4);
			g_passRef = resolve(c);
			continue;
		}
		else if (type == 'MMWP' && len == 8)
		{
			u32 pv[2]; g_serial->ReadRecordData(pv, sizeof pv);
			g_loadedPreview = resolve(pv[0]);
			if (pv[1]) g_liveRelease = true;
			continue;
		}
		else continue;
		if (resumeDrive)
		{
			memset(&g_car, 0, sizeof g_car);
			g_car.refID = resolve(driveRef);
			g_car.heading = heading;
			g_resume = true;
			g_needRelease = true;
		}
	}
	g_afterLoadPending = true; g_loads++;
}
static void resetRuntime()
{
	ghosts::unload();
	radioOff();
	musicOff(true);
	audio::setEngine(false, 0, 0, 0);
	if (g_driving || g_tcl) g_needRelease = true;
	g_driving = false; g_menu = 0; g_pendingChoice = -1; g_hinted = false; g_resume = false;
	for (int i = 0; i < 6; i++) g_loadedCops[i] = 0;
	for (int i = 0; i < 8; i++) g_hits[i].t = 0;
	g_activeCar = -1; g_parkedRef = 0; g_takenRef = 0; g_nWorldCars = 0; g_worldScanCell = 0;
	memset(g_display, 0, sizeof g_display);
	for (int i = 0; i < 8; i++) { g_slotCar[i] = -1; g_slotAge[i] = 0; }
	g_slotClock = 0;
	memset(&g_headBeam, 0, sizeof g_headBeam); memset(&g_headNear, 0, sizeof g_headNear); memset(&g_brakeBeam, 0, sizeof g_brakeBeam); memset(&g_headFar, 0, sizeof g_headFar);
	if (g_liveEdit) g_liveRelease = true;
	g_smokeRef[0] = g_smokeRef[1] = 0; g_burning = false; g_launchT = g_smokeT = 0; g_revsOverride = -1;
	g_liveEdit = false; g_reopenPaintAt = 0; g_previewRef = 0; g_previewCar = -1; g_previewRoot = 0; g_paintCar = -1;
	memset(g_ptrack, 0, sizeof g_ptrack);
	ridersDrop(); g_passRef = 0;
	driftFovOff(player());
	g_afterLoadPending = true; g_loads++;
}

// ---- (plugin 201, 2026-10-18, the author: the game "broke again" -; the log
namespace hang
{
static const double kHangSec = 5;
static volatile double s_beat;
static volatile u32 s_tid;
static void beat() { s_beat = rt::now(); if (!s_tid) s_tid = GetCurrentThreadId(); }
static u32 __stdcall watch(void *)
{
	HANDLE k = LoadLibraryA("kernel32.dll");
	auto openT = (HANDLE(__stdcall *)(u32, int, u32))GetProcAddress(k, "OpenThread");
	auto susp = (u32(__stdcall *)(HANDLE))GetProcAddress(k, "SuspendThread");
	auto res = (u32(__stdcall *)(HANDLE))GetProcAddress(k, "ResumeThread");
	auto ctx = (int(__stdcall *)(HANDLE, void *))GetProcAddress(k, "GetThreadContext");
	if (!openT || !susp || !res || !ctx) return 0;
	HANDLE th = 0;
	bool told = false;
	static u8 c[720] __attribute__((aligned(16)));
	for (;;)
	{
		Sleep(1000);
		if (!s_tid || s_beat <= 0) continue;
		double gap = rt::now() - s_beat;
		static int reports;
		if (gap < kHangSec) { if (told) { told = false; reports = 0; LOG("hang: the main loop is back (%.0f s without it)", gap); } continue; }
		if (reports >= 3 || (reports == 1 && gap < 15) || (reports == 2 && gap < 40)) continue;
		reports++;
		told = true;
		if (!th) th = openT(0x2 | 0x8 | 0x40, 0, s_tid);
		LOG("hang: no frame for %.0f s - last of ours: %s; the game's main thread (%u):", gap, g_crumb, s_tid);
		for (int i = 0; th && i < 8; i++)
		{
			static u32 stk[768];
			u32 ns = 0;
			memset(c, 0, sizeof c);
			*(u32 *)c = 0x10003;
			if (susp(th) == 0xFFFFFFFF) break;
			bool got = ctx(th, c) != 0;
			u32 eip = *(u32 *)(c + 0xB8), esp = *(u32 *)(c + 0xC4);
			u32 mbi[7] = {0};
			if (got && s_vq && s_vq((const void *)esp, mbi, sizeof mbi) && mbi[4] == 0x1000)
			{
				u32 top = mbi[0] + mbi[3];
				for (u32 a = esp; a + 4 <= top && ns < 768; a += 4) stk[ns++] = *(u32 *)a;
			}
			res(th);
			char line[480], w[120] = "?";
			int n, hits = 0;
			if (got) addrInfo(eip, w, sizeof w, false);
			n = rt::fmt(line, sizeof line, "  %d: at %s; stack:", i, got ? w : "(no context)");
			for (u32 j = 0; j < ns && hits < 14; j++)
			{
				char x[120];
				if (stk[j] < 0x10000 || !addrInfo(stk[j], x, sizeof x, true)) continue;
				n += rt::fmt(line + n, sizeof line - n, " %s", x); hits++;
			}
			LOG("%s", line);
			Sleep(250);
		}
		logFlush();
		u32 ln = 0;
		char *all = rt::readfile(g_logPath, &ln), cp[320];
		rt::fmt(cp, sizeof cp, "%sData\\NVSE\\Plugins\\MojaveMostWanted-crash.log", g_dir);
		if (all) { rt::writefile(cp, all, ln, false); rt::free(all); }
	}
}
static void start() { static bool on; if (on) return; on = true; u32 tid; CreateThread(0, 128 * 1024, watch, 0, 0, &tid); }
}
static void onMessage(NVSEMessagingInterface::Message *msg)
{
	switch (msg->type)
	{
	case kMessage_PreLoadGame: case kMessage_ExitToMainMenu: case kMessage_NewGame: if (msg->type == kMessage_PreLoadGame) coop::preload((const char *)msg->data); else coop::leavingWorld(msg->type == kMessage_NewGame); g_worldUp = false; CRUMB("before a load"); resetRuntime(); CRUMB("loading"); break;
	case kMessage_PostLoadGame: case kMessage_DeferredInit:
		if (msg->type == kMessage_PostLoadGame) { questlog::loaded(); coop::loaded(); }
		if (!g_ready && compileAll())
		{
			double idx = 255;
			callNum(s_modIndex, &idx);
			if (idx >= 0 && idx < 255) { g_mod = (u32)idx; g_ready = true; }
			LOG("mod index %u ready %d", g_mod, g_ready);
			installHooks();
			if (!g_ready) LOG("MojaveMostWanted.esp is not active; the plugin stays idle");
			loadConfig();
		}
		break;
	case kMessage_MainGameLoop: hang::beat(); hang::start(); tick(); break;
	case kMessage_SaveGame: ghosts::beforeSave(); coop::saved((const char *)msg->data); break;
	case kMessage_ExitGame: logFlush(); break;
	}
}

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface *nvse, PluginInfo *info)
{
	info->infoVersion = 1;
	info->name = "MojaveMostWanted";
	info->version = 2;
	return !nvse->isEditor && nvse->nvseVersion >= 0x06040000 && nvse->runtimeVersion == 0x040020D0;
}
__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface *nvse)
{
	g_handle = nvse->GetPluginHandle();
	rt::strcpy(g_dir, nvse->GetRuntimeDirectory ? nvse->GetRuntimeDirectory() : "", sizeof g_dir);
	rt::fmt(g_logPath, sizeof g_logPath, "%sData\\NVSE\\Plugins\\MojaveMostWanted.log", g_dir);
	{
		u32 n = 0;
		char *old = rt::readfile(g_logPath, &n), prev[320];
		rt::fmt(prev, sizeof prev, "%sData\\NVSE\\Plugins\\MojaveMostWanted-previous.log", g_dir);
		if (old) { rt::writefile(prev, old, n, false); rt::free(old); }
		rt::fmt(prev, sizeof prev, "%sData\\NVSE\\Plugins\\MojaveMostWanted.prev.log", g_dir);
		DeleteFileA(prev);
	}
	rt::writefile(g_logPath, "", 0, false);
	LOG("Mojave Enhanced plugin " STR(PLUGIN_BUILD) " (co-op: death after 20 s down, scaling to the host, no gate, character creation together) loaded, xNVSE %x, game %x", nvse->nvseVersion, nvse->runtimeVersion);
	crashReportStart();
	{
		HANDLE u = LoadLibraryA("user32.dll");
		int (__stdcall *aware)() = u ? (int (__stdcall *)())GetProcAddress(u, "SetProcessDPIAware") : 0;
		LOG("screen: DPI aware %s", aware ? (aware() ? "yes" : "refused") : "(no SetProcessDPIAware)");
	}
	g_script = (NVSEScriptInterface *)nvse->QueryInterface(kInterface_Script);
	g_console = (NVSEConsoleInterface *)nvse->QueryInterface(kInterface_Console);
	g_cmds = (NVSECommandTableInterface *)nvse->QueryInterface(kInterface_CommandTable);
	g_serial = (NVSESerializationInterface *)nvse->QueryInterface(kInterface_Serialization);
	NVSEMessagingInterface *msg = (NVSEMessagingInterface *)nvse->QueryInterface(kInterface_Messaging);
	if (!g_script || !g_console || !msg) { LOG("missing xNVSE interfaces"); return false; }
	msg->RegisterListener(g_handle, "NVSE", onMessage);
	if (g_serial)
	{
		g_serial->SetSaveCallback(g_handle, onSave);
		g_serial->SetLoadCallback(g_handle, onLoad);
	}
	rt::fmt(g_pluginDir, sizeof g_pluginDir, "%sData\\NVSE\\Plugins\\MojaveMostWanted\\", g_dir);
	audio::start(g_pluginDir);
	g_lastTime = rt::now();
	return true;
}
int __stdcall _DllMainCRTStartup(void *, u32, void *) { return 1; }
}
