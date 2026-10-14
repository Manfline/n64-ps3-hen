#!/bin/sh
# Compila rec-lab con el cc nativo (docs/lab-recompilador.md).
#   tools/lab/build.sh [SRC [OUT]]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-$ROOT}"
OUT="${2:-$ROOT/build/lab/bin}"
exec make -s -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" -f "$ROOT/tools/lab/Makefile" SRC="$SRC" OUT="$OUT"
