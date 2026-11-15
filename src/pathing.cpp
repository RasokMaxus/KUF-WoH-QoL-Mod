// pathing.cpp - units walk around blockers instead of waiting (setting unit-avoidance).
//
// Unit movement state machine 0x47E540, state word unit+0x31A. When a unit's next step is
// blocked it goes to state 11, handled by 0x47F760. If another unit is in the way (step result
// 3, branch 0x47F7F8: edi = unit, ebx = blocker's index in the object table [0xB870C0]+0x2C38):
//   - facing roughly opposite ways (|facing difference| - 5 <= 6, 16 directions): head-on,
//     resolved by 0x47ECD0;
//   - otherwise the game decides who yields (0x47F829...: value vt+0xF0, retry counter +0x362,
//     ids). The yielding unit sets "avoid units" (+0x271 = 1) and asks for a new path at once
//     (state 14, code at 0x47F8A6); the other one waits, counter +0x360 up to 50 ticks (1.5 s),
//     then does the same.
// The trace showed the long waits are almost always behind a unit that is not walking at all
// (itself blocked, often by a chicken), so whole columns stood still 1.5 s per unit in line.
//
// With unit-avoidance on, a unit only waits behind one of its own player's units that is walking
// (state 3 turn+step or 4 walk). Behind anything else (a stuck or idle unit, a critter, an enemy
// or ally) it goes around at once. Head-on cases are left to the game. The decision only uses
// game state, so it is the same on every PC (multiplayer needs everyone on the same setting).
// (Tried 2026-11-15: always waiting behind own units going the same way, and in lines at chokes.
// Units then followed each other in one long file and stop-stepped: blocks per moving minute
// 12 -> 47. The game's choice of who goes around is what spreads a group out.)
#include "kufmod.h"

bool g_unitAvoidance = true;
uint32_t g_syncYield;  // for the sync lines (sync.cpp)

static uintptr_t g_blockResume, g_yieldNow, g_objTable;

static int __stdcall Decide(BYTE* unit, int idx) {
    uintptr_t world = *(uintptr_t*)g_objTable;
    BYTE* b = world ? *(BYTE**)(world + 0x2C38 + idx * 4) : NULL;
    bool yieldNow = false;
    if (g_unitAvoidance && b) {
        int d = *(int*)(unit + 0x298) - *(int*)(b + 0x298);
        if (d < 0) d = -d;
        bool headOn = (unsigned)(d - 5) <= 6;
        int16_t state = *(int16_t*)(b + 0x31A);
        bool ownWalking = *(int16_t*)(b + 8) == *(int16_t*)(unit + 8) && (state == 3 || state == 4);
        // Neutral critters (player 10: sheep, chickens) do not take part in the head-on rule
        // (0x47ECD0 expects both to step aside), so units stood facing a sheep while the group
        // jammed up behind them: always go around those.
        bool critter = *(int16_t*)(b + 8) == 10;
        yieldNow = (!headOn || critter) && !ownWalking;
        if (yieldNow) g_syncYield++;
    }
    if (g_perfTrace) NoteBlock(unit, idx, yieldNow);
    return yieldNow;
}

__declspec(naked) static void BlockHook() {
    __asm {
        pushad
        push ebx
        push edi
        call Decide
        test eax, eax
        popad
        jnz yield
        mov eax, g_objTable          // the replaced instruction: mov eax, [0xB870C0]
        mov eax, dword ptr [eax]
        jmp dword ptr [g_blockResume]
    yield:
        jmp dword ptr [g_yieldNow]   // 0x47F8A6: avoid units + new path now (uses edi)
    }
}

// Turning (state 3 of 0x47E540, 0x47EAB8): the unit turns 2 of 16 directions a tick toward its
// path and only steps when within 1 direction. fast-turning: 4 a tick, steps when within 3.
bool g_fastTurning = true;

static void InstallFastTurning() {
    struct P { uint32_t va; BYTE was[3], now[3]; } p[] = {
        {0x47EACB, {0x83, 0xE8, 0x02}, {0x83, 0xE8, 0x04}},  // sub eax, 2 -> 4
        {0x47EACE, {0x83, 0xF8, 0x0B}, {0x83, 0xF8, 0x08}},  // turn when |diff| in 4..12
        {0x47EAE7, {0x8D, 0x42, 0x02}, {0x8D, 0x42, 0x04}},  // +2 -> +4
        {0x47EAF0, {0x8D, 0x42, 0xF2}, {0x8D, 0x42, 0xF4}},  // wrap -14 -> -12
        {0x47EB09, {0x8D, 0x42, 0xFE}, {0x8D, 0x42, 0xFC}},  // -2 -> -4
        {0x47EB10, {0x8D, 0x42, 0x0E}, {0x8D, 0x42, 0x0C}},  // wrap +14 -> +12
    };
    for (const P& x : p)
        if (memcmp((BYTE*)(x.va + g_slide), x.was, 3)) {
            Log("unexpected game code at 0x%X (different game version?). fast-turning off.", x.va);
            return;
        }
    for (const P& x : p) Patch(x.va, x.was, x.now, 3);
    Log("fast-turning on");
}

// Step results 1/2 (0x47D530: blocked, but not "unit with a known index" - e.g. the cells of a
// crowd of standing units) go to 0x47F7D9: state 14 = new path at once, but without "avoid
// units". The new path runs into the same crowd and the unit loops blocked/new path every tick
// (the jitter around a building under attack). With unit-avoidance on, this branch also sets
// +0x271 so the new path goes around units (cleared by the game at the next move, 0x47F5F6).
static uintptr_t g_terrainResume;

__declspec(naked) static void TerrainBlockHook() {
    __asm {
        mov byte ptr [edi + 0x271], 1
        mov eax, 0xE                 // the replaced instruction
        jmp dword ptr [g_terrainResume]
    }
}

// A third way into "new path now": state 11 in 0x47E540 checks the unit's mode word +0x31C; when
// it is 1 the unit skips the blocked handler and goes to state 14 at 0x47E70F (unit in ebx),
// again without "avoid units". The trace showed this loop (blocked/new path every tick, avoid 0)
// for fighting units. Same fix.
static uintptr_t g_modeResume;

__declspec(naked) static void ModeBlockHook() {
    __asm {
        mov byte ptr [ebx + 0x271], 1
        mov eax, 0xE                 // the replaced instruction
        jmp dword ptr [g_modeResume]
    }
}

void InstallPathing() {
    if (!g_unitAvoidance && !g_perfTrace && !g_fastTurning) return;
    g_objTable = 0xB870C0 + g_slide;
    g_yieldNow = 0x47F8A6 + g_slide;
    if (g_fastTurning) InstallFastTurning();
    static const BYTE yieldCode[4] = {0xB8, 0x0E, 0x00, 0x00};  // mov eax, 0xE
    if (memcmp((void*)g_yieldNow, yieldCode, sizeof yieldCode)) {
        Log("unexpected game code at 0x47F8A6 (different game version?). unit-avoidance off.");
        return;
    }
    BYTE expected[5] = {0xA1};  // mov eax, [0xB870C0]
    *(uint32_t*)(expected + 1) = (uint32_t)g_objTable;
    if (!Hook(0x47F7F8, expected, sizeof expected, (void*)BlockHook, &g_blockResume)) {
        Log("unexpected game code at 0x47F7F8 (different game version?). unit-avoidance off.");
        return;
    }
    if (!g_unitAvoidance) return;
    static const BYTE terrainExpected[5] = {0xB8, 0x0E, 0x00, 0x00, 0x00};  // mov eax, 0xE
    if (!Hook(0x47F7D9, terrainExpected, sizeof terrainExpected, (void*)TerrainBlockHook, &g_terrainResume))
        Log("unexpected game code at 0x47F7D9 (different game version?). unit-avoidance partly off.");
    if (!Hook(0x47E70F, terrainExpected, sizeof terrainExpected, (void*)ModeBlockHook, &g_modeResume))
        Log("unexpected game code at 0x47E70F (different game version?). unit-avoidance partly off.");
    Log("unit-avoidance on");
}
