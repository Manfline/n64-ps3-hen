#!/bin/sh
# Compila la ROM 3D de prueba (tests/roms/src/gfxtest) igual que vitest, en
# sus tres variantes: las mismas escenas con los comandos de F3DEX2
# (salsa64-gfxtest.z64), de F3D (salsa64-gfxtest-f3d.z64) y de F3DEX 1.x
# (salsa64-gfxtest-f3dex.z64).
#   tools/build-gfxtest.sh [args extra para docker build]
set -eu
B="$(dirname "$0")/build-vitest.sh"
ROM=gfxtest TITLE="SALSA64 GFXTEST" "$B" "$@"
ROM=gfxtest GBI=F3D NAME=salsa64-gfxtest-f3d TITLE="SALSA64 GFXTEST F3D" "$B" "$@"
ROM=gfxtest GBI=F3DEX NAME=salsa64-gfxtest-f3dex TITLE="SALSA64 GFXTEST F3DX" "$B" "$@"
