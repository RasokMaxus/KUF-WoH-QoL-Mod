// economy.cpp - the two gameplay changes, applied to the game's code in memory at startup.
//
// Standard match mode (setting standard-mode): the "Gold" mode on the setup screen becomes
// "Standard": every player starts with 0 gold and 4 workers, and it is the default mode for every
// map instead of Casual. Casual and Family are unchanged.
//
// Empty resource nodes (setting empty-nodes): a gold mine, iron vein or mana node at 0 remaining
// keeps paying one third of a normal trip (34 gold, 20 iron, 7 mana) instead of nothing.
#include "kufmod.h"
#include <string.h>

// ---------------------------------------------------------------- Standard mode
// Match setup, per player (ebx = game, edi = player index, ebp = setup's stack frame):
//   0x4C865C  cmp byte [game+0xC7B],0 / je skip   - mode byte 0 (Gold) skips the free workers
//   0x4C8754  cmp byte [game+0xC7E],0             - first instruction after the workers
// The worker count lives in [ebp-0x3D8]. Spawning workers charges and refunds gold in a way that
// leaves too little gold for more than two workers, so gold is set to 0 after spawning instead.
static uintptr_t g_spawnWorkers, g_afterWorkers;

__declspec(naked) static void StandardWorkers() {  // replaces 13 bytes at 0x4C865C
    __asm {
        cmp byte ptr [ebx + 0xC7B], 0
        jne spawn
        mov dword ptr [ebp - 0x3D8], 4  // Standard: 4 workers
    spawn:
        jmp dword ptr [g_spawnWorkers]   // 0x4C8669: the worker spawn code
    }
}

__declspec(naked) static void StandardGold() {  // replaces 7 bytes at 0x4C8754
    __asm {
        cmp byte ptr [ebx + 0xC7B], 0
        jne keep
        mov eax, [ebx + edi * 4 + 0x48]
        mov dword ptr [eax + 0xA0C], 0  // Standard: 0 gold
    keep:
        cmp byte ptr [ebx + 0xC7E], 0     // the instruction we replaced
        jmp dword ptr [g_afterWorkers]    // 0x4C875B (flags kept for its je)
    }
}

// The setup screens start every map on Casual: 0x432230 and 0x432270 write mode 1 and copy mode
// 1's two settings bytes from the mode tables (0xB33AC1, 0xB33AC5). We make them start on mode 0
// (Standard) and read its entries (0xB33AC0, 0xB33AC4) instead. The table addresses are relocated
// by Windows, so the expected bytes include g_slide.
static bool DefaultToStandard() {
    struct Read { uintptr_t va, table; };
    const Read reads[4] = {{0x432230, 0xB33AC1}, {0x43223D, 0xB33AC5}, {0x432273, 0xB33AC1}, {0x432287, 0xB33AC5}};
    const uintptr_t modes[2] = {0x432244, 0x432296};
    static const BYTE mode1[3] = {0xC6, 0x06, 0x01}, mode0[3] = {0xC6, 0x06, 0x00};
    BYTE before[4][7], after[4][7];
    for (int i = 0; i < 4; i++) {  // movzx eax, byte ptr [table]
        uint32_t from = (uint32_t)(reads[i].table + g_slide), to = from - 1;
        BYTE op[3] = {0x0F, 0xB6, 0x05};
        memcpy(before[i], op, 3);
        memcpy(before[i] + 3, &from, 4);
        memcpy(after[i], op, 3);
        memcpy(after[i] + 3, &to, 4);
        if (memcmp((void*)(reads[i].va + g_slide), before[i], 7)) return false;
    }
    for (uintptr_t va : modes)
        if (memcmp((void*)(va + g_slide), mode1, 3)) return false;
    for (int i = 0; i < 4; i++) Patch(reads[i].va, before[i], after[i], 7);
    for (uintptr_t va : modes) Patch(va, mode1, mode0, 3);
    return true;
}

static bool InstallStandardMode() {
    if (!DefaultToStandard()) Log("standard mode: could not make it the default mode");
    static const BYTE gate[13] = {0x80, 0xBB, 0x7B, 0x0C, 0x00, 0x00, 0x00, 0x0F, 0x84, 0xEB, 0x00, 0x00, 0x00};
    static const BYTE post[7] = {0x80, 0xBB, 0x7E, 0x0C, 0x00, 0x00, 0x00};
    if (!Hook(0x4C865C, gate, sizeof gate, StandardWorkers, &g_spawnWorkers)) return false;
    if (!Hook(0x4C8754, post, sizeof post, StandardGold, &g_afterWorkers)) return false;
    // The mode's name: "Gold" in an 8-byte slot, appended with an explicit length of 4.
    bool ok = Patch(0xA1D848, "Gold\0\0\0\0", "Standard", 8);
    static const BYTE len4[2] = {0x6A, 0x04}, len8[2] = {0x6A, 0x08};
    ok = Patch(0x4320A8, len4, len8, 2) && ok;
    ok = Patch(0xA52CBD, "Gold/Casual/Family Match Mode", "Standard/Casual/Family Mode  ", 29) && ok;
    return ok;
}

// ---------------------------------------------------------------- empty nodes
// Each harvest function (gold 0x529D40, mana 0x529E00, iron 0x529EE0) ends a trip with
//   cmp edi,esi / cmovg edi,esi / sub [ebx+0x14F8],edi      (edi = amount, esi = remaining)
// We call HarvestAmount instead: with something left it does exactly that; at 0 it pays a third
// of the amount, rounded up, and leaves the node at 0. Workers also give up on a node that
// reports 0 remaining at 0x544F34 and 0x5455A1 (jle); those jumps are removed.
__declspec(naked) static void HarvestAmount() {
    __asm {
        test esi, esi
        jg rich
        lea eax, [edi + 2]
        cdq
        mov ecx, 3
        idiv ecx
        mov edi, eax
        ret
    rich:
        cmp edi, esi
        cmovg edi, esi
        sub dword ptr [ebx + 0x14F8], edi
        ret
    }
}

static bool InstallEmptyNodes() {
    static const BYTE clamp[11] = {0x3B, 0xFE, 0x0F, 0x4F, 0xFE, 0x29, 0xBB, 0xF8, 0x14, 0x00, 0x00};
    static const BYTE gate1[6] = {0x0F, 0x8E, 0xC4, 0x00, 0x00, 0x00}, gate2[6] = {0x0F, 0x8E, 0x4C, 0x01, 0x00, 0x00};
    static const BYTE nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    // Check every site before changing any, so a different game version is left untouched.
    const uintptr_t sites[3] = {0x529D5B, 0x529E30, 0x529F14};
    for (uintptr_t va : sites)
        if (memcmp((void*)(va + g_slide), clamp, sizeof clamp)) return false;
    if (memcmp((void*)(0x544F34 + g_slide), gate1, 6) || memcmp((void*)(0x5455A1 + g_slide), gate2, 6)) return false;
    for (uintptr_t va : sites) HookCall(va, clamp, sizeof clamp, HarvestAmount);
    Patch(0x544F34, gate1, nops, 6);
    Patch(0x5455A1, gate2, nops, 6);
    return true;
}

void InstallEconomy() {
    if (g_standardMode)
        Log(InstallStandardMode() ? "standard mode on" : "standard mode: unexpected game code, left off");
    if (g_emptyNodes)
        Log(InstallEmptyNodes() ? "empty nodes on" : "empty nodes: unexpected game code, left off");
}
