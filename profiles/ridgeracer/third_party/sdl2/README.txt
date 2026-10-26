SDL2.dll - Simple DirectMedia Layer 2.30.9, Windows x64 (official release build, SDL2-2.30.9-win32-x64.zip from
https://github.com/libsdl-org/SDL/releases/tag/release-2.30.9, SHA-256 of the DLL:
58f0a2af6653445b48e58ace6890fbe237bf76452c9743e800358d6b935ded64).

The Windows build copies it next to RidgeRacerNative.exe.  The game loads it at run time (rr_pad.cpp) for controller
support beyond XInput: PlayStation 3/4/5, Switch Pro and Joy-Con, and DirectInput/HID pads.  Without it the game still
runs and reads Xbox-style (XInput) controllers.  To use a newer SDL2, replace the DLL (any 2.0.12 or later works).

Optional: put the community controller list gamecontrollerdb.txt
(https://github.com/mdqinc/SDL_GameControllerDB) next to the game for unusual pads.

License (zlib):

  Simple DirectMedia Layer
  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
