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
#include <stdarg.h>
#include <share.h>
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

// Stalls: the match clock (game+0x604, ms) normally moves every few frames. When it stands still
// for more than 200 ms of real time and then moves again, the game was waiting: in a network
// match usually for another player's turn (Network Speed index word 0xB3BAF4), otherwise slow
// frames. Pauses and menus also stop the clock, so long stalls may be those.
static double g_clockSince;
static int g_lastClock = -1;

static void CheckStall(double now) {
    uintptr_t game = Game<uintptr_t>(0xB8282C);
    if (!game) { g_lastClock = -1; return; }
    int clock = *(int*)(game + 0x604);
    if (clock == g_lastClock) return;
    if (g_lastClock > 0 && clock > g_lastClock && now - g_clockSince > 200) {
        int s = g_lastClock / 1000;
        Log("perf stall: match clock stood still %.0f ms at %d:%02d (%s, Network Speed index %d)",
            now - g_clockSince, s / 60, s % 60, Game<BYTE>(0xB83AD0) ? "network match" : "single player",
            Game<int16_t>(0xB3BAF4));
    }
    g_lastClock = clock;
    g_clockSince = now;
}

static void __fastcall StateUpdate(void* frame) {
    if (!g_msgHook) g_msgHook = SetWindowsHookExA(WH_GETMESSAGE, MsgHook, NULL, GetCurrentThreadId());
    int state = *(int*)((char*)frame + 0x128);
    double start = Now();
    CheckStall(start);
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
static PushFn g_spPush, g_mpPush;
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
    // Orders the game sends by itself every turn (0x62 turn marker; network: 0x3F1, 0x3EC) are not
    // traced: at 1-tick turns that would be ~33 log lines a second.
    uint16_t cmd = *(const uint16_t*)data;
    if (cmd == 0x62 || cmd == 0x3F1 || cmd == 0x3EC) return;
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

// Called by sync.cpp's dispatcher hook for every order carried out (perf-trace on).
void PerfOrderDispatched(int len, const void* data) {
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
}

static bool SwapSlot(uintptr_t slotVa, uintptr_t originalVa, void* replacement) {
    uintptr_t want = originalVa + g_slide, repl = (uintptr_t)replacement;
    return Patch(slotVa, &want, &repl, sizeof want);
}

static void InstallOrderTrace() {
    g_spPush = (PushFn)(0x58B8E0 + g_slide);
    g_mpPush = (PushFn)(0x6AFC00 + g_slide);
    bool ok = SwapSlot(0xA44488 + 4, 0x58B8E0, (void*)SpPush) && SwapSlot(0xA4C9A4 + 4, 0x6AFC00, (void*)MpPush);
    // The dispatcher (0x4DB870) is hooked by sync.cpp, which calls PerfOrderDispatched.
    Log(ok ? "perf order trace on" : "perf order trace: unexpected game code (partly off)");
}

// ---------------------------------------------------------------- path trace
// Unit movement state machine 0x47E540 (thiscall unit, 2 args, ret 8), state word unit+0x31A:
//  0 start, 1 set up, 3 turn + try a step, 4 walk, 9 ?, 10 arrived (stop), 11 blocked (0x47F760:
//  step result 0 ok -> 3, 1/2 -> 14, 3 = a unit is in the way: yield -> 14 with "avoid units"
//  flag +0x271, else wait, counter +0x360 up to 50), 12 give up, 13 wait then new path 0x47EE00
//  (retry counter +0x362; "arrived" when within 0x20 of the target, or 0x80 after 3 retries),
//  14 new path now. Position +0x18/+0x1C, target +0x178/+0x17C (or the path's end cell).
// Every state change of the local player's units (except 3 <-> 4) goes to pathtrace.txt.
static FILE* g_pathFile;
static BYTE* g_moveTramp;
typedef int(__thiscall* MoveFn)(void* unit, int a, int b);

static void PathLogV(const char* fmt, va_list ap) {
    if (!g_pathFile) {
        char path[MAX_PATH];
        if (g_instance > 1) sprintf_s(path, "%spathtrace%d.txt", g_dir, g_instance);
        else sprintf_s(path, "%spathtrace.txt", g_dir);
        g_pathFile = _fsopen(path, "w", _SH_DENYNO);  // readable while the game runs
        if (!g_pathFile) return;
    }
    vfprintf(g_pathFile, fmt, ap);
    fputc('\n', g_pathFile);
    fflush(g_pathFile);
}

static void PathLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    PathLogV(fmt, ap);
    va_end(ap);
}

// For other files (combat.cpp): a line in pathtrace.txt.
void PerfNote(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    PathLogV(fmt, ap);
    va_end(ap);
}

static int __fastcall MoveStep(BYTE* unit, void*, int a, int b) {
    int16_t before = *(int16_t*)(unit + 0x31A);
    uint16_t waitBefore = *(uint16_t*)(unit + 0x360);
    int r = ((MoveFn)(void*)g_moveTramp)(unit, a, b);
    int16_t after = *(int16_t*)(unit + 0x31A);
    uint16_t wait = *(uint16_t*)(unit + 0x360);
    bool waitMark = wait != waitBefore && (wait == 10 || wait == 25 || wait == 50);
    if (before == after && !waitMark) return r;
    if ((before == 3 && after == 4) || (before == 4 && after == 3)) return r;
    uintptr_t game = Game<uintptr_t>(0xB8282C);
    if (!game || *(int16_t*)(unit + 8) != *(int*)(game + 0x664)) return r;  // owner is a word
    int x = *(int*)(unit + 0x18), y = *(int*)(unit + 0x1C), tx, ty;
    BYTE* path = *(BYTE**)(unit + 0x168);
    if (path && path[0x16]) {
        tx = (*(int*)(path + 0xC) << 4) + 0x88;
        ty = (*(int*)(path + 0x10) << 4) + 0x88;
    } else {
        tx = *(int*)(unit + 0x178);
        ty = *(int*)(unit + 0x17C);
    }
    PathLog("%8lu unit %p id %d: state %d -> %d (ret %d) pos %d,%d target %d,%d (dx %d dy %d) "
            "wait %u retries %u avoid %d",
            GetTickCount(), unit, *(int16_t*)(unit + 0xA), before, after, r, x, y, tx, ty, tx - x,
            ty - y, wait, *(uint16_t*)(unit + 0x362), unit[0x271]);
    if (before == 0) {  // a new destination: what is the unit doing, and at whom?
        BYTE* at = *(BYTE**)(unit + 0x120);  // attack target
        if (at)
            PathLog("         attacking %p type %d kind %d owner %d pos %d,%d (attack mode %d)", at,
                    *(int16_t*)(at + 4), *(int16_t*)(at + 6), *(int16_t*)(at + 8), *(int*)(at + 0x18),
                    *(int*)(at + 0x1C), *(int16_t*)(unit + 0x31C));
        BYTE* t = *(BYTE**)(unit + 0x11C);
        if (t)
            PathLog("         action %d target obj %p type %d owner %d pos %d,%d state %d",
                    *(int16_t*)(unit + 0x6C), t, *(int16_t*)(t + 4), *(int16_t*)(t + 8),
                    *(int*)(t + 0x18), *(int*)(t + 0x1C), *(int16_t*)(t + 0x31A));
        else
            PathLog("         action %d no target obj", *(int16_t*)(unit + 0x6C));
    }
    return r;
}

// Called by pathing.cpp's hook at 0x47F7F8 ("a unit is in the way") when perf-trace is on.
// Logged when the blocker changes or every 16th time.
void NoteBlock(BYTE* unit, int idx, bool yieldNow) {
    uintptr_t g_objTable = 0xB870C0 + g_slide;
    uintptr_t game = Game<uintptr_t>(0xB8282C);
    if (!game || *(int16_t*)(unit + 8) != *(int*)(game + 0x664)) return;
    static BYTE* lastUnit;
    static int lastIdx, repeats;
    if (unit == lastUnit && idx == lastIdx && ++repeats % 16) return;
    if (unit != lastUnit || idx != lastIdx) repeats = 0;
    lastUnit = unit;
    lastIdx = idx;
    uintptr_t world = *(uintptr_t*)g_objTable;
    BYTE* b = world ? *(BYTE**)(world + 0x2C38 + idx * 4) : NULL;
    if (!b) {
        PathLog("%8lu unit %p blocked by object %d (none)", GetTickCount(), unit, idx);
        return;
    }
    PathLog("%8lu unit %p (face %d, wait %u) blocked by %p type %d owner %d state %d face %d pos %d,%d"
            " (x%d)%s",
            GetTickCount(), unit, *(int*)(unit + 0x298), *(uint16_t*)(unit + 0x360), b,
            *(int16_t*)(b + 4), *(int16_t*)(b + 8), *(int16_t*)(b + 0x31A), *(int*)(b + 0x298),
            *(int*)(b + 0x18), *(int*)(b + 0x1C), repeats + 1, yieldNow ? " -> go around" : "");
}

static void InstallPathTrace() {
    static const BYTE expected[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};
    BYTE* fn = (BYTE*)(0x47E540 + g_slide);
    if (memcmp(fn, expected, 6)) {
        Log("perf path trace: unexpected game code at 0x47E540");
        return;
    }
    g_moveTramp = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_moveTramp) return;
    memcpy(g_moveTramp, expected, 6);  // the replaced prologue, then jmp 0x47E546
    g_moveTramp[6] = 0xE9;
    *(int32_t*)(g_moveTramp + 7) = (int32_t)((uintptr_t)fn + 6 - ((uintptr_t)g_moveTramp + 11));
    uintptr_t unused;
    if (Hook(0x47E540, expected, 6, (void*)MoveStep, &unused)) Log("perf path trace on");
}

void InstallPerf() {
    if (!g_perfTrace) return;
    QueryPerformanceFrequency(&g_freq);
    InstallOrderTrace();
    InstallPathTrace();
    g_update = (StateUpdateFn)(0x593F80 + g_slide);
    // call 0x593F80 at 0x4FEE4B (rel32 = 0x593F80 - 0x4FEE50 = 0x95130)
    static const BYTE expected[5] = {0xE8, 0x30, 0x51, 0x09, 0x00};
    if (HookCall(0x4FEE4B, expected, sizeof expected, StateUpdate)) Log("perf-trace on");
    else Log("unexpected game code at 0x4FEE4B (different game version?). perf-trace off.");
}
