#!/bin/sh
# Compila la ROM de prueba del audio (tests/roms/src/audiotest) igual que vitest.
ROM=audiotest TITLE="SALSA64 AUDIOTEST" exec "$(dirname "$0")/build-vitest.sh" "$@"
