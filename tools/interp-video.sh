#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Salsa64 — animaciones para comparar a simple vista los cuadros intermedios
# del prototipo de interpolación (salsa64-run --interp, docs/interpolacion.md).
#
#   tools/interp-video.sh DIR K PRIMERA ÚLTIMA [natural|decimate]
#
# DIR es la salida de --interp; PRIMERA y ÚLTIMA, tareas que son cuadros del
# juego (con decimate, múltiplos de K). Escribe en DIR:
#   video-60.png    APNG a 60 cuadros por segundo, paneles lado a lado
#   video-lento.png lo mismo a 10 cuadros por segundo
# natural:  original (cada cuadro repetido K campos) | geometría | imagen | geometría extrapolada
# decimate: repetir | verdad (lo que dibujó el juego) | geometría | imagen
# Necesita build/ppmtool (cc -O2 -o build/ppmtool tools/ppmtool.c -lz).
set -e
DIR=$1 K=$2 A=$3 B=$4 MODE=${5:-natural}
T=build/ppmtool
[ -x "$T" ] || { echo "falta $T" >&2; exit 3; }
TMP=$DIR/video-tmp
rm -rf "$TMP"
mkdir -p "$TMP"
n=0
N=$A
STEP=1
[ "$MODE" = decimate ] && STEP=$K
while [ "$N" -le "$B" ]; do
	j=0
	while [ "$j" -lt "$K" ]; do
		if [ "$MODE" = decimate ]; then
			P=$((N - K))
			if [ "$j" -eq 0 ]; then
				set -- "$DIR/o-$P.ppm" "$DIR/o-$P.ppm" "$DIR/o-$P.ppm" "$DIR/o-$P.ppm"
			else
				set -- "$DIR/o-$P.ppm" "$DIR/o-$((P + j)).ppm" "$DIR/gi-$N-$j.ppm" "$DIR/ii-$N-$j.ppm"
			fi
		else
			P=$((N - 1))
			if [ "$j" -eq 0 ]; then
				set -- "$DIR/o-$P.ppm" "$DIR/o-$P.ppm" "$DIR/o-$P.ppm" "$DIR/o-$P.ppm"
			else
				set -- "$DIR/o-$P.ppm" "$DIR/gi-$N-$j.ppm" "$DIR/ii-$N-$j.ppm" "$DIR/ge-$P-$j.ppm"
			fi
		fi
		ok=1
		for f in "$@"; do [ -f "$f" ] || ok=0; done
		if [ "$ok" = 1 ]; then
			"$T" strip "$TMP/$(printf %05d $n).ppm" 4 1 "$@"
			n=$((n + 1))
		fi
		j=$((j + 1))
	done
	N=$((N + STEP))
done
[ "$n" -gt 0 ] || { echo "sin cuadros (¿tareas fuera de rango?)" >&2; exit 2; }
"$T" apng "$DIR/video-60.png" 17 1 "$TMP"/*.ppm
"$T" apng "$DIR/video-lento.png" 100 1 "$TMP"/*.ppm
rm -rf "$TMP"
echo "$n cuadros: $DIR/video-60.png, $DIR/video-lento.png"
