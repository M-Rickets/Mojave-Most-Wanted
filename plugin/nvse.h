// The parts of xNVSE's plugin API this plugin uses, with the same layout.
#pragma once
#include "rt.h"
struct TESForm;
struct TESObjectREFR;
struct Script;
typedef u32 PluginHandle;
enum { kInterface_Serialization = 0, kInterface_Console, kInterface_Messaging, kInterface_CommandTable, kInterface_StringVar, kInterface_ArrayVar, kInterface_Script };
enum { kMessage_PostLoad = 0, kMessage_ExitGame = 1, kMessage_ExitToMainMenu = 2, kMessage_LoadGame = 3, kMessage_SaveGame = 4, kMessage_PreLoadGame = 6,
	kMessage_PostLoadGame = 8, kMessage_PostPostLoad = 9, kMessage_NewGame = 14, kMessage_DeferredInit = 18, kMessage_MainGameLoop = 20 };
struct PluginInfo { u32 infoVersion; const char *name; u32 version; };
struct NVSEInterface
{
	u32 nvseVersion, runtimeVersion, editorVersion, isEditor;
	bool (*RegisterCommand)(void *);
	void (*SetOpcodeBase)(u32);
	void *(*QueryInterface)(u32);
	PluginHandle (*GetPluginHandle)();
	bool (*RegisterTypedCommand)(void *, u32);
	const char *(*GetRuntimeDirectory)();
};
// The command table: whether a command is in this game - NVSECommandTableInterface's first members (version, Start,
// End, GetByOpcode, GetByName)
struct NVSECommandTableInterface { u32 version; const void *(*Start)(); const void *(*End)(); const void *(*GetByOpcode)(u32); const void *(*GetByName)(const char *); };
struct NVSEConsoleInterface { u32 version; bool (*RunScriptLine)(const char *, TESObjectREFR *); bool (*RunScriptLine2)(const char *, TESObjectREFR *, bool); };
struct NVSEMessagingInterface
{
	struct Message { const char *sender; u32 type; u32 dataLen; void *data; };
	typedef void (*EventCallback)(Message *);
	u32 version;
	bool (*RegisterListener)(PluginHandle, const char *, EventCallback);
	bool (*Dispatch)(PluginHandle, u32, void *, u32, const char *);
};
struct NVSEElement
{
	union { char *str; void *arr; TESForm *form; double num; };
	u8 type;
};
struct NVSEScriptInterface
{
	bool (*CallFunction)(Script *, TESObjectREFR *, TESObjectREFR *, NVSEElement *, u8, ...);
	u32 (*GetFunctionParams)(Script *, u8 *);
	void *ExtractArgsEx;
	void *ExtractFormatStringArgs;
	bool (*CallFunctionAlt)(Script *, TESObjectREFR *, u8, ...);
	Script *(*CompileScript)(const char *);
	Script *(*CompileExpression)(const char *);
};
struct NVSESerializationInterface
{
	typedef void (*EventCallback)(void *);
	u32 version;
	void (*SetSaveCallback)(PluginHandle, EventCallback);
	void (*SetLoadCallback)(PluginHandle, EventCallback);
	void (*SetNewGameCallback)(PluginHandle, EventCallback);
	bool (*WriteRecord)(u32 type, u32 version, const void *buf, u32 length);
	bool (*OpenRecord)(u32, u32);
	bool (*WriteRecordData)(const void *, u32);
	bool (*GetNextRecordInfo)(u32 *type, u32 *version, u32 *length);
	u32 (*ReadRecordData)(void *buf, u32 length);
	bool (*ResolveRefID)(u32 refID, u32 *outRefID);
	void (*SetPreLoadCallback)(PluginHandle, EventCallback);
};
