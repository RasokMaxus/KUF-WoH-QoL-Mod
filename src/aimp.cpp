// aimp.cpp - computer players in multiplayer. Notes: docs/AI-MP.md.
//
// The computer player's script changes units directly (it does not send orders through the
// network command queue), so it runs on every PC: every PC adds the same computer players at
// match start, and the game's lockstep keeps them in step (the scripts use the game's synced
// random numbers through kufmain.Random).
//
// Room: in an empty slot's name box the host can pick COMPUTER (the mod adds that entry). The
// slot is then closed for the game (nobody can join it) and the mod shows COMPUTER, the race box
// (Human / Devil / Random, host only) and the team button (host clicks: none, A ... H). The host's
// mod sends the list as a hidden room line "#KUFC I slot:race:team ..." (race 1000 Human,
// 2000 Devil, 4000 random; team 0 none, 1-8 = A-H), again whenever the room's players change.
// Match start (0x4C7E0D, where the map's start positions are known): the computer players are
// added after the n network players in slot order: slot record type 2, race, team, name
// "Computer", alliances with same-team players, player count + k. Then the start-of-match script
// call, which the game skips in network games, is started at the computer's first simulation step.
#include "kufmod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <share.h>

static FILE* g_pyTrace;          // AI script call trace (lan-trace, InstallPyTrace)
static volatile LONG g_pyLines;
bool g_mpComputers = true;  // setting multiplayer-computers
int g_aiFirst = 99;         // first computer player index in this match (colors are automatic from here)

static const int SLOTS = 8;      // room slots
static const int ROOM_SLOT = 816;  // slot i controls: +0 team button, +1 name box, +4 race box
static const char COMPUTER[] = "COMPUTER";
struct AiSlot { int race; int team; };  // race 0 = not a computer
static AiSlot g_ai[SLOTS];
static bool g_touched[SLOTS];  // we changed this slot's race box / team button
static void* g_started[8];     // computer players whose script was started (StartScript)
static unsigned g_stepCount;   // computer player steps this match (sync check)

static const char* RaceName(int race) {
    return race == 1000 ? "Human" : race == 2000 ? "Devil" : "random";
}
static int RaceIndex(int race) { return race == 1000 ? 0 : race == 2000 ? 1 : 2; }  // HUMAN DEVIL RANDOM
static const int RACE_OF_INDEX[3] = {1000, 2000, 4000};

void AiRoomReset() { memset(g_ai, 0, sizeof g_ai); }

void AiHostSend() {
    char text[160];
    int len = sprintf_s(text, "#KUFC I");
    for (int i = 0; i < SLOTS; i++)
        if (g_ai[i].race) len += sprintf_s(text + len, sizeof text - len, " %d:%d:%d", i, g_ai[i].race, g_ai[i].team);
    SendRoomLine(text);
}

// A room chat line. Returns 0 if it is not ours, 2 if handled and hidden.
int AiRoomChat(const char* sender, const char* text) {
    (void)sender;
    if (!g_mpComputers || strncmp(text, "#KUFC I", 7)) return 0;
    AiSlot list[SLOTS] = {};
    const char* p = text + 7;
    for (;;) {
        int slot, race, team, used = 0;
        if (sscanf_s(p, " %d:%d:%d%n", &slot, &race, &team, &used) != 3 || !used) break;
        p += used;
        if (slot >= 0 && slot < SLOTS && (race == 1000 || race == 2000 || race == 4000) && team >= 0 && team <= 8)
            list[slot] = {race, team};
    }
    if (!IsHost()) memcpy(g_ai, list, sizeof g_ai);  // the host keeps its own list
    return 2;
}

// ------------------------------------------------------------------ room window

static int ComboFind(HWND box, const char* text) {
    LRESULT n = SendMessageA(box, CB_GETCOUNT, 0, 0);
    for (int j = 0; j < n; j++) {
        char item[64] = "";
        if (SendMessageA(box, CB_GETLBTEXTLEN, j, 0) < (LRESULT)sizeof item) SendMessageA(box, CB_GETLBTEXT, j, (LPARAM)item);
        if (!_stricmp(item, text)) return j;
    }
    return -1;
}

static int SlotState(int slot) {  // the game's: 0 OPEN, 1 CLOSE, 2 OBSERVER, 3 player
    uintptr_t dialog = Game<uintptr_t>(0xBED22C);
    return dialog ? *(int*)(dialog + 0x3D0 + slot * 4) : -1;
}

// Called every 200 ms on the room's UI thread: shows the computer slots (the game redraws its
// boxes now and then) and gives the host the COMPUTER entry in empty slots.
void AiRoomSync(HWND room) {
    if (!g_mpComputers) return;
    bool host = IsHost();
    for (int i = 0; i < SLOTS; i++) {
        HWND name = GetDlgItem(room, ROOM_SLOT + 10 * i + 1);
        HWND race = GetDlgItem(room, ROOM_SLOT + 10 * i + 4);
        HWND team = GetDlgItem(room, ROOM_SLOT + 10 * i + 0);
        if (!name || !race || !team) continue;
        if (!g_ai[i].race) {
            if (g_touched[i]) {  // was a computer: give the boxes back to the game
                g_touched[i] = false;
                SendMessageA(race, CB_RESETCONTENT, 0, 0);
                EnableWindow(race, FALSE);
                SetWindowTextA(team, "");
                int c = ComboFind(name, COMPUTER);
                if (c >= 0) SendMessageA(name, CB_DELETESTRING, c, 0);
                InvalidateRect(name, NULL, TRUE);
            }
            // The game edits these entries by position (OPEN 0, CLOSE 1, OBSERVER 2, a player's
            // name 3), so COMPUTER is only there while the host has the list open (AiRoomCommand,
            // CBN_DROPDOWN); once it is closed again, take it out.
            int c = ComboFind(name, COMPUTER);
            if (c >= 0 && !SendMessageA(name, CB_GETDROPPEDSTATE, 0, 0)) {
                LRESULT sel = SendMessageA(name, CB_GETCURSEL, 0, 0);
                SendMessageA(name, CB_DELETESTRING, c, 0);
                if (sel >= 0 && sel != c) SendMessageA(name, CB_SETCURSEL, sel, 0);
            }
            continue;
        }
        g_touched[i] = true;
        int c = ComboFind(name, COMPUTER);
        if (c < 0) c = (int)SendMessageA(name, CB_ADDSTRING, 0, (LPARAM)COMPUTER);
        if (SendMessageA(name, CB_GETCURSEL, 0, 0) != c) SendMessageA(name, CB_SETCURSEL, c, 0);
        if (SendMessageA(race, CB_GETCOUNT, 0, 0) != 3 || ComboFind(race, "HUMAN") != 0) {
            SendMessageA(race, CB_RESETCONTENT, 0, 0);
            SendMessageA(race, CB_ADDSTRING, 0, (LPARAM)"HUMAN");
            SendMessageA(race, CB_ADDSTRING, 0, (LPARAM)"DEVIL");
            SendMessageA(race, CB_ADDSTRING, 0, (LPARAM)"RANDOM");
        }
        if (SendMessageA(race, CB_GETCURSEL, 0, 0) != RaceIndex(g_ai[i].race))
            SendMessageA(race, CB_SETCURSEL, RaceIndex(g_ai[i].race), 0);
        if ((IsWindowEnabled(race) != FALSE) != host) EnableWindow(race, host);
        char want[2] = {g_ai[i].team ? (char)('A' + g_ai[i].team - 1) : 'X', 0}, now[8] = "";
        GetWindowTextA(team, now, sizeof now);
        if (strcmp(now, want)) SetWindowTextA(team, want);
    }
}

// WM_COMMAND in the room window. Returns true if the mod handled it (the game must not see it).
bool AiRoomCommand(HWND room, WPARAM w, LPARAM l, WNDPROC original) {
    if (!g_mpComputers) return false;
    int id = LOWORD(w), code = HIWORD(w);
    if (id < ROOM_SLOT || id >= ROOM_SLOT + 10 * SLOTS) return false;
    int slot = (id - ROOM_SLOT) / 10, part = (id - ROOM_SLOT) % 10;
    bool host = IsHost();
    if (part == 1 && code == CBN_DROPDOWN && host && !g_ai[slot].race) {
        // The host opens an empty slot's list (OPEN, CLOSE, OBSERVER): offer COMPUTER too.
        HWND name = (HWND)l;
        if (SendMessageA(name, CB_GETCOUNT, 0, 0) == 3 && ComboFind(name, "OPEN") == 0 && ComboFind(name, COMPUTER) < 0)
            SendMessageA(name, CB_ADDSTRING, 0, (LPARAM)COMPUTER);
        return false;
    }
    if (part == 1 && code == CBN_SELCHANGE && host) {
        HWND name = (HWND)l;
        int sel = (int)SendMessageA(name, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel == ComboFind(name, COMPUTER)) {
            if (!g_ai[slot].race) {
                g_ai[slot] = {4000, 0};
                Log("room: slot %d -> computer", slot);
                if (SlotState(slot) == 0) {  // OPEN: close it for the game, so nobody joins
                    SendMessageA(name, CB_SETCURSEL, 1, 0);
                    CallWindowProcA(original, room, WM_COMMAND, w, l);
                }
                AiHostSend();
                AiRoomSync(room);
            }
            return true;
        }
        if (g_ai[slot].race) {  // OPEN / CLOSE / OBSERVER: no longer a computer; the game does the rest
            g_ai[slot] = {};
            Log("room: slot %d -> not a computer", slot);
            AiHostSend();
        }
        return false;
    }
    if (!g_ai[slot].race || (part != 0 && part != 4)) return false;
    if (host && part == 4 && code == CBN_SELCHANGE) {
        int sel = (int)SendMessageA((HWND)l, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < 3) {
            g_ai[slot].race = RACE_OF_INDEX[sel];
            Log("room: slot %d computer race %s", slot, RaceName(g_ai[slot].race));
            AiHostSend();
        }
    } else if (host && part == 0 && code == BN_CLICKED) {
        g_ai[slot].team = (g_ai[slot].team + 1) % 9;
        Log("room: slot %d computer team %d", slot, g_ai[slot].team);
        AiHostSend();
        AiRoomSync(room);
    }
    return true;  // the game's own handlers must not act on a computer slot
}

// ------------------------------------------------------------------ match start

typedef void(__thiscall* AssignFn)(void* cstring, const char* text, int length);

// ebp = the frame of the game-start function 0x4C77D0: start positions at ebp-0x2C8
// (11 x {player or -1, ?}, filled from the map). Runs before the player count is read.
extern "C" void __cdecl AiBeforePositions(uintptr_t ebp) {
    g_aiFirst = 99;
    g_stepCount = 0;
    memset(g_started, 0, sizeof g_started);
    if (g_pyTrace) {  // research trace: one match per file
        fflush(g_pyTrace);
        _chsize_s(_fileno(g_pyTrace), 0);
        rewind(g_pyTrace);
        g_pyLines = 0;
    }
    AiSlot list[SLOTS];
    int count = 0;
    for (int i = 0; i < SLOTS; i++)
        if (g_ai[i].race) list[count++] = g_ai[i];
    if (!g_mpComputers || Game<uint8_t>(0xB83AD0) != 1 || Game<uint16_t>(0xB83AD2) != 0 ||
        Game<uint8_t>(0xB83AD8) == 1 || count == 0)
        return;
    int positions = 0;
    for (int i = 0; i < 11; i++)
        if (*(int*)(ebp - 0x2C8 + i * 8) != -1) positions++;
    int humans = Game<int16_t>(0xB83AD6) - 1;
    int k = count;
    if (k > positions - humans) k = positions - humans;
    if (k > 8 - humans) k = 8 - humans;
    if (k <= 0) {
        Log("match start: no room for computer players (map has %d start positions, %d players)", positions, humans);
        return;
    }
    if (k < count) Log("match start: only %d of %d computer players fit on this map", k, count);
    uintptr_t game = Game<uintptr_t>(0xB8282C);
    typedef void(__cdecl * AllyFn)(int player, int other, int state);  // 0x4E7FC0: 0 ally, 1 enemy
    AllyFn ally = (AllyFn)(0x4E7FC0 + g_slide);
    static const char NAME[] = "Computer";
    for (int j = 0; j < k; j++) {
        int idx = humans + j;
        BYTE* rec = (BYTE*)(0xB83CDC + g_slide + idx * 0x40C);
        *(int*)(rec + 0) = 2;              // computer
        *(int*)(rec + 4) = list[j].race;   // 1000 / 2000 / 4000 (random: the game picks, synced)
        rec[8] = 1;  // still in the game (the win/lose check 0x4C0A60 only counts players with this set)
        *(BYTE*)(game + 0x5E0 + idx) = (BYTE)list[j].team;  // 0 = no team
        ((AssignFn)(0x429000 + g_slide))((void*)(game + 0x500 + idx * 4), NAME, (int)sizeof NAME - 1);
        Log("match start: computer player %d (%s, team %d)", idx, RaceName(list[j].race), list[j].team);
    }
    // Alliances, as the game does for the network players (0x58EEA0): same team (not 0) = allies.
    int total = humans + k;
    for (int a = 0; a < total; a++)
        for (int b = humans; b < total; b++) {
            if (a == b) continue;
            BYTE ta = *(BYTE*)(game + 0x5E0 + a), tb = *(BYTE*)(game + 0x5E0 + b);
            if (ta && ta == tb) {
                ally(a, b, 0);
                ally(b, a, 0);
                Log("match start: players %d and %d are allies (team %d)", a, b, ta);
            }
        }
    *(int16_t*)(0xB83AD6 + g_slide) = (int16_t)(total + 1);
    *(int*)(game + 0x5E8) = total;  // players in the in-game lists (alliance screen, score): the computers too
    g_aiFirst = humans;
}

static uintptr_t g_positionsResume, g_playerCount;

// Replaces, at 0x4C7E0D: movsx eax, word ptr [0xB83AD6]
__declspec(naked) static void PositionsHook() {
    __asm {
        pushad
        push ebp
        call AiBeforePositions
        add esp, 4
        popad
        mov eax, dword ptr [g_playerCount]
        movsx eax, word ptr [eax]
        jmp dword ptr [g_positionsResume]
    }
}

// ------------------------------------------------------------------ sync check
// The game compares only the order streams between PCs, not the game state, so a computer
// player that behaves differently on two PCs would go unnoticed. Every computer player step
// (0x5D1150, called from the player updates at 0x53E63E and 0x525C8E) happens at the same simulation step on
// every PC: every 512 steps we log the game's random seed and how many random numbers were drawn.
// The lines must be equal on every PC; if they differ, the game has gone out of sync.
static uintptr_t g_aiStep;

// lan-trace: the AI scripts' Python errors (normally printed to an invisible stderr) go to
// pyerr.txt (pyerr2.txt for the second copy). Python 1.5: python15.dll, already initialized here.
static void PythonErrorsToFile() {
    HMODULE py = GetModuleHandleA("python15.dll");
    typedef int(__cdecl * RunFn)(const char*);
    RunFn run = py ? (RunFn)GetProcAddress(py, "PyRun_SimpleString") : NULL;
    if (!run) return;
    typedef void(__cdecl * InitFn)();  // the game initializes Python lazily; a second call does nothing
    InitFn init = (InitFn)GetProcAddress(py, "Py_Initialize");
    if (init) init();
    char path[MAX_PATH], code[MAX_PATH + 128];
    sprintf_s(path, g_instance > 1 ? "%spyerr%d.txt" : "%spyerr.txt", g_dir, g_instance);
    for (char* c = path; *c; c++)
        if (*c == '\\') *c = '/';
    sprintf_s(code, "import sys\nsys.stderr = open('%s', 'w', 0)\n", path);
    Log("python errors -> %s (%d)", path, run(code));
}

// lan-trace: writes every computer player's plan and its cells' state to pyerr.txt (attribute reads
// only: nothing that could change the game).
static void DumpAiPlans() {
    // The halls: kufmain's EMPTY_ROOM test (0x5C7C3B) wants state (vtable +0x14) not 0x27D8, 0x2A30,
    // 0x28A0 and the word at +0x14B0 == 0. Objects: [0xB870C0]+0x2C38, +4 type, +8 owner.
    uintptr_t world = Game<uintptr_t>(0xB870C0);
    typedef uint16_t(__thiscall * StateFn)(void* obj);
    for (int i = 0; world && i < 8000; i++) {
        BYTE* obj = *(BYTE**)(world + 0x2C38 + i * 4);
        if (!obj) continue;
        int16_t type = *(int16_t*)(obj + 4), owner = *(int16_t*)(obj + 8);
        if (type != 46 && type != 47 && type != 48 && type != 62 && type != 63 && type != 64) continue;
        StateFn state = *(StateFn*)(*(uintptr_t*)obj + 0x14);
        Log("hall check: step %u object %d type %d owner %d state 0x%X room %d byte69 %d", g_stepCount, i, type,
            owner, state(obj), *(int16_t*)(obj + 0x14B0), obj[0x69]);
    }
    HMODULE py = GetModuleHandleA("python15.dll");
    typedef int(__cdecl * RunFn)(const char*);
    RunFn run = py ? (RunFn)GetProcAddress(py, "PyRun_SimpleString") : NULL;
    if (!run) return;
    typedef void(__cdecl * InitFn)();  // the game initializes Python lazily; a second call does nothing
    InitFn init = (InitFn)GetProcAddress(py, "Py_Initialize");
    if (init) init();
    char code[4096];
    sprintf_s(code,
              "import sys\n"
              "sys.stderr.write('--- step %u\\n')\n"
              "m = sys.modules.get('main')\n"
              "if m is not None:\n"
              "    for pid in range(1, 5):\n"
              "        b = m.Using_Lock_Stack_List(pid, m.Object_Type['OT_TYPESELECT'] | m.Object_Type['BUILDING'], m.CBuild['ANYONE'])\n"
              "        c = m.Using_Lock_Stack_List(pid, m.Object_Type['OT_TYPESELECT'] | m.Object_Type['CHAR'], m.CBuild['ANYONE'])\n"
              "        sys.stderr.write('query player %%d: buildings %%s units %%s\\n' %% (pid, repr(b), repr(c)))\n"
              "for n in sys.modules.keys():\n"
              "    m = sys.modules[n]\n"
              "    if m is None or not hasattr(m, 'RunAI'): continue\n"
              "    r = m.RunAI\n"
              "    sys.stderr.write('module %%s RunAI %%s\\n' %% (n, repr(type(r))))\n"
              "    keys = range(len(r))\n"
              "    if type(r) == type({}): keys = r.keys()\n"
              "    for i in keys:\n"
              "        p = r[i]\n"
              "        if type(p) == type(0): continue\n"
              "        sys.stderr.write(' player %%s plan %%s\\n' %% (i, p.__class__.__name__))\n"
              "        d = p.Manager.__dict__\n"
              "        for k in d.keys():\n"
              "            sys.stderr.write('  M.%%s = %%s\\n' %% (k, repr(d[k])[:200]))\n"
              "        for c in p.Ai_Cell_Console_List:\n"
              "            sys.stderr.write('  %%s %%s\\n' %% (c.__class__.__name__, repr(c.__dict__)[:400]))\n",
              g_stepCount);
    int rc = run(code);
    if (rc) Log("AI plan dump failed (%d)", rc);
}

// The computer players' scripts start at their first simulation step, not with the game's own
// start-of-match call (0x4C8959 / 0x4C96E7, which skips them in network games): in a network game
// that call comes before the players' start buildings exist, and a script that finds no buildings
// picks its island plan ("No_Land") and never builds. Every PC starts them at the same step.

static void DumpAiPlans();
static void StartScript(void* ai) {
    if (g_aiFirst == 99 || !ai || *(uintptr_t*)ai) return;  // not ours, or the script is loaded
    // Wait until the player's building list (world [0xB870C0] + 0x2EC44 + player * 16, a vector:
    // begin, end; the one kufmain's searches use) has its start buildings.
    typedef int(__thiscall * IndexFn)(void* player);  // 0x522A10
    void* player = *(void**)((BYTE*)ai + 4);
    uintptr_t world = Game<uintptr_t>(0xB870C0);
    if (!player || !world) return;
    int index = ((IndexFn)(0x522A10 + g_slide))(player);
    if (index < 0 || index > 10) return;
    uintptr_t* list = (uintptr_t*)(world + 0x2EC44 + index * 16);
    if (list[1] <= list[0]) return;
    for (void*& p : g_started) {
        if (p == ai) return;
        if (!p) {
            p = ai;
            if (g_lanTrace) DumpAiPlans();  // what the script will see (lan-trace)
            typedef void(__thiscall * RunFn)(void* ai, const char* function);  // 0x5D1300
            ((RunFn)(0x5D1300 + g_slide))(ai, (const char*)(0xA35CE4 + g_slide));  // "main"
            Log("computer player %d script started at step %u", index, g_stepCount);
            return;
        }
    }
}

// ------------------------------------------------------------------ end of the match
// A network game ends only through "out of the game" orders (0x64 / 0x320 / player), which each
// PC sends for its own player: when it has nothing left (0x4C0C70), or when all other network
// players have left (0x4CFEC9, "last one standing"). The order's handler (0x4DBC88) then shows
// victory if none of the player's enemies is still in the game (slot record +8), else defeat.
// Computer players have no PC of their own, so:
//  - a computer with nothing left is marked out of the game here, at the same step on every PC;
//  - "last one standing" waits while an enemy computer is still in the game (EndCheckHook);
//  - a player with no enemy left in the game sends his "out" order (victory), as single player
//    does (0x4C0D37), also when other humans are still playing on his side.
static BYTE* InGame(int player) { return (BYTE*)(0xB83CE4 + g_slide + player * 0x40C); }

static bool EnemyInGame(uintptr_t game, int me, bool computersOnly) {
    for (int i = computersOnly ? g_aiFirst : 0; i < 11; i++) {
        BYTE* p = *(BYTE**)(game + 0x48 + i * 4);
        if (i == me || !p || p[0xC] != 1 || *InGame(i) != 1) continue;
        if (*(int16_t*)(*(BYTE**)(game + 0x48 + me * 4) + 0x1048 + i * 2) == 1) return true;  // enemy
    }
    return false;
}

static void EndChecks(void* ai) {
    uintptr_t game = Game<uintptr_t>(0xB8282C);
    if (g_aiFirst == 99 || !game || !*(BYTE*)(game + 0x489)) return;
    typedef int(__thiscall * IndexFn)(void* player);
    BYTE* player = *(BYTE**)((BYTE*)ai + 4);
    int index = player ? ((IndexFn)(0x522A10 + g_slide))(player) : -1;
    if (index >= g_aiFirst && index < 8 && *InGame(index) && *(int*)(player + 0xA04 + 0x42C) <= 0) {
        *InGame(index) = 0;  // nothing left: out of the game (every PC, same step)
        Log("computer player %d is out of the game (step %u)", index, g_stepCount);
    }
    int me = *(int*)(game + 0x664);
    if (me < 0 || me > 10 || *InGame(me) != 1 || *(BYTE*)(game + 0x94)) return;
    if (EnemyInGame(game, me, false)) return;
    typedef void(__thiscall * SendFn)(uintptr_t game, int type, int kind, int player);  // game vtable +0x64
    Log("no enemy left in the game: player %d wins (step %u)", me, g_stepCount);
    (*(SendFn*)(*(uintptr_t*)game + 0x64))(game, 0x64, 0x320, me);
    *(BYTE*)(game + 0x94) = 1;
}

// Replaces, at 0x4CFEC9 (all other network players have left): cmp byte ptr [ebx+0x94], 0
static uintptr_t g_lastStandingResume, g_lastStandingSkip;

extern "C" int __cdecl KeepPlaying(uintptr_t game) {
    if (g_aiFirst == 99) return 0;
    int me = *(int*)(game + 0x664);
    return me >= 0 && me <= 10 && EnemyInGame(game, me, true);
}

__declspec(naked) static void LastStandingHook() {
    __asm {
        pushad
        push ebx
        call KeepPlaying
        add esp, 4
        test eax, eax
        popad
        jnz keep
        cmp byte ptr [ebx + 0x94], 0
        jmp dword ptr [g_lastStandingResume]
    keep:
        jmp dword ptr [g_lastStandingSkip]
    }
}

extern "C" void __cdecl AiStepCheck(void* ai) {
    EndChecks(ai);
    // The step counter and (with lan-trace) the dumps also run in single player, to compare with a
    // computer player there; the sync check only in network games with computer players.
    if (++g_stepCount == 1 && g_lanTrace) PythonErrorsToFile();
    StartScript(ai);
    if (g_lanTrace && (g_stepCount == 200 || g_stepCount == 2000 || g_stepCount == 6000)) DumpAiPlans();
    if (g_aiFirst == 99 || g_stepCount % 512) return;
    Log("sync check: step %u seed %08X draws %u", g_stepCount, Game<uint32_t>(0xBED1E8), Game<uint32_t>(0xB827DC));
}

// Replaces, at 0x53E63E and 0x525C8E: call 0x5D1150 (thiscall, ecx = the AI object)
__declspec(naked) static void AiStepHook() {
    __asm {
        pushad
        push ecx
        call AiStepCheck
        add esp, 4
        popad
        jmp dword ptr [g_aiStep]
    }
}

// ------------------------------------------------------------------ order trace (research)

typedef int(__fastcall* PushFn)(void* queue, void* edx, const uint16_t* order, int length, int target);
static PushFn g_localPush, g_netPush;
static volatile LONG g_logged;

static void LogOrder(const char* kind, const uint16_t* order, int length, int target) {
    if (InterlockedIncrement(&g_logged) > 3000) return;
    char hex[64] = "";
    int n = length < 24 ? length : 24;
    for (int i = 0; i < n; i++) sprintf_s(hex + i * 2, 3, "%02X", ((const BYTE*)order)[i]);
    Log("order %s: cmd 0x%X player %d len %d target %d [%s] tick %u", kind, order[0], (int16_t)order[1],
        length, target, hex, GetTickCount());
}

static int __fastcall LocalPush(void* q, void* edx, const uint16_t* order, int length, int target) {
    if (order && length >= 4) LogOrder("local", order, length, target);
    return g_localPush(q, edx, order, length, target);
}

static int __fastcall NetPush(void* q, void* edx, const uint16_t* order, int length, int target) {
    if (order && length >= 4) LogOrder("net", order, length, target);
    return g_netPush(q, edx, order, length, target);
}

static bool PatchSlot(uintptr_t slotVa, uintptr_t expected, void* replacement, PushFn* original) {
    uintptr_t want = expected + g_slide, repl = (uintptr_t)replacement;
    *original = (PushFn)want;
    return Patch(slotVa, &want, &repl, 4);
}

// ------------------------------------------------------------------ AI script trace (research)
// Wraps some kufmain functions (method table 0xB47B98: {name, function, flags, doc} x 120) and
// logs each call's arguments and result to pytrace.txt (pytrace2.txt for a second copy), in single
// player and multiplayer, so a working and a stuck computer player can be compared.
typedef void* (__cdecl* PyFn)(void* self, void* args);
typedef void* (__cdecl* ReprFn)(void* obj);
typedef const char* (__cdecl* StrFn)(void* str);
static ReprFn g_repr;
static StrFn g_str;
static const char* const TRACED[] = {"Train", "Command", "CommandBuild", "CommandAttack", "GetMakeState",
                                     "IsRaisePeasant", "GetStatus", "SelectBuild", "SelectUnit", "SelectPlayer",
                                     "GetCurrentPlayerIndex", "GetPlayerNumber", "GetRace", "GetResValue",
                                     "GetNumber", "GetLive", "IsAllyState", "IsBuildingHere", "GetBuildBuilding",
                                     "SearchingNearObjectEX", "SearchingNearObject", "GetCoordinate",
                                     "GetTargetPositionDistance", "GetTargetPositionDistanceEX", "GetDistanceNarrow"};
static const int NTRACED = sizeof TRACED / sizeof TRACED[0];
static PyFn g_pyOrig[NTRACED];

static void PyTrace(int i, void* args, void* result) {
    if (!g_pyTrace || InterlockedIncrement(&g_pyLines) > 150000) return;
    char a[200] = "?", r[120] = "NULL";
    if (args) {
        void* s = g_repr(args);
        if (s) strncpy_s(a, g_str(s), _TRUNCATE);  // (leaks the small repr string; lan-trace only)
    }
    if (result) {
        void* s = g_repr(result);
        if (s) strncpy_s(r, g_str(s), _TRUNCATE);
    }
    typedef int(__thiscall * IndexFn)(void* player);  // 0x522A10: a player's index
    void* player = (void*)Game<uintptr_t>(0xBEDC88);   // the computer player whose script runs
    int p = player ? ((IndexFn)(0x522A10 + g_slide))(player) : -1;
    fprintf(g_pyTrace, "%u %u p%d %s%s -> %s\n", GetTickCount() / 1000, g_stepCount, p, TRACED[i], a, r);
}

template <int I> static void* __cdecl PyWrap(void* self, void* args) {
    void* result = g_pyOrig[I](self, args);
    PyTrace(I, args, result);
    return result;
}

template <int I> struct PyWraps {
    static void Fill(PyFn* out) { out[I] = PyWrap<I>; PyWraps<I - 1>::Fill(out); }
};
template <> struct PyWraps<-1> {
    static void Fill(PyFn*) {}
};

static void InstallPyTrace() {
    HMODULE py = GetModuleHandleA("python15.dll");
    if (!py) py = LoadLibraryA("python15.dll");
    g_repr = py ? (ReprFn)GetProcAddress(py, "PyObject_Repr") : NULL;
    g_str = py ? (StrFn)GetProcAddress(py, "PyString_AsString") : NULL;
    if (!g_repr || !g_str) return;
    char path[MAX_PATH];
    sprintf_s(path, g_instance > 1 ? "%spytrace%d.txt" : "%spytrace.txt", g_dir, g_instance);
    g_pyTrace = _fsopen(path, "w", _SH_DENYNO);  // readable while the game runs
    if (!g_pyTrace) return;
    setvbuf(g_pyTrace, NULL, _IOLBF, 4096);
    PyFn wraps[NTRACED];
    PyWraps<NTRACED - 1>::Fill(wraps);
    int done = 0;
    for (int e = 0; e < 120; e++) {
        uintptr_t entry = 0xB47B98 + g_slide + e * 16;
        const char* name = *(const char**)entry;
        for (int i = 0; i < NTRACED; i++) {
            if (!name || strcmp(name, TRACED[i]) || g_pyOrig[i]) continue;
            PyFn orig = *(PyFn*)(entry + 4);
            g_pyOrig[i] = orig;
            if (Patch(entry + 4 - g_slide, &orig, &wraps[i], 4)) done++;
        }
    }
    Log("AI script trace: %d functions -> %s", done, path);
}

// ------------------------------------------------------------------ the map's AI data
// The game loads the map's AI data (0x5D4030, called at 0x4A390E and 0x4A4B89 while the map loads;
// it fills among others the unit flag table 0xBF12A0 that kufmain's searches use) and updates it
// every frame (0x5D4820 at 0x4CD6E0) only when it is not a network game. Without it every type
// search of a computer player's script finds nothing (it then thinks it has no buildings, picks its
// island plan and never builds). Each gate is "cmp byte [0xB83AD0], al / jne skip": we let it
// through in network games that have computer players.
static const uintptr_t GATE_SITE[3] = {0x4A3900, 0x4A4B7B, 0x4CD6D2};
static const uintptr_t GATE_SKIP[3] = {0x4A3913, 0x4A4B8E, 0x4CD6E5};
static uintptr_t g_gateRun[3], g_gateSkip[3];

extern "C" int __cdecl AiGateOpen(int which) {
    if (Game<uint8_t>(0xB83AD0) == 0) return 1;  // not a network game: as before
    if (!g_mpComputers) return 0;
    if (which == 2) return g_aiFirst != 99;      // every frame: this match has computer players
    for (const AiSlot& a : g_ai)                 // map load: the room's list (same on every PC)
        if (a.race) return 1;
    return 0;
}

#define GATE_STUB(N)                                  \
    __declspec(naked) static void GateStub##N() {     \
        __asm { pushad }                              \
        __asm { push N }                              \
        __asm { call AiGateOpen }                     \
        __asm { add esp, 4 }                          \
        __asm { test eax, eax }                       \
        __asm { popad }                               \
        __asm { jnz run }                             \
        __asm { jmp dword ptr [g_gateSkip + N * 4] }  \
        __asm { run: jmp dword ptr [g_gateRun + N * 4] } \
    }
GATE_STUB(0)
GATE_STUB(1)
GATE_STUB(2)

static bool InstallAiDataGates() {
    BYTE expected[8] = {0x38, 0x05};
    uintptr_t flag = 0xB83AD0 + g_slide;
    memcpy(expected + 2, &flag, 4);
    expected[6] = 0x75;
    expected[7] = 0x0B;
    for (int i = 0; i < 3; i++)
        if (memcmp((BYTE*)(GATE_SITE[i] + g_slide), expected, 8)) return false;
    void* stubs[3] = {(void*)GateStub0, (void*)GateStub1, (void*)GateStub2};
    for (int i = 0; i < 3; i++) {
        g_gateSkip[i] = GATE_SKIP[i] + g_slide;
        if (!Hook(GATE_SITE[i], expected, 8, stubs[i], &g_gateRun[i])) return false;
    }
    return true;
}

// The network queue's player state (EventTCPIPCmdQueue vtable slot +0x30, 0x6B0880: 0 = gone,
// 1/2 = playing, 4 = left). Every frame the game clears the "still in the game" flag of a player
// whose state is 0 or 4 (0x4CF89A); a computer player has no network node, so it would read 0.
static PushFn g_nodeState;  // (same calling convention as the push wrapper: thiscall, one int)

static int __fastcall NodeState(void* q, void* edx, int player) {
    if (g_aiFirst != 99 && player >= g_aiFirst && player < 8) return 1;  // a computer player: playing
    typedef int(__fastcall * StateFn)(void*, void*, int);
    return ((StateFn)g_nodeState)(q, edx, player);
}

// lan-trace: log the game-end commands (0x4DBA83 in the order handler 0x4DB870: order word 1 = player,
// word 2 = 0x320 out of the game / 0x384 left / 0x44C defeated, dword at +4 = the player concerned).
static uintptr_t g_endCmdResume;

extern "C" void __cdecl LogEndCommand(uintptr_t ebp) {
    int16_t from = *(int16_t*)(ebp - 0x82A), kind = *(int16_t*)(ebp - 0x828);
    int who = *(int*)(ebp - 0x824);
    char flags[16] = "";
    for (int i = 0; i < 5; i++) flags[i] = (char)('0' + *(BYTE*)(0xB83CE4 + g_slide + i * 0x40C));
    Log("end command: from player %d kind 0x%X player %d (me %d) in-game flags %s", from, kind, who,
        Game<int>(Game<uintptr_t>(0xB8282C) - g_slide + 0x664), flags);
}

__declspec(naked) static void EndCommandHook() {
    __asm {
        pushad
        push ebp
        call LogEndCommand
        add esp, 4
        popad
        movsx eax, word ptr [ebp - 0x828]
        jmp dword ptr [g_endCmdResume]
    }
}

void InstallAiMp() {
    if (g_lanTrace) {  // research traces (docs/AI-MP.md)
        InstallPyTrace();  // AI script calls -> pytrace.txt
        static const BYTE endCmd[7] = {0x0F, 0xBF, 0x85, 0xD8, 0xF7, 0xFF, 0xFF};
        if (!Hook(0x4DBA83, endCmd, 7, EndCommandHook, &g_endCmdResume)) Log("end command log: unexpected code");
        // Orders that enter the command queue:
        // EventCmdQueue (single player) vtable 0xA44488, EventTCPIPCmdQueue vtable 0xA4C9A4.
        bool a = PatchSlot(0xA44488 + 4, 0x58B8E0, (void*)LocalPush, &g_localPush);
        bool b = PatchSlot(0xA4C9A4 + 4, 0x6AFC00, (void*)NetPush, &g_netPush);
        Log("order trace: local %d net %d", a, b);
    }
    if (!g_mpComputers) return;
    g_playerCount = 0xB83AD6 + g_slide;
    BYTE count[7] = {0x0F, 0xBF, 0x05};
    memcpy(count + 3, &g_playerCount, 4);
    static const BYTE stepCall[5] = {0xE8, 0x0D, 0x2B, 0x09, 0x00};
    static const BYTE stepCall2[5] = {0xE8, 0xBD, 0xB4, 0x0A, 0x00};
    g_aiStep = 0x5D1150 + g_slide;
    // Both player classes' updates call the step: 0x53E620 (vtable 0xA41848) and 0x525C70 (0xA3D690).
    if (memcmp((BYTE*)(0x4C7E0D + g_slide), count, 7) || memcmp((BYTE*)(0x53E63E + g_slide), stepCall, 5) ||
        memcmp((BYTE*)(0x525C8E + g_slide), stepCall2, 5)) {
        Log("unexpected game code for computer players (different game version?). multiplayer-computers off.");
        g_mpComputers = false;
        return;
    }
    if (!Hook(0x4C7E0D, count, 7, PositionsHook, &g_positionsResume) ||
        !HookCall(0x53E63E, stepCall, 5, AiStepHook) || !HookCall(0x525C8E, stepCall2, 5, AiStepHook)) {
        Log("computer players: patch failed. multiplayer-computers off.");
        g_mpComputers = false;
        return;
    }
    if (!InstallAiDataGates()) Log("computer players: unexpected code at the AI data gates (0x4A3900 ...)");
    static const BYTE lastStanding[7] = {0x80, 0xBB, 0x94, 0x00, 0x00, 0x00, 0x00};
    g_lastStandingSkip = 0x4CFEF4 + g_slide;
    if (!Hook(0x4CFEC9, lastStanding, 7, LastStandingHook, &g_lastStandingResume))
        Log("computer players: unexpected code at 0x4CFEC9; the game may end while computers play");
    if (!PatchSlot(0xA4C9A4 + 0x30, 0x6B0880, (void*)NodeState, &g_nodeState))
        Log("computer players: unexpected network queue (0xA4C9A4); computer players may count as gone");
    Log("computer players in multiplayer on");
}
