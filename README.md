# ahk-fix

Standalone C++ port of `soru_autox.ahk` (Soru Auto X/Z/C for Blox Fruits). It watches your own Soru
(position jump + a "Soru" / "DodgeQuick" sound, gated by a recent R press) and then sends the current
mode's key (X, Z or C). The mode key (Caps Lock by default) cycles OFF -> X -> Z -> C -> V -> OFF.

## Fix: Roblox 0.744 connection

Roblox 0.744 removed the 8-byte header in front of an Instance's name string, so every name read came
back empty. The tool could not find `Players` / `Workspace` (tray showed
`Waiting: couldn't find Players/Workspace ... [?, ?, ...]`), and the child-stride detection, which counts
named children, never picked 0x10 either. `ReadName` now reads the string at `namePtr+0` and falls back
to `namePtr+8`, so it works on both layouts.

## Build

MSVC (x64 Native Tools prompt), with [nlohmann `json.hpp`](https://github.com/nlohmann/json/releases)
(3.11.x) placed next to `SoruAutoX.cpp`:

    rc meow.rc
    cl /EHsc /std:c++17 /O2 SoruAutoX.cpp meow.res /Fe:meow.exe /link /MANIFEST:EMBED

Offsets are fetched at runtime from offsets.imtheo.lol and cached per Roblox build.
