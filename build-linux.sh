#!/usr/bin/env bash
# Kept for convenience: Linux and macOS use the same script.
exec "$(cd "$(dirname "$0")" && pwd)/build-unix.sh" "$@"
