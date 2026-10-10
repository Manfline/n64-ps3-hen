#!/bin/sh
# Compila la ROM 3D de prueba (tests/roms/src/gfxtest) igual que vitest.
ROM=gfxtest TITLE="SALSA64 GFXTEST" exec "$(dirname "$0")/build-vitest.sh" "$@"
