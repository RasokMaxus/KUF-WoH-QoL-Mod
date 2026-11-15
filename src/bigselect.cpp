// bigselect.cpp - big-selection: up to 240 units in one selection (the game: 16).
//
// A player object (base class 0x170C bytes, subclass vtable 0xA4184C 0x1784 bytes) keeps two
// selections: the synced one (what orders act on, set by orders 0x12/0x13 in the dispatcher
// case 0x4DB903): count word +0x1062, 16 ids +0x1064; and the local one (set at once by the
// order builders 0x4DD350 / 0x5359D9): count +0x1084, 16 ids +0x1086. Other fields follow
// right after (+0x10A8 common type, +0x10AC owner, +0x10B0 an object), so the id arrays cannot
// grow in place. big-selection makes the four player allocations (0x4BF8F0, 0x4C9DF4, 0x531F34,
// 0x532F90) bigger and moves both id arrays behind the subclass's fields: every instruction
// that names +0x1064 or +0x1086 (69 of them: displacements and immediates) gets the new offset.
// The counts stay where they are.
//
// Other places the 16 lives:
//  - The order builders copy the selection into a 16-word stack buffer [ebp-0x24] (the stack
//    cookie is right above it). Seven instructions in each builder use it; they now use a
//    240-word buffer of ours (hooks, and two in-place rewrites of [ebp+edx-0x10AC]).
//  - The limit: 0x4D0AF0 passes 16 (push 0x10 at 0x4D0AF8) through 0x525C30 to the selection
//    switch 0x527860, which hands it to the click/box/add modes; mode 2 (0x518B40) has its own
//    push 0x10 at 0x5279E7. Both become 240. (Other players' units: still 1.)
//  - The unit panel's copy (KufMenu = player+0x5E0): count menu+0x1898, ids in a heap buffer
//    [menu+0x189C] of 0x20 bytes (malloc at 0x4FFD9D) -> 0x1E0 bytes. The panel draws at most
//    16 portraits (0x512034); the rest are selected but have no portrait.
//  - Saved games (save 0x4D70A0, load 0x4CA620) store each selection as count + 16 entries.
//    A selection of more than 16 is saved as count | 0x8000, the first 16 entries, then the
//    rest; a save with no such selection is the game's own format (old saves load as before).
//  - A network turn packs all queued orders into a 0x800-byte buffer without a bounds check
//    (EventTCPIPCmdQueue flush 0x6B0430, vtable 0xA4C9A4 slot +0x14). A 240-unit selection order
//    is 488 bytes, so orders that would not fit wait for the next turn (slot +4 push, 0x6AFC00).
//
// Gameplay change: every PC in a match must use the same setting (an old build would write a
// long selection order past its 16-id array).
#include "kufmod.h"
#include <climits>
#include <deque>
#include <string>

bool g_bigSelection = true;
int g_selMax = 16, g_syncIds = 0x1064, g_localIds = 0x1086;

static const int kMax = 240;
static const int kSync = 0x1784;              // behind the subclass's fields
static const int kLocal = kSync + 2 * kMax;   // 0x1964
static const int kSize = kLocal + 2 * kMax + 4;  // 0x1B48

static int16_t g_bufA[kMax + 16], g_bufB[kMax + 16];  // the builders' selection buffers
static uintptr_t g_objTable;                           // address of [0xB870C0]
static uintptr_t rA1, rA2, rA3, rA4, rA6, rA7, rB1, rB2, rB3, rB4, rB6, rB7, rCap, rCap2, rPanel,
    g_saveEnd, g_loadEnd, rCap4C, rUnused, rQueued, g_dispatchEnd, rPlain, g_groupMoveEnd;

// ---------------------------------------------------------------- builder 0x4DD350
__declspec(naked) static void A1() {  // mov [ebp+ecx*2-0x24], ax
    __asm {
        mov word ptr g_bufA[ecx * 2], ax
        jmp dword ptr [rA1]
    }
}
__declspec(naked) static void A2() {  // lea eax, [ebp-0x24] / mov ecx, esi
    __asm {
        lea eax, g_bufA
        mov ecx, esi
        jmp dword ptr [rA2]
    }
}
__declspec(naked) static void A3() {  // lea eax, [ebp-0x24] / push eax / lea eax, [ebp-0x834]
    __asm {
        lea eax, g_bufA
        push eax
        lea eax, [ebp - 0x834]
        jmp dword ptr [rA3]
    }
}
__declspec(naked) static void A4() {  // movsx ecx, word [ebp-0x24] / mov eax, [0xB870C0]
    __asm {
        movsx ecx, word ptr g_bufA
        mov eax, g_objTable
        mov eax, dword ptr [eax]
        jmp dword ptr [rA4]
    }
}
__declspec(naked) static void A6() {  // movzx ecx, word [ebp+edx*2-0x24]
    __asm {
        movzx ecx, word ptr g_bufA[edx * 2]
        jmp dword ptr [rA6]
    }
}
__declspec(naked) static void A7() {  // lea ecx, [ebp-0x24] / push ecx / push [ebp-0x834]
    __asm {
        lea ecx, g_bufA
        push ecx
        push dword ptr [ebp - 0x834]
        jmp dword ptr [rA7]
    }
}

// ---------------------------------------------------------------- builder 0x5359D9
__declspec(naked) static void B1() {
    __asm {
        mov word ptr g_bufB[ecx * 2], ax
        jmp dword ptr [rB1]
    }
}
__declspec(naked) static void B2() {
    __asm {
        lea eax, g_bufB
        mov ecx, esi
        jmp dword ptr [rB2]
    }
}
__declspec(naked) static void B3() {
    __asm {
        movsx ecx, word ptr g_bufB
        mov eax, g_objTable
        mov eax, dword ptr [eax]
        jmp dword ptr [rB3]
    }
}
__declspec(naked) static void B4() {  // lea eax, [ebp-0x24] / push eax / lea eax, [ebp-0x82C]
    __asm {
        lea eax, g_bufB
        push eax
        lea eax, [ebp - 0x82C]
        jmp dword ptr [rB4]
    }
}
__declspec(naked) static void B6() {
    __asm {
        movzx ecx, word ptr g_bufB[edx * 2]
        jmp dword ptr [rB6]
    }
}
__declspec(naked) static void B7() {  // lea ecx, [ebp-0x24] / push ecx / push [ebp-0x82C]
    __asm {
        lea ecx, g_bufB
        push ecx
        push dword ptr [ebp - 0x82C]
        jmp dword ptr [rB7]
    }
}

// ---------------------------------------------------------------- limit and panel buffer
__declspec(naked) static void Cap() {  // push 0x10 / push [ebp+0x18]
    __asm {
        push 240  // kMax
        push dword ptr [ebp + 0x18]
        jmp dword ptr [rCap]
    }
}
__declspec(naked) static void Cap2() {  // mov ecx, [0xB870C0] / push 0x10
    __asm {
        mov ecx, g_objTable
        mov ecx, dword ptr [ecx]
        push 240  // kMax
        jmp dword ptr [rCap2]
    }
}
__declspec(naked) static void Panel() {  // push 0x20 / mov dword [esi+0x18A4], -1
    __asm {
        push 0x1E0  // 2 * kMax bytes
        mov dword ptr [esi + 0x18A4], -1
        jmp dword ptr [rPanel]
    }
}

// ---------------------------------------------------------------- list cleanup
// 0x525EF0 (thiscall player, ids, count*, flag; ret 0xC) and 0x526020 (player, ids, count*;
// ret 8) drop dead/foreign units from a selection or group. Both mark entries in a 16-word
// stack array; these are the same rules without the array. They run on synced state on every
// PC, so both PCs must use the same build (as for the whole feature).
typedef BYTE*(__cdecl* CarrierFn)(BYTE* unit, int flag);
static CarrierFn g_carrier;

static BYTE* Obj(int id) {
    BYTE* objects = *(BYTE**)g_objTable;
    return objects ? *(BYTE**)(objects + 0x2C38 + id * 4) : NULL;
}

// Own unit still usable: not gone (+0x4C), owned by this player (+8 vs player+0x590), not
// inside a carrier of type 0x32.
static bool Usable(BYTE* player, BYTE* o) {
    if (*(int16_t*)(o + 0x4C)) return false;
    if (*(int16_t*)(o + 8) != *(int32_t*)(player + 0x590)) return false;
    if (*(BYTE*)(o + 0x69)) {
        BYTE* c = g_carrier(o, 0);
        if (c && *(int16_t*)(c + 4) == 0x32) return false;
    }
    return true;
}

static int Compact(int16_t* ids, int n) {
    int k = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] != -1) ids[k++] = ids[i];
    return k;
}

static char __fastcall Clean1(BYTE* player, void*, int16_t* ids, int16_t* count, int flag) {
    bool changed = false;
    int n = *count;
    for (int i = 0; i < n; i++) {
        int16_t id = ids[i];
        BYTE* o = id == -1 ? NULL : Obj(id);
        bool keep = o && ((BYTE)flag ? *(int16_t*)(o + 0x4C) == 0 : Usable(player, o));
        if (!keep) { ids[i] = -1; changed = true; }
    }
    if (!changed) return 0;
    *count = (int16_t)Compact(ids, n);
    return 1;
}

static char __fastcall Clean2(BYTE* player, void*, int16_t* ids, int16_t* count) {
    bool changed = false;
    int n = *count;
    for (int i = 0; i < n; i++) {
        int16_t id = ids[i];
        if (id == -1) { changed = true; continue; }
        BYTE* o = Obj(id);
        if (o && !Usable(player, o)) { ids[i] = -1; changed = true; }  // (no object: kept)
    }
    if (!changed) return 0;
    *count = (int16_t)Compact(ids, n);
    return 1;
}

// Order 0x4C in 0x519EF0 keeps a 16-dword stack array and loops to the selection count:
// count capped at 16 there (the game only acts on the first unit's +0x5A0 for <= 16 anyway).
__declspec(naked) static void Cap4C() {  // movsx esi, word [ebp+8] / xor eax, eax
    __asm {
        movsx esi, word ptr [ebp + 8]
        cmp esi, 16
        jle ok
        mov esi, 16
    ok:
        xor eax, eax
        jmp dword ptr [rCap4C]
    }
}

// ---------------------------------------------------------------- formations
// Formation orders (0x1EA..0x226 -> 0x519630) place units with 0x5196E0 (thiscall object table;
// formation, count, ids, x, y; ret 0x14): sort the units (0x435530), average their position,
// facing = 0x48A230(cx, cy, x, y), spots = 0x434DC0(formation, x, y, facing, n, spots), clamp to
// the map, then each unit gets move order 0x118 to its spot (vtable +0x1C). It and the shape
// function 0x434DC0 keep 16 units/spots on the stack. Up to 16 units: the same steps. More: blocks
// of 16 (in the sorted order), each in the formation; block k stands k * (block depth + gap)
// behind the first, toward where the units come from. (0x5198C0, the one-hero variant, only
// places that hero's own squad.)
typedef void(__cdecl* SortFn)(BYTE** units, int n);
typedef int(__cdecl* FacingFn)(int cx, int cy, int x, int y);
typedef void(__cdecl* ShapeFn)(int formation, int x, int y, int facing, int n, int* spots);
typedef void(__thiscall* UnitOrderFn)(BYTE* unit, int code, int xy, int a, int b, int c, int d);
static SortFn g_sortUnits;
static FacingFn g_facing;
static ShapeFn g_shape;

static int ISqrt(int64_t v) {
    if (v <= 0) return 0;
    int64_t r = 0, bit = (int64_t)1 << 40;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

static char __fastcall Formation(BYTE* table, void*, int formation, int16_t count, const int16_t* ids, int x, int y) {
    int n = count;
    if (n <= 0) return 1;
    if (n > kMax) n = kMax;
    int maxX = (Game<int>(0xBEA630) + 8) << 4, maxY = (Game<int>(0xBEA634) + 8) << 4;
    BYTE* units[kMax];
    int m = 0;
    for (int i = 0; i < n; i++) {
        BYTE* u = *(BYTE**)(table + 0x2C38 + ids[i] * 4);
        if (u) units[m++] = u;
    }
    n = m;
    if (!n) return 1;
    g_sortUnits(units, n);
    int sx = 0, sy = 0;
    for (int i = 0; i < n; i++) { sx += *(int*)(units[i] + 0x18); sy += *(int*)(units[i] + 0x1C); }
    int cx = sx / n, cy = sy / n;
    int facing = g_facing(cx, cy, x, y);
    // back = unit vector (x1024) from the target toward the units
    int bx = cx - x, by = cy - y, len = ISqrt((int64_t)bx * bx + (int64_t)by * by);
    if (len) { bx = bx * 1024 / len; by = by * 1024 / len; }
    else { bx = 0; by = 1024; }
    int spots[2 * kMax], step = 0;
    for (int k = 0; k * 16 < n; k++) {
        int first = k * 16, size = n - first < 16 ? n - first : 16;
        int tx = x + bx * step * k / 1024, ty = y + by * step * k / 1024;
        int* s = spots + 2 * first;
        g_shape(formation, tx, ty, facing, size, s);
        if (k == 0 && n > 16) {  // depth of a block along the back direction
            int lo = INT_MAX, hi = INT_MIN;
            for (int i = 0; i < size; i++) {
                int p = ((s[2 * i] - x) * bx + (s[2 * i + 1] - y) * by) / 1024;
                if (p < lo) lo = p;
                if (p > hi) hi = p;
            }
            step = hi - lo + 0x60;
            if (step < 0x80) step = 0x80;
        }
    }
    for (int i = 0; i < n; i++) {
        int px = spots[2 * i], py = spots[2 * i + 1];
        if (px < 0x80) px = 0x80;
        if (px > maxX) px = maxX;
        if (py < 0x80) py = 0x80;
        if (py > maxY) py = maxY;
        UnitOrderFn order = *(UnitOrderFn*)(*(BYTE**)units[i] + 0x1C);
        order(units[i], 0x118, px << 16 | py, 0, -1, 0, 0);
    }
    return 1;
}

// ---------------------------------------------------------------- queued moves
// Shift+right-click on the ground (order 5000+0x730 = 0x1AB8) and Shift+move (5000+0xF0) reach
// the default case of 0x519EF0 (0x51A523), which gives every unit the same point: a big army
// then crowds one spot and keeps shoving before it takes the next waypoint. (Plain moves go
// through 0x51A8E0, which keeps each unit's offset from the group's middle, but only in
// multiplayer, [0xB83AD4], and only if the group fits in 600 x 600.) Here: each unit keeps its
// offset from the middle of the group's box, squeezed so the block is at most ~40 per unit across
// (half-width 20 * sqrt(n)), clamped to the map like 0x51A8E0. Orders on a target object are left
// as they are. Called on synced state on every PC (integer math only).
typedef void(__cdecl* UnitCmdFn)(BYTE* table, BYTE* unit, int player, int code, int xy, int target, int a, int b);
static UnitCmdFn g_unitCmd;

// Gives each unit (ids[0..n)) order `code` to its own spot around xy; 0 = nothing done.
static int Spread(BYTE* table, const int16_t* ids, int n, int player, int code, int xy, int target,
                  int a, int last) {
    if (n < 2) return 0;
    if (n > kMax) n = kMax;
    int x = xy >> 16, y = xy & 0xFFFF;
    BYTE* units[kMax];
    int px[kMax], py[kMax], m = 0;
    int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
    for (int i = 0; i < n; i++) {
        if (ids[i] < 0) continue;
        BYTE* u = *(BYTE**)(table + 0x2C38 + ids[i] * 4);
        if (!u) continue;
        BYTE* at = *(int16_t*)(u + 6) == 0 ? g_carrier(u, 1) : NULL;  // buildings: the point itself
        units[m] = u;
        px[m] = at ? *(int*)(at + 0x18) : INT_MIN;
        py[m] = at ? *(int*)(at + 0x1C) : 0;
        if (at) {
            if (px[m] < minX) minX = px[m];
            if (px[m] > maxX) maxX = px[m];
            if (py[m] < minY) minY = py[m];
            if (py[m] > maxY) maxY = py[m];
        }
        m++;
    }
    if (minX == INT_MAX) return 0;
    int cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    int spread = maxX - cx;
    if (maxY - cy > spread) spread = maxY - cy;
    int half = 20 * ISqrt((int64_t)m * 1024) / 32;  // 20 * sqrt(m)
    if (half < 64) half = 64;
    int mapX = Game<int>(0xBEA630), mapY = Game<int>(0xBEA634);
    for (int i = 0; i < m; i++) {
        int tx = x, ty = y;
        if (px[i] != INT_MIN) {
            int dx = px[i] - cx, dy = py[i] - cy;
            if (spread > half) { dx = dx * half / spread; dy = dy * half / spread; }
            tx = x + dx;
            ty = y + dy;
            if (tx < 0x80) tx = 0x81;
            if (tx >= (mapX - 8) << 4) tx = (mapX << 4) - 0x81;
            if (ty < 0x80) ty = 0x81;
            if (ty >= (mapY - 8) << 4) ty = (mapY << 4) - 0x81;
        }
        g_unitCmd(table, units[i], player, code, tx << 16 | (ty & 0xFFFF), target, a, last);
    }
    return 1;
}

static int __cdecl QueuedSpread(BYTE* ebp) {  // in 0x519EF0
    return Spread(*(BYTE**)(ebp - 0x74), *(const int16_t**)(ebp + 0xC), *(int16_t*)(ebp + 8),
                  *(int*)(ebp + 0x10), *(int*)(ebp + 0x14), *(int*)(ebp + 0x18), *(int*)(ebp - 0x68),
                  *(int*)(ebp + 0x20), 0);
}

// Plain group moves: 0x51A8E0 (thiscall table; count, ids, player, code, xy, target, a; ret 0x1C)
// sends everyone to the same point at 0x51AD0C when not in multiplayer or when the group is wider
// than 600. Ground orders (no target) of two or more units are spread there instead; the
// build order 0x1B8 keeps the game's way.
static int __cdecl PlainSpread(BYTE* ebp) {
    int code = *(int*)(ebp + 0x14), target = *(int*)(ebp + 0x1C);
    if (code == 0x1B8 || (target != 0 && target != -1)) return 0;
    return Spread(*(BYTE**)(ebp - 4), *(const int16_t**)(ebp + 0xC), *(int*)(ebp + 8), *(int*)(ebp + 0x10),
                  code, *(int*)(ebp + 0x18), target, *(int*)(ebp + 0x20), 1);
}

__declspec(naked) static void PlainHook() {  // 0x51AD0C: xor esi, esi / cmp [ebp+8], esi
    __asm {
        pushad
        push ebp
        call PlainSpread
        add esp, 4
        mov dword ptr [esp + 28], eax
        popad
        test eax, eax
        jz original
        jmp dword ptr [g_groupMoveEnd]
    original:
        xor esi, esi
        cmp dword ptr [ebp + 8], esi
        jmp dword ptr [rPlain]
    }
}

__declspec(naked) static void QueuedHook() {  // 0x51A523: movsx eax, word [ebp+8] / xor esi, esi
    __asm {
        cmp esi, 0x1AB8
        je check
        cmp esi, 0x1478
        jne original
    check:
        cmp dword ptr [ebp - 0x68], 0
        jne original
        pushad
        push ebp
        call QueuedSpread
        add esp, 4
        mov dword ptr [esp + 28], eax  // popad's eax
        popad
        test eax, eax
        jz original
        jmp dword ptr [g_dispatchEnd]
    original:
        movsx eax, word ptr [ebp + 8]
        xor esi, esi
        jmp dword ptr [rQueued]
    }
}

// ---------------------------------------------------------------- saved games
typedef void(__thiscall* StreamFn)(void* s, void* p, int n);
static StreamFn StreamSlot(void* s, int off) { return *(StreamFn*)(*(BYTE**)s + off); }

static BYTE* LocalPlayer() {
    BYTE* game = Game<BYTE*>(0xB8282C);
    return game ? *(BYTE**)(game + 0x48 + 4 * *(int32_t*)(game + 0x664)) : NULL;
}

// Save (vtable +0xC = write): the game writes each id as its index in the saved-object list
// [ebp-0x16C] = {int n; int* ids}, -1 if not there.
static void __cdecl SaveSel(void* stream, BYTE* ebp) {
    BYTE* player = LocalPlayer();
    const int* list = *(const int**)(ebp - 0x16C);
    StreamFn write = StreamSlot(stream, 0xC);
    for (int which = 0; which < 2 && player; which++) {
        int16_t n = *(int16_t*)(player + (which ? 0x1084 : 0x1062));
        const int16_t* ids = (const int16_t*)(player + (which ? kLocal : kSync));
        if (n < 0) n = 0;
        if (n > kMax) n = kMax;
        int16_t head = n > 16 ? (int16_t)(n | 0x8000) : n;
        write(stream, &head, 2);
        int total = n > 16 ? n : 16;
        for (int i = 0; i < total; i++) {
            int32_t v = -1;  // the low word is written
            if (i < n) {
                const int* arr = *(int* const*)(list + 1);
                for (int k = 0; k < list[0] && v < 0; k++)
                    if (arr[k] == ids[i]) v = k;
            }
            write(stream, &v, 2);
        }
    }
}

// Load (vtable +8 = read): the ids are list indices until the game maps them back.
static void __cdecl LoadSel(void* stream, BYTE* game) {
    BYTE* player = *(BYTE**)(game + 0x48 + 4 * *(int32_t*)(game + 0x664));
    StreamFn read = StreamSlot(stream, 8);
    for (int which = 0; which < 2; which++) {
        int16_t* count = (int16_t*)(player + (which ? 0x1084 : 0x1062));
        int16_t* ids = (int16_t*)(player + (which ? kLocal : kSync));
        int16_t head = 0;
        read(stream, &head, 2);
        read(stream, ids, 32);
        int n = head;
        if (head & 0x8000) {
            n = head & 0x7FFF;
            if (n > kMax) n = kMax;  // (never written)
            read(stream, ids + 16, (n - 16) * 2);
        } else if (n > 16 || n < 0) {
            n = 0;
        }
        *count = (int16_t)n;
    }
}

__declspec(naked) static void SaveHook() {  // 0x4D86F8: edi = stream
    __asm {
        pushad
        push ebp
        push edi
        call SaveSel
        add esp, 8
        popad
        mov esi, dword ptr [ebp - 0x16C]  // as after the game's loops
        jmp dword ptr [g_saveEnd]
    }
}
__declspec(naked) static void LoadHook() {  // 0x4CB430: edi = stream, esi = game
    __asm {
        pushad
        push esi
        push edi
        call LoadSel
        add esp, 8
        popad
        jmp dword ptr [g_loadEnd]
    }
}

// ---------------------------------------------------------------- network turn size
// Queue node: +8 next, +0x18 length; queue +0x8D8 count, +0x8DC first node. The flush writes
// each order as length byte(s) (one 0xFF per 255) and the bytes.
typedef int(__thiscall* PushFn)(void* q, const void* data, int len, int x);
typedef int(__thiscall* FlushFn)(void* q, int arg);
static PushFn g_push;
static FlushFn g_flush;
static const int kTurnLimit = 0x700;  // of 0x800, minus the header
struct Held { std::string data; int x; };
static std::deque<Held> g_held;
static void* g_heldQueue;
static int g_heldLogged;

static int Cost(int len) { return len + 1 + len / 255; }
static int Queued(BYTE* q) {
    int sum = 0, guard = 0;
    for (BYTE* node = *(BYTE**)(q + 0x8DC); node && guard < 4096; node = *(BYTE**)(node + 8), guard++)
        sum += Cost(*(int*)(node + 0x18));
    return sum;
}

static int __fastcall NetPush(void* q, void*, const void* data, int len, int x) {
    if (q != g_heldQueue) { g_held.clear(); g_heldQueue = q; }
    if (data && len > 0 && (!g_held.empty() || Queued((BYTE*)q) + Cost(len) > kTurnLimit)) {
        g_held.push_back({std::string((const char*)data, len), x});
        if (g_heldLogged < 20) {
            g_heldLogged++;
            Log("big-selection: order 0x%X (%d bytes) waits for the next turn (%d waiting)",
                *(const uint16_t*)data, len, (int)g_held.size());
        }
        return 1;
    }
    return g_push(q, data, len, x);
}

static int __fastcall NetFlush(void* q, void*, int arg) {
    if (q == g_heldQueue)
        while (!g_held.empty() && Queued((BYTE*)q) + Cost((int)g_held.front().data.size()) <= kTurnLimit) {
            Held h = g_held.front();
            g_held.pop_front();
            g_push(q, h.data.data(), (int)h.data.size(), h.x);
        }
    return g_flush(q, arg);
}

// Replace a vtable slot, chaining to whatever is there now (other features may wrap it too).
static bool ChainSlot(uintptr_t slotVa, uintptr_t originalVa, void* replacement, void** next) {
    uintptr_t cur = *(uintptr_t*)(slotVa + g_slide), repl = (uintptr_t)replacement;
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&ChainSlot, &self);
    HMODULE at = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)cur, &at);
    if (cur != originalVa + g_slide && at != self) return false;  // neither the game's nor ours
    *next = (void*)cur;
    return Patch(slotVa, &cur, &repl, 4);
}

// ---------------------------------------------------------------- install
struct Fix { uint32_t va; uint8_t off; uint16_t old; };
static const Fix kFixes[] = {
    {0x48FED9, 1, 0x1064}, {0x497897, 1, 0x1064}, {0x4CB8C7, 1, 0x1064}, {0x4D0822, 3, 0x1086},
    {0x4DB978, 4, 0x1064}, {0x4DBA49, 2, 0x1064}, {0x4DD452, 4, 0x1064}, {0x4DD486, 1, 0x1086},
    {0x4DD5B3, 1, 0x1086}, {0x4DD678, 4, 0x1086}, {0x4DD6C7, 2, 0x1086}, {0x4DD98E, 1, 0x1086},
    {0x4DDA2D, 4, 0x1086}, {0x4DDA8C, 1, 0x1086}, {0x4DDE69, 6, 0x1086}, {0x510A4B, 3, 0x1064},
    {0x513CB0, 3, 0x1064}, {0x520883, 2, 0x1064}, {0x521600, 2, 0x1064}, {0x521617, 2, 0x1086},
    {0x521892, 2, 0x1064}, {0x5219DE, 2, 0x1064}, {0x521A34, 1, 0x1064}, {0x521E65, 3, 0x1086},
    {0x522100, 2, 0x1064}, {0x522AB5, 2, 0x1086}, {0x522B10, 2, 0x1064}, {0x522B3A, 3, 0x1086},
    {0x523143, 2, 0x1064}, {0x5232F1, 3, 0x1086}, {0x5237F5, 2, 0x1064}, {0x523891, 2, 0x1064},
    {0x5238CE, 2, 0x1064}, {0x523E98, 2, 0x1086}, {0x525895, 2, 0x1064}, {0x526CD0, 2, 0x1064},
    {0x526D54, 2, 0x1064}, {0x526D9C, 1, 0x1064}, {0x52777B, 4, 0x1064}, {0x5277A1, 2, 0x1064},
    {0x527841, 2, 0x1064}, {0x527BDB, 3, 0x1064}, {0x527D2C, 2, 0x1064}, {0x527F26, 2, 0x1064},
    {0x527F68, 2, 0x1064}, {0x527F9B, 2, 0x1064}, {0x527FF8, 2, 0x1064}, {0x528033, 2, 0x1064},
    {0x5280AE, 2, 0x1086}, {0x5284D4, 2, 0x1086}, {0x528851, 2, 0x1064}, {0x5288E0, 2, 0x1064},
    {0x528D5F, 2, 0x1064}, {0x528D76, 2, 0x1086}, {0x528DBA, 2, 0x1064}, {0x528DCF, 2, 0x1086},
    {0x528E30, 2, 0x1064}, {0x529075, 2, 0x1064}, {0x535A82, 4, 0x1064}, {0x535AC1, 1, 0x1086},
    {0x535C21, 1, 0x1086}, {0x535CD8, 4, 0x1086}, {0x535D27, 2, 0x1086}, {0x53600D, 1, 0x1086},
    {0x53610A, 4, 0x1086}, {0x536163, 1, 0x1086}, {0x5364A0, 6, 0x1086}, {0x53DF4F, 4, 0x1086},
    {0x5D6145, 3, 0x1064},
};

struct Site { uint32_t va; BYTE len; BYTE bytes[12]; void* target; uintptr_t* resume; };

void InstallBigSelection() {
    if (!g_bigSelection) return;
    g_objTable = 0xB870C0 + g_slide;
    // Check everything first: all or nothing.
    for (const Fix& f : kFixes)
        if (*(uint32_t*)(f.va + f.off + g_slide) != f.old) {
            Log("big-selection: unexpected game code at 0x%X (different game version?). Off.", f.va);
            return;
        }
    BYTE ot[4];
    memcpy(ot, &g_objTable, 4);
    Site sites[] = {
        {0x4DD45A, 5, {0x66, 0x89, 0x44, 0x4D, 0xDC}, (void*)A1, &rA1},
        {0x4DD4F7, 5, {0x8D, 0x45, 0xDC, 0x8B, 0xCE}, (void*)A2, &rA2},
        {0x4DD528, 10, {0x8D, 0x45, 0xDC, 0x50, 0x8D, 0x85, 0xCC, 0xF7, 0xFF, 0xFF}, (void*)A3, &rA3},
        {0x4DD569, 9, {0x0F, 0xBF, 0x4D, 0xDC, 0xA1, ot[0], ot[1], ot[2], ot[3]}, (void*)A4, &rA4},
        {0x4DD660, 5, {0x0F, 0xB7, 0x4C, 0x55, 0xDC}, (void*)A6, &rA6},
        {0x4DD8C5, 10, {0x8D, 0x4D, 0xDC, 0x51, 0xFF, 0xB5, 0xCC, 0xF7, 0xFF, 0xFF}, (void*)A7, &rA7},
        {0x535A8A, 5, {0x66, 0x89, 0x44, 0x4D, 0xDC}, (void*)B1, &rB1},
        {0x535B85, 5, {0x8D, 0x45, 0xDC, 0x8B, 0xCE}, (void*)B2, &rB2},
        {0x535BC3, 9, {0x0F, 0xBF, 0x4D, 0xDC, 0xA1, ot[0], ot[1], ot[2], ot[3]}, (void*)B3, &rB3},
        {0x535BE8, 10, {0x8D, 0x45, 0xDC, 0x50, 0x8D, 0x85, 0xD4, 0xF7, 0xFF, 0xFF}, (void*)B4, &rB4},
        {0x535CC0, 5, {0x0F, 0xB7, 0x4C, 0x55, 0xDC}, (void*)B6, &rB6},
        {0x535E88, 10, {0x8D, 0x4D, 0xDC, 0x51, 0xFF, 0xB5, 0xD4, 0xF7, 0xFF, 0xFF}, (void*)B7, &rB7},
        {0x4D0AF8, 5, {0x6A, 0x10, 0xFF, 0x75, 0x18}, (void*)Cap, &rCap},
        {0x5279E1, 8, {0x8B, 0x0D, ot[0], ot[1], ot[2], ot[3], 0x6A, 0x10}, (void*)Cap2, &rCap2},
        {0x4FFD9D, 12, {0x6A, 0x20, 0xC7, 0x86, 0xA4, 0x18, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF}, (void*)Panel, &rPanel},
        {0x4CB430, 6, {0x8B, 0x07, 0x8B, 0xCF, 0x6A, 0x02}, (void*)LoadHook, &rUnused},  // resumes at g_loadEnd
        {0x525EF0, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30}, (void*)Clean1, &rUnused},
        {0x526020, 6, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x30}, (void*)Clean2, &rUnused},
        {0x51A076, 6, {0x0F, 0xBF, 0x75, 0x08, 0x33, 0xC0}, (void*)Cap4C, &rCap4C},
        {0x5196E0, 9, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00}, (void*)Formation, &rUnused},
        {0x51A523, 6, {0x0F, 0xBF, 0x45, 0x08, 0x33, 0xF6}, (void*)QueuedHook, &rQueued},
        {0x51AD0C, 5, {0x33, 0xF6, 0x39, 0x75, 0x08}, (void*)PlainHook, &rPlain},
    };
    g_unitCmd = (UnitCmdFn)(0x51AD60 + g_slide);
    g_dispatchEnd = 0x51A6CC + g_slide;
    g_groupMoveEnd = 0x51AD51 + g_slide;  // pop edi/esi/ebx, ret 0x1C
    g_sortUnits = (SortFn)(0x435530 + g_slide);
    g_facing = (FacingFn)(0x48A230 + g_slide);
    g_shape = (ShapeFn)(0x434DC0 + g_slide);
    g_carrier = (CarrierFn)(0x516DC0 + g_slide);
    uintptr_t gameRef = 0xB8282C + g_slide;
    BYTE save[8] = {0x8B, 0x07, 0x8B, 0x0D};
    memcpy(save + 4, &gameRef, 4);
    static const BYTE rd[8] = {0x66, 0x8B, 0x84, 0x15, 0x54, 0xEF, 0xFF, 0xFF};  // mov ax, [ebp+edx-0x10AC]
    static const BYTE end2[2] = {0x8B, 0x07};
    bool ok = !memcmp((BYTE*)(0x4D86F8 + g_slide), save, 8) &&
              !memcmp((BYTE*)(0x4D88EA + g_slide), end2, 2) && !memcmp((BYTE*)(0x4CB49C + g_slide), end2, 2) &&
              !memcmp((BYTE*)(0x4DD5CE + g_slide), rd, 8) && !memcmp((BYTE*)(0x535C34 + g_slide), rd, 8);
    for (const Site& s : sites) ok = ok && !memcmp((BYTE*)(s.va + g_slide), s.bytes, s.len);
    static const uint32_t allocs[4] = {0x4BF8F0, 0x4C9DF4, 0x531F34, 0x532F90};
    static const uint32_t sizes[4] = {0x170C, 0x170C, 0x1784, 0x1784};
    for (int i = 0; i < 4; i++)
        ok = ok && *(BYTE*)(allocs[i] + g_slide) == 0x68 && *(uint32_t*)(allocs[i] + 1 + g_slide) == sizes[i];
    if (!ok) {
        Log("big-selection: unexpected game code (different game version?). Off.");
        return;
    }
    // Apply.
    for (int i = 0; i < 4; i++) {
        BYTE was[5] = {0x68}, now[5] = {0x68};
        memcpy(was + 1, &sizes[i], 4);
        uint32_t size = kSize;
        memcpy(now + 1, &size, 4);
        Patch(allocs[i], was, now, 5);
    }
    for (const Fix& f : kFixes) {
        uint32_t was = f.old, now = f.old == 0x1064 ? kSync : kLocal;
        Patch(f.va + f.off, &was, &now, 4);
    }
    for (const Site& s : sites) Hook(s.va, s.bytes, s.len, s.target, s.resume);
    g_saveEnd = 0x4D88EA + g_slide;  // after the game's selection blocks (Hook sets resume = site + len)
    g_loadEnd = 0x4CB49C + g_slide;
    uintptr_t unused;
    Hook(0x4D86F8, save, 8, (void*)SaveHook, &unused);
    // mov ax, [edx + buffer - (local+2)] (edx = local + 2 * (i + 1)) and a nop
    BYTE now[8] = {0x66, 0x8B, 0x82};
    int32_t d = (int32_t)((uintptr_t)g_bufA - (kLocal + 2));
    memcpy(now + 3, &d, 4);
    now[7] = 0x90;
    Patch(0x4DD5CE, rd, now, 8);
    d = (int32_t)((uintptr_t)g_bufB - (kLocal + 2));
    memcpy(now + 3, &d, 4);
    Patch(0x535C34, rd, now, 8);
    g_selMax = kMax;
    g_syncIds = kSync;
    g_localIds = kLocal;
    bool net = ChainSlot(0xA4C9A4 + 4, 0x6AFC00, (void*)NetPush, (void**)&g_push) &&
               ChainSlot(0xA4C9A4 + 0x14, 0x6B0430, (void*)NetFlush, (void**)&g_flush);
    if (!net) Log("big-selection: unexpected network queue; large selections may not fit a turn");
    Log("big-selection on: up to %d units", kMax);
}
