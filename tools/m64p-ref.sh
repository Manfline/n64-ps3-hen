#!/bin/sh
# Captura de referencia con mupen64plus (GLide64 + rsp-hle) llevando el juego a
# una escena con un guion de control como el --pad de salsa64-run.
#
#   tools/m64p-ref.sh ROM "GUION" DIR [CUADROS]
#
# Deja en DIR las capturas (--testshots CUADROS, por omisión una serie larga;
# OJO: esos cuadros no son los campos del VI de salsa64-run, van más lentos:
# pedir de sobra y mirar) y m64p.log. Variables:
#   M64P_SAVE=partida.sra  M64P_SAVE_DST="nombre.sra"
#       copia la partida al directorio de guardados de mupen64plus con los
#       bytes de cada palabra de 32 bits invertidos (su formato); el nombre
#       lo pone mupen64plus la primera vez que corre la ROM (ver su save/).
#   M64P_PAD_OFFSET=N      desplaza el guion N campos.
#   M64P_PAD_PORT=N        puerto (1 a 4) al que va el guion; los anteriores
#                          quedan conectados y quietos (como --pad-port).
#
# Requiere `brew install mupen64plus` (macOS). El teclado de su plugin de
# entrada no funciona con sdl2-compat; por eso el plugin propio
# (tools/m64p-pad/pad_plugin.c), que se compila aquí si falta.
set -e
ROM=$1; PAD=$2; OUT=$3
SHOTS=${4:-1500,2000,2500,3000,3300,3450,3550,3650,3750,3900,4100,4400,4800}
[ -n "$ROM" ] && [ -n "$PAD" ] && [ -n "$OUT" ] || { sed -n 2,20p "$0"; exit 3; }
HERE=$(cd "$(dirname "$0")" && pwd)
PLUGIN="$HERE/../build/m64p-pad/mupen64plus-input-pad.dylib"
if [ ! -f "$PLUGIN" ] || [ "$HERE/m64p-pad/pad_plugin.c" -nt "$PLUGIN" ]; then
	mkdir -p "$(dirname "$PLUGIN")"
	cc -O2 -shared -fPIC -o "$PLUGIN" "$HERE/m64p-pad/pad_plugin.c"
fi
if [ -n "$M64P_SAVE" ]; then
	[ -n "$M64P_SAVE_DST" ] || { echo "M64P_SAVE necesita M64P_SAVE_DST (nombre que usa mupen64plus)" >&2; exit 3; }
	SAVEDIR="$HOME/Library/Application Support/Mupen64Plus/save"
	mkdir -p "$SAVEDIR"
	python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); open(sys.argv[2],"wb").write(b"".join(d[i:i+4][::-1] for i in range(0,len(d),4)))' \
		"$M64P_SAVE" "$SAVEDIR/$M64P_SAVE_DST"
fi
mkdir -p "$OUT"
M64P_PAD="$PAD" exec mupen64plus --windowed --resolution 640x480 --gfx mupen64plus-video-glide64mk2 \
	--input "$PLUGIN" --sshotdir "$OUT" --testshots "$SHOTS" "$ROM" > "$OUT/m64p.log" 2>&1
