# KUF WoH Mod — Patch Notes

## Where to change your hotkeys

All settings live in **`kufmod.yml`**, in your Kingdom Under Fire folder (next to
`KingdomUnderFire.exe`). It is created with the default settings the first time you start the game
with the mod. Open it with any text editor (Notepad works), save, and restart the game.

**Grid hotkeys** — the keys for the 3×3 grid (buildings, units to train). Each line is one row of
the grid, left to right:

```yaml
grid-hotkeys:
  enabled: true
  layout:
    - QWE
    - ASD
    - ZXC
```

**Command hotkeys** — the letters of the six oval command buttons (Attack, Stop, Patrol, …). Add a
line under `command-hotkeys:` with the command's name and the key you want, or `none` to remove it:

```yaml
command-hotkeys:
  attack: F
  stop: none
```

The mod writes **`kufmod_commands.txt`** to the game folder every time the game starts. It lists
every command's name (first column) and its current key, and marks with `*` the keys that are also
grid keys.

---

## Next version (in testing)

These changes are in the mod's source and will be in the next download in `Release`; the current
1.0 download does not have them yet. Every player in a multiplayer match needs the same version.

### Faster unit response

- The game carries out orders in "turns" of a few ticks (1 tick = 30 ms): an order waits for the
  end of the current turn and one more turn. Single player used 6-tick turns (an order took about
  0.25 s); it now uses 1-tick turns (about 0.03 s). Setting: `single-player-turn: 1` (6 = as
  before).
- Multiplayer Network Speed choices are now 3 / 6 / 10 ticks instead of 6 / 10 / 14, and matches
  start on the fastest one (Low): orders take about 0.1-0.15 s instead of 0.3-0.6 s. On a slow
  connection the game pauses briefly while it waits; pick a higher Network Speed in the game's
  options then. Settings: `network-speed: 3, 6, 10` and `network-speed-start: low` (low, high or
  extra). Every player needs the same values.

### Target priority

- Units choosing their own target used to switch every 1.8 s to whatever enemy was a little
  closer, and melee units let go of a target that stepped back a few steps (walking off toward the
  attack-move point, then coming back). Now:
  - they prefer enemy fighters, then workers and units that cannot attack, then towers, then other
    buildings (nearest first within each);
  - they keep their target unless something of a higher priority shows up or the target gets away;
  - they chase a target that backs off (up to twice their search range) instead of letting go.
- Your own attack orders are unchanged. Setting: `target-priority: true` (`false` = as before).
  Every player needs the same value.

### Units walk around blockers

- A unit blocked by another unit waited up to 1.5 s, even behind a unit that was not moving at all
  (for example one stuck behind a chicken), so whole columns stood still. Now a unit only waits
  behind one of your own units that is walking; behind anything else it walks around at once.
- When a unit asked for a new path after being blocked, the game often made one that ignored other
  units, so it ran into the same crowd again every tick (units twitching and bumping around a
  building or a fight, reinforcements entering in single file). These new paths now go around
  units. Setting: `unit-avoidance: true` (`false` = as before). Every player needs the same value.

### Smooth main menu

- The main menu and the other animated menus were drawn only 20 times per second, so the mouse
  felt slow there. They are now drawn 60 times per second; the menu animations keep their normal
  speed. Setting: `menu-fps: 60` (20 to 240; `0` = the game's own 20).
- `perf-trace: true` (off by default) writes frame times and mouse delay per second to
  `kufmod.log` (a help for bug reports).

### Computer players in multiplayer

- The room's host can add **computer players** to a multiplayer room, in Locus Online and LAN
  rooms alike. Open an empty slot's list (OPEN / CLOSE / OBSERVER) and pick **COMPUTER**; the slot
  is closed for people.
- Choose the computer's race in its race box (**Human**, **Devil** or **Random**), and click its
  team button to put it on a team (none, A … H). Players and computers on the same team are
  allies. Pick OPEN or CLOSE again to remove the computer.
- The computers play the game's own skirmish AI. They join after the human players and get a
  free color automatically.
- The match goes on while an enemy computer is still playing, even when all other people have
  surrendered or left. A computer with nothing left is out of the game; when no enemy is left,
  you win.
- The computer runs on every player's PC at the same time. `kufmod.log` writes a `sync check` line
  now and then; these lines must be the same on every PC (a help for bug reports).
- Setting: `multiplayer-computers: true` / `false` (needs `multiplayer-colors`).

### LAN play

- **Multi Play** asks: **Locus Online** (as before) or **LAN**. LAN play works over a home network
  or a virtual LAN such as Radmin VPN or Hamachi, without a Locus account or password.
- Rooms and the players in the lobby are shared over the network; the room's host runs everything
  the Locus servers did.
- Settings: `lan-play`, `lan-ip`, `lan-peers`, `lan-trace`. See the README.

### Player colors in multiplayer rooms

- The square next to each player's name in a multiplayer room becomes a color box: click your own
  to choose your color ("?" = automatic). First come, first served; the room's host keeps the list.
- Fixed: on some maps every player (and the computer) got the same orange color, when nobody
  started at the map's first start position. This also affected skirmish.
- Setting: `multiplayer-colors: true` / `false`.

### Locus "start new account?" question

- Locus sometimes asks at startup: "The Network has been restarted … start new account?" The mod
  now answers **No** for you, so the box no longer appears. Setting:
  `locus-new-account: no` (default) / `yes` / `ask` (show the question as before).

---

## Version 1.0

### Economy

**Standard match mode**
- The "Gold" match mode on the skirmish setup screen is now called **Standard**.
- Every player (human and computer) starts with **0 gold and 4 workers**, a more standard RTS
  opening: you build your economy from your workers instead of spending a starting bank.
- Standard is now the **default mode** for every map, instead of Casual. (To pick another mode,
  left/right-click the map information panel on the setup screen, as before.)
- Casual and Family modes are unchanged. Campaign missions are unaffected.
- Setting: `standard-mode: true` / `false`.

**Mines that never run dry**
- A gold mine, iron vein or mana node that reaches **0 remaining** no longer stops paying. Every
  trip from an exhausted node still returns **one third** of a normal trip:

  | Resource | Normal trip | Exhausted node |
  |----------|-------------|----------------|
  | Gold     | 100         | 34             |
  | Iron     | 60          | 20             |
  | Mana     | 20          | 7              |

- Workers keep mining an exhausted node on their own, and the computer players do too.
- Being over your supply limit still reduces each trip as before (the third is taken from the
  reduced amount).
- Setting: `empty-nodes: true` / `false`.

### Grid hotkeys

- The 3×3 grid can be used with **Q W E / A S D / Z X C** (changeable, see above). Each key
  presses the button in the same position, whatever that button is: buildings for workers,
  units for barracks, research for other buildings.
- **Button labels show the real key.** Hovering a button shows, for example, *Train Peasant (Q)*
  or *Build Barrack (W)*. The game's old highlighted letters in button names are removed.
- **The game's old building letters are switched off** while a grid is showing (pressing F no
  longer builds a Farm), so only the grid keys build.
- The numpad (7 8 9 / 4 5 6 / 1 2 3) still presses grid buttons too.
- Command buttons keep their own letters. When a grid key takes a letter (for example A or S while
  a worker's building grid is open), that command shows no key in its label until the grid closes.
- Hero skills on Q W E R T Y work as before whenever no grid is showing.
- Ctrl and Alt key combinations are never changed.

### Build button for workers

- Peasants and Gnome Slaves show a single **Build** button (Q) instead of their whole building
  grid. Press it to open the buildings; choose a building, press **Esc**, or select another unit
  to close it again. This keeps A, S and the other letters free for commands.
- New hammer icons: a forged steel hammer for the Human Alliance and a rope-bound stone hammer for
  the Dark Legion.
- Setting: `build-button: true` / `false`.

### Player colors (single-player skirmish)

- A color box beside each player on the skirmish setup screen. Click it to choose one of the eight
  player colors, or "?" to let the game decide. A color can only be chosen by one player; computer
  players whose usual color is taken get a free one.

### Notes

- **Multiplayer:** the economy changes alter the game's rules. Every player in a match needs the
  mod with the same `standard-mode` and `empty-nodes` settings, or the game will go out of sync.
- If anything does not work, see `kufmod.log` in the game folder: it says which features were
  switched on, and why one was left off.
