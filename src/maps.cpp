// maps.cpp - "Random" maps that are actually random (setting random-maps).
//
// The map lists' "Random (N players)" entries come from res\Map\Random\N+.def ("<title>", then
// "min,max" players, then optional map names). The loader 0x6F2070 fills the entry's candidate
// list with every map that has min <= players (+0x4C) <= max ... but skips maps whose folder word
// +0x5A is 1 (res\Map\Custom) at 0x6F2730. On the Steam install almost every official map lives in
// Custom, so only the six community maps that exist only in res\Map qualified; for 4 players just
// (4)GroundZero1.2 and (4)Last wish(beta 1.1).
// The pickers (0x6DE631, 0x6EC412, 0x6EC869: rand() % candidates) use the C runtime's rand(), which
// the game only ever seeds with the fixed value 999 (0x4A2869, 0x4A3A69), so the picks repeat.
//
// random-maps:
// - Custom-folder maps take part when the player count in their name ("(4)Ragnarok") is within
//   min..max. Their +0x4C is not a reliable player count (letting them in unchecked gave a 2-player
//   map with 4 players); names without a number, e.g. "(Custom)Lich May Cry", stay out.
//   Hook at 0x6F2730 (cmp word [eax+0x5A],1 / je skip; eax = map record, name at +4,
//   min [ebp-0x26C], max [ebp-0x274]).
// - The three pickers get a real random number (rand_s). The pick is made where the room or match
//   settings are made, and the chosen map's name is what gets sent to the other players.
#define _CRT_RAND_S
#include <stdlib.h>
#include <string.h>
#include "kufmod.h"

bool g_randomMaps = true;

static int __cdecl MapRandom() {
    unsigned int r = 0;
    if (rand_s(&r)) r = GetTickCount() * 2654435761u;
    return (int)(r >> 1);
}

// Players from a name like "(4)Ragnarok" (any folder prefix before it is skipped); 0 if none.
static int PlayersInName(const char* name) {
    const char* p = name;
    for (const char* s = name; *s; s++)
        if (*s == '\\' || *s == '/') p = s + 1;
    if (*p != '(' || p[1] < '1' || p[1] > '9') return 0;
    int n = 0;
    for (p++; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0');
    return *p == ')' ? n : 0;
}

static uintptr_t g_customGo, g_customSkip;

static int __stdcall CustomMapOk(BYTE* record, int minPlayers, int maxPlayers) {
    int n = PlayersInName((const char*)(record + 4));
    bool ok = n >= minPlayers && n <= maxPlayers;
    static int logged;
    if (logged < 12) {
        logged++;
        Log("random-maps: custom map \"%.60s\" (%d players) for %d-%d: %s", (const char*)(record + 4), n,
            minPlayers, maxPlayers, ok ? "yes" : "no");
    }
    return ok;
}

__declspec(naked) static void CustomMapHook() {
    __asm {
        cmp word ptr [eax + 0x5A], 1
        jne go                          // not a Custom-folder map: as before
        pushad
        push dword ptr [ebp - 0x274]    // max
        push dword ptr [ebp - 0x26C]    // min
        push eax                        // map record
        call CustomMapOk
        test eax, eax
        popad
        jz skip
    go:
        jmp dword ptr [g_customGo]      // 0x6F273B
    skip:
        jmp dword ptr [g_customSkip]    // 0x6F2AB8
    }
}

// ---- melee-default: when a map is chosen (0x6EC4E0, skirmish screen and multiplayer room), the
// game type box (control 0x158) is set to "Use Map Settings" if the map comes from the Custom
// folder (+0x5A == 1) and 0x6F0C40 finds scenario data in its file, else back to Melee. On the
// Steam install the official maps also live in Custom, and some (Simple2, Open Field) start on
// UMS, which gives a black screen. With melee-default only maps with "(Custom)" in the name start
// on UMS. Hook at 0x6EC5A7: cmp word [edi+0x5A],1 / jne 0x6EC5C3 (edi = map record, name at +4;
// al = the 0x6F0C40 result, used at 0x6EC5AE).
bool g_meleeDefault = true;
static uintptr_t g_umsCheck, g_umsMelee;

static int __stdcall UmsAllowed(BYTE* record) {
    return *(int16_t*)(record + 0x5A) == 1 && strstr((const char*)(record + 4), "(Custom)") != NULL;
}

__declspec(naked) static void MeleeDefaultHook() {
    __asm {
        pushad
        push edi
        call UmsAllowed
        test eax, eax
        popad
        jz melee
        jmp dword ptr [g_umsCheck]      // 0x6EC5AE: test al,al / UMS if the map has scenario data
    melee:
        jmp dword ptr [g_umsMelee]      // 0x6EC5C3: Melee
    }
}

static void InstallMeleeDefault() {
    if (!g_meleeDefault) return;
    static const BYTE expected[7] = {0x66, 0x83, 0x7F, 0x5A, 0x01, 0x75, 0x15};
    g_umsMelee = 0x6EC5C3 + g_slide;
    if (Hook(0x6EC5A7, expected, sizeof expected, (void*)MeleeDefaultHook, &g_umsCheck))
        Log("melee-default on");
    else
        Log("unexpected game code at 0x6EC5A7 (different game version?). melee-default off.");
}

void InstallRandomMaps() {
    InstallMeleeDefault();
    if (!g_randomMaps) return;
    // cmp word ptr [eax+0x5A],1 / je 0x6F2AB8
    static const BYTE expected[11] = {0x66, 0x83, 0x78, 0x5A, 0x01, 0x0F, 0x84, 0x7D, 0x03, 0x00, 0x00};
    g_customSkip = 0x6F2AB8 + g_slide;
    bool ok = Hook(0x6F2730, expected, sizeof expected, (void*)CustomMapHook, &g_customGo);
    static const uintptr_t sites[3] = {0x6DE631, 0x6EC412, 0x6EC869};
    for (uintptr_t site : sites) {
        BYTE call[5] = {0xE8};  // call rand (0x9822F8)
        *(int32_t*)(call + 1) = (int32_t)(0x9822F8 - (site + 5));
        ok = HookCall(site, call, sizeof call, (void*)MapRandom) && ok;
    }
    if (ok) Log("random-maps on");
    else Log("unexpected game code for the random map pick (different game version?). random-maps partly off.");
}
