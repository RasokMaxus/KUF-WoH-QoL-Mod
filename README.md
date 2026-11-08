# KUF WoH Mod

A gameplay and interface mod for **Kingdom Under Fire: A War of Heroes (GOLD Edition)** on Steam.

- **Standard match mode:** start with 0 gold and 4 workers; the default mode for every map.
- **Mines never run dry:** exhausted mines and veins keep paying a third of a normal trip.
- **Grid hotkeys:** Q W E / A S D / Z X C for the 3×3 grid, with the real key shown on every button.
- **Build button** for Peasants and Gnome Slaves, with new hammer icons.
- **Player color picker** on the skirmish setup screen.
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
| `command-hotkeys` | change the letter of any command (names in `kufmod_commands.txt`) |

## Multiplayer

The economy changes alter the game's rules. Everyone in a multiplayer match needs the mod with the
same `standard-mode` and `empty-nodes` settings, or the match will go out of sync. With both set to
`false`, only interface features remain.

## Compatibility

Made for the current Steam GOLD Edition as of November 2026 (`KingdomUnderFire.exe` SHA-256
`D72BC8814C4E3C5614B893C79FDE5AC13BF17FCAA7F830DFF8F4E240126E5CFC`). Every change checks the exact
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
  `src/icons_data.h`; `res.py` lists and extracts files from the game's `.res` archives.
- `docs/REVERSING.md` — notes on how the game works internally, with the addresses the mod uses.

## License

The mod's code and art are under the [MIT License](LICENSE).

## About

This is a fan-made mod, not affiliated with or endorsed by the game's developers or publishers.
It contains no game files: the repository holds only the mod's own code and art.
