# Ridge Racer (PSP) – PC port

A native PC port of Ridge Racer (PSP), made by statically recompiling the game's own code. You build it on your own computer from **your own, legally obtained copy** of the game.

> **Beta.** Windows is the main target. Linux builds and boots; macOS is untested. Not affiliated with or endorsed by the game's publisher.

**This repository contains no game files, no keys, no decryption code and no generated game code.** You need your own copy of the game; the setup tool turns it into a PC program on your machine.

## Features
- Renders at a multiple of the PSP's 480x272 (OpenGL 3.3), with anti-aliasing, anisotropic filtering and sharpening options
- Frame-rate options above the PSP's 60 (tested up to 144 fps)
- **Online Wireless Battle**: host or join races over the internet with a server browser (search, filters, room codes, passwords), up to 8 players
- Xbox, PlayStation, Switch and most other controllers (SDL2), with matching button pictures in the game; triggers for accelerate/brake
- Full sound and music (built-in ATRAC3+ decoder)
- Built-in bug reports: Ctrl+F12 in game (or automatically after a crash) saves one zip to send
- On-screen name entry; save games kept in a `savedata` folder next to the game

## Install (Windows 10/11)
1. Download **Setup.exe** from the [Releases](../../releases) page and run it. (Windows may show an "unknown publisher" warning because the file is not code-signed; choose *More info → Run anyway*.)
2. Pick your game: a **decrypted** disc image (`.iso`) or the folder that contains `PSP_GAME`.
3. Pick where to install it (any drive). Setup builds the game in the background; this can take 20 minutes or more on the first install and needs no other downloads.
4. Press *Finish*. A settings window opens on the first start.

### Your game copy must be decrypted
The port reads the game's executable (`PSP_GAME/SYSDIR/BOOT.BIN`). It must be the decrypted form (it starts with `ELF`); an encrypted one (starts with `~PSP`) is rejected with a clear message. Dumping and decrypting your own UMD can be done with existing community tools; the PPSSPP documentation is a good place to start. No decryption tools or keys are provided here.

## Linux and macOS (experimental)
Needs a C++20 compiler, CMake, Ninja and SDL2 (`sudo apt install build-essential cmake ninja-build libsdl2-dev`, or `brew install cmake ninja sdl2` on macOS).

    ./build-unix.sh /path/to/RidgeRacer.iso
    ./RidgeRacer-Unix/RidgeRacerNative

There is no settings window on these platforms (edit `ridgeracer.ini`).

## Controls
| Action | Keyboard | Controller |
|---|---|---|
| Steer | Arrow keys | D-pad / left stick |
| Accelerate / Brake | Z / X | bottom / right face button, or right / left trigger |
| Triangle / Square | S / A | top / left face button |
| L / R | Q / E | shoulder buttons |
| Start / Select | Enter / Right Shift or Backspace | Start / Back (Options / Share, + / -) |

Buttons are mapped by position like the PSP on every controller. Esc quits, F11 or Alt+Enter toggles full screen, F12 saves a screenshot, Ctrl+F12 makes a bug report, F8 mutes, F9/F10 change volume.

## Reporting bugs
The game makes a bug report for you: `RidgeRacer-bug-report-<date>.zip` on your Desktop. You get one automatically after a crash; press **Ctrl+F12** in game when something looks wrong; or click **Bug report...** in the settings window (hold Shift while starting the game) after a freeze. Attach it to an issue with a few words about what happened. It contains the game's logs, settings (room password removed), crash files and a short PC summary - no game files.

## Known limits
Car LOD pop-in, some intro camera shots and Time Attack on some tracks are not fully checked. Online play has been tested with up to 3 players.

## Building from source
- Windows: `build.ps1 -Game <iso or BOOT.BIN>` (uses the zig/clang compiler in `toolchain\zig`). `make-release.cmd` builds `Setup.exe`.
- Browser build (local use only, never host it: it contains code translated from your game): `build-web.ps1`, then `web-dist\serve.cmd`.
- Online room service (server list and relay): `online/worker` (Cloudflare Worker).
- Linux/macOS: `build-unix.sh` as above.

## Credits and licences
Built on the PSPRecomp static-recompilation framework (`src/`, `include/`, `tools/`). Project code: MIT (see `LICENSE`). The ATRAC3+ decoder in `profiles/ridgeracer/third_party/atrac3plus` and the H.264 movie decoder in `profiles/ridgeracer/third_party/ffmpeg-h264` are derived from FFmpeg and are LGPL 2.1 (see `COPYING.LGPLv2.1` in each). `profiles/ridgeracer/third_party/sdl2/SDL2.dll` is SDL 2.30.9 (zlib licence, see the README there). Ridge Racer is a trademark of its owner; no game assets are distributed here.
