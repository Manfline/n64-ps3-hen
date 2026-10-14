#!/bin/sh
# Compara la HLE gráfica con la consola en un mismo cuadro: dibuja con la HLE
# y el rasterizador del host las tareas gráficas de un volcado de la RDRAM
# exportado por ares (Tools → Memory → Export) y las compara con el búfer de
# imagen que dibujó ares en ese volcado (docs/referencia-ares.md).
#
#   tools/ares-comparar.sh [opciones] volcado-rdram.bin...
#
# Las opciones son las de `salsa64-run --ares` (--help las lista): --task,
# --fb, --tol, --q5, --dmem, --out DIR, --no-png. Compila antes salsa64-run
# nativo, sin sanitizadores, en build/host-mac (SALSA64_HOST_OUT lo cambia).
# Los volcados y los PNG llevan datos del juego: fuera del repositorio
# (build/ares/, que git ignora).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${SALSA64_HOST_OUT:-$ROOT/build/host-mac}
[ $# -gt 0 ] || { sed -n 2,14p "$0"; exit 3; }
make -C "$ROOT/src/host" OUT="$OUT" CC="${CC:-cc}" SAN= -j8 >"$OUT.log" 2>&1 || {
	tail -20 "$OUT.log" >&2
	exit 3
}
exec "$OUT/salsa64-run" --ares "$@"
