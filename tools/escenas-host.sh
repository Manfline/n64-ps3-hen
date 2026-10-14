#!/bin/sh
# Escenas del host con respaldos propios: las mismas imágenes con dos
# compilaciones de salsa64-run dicen si un cambio de la HLE las movió.
#   tools/escenas-host.sh [-b SALSA64_RUN] SALIDA     corre las escenas
#   tools/escenas-host.sh --comparar SALIDA_A SALIDA_B  qué imágenes cambian
#
# SALSA64_ROMS: carpeta con los respaldos (sm64.z64, banjo.z64, oot.z64,
# dk64.z64, perfectdark.z64, goldeneye.z64, mk64.z64, kirby64.z64); la que
# falte se omite. El ejecutor se compila con
#   make -C src/host OUT=$PWD/build/host-mac CC=cc SAN= -j8
# Sin partidas del jugador: la EEPROM empieza vacía y la partida de Zelda es
# nueva. Cada escena deja SALIDA/<nombre>.ppm (la imagen de la HLE gráfica) y
# SALIDA/<nombre>.log; las 8 primeras son las 7 de siempre (SM64 en el campo
# 700, el resto en el 900) y Kirby 64 (S2DEX nativo, 308 fondos).
#
# «casa» es la que faltaba: Zelda OoT con partida nueva hasta despertar en la
# casa de Link, una sala prerrenderizada (docs/pads/zelda-casa-nueva.pad). Su
# fondo sale de un G_LOAD_UCODE a S2DEX dentro de la tarea de F3DZEX; si la
# HLE no lo reconoce, la sala sale en colores planos y el log dice «0 fondos
# de S2DEX» (la escena de Zelda del campo 900 es el título y no lo usa).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ "${1:-}" = "--comparar" ]; then
	a="${2:?falta SALIDA_A}"
	b="${3:?falta SALIDA_B}"
	same=0
	diff=0
	for f in "$a"/*.ppm; do
		n="$(basename "$f" .ppm)"
		[ -f "$b/$n.ppm" ] || continue
		if cmp -s "$f" "$b/$n.ppm"; then
			same=$((same + 1))
			printf '%-8s idéntica\n' "$n"
		else
			diff=$((diff + 1))
			printf '%-8s DISTINTA\n' "$n"
		fi
	done
	echo "$same idénticas, $diff distintas"
	[ "$diff" -eq 0 ]
	exit
fi

BIN="$ROOT/build/host-mac/salsa64-run"
if [ "${1:-}" = "-b" ]; then
	BIN="${2:?falta SALSA64_RUN}"
	shift 2
fi
OUT="${1:?uso: tools/escenas-host.sh [-b SALSA64_RUN] SALIDA}"
ROMS="${SALSA64_ROMS:?SALSA64_ROMS: carpeta con los respaldos}"
[ -x "$BIN" ] || { echo "no existe $BIN (ver la cabecera de este guion)" >&2; exit 1; }
mkdir -p "$OUT"

# escena NOMBRE ROM CAMPOS [opciones de salsa64-run…]
escena() {
	nombre="$1"
	archivo="$2"
	rom="$ROMS/$2"
	campos="$3"
	shift 3
	if [ ! -f "$rom" ]; then
		echo "$nombre: sin $archivo, se omite"
		return
	fi
	"$BIN" -q --fields "$campos" --gfx-frame "$OUT/$nombre.ppm" "$@" "$rom" >"$OUT/$nombre.log" 2>&1 &
}

escena sm64 sm64.z64 700
escena banjo banjo.z64 900
escena zelda oot.z64 900
escena dk64 dk64.z64 900
escena pd perfectdark.z64 900
escena ge goldeneye.z64 900
escena mk64 mk64.z64 900
escena kirby kirby64.z64 900
rm -rf "$OUT/casa-save"
mkdir -p "$OUT/casa-save"
escena casa oot.z64 14500 --pad "$(cat "$ROOT/docs/pads/zelda-casa-nueva.pad")" --save-dir "$OUT/casa-save"
wait

status=0
for f in "$OUT"/*.log; do
	n="$(basename "$f" .log)"
	bg="$(sed -n 's/.* \([0-9]*\) fondos de S2DEX.*/\1/p' "$f" | head -1)"
	printf '%-8s %s fondos de S2DEX\n' "$n" "${bg:-?}"
	case "$n" in
	casa | kirby)
		if [ "${bg:-0}" -eq 0 ]; then
			echo "FALLO $n: ningún fondo de S2DEX (la HLE no reconoce el microcódigo)" >&2
			status=1
		fi
		;;
	esac
done
exit $status
