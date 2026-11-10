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
// has left its search range. Orders the player gives (attack this) are not affected: they set the
// target directly. Everything here uses game state only, so it is the same on every PC.
#include "kufmod.h"

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

// ---- sticking: 0x485414 in the attack routine: test eax,eax / je 0x485443 /
// cmp [ebx+0x120],eax / (je 0x485443 follows). ebx = unit, eax = proposed new target.
static uintptr_t g_keepResume, g_keepSkip;

static BYTE* __stdcall ChooseTarget(BYTE* unit, BYTE* proposed) {
    BYTE* current = *(BYTE**)(unit + 0x120);
    if (!proposed || !current || proposed == current) return proposed;
    if (Tier(proposed) < Tier(current)) return proposed;
    typedef int(__thiscall * RangeFn)(void*);
    int range = (*(RangeFn**)unit)[0x74 / 4](unit);
    if (g_dist(unit, current) > range) return proposed;  // current target got away
    return NULL;                                          // keep the current target
}

__declspec(naked) static void KeepHook() {
    __asm {
        push ecx
        push edx
        push eax
        push ebx
        call ChooseTarget
        pop edx
        pop ecx
        test eax, eax
        je keep
        cmp dword ptr [ebx + 0x120], eax
        jmp dword ptr [g_keepResume]    // 0x48541E: je 0x485443 (flags from the cmp)
    keep:
        jmp dword ptr [g_keepSkip]      // 0x485443
    }
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

void InstallCombat() {
    if (!g_targetPriority) return;
    g_dist = (DistFn)(0x4E4E80 + g_slide);
    g_typeTable = 0xB870F4 + g_slide;
    static const BYTE scoreExpected[5] = {0x83, 0xF8, 0xFF, 0x74, 0x15};
    static const BYTE keepExpected[10] = {0x85, 0xC0, 0x74, 0x2B, 0x39, 0x83, 0x20, 0x01, 0x00, 0x00};
    BYTE* keepNext = (BYTE*)(0x48541E + g_slide);
    if (keepNext[0] != 0x74) {  // je 0x485443 must follow
        Log("unexpected game code at 0x48541E (different game version?). target-priority off.");
        return;
    }
    g_scoreSkip = 0x496937 + g_slide;
    g_keepSkip = 0x485443 + g_slide;
    if (!Hook(0x49691D, scoreExpected, sizeof scoreExpected, (void*)ScoreHook, &g_scoreResume)) {
        Log("unexpected game code at 0x49691D (different game version?). target-priority off.");
        return;
    }
    if (!Hook(0x485414, keepExpected, sizeof keepExpected, (void*)KeepHook, &g_keepResume))
        Log("unexpected game code at 0x485414 (different game version?). target sticking off.");
    static const BYTE leashExpected[7] = {0x83, 0xC4, 0x08, 0x3B, 0xC6, 0x7E, 0x11};
    uintptr_t unused;
    g_leashDrop1 = 0x4855E0 + g_slide;  // after the 7 bytes: the drop
    g_leashKeep1 = 0x4855F1 + g_slide;  // the jle target
    g_leashDrop2 = 0x486F40 + g_slide;
    g_leashKeep2 = 0x486F51 + g_slide;
    if (!Hook(0x4855D9, leashExpected, sizeof leashExpected, (void*)LeashHook1, &unused) ||
        !Hook(0x486F39, leashExpected, sizeof leashExpected, (void*)LeashHook2, &unused))
        Log("unexpected game code at 0x4855D9/0x486F39 (different game version?). chasing unchanged.");
    Log("target-priority on");
}
