# Ridge Racer PSP – PC port (beta)

A native Windows port of Ridge Racer (PSP), made by statically recompiling the game's own code.
**No game files, keys or generated code are included.** You build the port on your own PC from your own legally obtained copy of the game; the installer does this for you. Everything else it needs (the compiler, the ATRAC3+ music decoder based on FFmpeg's LGPL code, SDL2 for controllers) comes with it.

This port is not affiliated with or endorsed by Bandai Namco.

## What you need
- Windows 10 or 11 (64-bit), a GPU with OpenGL 3.3 or newer
- Your own **decrypted** disc image of the game (`.iso`), or its `PSP_GAME` folder
- A few GB of free disk space while installing; the first build can take 20 minutes or more

## Install
Run `Setup.exe`, pick your disc image and an install folder, and wait for the build to finish. Windows may warn that the installer is from an unknown publisher: choose *More info* → *Run anyway*.
Start the game from the shortcut. All of the PC version's settings (resolution, frame rate, anti-aliasing, draw distance, sound, controls, online, updates) are in the game under **Options > RECOMP OPTIONS**. The older settings window still opens if you hold **Shift** while starting the game.

## Controls
- **Controllers:** Xbox, PlayStation 3/4/5, Switch Pro / Joy-Con and most other pads work. Buttons are mapped by position like the PSP (bottom = accelerate in the default layout). The right trigger accelerates and the left trigger brakes (can be turned off in Recomp Options > Controls). The button pictures in the game follow the controller or keyboard you used last; you can fix them in Recomp Options > Controls.
- **Keyboard:** arrows steer, Z accelerate, X brake/back, A / S other buttons, Q / E shift, Enter start, Right Shift select.
- **Keys:** Esc quits. F11 or Alt+Enter: full screen. F12: screenshot (`screenshots\`). Ctrl+F12: bug report. F8 mute, F9 / F10 volume.

## Updates
Options > Recomp Options > Updates & Help checks for a new version and installs it, then restarts the game. The game also checks when you go online.

## Online play
Wireless Battle works over the internet: choose it in the game, then host a race or pick one from the server browser (or type a friend's room code with *Direct connect*). Up to 8 players.

## Reporting bugs
The game makes a bug report for you: a zip file on your Desktop named `RidgeRacer-bug-report-<date>.zip`. Send that file, together with a few words about what you were doing (track, mode, online or not, what went wrong).

- **The game crashed or stopped with an error:** a message tells you a report was saved on your Desktop.
- **Something looks or sounds wrong while playing:** press **Ctrl+F12** at that moment. The report includes a picture of the screen.
- **Anything else:** Options > Recomp Options > Updates & Help > Make a bug report.
- **The game froze, or will not start:** open the settings window (hold Shift while starting the game) and click **Bug report...**. It includes the log of the last session.

A report holds the game's logs (this session and the one before), your settings (your room password is removed), the performance report, crash files (if any) and a short summary of your PC (Windows version, CPU, graphics card, controllers). It holds no game files and nothing else from your PC.

## Known limits
- The browser version cannot be offered as a download or website (it would contain the game's code).
- **Known bug, still being fixed:** cars far away can still be drawn with their simple (low-poly) model, even with the draw distance at its maximum. The draw distance setting only extends scenery; the game switches car models by a separate rule.
- Unlocked frame rate, intro camera shots other than the default, and Time Attack on some tracks are not fully checked. Frame rates above your monitor's refresh rate (for example 240 on a 144 Hz monitor) can look uneven; use a rate at or below your monitor's.
