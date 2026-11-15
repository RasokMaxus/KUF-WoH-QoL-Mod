// camera.cpp - group-camera: double-tapping a control group goes to one of its buildings/units.
//
// The control-group keys are handled by the player at 0x523970. The player keeps 30 groups at
// player+0x40 (stride 0x22: word count, then up to 16 unit ids) and the last recalled group in
// word player+0x5CA. Pressing a group key:
//  - group != player+0x5CA (first tap): 0x523DD8, then the order 0x13 selects the group (the
//    order builder 0x4DD350 sets player+0x5CA to the group).
//  - group == player+0x5CA (another tap): 0x523C32 adds up the units' positions (0x516DC0 gives
//    the transport a unit sits in, else the unit; +0x18/+0x1C = x/y) and, unless the flag byte
//    player+0x5C9 says to skip it once, 0x523D86 puts the camera there: camera top-left
//    player+0x1038/+0x103C = average - half the view (0xBF2588/0xBF258C) - map origin
//    (0xB869C4/0xB869C8). The group is selected again, so every further tap does this too.
//    player+0x5C9 is set by any camera move or map-edge clamp (0x5283D0, 0x5287xx, 0x529010).
// With two buildings far apart the average is empty ground between them. group-camera replaces
// the average with the position of one member: each further tap goes to the next one.
//
// The order. The game does not keep the order things were selected in: every select click goes
// through the order builder (0x4DD350, order 0x12), which copies the selection, applies the click
// (0x4D0AF0) and sorts the list (0x528B30 -> std::sort 0x51E290: flag +0x1AC, unit type rank,
// then a per-object value). Ctrl+key copies that sorted list into the group (0x5239CF; Shift+key
// appends). So the mod records it itself:
//  - after the sort (call at 0x4DD537) every id that was not selected before gets a number from a
//    running clock (the old selection is still in player+0x1084/+0x1086 there);
//  - when a group is set (0x523D6B, end of the Ctrl/Shift+key loop) the group's order is saved:
//    Ctrl = all members by clock, Shift = the saved order, then the added members by clock.
// The tour: buildings first, by type (the type selected first comes first), each type in the
// saved order; then units in the game's own order (heroes and higher ranks first). Members the
// mod has no order for (e.g. groups from a loaded save) follow the game's order.
// The cycle restarts when another group key is pressed. Everything here is local to this PC
// (camera, the mod's own bookkeeping), so it cannot cause a desync.
#include "kufmod.h"

bool g_groupCamera = true;
static uintptr_t g_resumeTap, g_resumeCenter, g_resumeGroup, g_sortFn;
static int g_next;            // how many tours steps the next double tap goes past the first
static int g_lastGroup = -1;  // the group key pressed last

static const int kGroups = 30, kMax = 16;
static uint32_t g_clock;
static uint32_t g_stamp[0x8000];          // per object id: clock when it was selected
static int16_t g_order[kGroups][kMax];    // per group: ids in the recorded order
static int g_orderN[kGroups];

typedef BYTE*(__cdecl* CarrierFn)(BYTE* unit, int flag);
static CarrierFn g_carrier;

static BYTE* Object(int id) {
    BYTE* objects = Game<BYTE*>(0xB870C0);
    return objects && id >= 0 ? *(BYTE**)(objects + 0x2C38 + id * 4) : NULL;
}

// After the selection sort of a select click: stamp the newly selected ids.
static void __cdecl OnSelect(BYTE* player, const int16_t* count, const int16_t* ids) {
    int oldN = *(int16_t*)(player + 0x1084);
    const int16_t* old = (const int16_t*)(player + 0x1086);
    for (int i = 0; i < *count; i++) {
        int id = ids[i];
        if (id < 0) continue;
        bool was = false;
        for (int k = 0; k < oldN && !was; k++) was = old[k] == id;
        if (!was) g_stamp[id & 0x7FFF] = ++g_clock;
    }
}

// After Ctrl+key (append = 0) or Shift+key (append = 1) wrote a group.
static void __cdecl OnGroupSet(BYTE* player, int16_t* group, int append) {
    int g = (int)((BYTE*)group - (player + 0x40)) / 0x22;
    if (g < 0 || g >= kGroups) return;
    int n = group[0];
    if (n > kMax) n = kMax;
    const int16_t* ids = group + 1;
    int16_t out[kMax];
    int m = 0;
    if (append)
        for (int k = 0; k < g_orderN[g]; k++)
            for (int i = 0; i < n; i++)
                if (ids[i] == g_order[g][k]) { out[m++] = ids[i]; break; }
    int start = m;
    for (int i = 0; i < n; i++) {
        bool have = false;
        for (int k = 0; k < start && !have; k++) have = out[k] == ids[i];
        if (have || ids[i] < 0) continue;
        int j = m++;  // insert by clock, stable
        while (j > start && g_stamp[out[j - 1] & 0x7FFF] > g_stamp[ids[i] & 0x7FFF]) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = ids[i];
    }
    memcpy(g_order[g], out, m * sizeof out[0]);
    g_orderN[g] = m;
    if (g == g_lastGroup) g_next = 0;  // a re-set group starts its tour at the first member
}

// Double tap: sums = the game's x and y sums ([ebp-0x18], [ebp-0x14]); set to the chosen
// member's position times count so the game's own division gives exactly that position.
static void __cdecl Center(int count, const int16_t* ids, int* sums) {
    if (count <= 0 || count > kMax) return;
    int g = g_lastGroup;
    struct Entry { int key, rank, x, y, id; } e[kMax];
    int n = 0;
    for (int k = 0; k < count; k++) {
        BYTE* unit = Object(ids[k]);
        BYTE* at = unit ? g_carrier(unit, 0) : NULL;
        if (!at) continue;
        Entry& x = e[n++];
        x.id = ids[k];
        x.x = *(int*)(at + 0x18);
        x.y = *(int*)(at + 0x1C);
        if (*(int16_t*)(unit + 6) == 1) {  // building: recorded order, else the game's
            x.rank = 100 + k;
            if (g >= 0 && g < kGroups)
                for (int r = 0; r < g_orderN[g]; r++)
                    if (g_order[g][r] == ids[k]) { x.rank = r; break; }
            x.key = *(int16_t*)(unit + 4);  // type; replaced by the type's first rank below
        } else {
            x.rank = 1000 + k;
            x.key = -1;
        }
    }
    if (!n) return;
    int typeKey[kMax];
    for (int i = 0; i < n; i++) {
        if (e[i].key < 0) { typeKey[i] = 1000; continue; }
        int best = e[i].rank;
        for (int j = 0; j < n; j++)
            if (e[j].key == e[i].key && e[j].rank < best) best = e[j].rank;
        typeKey[i] = best;
    }
    for (int i = 0; i < n; i++) e[i].key = typeKey[i];
    for (int i = 1; i < n; i++) {  // sort by (type's first rank, rank)
        Entry v = e[i];
        int j = i;
        while (j > 0 && (e[j - 1].key > v.key || (e[j - 1].key == v.key && e[j - 1].rank > v.rank))) {
            e[j] = e[j - 1];
            j--;
        }
        e[j] = v;
    }
    int step = g_next % n;
    g_next = step + 1;
    sums[0] = e[step].x * count;
    sums[1] = e[step].y * count;
}

// 0x523C23: movsx eax, word [esi+0x5CA] (then cmp eax, ecx; ecx = the pressed group).
// Another group than last time restarts the cycle.
__declspec(naked) static void TapHook() {
    __asm {
        cmp ecx, g_lastGroup
        je same
        mov g_lastGroup, ecx
        mov g_next, 0
    same:
        movsx eax, word ptr [esi + 0x5CA]
        jmp dword ptr [g_resumeTap]
    }
}

// 0x523D8B: cdq / idiv ecx / mov ecx, eax (eax = x sum, ecx = count; ids at [ebp-0x1C]).
__declspec(naked) static void CenterHook() {
    __asm {
        push ecx
        lea eax, [ebp - 0x18]
        push eax
        push dword ptr [ebp - 0x1C]
        push ecx
        call Center
        add esp, 12
        pop ecx
        mov eax, dword ptr [ebp - 0x18]
        cdq
        idiv ecx
        mov ecx, eax
        jmp dword ptr [g_resumeCenter]
    }
}

// 0x4DD537: call 0x528B30 (thiscall player, args &count, ids; ret 8) - the selection sort.
__declspec(naked) static void SelectHook() {
    __asm {
        push ecx
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        call dword ptr [g_sortFn]
        pop ecx
        push dword ptr [esp + 8]
        push dword ptr [esp + 8]
        push ecx
        call OnSelect
        add esp, 12
        ret 8
    }
}

// 0x523D6B: mov edi, [ebp-0x18] (the group) / mov word [edi], cx (its new count).
// esi = player, byte [ebp-1] = Shift held (append).
__declspec(naked) static void GroupHook() {
    __asm {
        mov edi, dword ptr [ebp - 0x18]
        mov word ptr [edi], cx
        pushad
        movzx eax, byte ptr [ebp - 1]
        push eax
        push edi
        push esi
        call OnGroupSet
        add esp, 12
        popad
        jmp dword ptr [g_resumeGroup]
    }
}

void InstallGroupCamera() {
    if (!g_groupCamera) return;
    g_carrier = (CarrierFn)(0x516DC0 + g_slide);
    g_sortFn = 0x528B30 + g_slide;
    static const BYTE tap[7] = {0x0F, 0xBF, 0x86, 0xCA, 0x05, 0x00, 0x00};
    static const BYTE center[5] = {0x99, 0xF7, 0xF9, 0x8B, 0xC8};
    static const BYTE group[6] = {0x8B, 0x7D, 0xE8, 0x66, 0x89, 0x0F};
    static const BYTE sel[5] = {0xE8, 0xF4, 0xB5, 0x04, 0x00};  // call 0x528B30
    if (!Hook(0x523D8B, center, sizeof center, (void*)CenterHook, &g_resumeCenter)) {
        Log("unexpected game code at 0x523D8B (different game version?). group-camera off.");
        return;
    }
    if (!Hook(0x523C23, tap, sizeof tap, (void*)TapHook, &g_resumeTap))
        Log("unexpected game code at 0x523C23. group-camera will not restart its cycle.");
    if (!HookCall(0x4DD537, sel, sizeof sel, (void*)SelectHook) ||
        !Hook(0x523D6B, group, sizeof group, (void*)GroupHook, &g_resumeGroup))
        Log("unexpected game code at 0x4DD537/0x523D6B. group-camera uses the game's order.");
}
