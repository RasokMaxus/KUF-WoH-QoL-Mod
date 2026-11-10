// perf.cpp - diagnostic for menu/game responsiveness (setting perf-trace, default off).
//
// The game runs its menus and matches from MFC's idle handler (CKufApp::OnIdle 0x4FEE30), which
// calls the main frame's state update 0x593F80 once per "frame" (state number at frame+0x128).
// Between two idle calls MFC handles the window messages (mouse, keyboard, timers).
//
// With perf-trace on, the call at 0x4FEE4B is routed through StateUpdate below, and a message
// hook on the game's main thread watches mouse messages. Once per second kufmod.log gets:
//   perf state S: F frames, update avg/max ms, gap max ms, mouse N msgs, lag avg/max ms, cursor ...
// "update" = time inside 0x593F80, "gap" = time from the end of one update to the start of the
// next (message handling + MFC), "lag" = how old a mouse message is when the game takes it.
#include "kufmod.h"
#include <stdio.h>

bool g_perfTrace = false;

typedef void(__thiscall* StateUpdateFn)(void* frame);
static StateUpdateFn g_update;
static LARGE_INTEGER g_freq;
static double g_lastEnd, g_secondStart;
static int g_frames, g_lastState = -1;
static double g_updSum, g_updMax, g_gapMax;
static int g_mouseMsgs, g_mouseOther;
static double g_lagSum, g_lagMax;
static HHOOK g_msgHook;

static double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_freq.QuadPart;
}

static LRESULT CALLBACK MsgHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == PM_REMOVE) {
        const MSG* m = (const MSG*)l;
        if (m->message == WM_MOUSEMOVE) {
            g_mouseMsgs++;
            double lag = (double)(LONG)(GetTickCount() - m->time);
            g_lagSum += lag;
            if (lag > g_lagMax) g_lagMax = lag;
        } else if (m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST) {
            g_mouseOther++;
        }
    }
    return CallNextHookEx(g_msgHook, code, w, l);
}

static void Report(int state) {
    CURSORINFO ci = {sizeof ci};
    GetCursorInfo(&ci);
    RECT clip = {};
    GetClipCursor(&clip);
    Log("perf state %d: %d frames, update avg %.1f max %.1f ms, gap max %.1f ms, mouse %d moves "
        "(+%d other), lag avg %.0f max %.0f ms, cursor %s %p at %ld,%ld clip %ld,%ld-%ld,%ld",
        state, g_frames, g_frames ? g_updSum / g_frames : 0.0, g_updMax, g_gapMax, g_mouseMsgs,
        g_mouseOther, g_mouseMsgs ? g_lagSum / g_mouseMsgs : 0.0, g_lagMax,
        (ci.flags & CURSOR_SHOWING) ? "shown" : "hidden", (void*)ci.hCursor, ci.ptScreenPos.x,
        ci.ptScreenPos.y, clip.left, clip.top, clip.right, clip.bottom);
    g_frames = g_mouseMsgs = g_mouseOther = 0;
    g_updSum = g_updMax = g_gapMax = g_lagSum = g_lagMax = 0;
}

static void __fastcall StateUpdate(void* frame) {
    if (!g_msgHook) g_msgHook = SetWindowsHookExA(WH_GETMESSAGE, MsgHook, NULL, GetCurrentThreadId());
    int state = *(int*)((char*)frame + 0x128);
    double start = Now();
    if (g_secondStart == 0) g_secondStart = start;
    if (state != g_lastState || start - g_secondStart >= 1000) {
        if (g_lastState != -1) Report(g_lastState);
        if (state != g_lastState) Log("perf: state %d -> %d", g_lastState, state);
        g_lastState = state;
        g_secondStart = start;
    }
    if (g_lastEnd != 0 && start - g_lastEnd > g_gapMax) g_gapMax = start - g_lastEnd;
    g_update(frame);
    double end = Now();
    double d = end - start;
    g_updSum += d;
    if (d > g_updMax) g_updMax = d;
    g_frames++;
    g_lastEnd = end;
}

// ---------------------------------------------------------------- order delay
// Orders the local player gives go into the command queue [0xB870C8] (vtable slot +4:
// single player 0x58B8E0, network EventTCPIPCmdQueue 0x6AFC00; args data, len, -1) and are later
// carried out by 0x4DB870 (thiscall game, len, data; called from 0x4DB124, 0x4DB170, 0x4DB809).
// We note each queued order and log how long it waited when the same bytes are carried out.
typedef int(__thiscall* PushFn)(void* q, const void* data, int len, int x);
typedef char(__thiscall* DispatchFn)(void* game, int len, const void* data);
static PushFn g_spPush, g_mpPush;
static DispatchFn g_dispatch;
struct Pending { uint32_t hash; uint16_t cmd; int len; double t; };
static Pending g_pending[64];
static int g_numPending;

static uint32_t Hash(const void* data, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len && i < 512; i++) h = (h ^ ((const BYTE*)data)[i]) * 16777619u;
    return h;
}

static void NoteOrder(const void* data, int len) {
    if (!data || len < 2) return;
    if (g_numPending == 64) memmove(g_pending, g_pending + 1, --g_numPending * sizeof(Pending));
    g_pending[g_numPending++] = {Hash(data, len), *(const uint16_t*)data, len, Now()};
}

static int __fastcall SpPush(void* q, void*, const void* data, int len, int x) {
    NoteOrder(data, len);
    return g_spPush(q, data, len, x);
}
static int __fastcall MpPush(void* q, void*, const void* data, int len, int x) {
    NoteOrder(data, len);
    return g_mpPush(q, data, len, x);
}

static char __fastcall Dispatch(void* game, void*, int len, const void* data) {
    if (g_numPending && data && len >= 2) {
        uint32_t h = Hash(data, len);
        uint16_t cmd = *(const uint16_t*)data;
        int found = -1;
        for (int i = 0; i < g_numPending && found < 0; i++)
            if (g_pending[i].hash == h && g_pending[i].len == len) found = i;
        for (int i = 0; i < g_numPending && found < 0; i++)  // bytes changed on the way
            if (g_pending[i].cmd == cmd && g_pending[i].len == len) found = i;
        if (found >= 0) {
            Log("perf order 0x%X len %d: carried out after %.0f ms%s", cmd, len,
                Now() - g_pending[found].t, g_pending[found].hash == h ? "" : " (bytes changed)");
            memmove(g_pending + found, g_pending + found + 1, (--g_numPending - found) * sizeof(Pending));
        }
    }
    return g_dispatch(game, len, data);
}

static bool SwapSlot(uintptr_t slotVa, uintptr_t originalVa, void* replacement) {
    uintptr_t want = originalVa + g_slide, repl = (uintptr_t)replacement;
    return Patch(slotVa, &want, &repl, sizeof want);
}

static void InstallOrderTrace() {
    g_spPush = (PushFn)(0x58B8E0 + g_slide);
    g_mpPush = (PushFn)(0x6AFC00 + g_slide);
    g_dispatch = (DispatchFn)(0x4DB870 + g_slide);
    bool ok = SwapSlot(0xA44488 + 4, 0x58B8E0, (void*)SpPush) && SwapSlot(0xA4C9A4 + 4, 0x6AFC00, (void*)MpPush);
    static const uintptr_t sites[3] = {0x4DB124, 0x4DB170, 0x4DB809};
    for (uintptr_t site : sites) {
        BYTE expected[5] = {0xE8};
        *(int32_t*)(expected + 1) = (int32_t)(0x4DB870 - (site + 5));
        ok = HookCall(site, expected, sizeof expected, (void*)Dispatch) && ok;
    }
    Log(ok ? "perf order trace on" : "perf order trace: unexpected game code (partly off)");
}

void InstallPerf() {
    if (!g_perfTrace) return;
    QueryPerformanceFrequency(&g_freq);
    InstallOrderTrace();
    g_update = (StateUpdateFn)(0x593F80 + g_slide);
    // call 0x593F80 at 0x4FEE4B (rel32 = 0x593F80 - 0x4FEE50 = 0x95130)
    static const BYTE expected[5] = {0xE8, 0x30, 0x51, 0x09, 0x00};
    if (HookCall(0x4FEE4B, expected, sizeof expected, StateUpdate)) Log("perf-trace on");
    else Log("unexpected game code at 0x4FEE4B (different game version?). perf-trace off.");
}
