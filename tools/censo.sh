#!/bin/sh
# Censo de compatibilidad (docs/compatibilidad.md): corre cada ROM unos
# minutos de consola con salsa64-run (intérprete) y deja por juego el log
# completo y las imágenes del VI y de la HLE gráfica.
#
#   tools/censo.sh [-c ciclos] [-o dir] rom...
#
# Los ROMs son respaldos del usuario, fuera del repositorio: se leen desde su
# ruta y no se copian a ningún lado. El Docker de desarrollo (colima) solo
# comparte la carpeta de proyectos, así que salsa64-run se compila aquí con
# el compilador de la Mac (src/host/Makefile sin sanitizadores) en
# build/host-mac. Las imágenes son capturas de juegos comerciales: no se
# suben al repositorio.
#
# Por omisión 3 minutos de consola (1,69e10 ciclos) y salida en build/censo.
# Los juegos corren en paralelo, uno por proceso.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CYCLES=16875000000
OUT="$ROOT/build/censo"
while getopts c:o: opt; do
	case $opt in
	c) CYCLES=$OPTARG ;;
	o) OUT=$OPTARG ;;
	*) echo "uso: tools/censo.sh [-c ciclos] [-o dir] rom..." >&2; exit 3 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { echo "uso: tools/censo.sh [-c ciclos] [-o dir] rom..." >&2; exit 3; }

BIN="$ROOT/build/host-mac/salsa64-run"
mkdir -p "$OUT"
rm -f "$BIN"
make -s -C "$ROOT/src/host" OUT="$ROOT/build/host-mac" CC=cc SAN= -j8 2>&1 | grep -v "reducing alignment" || true
[ -x "$BIN" ] || { echo "no se pudo compilar salsa64-run" >&2; exit 1; }

for rom in "$@"; do
	name=$(basename "$rom" | sed 's/\.[^.]*$//; s/[^A-Za-z0-9]\{1,\}/-/g; s/-$//')
	"$BIN" -c "$CYCLES" -q -r --trace-rsp 64 --trace-exc 16 --trace-vi 2 \
		--frame "$OUT/$name-vi.ppm" --gfx-frame "$OUT/$name-gfx.ppm" "$rom" >"$OUT/$name.log" 2>&1 &
done
wait

for rom in "$@"; do
	name=$(basename "$rom" | sed 's/\.[^.]*$//; s/[^A-Za-z0-9]\{1,\}/-/g; s/-$//')
	echo "== $name"
	grep -E '^salsa64-run: |^MI intr|tareas RSP' "$OUT/$name.log" | sed 's/^salsa64-run: /  /'
done
