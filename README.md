# FFX-60fps

A 60 fps mod for **Final Fantasy X HD Remaster** (Steam, Windows).

The game's logic still runs at its native 30 Hz, so gameplay, timing and battles behave as in the original. The mod
draws an extra frame between each pair of logic ticks, so the camera, characters and animations move at 60 fps. It
also fixes the effects, menus and inputs that would otherwise run at double speed or lose button presses.

## Install

1. Download the release zip.
2. Copy `dbghelp.dll` and `fps60.ini` into the game folder, next to `FFX.exe`.
3. Start the game from Steam.

To uninstall, delete `dbghelp.dll`.

- **FFX-2:** not affected. `FFX-2.exe` loads the same file, but the mod stays inactive there.
- **Other `dbghelp.dll` mods:** they can't be installed at the same time, since they use the same file name.

## Settings

`fps60.ini` is read when the game starts. Each fix has its own on/off switch under `[rows]`, with a comment
saying what it does.

| Key | Action |
|---|---|
| F7 | Turn 60 fps smoothing of the camera and characters on or off |
| Numpad 0 (hold) | Fast-forward the game (speed set by `DEV-TURBO-SPEED`; turn off with `DEV-TURBO=0`) |

`fps60.log` in the game folder lists what the mod found and turned on.

## Known issues

- Cloth (sleeves, skirts) and ship sails swing faster than in the original.
- Pre-rendered movies play at their original 30 fps.

## Build

Visual Studio 2026 (toolset v145), Win32:

```
MSBuild fps60.vcxproj /p:Configuration=Release /p:Platform=Win32
```

The output is `artifacts/Release/dbghelp.dll`.

## License

Apache-2.0 (`LICENSE`). MinHook (`third_party/minhook`) is BSD-2-Clause.
