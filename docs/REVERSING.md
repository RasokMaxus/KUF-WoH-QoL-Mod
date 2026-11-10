# How the mod works: reverse-engineering notes

Notes on `KingdomUnderFire.exe` from the Steam GOLD Edition 1.416a (SHA-256
`D72BC8814C4E3C5614B893C79FDE5AC13BF17FCAA7F830DFF8F4E240126E5CFC`); 1.416b (`B21E7A0A...E203`,
9 November 2026) changed only version strings, the signature and resources, so every address
below is the same. All addresses are virtual
addresses at the exe's preferred image base `0x400000`; Windows loads the game elsewhere (ASLR), so
the mod adds the difference (`g_slide`) at runtime.

The exe is a 32-bit MSVC/MFC program, not packed, with RTTI class names. Game logic is native C++;
`res\python\*.pyc` (Python 1.5) holds AI and mission scripts.

## Loading the mod

The game imports `DirectInput8Create` from `DINPUT8.dll`. `dinput8` is not on Windows' KnownDLLs
list, so the loader looks in the game folder first. The mod is built as `dinput8.dll`, exports the
six DirectInput functions and forwards them to the real DLL in the system folder (`src/proxy.cpp`).
`DllMain` applies all changes before the game's own code runs. Every change compares the exact
bytes it expects first (`Hook`, `HookCall`, `Patch` in `src/kufmod.cpp`).

## Game state

| What | Where |
|------|-------|
| Game object pointer | `0xB8282C` |
| Players | `game + 0x48 + i*4` (10 = neutral) |
| Player iron / gold / mana | `player + 0xA08 / 0xA0C / 0xA10` (display copies `+0xA14..0xA1C`) |
| Supply used / capacity | `player + 0xE24 / 0xE28` |
| Player color index | `player + 0x5C0` |
| Match settings block | `game + 0xC78` (lobby copy `0xBEDA08`) |

Match settings bytes: `+0` mode (0 Gold, 1 Casual, 2 Family), `+1` resource sharing tier,
`+2` No Sub Hero, `+3` free workers rule, `+4` Map, `+5` Nearby Allies (0 = on), `+6` Rich
(500,000 of each resource), `+7` Beta. Feature switch `0x4319A0(n)`: features 8–11 need Casual or
Family, 12–15 Family, 16–18 Beta.

## Economy

**Harvesting.** Each resource node's harvest function is vtable slot `+0x178`: gold `0x529D40`
(100 per trip), mana `0x529E00` (20), iron `0x529EE0` (60), all scaled by the over-supply penalty
from `0x529A50`. Node remaining / maximum: `+0x14F8 / +0x14FC`. Each ends a trip with
`cmp edi,esi / cmovg edi,esi / sub [ebx+0x14F8],edi` at `0x529D5B`, `0x529E30`, `0x529F14`;
the mod calls its own routine there instead. Workers skip a node reporting 0 at `0x544F34` and
`0x5455A1` (`jle`), which the mod removes.

**Match start.** `0x4C77D0` loads the map; skirmish setup runs `0x4C8242`–`0x4C87ED` per player.
Mode byte `+3` = 0 skips the free workers at `0x4C865C`; the count is `[ebp-0x3D8]`. Each spawned
worker is charged and refunded, but the "saved" gold is re-read after two workers, so more than two
workers leave gold short; the mod sets gold to 0 after spawning instead (`0x4C8754`). The starting
4,000 gold is set in the player constructor (`0x522C22`). Campaign missions take a different branch
(`0xB83AD2` ≠ 0) and never reach this code.

**Mode name.** "Gold" is at `0xA1D848` in an 8-byte slot, appended with length 4 at `0x4320A8`.

**Default mode.** The setup screens initialise the settings block with Casual: `0x432230` and
`0x432270` write mode 1 and copy that mode's bytes from the tables `0xB33AC0` (sharing tier) and
`0xB33AC4` (free workers rule), indexed by mode. `0x432270` then steps the mode back
`multiplayergamemode` times (from `res\kufmain.ini`, stored at `0xB82720`). The mod makes both
start on mode 0.

## Player colors

Color numbers index a 32-bit table at `0xB3E390` (Orange, Teal, Blue, Red, Purple, Yellow, Cyan,
Olive) and 16-bit copies from `0xBEA770`. They are written once per match at `0x4C7E43` from an
array that `0x4D5860` fills (skirmish: start positions are shuffled, `0x4E0920`). The skirmish
setup screen (`CSingleGameRegistrationMenu`) is made of real Win32 controls: player boxes are
combo boxes with control IDs 350–357, race boxes 360–367.

**Multiplayer room** (`CXCreateDialog`, a 1200×900 window of Win32 controls). Slot i (0–7, left
and right column alternating) uses control IDs 816 + 10·i + 0 kick button, 1 player name box,
2 record "(level)wins-losses", 3 portrait, 4 race box, 5 rank icon (`CXPNetGrade`, 33×28, which
the mod replaces with its color swatch). Chat: box 786, Send 787, messages 784; Start 800. Room
chat goes out through the room client `[[0xBED22C]+0x188]`, vtable `+0xA0` (message 0x4027, text)
and comes in through the client's message handler `0x6E6600` (message 0x4020, string
`"<type><sender>\n\r<text>"`, shown at `0x6E6687`). The mod sends its color messages as chat lines
starting with `#KUFC ` and hides them there; the host keeps the list and every PC applies it at
`0x4C7E3C`. In a network game `0xB83AD0` = 1.

## Keyboard and the command grid

Input comes from DirectInput; scan codes become key codes through the table at `0xB3E3C0`. The
in-game menu (`KufMenu`, vtable `0xA3BD0C`) handles keys from `0x502D19` (key in `edi`), trying in
order: each grid button's own letter, Numpad 7-8-9 / 4-5-6 / 1-2-3 (grid slots 0–8), the six command
buttons' letters, Insert/Home/PgUp/Delete/End/PgDn (command buttons by position), and Q–Y for hero
skills.

| Menu field | Meaning |
|------------|---------|
| `+0x440 + i*0x10` | command button i: id, icon, hotkey letter, state |
| `+0x4A0 + i*0x10` | grid slot i: id (-1 empty), icon (`0xD` empty), hotkey, state (`0x50` greyed) |
| `+0x284 / +0x286` | pending or hovered button: kind (2 command, 3 grid) / index |
| `+0x6A4` | grid page |

Pressing a button runs `KufMenu` vtable `+0x2C` (`0x512F80`); grid buttons branch at `0x513396`.
The grid is filled by vtable `+0x64` (`0x50EFF0`), which the mod wraps for the Build button.

**Command table.** Pointer `0xB82824`, count `0xB82810`, 0x200 bytes per command: `+8` type
(`0x12C` build a structure, `0x190` train a unit), `+0x10` hotkey (letter − 'A'), `+0x14 / +0x94 /
+0x114` texts. Texts use markup: `\1` starts the hotkey highlight, `\0` ends a colored part, `\n`
is a new line. Hover labels are drawn from `0x503F8D` (and `0x503A2D` for buttons with a cost box).

## Images (.URE)

Interface pictures are "RLE Encode File" images (`tools/ure.py` reads and writes them).

- Header: 32-byte title, version, an extra int (version 2), frame count, then one frame header per
  frame (0x6B bytes for one-part frames: size at `+8`, ...), the size of block 1, then **block 1
  (RGB555) and block 2 (RGB565)** with the same frames; the game uses one depending on the display.
- **Version 1** (e.g. `MenuIcon8.URE`, also the game's runtime format): each row starts with its
  length in words; 16-bit ops follow until the row is full or `0xFFFF`: low byte alpha
  (0 = transparent), high byte count with bit 7 = "repeat the next color".
- **Version 2** (e.g. `MenuIcon8_2.URE`): each row has a color section and an alpha section, each a
  length byte followed by runs (`0x80|n` + one value repeated, or `n` + n values).

The grid draws icon numbers below 1000 from `MenuIcon8.URE` (in `res6.res`) and 1000+ from
`res\misc\MenuIcon8_2.URE`, loaded at `0x515044`. Frame 13 of `MenuIcon8.URE` is the empty grid
tile the grid draws under every icon. The mod writes `res\misc\kufmod_icons.URE` (the player's
`MenuIcon8_2.URE` plus two hammer frames) and points `0x515044` at it.

`.res` archives ("EFB KUF Resource File") hold a directory of entries (name, folder, flag, offset,
size) and store files uncompressed (`tools/res.py`).

## Frame pacing and order delay (2.0)

Found with `perf-trace` (`src/perf.cpp`): it times the main frame's state update and logs every
order from the moment it is queued until it is carried out.

- **The game loop** runs from MFC's idle handler (`0x4FEE30`), which calls the main frame's state
  update `0x593F80` (state number at `frame+0x128`).
- **Animated menus** (state 19, case `0x595C97`): `menu->Update()` (`[0xBED218]` vtable `+0x14`,
  `0x721B40`; animations count frames at `menu+0x2B4`), then `Render()` (`[0xB82808]` vtable
  `+0xC`, which also draws the game's own cursor), then a busy-wait on the frame timer `0xBED7CC`
  (rate 20, set at `0x401C20`). `menu-fps` replaces the case: Update every 50 ms, Render at 60 fps.
- **Orders** go into the command queue `[0xB870C8]` (vtable `+4`: single player `0x58B8E0`,
  network `0x6AFC00`) and are carried out by `0x4DB870(len, data)`. They run in **turns** of N
  ticks (tick `[0xB3BAE8]` = 30 ms): N = word table `0xB3BAF8` {6, 10, 14} at index `0xB3BAF4`
  (single player 0, network 1, set at `0x4BF6F3`; the Network Speed order is handled at `0x4DC66B`;
  the options dialog keeps a copy at `0xBED25C`). The game loop reads N at `0x4CF375` and
  `0x4CFBD5`. An order waits for the end of the current turn plus one turn.

## Unit movement (2.0)

`perf-trace` also writes `pathtrace.txt`: every movement state change of your units, who blocks
them, and what they attack.

- **Movement state machine** `0x47E540` (thiscall, 2 args), state word `unit+0x31A`: 3 turn and
  try a step, 4 walk, 10 arrived, 11 blocked, 12 give up, 13 wait then new path (`0x47EE00`, retry
  counter `+0x362`), 14 new path now. Position `+0x18/+0x1C`, target `+0x178/+0x17C`, facing
  `+0x298` (16 directions), owner word `+8`.
- **Blocked** (state 11, `0x47F760`): the step test `0x47D530` returns 0 free, 1/2 blocked, 3 a
  unit is in the way (its index in the object table `[0xB870C0]+0x2C38`). For a unit in the way
  (`0x47F7F8`) the game resolves head-on meetings (`0x47ECD0`) or decides who yields; the other one
  waits up to 50 ticks (`+0x360`). "Avoid units" (`+0x271`) makes the new path go around units.
- `unit-avoidance` (`src/pathing.cpp`): yield at once unless the blocker is an own unit that is
  walking, and set `+0x271` on the two other "new path now" branches (`0x47F7D9` for results 1/2,
  `0x47E70F` for units whose mode word `+0x31C` is 1).

## Combat targeting (2.0)

- **Attack routine** `0x4850C0` (unit vtable `+0x18C`, vtable `0xA36C30`; runs while the order
  word `unit+0xC` is attack), target `+0x120`, sub-state `+0x31C`.
- **Target chooser** `0x496880` (18 callers): goes through enemies in range (`0x4E7180/0x4E7340`),
  scores each with vtable `+0x210` (`0x489D70`: walking path length for up to 4 enemies, else 2000
  for units / 4000 for buildings / +1500 for workers); the lowest wins. Re-checked every 60 ticks.
- **Leash**: the target is dropped when farther than max(2 × type data `+0x174`, reach + 1, search
  range + 1) (`0x4855D9`, `0x486F39`).
- **Building spots**: `0x48DDA0` walks a line from the building's center toward the unit and takes
  the first free cell (called at `0x4852FE`).
- Type data: `[0xB870F4] + type * 0xAF8`; `+0x17C` ground attack, `+0x18C` air attack, `+0x278`
  search range. Workers are types 0 (Human) and 15 (Devil).
- `target-priority` (`src/combat.cpp`) adds a tier to the score, wraps the chooser to keep the
  current target (towers until they fall), widens the leash, and runs the spot search toward 8
  directions.

## Maps and timer (2.0)

- **Random maps**: `res\Map\Random\N+.def` = title, `min,max` players, optional map names. The
  loader `0x6F2070` skips Custom-folder maps (`+0x5A == 1`, at `0x6F2730`); the pickers
  (`0x6DE631`, `0x6EC412`, `0x6EC869`) use `rand() % count`, and `rand()` is only seeded with 999
  (`0x4A2869`, `0x4A3A69`). Custom maps' player count `+0x4C` is not reliable; the mod uses the
  "(N)" in the name.
- **Melee/UMS default**: the map-change handler `0x6EC4E0` sets the game type box (control `0x158`)
  to Use Map Settings for Custom-folder maps with scenario data (`0x6F0C40`); hook at `0x6EC5A7`.
- **Clock**: the top bar draws minutes:seconds from `game+0x604` (ms) at `0x505B93` but skips it in
  campaign (`word 0xB83AD2 == 1`, je at `0x505B8D`).
- **Player colors**: the table `0xB3E390` (10 × 0xRRGGBB, 8 used) only drives the swatches, the
  minimap and the hue matching for automatic colors (builder `0x4E16E0`, automatic pool
  `0xBEA7AC`). Unit and building art stores one ready-colored copy per original color, so new
  player colors would need new art copies.
