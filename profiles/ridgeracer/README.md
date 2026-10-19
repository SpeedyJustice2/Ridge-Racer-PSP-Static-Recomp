# Ridge Racer (PSP, 2004) - native PC port (recompilation profile)

A native Windows build of Ridge Racer (PSP, US) made by static recompilation
with the PSPRecomp toolkit in this repository. The game's code is translated to
C++ ahead of time; this profile supplies the PSP "operating system" around it
(threads, file I/O, graphics, sound, video, controls) and renders at a higher
internal resolution than the PSP's 480x272.

No game files, keys, decrypted executables or generated code are included or
may be distributed. You must bring your own legally obtained copy of the game.

## What works

- Boots and runs the whole game: menus, races, minigames, the opening movie.
- Rendering: the PSP graphics engine is emulated; a GL 3.3 backend renders at an
  integer multiple of 480x272 (`--internal-scale 3` = 1440x816). A software
  backend (native resolution, no `--gpu`) is kept as a reference.
- Controls: keyboard and Xbox-style (XInput) gamepads, hot-pluggable.
- Sound: all sound effects and in-game music through the Windows audio system.
- Movies: real H.264 video decoding of the `.pmf` files (Windows Media
  Foundation) with their ATRAC3+ soundtrack, in sync.
- ATRAC3/ATRAC3+ music (movie soundtracks, the 8-bit Rally X minigames) is
  decoded with FFmpeg's libavcodec (see "FFmpeg" below).

- On-screen keyboard for name entry: type on your keyboard (Enter = OK, Esc = cancel)
  or use the letter grid with a gamepad.

Not done yet: Android, DXT textures and
mip-mapping (not used by anything seen so far), network/wireless features
(reported as unavailable so the game disables them).

## Layout assumed by these instructions

    Ridge Racer PC Recomp\
      PSPRecomp-MSAE\                 (the cloned toolkit; this folder goes in profiles\)
      RidgeRacerPSPFiles\
        PSP_GAME\SYSDIR\BOOT.BIN      (decrypted ELF)
        generated\                    (output of psp_recomp --auto)
      ffmpeg\                         (optional, see below)

## Build (Developer PowerShell)

    cd "...\Ridge Racer PC Recomp\PSPRecomp-MSAE"
    cmake -S . -B out/ridgeracer -DPSPRECOMP_PROFILE=ridgeracer `
      -DPSPRECOMP_RIDGERACER_GENERATED_DIR="C:/Users/Ian/source/repos/Ridge Racer PC Recomp/RidgeRacerPSPFiles/generated" `
      -DFFMPEG_ROOT="C:/ffmpeg" `
      -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
    cmake --build out/ridgeracer --config Release --target RidgeRacerNative

Compiling the 428 generated files is slow and memory-hungry. If the build runs
out of RAM, add `-DPSPRECOMP_RIDGERACER_MP_JOBS=2` to the first command.

To add FFmpeg to an existing build folder without redoing the rest:

    cmake out/ridgeracer -DFFMPEG_ROOT=C:\ffmpeg
    cmake --build out/ridgeracer --config Release --target RidgeRacerNative

## FFmpeg (needed only for ATRAC music)

The PSP stores music as ATRAC3/ATRAC3+, which Windows cannot decode. The build
can use a *shared* FFmpeg build (LGPL is fine, e.g. BtbN's
`ffmpeg-master-latest-win64-lgpl-shared.zip`). Extract it somewhere and point
`FFMPEG_ROOT` at the folder that directly contains `include`, `lib` and `bin`.
CMake prints "FFmpeg ATRAC decoding enabled" when it is found, and copies the
FFmpeg DLLs next to `RidgeRacerNative.exe`; keep them there when moving the exe.

Without FFmpeg the port still runs; the ATRAC music is simply silent.

Nothing else needs installing: video uses Windows' built-in H.264 decoder
(on "N" editions of Windows install the Media Feature Pack), audio uses the
standard Windows audio API, the gamepad uses XInput.

## Run

    .\out\ridgeracer\bin\Release\RidgeRacerNative.exe "..\RidgeRacerPSPFiles\PSP_GAME\SYSDIR\BOOT.BIN" --gpu --internal-scale 3

### Controls

| PSP | Keyboard | Gamepad |
|---|---|---|
| D-pad | arrow keys | D-pad |
| Cross / Circle | Z / X | A / B |
| Square / Triangle | A / S | X / Y |
| L / R | Q / E | LB / RB |
| Start / Select | Enter / Right Shift | Start / Back |
| Analog stick | I J K L | left stick |

On-screen keyboard (name entry): type normally, Backspace deletes, Enter accepts,
Esc cancels. With a gamepad: D-pad moves over the letter grid, A picks, B deletes,
Y adds a space, Start accepts, Back cancels. The game ignores other input while it is open.

`F11` (or `Alt+Enter`) toggles borderless full screen; `--fullscreen` starts that way.
`F12` saves a screenshot (480x272 plus a full-resolution `_hires` copy).

### Command line options

- `--gpu` use the GL backend (recommended); `--internal-scale N` sets its
  resolution multiplier (1 to 4 or more; 3 is a good default).
- Image quality (all on by default, each can be turned off or tuned):
  `--no-fxaa` turns off the anti-aliasing of the finished image; `--sharpen X`
  sets the texture-detail sharpening (0 = off, default 0.35, 1 = strong; hard
  edges are left to the anti-aliasing); `--no-mipmaps` turns off trilinear
  filtering of 3D textures and `--aniso N` (1 to 16, default 8) sets the
  anisotropic filtering that keeps roads sharp at a slant; `--ui-crisp X`
  (0 = off, default 0.8, 1 = crispest) keeps the pixels of menu text and icons
  solid when they are drawn large instead of blurring them. For the best edge
  quality, render above the window size and let the GPU shrink it, e.g.
  `--internal-scale 6 --scale 3` (supersampling).
- `--scale N` window size multiplier; `--no-window` headless; `--unthrottled`
  run faster than 59.94 fps.
- `--log FILE` also write everything to a file; `--quiet` / `--trace-all` change
  the amount of per-call tracing.
- `--screenshots DIR EVERY_N [MAX]` periodic PNG captures; `--stop-after-frame N`.
- `--replay-input LOG` replays the button input recorded in an earlier `--log`
  file, which makes runs repeatable for testing.
- A second positional path sets the disc root (the folder containing PSP_GAME).
- `--max-dispatches N` limits how long the guest may run.

### Debugging environment variables

All optional and off by default; set them in PowerShell with `$env:NAME = "1"`
and clear them with `Remove-Item Env:NAME` (they stay set for the whole window).

- `RR_VERBOSE` full per-call log (default hides repetitive lines).
- `RR_NO_AUDIO` disable sound output.
- `RR_AUDIO_LEVELS=1` logs the loudness of the game's audio output (`AUDIOLEVEL` lines, twice a second per channel, plus a `SASVOICES` line listing the sound voices the game is playing) for comparing against another emulator.
- `RR_PROF=1` adds a `HEARTBEAT PROF` line to the log (time per section: GE, batching, textures, GL calls) for finding frame-rate problems.
- `RR_AUDIO_MME=1` use the older waveOut backend instead of WASAPI (more latency; only needed if WASAPI misbehaves on your device).
- `RR_MOVIE_AUDIO_DELAY_MS=n` override the silence inserted before the movie audio so it lines up with the video (default is derived from the device latency).
- `RR_AUDIO_DUMP=file.wav` record what the mixer plays (first 2 minutes), plus
  `file.wav.chN.raw` per channel.
- `RR_SHOT_RANGE="start,count;..."` with `--screenshots`, and `RR_SHOT_HIRES`,
  capture specific frames (optionally at full internal resolution).
- `RR_DRAW_LOG`, `RR_PIXEL_TRACE=frame,x,y`, `RR_NO_FOG`, `RR_NO_LIGHTING`,
  `RR_NO_STENCIL`, `RR_NO_MARGIN_CLIP`, `RR_SKIP`: graphics diagnostics.

When reporting a problem, run once with `--log somefile.txt` and send that file.

## How it is organised (`host/`)

- `rr_hle.cpp` PSP kernel layer: threads/scheduler, semaphores, file I/O against
  `disc0:/`, memory, savedata, timing, the `sceAudio` output API, VPL/FPL pools.
  Derived from the MotorStorm profile with its game-specific code removed.
- `rr_ge.cpp`, `rr_gpu.hpp`, `rr_gl.cpp`, `rr_raster.cpp` graphics engine
  emulation, the backend interface, the GL 3.3 renderer and the software
  reference renderer.
- `rr_present_win32.cpp` window, keyboard, XInput gamepad, frame pacing.
- `rr_audio.*` mixer and Windows output (WASAPI shared mode, event-driven, low latency; waveOut fallback). The mixer keeps a small adaptive buffer that grows after a dropout, so brief emulation stalls (e.g. movie loading) do not cause stutter.
- `rr_atrac.cpp` the `sceAtrac3plus` HLE; `rr_atrac_decoder.*` the FFmpeg wrapper.
- `rr_mpeg.cpp`, `rr_pmf.*`, `rr_video*.cpp` the `sceMpeg` HLE, the PMF
  (MPEG-2 program stream) demuxer, and the platform H.264 decoder
  (`rr_video_win32.cpp`; `rr_video_null.cpp` on other platforms).
- `rr_utility.*`, `rr_osk_ui.*` the on-screen keyboard / message dialog HLE and the keyboard overlay
  (its 8x16 font is derived from GNU Unifont, SIL OFL 1.1).
- `rr_log.*` logging with the noise filter.

## Notes on game quirks the port reproduces

- The game keeps palettes in an invisible strip of the frame buffer; drawing is
  clipped to the visible 480 columns so the strip is never overwritten.
- The "clipping" of the bridge on one course is the original game's behaviour
  (it looks the same in PPSSPP).
- The game recycles ten movie-audio buffers faster than it plays them; the
  port keeps each decoded frame so playback is not corrupted.
