# ahk-fix

`meow.exe`: standalone C++ port of `soru_autox.ahk` (Soru Auto X/Z/C for Blox Fruits), fixed to connect
to Roblox 0.744. It watches your own Soru (position jump + a "Soru" / "DodgeQuick" sound, gated by a recent
R press) and then sends the current mode's key (X, Z or C). The mode key (Caps Lock by default) cycles
OFF -> X -> Z -> C -> V -> OFF. Hover the tray icon for the status; it should read `Connected (version-...)`.

## Fix: Roblox 0.744 connection

Roblox 0.744 removed the 8-byte header in front of an Instance's name string, so every name read came
back empty. The tool could not find `Players` / `Workspace` (tray showed
`Waiting: couldn't find Players/Workspace ... [?, ?, ...]`), and the child-stride detection, which counts
named children, never picked 0x10 either. `ReadName` now reads the string at `namePtr+0` and falls back
to `namePtr+8`, so it works on both layouts.

## Source

The C++ source (`SoruAutoX.cpp`) is no longer in the current tree. It is in the git history: commit
`b89fe2a` has it, e.g. `git show b89fe2a:SoruAutoX.cpp > SoruAutoX.cpp`. Build notes are in that
file's header (MSVC + nlohmann `json.hpp`, `meow.rc` and `meow.ico` from this repo).

Offsets are fetched at runtime from offsets.imtheo.lol and cached per Roblox build.