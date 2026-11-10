// kufmod - Kingdom Under Fire: War of Heroes mod, built as dinput8.dll.
//
// The game loads dinput8.dll from its own folder first, so dropping this file next to
// KingdomUnderFire.exe is the whole installation (proxy.cpp passes DirectInput on to Windows).
// At startup it changes the game's code in memory only; no game file is modified. Every change
// first checks the exact bytes it expects, so a different game version is left alone.
// Settings are read from kufmod.yml next to this DLL (written with defaults if missing).
//
// Feature: Standard match mode and empty resource nodes (economy.cpp).
// Feature: grid hotkeys, command hotkeys, button labels and the worker Build button (below;
//          the Build button's icons come from icons.cpp).
//
// Feature: player color selection on the single-player skirmish setup screen and in the
//          multiplayer room (see "multiplayer room" below for how choices are shared).
//
//  1. A watcher thread finds the setup window (class "XWndClass" with player boxes 350-357)
//     and subclasses it, so our code runs on the game's own UI thread.
//  2. We add a color swatch beside each player box. Clicking it opens a palette with "?"
//     (automatic, the game's normal color) and the 8 player colors. A color another player has
//     picked is crossed out.
//  3. At match start the game writes each player's color index to player+0x5C0 (0x4C7E43).
//     We redirect those instructions to ColorHook, which uses the picked colors and moves any
//     automatic player whose normal color was taken onto a free one.
//
// Addresses are the game's own (image base 0x400000); g_slide corrects them for ASLR.

#include "kufmod.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int NUM_COLORS = 8;          // Orange Teal Blue Red Purple Yellow Cyan Olive
static const int NUM_SLOTS = 8;           // setup screen rows, slot = player index
static const int PLAYER_BOX_ID = 350;     // player boxes 350-357, race boxes 360-367
static const int RACE_BOX_ID = 360;
static const UINT WM_KUFMOD_DECORATE = WM_APP + 0x4B;
static const UINT_PTR SYNC_TIMER = 0x4B4D;

uintptr_t g_slide;                        // actual load address - 0x400000
static uintptr_t g_resume;                // where ColorHook returns to
// Chosen color per slot, -1 = automatic ("?"). Single player and the multiplayer room keep their
// own; g_choice points at the one of the screen that is open.
static int g_spChoice[NUM_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int g_mpChoice[NUM_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int* g_choice = g_spChoice;
static bool g_room;  // g_setup is the multiplayer room (CXCreateDialog), not the skirmish setup
static int g_plan[11];                    // color per game slot, computed at match start
static HINSTANCE g_inst;
static HWND g_setup;                      // setup window we subclassed
static WNDPROC g_setupProc;               // its original window procedure
static HWND g_swatch[NUM_SLOTS];
static HWND g_palette;
static int g_paletteSlot = -1;
char g_dir[MAX_PATH];                     // folder of this DLL (the game folder), with trailing '\'
static char g_logPath[MAX_PATH];
int g_instance = 1;

void Log(const char* fmt, ...) {
    FILE* f = NULL;
    if (fopen_s(&f, g_logPath, "a") || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// The game's 32-bit player color table (0x00RRGGBB), indexed by color number.
static COLORREF ColorOf(int c) {
    uint32_t v = ((const uint32_t*)(0xB3E390 + g_slide))[c];
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

// 0xB83AD0 = 1 for network games, word 0xB83AD2 != 0 for campaign missions, 0xB83AD8 = 1 when
// a replay is playing back. Picked colors only apply to a normal single-player skirmish.
static bool SinglePlayerSkirmish() {
    return Game<uint8_t>(0xB83AD0) == 0 && Game<uint16_t>(0xB83AD2) == 0 && Game<uint8_t>(0xB83AD8) != 1;
}

// ---------------------------------------------------------------- match start
// Called once per game slot (slotOffset = 0, 4, ... 0x28). colors[s] is the color the game chose
// for slot s, players[s] the player index in that slot (-1 = none). Returns the color to use.
bool g_mpColors = true;  // setting multiplayer-colors

// A network skirmish (not a campaign mission, not a replay): colors come from the room (g_mpChoice,
// the host's list, the same on every PC).
static bool NetworkSkirmish() {
    return Game<uint8_t>(0xB83AD0) == 1 && Game<uint16_t>(0xB83AD2) == 0 && Game<uint8_t>(0xB83AD8) != 1;
}

extern "C" int __cdecl PickColor(int slotOffset, const int* colors, const int* players) {
    int s = slotOffset / 4;
    if (!g_mpColors && NetworkSkirmish()) return colors[s];
    if (!SinglePlayerSkirmish() && !NetworkSkirmish()) return colors[s];
    const int* choice = SinglePlayerSkirmish() ? g_spChoice : g_mpChoice;
    int first = 0;  // the first start slot with a player (slot 0 can be empty)
    while (first < 10 && players[first] < 0) first++;
    if (s == first) {  // first occupied slot: plan every player at once
        bool used[NUM_COLORS] = {};
        for (int i = 0; i < 11; i++) {
            int p = players[i];
            if (p >= 0 && p < NUM_SLOTS && p < g_aiFirst && choice[p] >= 0) used[choice[p]] = true;
        }
        for (int i = 0; i < 11; i++) {
            int p = players[i], c = colors[i];
            g_plan[i] = c;
            if (p < 0 || p >= NUM_SLOTS) continue;                  // empty slot or neutral player
            if (p < g_aiFirst && choice[p] >= 0) { g_plan[i] = choice[p]; continue; }  // computers: automatic
            if (c >= 0 && c < NUM_COLORS && !used[c]) { used[c] = true; continue; }
            g_plan[i] = -2;                                        // automatic, but color is taken
        }
        for (int i = 0; i < 11; i++) {
            if (g_plan[i] != -2) continue;
            int c = 0;
            while (c < NUM_COLORS && used[c]) c++;
            g_plan[i] = c < NUM_COLORS ? c : colors[i];
            if (c < NUM_COLORS) used[c] = true;
        }
        for (int i = 0; i < 11; i++)
            if (players[i] >= 0) Log("match start: slot %d player %d color %d -> %d", i, players[i], colors[i], g_plan[i]);
    }
    return g_plan[s];
}

// Replaces, at 0x4C7E3C:  mov eax,[ebp+edx-0x270] / mov [ecx+0x5C0],eax
// ebp = the game's stack frame (color array at -0x270, player array at -0x244),
// edx = slot * 4, ecx = the player object.
__declspec(naked) static void ColorHook() {
    __asm {
        push ecx
        push edx
        lea eax, [ebp - 0x244]
        push eax
        lea eax, [ebp - 0x270]
        push eax
        push edx
        call PickColor
        add esp, 12
        pop edx
        pop ecx
        mov [ecx + 0x5C0], eax
        jmp dword ptr [g_resume]
    }
}

static bool WriteCode(BYTE* site, BYTE opcode, void* target, size_t len) {
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = opcode;  // E9 = jmp rel32, E8 = call rel32
    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)target - ((uintptr_t)site + 5));
    memset(site + 5, 0x90, len - 5);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

bool Hook(uintptr_t va, const BYTE* expected, size_t len, void* target, uintptr_t* resume) {
    BYTE* site = (BYTE*)(va + g_slide);
    if (memcmp(site, expected, len)) return false;
    *resume = (uintptr_t)site + len;
    return WriteCode(site, 0xE9, target, len);
}

bool HookCall(uintptr_t va, const BYTE* expected, size_t len, void* target) {
    BYTE* site = (BYTE*)(va + g_slide);
    return !memcmp(site, expected, len) && WriteCode(site, 0xE8, target, len);
}

bool Patch(uintptr_t va, const void* expected, const void* replacement, size_t len) {
    BYTE* site = (BYTE*)(va + g_slide);
    if (memcmp(site, expected, len)) return false;
    DWORD old;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, replacement, len);
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return true;
}

static bool InstallColorHook() {
    static const BYTE expected[13] = {0x8B, 0x84, 0x15, 0x90, 0xFD, 0xFF, 0xFF, 0x89, 0x81, 0xC0, 0x05, 0x00, 0x00};
    return Hook(0x4C7E3C, expected, sizeof expected, ColorHook, &g_resume);
}

// ---------------------------------------------------------------- config (kufmod.yml)
// A small reader for the subset of YAML the config uses: "section:" headers, "key: value" pairs,
// one nested "name:" level and "- item" list entries. # starts a comment.
static bool g_gridEnabled = true;
bool g_buildButton = true, g_standardMode = true, g_emptyNodes = true;
static int g_gridKeys[9] = {'Q', 'W', 'E', 'A', 'S', 'D', 'Z', 'X', 'C'};
struct CommandKey { char name[64]; int key; };  // key: virtual key code, 0 = none
static CommandKey g_commandKeys[64];
static int g_numCommandKeys;

static const char* DEFAULT_CONFIG =
    "# kufmod config\n"
    "#\n"
    "# Lines starting with # are comments. Edit while the game is closed; changes apply the next\n"
    "# time the game starts. In multiplayer, every player needs the mod with the same settings.\n"
    "\n"
    "# Standard match mode\n"
    "# The \"Gold\" mode on the skirmish setup screen becomes \"Standard\": every player starts with\n"
    "# 0 gold and 4 workers. It is also the default mode for every map (instead of Casual).\n"
    "# Casual and Family are unchanged.\n"
    "standard-mode: true\n"
    "\n"
    "# Empty resource nodes\n"
    "# A gold mine, iron vein or mana node at 0 remaining keeps paying a third of a normal trip\n"
    "# (34 gold, 20 iron, 7 mana) instead of nothing.\n"
    "empty-nodes: true\n"
    "\n"
    "# LAN play (test version)\n"
    "# Multi Play then asks \"Locus Online\" or \"LAN\". LAN plays over your network or a virtual\n"
    "# LAN (Radmin VPN, Hamachi): no Locus account, password or gamechain needed. Every player\n"
    "# needs the mod with lan-play on. Locus Online works as before.\n"
    "lan-play: false\n"
    "# The address other players connect to. auto = Radmin VPN or Hamachi if present, else your\n"
    "# LAN. Or write your address, e.g. lan-ip: 26.1.2.3\n"
    "lan-ip: auto\n"
    "# Rooms are announced by broadcast. If the others don't see your room, list their addresses,\n"
    "# e.g. lan-peers: 26.1.2.3, 26.4.5.6\n"
    "lan-peers:\n"
    "# Write network details to kufmod.log and every LAN packet to netdump.txt (for bug reports).\n"
    "lan-trace: false\n"
    "\n"
    "# Menu frame rate\n"
    "# The animated menus (main menu, Single/Multi Play choice) are drawn this many times per\n"
    "# second; the game draws them 20 times, which makes the mouse feel slow there. Menu\n"
    "# animations keep their normal speed. 0 = the game's own 20.\n"
    "menu-fps: 60\n"
    "\n"
    "# Order delay\n"
    "# The game carries out orders in turns of a few ticks (1 tick = 30 ms); an order waits for the\n"
    "# end of the current turn and one more turn. Shorter turns = units react sooner.\n"
    "# Single player turn in ticks (the game's own: 6, about 0.25 s per order; 1 = about 0.05 s).\n"
    "single-player-turn: 1\n"
    "# Multiplayer: ticks for the three Network Speed choices (the game's own: 6, 10, 14). The\n"
    "# default choice is the middle one. Too short for a slow connection makes the game pause\n"
    "# briefly while it waits. Every player needs the same values.\n"
    "network-speed: 3, 6, 10\n"
    "# The Network Speed a multiplayer match starts with: low, high or extra (the game's own: high).\n"
    "# It can still be changed during the match in the game's options.\n"
    "network-speed-start: low\n"
    "\n"
    "# Unit avoidance\n"
    "# A unit blocked by another unit waits (up to 1.5 s) only if the blocker is one of your own\n"
    "# units that is walking; otherwise (a stuck unit, a chicken, an enemy) it walks around at\n"
    "# once instead of standing in line. false = the game's own behaviour.\n"
    "unit-avoidance: true\n"
    "\n"
    "# Target priority\n"
    "# Units choosing their own target prefer enemy fighters, then workers, then towers, then other\n"
    "# buildings (nearest first within each), and keep their target instead of switching to\n"
    "# whatever is a little closer. Your own attack orders are unchanged. false = as before.\n"
    "target-priority: true\n"
    "\n"
    "# Campaign timer\n"
    "# Show the match clock (top right, as in skirmish and multiplayer) in campaign missions too.\n"
    "campaign-timer: true\n"
    "\n"
    "# Random maps\n"
    "# The \"Random (N players)\" map choices pick from every map for that many players (the game\n"
    "# only used six community maps, always in the same order). false = as before.\n"
    "random-maps: true\n"
    "# Maps start on Melee unless their name contains \"(Custom)\" (some official maps, like Simple2\n"
    "# and Open Field, started on Use Map Settings, which gives a black screen). false = as before.\n"
    "melee-default: true\n"
    "\n"
    "# Locus Online sometimes asks at startup: \"The Network has been restarted ... start new\n"
    "# account?\" Yes makes a new Locus account (deletes LocusGameChain\\test_db and\n"
    "# hostAccount.keystore; your multi\\ profiles stay). no = always answer No without asking,\n"
    "# yes = always Yes, ask = show the question.\n"
    "locus-new-account: no\n"
    "\n"
    "# Grid hotkeys\n"
    "# Press the buttons of the 3x3 grid (buildings to construct, units to train) with these keys.\n"
    "# Each layout row is one grid row, left to right. While a 3x3 grid is showing, these keys go to\n"
    "# the grid; otherwise they do what they normally do (hero skills, command hotkeys).\n"
    "# The numpad (7 8 9 / 4 5 6 / 1 2 3) keeps working as well.\n"
    "grid-hotkeys:\n"
    "  enabled: true\n"
    "  layout:\n"
    "    - QWE\n"
    "    - ASD\n"
    "    - ZXC\n"
    "\n"
    "# Build button\n"
    "# Workers (Peasants, Gnome Slaves) show a single Build button in the grid instead of all their\n"
    "# buildings, so the grid keys stay free for commands. Press it (or its grid key) to open the\n"
    "# building grid; Esc or choosing a building closes it again.\n"
    "build-button: true\n"
    "\n"
    "# Multiplayer colors\n"
    "# In a multiplayer room, the square next to each player's name becomes a color swatch: click\n"
    "# your own to choose your color (\"?\" = automatic). First come, first served; the room's host\n"
    "# keeps the list. Players without the mod see the game's usual colors.\n"
    "multiplayer-colors: true\n"
    "\n"
    "# Computer players in multiplayer (needs multiplayer-colors)\n"
    "# In a multiplayer room the host can pick COMPUTER in an empty slot, then choose its race\n"
    "# (Human, Devil, Random) and click its team button for a team (none, A ... H).\n"
    "# Every player needs the mod with this on.\n"
    "multiplayer-computers: true\n"
    "\n"
    "# Command hotkeys\n"
    "# Change the letter of a command button (the six ovals above the grid). Use the names listed\n"
    "# in kufmod_commands.txt, which the mod writes to the game folder each time the game starts.\n"
    "# Use \"none\" to remove a hotkey. Example:\n"
    "#   attack: F\n"
    "command-hotkeys:\n";

static void Trim(char* s) {
    char* e = s + strlen(s);
    while (e > s && (unsigned char)e[-1] <= ' ') *--e = 0;
    char* b = s;
    while (*b && (unsigned char)*b <= ' ') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

// "Q" -> 'Q', "f" -> 'F', "5" -> '5', "none" -> 0, anything else -> -1
static int ParseKey(const char* v) {
    if (!_stricmp(v, "none")) return 0;
    if (strlen(v) == 1) {
        char c = (char)toupper((unsigned char)v[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    return -1;
}

static bool IsTrue(const char* v) {
    return !_stricmp(v, "true") || !_stricmp(v, "yes") || !strcmp(v, "1");
}

// Command names are compared lowercase with spaces as dashes: "Hold Position" == "hold-position".
// The game's markup (\1, \0, \4 ...) is dropped and only the first line (up to \n) is used.
static void Normalize(const char* in, char* out, size_t size) {
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        if (in[0] == '\\' && in[1]) {
            if (in[1] == 'n') break;
            in++;
            continue;
        }
        char c = (char)tolower((unsigned char)*in);
        out[n++] = c == ' ' || c == '_' ? '-' : c;
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = 0;
}

// ---------------------------------------------------------------- Locus "new account" question
// At startup Locus sometimes asks "The Network has been restarted ... start new account?"
// (0x57CA70, MessageBoxA at 0x57CC39; 6 Yes = new account, 7 No = keep). Setting locus-new-account.
static int g_locusNewAccount = 0;  // 0 no, 1 yes, 2 ask

static int WINAPI LocusQuestion(HWND owner, const char* text, const char* caption, UINT type) {
    if (g_locusNewAccount == 2) return MessageBoxA(owner, text, caption, type);
    Log("locus: answered %s to the new-account question (setting locus-new-account)", g_locusNewAccount ? "Yes" : "No");
    return g_locusNewAccount ? IDYES : IDNO;
}

// ---------------------------------------------------------------- campaign timer
// The top bar draws the match clock (minutes:seconds from game+0x604) at 0x505B93, but skips it
// in campaign: cmp byte [0xB83AD0],0 / jne draw / cmp word [0xB83AD2],1 (campaign) /
// je 0x505DEC. campaign-timer turns that je into NOPs. The code after the clock checks for a
// network game itself, so nothing else is drawn. Display only.
static bool g_campaignTimer = true;

static void InstallCampaignTimer() {
    if (!g_campaignTimer) return;
    static const BYTE expected[6] = {0x0F, 0x84, 0x59, 0x02, 0x00, 0x00};  // je 0x505DEC
    static const BYTE nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (!Patch(0x505B8D, expected, nops, sizeof nops))
        Log("unexpected game code at 0x505B8D (different game version?). campaign-timer off.");
}

static bool InstallLocusQuestion() {
    BYTE expected[6] = {0xFF, 0x15};  // call dword ptr [MessageBoxA]
    uint32_t iat = (uint32_t)(0xA16B1C + g_slide);
    memcpy(expected + 2, &iat, 4);
    return HookCall(0x57CC39, expected, 6, (void*)LocusQuestion);
}

static void LoadConfig() {
    char path[MAX_PATH];
    sprintf_s(path, "%skufmod.yml", g_dir);
    FILE* f = NULL;
    if (fopen_s(&f, path, "r") || !f) {
        if (!fopen_s(&f, path, "w") && f) {
            fputs(DEFAULT_CONFIG, f);
            fclose(f);
            Log("wrote default %s", path);
        }
        return;
    }
    char line[256], section[64] = "", sub[64] = "";
    int layoutRow = 0;
    while (fgets(line, sizeof line, f)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = 0;
        int indent = 0;
        while (line[indent] == ' ') indent++;
        Trim(line);
        if (!*line) continue;
        if (line[0] == '-') {  // list item
            char item[64];
            strncpy_s(item, line + 1, _TRUNCATE);
            Trim(item);
            if (!strcmp(section, "grid-hotkeys") && !strcmp(sub, "layout") && layoutRow < 3) {
                for (int c = 0; c < 3; c++) {
                    char one[2] = {item[c], 0};
                    int k = item[c] ? ParseKey(one) : -1;
                    if (k > 0) g_gridKeys[layoutRow * 3 + c] = k;
                    else Log("config: bad grid key in row %d: \"%s\"", layoutRow + 1, item);
                }
                layoutRow++;
            }
            continue;
        }
        char* colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char key[64], value[256];
        strncpy_s(key, line, _TRUNCATE);
        strncpy_s(value, colon + 1, _TRUNCATE);
        Trim(key);
        Trim(value);
        if (indent == 0 && *value) {  // top-level setting
            if (!strcmp(key, "build-button")) g_buildButton = IsTrue(value);
            else if (!strcmp(key, "multiplayer-colors")) g_mpColors = IsTrue(value);
            else if (!strcmp(key, "multiplayer-computers")) g_mpComputers = IsTrue(value);
            else if (!strcmp(key, "standard-mode")) g_standardMode = IsTrue(value);
            else if (!strcmp(key, "empty-nodes")) g_emptyNodes = IsTrue(value);
            else if (!strcmp(key, "lan-play")) g_lanPlay = IsTrue(value);
            else if (!strcmp(key, "lan-ip")) strncpy_s(g_lanIp, value, _TRUNCATE);
            else if (!strcmp(key, "lan-peers")) strncpy_s(g_lanPeers, value, _TRUNCATE);
            else if (!strcmp(key, "lan-trace")) g_lanTrace = IsTrue(value);
            else if (!strcmp(key, "perf-trace")) g_perfTrace = IsTrue(value);
            else if (!strcmp(key, "menu-fps")) g_menuFps = atoi(value);
            else if (!strcmp(key, "single-player-turn")) g_spTurn = atoi(value);
            else if (!strcmp(key, "unit-avoidance")) g_unitAvoidance = IsTrue(value);
            else if (!strcmp(key, "target-priority")) g_targetPriority = IsTrue(value);
            else if (!strcmp(key, "campaign-timer")) g_campaignTimer = IsTrue(value);
            else if (!strcmp(key, "random-maps")) g_randomMaps = IsTrue(value);
            else if (!strcmp(key, "melee-default")) g_meleeDefault = IsTrue(value);
            else if (!strcmp(key, "network-speed-start"))
                g_netStart = !_stricmp(value, "high") ? 1 : !_stricmp(value, "extra") ? 2 : 0;
            else if (!strcmp(key, "network-speed")) {
                int a, b, c;
                if (sscanf_s(value, "%d , %d , %d", &a, &b, &c) == 3) {
                    g_netTurns[0] = a; g_netTurns[1] = b; g_netTurns[2] = c;
                } else Log("config: network-speed needs three numbers, e.g. 3, 6, 10");
            }
            else if (!strcmp(key, "locus-new-account"))
                g_locusNewAccount = !_stricmp(value, "yes") ? 1 : !_stricmp(value, "ask") ? 2 : 0;
            else Log("config: unknown setting \"%s\"", key);
            section[0] = sub[0] = 0;
            continue;
        }
        if (indent == 0) {  // top-level section
            strncpy_s(section, key, _TRUNCATE);
            sub[0] = 0;
            continue;
        }
        if (!*value) {  // nested section, e.g. "layout:"
            strncpy_s(sub, key, _TRUNCATE);
            continue;
        }
        if (!strcmp(section, "grid-hotkeys") && !strcmp(key, "enabled")) {
            g_gridEnabled = IsTrue(value);
        } else if (!strcmp(section, "command-hotkeys") && g_numCommandKeys < 64) {
            int k = ParseKey(value);
            if (k < 0) {
                Log("config: bad key for command %s: \"%s\"", key, value);
                continue;
            }
            CommandKey& ck = g_commandKeys[g_numCommandKeys++];
            Normalize(key, ck.name, sizeof ck.name);
            ck.key = k;
        }
    }
    fclose(f);
    Log("config: standard mode %s, empty nodes %s, grid hotkeys %s, build button %s, lan play %s, layout "
        "%c%c%c/%c%c%c/%c%c%c, %d command hotkey changes",
        g_standardMode ? "on" : "off", g_emptyNodes ? "on" : "off", g_gridEnabled ? "on" : "off",
        g_buildButton ? "on" : "off", g_lanPlay ? "on" : "off", g_gridKeys[0], g_gridKeys[1], g_gridKeys[2], g_gridKeys[3],
        g_gridKeys[4], g_gridKeys[5], g_gridKeys[6], g_gridKeys[7], g_gridKeys[8], g_numCommandKeys);
}

// ---------------------------------------------------------------- hotkeys
// The in-game menu (KufMenu) handles a pressed key from 0x502D19 on, with the key code in edi
// (and in [ebp+8]) and the menu in esi. In order it checks:
//   1. each 3x3 grid slot's own letter hotkey from the game data (both grid pages),
//   2. Numpad 7 8 9 / 4 5 6 / 1 2 3, which press grid slots 0-8 (slot records at menu+0x4A0,
//      16 bytes each, id -1 = empty),
//   3. the six command buttons' letter hotkeys, then Insert/Home/PgUp/Delete/End/PgDn,
//   4. Q W E R T Y for hero skills.
// Grid hotkeys. A letter key is decided here:
//   1. a layout key whose grid slot has a button becomes the numpad key for that slot,
//   2. else a command button's letter becomes that button's position key (Insert, Home, PgUp,
//      Delete, End, PgDn for buttons 0-5), which no grid letter can catch,
//   3. else a grid button's old letter from the game data (F for Farm...) is swallowed,
//   4. else the key is left alone (hero skills and anything else).
// Soldiers can have a grid with only one or two buttons, so A for Attack still works for them
// while slot 3 (A) is empty.
static uintptr_t g_keyResume;
static const int NUMPAD_FOR_SLOT[9] = {VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_NUMPAD4, VK_NUMPAD5,
                                       VK_NUMPAD6, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3};
static const int POSITION_KEY_FOR_COMMAND[6] = {VK_INSERT, VK_HOME, VK_PRIOR, VK_DELETE, VK_END, VK_NEXT};
static const int SWALLOWED_KEY = 0x07;  // an unassigned key code: matches nothing in the handler

static bool SlotFilled(const BYTE* menu, int slot) {
    return *(const int32_t*)(menu + 0x4A0 + slot * 0x10) != -1;
}

// True when key is a layout key whose grid slot currently has a button.
static bool GridOwnsKey(const BYTE* menu, int key) {
    if (!g_gridEnabled) return false;
    for (int i = 0; i < 9; i++)
        if (g_gridKeys[i] == key && SlotFilled(menu, i)) return true;
    return false;
}

// ---------------------------------------------------------------- build button
// KufMenu fills the 3x3 grid in its virtual function +0x64 (0x50EFF0, only ever called through
// the vtable). We replace that vtable entry with FillWrapper, which runs the game's fill and then,
// for a worker's grid (it holds "construct building" buttons, command type 0x12C), replaces the
// grid with a single Build button in slot 0 unless the build menu is open.
// Pressing a grid button runs KufMenu +0x2C (0x512F80); its grid branch starts at 0x513396,
// where GridExecHook catches our Build button and opens the build menu.
static const int TYPE_BUILD = 0x12C;        // command type of "construct a building" buttons
static const int BUILD_BUTTON_ID = 0xD9;    // id we put in slot 0 (the game's "next page" record)
static bool g_buildOpen;                    // build menu open for the current selection
static uint32_t g_openSelection;            // selection the build menu was opened for
static bool g_buildButtonShown;             // slot 0 currently holds our Build button
static uintptr_t g_execResume;
// Hammer icon numbers from icons.cpp (0 if the icons could not be prepared).
static int32_t g_iconHuman, g_iconDevil;
typedef int(__fastcall* FillFn)(BYTE* menu, void* unused);
static FillFn g_originalFill;

static int CommandType(int32_t id) {
    uintptr_t table = Game<uintptr_t>(0xB82824);
    int count = Game<int32_t>(0xB82810);
    if (!table || id < 0 || id >= count) return -1;
    return *(const int32_t*)(table + id * 0x200 + 8);
}

static uint32_t SelectionSignature(const BYTE* menu) {
    int count = *(const int16_t*)(menu + 0x1898);
    const int16_t* ids = *(const int16_t* const*)(menu + 0x189C);
    uint32_t sig = (uint32_t)count * 2654435761u;
    for (int i = 0; i < count && i < 4 && ids; i++) sig = sig * 31 + (uint16_t)ids[i];
    return sig;
}

static bool GridHasBuildings(const BYTE* menu) {
    for (int i = 0; i < 9; i++)
        if (CommandType(*(const int32_t*)(menu + 0x4A0 + i * 0x10)) == TYPE_BUILD) return true;
    return false;
}

// Ask KufMenu to refill the grid, as the game's own page buttons do.
static void RequestGridRefresh(BYTE* menu) {
    *(uint16_t*)(menu + 0x6A8) |= 8;
    *(uint32_t*)(menu + 0x2B4) |= 0x10;
}

static void AfterFill(BYTE* menu) {
    g_buildButtonShown = false;
    if (!g_buildButton || !GridHasBuildings(menu)) return;
    if (g_buildOpen && SelectionSignature(menu) == g_openSelection) return;
    g_buildOpen = false;
    int32_t icon = 0xD;
    if (g_iconHuman) {
        // Race from the worker's command buttons: Dark Legion Dismantle/Repair icons are 291/292.
        icon = g_iconHuman;
        for (int i = 0; i < 6; i++) {
            int32_t oval = *(const int32_t*)(menu + 0x444 + i * 0x10);
            if (oval == 291 || oval == 292) icon = g_iconDevil;
        }
    }
    for (int i = 0; i < 9 && icon == 0xD; i++)  // fallback: the first building's picture
        if (CommandType(*(const int32_t*)(menu + 0x4A0 + i * 0x10)) == TYPE_BUILD)
            icon = *(const int32_t*)(menu + 0x4A4 + i * 0x10);
    for (int i = 0; i < 9; i++) {
        BYTE* slot = menu + 0x4A0 + i * 0x10;
        *(int32_t*)(slot + 0) = -1;
        *(int32_t*)(slot + 4) = 0xD;
        *(int32_t*)(slot + 8) = 0xFFFF;
        *(uint16_t*)(slot + 0xC) = 0xFF;
    }
    *(int32_t*)(menu + 0x4A0) = BUILD_BUTTON_ID;
    *(int32_t*)(menu + 0x4A4) = icon;
    g_buildButtonShown = true;
}

static int __fastcall FillWrapper(BYTE* menu, void* unused) {
    int result = g_originalFill(menu, unused);
    AfterFill(menu);
    return result;
}

// Returns 1 if the pressed grid button was our Build button (handled), else 0.
extern "C" int __cdecl OnGridExecute(BYTE* menu) {
    int index = *(const int16_t*)(menu + 0x286);
    if (index < 0 || index >= 9) return 0;
    int32_t id = *(const int32_t*)(menu + 0x4A0 + index * 0x10);
    if (g_buildButtonShown && index == 0 && id == BUILD_BUTTON_ID) {
        g_buildOpen = true;
        g_openSelection = SelectionSignature(menu);
        *(int32_t*)(menu + 0x6A4) = 0;  // first page of buildings
        RequestGridRefresh(menu);
        return 1;
    }
    if (CommandType(id) == TYPE_BUILD) g_buildOpen = false;  // a building was chosen: close
    return 0;
}

// Replaces, at 0x513396 (grid branch of KufMenu::Execute, edi = menu):
//   movsx ecx, word ptr [edi+0x286]
__declspec(naked) static void GridExecHook() {
    __asm {
        pushad
        push edi
        call OnGridExecute
        add esp, 4
        test eax, eax
        popad
        jnz handled
        movsx ecx, word ptr [edi + 0x286]
        jmp dword ptr [g_execResume]
    handled:
        mov eax, 1           // same exit as the function's own "page button" branch
        pop edi
        pop esi
        pop ebx
        mov esp, ebp
        pop ebp
        ret 4
    }
}

static bool InstallBuildButton() {
    if (!g_buildButton) return true;
    if (!PrepareBuildIcons(&g_iconHuman, &g_iconDevil)) {
        g_iconHuman = g_iconDevil = 0;
        Log("build button will use a building picture");
    }
    static const BYTE expected[7] = {0x0F, 0xBF, 0x8F, 0x86, 0x02, 0x00, 0x00};
    uintptr_t* slot = (uintptr_t*)(0xA3BD0C + 0x64 + g_slide);  // KufMenu vtable, +0x64
    if (*slot != 0x50EFF0 + g_slide) return false;
    if (!Hook(0x513396, expected, sizeof expected, GridExecHook, &g_execResume)) return false;
    g_originalFill = (FillFn)*slot;
    DWORD old;
    VirtualProtect(slot, 4, PAGE_READWRITE, &old);
    *slot = (uintptr_t)FillWrapper;
    VirtualProtect(slot, 4, old, &old);
    return true;
}

extern "C" int __cdecl TranslateKey(int key, const BYTE* menu) {
    if (key == VK_ESCAPE && g_buildOpen && GridHasBuildings(menu)) {  // close the build menu
        g_buildOpen = false;
        RequestGridRefresh((BYTE*)menu);
        return SWALLOWED_KEY;
    }
    if (!g_gridEnabled || key < 'A' || key > 'Z') return key;
    if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) return key;
    for (int i = 0; i < 9; i++)
        if (g_gridKeys[i] == key && SlotFilled(menu, i)) return NUMPAD_FOR_SLOT[i];
    for (int i = 0; i < 6; i++) {
        const BYTE* button = menu + 0x440 + i * 0x10;
        if (*(const int32_t*)(button + 8) == key - 'A' && *(const uint16_t*)(button + 0xC) != 0x50)
            return POSITION_KEY_FOR_COMMAND[i];
    }
    for (int i = 0; i < 9; i++)
        if (SlotFilled(menu, i) && *(const int32_t*)(menu + 0x4A8 + i * 0x10) == key - 'A') return SWALLOWED_KEY;
    return key;
}

// Replaces, at 0x502D19:  xor eax,eax / lea ecx,[esi+0x4AC]   (8 bytes; all jumps land on the
// first one). The key is translated in edi and [ebp+8], which later code reloads it from.
__declspec(naked) static void KeyHook() {
    __asm {
        push ecx
        push edx
        push esi
        push edi
        call TranslateKey
        add esp, 8
        pop edx
        pop ecx
        mov edi, eax
        mov [ebp + 8], eax
        xor eax, eax
        lea ecx, [esi + 0x4AC]
        jmp dword ptr [g_keyResume]
    }
}

static bool InstallKeyHook() {
    static const BYTE expected[8] = {0x33, 0xC0, 0x8D, 0x8E, 0xAC, 0x04, 0x00, 0x00};
    return Hook(0x502D19, expected, sizeof expected, KeyHook, &g_keyResume);
}

// Button labels. When the mouse is over a button, KufMenu draws its name from 0x503F8D with the
// text pointer on the stack, edi = menu, word [menu+0x284] = kind of button (2 = command oval,
// 3 = grid slot) and word [menu+0x286] = its index. The texts use markup: \1 starts the hotkey
// highlight, \0 ends a colored part, \n is a new line. We hand the game a copy without the
// highlight and with the real key after the first line, e.g. "Train Peasant (Q)".
static uintptr_t g_labelResume;
static char g_label[600];

extern "C" const char* __cdecl LabelText(const char* text, const BYTE* menu) {
    int kind = *(const int16_t*)(menu + 0x284), index = *(const int16_t*)(menu + 0x286);
    if (kind == 3 && index == 0 && g_buildButtonShown) text = "Build";
    int key = 0;
    if (kind == 3 && index >= 0 && index < 9 && g_gridEnabled) key = g_gridKeys[index];
    if (kind == 2 && index >= 0 && index < 6) {
        int32_t stored = *(const int32_t*)(menu + 0x448 + index * 0x10);
        if (stored != 0xFFFF && stored != -1) key = stored + 'A';
        // A layout key whose grid slot has a button belongs to the grid, so the command can't use it.
        if (key && GridOwnsKey(menu, key)) key = 0;
    }
    char tag[8];
    sprintf_s(tag, " (%c)", key ? key : '?');
    size_t n = 0;
    bool keyAdded = key == 0 || strstr(text, tag) != NULL, inHighlight = false;  // already tagged
    for (const char* p = text; *p && n < sizeof g_label - 8;) {
        if (p[0] == '\\' && p[1] == '1') { inHighlight = true; p += 2; continue; }
        if (p[0] == '\\' && p[1] == '0' && inHighlight) { inHighlight = false; p += 2; continue; }
        if (p[0] == '\\' && p[1] == 'n' && !keyAdded) {
            n += sprintf_s(g_label + n, sizeof g_label - n, " (%c)", key);
            keyAdded = true;
        }
        g_label[n++] = *p++;
    }
    g_label[n] = 0;
    if (!keyAdded) sprintf_s(g_label + n, sizeof g_label - n, " (%c)", key);
    return g_label;
}

// Replaces, at 0x503F8D:  lea eax,[ebp-0x140] / push eax   (the text pointer is already pushed)
__declspec(naked) static void LabelHook() {
    __asm {
        push ecx
        push edx
        push edi
        push dword ptr [esp + 12]
        call LabelText
        add esp, 8
        pop edx
        pop ecx
        mov [esp], eax
        lea eax, [ebp - 0x140]
        push eax
        jmp dword ptr [g_labelResume]
    }
}

// Grid buttons that show a cost box (gold, iron...) copy their name into a local buffer at
// [ebp-0x110] and draw it through other routines, not through 0x503F8D. We fix the buffer in
// place right after the copy, at 0x503A2D.
static uintptr_t g_gridLabelResume;

extern "C" void __cdecl FixLabelBuffer(char* buffer, const BYTE* menu) {
    const char* fixed = LabelText(buffer, menu);
    strncpy_s(buffer, 0x100, fixed, _TRUNCATE);
}

// Replaces, at 0x503A2D:  movzx edx, word ptr [edi+0x286]   (7 bytes)
__declspec(naked) static void GridLabelHook() {
    __asm {
        pushad
        push edi
        lea eax, [ebp - 0x110]
        push eax
        call FixLabelBuffer
        add esp, 8
        popad
        movzx edx, word ptr [edi + 0x286]
        jmp dword ptr [g_gridLabelResume]
    }
}

static bool InstallLabelHook() {
    static const BYTE expected[7] = {0x8D, 0x85, 0xC0, 0xFE, 0xFF, 0xFF, 0x50};
    static const BYTE gridExpected[7] = {0x0F, 0xB7, 0x97, 0x86, 0x02, 0x00, 0x00};
    bool a = Hook(0x503F8D, expected, sizeof expected, LabelHook, &g_labelResume);
    bool b = Hook(0x503A2D, gridExpected, sizeof gridExpected, GridLabelHook, &g_gridLabelResume);
    return a && b;
}

// Command hotkeys live in the game's button table, loaded from its data at startup:
// pointer at 0xB82824, count at 0xB82810, 0x200 bytes per command, hotkey at +0x10 stored as
// (key - 'A') or 0xFFFF for none, text fields at +0x14, +0x94 and +0x114 (128 bytes each).
// The menu copies the hotkey from here whenever it fills the command buttons, so changing the
// table changes the hotkey everywhere.
static uintptr_t g_commandTable;

static void KeyName(int32_t stored, char* out, size_t size) {
    if (stored == 0xFFFF || stored == -1) strcpy_s(out, size, "-");
    else sprintf_s(out, size, "%c", (char)(stored + 'A'));
}

static void ApplyCommandHotkeys() {
    uintptr_t table = Game<uintptr_t>(0xB82824);
    int count = Game<int32_t>(0xB82810);
    if (!table || table == g_commandTable || count <= 0 || count > 4000) return;
    g_commandTable = table;
    bool used[64] = {};
    for (int i = 0; i < count; i++) {
        BYTE* rec = (BYTE*)table + i * 0x200;
        char a[128], b[128];
        Normalize((const char*)rec + 0x14, a, sizeof a);
        Normalize((const char*)rec + 0x94, b, sizeof b);
        for (int c = 0; c < g_numCommandKeys; c++) {
            if (strcmp(g_commandKeys[c].name, a) && strcmp(g_commandKeys[c].name, b)) continue;
            int k = g_commandKeys[c].key;
            *(int32_t*)(rec + 0x10) = k ? k - 'A' : 0xFFFF;
            used[c] = true;
        }
    }
    for (int c = 0; c < g_numCommandKeys; c++)
        if (!used[c]) Log("config: no command named \"%s\" (see kufmod_commands.txt)", g_commandKeys[c].name);

    char path[MAX_PATH];
    sprintf_s(path, "%skufmod_commands.txt", g_dir);
    FILE* f = NULL;
    if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "Command buttons and their hotkeys, as the game currently uses them (written by kufmod).\n"
               "Change one in kufmod.yml under command-hotkeys, using the name in the first column.\n"
               "* = the key is also a grid hotkey, so it only works while no 3x3 grid is showing.\n\n");
    for (int i = 0; i < count; i++) {
        BYTE* rec = (BYTE*)table + i * 0x200;
        const char* name = (const char*)rec + 0x14;
        if (!*name) continue;
        int32_t stored = *(int32_t*)(rec + 0x10);
        char key[8], norm[128];
        KeyName(stored, key, sizeof key);
        Normalize(name, norm, sizeof norm);
        bool clash = false;
        for (int g = 0; g < 9 && g_gridEnabled; g++)
            if (stored != 0xFFFF && g_gridKeys[g] == stored + 'A') clash = true;
        fprintf(f, "%-32s %s%s  type %-5d id %-4d %.50s | %.50s\n", norm, key, clash ? "*" : " ",
                *(int32_t*)(rec + 8), i, (const char*)rec + 0x94, (const char*)rec + 0x114);
    }
    fclose(f);
    Log("command table: %d commands, wrote %s", count, path);
}

// ---------------------------------------------------------------- drawing
static void Fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc = {l, t, r, b};
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

// A framed swatch: color c, or "?" for automatic (-1). Taken colors are dimmed and crossed out.
static void DrawSwatch(HDC dc, RECT r, int c, bool taken, bool current) {
    Fill(dc, r.left, r.top, r.right, r.bottom, current ? RGB(240, 200, 60) : RGB(12, 12, 12));
    Fill(dc, r.left + 2, r.top + 2, r.right - 2, r.bottom - 2, RGB(150, 150, 150));
    RECT in = {r.left + 3, r.top + 3, r.right - 3, r.bottom - 3};
    if (c < 0) {
        Fill(dc, in.left, in.top, in.right, in.bottom, RGB(45, 45, 45));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(230, 230, 230));
        DrawTextA(dc, "?", 1, &in, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    COLORREF col = ColorOf(c);
    if (taken) col = RGB(GetRValue(col) / 3, GetGValue(col) / 3, GetBValue(col) / 3);
    Fill(dc, in.left, in.top, in.right, in.bottom, col);
    if (taken) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(20, 20, 20));
        HGDIOBJ prev = SelectObject(dc, pen);
        MoveToEx(dc, in.left + 2, in.top + 2, NULL);
        LineTo(dc, in.right - 2, in.bottom - 2);
        MoveToEx(dc, in.right - 2, in.top + 2, NULL);
        LineTo(dc, in.left + 2, in.bottom - 2);
        SelectObject(dc, prev);
        DeleteObject(pen);
    }
}

static bool TakenByOther(int c, int slot) {
    for (int i = 0; i < NUM_SLOTS; i++)
        if (i != slot && g_swatch[i] && IsWindowVisible(g_swatch[i]) && g_choice[i] == c) return true;
    return false;
}

// ---------------------------------------------------------------- multiplayer room
// The room (CXCreateDialog) is a 1200x900 window of Win32 controls. Slot i (0-7, left and right
// column alternating) uses control IDs ROOM_SLOT + 10*i + : 0 kick "X", 1 player name box,
// 2 record "(level)wins-losses", 3 portrait, 4 race box, 5 rank icon (CXPNetGrade, 33x28). The
// swatches replace the rank icons. Room chat: box 786, Send 787, messages shown in 784.
//
// Choices travel as room chat lines the mod hides (the host relays chat to everyone, in LAN and
// in Locus Online):
//   "#KUFC R <color>"        a player asks for a color (-1 = "?") for himself
//   "#KUFC A <c0> ... <c7>"  the host's list: the color of every slot. Everyone shows it and the
//                            match starts with it (PickColor), so every PC gets the same colors.
// The host decides: first come, first served; a taken color is refused. It keeps choices by
// player name, so a color follows its player when he moves to another slot, and sends the list
// again whenever the names in the slots change.
static const int ROOM_SLOT = 816;
static const char COLOR_TAG[] = "#KUFC ";

struct NamedColor { char name[36]; int color; };
static NamedColor g_named[16];   // host: choices by player name
static char g_slotName[NUM_SLOTS][36];
static int g_hostList[NUM_SLOTS] = {-1, -1, -1, -1, -1, -1, -1, -1};  // last list from the host

bool IsHost() { return Game<uint32_t>(0xBED1FC) != 0; }  // this PC runs the room server
static void ApplyList();
const char* MyName() { return (const char*)(0xBEDA40 + g_slide); }

static void RoomName(int slot, char* out, size_t size) {
    *out = 0;
    HWND box = g_setup ? GetDlgItem(g_setup, ROOM_SLOT + 10 * slot + 1) : NULL;
    if (box) GetWindowTextA(box, out, (int)size);
}

// Room chat out: the room client [CXCreateDialog+0x188], vtable +0xA0 (message 0x4027, text),
// as the chat box's Send does (0x6E3E3A). UI thread only.
typedef void(__thiscall* ChatFn)(void* client, int message, const char* text);

void SendRoomLine(const char* text) {
    uintptr_t dialog = Game<uintptr_t>(0xBED22C);
    void* client = dialog ? *(void**)(dialog + 0x188) : NULL;
    if (!client) return;
    ChatFn send = *(ChatFn*)(*(uintptr_t*)client + 0xA0);
    send(client, Game<uint8_t>(0xB41726) ? 0x4027 : 0x4020, text);
}

static int NamedColorOf(const char* name) {
    for (const NamedColor& n : g_named)
        if (n.name[0] && !strcmp(n.name, name)) return n.color;
    return -1;
}

static void HostSendList() {
    char text[128];
    int len = sprintf_s(text, "%sA", COLOR_TAG);
    for (int i = 0; i < NUM_SLOTS; i++) {
        g_hostList[i] = g_slotName[i][0] ? NamedColorOf(g_slotName[i]) : -1;
        len += sprintf_s(text + len, sizeof text - len, " %d", g_hostList[i]);
    }
    ApplyList();  // the host's own swatches at once; the others get it through the chat
    SendRoomLine(text);
}

static void HostSetColor(const char* name, int c) {
    if (c >= 0) {  // first come, first served
        for (const NamedColor& n : g_named)
            if (n.name[0] && n.color == c && strcmp(n.name, name)) {
                Log("room: %s asked for color %d, taken by %s", name, c, n.name);
                return;
            }
    }
    NamedColor* slot = NULL;
    for (NamedColor& n : g_named)
        if (n.name[0] && !strcmp(n.name, name)) slot = &n;
    if (!slot)
        for (NamedColor& n : g_named)
            if (!n.name[0]) { slot = &n; break; }
    if (!slot) return;
    strncpy_s(slot->name, name, _TRUNCATE);
    slot->color = c;
    Log("room: %s -> color %d", name, c);
}

static void RequestColor(int c) {  // from the palette, for this player's own slot
    if (IsHost()) {
        HostSetColor(MyName(), c);
        HostSendList();
    } else {
        char text[32];
        sprintf_s(text, "%sR %d", COLOR_TAG, c);
        SendRoomLine(text);
    }
}

static void ApplyList() {
    for (int i = 0; i < NUM_SLOTS; i++) {
        g_mpChoice[i] = g_hostList[i];
        if (g_room && g_swatch[i]) InvalidateRect(g_swatch[i], NULL, FALSE);
    }
}

// Room chat in: the room client's message handler (0x6E6600) shows a chat message (0x4020) from
// one string, "<type><sender name>\n\r<text>" (case at 0x6E6687). Our lines are handled here and
// not shown.
extern "C" bool __cdecl OnRoomChat(const char* raw) {
    if (!raw || !raw[0]) return false;
    const char* sep = strstr(raw, "\n\r");
    if (!sep) return false;
    const char* text = sep + 2;
    char sender[36] = "";
    size_t n = (size_t)(sep - raw - 1);
    if (n >= sizeof sender) n = sizeof sender - 1;
    memcpy(sender, raw + 1, n);
    sender[n] = 0;
    if (int ai = AiRoomChat(sender, text)) return ai == 2;  // computer players (aimp.cpp)
    if (strncmp(text, COLOR_TAG, sizeof COLOR_TAG - 1)) return false;
    const char* body = text + sizeof COLOR_TAG - 1;
    if (body[0] == 'R' && IsHost()) {
        int c = atoi(body + 1);
        if (c >= -1 && c < NUM_COLORS) {
            HostSetColor(sender, c);
            HostSendList();
        }
    } else if (body[0] == 'A') {
        const char* p = body + 1;
        for (int i = 0; i < NUM_SLOTS; i++) {
            char* end;
            long c = strtol(p, &end, 10);
            if (end == p) break;
            g_hostList[i] = (c >= -1 && c < NUM_COLORS) ? (int)c : -1;
            p = end;
        }
        ApplyList();
    }
    Log("room: hidden line from %s: %s", sender, body);
    return true;  // hidden
}

// Replaces, at 0x6E6687: mov edi,[edi+8] / push 0xA4DCE0 (the "\n\r" separator, relocated).
static uintptr_t g_roomChatResume, g_roomChatExit, g_chatSeparator;
static bool g_chatHidden;

__declspec(naked) static void RoomChatHook() {
    __asm {
        mov edi, dword ptr [edi + 8]      // the chat string
        pushad
        push edi
        call OnRoomChat
        add esp, 4
        mov byte ptr [g_chatHidden], al
        popad
        cmp byte ptr [g_chatHidden], 0
        jne hidden
        push dword ptr [g_chatSeparator]
        jmp dword ptr [g_roomChatResume]  // 0x6E668F
    hidden:
        jmp dword ptr [g_roomChatExit]    // 0x6E6789: the handler's exit, as after a shown line
    }
}

static bool InstallRoomChatHook() {
    uint32_t sep = (uint32_t)(0xA4DCE0 + g_slide);
    BYTE code[8] = {0x8B, 0x7F, 0x08, 0x68};
    memcpy(code + 4, &sep, 4);
    g_chatSeparator = sep;
    g_roomChatExit = 0x6E6789 + g_slide;
    return Hook(0x6E6687, code, sizeof code, RoomChatHook, &g_roomChatResume);
}

// ---------------------------------------------------------------- palette popup
static const int CELL = 32, GAP = 4, COLS = 3;
static const int PAL_SIZE = GAP + COLS * (CELL + GAP);

static RECT CellRect(int i) {  // cell 0 = "?", cells 1-8 = colors 0-7
    int x = GAP + (i % COLS) * (CELL + GAP), y = GAP + (i / COLS) * (CELL + GAP);
    RECT r = {x, y, x + CELL, y + CELL};
    return r;
}

static void ClosePalette() {
    static bool closing;
    if (!g_palette || closing) return;
    closing = true;
    HWND p = g_palette;
    g_palette = NULL;
    g_paletteSlot = -1;
    if (GetCapture() == p) ReleaseCapture();
    DestroyWindow(p);
    closing = false;
}

static LRESULT CALLBACK PaletteProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Fill(dc, 0, 0, PAL_SIZE, PAL_SIZE, RGB(150, 140, 120));
        Fill(dc, 1, 1, PAL_SIZE - 1, PAL_SIZE - 1, RGB(32, 28, 24));
        int slot = g_paletteSlot;
        for (int i = 0; i <= NUM_COLORS; i++) {
            int c = i - 1;
            DrawSwatch(dc, CellRect(i), c, c >= 0 && TakenByOther(c, slot), slot >= 0 && g_choice[slot] == c);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
        int slot = g_paletteSlot;
        for (int i = 0; i <= NUM_COLORS && slot >= 0; i++) {
            RECT r = CellRect(i);
            int c = i - 1;
            if (PtInRect(&r, pt) && !(c >= 0 && TakenByOther(c, slot))) {
                if (g_room) {  // the host decides; its list updates the swatches
                    RequestColor(c);
                } else {
                    g_choice[slot] = c;
                    Log("slot %d color -> %d", slot, c);
                    InvalidateRect(g_swatch[slot], NULL, FALSE);
                }
            }
        }
        ClosePalette();  // any click, inside or outside, closes the palette
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)l != h) ClosePalette();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcA(h, m, w, l);
}

static void ShowPalette(int slot) {
    ClosePalette();
    RECT sw, parent;
    GetWindowRect(g_swatch[slot], &sw);
    GetWindowRect(g_setup, &parent);
    int x = sw.left, y = sw.bottom + 2;
    if (y + PAL_SIZE > parent.bottom) y = sw.top - 2 - PAL_SIZE;
    if (x + PAL_SIZE > parent.right) x = parent.right - PAL_SIZE;
    g_paletteSlot = slot;
    g_palette = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "KufModPalette", "", WS_POPUP,
                                x, y, PAL_SIZE, PAL_SIZE, g_setup, NULL, g_inst, NULL);
    ShowWindow(g_palette, SW_SHOWNOACTIVATE);
    SetCapture(g_palette);
}

// ---------------------------------------------------------------- swatches
static LRESULT CALLBACK SwatchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int slot = (int)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        DrawSwatch(dc, r, g_choice[slot], false, false);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (!g_room || !strcmp(g_slotName[slot], MyName())) ShowPalette(slot);  // in a room: own slot only
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcA(h, m, w, l);
}

// Room: a slot with a player (its name box shows a name, not OPEN / CLOSE / CLOSED).
static void SyncRoom() {
    bool namesChanged = false;
    for (int i = 0; i < NUM_SLOTS; i++) {
        char name[36];
        RoomName(i, name, sizeof name);
        bool player = name[0] && _stricmp(name, "OPEN") && _stricmp(name, "CLOSE") && _stricmp(name, "CLOSED") &&
                      _stricmp(name, "OBSERVER") && _stricmp(name, "COMPUTER");
        if (!player) name[0] = 0;
        if (strcmp(name, g_slotName[i])) {
            strcpy_s(g_slotName[i], name);
            namesChanged = true;
        }
        HWND rank = GetDlgItem(g_setup, ROOM_SLOT + 10 * i + 5);
        if (rank && IsWindowVisible(rank)) ShowWindow(rank, SW_HIDE);  // the swatch takes its place
        if (!g_swatch[i]) continue;
        if (player != (IsWindowVisible(g_swatch[i]) != FALSE)) ShowWindow(g_swatch[i], player ? SW_SHOWNA : SW_HIDE);
    }
    AiRoomSync(g_setup);  // computer slots (aimp.cpp)
    if (namesChanged && IsHost()) {  // someone joined, left or moved
        HostSendList();
        AiHostSend();
    }
}

// A slot gets a swatch while its player box is shown and not set to "Closed" / "OPEN".
static bool SlotInUse(HWND box) {
    if (!box || !IsWindowVisible(box)) return false;
    LRESULT sel = SendMessageA(box, CB_GETCURSEL, 0, 0);
    if (sel < 0) return false;
    char text[64] = "";
    if (SendMessageA(box, CB_GETLBTEXTLEN, sel, 0) < (LRESULT)sizeof text)
        SendMessageA(box, CB_GETLBTEXT, sel, (LPARAM)text);
    return _stricmp(text, "Closed") && _stricmp(text, "OPEN");
}

static void SyncSwatches() {
    if (g_room) {
        SyncRoom();
        return;
    }
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (!g_swatch[i]) continue;
        bool show = SlotInUse(GetDlgItem(g_setup, PLAYER_BOX_ID + i));
        if (show != (IsWindowVisible(g_swatch[i]) != FALSE)) ShowWindow(g_swatch[i], show ? SW_SHOWNA : SW_HIDE);
    }
}

static void RegisterClasses() {
    static bool done;
    if (done) return;
    WNDCLASSA wc = {};
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.lpfnWndProc = SwatchProc;
    wc.lpszClassName = "KufModSwatch";
    RegisterClassA(&wc);
    wc.lpfnWndProc = PaletteProc;
    wc.lpszClassName = "KufModPalette";
    wc.style = CS_DROPSHADOW;
    RegisterClassA(&wc);
    done = true;
}

static void CreateSwatches(HWND setup) {
    RegisterClasses();
    if (g_room) {  // over each slot's rank icon
        for (int i = 0; i < NUM_SLOTS; i++) {
            HWND rank = GetDlgItem(setup, ROOM_SLOT + 10 * i + 5);
            if (!rank || g_swatch[i]) continue;
            RECT r;
            GetWindowRect(rank, &r);
            MapWindowPoints(NULL, setup, (POINT*)&r, 2);
            g_swatch[i] = CreateWindowExA(0, "KufModSwatch", "", WS_CHILD | WS_CLIPSIBLINGS, r.left, r.top,
                                          r.right - r.left, r.bottom - r.top, setup, (HMENU)(INT_PTR)(9000 + i),
                                          g_inst, NULL);
            SetWindowLongPtrA(g_swatch[i], GWLP_USERDATA, i);
            SetWindowPos(g_swatch[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SyncSwatches();
        Log("room decorated (%s)", IsHost() ? "host" : "joined");
        return;
    }
    for (int i = 0; i < NUM_SLOTS; i++) {
        HWND box = GetDlgItem(setup, PLAYER_BOX_ID + i);
        if (!box || g_swatch[i]) continue;
        RECT r;
        GetWindowRect(box, &r);
        MapWindowPoints(NULL, setup, (POINT*)&r, 2);
        int h = r.bottom - r.top;
        g_swatch[i] = CreateWindowExA(0, "KufModSwatch", "", WS_CHILD | WS_CLIPSIBLINGS, r.right + 10, r.top,
                                      h + 8, h, setup, (HMENU)(INT_PTR)(9000 + i), g_inst, NULL);
        SetWindowLongPtrA(g_swatch[i], GWLP_USERDATA, i);
        SetWindowPos(g_swatch[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SyncSwatches();
    Log("setup screen decorated");
}

static LRESULT CALLBACK SetupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC original = g_setupProc;
    if (m == WM_KUFMOD_DECORATE) {
        CreateSwatches(h);
        SetTimer(h, SYNC_TIMER, 200, NULL);
        return 0;
    }
    if (m == WM_TIMER && w == SYNC_TIMER) {
        SyncSwatches();
        return 0;
    }
    if (m == WM_COMMAND && g_room && AiRoomCommand(h, w, l, original)) return 0;  // computer slots
    if (m == WM_NCDESTROY) {
        KillTimer(h, SYNC_TIMER);
        ClosePalette();
        memset(g_swatch, 0, sizeof g_swatch);
        SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)original);
        g_setup = NULL;
        g_room = false;  // g_mpChoice keeps the host's list for the match start
        g_choice = g_spChoice;
    }
    return CallWindowProcA(original, h, m, w, l);
}

// Polls for the setup window, then hands over to the game's UI thread via SetupProc.
static DWORD WINAPI Watcher(LPVOID) {
    DWORD pid = GetCurrentProcessId();
    for (;;) {
        Sleep(250);
        ApplyCommandHotkeys();
        if (g_setup) continue;
        for (HWND h = NULL; (h = FindWindowExA(NULL, h, "XWndClass", NULL)) != NULL;) {
            DWORD owner = 0;
            GetWindowThreadProcessId(h, &owner);
            if (owner != pid) continue;
            bool setup = GetDlgItem(h, PLAYER_BOX_ID) && GetDlgItem(h, RACE_BOX_ID);
            bool room = g_mpColors && GetDlgItem(h, ROOM_SLOT + 5) && GetDlgItem(h, 800) && GetDlgItem(h, 786);
            if (!setup && !room) continue;
            if (room) {  // a new room: nothing chosen yet
                memset(g_named, 0, sizeof g_named);
                memset(g_slotName, 0, sizeof g_slotName);
                for (int i = 0; i < NUM_SLOTS; i++) g_mpChoice[i] = g_hostList[i] = -1;
                AiRoomReset();
            }
            g_room = room;
            g_choice = room ? g_mpChoice : g_spChoice;
            g_setup = h;
            g_setupProc = (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
            SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)SetupProc);
            PostMessageA(h, WM_KUFMOD_DECORATE, 0, 0);
            Log("found setup window %p", h);
            break;
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_inst = inst;
    DisableThreadLibraryCalls(inst);
    GetModuleFileNameA(inst, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
    sprintf_s(g_logPath, "%skufmod.log", g_dir);
    // Only change KingdomUnderFire.exe; in any other program this is just dinput8.dll.
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    if (_stricmp(name ? name + 1 : exe, "KingdomUnderFire.exe")) return TRUE;
    g_slide = (uintptr_t)GetModuleHandleA(NULL) - 0x400000;
    // A second (third...) copy of the game on this PC is instance 2 (3...): it writes kufmod2.log
    // and uses its own network port (lan.cpp). The mutex lives as long as the process.
    for (g_instance = 1; g_instance < 8; g_instance++) {
        char mutexName[64];
        sprintf_s(mutexName, "Local\\kufmod-instance-%d", g_instance);
        HANDLE m = CreateMutexA(NULL, TRUE, mutexName);
        if (m && GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (m) CloseHandle(m);
    }
    if (g_instance > 1) sprintf_s(g_logPath, "%skufmod%d.log", g_dir, g_instance);
    {  // start a fresh log each time the game starts
        FILE* f = NULL;
        if (!fopen_s(&f, g_logPath, "w") && f) fclose(f);
    }
    Log("kufmod 2.0 loaded, base %p", (void*)(g_slide + 0x400000));
    LoadConfig();
    InstallEconomy();
    InstallLan();
    InstallAiMp();
    InstallPerf();
    InstallMenuFps();
    InstallTurns();
    InstallPathing();
    InstallCombat();
    InstallCampaignTimer();
    InstallRandomMaps();
    if (!InstallLocusQuestion()) Log("unexpected game code at 0x57CC39 (different game version?). Locus question unchanged.");
    if (!InstallKeyHook()) Log("unexpected game code at 0x502D19 (different game version?). Hotkeys disabled.");
    if (!InstallLabelHook()) Log("unexpected game code at 0x503F8D (different game version?). Labels unchanged.");
    if (!InstallBuildButton()) Log("unexpected game code for the grid fill (different game version?). Build button off.");
    if (!InstallColorHook()) {
        Log("unexpected game code at 0x4C7E3C (different game version?). Colors disabled.");
        return TRUE;
    }
    if (g_mpColors && !InstallRoomChatHook()) {
        Log("unexpected game code at 0x6E7FE0 (different game version?). Multiplayer colors off.");
        g_mpColors = false;
    }
    HANDLE t = CreateThread(NULL, 0, Watcher, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
