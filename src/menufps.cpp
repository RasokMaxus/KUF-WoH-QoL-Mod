// menufps.cpp - smooth main menu (setting menu-fps, default 60; 0 = game's own 20 fps).
//
// The animated menus (state 19 of the main frame's state update 0x593F80: the main menu, the
// Single/Multi Play choice and the other menus built by 0x5939D0) run their frame through one
// case at 0x595C97:
//     menu->Update()          [0xBED218] vtable +0x14 (0x721B40: frame counter menu+0x2B4,
//                             animated sprites, timed sounds/events - all counted in frames)
//     if timer 0xBED7CC is on time: Render() ([0xB82808] vtable +0xC, also draws the game's own
//                             mouse cursor), then busy-wait until 1000/20 = 50 ms have passed
// The timer's rate is 20 (static constructor at 0x401C20), so the menu - and its cursor - is drawn
// 20 times per second, which makes the mouse feel slow. Raising the rate alone would also run the
// menu animations 3x faster, because they count frames.
//
// MenuFrame below replaces that case: Update() still runs every 50 ms (so the menu animates as
// before), but Render() runs menu-fps times per second, and the wait sleeps instead of spinning.
#include "kufmod.h"
#include <mmsystem.h>

int g_menuFps = 60;

static uintptr_t g_menuExit;  // 0x59800C: the state update's common tail (expects esi = frame)
static LARGE_INTEGER g_freq;
static double g_nextTick, g_nextFrame;

typedef void(__thiscall* VoidMethod)(void* self);

static double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / g_freq.QuadPart;
}

static void __stdcall MenuFrame() {
    const double tick = 50.0, frame = 1000.0 / g_menuFps;
    double now = Now();
    if (now >= g_nextTick) {
        void* menu = Game<void*>(0xBED218);
        if (menu) (*(VoidMethod**)menu)[0x14 / 4](menu);
        g_nextTick += tick;
        if (g_nextTick <= now) g_nextTick = now + tick;  // late (or first frame): no catch-up burst
    }
    if (Game<char>(0xB41727)) {
        void* gfx = Game<void*>(0xB82808);
        if (gfx) (*(VoidMethod**)gfx)[0xC / 4](gfx);
    }
    g_nextFrame += frame;
    now = Now();
    if (g_nextFrame <= now) {
        g_nextFrame = now;  // drawing took longer than a frame: carry on without waiting
        return;
    }
    while (g_nextFrame - now > 2.0) {
        Sleep(1);
        now = Now();
    }
    while (Now() < g_nextFrame) YieldProcessor();
}

__declspec(naked) static void MenuFrameHook() {
    __asm {
        call MenuFrame
        jmp dword ptr [g_menuExit]
    }
}

void InstallMenuFps() {
    if (g_menuFps <= 0) return;
    if (g_menuFps < 20) g_menuFps = 20;
    if (g_menuFps > 240) g_menuFps = 240;
    QueryPerformanceFrequency(&g_freq);
    g_menuExit = 0x59800C + g_slide;
    // mov ecx,[0xBED218] / mov eax,[ecx] / call [eax+0x14]
    static const BYTE expected[11] = {0x8B, 0x0D, 0x18, 0xD2, 0xBE, 0x00, 0x8B, 0x01, 0xFF, 0x50, 0x14};
    BYTE want[11];
    memcpy(want, expected, sizeof want);
    *(uint32_t*)(want + 2) = 0xBED218 + (uint32_t)g_slide;  // absolute address is relocated
    uintptr_t unused;
    if (!Hook(0x595C97, want, sizeof want, MenuFrameHook, &unused)) {
        Log("unexpected game code at 0x595C97 (different game version?). menu-fps off.");
        return;
    }
    timeBeginPeriod(1);  // Sleep(1) = about 1 ms instead of up to 15.6 ms
    Log("menu-fps %d", g_menuFps);
}
