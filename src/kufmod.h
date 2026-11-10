// kufmod.h - shared by the kufmod source files.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

// Game addresses in this source are the ones from KingdomUnderFire.exe's preferred image base
// (0x400000). Windows may load the game elsewhere (ASLR); g_slide is the difference.
extern uintptr_t g_slide;
extern char g_dir[MAX_PATH];  // game folder, with a trailing backslash
extern int g_instance;        // 1 for the first copy of the game on this PC, 2 for a second...

template <class T> T Game(uintptr_t va) { return *(volatile T*)(va + g_slide); }

void Log(const char* fmt, ...);

// Replaces len bytes of game code at va (which must equal expected) with a jump to target and
// NOPs. *resume receives the address right after the replaced bytes.
bool Hook(uintptr_t va, const BYTE* expected, size_t len, void* target, uintptr_t* resume);
// Same, but with a call to target (which returns with ret) instead of a jump.
bool HookCall(uintptr_t va, const BYTE* expected, size_t len, void* target);
// Overwrites len bytes at va (code or data) if they equal expected.
bool Patch(uintptr_t va, const void* expected, const void* replacement, size_t len);

// Settings from kufmod.yml that other files use.
extern bool g_standardMode, g_emptyNodes, g_buildButton, g_lanPlay;
extern bool g_locusOnline;     // locus-online: false = LAN only (lan.cpp)
extern char g_lanIp[64];      // lan-ip: "auto" or an address
extern char g_lanPeers[256];  // lan-peers: extra addresses for room announcements
extern bool g_lanTrace;       // lan-trace: network log + netdump.txt

// perf.cpp (perf-trace diagnostic)
extern bool g_perfTrace;
void InstallPerf();
// sync.cpp
void InstallSyncLog();
// maps.cpp
extern bool g_randomMaps, g_meleeDefault;
void InstallRandomMaps();
// combat.cpp
extern bool g_targetPriority;
void InstallCombat();
// pathing.cpp
extern bool g_unitAvoidance;
void InstallPathing();
// turns.cpp
extern int g_spTurn, g_netTurns[3], g_netStart;
void InstallTurns();
// menufps.cpp
extern int g_menuFps;
void InstallMenuFps();
// economy.cpp
void InstallEconomy();
// lan.cpp
void InstallLan();
// aimp.cpp
void InstallAiMp();
extern bool g_mpComputers;  // multiplayer-computers
extern int g_aiFirst;       // first computer player index in the current match (99 = none)
void AiRoomReset();
void AiHostSend();
int AiRoomChat(const char* sender, const char* text);  // 0 not ours, 2 hidden
void AiRoomSync(HWND room);                            // every 200 ms in the room
bool AiRoomCommand(HWND room, WPARAM w, LPARAM l, WNDPROC original);  // true = handled
// kufmod.cpp (multiplayer room)
bool IsHost();
const char* MyName();
void SendRoomLine(const char* text);
// icons.cpp: builds res\misc\kufmod_icons.URE and points the game at it. Returns false if the
// icons could not be prepared (the Build button then uses a building picture).
bool PrepareBuildIcons(int32_t* humanIcon, int32_t* devilIcon);
