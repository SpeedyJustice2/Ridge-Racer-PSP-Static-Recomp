#!/usr/bin/env bash
# Builds Ridge Racer PC for Linux and macOS from your own, decrypted copy of the game.
#   ./build-unix.sh /path/to/game.iso            (or the folder that contains PSP_GAME, or a BOOT.BIN)
# Needs: g++ or clang++ (C++20), cmake, make or ninja, and the SDL2 development package
#   Debian/Ubuntu: sudo apt install build-essential cmake ninja-build libsdl2-dev
#   Fedora:        sudo dnf install gcc-c++ cmake ninja-build SDL2-devel
#   Arch:          sudo pacman -S base-devel cmake ninja sdl2
#   macOS:         xcode-select --install   and   brew install cmake ninja sdl2
# Result: ./RidgeRacer-Unix/RidgeRacerNative  (run it from there; the first start asks nothing, settings are in ridgeracer.ini)
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
game="${1:-}"
[ -n "$game" ] || { echo "usage: $0 <game.iso | folder with PSP_GAME | BOOT.BIN>"; exit 1; }
game="$(cd "$(dirname "$game")" && pwd)/$(basename "$game")"   # absolute path (no realpath on old macOS)
build="$root/build-linux"
out="$root/RidgeRacer-Unix"
jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
gen="$build/generated"
mkdir -p "$build" "$out"

gen_args=(cmake -G "$(command -v ninja >/dev/null && echo Ninja || echo 'Unix Makefiles')")
step() { echo; echo "== $*"; }

# 1. Find BOOT.BIN
boot=""
if [ -d "$game" ]; then
  for rel in PSP_GAME/SYSDIR/BOOT.BIN SYSDIR/BOOT.BIN BOOT.BIN; do [ -f "$game/$rel" ] && { boot="$game/$rel"; break; }; done
  [ -n "$boot" ] || { echo "No PSP_GAME/SYSDIR/BOOT.BIN in $game"; exit 1; }
elif [[ "$(printf %s "$game" | tr A-Z a-z)" == *.iso ]]; then
  step "Reading BOOT.BIN from the disc image"
  c++ -std=c++20 -O1 -I"$root/profiles/ridgeracer/host" "$root/tools/rr_iso_tool.cpp" "$root/profiles/ridgeracer/host/rr_iso.cpp" -o "$build/rr_iso_tool"
  mkdir -p "$build/iso_boot"
  "$build/rr_iso_tool" "$game" --file PSP_GAME/SYSDIR/BOOT.BIN "$build/iso_boot/BOOT.BIN"
  boot="$build/iso_boot/BOOT.BIN"
else
  boot="$game"
fi
magic="$(head -c 4 "$boot" | od -An -c | tr -d ' ')"
case "$magic" in
  '~PSP'*) echo "This game is still encrypted. A decrypted copy of your own game is needed (see the README)."; exit 1;;
  '177ELF'*|*ELF*) ;;
  *) echo "BOOT.BIN is not a PSP program."; exit 1;;
esac

# 2. Code generator
step "Building the code generator"
"${gen_args[@]}" -S "$root" -B "$build/tools" -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_BUILD_TESTS=OFF >/dev/null
cmake --build "$build/tools" --target psp_recomp -j "$jobs"
recomp="$(find "$build/tools" -name psp_recomp -type f | head -n1)"

# 3. Generate the game's code from your BOOT.BIN
if [ ! -f "$gen/generated_registry.cpp" ]; then
  step "Reading your game's code (a few minutes)"
  "$recomp" "$boot" --auto "$gen" --jobs 0 --builtin-accessors --function-gates 0x08804000 4096
fi

# 4. The game
step "Building the game (this is the long part)"
"${gen_args[@]}" -S "$root" -B "$build/game" -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_PROFILE=ridgeracer \
  -DPSPRECOMP_RIDGERACER_GENERATED_DIR="$gen" -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
cmake --build "$build/game" --target RidgeRacerNative -j "$jobs"
exe="$(find "$build/game" -name RidgeRacerNative -type f | head -n1)"
cp -f "$exe" "$out/"

# 5. First-start settings: remember where the game is
[ -f "$out/ridgeracer.ini" ] || printf 'game_path=%s\n' "$game" > "$out/ridgeracer.ini"

echo
echo "== Done. Start it with:  $out/RidgeRacerNative"
echo "   Keys: arrows steer, Z accelerate (cross), X brake (circle), Enter start, F11 full screen, Esc quit."
