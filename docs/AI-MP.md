# Computer players in multiplayer

Code: `src/aimp.cpp` (setting `multiplayer-computers`, default on). Status 2026-11-10: works on two
game copies on one PC (LAN): computers build and fight, teams and alliances work, the match stays
in sync (27 equal sync checks in a full match), it goes on when a human leaves. Not tested yet:
two real PCs, winning by destroying a computer.

Addresses are for KingdomUnderFire.exe 1.416a/b (image base 0x400000).

## How the game runs a computer player

- Each computer player has an AI object at `player+0x1158` (0x2220 bytes, constructor 0x5D0920),
  created by player vtable slot `+0x14` (0x5250D0) for slot records with type 2 (computer) that
  aren't the local player.
- The script is Python 1.5 (`python15.dll`, scripts in `res\python\*.pyc`, native module
  `kufmain`, method table 0xB47B98, 120 functions). The runner 0x5D1300(ai, "main") loads the
  module and calls `initialize`, then `main`. After that the player update calls 0x5D1150 every
  simulation step (0x53E620 for one player class, vtable 0xA41848; 0x525C70 for the other,
  vtable 0xA3D690), which calls `main` again every 32 steps. The AI object's script module is
  at `ai+0`, the player at `ai+4`; while a script runs the current player is `[0xBEDC88]`.
- **The scripts change units directly.** They don't send orders through the network command
  queue `[0xB870C8]` (checked: no AI orders enter it). So a computer player must run on every PC
  at the same simulation step. It can: its timing comes from the simulation, and
  `kufmain.Random` uses the game's synced random generator `[0xBED1E8]` (0x4943A0). `whrandom`
  (seeded from the clock) only appears in the campaign scripts.
- The game has no game-state check between PCs (the network queue only sums the orders,
  `+0x954`), so the mod logs `sync check: step N seed S draws D` every 512 AI steps (seed and
  draw count `[0xB827DC]` of the random generator). These lines must be equal on every PC.

## What the game switches off in network games, and what the mod does

`[0xB83AD0]` = 1 in a network game. Four things stop a computer player there:

1. **Players.** The network start (`InitNetworkPlay` 0x58E5A0, then 0x4CFD28) only knows the
   network players: one network node per start-info player (`[0xBED230]`, queue 0x6AFC20), so a
   computer can't be one of them. The mod adds the computers in the game-start function 0x4C77D0
   at 0x4C7E0D, just before it reads the player count `[0xB83AD6]` for the start positions
   (0x4D5860 → 0x4E0920, synced random): after the n network players, slot record
   `0xB83CDC + i*0x40C` type 2, race (1000 / 2000 / 4000 random), flag `+8` = 1 (in the game),
   team byte `game+0x5E0+i`, name `game+0x500+4i` (CString assign 0x429000), alliances with
   same-team players (0x4E7FC0, cdecl, writes `player+0x1048+2*other`: 0 ally, 1 enemy, 2 self),
   `[0xB83AD6]` = total + 1, `game+0x5E8` = total. The number is capped to the map's free start
   positions (`ebp-0x2C8`), because 0x4E0920 would loop forever.
2. **The start-of-match script call** (0x4C8959 / 0x4C96E7) skips computers in network games.
   The mod leaves it skipped: there it would run before the players' start buildings exist, and a
   script that finds no buildings picks its island plan and never builds. Instead the mod starts
   each computer's script itself (0x5D1300 "main") at its first simulation step on which the
   player's building list (`[0xB870C0] + 0x2EC44 + player*16`, a vector) is not empty.
3. **The map's AI data.** The game loads it while the map loads (0x5D4030, at 0x4A390E and
   0x4A4B89) and updates it every frame (0x5D4820 at 0x4CD6E0) only outside network games. It
   fills, among others, the unit flag table 0xBF12A0 that every `kufmain` type search uses
   (0x5F6280 with its third argument 1). Without it the scripts find no buildings or units at all,
   decide their enemies are unreachable by land (`Check_Distance_Area`), pick an island plan
   (`No_Land`, `H_No_Land1`) and only gather gold. Each gate is
   `cmp byte [0xB83AD0], al / jne skip`; the mod lets it through in network games with computers.
4. **The end of the match.** A network game ends only through "out of the game" orders
   (type 0x64, kind 0x320, player), which each PC sends for its own player: when it has nothing
   left (`player+0xE30` ≤ 0, 0x4C0C70), when all other network players have left
   ("last one standing", 0x4CFEC9), or after another player's surrender when it has no enemy left
   (0x4DC34D). The handler (0x4DBC88) then shows victory if none of the player's enemies is still
   in the game (slot record `+8`), else defeat. The mod: computers start with `+8` = 1; the
   network queue's player state (vtable slot `+0x30`, 0x6B0880; the game clears `+8` of players
   reading 0 or 4 every frame at 0x4CF89A) answers 1 ("playing") for computers; "last one
   standing" waits while an enemy computer is in the game; a computer with nothing left is marked
   out of the game at the same step on every PC; a player with no enemy left in the game sends
   his own "out" order (victory), as single player does (0x4C0D37).

## The room

Room slot `i` controls: `816+10i` + 0 team button (`X` or the team letter), +1 name box, +4 race
box (HUMAN, DEVIL, RANDOM). The game's slot state `[0xBED22C]+0x3D0+4i` is 0 OPEN (also a
player's slot), 1 CLOSE, 2 OBSERVER. The game fills the name box by position (0x6E7470: OPEN,
CLOSE, OBSERVER, then a player's name) and ignores any other selection (0x6E4303). Team buttons
A–H (921–928) send `id - 0x398` = team 1–8 for the player's own slot.

The mod (room window procedure, `AiRoomCommand`, and `AiRoomSync` every 200 ms):

- When the host opens an empty slot's list (`CBN_DROPDOWN`) it adds COMPUTER; the entry is taken
  out again once the list is closed and the slot isn't a computer (a permanent entry collided
  with the game's by-position editing when a player joined).
- Picking COMPUTER: the mod closes an OPEN slot for the game (nobody can join) and marks it as a
  computer (race random, no team). Picking OPEN / CLOSE / OBSERVER removes it.
- A computer slot shows COMPUTER, the race box (enabled for the host) and the team letter. The
  host changes the race in the race box and the team by clicking the team button; the game never
  sees these clicks.
- The host sends the list as a hidden room line `#KUFC I slot:race:team ...` (race 1000 / 2000 /
  4000, team 0 none, 1–8 = A–H) after each change and when the room's players change. It uses
  the same room chat hook as the multiplayer colors (`multiplayer-colors` must be on).
- Computers get an automatic free color (`g_aiFirst` in `PickColor`).

## Not done

- The in-game Message / Alliance screen lists only the human players.
- The host can't choose a computer's color.
- Two real PCs, and the victory after destroying a computer, are untested.

## Research tools (setting `lan-trace: true`)

- `pytrace.txt` (`pytrace2.txt` for a second copy): every call of a list of `kufmain` functions
  by the scripts, with arguments and result (method table pointers wrapped).
- `pyerr.txt`: the scripts' Python errors, and at script start and steps 200 / 2000 / 6000 each
  computer's plan, its cells and its manager's state, test queries, and every hall's state.
- `kufmod.log`: the game-end orders as they are executed, and the orders entering the command
  queue.
- `tools/pyc15.py`: a Python 1.5 `.pyc` reader / disassembler for the game's scripts
  (e.g. `pyc15.py main.pyc initialize` in `res\python`).

How the cause was found (for the record): the stuck computer gathered gold forever. The trace
showed its plan was an island plan; the plan dump showed `Land_Yes_or_No_Enemy: [0, 0]`; the script
found no buildings of its own; a direct query found no buildings or units of any player; the type
filter's table 0xBF12A0 is filled by the map's AI data, which network games skip.
