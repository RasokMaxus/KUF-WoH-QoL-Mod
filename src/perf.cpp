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

void InstallPerf() {
    if (!g_perfTrace) return;
    QueryPerformanceFrequency(&g_freq);
    g_update = (StateUpdateFn)(0x593F80 + g_slide);
    // call 0x593F80 at 0x4FEE4B (rel32 = 0x593F80 - 0x4FEE50 = 0x95130)
    static const BYTE expected[5] = {0xE8, 0x30, 0x51, 0x09, 0x00};
    if (HookCall(0x4FEE4B, expected, sizeof expected, StateUpdate)) Log("perf-trace on");
    else Log("unexpected game code at 0x4FEE4B (different game version?). perf-trace off.");
}
