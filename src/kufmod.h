// kufmod.h - shared by the kufmod source files.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

// Game addresses in this source are the ones from KingdomUnderFire.exe's preferred image base
// (0x400000). Windows may load the game elsewhere (ASLR); g_slide is the difference.
extern uintptr_t g_slide;
extern char g_dir[MAX_PATH];  // game folder, with a trailing backslash

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
extern bool g_standardMode, g_emptyNodes, g_buildButton;

// economy.cpp
void InstallEconomy();
// icons.cpp: builds res\misc\kufmod_icons.URE and points the game at it. Returns false if the
// icons could not be prepared (the Build button then uses a building picture).
bool PrepareBuildIcons(int32_t* humanIcon, int32_t* devilIcon);
