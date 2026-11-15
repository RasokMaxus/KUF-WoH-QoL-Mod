// select.cpp - select-construction: buildings under construction can be selected together with
// other buildings (Shift+click and the other add-to-selection clicks) and added to a control
// group with Shift+number.
//
// A building's class word (virtual +0x14, for buildings the word +0xC) is 0x27D8 while it is
// under construction; 0x283C and 0x2A30 are two other special states. 0x523850(obj) answers "is
// a normal building" (false for those three). The add-to-selection modes of the selection
// switch 0x5278AB (0x518B40, 0x518DD0, 0x518FB0) call it twice: for the current selection (a
// special building there means nothing can be added) and for each candidate under the cursor
// (special ones are skipped). select-construction replaces those six calls with one that also
// says yes for 0x27D8. Double-click "all of this type" (0x51BF70) keeps the game's rule.
// Shift+number (append to a group, 0x5239CF) refuses when the group's first member or the
// selection's first member has class 0x27D8 or 0x2A30 (mov ecx, 0x27D8 at 0x523AFB, 0x523B22);
// the 0x27D8 there is changed to 0x7FFF (no class), so only 0x2A30 is still refused.
// A finished building keeps its object id, so it stays in its groups and selection.
// The selection is sent as an order (0x12) and carried out the same way on every PC, so this
// cannot cause a desync.
#include "kufmod.h"

bool g_selectConstruction = true;

typedef bool(__cdecl* NormalFn)(BYTE* obj);
typedef short(__thiscall* ClassFn)(BYTE* obj);
static NormalFn g_normal;

static bool __cdecl NormalOrConstruction(BYTE* obj) {
    ClassFn cls = *(ClassFn*)(*(BYTE**)obj + 0x14);
    if (cls(obj) == 0x27D8) return true;
    return g_normal(obj);
}

void InstallSelectConstruction() {
    if (!g_selectConstruction) return;
    g_normal = (NormalFn)(0x523850 + g_slide);
    static const uintptr_t calls[] = {0x518B9E, 0x518C1A, 0x518E1B, 0x518E8F, 0x518FFB, 0x51907F};
    int ok = 0;
    for (uintptr_t va : calls) {
        BYTE expected[5] = {0xE8};
        int32_t rel = (int32_t)(0x523850 - (va + 5));  // relative, so the same at any base
        memcpy(expected + 1, &rel, 4);
        if (HookCall(va, expected, 5, (void*)NormalOrConstruction)) ok++;
    }
    static const BYTE was[5] = {0xB9, 0xD8, 0x27, 0x00, 0x00}, now[5] = {0xB9, 0xFF, 0x7F, 0x00, 0x00};
    bool groups = Patch(0x523AFB, was, now, 5) && Patch(0x523B22, was, now, 5);
    if (ok != 6 || !groups)
        Log("unexpected game code for select-construction (%d of 6 calls%s; different game version?)",
            ok, groups ? "" : ", Shift+number unchanged");
}
