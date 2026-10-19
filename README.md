# Ridge Racer (PSP) – PC port

A native PC port of Ridge Racer (PSP), made by statically recompiling the game's own code. You build it on your own computer from **your own, legally obtained copy** of the game.

> **Beta.** Windows is the main target. Linux builds and boots; macOS is untested. Not affiliated with or endorsed by the game's publisher.

**This repository contains no game files, no keys, no decryption code and no generated game code.** You need your own copy of the game; the setup tool turns it into a PC program on your machine.

## Features
- Renders at a multiple of the PSP's 480x272 (OpenGL 3.3), with anti-aliasing and anisotropic filtering options
- Frame-rate options above the PSP's 60 (tested up to 144 fps)
- Keyboard and gamepad, hot-pluggable
- Full sound and music (built-in ATRAC3+ decoder, no extra DLLs)
- On-screen name entry; save games kept in a `savedata` folder next to the game

## Install (Windows 10/11)
1. Download **Setup.exe** from the [Releases](../../releases) page and run it. (Windows may show an "unknown publisher" warning because the file is not code-signed; choose *More info → Run anyway*.)
2. Pick your game: a **decrypted** disc image (`.iso`) or the folder that contains `PSP_GAME`.
3. Pick where to install it (any drive). Setup builds the game in the background; this takes roughly 5–15 minutes and needs no other downloads.
4. Press *Finish*. A settings window opens on the first start.

### Your game copy must be decrypted
The port reads the game's executable (`PSP_GAME/SYSDIR/BOOT.BIN`). It must be the decrypted form (it starts with `ELF`); an encrypted one (starts with `~PSP`) is rejected with a clear message. Dumping and decrypting your own UMD can be done with existing community tools; the PPSSPP documentation is a good place to start. No decryption tools or keys are provided here.

## Linux and macOS (experimental)
Needs a C++20 compiler, CMake, Ninja and SDL2 (`sudo apt install build-essential cmake ninja-build libsdl2-dev`, or `brew install cmake ninja sdl2` on macOS).

    ./build-unix.sh /path/to/RidgeRacer.iso
    ./RidgeRacer-Unix/RidgeRacerNative

Intro/ending movies are skipped on these platforms for now, and there is no settings window (edit `ridgeracer.ini`).

## Controls
| Action | Keyboard | Gamepad |
|---|---|---|
| Steer | Arrow keys | D-pad / left stick |
| Accelerate / Brake | Z / X | A / B |
| Triangle / Square | S / A | Y / X |
| L / R | Q / E | Shoulder buttons |
| Start / Select | Enter / Right Shift or Backspace | Start / Back |

Esc quits, F11 or Alt+Enter toggles full screen, F12 saves a screenshot, F1 cycles anti-aliasing, End cycles anisotropic filtering, F8 mutes, F9/F10 change volume.

## Reporting bugs
Run `RidgeRacerNative.exe --dev --log bug.txt` (`--dev` turns on diagnostics), and attach `bug.txt` with the track, mode and frame rate you used, plus a screenshot for visual problems.

## Known limits
Car LOD pop-in, pause-screen engine noise at 120/144 fps, some intro camera shots and Time Attack on some tracks are not fully checked.

## Building from source
- Windows: `build.ps1 -Game <iso or BOOT.BIN>` (uses the bundled zig/clang compiler in `toolchain\zig`), or `setup.ps1` with Visual Studio. `make-release.cmd` builds `Setup.exe`.
- Linux/macOS: `build-unix.sh` as above.

## Credits and licences
Built on the PSPRecomp static-recompilation framework (`src/`, `include/`, `tools/`). Project code: MIT (see `LICENSE`). The ATRAC3+ decoder in `profiles/ridgeracer/third_party/atrac3plus` is derived from FFmpeg and is LGPL 2.1 (see `COPYING.LGPLv2.1` there). Ridge Racer is a trademark of its owner; no game assets are distributed here.
