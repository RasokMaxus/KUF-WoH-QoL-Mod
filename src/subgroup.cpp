// subgroup.cpp - tab-subgroups: a selection of several building or unit types shows the buttons
// of one type at a time, and Tab switches to the next type. Plus the production-order check.
//
// The button panel (KufMenu, player+0x5E0) keeps its own copy of the selection: word count
// menu+0x1898 and a 16-id buffer at [menu+0x189C], rewritten by the game when the selection
// changes (0x500E76, 0x537893); the portraits / single-object info are drawn from it too.
// menu+0x18A4 (from player+0x5F0, set by 0x5284C0) is the selection's common type, 10000 when
// types are mixed; menu+0x18A0 is the selected hero (-1 none). The grid fill (0x50EFF0) only
// builds the 3x3 grid (train/build) for one type; for mixed types the six command buttons show
// only commands common to all types and the grid stays empty. Orders go to the real selection,
// player+0x1084/+0x1086 (count/ids, in the game's sorted order).
// A subgroup = selected objects with the same kind (+6) and type (+4) and the same "under
// construction" state (class +0xC == 0x27D8), in order of first appearance in the selection.
// During each fill (FillWrapper in kufmod.cpp) the panel's copy and +0x18A4 are set to the active
// subgroup; the copy is restored right after (the portraits show the whole selection), +0x18A4
// keeps the subgroup's type. The panel's selection check 0x512A00 (every frame, then a refill
// from 0x512D6D / 0x505833) sets the grid page +0x6A4 to -1 for the mixed list, and with page -1
// the fill builds no grid, so the cut fill uses the last real page (0 after Tab or a new
// selection). Hero selections keep the game's own panel.
// Tab: the player's key poll (0x523970) handles Tab (key state player+0x628, latch byte +0x565)
// at 0x52421E: it selects every building of the next type (type list 0xBEB2F0 / 0xBEB380, the
// last one in player+0x57C, 0x51EF50 selects). With two or more subgroups in the selection the
// mod handles Tab instead: the next subgroup becomes active and the panel is refilled.
// All of that is local (no order), so it cannot cause a desync.
//
// Order checks (always on, gameplay; the game never made such mixed selections, so unmodded
// play is unchanged; both PCs need this build):
//  - Train: a train button sends order 0x190/0x191 (value = unit type); the player's handler
//    0x527AE0 passes it as 0x3FC/0x3FD to 0x519EF0, whose picker (0x51A160, loop at 0x51A300)
//    takes the least busy selected building that is not in a special state - without checking
//    that it can train the unit (a Fortress took Orc Fighter orders and produced nothing).
//    The picker now skips buildings without that train command.
//  - Build: placing a building (order 0x1B8, 0x527F7B) calls 0x4C1A80 with the first selected
//    unit as the builder; with a Dark Elf first, nothing was built. It now gets the first
//    selected unit that can build that building. Normal buildings take the other branch
//    (0x527F47): order 0x1B8 to every selected unit via 0x51AD60, where now only units that can
//    build that building keep it.
#include "kufmod.h"

bool g_tabSubgroups = true;
static uintptr_t g_tabResume, g_tabDone, g_pickerResume, g_pickerSkip, g_builderResume,
    g_orderResume;
static int g_active;              // index of the active subgroup
static uint32_t g_activeSig;      // selection the index belongs to
static bool g_cut;                // the panel's copy is cut during this fill
static int16_t g_savedIds[16], g_savedCount;
static int32_t g_page;            // the grid page last used for this selection/subgroup

static BYTE* Obj(int id) {
    BYTE* objects = Game<BYTE*>(0xB870C0);
    return objects && id >= 0 ? *(BYTE**)(objects + 0x2C38 + id * 4) : NULL;
}

typedef short(__thiscall* ClassFn)(BYTE* obj);
static bool Building(BYTE* o) { return *(int16_t*)(o + 6) == 1; }
static bool UnderConstruction(BYTE* o) {
    return Building(o) && (*(ClassFn*)(*(BYTE**)o + 0x14))(o) == 0x27D8;
}

static uint32_t Key(BYTE* o) {
    return (uint32_t)UnderConstruction(o) << 31 | (uint32_t)*(uint16_t*)(o + 6) << 16 |
           *(uint16_t*)(o + 4);
}

// The real selection's ids (valid ones) and its subgroup keys in order.
static int Selection(BYTE* player, int16_t* ids, uint32_t* keys, int* nKeys) {
    int count = *(int16_t*)(player + 0x1084);
    const int16_t* list = (const int16_t*)(player + 0x1086);
    int n = 0;
    *nKeys = 0;
    for (int i = 0; i < count && n < 64; i++) {
        BYTE* o = Obj(list[i]);
        if (!o) continue;
        ids[n++] = list[i];
        uint32_t k = Key(o);
        bool have = false;
        for (int j = 0; j < *nKeys && !have; j++) have = keys[j] == k;
        if (!have && *nKeys < 32) keys[(*nKeys)++] = k;
    }
    return n;
}

static uint32_t Signature(const int16_t* ids, int n) {
    uint32_t s = (uint32_t)n * 2654435761u;
    for (int i = 0; i < n; i++) s = s * 31 + (uint16_t)ids[i];
    return s;
}

static BYTE* LocalPlayer() {
    BYTE* game = Game<BYTE*>(0xB8282C);
    return game ? *(BYTE**)(game + 0x48 + 4 * *(int32_t*)(game + 0x664)) : NULL;
}

// Before the game's grid fill: cut the panel's copy to the active subgroup.
void SubgroupBeforeFill(BYTE* menu) {
    g_cut = false;
    if (!g_tabSubgroups) return;
    BYTE* player = LocalPlayer();
    if (!player || *(BYTE**)(player + 0x5E0) != menu) return;
    if (*(int32_t*)(menu + 0x18A0) != -1) return;  // a hero is selected
    int16_t* buffer = *(int16_t**)(menu + 0x189C);
    int menuCount = *(int16_t*)(menu + 0x1898);
    if (!buffer || menuCount < 2 || menuCount > 16) return;  // not a multiple selection
    int16_t ids[64];
    uint32_t keys[32];
    int nKeys;
    int n = Selection(player, ids, keys, &nKeys);
    if (nKeys < 2) return;
    uint32_t sig = Signature(ids, n);
    if (sig != g_activeSig) {
        g_activeSig = sig;
        g_active = 0;
        Log("subgroups: %d selected, %d types", n, nKeys);
        g_page = 0;
    }
    g_active %= nKeys;
    // The panel's selection check (0x512A00, every frame) sets the grid page +0x6A4 to -1 for a
    // mixed list, and the fill then builds no grid. Use the last real page instead.
    int32_t* page = (int32_t*)(menu + 0x6A4);
    if (*page == -1) *page = g_page;
    else g_page = *page;
    g_savedCount = (int16_t)menuCount;
    memcpy(g_savedIds, buffer, sizeof g_savedIds);
    int m = 0;
    for (int i = 0; i < n && m < 16; i++)
        if (Key(Obj(ids[i])) == keys[g_active]) buffer[m++] = ids[i];
    for (int i = m; i < 16; i++) buffer[i] = -1;
    *(int16_t*)(menu + 0x1898) = (int16_t)m;
    *(int32_t*)(menu + 0x18A4) = (int16_t)keys[g_active];  // the one type
    g_cut = true;
}

// After the fill: the panel's copy is the whole selection again.
void SubgroupAfterFill(BYTE* menu) {
    if (!g_cut) return;
    g_cut = false;
    memcpy(*(int16_t**)(menu + 0x189C), g_savedIds, sizeof g_savedIds);
    *(int16_t*)(menu + 0x1898) = g_savedCount;
    // +0x18A4 keeps the subgroup's type: with 10000 (mixed) the panel clears the grid again
    // after the fill. The game writes it anew on every selection change.
}

static int __cdecl OnTab(BYTE* player) {
    if (!g_tabSubgroups) return 0;
    BYTE* menu = *(BYTE**)(player + 0x5E0);
    if (!menu || *(int32_t*)(menu + 0x18A0) != -1) return 0;
    int16_t ids[64];
    uint32_t keys[32];
    int nKeys;
    int n = Selection(player, ids, keys, &nKeys);
    if (nKeys < 2) return 0;
    uint32_t sig = Signature(ids, n);
    if (sig != g_activeSig) { g_activeSig = sig; g_active = 0; }
    g_active = (g_active + 1) % nKeys;
    // Refresh the panel the way a selection change does (0x500E43), no command waiting,
    // first page of the new subgroup's grid.
    g_page = 0;
    *(int32_t*)(menu + 0x6A4) = 0;
    *(int32_t*)(menu + 0x694) = 0;
    *(uint16_t*)(menu + 0x6A8) |= 0xE;
    *(uint32_t*)(menu + 0x2B4) |= 0x1E;
    return 1;
}

// 0x52422B: push edi / push dword [esi+0x57C] (Tab pressed, latch passed; esi = player).
__declspec(naked) static void TabHook() {
    __asm {
        pushad
        push esi
        call OnTab
        add esp, 4
        test eax, eax
        popad
        jnz handled
        push edi
        push dword ptr [esi + 0x57C]
        jmp dword ptr [g_tabResume]
    handled:
        mov byte ptr [esi + 0x565], 0
        jmp dword ptr [g_tabDone]  // 0x5242CA: the function's end
    }
}

// True if the object's type has a command of one of the two kinds with this value (type table
// [0xB870F4] + type*0xAF8: +0xA9C count, +0xAA8 command ids; command [0xB82824] + id*0x200:
// +8 kind, +0xC value).
static bool HasCommand(BYTE* obj, int kindA, int kindB, int value) {
    BYTE* types = Game<BYTE*>(0xB870F4);
    BYTE* commands = Game<BYTE*>(0xB82824);
    int nCommands = Game<int32_t>(0xB82810);
    if (!obj || !types || !commands) return false;
    BYTE* type = types + *(int16_t*)(obj + 4) * 0xAF8;
    int n = *(int32_t*)(type + 0xA9C);
    for (int i = 0; i < n && i < 64; i++) {
        int id = *(int32_t*)(type + 0xAA8 + i * 4);
        if (id < 0 || id >= nCommands) continue;
        int kind = *(int32_t*)(commands + id * 0x200 + 8);
        if ((kind == kindA || kind == kindB) && *(int32_t*)(commands + id * 0x200 + 0xC) == value)
            return true;
    }
    return false;
}

// Train picker (0x519EF0, codes 0x3FC/0x3FD = train orders 0x190/0x191, value = unit type):
// true = this building cannot train the unit, skip it.
static int __cdecl SkipTrainer(BYTE* obj, int code, int value) {
    if (code != 0x3FC && code != 0x3FD) return 0;
    return !HasCommand(obj, 0x190, 0x191, value);
}

// 0x51A31B: cmp word [edi+0x4C], 0 (then jne skip 0x51A390); edi = candidate building,
// [ebp+0x14] = code, [ebp+0x18] = value.
__declspec(naked) static void PickerHook() {
    __asm {
        pushad
        push dword ptr [ebp + 0x18]
        push dword ptr [ebp + 0x14]
        push edi
        call SkipTrainer
        add esp, 12
        test eax, eax
        popad
        jnz skip
        cmp word ptr [edi + 0x4C], 0
        jmp dword ptr [g_pickerResume]
    skip:
        jmp dword ptr [g_pickerSkip]
    }
}

// Building placement (0x527F7B): 0x4C1A80 gets the first selected unit as the builder
// (objects at [ebp-0x44], count word [ebp-0x48], esi = building type). Use the first selected
// unit that has that building in its build list (command kind 0x12C) instead.
static BYTE* __cdecl Builder(BYTE** units, int count, int buildingType) {
    for (int i = 0; i < count && i < 16; i++)
        if (units[i] && HasCommand(units[i], 0x12C, 0x12C, buildingType)) return units[i];
    return units[0];
}

// 0x527FD2: push dword [edx+8] / push dword [ebp-0x44] / push esi (then call 0x4C1A80).
__declspec(naked) static void BuilderHook() {
    __asm {
        push dword ptr [edx + 8]
        push edx
        push ecx  // the game object for the thiscall 0x4C1A80
        push esi
        movsx eax, word ptr [ebp - 0x48]
        push eax
        lea eax, [ebp - 0x44]
        push eax
        call Builder
        add esp, 12
        pop ecx
        pop edx
        push eax
        push esi
        jmp dword ptr [g_builderResume]
    }
}

// Build orders (0x1B8 = place a building, value = building type; +5000 Shift-queued) go to every
// selected object through 0x51AD60 (args: table, object, player, code, value, ...). Only units
// that have that building in their build list (command kind 0x12C) keep it.
static int __cdecl SkipBuilder(BYTE* obj, int code, int value) {
    int c = code >= 5000 ? code - 5000 : code;
    if (c != 0x1B8 || !obj || Building(obj)) return 0;
    return !HasCommand(obj, 0x12C, 0x12C, value);
}

// 0x51AD60 entry: push ebp / mov ebp, esp / sub esp, 0xC (cdecl, nothing returned).
__declspec(naked) static void OrderHook() {
    __asm {
        push dword ptr [esp + 0x14]  // value
        push dword ptr [esp + 0x14]  // code
        push dword ptr [esp + 0x10]  // object
        call SkipBuilder
        add esp, 12
        test eax, eax
        jnz drop
        push ebp
        mov ebp, esp
        sub esp, 0xC
        jmp dword ptr [g_orderResume]
    drop:
        ret
    }
}

void InstallSubgroups() {
    static const BYTE entry[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};
    if (!Hook(0x51AD60, entry, sizeof entry, (void*)OrderHook, &g_orderResume))
        Log("unexpected game code at 0x51AD60 (different game version?). Build order check off.");
    // Order checks (gameplay, always on: only matter for mixed selections the game never made).
    static const BYTE picker[5] = {0x66, 0x83, 0x7F, 0x4C, 0x00};
    static const BYTE builder[7] = {0xFF, 0x72, 0x08, 0xFF, 0x75, 0xBC, 0x56};
    g_pickerSkip = 0x51A390 + g_slide;
    if (!Hook(0x51A31B, picker, sizeof picker, (void*)PickerHook, &g_pickerResume))
        Log("unexpected game code at 0x51A31B (different game version?). Train check off.");
    if (!Hook(0x527FD2, builder, sizeof builder, (void*)BuilderHook, &g_builderResume))
        Log("unexpected game code at 0x527FD2 (different game version?). Builder choice off.");
    if (!g_tabSubgroups) return;
    static const BYTE tab[7] = {0x57, 0xFF, 0xB6, 0x7C, 0x05, 0x00, 0x00};
    g_tabDone = 0x5242CA + g_slide;
    if (!Hook(0x52422B, tab, sizeof tab, (void*)TabHook, &g_tabResume))
        Log("unexpected game code at 0x52422B (different game version?). Tab keeps the game's own.");
}
