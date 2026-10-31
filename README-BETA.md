# Ridge Racer PSP – PC port (beta)

A native Windows port of Ridge Racer (PSP), made by statically recompiling the game's own code.
**No game files, keys or generated code are included.** You build the port on your own PC from your own legally obtained copy of the game; the installer does this for you. Everything else it needs (the compiler, the ATRAC3+ music decoder based on FFmpeg's LGPL code, SDL2 for controllers) comes with it.

This port is not affiliated with or endorsed by Bandai Namco.

AI disclosure: all of this project's code was written by AI (Claude, by Anthropic); the project's owner directed and tested it. It is a beta, so please send bug reports (see below).

## What you need
- Windows 10 or 11 (64-bit), a GPU with OpenGL 3.3 or newer
- Your own **decrypted** disc image of the game (`.iso`), or its `PSP_GAME` folder
- A few GB of free disk space while installing; the first build can take 20 minutes or more

## Install
Run `Setup.exe`, pick your disc image and an install folder, and wait for the build to finish. Windows may warn that the installer is from an unknown publisher: choose *More info* → *Run anyway*.
Start the game from the shortcut. A settings window opens on the first start (hold **Shift** while starting to open it again later).

## Controls
- **Controllers:** Xbox, PlayStation 3/4/5, Switch Pro / Joy-Con and most other pads work. Buttons are mapped by position like the PSP (bottom = accelerate in the default layout). The right trigger accelerates and the left trigger brakes (can be turned off in the settings window). The button pictures in the game follow the controller or keyboard you used last; you can fix them in the settings window.
- **Keyboard:** arrows steer, Z accelerate, X brake/back, A / S other buttons, Q / E shift, Enter start, Right Shift select.
- **Keys:** Esc quits. F11 or Alt+Enter: full screen. F12: screenshot (`screenshots\`). Ctrl+F12: bug report. F8 mute, F9 / F10 volume.

## Online play
Wireless Battle works over the internet: choose it in the game, then host a race or pick one from the server browser (or type a friend's room code with *Direct connect*). Up to 8 players.

## Reporting bugs
The game makes a bug report for you and asks where to save it: a zip file named `RidgeRacer-bug-report-<date>.zip`. Send that file, together with a few words about what you were doing (track, mode, online or not, what went wrong).

- **The game crashed or stopped with an error:** the game asks where to save a report, then offers to open the GitHub bug report page.
- **Something looks or sounds wrong while playing:** press **Ctrl+F12** at that moment. The report includes a picture of the screen.
- **The game froze, or will not start:** open the settings window (hold Shift while starting the game) and click **Bug report...**. It includes the log of the last session.

A report holds the game's logs (this session and the one before), your settings (your room password is removed), the performance report, crash files (if any) and a short summary of your PC (Windows version, CPU, graphics card, controllers). It holds no game files and nothing else from your PC.

## Known limits
- The browser version cannot be offered as a download or website (it would contain the game's code).
- Unlocked frame rate, car LOD pop-in, intro camera shots other than the default, and Time Attack on some tracks are not fully checked.

## Community
Join the Discord for news, updates and to find opponents to race against: https://discord.gg/XENZw6VXdd
