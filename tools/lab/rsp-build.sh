#!/bin/sh
# Compila rsp-lab contra un árbol con el traductor del RSP (src/rsp/jit).
#   tools/lab/rsp-build.sh [SRC [OUT]]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="${1:-$ROOT}"
OUT="${2:-$ROOT/build/lab/bin}"
mkdir -p "$OUT"
cc -O1 -std=gnu11 -Wall -I"$SRC/src" "$ROOT/tools/lab/rsp-lab.c" "$SRC"/src/rsp/jit/jit.c "$SRC"/src/rsp/jit/jit_stubs.c \
	"$SRC"/src/rsp/jit/jit_vu.c "$SRC"/src/rsp/lle/cpu.c "$SRC"/src/rsp/lle/vu.c "$SRC"/src/rsp/lle/vmem.c -o "$OUT/rsp-lab"
