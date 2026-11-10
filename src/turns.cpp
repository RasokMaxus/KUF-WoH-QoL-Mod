// turns.cpp - shorter order delay (settings single-player-turn and network-speed).
//
// The game runs in ticks of 30 ms ([0xB3BAE8]) and carries out orders in "turns" of N ticks: an
// order waits for the end of the current turn and then one more turn (lockstep, so every PC in a
// network game carries it out on the same tick). N comes from the word table 0xB3BAF8 {6, 10, 14}
// at index word 0xB3BAF4: single player uses index 0 (set at match start, 0x4BF6F3), network
// games index 1, changed by the Network Speed slider (order handled at 0x4DC66B). Measured with
// perf-trace: single player 180-350 ms per order, network default 300-600 ms.
//
// - network-speed: the three Network Speed choices, written into the table at startup. Every
//   reader (game loop, slider, "Network Latency %d Tick" text, network setup 0x58ECAA) sees them.
// - single-player-turn: the game loop reads the turn length at 0x4CF375 and 0x4CFBD5
//   (movsx eax, word [eax*2+0xB3BAF8]); both reads go through TurnTicks, which returns this
//   value outside network games (net flag byte 0xB83AD0 == 0) and the table entry otherwise.
#include "kufmod.h"

int g_spTurn = 1;                      // single-player-turn (ticks); 6 = the game's own
int g_netTurns[3] = {3, 6, 10};        // network-speed (ticks for Low / High / Extra high)

static const BYTE* g_netFlag;
static const int16_t* g_table;

// in: eax = table index; out: eax = turn length in ticks. Keeps every other register and the
// flags do not matter at either site (the next instruction sets them).
__declspec(naked) static void TurnTicks() {
    __asm {
        push ecx
        mov ecx, g_netFlag
        cmp byte ptr [ecx], 0
        jne net
        mov eax, g_spTurn
        pop ecx
        ret
    net:
        mov ecx, g_table
        movsx eax, word ptr [ecx + eax * 2]
        pop ecx
        ret
    }
}

// network-speed-start: the Network Speed choice a network match starts with (0 Low, 1 High,
// 2 Extra high; the game's own is 1). Match setup stores the index at 0x4BF6F3
// (mov word [0xB3BAF4], ax; ax = 0 single player, 1 network). Every PC runs it, so they agree.
// The in-game options dialog keeps its own copy (dword 0xBED25C, shown on open and sent back as
// the order 0x3EE on close), so it is set too.
int g_netStart = 0;
static uintptr_t g_startResume;
static int16_t* g_index;
static int32_t* g_dialogIndex;

__declspec(naked) static void StartHook() {
    __asm {
        push ecx
        mov ecx, g_netFlag
        cmp byte ptr [ecx], 0
        je store
        mov eax, g_netStart
        mov ecx, g_dialogIndex
        mov dword ptr [ecx], eax
    store:
        mov ecx, g_index
        mov word ptr [ecx], ax
        pop ecx
        jmp dword ptr [g_startResume]
    }
}

void InstallTurns() {
    g_netFlag = (const BYTE*)(0xB83AD0 + g_slide);
    g_table = (const int16_t*)(0xB3BAF8 + g_slide);
    for (int& t : g_netTurns) t = t < 1 ? 1 : t > 30 ? 30 : t;
    static const int16_t original[3] = {6, 10, 14};
    int16_t wanted[3] = {(int16_t)g_netTurns[0], (int16_t)g_netTurns[1], (int16_t)g_netTurns[2]};
    if (memcmp(original, wanted, sizeof wanted)) {
        if (Patch(0xB3BAF8, original, wanted, sizeof wanted))
            Log("network-speed %d, %d, %d ticks", wanted[0], wanted[1], wanted[2]);
        else
            Log("unexpected game data at 0xB3BAF8 (different game version?). network-speed unchanged.");
    }
    g_index = (int16_t*)(0xB3BAF4 + g_slide);
    g_dialogIndex = (int32_t*)(0xBED25C + g_slide);
    if (g_netStart < 0 || g_netStart > 2) g_netStart = 0;
    if (g_netStart != 1) {
        BYTE expected[6] = {0x66, 0xA3};  // mov word ptr [0xB3BAF4], ax
        *(uint32_t*)(expected + 2) = 0xB3BAF4 + (uint32_t)g_slide;
        if (Hook(0x4BF6F3, expected, sizeof expected, (void*)StartHook, &g_startResume))
            Log("network-speed-start %d", g_netStart);
        else
            Log("unexpected game code at 0x4BF6F3 (different game version?). network-speed-start off.");
    }
    if (g_spTurn < 1) g_spTurn = 1;
    if (g_spTurn >= 6) return;  // the game's own
    BYTE expected[8] = {0x0F, 0xBF, 0x04, 0x45};
    *(uint32_t*)(expected + 4) = 0xB3BAF8 + (uint32_t)g_slide;  // absolute address is relocated
    BYTE site1[8], site2[8];
    memcpy(site1, (void*)(0x4CF375 + g_slide), 8);
    memcpy(site2, (void*)(0x4CFBD5 + g_slide), 8);
    if (memcmp(site1, expected, 8) || memcmp(site2, expected, 8)) {
        Log("unexpected game code at 0x4CF375/0x4CFBD5 (different game version?). single-player-turn off.");
        return;
    }
    HookCall(0x4CF375, expected, 8, (void*)TurnTicks);
    HookCall(0x4CFBD5, expected, 8, (void*)TurnTicks);
    Log("single-player-turn %d ticks", g_spTurn);
}
