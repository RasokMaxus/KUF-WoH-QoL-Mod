// combat.cpp - target priority and sticking to a target (setting target-priority).
//
// The unit attack routine 0x4850C0 (unit vtable +0x18C; target +0x120, sub-state +0x31C) re-checks
// its target every 60 ticks (counter +0x37C, about 1.8 s) while attacking:
//     range = vt+0x74 (0x516AD0: type data +0x278); new = 0x496880(range, 1)
//     0x485414: if new && new != target -> switch to new
// 0x496880 goes through the enemies in range (0x4E7180 / 0x4E7340), skips invalid ones (0x48A390)
// and scores each with vt+0x210 (0x489D70): the walking path length for up to 4 enemies per pass,
// otherwise 2000 (unit) / 4000 (building) + 1500 for workers; the lowest score wins.
// So units switch to whatever is a little closer every 1.8 s, with no idea of threat.
//
// target-priority adds a tier to every score (tier * 100000, so a better tier always wins and the
// game's own order decides within a tier):
//   0 enemy units that can attack (type data +0x17C ground or +0x18C air attack > 0)
//   1 workers (types 0 Human, 15 Devil) and units that cannot attack
//   2 buildings that can attack (towers)
//   3 other buildings and anything else
// and a unit keeps its current target unless the new one is in a better tier or the current one
// has left its search range; a unit already attacking a tower keeps it until it falls. Orders the player gives (attack this) are not affected: they set the
// target directly. Everything here uses game state only, so it is the same on every PC.
#include "kufmod.h"
#include <limits.h>
#include <string.h>

bool g_targetPriority = true;

typedef int(__cdecl* DistFn)(void* a, void* b);  // 0x4E4E80: distance in the units 0x496880 uses
static DistFn g_dist;
static uintptr_t g_typeTable;                      // [0xB870F4]: 0xAF8 bytes per object type

static int Tier(BYTE* o) {
    if (!o) return 3;
    int16_t kind = *(int16_t*)(o + 6), type = *(int16_t*)(o + 4);
    BYTE* data = *(BYTE**)g_typeTable;
    int attack = 0;
    if (data && type >= 0) {
        BYTE* t = data + type * 0xAF8;
        attack = *(int*)(t + 0x17C) + *(int*)(t + 0x18C);
    }
    if (kind == 0) {
        if (type == 0 || type == 15) return 1;  // workers
        return attack > 0 ? 0 : 1;
    }
    if (kind == 1) return attack > 0 ? 2 : 3;
    return 3;
}

// ---- priority: 0x49691D, right after score = vt+0x210(candidate): cmp eax,-1 / je 0x496937
// (esi = candidate, edi = the unit choosing)
static uintptr_t g_scoreResume, g_scoreSkip;

static int __stdcall AddTier(int score, BYTE* candidate) {
    return score + Tier(candidate) * 100000;
}

__declspec(naked) static void ScoreHook() {
    __asm {
        cmp eax, -1
        je skip
        push ecx
        push edx
        push esi
        push eax
        call AddTier
        pop edx
        pop ecx
        jmp dword ptr [g_scoreResume]   // 0x496922: cmp eax, ebx ...
    skip:
        jmp dword ptr [g_scoreSkip]     // 0x496937
    }
}

// ---- sticking: the chooser 0x496880 (thiscall unit, range, flag; ret 8; returns the best enemy
// or 0) has 18 callers (attack routines, idle/guard acquire, group code), each switching when the
// result differs from the current target +0x120. The wrapper returns the current target instead
// when the unit should keep it, so no caller switches. Prologue push ebp / mov ebp,esp /
// sub esp,8 (6 bytes) is moved to a trampoline.
typedef BYTE*(__thiscall* ChooserFn)(void* unit, int range, int flag);
static BYTE* g_chooserTramp;

static BYTE* ChooseTarget(BYTE* unit, BYTE* proposed) {
    BYTE* current = *(BYTE**)(unit + 0x120);
    if (!proposed || !current || proposed == current) return proposed;
    typedef int(__thiscall * RangeFn)(void*);
    int range = (*(RangeFn**)unit)[0x74 / 4](unit);
    if (g_dist(unit, current) > range) return proposed;  // current target got away
    // A unit already on a defensive building (tower) stays on it until it falls: arriving
    // enemy fighters do not pull it away.
    if (Tier(current) == 2) return current;
    if (Tier(proposed) < Tier(current)) return proposed;
    return current;                                       // keep the current target
}

static BYTE* __fastcall Chooser(BYTE* unit, void*, int range, int flag) {
    BYTE* proposed = ((ChooserFn)(void*)g_chooserTramp)(unit, range, flag);
    return ChooseTarget(unit, proposed);
}

// ---- chasing: the attack routine drops its target (+0x120 = 0, back to the previous order, e.g.
// the attack-move march) when dist(unit, target) > leash, leash = max(2 * type data +0x174,
// target reach + 1, own search range + 1). Checked at 0x4855D9 and 0x486F39, both:
//   add esp,8 / cmp eax,esi / jle keep   (eax = distance, esi = leash, ebx = unit)
// A melee unit whose target steps back a little let go and marched off toward the attack-move
// point, then came back (the "walk away diagonally, then return"). target-priority makes the leash
// at least twice the unit's search range, so units chase like they do in Brood War.
void PerfNote(const char* fmt, ...);  // perf.cpp (writes to pathtrace.txt when perf-trace is on)
extern bool g_perfTrace;
static uintptr_t g_leashKeep1, g_leashDrop1, g_leashKeep2, g_leashDrop2;

static int __stdcall Leash(BYTE* unit, int leash, int dist) {
    typedef int(__thiscall * RangeFn)(void*);
    int range = (*(RangeFn**)unit)[0x74 / 4](unit);
    int wide = range * 2 + 1;
    int result = leash > wide ? leash : wide;
    if (g_perfTrace && dist > leash)
        PerfNote("%8lu unit %p target %p at distance %d: game leash %d, search range %d -> %s",
                 GetTickCount(), unit, *(void**)(unit + 0x120), dist, leash, range,
                 dist > result ? "dropped" : "kept chasing");
    return result;
}

#define LEASH_HOOK(name, keep, drop)                        \
    __declspec(naked) static void name() {                  \
        __asm add esp, 8                                     \
        __asm push eax                                       \
        __asm push ecx                                       \
        __asm push edx                                       \
        __asm push eax                                       \
        __asm push esi                                       \
        __asm push ebx                                       \
        __asm call Leash                                     \
        __asm mov esi, eax                                   \
        __asm pop edx                                        \
        __asm pop ecx                                        \
        __asm pop eax                                        \
        __asm cmp eax, esi                                   \
        __asm jle k                                          \
        __asm jmp dword ptr [drop]                           \
        __asm k: jmp dword ptr [keep]                        \
    }
LEASH_HOOK(LeashHook1, g_leashKeep1, g_leashDrop1)
LEASH_HOOK(LeashHook2, g_leashKeep2, g_leashDrop2)

// ---- spots around buildings: the attack routine (case 0, building target) picks where to stand
// with 0x48DDA0 (thiscall search, int* x, int* y, towardX, towardY, radius 15; ret 0x14; al =
// found), called at 0x4852FE with x/y = the building's center cell and toward = the unit's cell.
// It walks a straight line from the center toward the unit and takes the first free cell, so all
// units coming from one side stack up on that side, and later ones stand farther out on the same
// line, out of reach. The wrapper runs the same search toward 8 directions (the unit's side first,
// then the neighbouring sides), each with a fresh copy of the search state, and takes the result
// closest to the building; ties go to the side nearest the unit. The game's own occupancy rules
// decide what is free.
typedef char(__thiscall* SpotFn)(void* search, int* x, int* y, int tx, int ty, int radius);
static SpotFn g_spot;
static const size_t SEARCH_SIZE = 0xB8;  // the local search object [ebp-0xBC .. ebp-0x8]

static char __fastcall BuildingSpot(void* search, void*, int* x, int* y, int tx, int ty, int radius) {
    BYTE pristine[SEARCH_SIZE];
    memcpy(pristine, search, SEARCH_SIZE);
    const int cx = *x, cy = *y;
    static const int DIRS[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    int order[8], score[8];
    int ux = tx - cx, uy = ty - cy;
    for (int i = 0; i < 8; i++) {
        order[i] = i;
        score[i] = DIRS[i][0] * ux + DIRS[i][1] * uy;  // larger = closer to the unit's side
    }
    for (int i = 1; i < 8; i++)  // insertion sort, best side first (stable: deterministic)
        for (int j = i; j > 0 && score[order[j]] > score[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    int bestX = 0, bestY = 0, bestDist = INT_MAX;
    BYTE bestState[SEARCH_SIZE];
    for (int k = 0; k < 8; k++) {
        const int* d = DIRS[order[k]];
        int sx = cx, sy = cy;
        memcpy(search, pristine, SEARCH_SIZE);
        if (!g_spot(search, &sx, &sy, cx + d[0] * 20, cy + d[1] * 20, radius)) continue;
        int ddx = sx - cx, ddy = sy - cy;
        int dist = (ddx < 0 ? -ddx : ddx) > (ddy < 0 ? -ddy : ddy) ? (ddx < 0 ? -ddx : ddx) : (ddy < 0 ? -ddy : ddy);
        if (dist < bestDist) {
            bestDist = dist; bestX = sx; bestY = sy;
            memcpy(bestState, search, SEARCH_SIZE);
        }
    }
    if (bestDist == INT_MAX) {  // nothing found in any direction: the game's own search
        memcpy(search, pristine, SEARCH_SIZE);
        return g_spot(search, x, y, tx, ty, radius);
    }
    memcpy(search, bestState, SEARCH_SIZE);
    *x = bestX;
    *y = bestY;
    return 1;
}

void InstallCombat() {
    if (!g_targetPriority) return;
    g_dist = (DistFn)(0x4E4E80 + g_slide);
    g_typeTable = 0xB870F4 + g_slide;
    static const BYTE scoreExpected[5] = {0x83, 0xF8, 0xFF, 0x74, 0x15};
    g_scoreSkip = 0x496937 + g_slide;
    if (!Hook(0x49691D, scoreExpected, sizeof scoreExpected, (void*)ScoreHook, &g_scoreResume)) {
        Log("unexpected game code at 0x49691D (different game version?). target-priority off.");
        return;
    }
    uintptr_t unused;
    static const BYTE chooserExpected[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    BYTE* chooser = (BYTE*)(0x496880 + g_slide);
    g_chooserTramp = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (g_chooserTramp && !memcmp(chooser, chooserExpected, 6)) {
        memcpy(g_chooserTramp, chooserExpected, 6);  // the replaced prologue, then jmp 0x496886
        g_chooserTramp[6] = 0xE9;
        *(int32_t*)(g_chooserTramp + 7) = (int32_t)((uintptr_t)chooser + 6 - ((uintptr_t)g_chooserTramp + 11));
        if (!Hook(0x496880, chooserExpected, 6, (void*)Chooser, &unused))
            Log("could not hook 0x496880. target sticking off.");
    } else {
        Log("unexpected game code at 0x496880 (different game version?). target sticking off.");
    }
    static const BYTE leashExpected[7] = {0x83, 0xC4, 0x08, 0x3B, 0xC6, 0x7E, 0x11};
    g_leashDrop1 = 0x4855E0 + g_slide;  // after the 7 bytes: the drop
    g_leashKeep1 = 0x4855F1 + g_slide;  // the jle target
    g_leashDrop2 = 0x486F40 + g_slide;
    g_leashKeep2 = 0x486F51 + g_slide;
    if (!Hook(0x4855D9, leashExpected, sizeof leashExpected, (void*)LeashHook1, &unused) ||
        !Hook(0x486F39, leashExpected, sizeof leashExpected, (void*)LeashHook2, &unused))
        Log("unexpected game code at 0x4855D9/0x486F39 (different game version?). chasing unchanged.");
    g_spot = (SpotFn)(0x48DDA0 + g_slide);
    static const BYTE spotCall[5] = {0xE8, 0x9D, 0x8A, 0x00, 0x00};  // call 0x48DDA0
    if (!HookCall(0x4852FE, spotCall, sizeof spotCall, (void*)BuildingSpot))
        Log("unexpected game code at 0x4852FE (different game version?). building spots unchanged.");
    Log("target-priority on");
}
