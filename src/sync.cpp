// sync.cpp - "sync" lines in kufmod.log during network matches (always on).
//
// The game's own sync check (0x4F9CB0, every turn) compares the shared random generator between
// PCs: its state [0xBED1E8] and how often it was drawn [0xB827DC]. When they differ, the match
// shows "-SyncErr-" (top bar, flag game+0x46C) - but not when or why.
//
// Every order the game carries out goes through 0x4DB870 (thiscall game, len, data; called at
// 0x4DB124, 0x4DB170, 0x4DB809). In a network match every PC carries out the same orders in the
// same order, among them a turn marker 0x62 from each player every turn. Every 200 markers this
// file logs one line with the random generator and the mod's own decision counters (target
// switches kept/allowed, chases kept by the leash, building spot searches, avoidance yields).
// Two players' lines with the same number must be identical; the first line that differs shows
// when the games went apart and which feature's counter moved first.
#include "kufmod.h"

extern uint32_t g_syncKept, g_syncSwitched, g_syncLeash, g_syncSpots;  // combat.cpp
extern uint32_t g_syncYield;                                           // pathing.cpp
void PerfOrderDispatched(int len, const void* data);                   // perf.cpp
extern bool g_perfTrace;

typedef char(__thiscall* DispatchFn)(void* game, int len, const void* data);
static DispatchFn g_dispatch;
static uintptr_t g_syncGame;
static uint32_t g_markers;

static char __fastcall SyncDispatch(void* game, void*, int len, const void* data) {
    if (g_perfTrace) PerfOrderDispatched(len, data);
    char r = g_dispatch(game, len, data);
    if (data && len >= 2 && *(const uint16_t*)data == 0x62 && Game<BYTE>(0xB83AD0)) {
        static int lastClock;
        int clock = *(int*)((BYTE*)game + 0x604);
        bool restarted = clock < lastClock;
        lastClock = clock;
        if ((uintptr_t)game != g_syncGame || restarted) {  // a new match: start counting again
            g_syncGame = (uintptr_t)game;
            g_markers = 0;
            g_syncKept = g_syncSwitched = g_syncLeash = g_syncSpots = g_syncYield = 0;
        }
        if (++g_markers % 200 == 0)
            Log("sync %u: random %08X drawn %u | targets kept %u switched %u, chases %u, spots %u, "
                "yields %u",
                g_markers, Game<uint32_t>(0xBED1E8), Game<uint32_t>(0xB827DC), g_syncKept,
                g_syncSwitched, g_syncLeash, g_syncSpots, g_syncYield);
    }
    return r;
}

void InstallSyncLog() {
    g_dispatch = (DispatchFn)(0x4DB870 + g_slide);
    static const uintptr_t sites[3] = {0x4DB124, 0x4DB170, 0x4DB809};
    bool ok = true;
    for (uintptr_t site : sites) {
        BYTE expected[5] = {0xE8};
        *(int32_t*)(expected + 1) = (int32_t)(0x4DB870 - (site + 5));
        ok = HookCall(site, expected, sizeof expected, (void*)SyncDispatch) && ok;
    }
    if (!ok) Log("unexpected game code at the order dispatcher (different game version?). sync lines off.");
}
