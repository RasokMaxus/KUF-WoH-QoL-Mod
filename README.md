# KUF WoH QoL Mod

A gameplay, interface, and quality of life mod for **Kingdom Under Fire: A War of Heroes (GOLD Edition)** on Steam.

- **Standard match mode:** start with 0 gold and 4 workers; the default mode for every map.
- **Mines never run dry:** exhausted mines and veins keep paying a third of a normal trip.
- **Grid hotkeys:** Q W E / A S D / Z X C for the 3×3 grid, with the real key shown on every button.
- **Build button** for Peasants and Gnome Slaves, with new hammer icons.
- **Player color picker** on the skirmish setup screen, and in multiplayer rooms (click the square
  next to your name; first come, first served).
- **Computer players in multiplayer:** the room's host can add computer opponents or teammates,
  with race and team, in Locus Online and LAN rooms (in testing).
- **LAN play** over a home network, Radmin VPN or Hamachi, next to Locus Online (in testing).
- Everything can be switched on or off, and every hotkey changed, in one settings file.

See [PATCH_NOTES.md](PATCH_NOTES.md) for the full list of changes and how to change your hotkeys.

## Install

1. Download **`dinput8.dll`** from the [`Release`](Release) folder (or the whole repository as a
   ZIP: green **Code** button → **Download ZIP**).
2. Drop `dinput8.dll` into your Kingdom Under Fire folder, next to `KingdomUnderFire.exe`.
   On Steam: right-click the game → **Manage** → **Browse local files**.
3. Start the game. The first start creates **`kufmod.yml`** (your settings) in the same folder.

That's all. No game file is changed: the mod makes its changes in memory while the game runs.

## Uninstall

Delete `dinput8.dll` from the game folder. You can also delete `kufmod.yml`, `kufmod.log`,
`kufmod_commands.txt` and `res\misc\kufmod_icons.URE`, which the mod creates.

## Settings

Open `kufmod.yml` in the game folder with any text editor. Each setting is explained in the file.
Changes apply the next time the game starts.

| Setting | What it does |
|---------|--------------|
| `standard-mode` | "Gold" mode becomes "Standard" (0 starting gold, 4 workers) and the default mode |
| `empty-nodes` | exhausted mines/veins keep paying a third of a trip |
| `grid-hotkeys` | turn grid hotkeys on/off and choose the 9 keys |
| `build-button` | the Build button for workers |
| `multiplayer-colors` | the color swatches in multiplayer rooms |
| `command-hotkeys` | change the letter of any command (names in `kufmod_commands.txt`) |
| `lan-play` | adds LAN to Multi Play (see below) |
| `lan-ip`, `lan-peers` | LAN: your address, and extra addresses to announce rooms to |
| `lan-trace` | write network and computer-player details for bug reports |
| `multiplayer-computers` | computer players in multiplayer rooms (see below) |
| `locus-new-account` | the answer to Locus's "start new account?" question: `no` (default), `yes` or `ask` |

## Multiplayer: Locus Online or LAN

The Steam version plays multiplayer through **Locus Online** (the Locus GameChain lobby). The mod
adds **LAN play** next to it, so you can also play over a home network or a virtual LAN such as
**Radmin VPN** or **Hamachi**, the way the original CD version did.

> **LAN play is in testing.** It works in the mod's source (see *Building from source*) and will be
> in the next download in [`Release`](Release); the current download does not have it yet.

With `lan-play: true` in `kufmod.yml`, **Multi Play** asks which one you want.

### Locus Online (as before)

1. Multi Play → **Locus Online**.
2. Choose your profile in the Registry and enter its password (your Locus account).
3. Wait until the lobby says **Participating in GameChain** (the first time on a PC this can take
   6–8 minutes), then create or join a room.

Needs an internet connection and the LocusGameChain program that comes with the game. Players
without the mod can join you, **but only if the match rules are the same**: set `standard-mode`
and `empty-nodes` to `false`, or every player needs the mod with the same settings.

### LAN (Radmin VPN, Hamachi or a real LAN)

1. Everyone joins the same network: the same Radmin VPN or Hamachi network, or the same home LAN.
2. Everyone installs the mod and sets `lan-play: true` in `kufmod.yml`, with the same
   `standard-mode` and `empty-nodes` settings.
3. Multi Play → **LAN** → choose a profile → **OK**. No password and no Locus account are needed;
   the lobby says **LAN play**.
4. One player creates a room; the others see it in the room list and join.

The mod picks your Radmin/Hamachi address by itself. If it picks the wrong one, set it with
`lan-ip` (e.g. `lan-ip: 26.1.2.3`). If the others don't see your room, list their addresses in
`lan-peers` (e.g. `lan-peers: 26.4.5.6, 26.7.8.9`). Windows Firewall must allow Kingdom Under
Fire on the network (it asks the first time). LAN rooms only show up for LAN players and never
appear in Locus Online.

### Switching

You can switch every time you click Multi Play. Your profiles (the `multi` folder) work in both.
To go back to Locus only, set `lan-play: false`.

### Computer players

The room's host can add computer players, in Locus Online and LAN rooms alike:

1. Open an empty slot's list (OPEN / CLOSE / OBSERVER) and pick **COMPUTER**. The slot is closed for
   people, so nobody can join it.
2. Choose its race in the race box (Human, Devil or Random).
3. Click its team button to put it on a team (none, A ... H). Players and computers on the same
   team are allies.

Picking OPEN or CLOSE again removes the computer. The computers play the game's own AI and join
after the human players, with an automatic color. A match goes on while an enemy computer is
still playing, even if all other people have left; a computer with nothing left is out of the game.

> **In testing.** Every player in the room needs this version of the mod. The computer runs on every
> player's PC at the same time; `kufmod.log` writes a `sync check` line now and then, and these
> lines must be the same on every PC. Not tested yet: a match between two real PCs, and winning
> by destroying a computer.

### Keeping matches in sync

The economy changes alter the game's rules. Everyone in a match needs the same `standard-mode`
and `empty-nodes` settings, or the match will go out of sync. With both set to `false`, only
interface features remain.

## Compatibility

Made for the Steam GOLD Edition **1.416b** (9 November 2026, `KingdomUnderFire.exe` SHA-256
`B21E7A0A0A20671AD8B7A6743CB3F97811A1EE1184E2B718420BE729FA38E203`); also works with 1.416a
(`D72BC8814C4E3C5614B893C79FDE5AC13BF17FCAA7F830DFF8F4E240126E5CFC`), whose program code is
identical. Every change checks the exact
game code it expects before touching it. After a game update, any feature that no longer matches
switches itself off, and `kufmod.log` says which.

## Troubleshooting

- **Nothing changed in game:** check that `dinput8.dll` is in the same folder as
  `KingdomUnderFire.exe`, and look at `kufmod.log` there.
- **A feature is missing:** `kufmod.log` lists each feature as on, off (by your settings), or left
  off because the game code was different.

## Building from source

- `src/` — the mod (C++). Build with `src\build.bat` (Visual Studio with "Desktop development
  with C++"). The result is `src\dinput8.dll`.
- `tools/` — Python 3 tools: `ure.py` reads and writes the game's `.URE` images and converts them
  to PNG; `draw_hammers.py` draws the Build icons; `make_icons_data.py` encodes them into
  `src/icons_data.h`; `res.py` lists and extracts files from the game's `.res` archives;
  `pyc15.py` disassembles the game's Python 1.5 AI scripts.
- `docs/REVERSING.md` — notes on how the game works internally, with the addresses the mod uses.
- `docs/LAN.md` — how Steam multiplayer and Locus work, and how LAN play replaces Locus.
- `docs/AI-MP.md` — how the computer players work in multiplayer (the game switches its AI off in
  network games in several places; what the mod changes and how it was found).

## License

The mod's code and art are under the [MIT License](LICENSE).

## About

This is a fan-made mod, not affiliated with or endorsed by the game's developers or publishers.
It contains no game files: the repository holds only the mod's own code and art.
