# ahk-fix

`meow.exe`: standalone C++ port of `soru_autox.ahk` (Soru Auto X/Z/C for Blox Fruits), fixed to connect
to Roblox 0.744. It watches your own Soru and then sends the current mode's key (X, Z or C). The mode key
(Caps Lock by default) cycles OFF -> X -> Z -> C -> V -> OFF. Hover the tray icon for the status; it should
read `Connected (version-..., LastSoru)`. You need to be inside a Blox Fruits server for it to connect.

## How it detects a Soru

From v1.2.0 it reads the `LastSoruTick` / `LastSoruPosition` attributes the game puts on your character.
They are created on your first Soru after spawning and rewritten on every Soru after that, so the key goes
out the moment they change. That does not depend on the R key, the Soru sound or how far you moved, so
short hops count and it works if Soru isn't on R. It finds where those attributes are stored by itself each
time it connects, so a Roblox update shouldn't break it. If it can't read them it falls back to the older
detection (position jump + a "Soru" / "DodgeQuick" sound, gated by a recent R press), which also still
handles the V4 dodge. Switch it with tray menu -> **LastSoru detection** (on by default). While it is
active the status line says `LastSoru`.

## Download

Grab `meow.exe` from the [latest release](https://github.com/zurxxot/ahk-fix/releases/latest) and run it.
It lives in the system tray.

## Changing the mode key

Right-click the tray icon -> Settings -> Keybind tab -> click **Change key**, then press the key you want
(Esc cancels) and click Save. The key only changes after you click the button, so a stray keypress can't pick
one by accident. Caps Lock is the default and works as is.

## Auto update

From v1.1.0 it updates itself: shortly after start and every 6 hours it checks this repo's latest release, and
if there is a newer `meow.exe` it downloads it, checks the size and SHA-256, swaps itself in and restarts.
It never restarts while Roblox is the focused window. Turn it off in Settings -> General -> **Auto update**,
or check by hand from the tray menu -> **Check for updates**. Versions older than 1.1.0 can't update
themselves, so download the new exe once.

## Offsets

Fetched at runtime from https://offsets.femboythighs.org (`/roblox/version` and `/offsets.json`) and cached
per Roblox build.

## Fix: Roblox 0.744 connection

Roblox 0.744 removed the 8-byte header in front of an Instance's name string, so every name read came
back empty. The tool could not find `Players` / `Workspace` (tray showed
`Waiting: couldn't find Players/Workspace ... [?, ?, ...]`), and the child-stride detection, which counts
named children, never picked 0x10 either. `ReadName` now reads the string at `namePtr+0` and falls back
to `namePtr+8`, so it works on both layouts.

## Source

The C++ source (`SoruAutoX.cpp`) is not in the current tree. The 0.744 fix version is in the git history:
commit `b89fe2a`, e.g. `git show b89fe2a:SoruAutoX.cpp > SoruAutoX.cpp`. Build notes are in that file's
header (MSVC + nlohmann `json.hpp`, `meow.rc` and `meow.ico` from this repo).