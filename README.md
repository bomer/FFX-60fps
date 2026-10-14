# FFX-60fps

A 60 fps mod for **Final Fantasy X HD Remaster** (Steam, Windows).

The game's logic still runs at its native 30 Hz, so gameplay, timing and battles behave as in the original. The mod
draws an extra frame between each pair of logic ticks, so the camera, characters and animations move at 60 fps. It
also fixes the effects, menus and inputs that would otherwise run at double speed or lose button presses.

## Install

1. Download the release zip.
2. Copy `dbghelp.dll`, `fps60_ffx.dll` and `fps60.ini` into the game folder, next to `FFX.exe`.
3. Start the game from Steam.

To uninstall, delete `fps60_ffx.dll` (FFX then runs stock), or also `dbghelp.dll` if no other game mod uses it.

- **`dbghelp.dll` is a small shared loader.** It loads `fps60_ffx.dll` in FFX and `fps60_ffx2.dll` in FFX-2, when
  that file is present; otherwise the game runs stock. FFX-2 is not affected unless its own mod DLL is installed.
- **Other `dbghelp.dll` mods:** they can't be installed at the same time, since they use the same file name.

## Settings

`fps60.ini` is read when the game starts. The release copy lists only the settings players may want to change;
everything else uses its built-in default. The full list of switches (one per fix under `[rows]`, plus test and
diagnostic switches) is the development copy, `fps60.ini` in the source tree. `ship/fps60.ini` is the release copy;
`tools/pack_dist.py` (outer repo) packs it and checks that every value in it equals the built-in default.

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
MSBuild loader.vcxproj /p:Configuration=Release /p:Platform=Win32
MSBuild fps60.vcxproj /p:Configuration=Release /p:Platform=Win32
```

The outputs are `artifacts/loader/Release/dbghelp.dll` (the shared loader) and `artifacts/Release/fps60_ffx.dll`.
`x2.vcxproj` builds `fps60_ffx2.dll`, the FFX-2 mod (work in progress).

Sources: `src/loader/` (shared loader), `src/x/` (FFX mod), `src/x2/` (FFX-2 mod). The two mods share no code.

## License

Apache-2.0 (`LICENSE`). MinHook (`third_party/minhook`) is BSD-2-Clause.
