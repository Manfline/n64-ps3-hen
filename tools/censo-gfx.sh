#!/bin/sh
# Censo gráfico (docs/gfx-censo.md): corre salsa64-run --lle=all --lle-check-gfx
# sobre una escena de cada juego y deja, por juego, el informe (NOMBRE.log) y el
# detalle por tarea (NOMBRE.tsv) en la carpeta de salida.
#
#   tools/censo-gfx.sh [-o dir] [-j trabajos] [-t segundos] [-f campos_max] [juego...]
#
# SALSA64_ROMS: carpeta con los respaldos del usuario (fuera del repositorio),
# con los nombres de la tabla de abajo (sm64.z64, oot.z64…); el que falte se
# omite. salsa64-run se compila con el compilador de la Mac, sin sanitizadores
# (como tools/censo.sh). Sin juegos en la línea de comandos, corre todos.
#
# Cada escena es un guion de pulsaciones: uno de docs/pads/ si hay, o START y A
# alternados cada 60 campos desde el 200 (pasa títulos y menús). Con
# --lle-check-gfx el microcódigo corre cada tarea en el acto, antes de que el
# CPU avance, así la HLE (que lee la RDRAM al arrancar la tarea) y el RSP ven
# lo mismo (docs/gfx-censo.md, «Carrera con el CPU»).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/censo-gfx"
JOBS=6
SECS=300
MAXF=7000
while getopts o:j:t:f: opt; do
	case $opt in
	o) OUT=$OPTARG ;;
	j) JOBS=$OPTARG ;;
	t) SECS=$OPTARG ;;
	f) MAXF=$OPTARG ;;
	*) echo "uso: tools/censo-gfx.sh [-o dir] [-j trabajos] [-t segundos] [-f campos_max] [juego...]" >&2; exit 3 ;;
	esac
done
shift $((OPTIND - 1))
ROMS="${SALSA64_ROMS:?SALSA64_ROMS: carpeta con los respaldos}"

BIN="$ROOT/build/host-mac/salsa64-run"
mkdir -p "$OUT"
make -s -C "$ROOT/src/host" OUT="$ROOT/build/host-mac" CC=cc SAN= -j8 2>&1 | grep -v "reducing alignment" || true
[ -x "$BIN" ] || { echo "no se pudo compilar salsa64-run" >&2; exit 1; }

# nombre  rom  campos  guion (auto | nada | archivo de docs/pads sin .pad)
ESCENAS="
sm64          sm64.z64          4250  sm64-castillo
oot           oot.z64           3600  auto
mk64          mk64.z64          5800  mk64-luigi
sf64          sf64.z64          4000  auto
goldeneye     goldeneye.z64     7000  goldeneye-dam
perfectdark   perfectdark.z64   3600  auto
mm            mm.z64               3600  auto
dk64          dk64.z64          3000  auto
banjo         banjo.z64         3000  auto
banjotooie    banjotooie.z64    3000  auto
conker        conker.z64        3600  auto
kirby64       kirby64.z64       3000  auto
smash         smash.z64         3000  auto
pkstadium     pkstadium.z64     1800  stadium-vs
pkstadium2    pkstadium2.z64    3000  auto
pksnap        pksnap.z64        3000  auto
papermario    papermario.z64    3000  auto
dkr           dkr.z64           5600  dkr-pista
jfg           jfg.z64           2600  jfg-3d
mickey        mickey.z64        2800  mickey-semaforo
fzerox        fzerox.z64        3000  auto
waverace      waverace.z64      3000  auto
1080          1080.z64          3000  auto
yoshi         yoshi.z64         3000  auto
castlevania   castlevania.z64   3000  auto
ewj3d         ewj3d.z64         3000  auto
mace          mace.z64          3000  auto
mk4           mk4.z64           3000  auto
mkt           mkt.z64           3000  auto
mkmythologies mkmythologies.z64 3000  auto
fifa99        fifa99.z64        3000  auto
rogue         rogue.z64         3000  auto
re2           re2.z64           3000  auto
mariotennis   mariotennis.z64   3000  auto
marioparty    marioparty.z64    3000  auto
marioparty3   marioparty3.z64   3000  auto
doom64        doom64.z64        3000  auto
nflblitz      nflblitz.z64      3000  auto
nbashowtime   nbashowtime.z64   3000  auto
swracer       swracer.z64       3000  auto
killerinstinct killerinstinct.z64 4000 killerinstinct-pelea
hydrothunder  hydrothunder.z64  3000  auto
cruisnusa     cruisnusa.z64     3000  auto
rush2049      rush2049.z64      3000  auto
roadrash64    roadrash64.z64    3000  auto
indy          indy.z64          3000  auto
mspacman      mspacman.z64      3000  auto
"

auto_pad() {
	n=200
	s=""
	while [ "$n" -lt "$1" ]; do
		s="$s$n:START;$((n + 6)):-;$((n + 60)):A;$((n + 66)):-;"
		n=$((n + 120))
	done
	echo "$s"
}

escena() {
	nombre=$1 rom=$2 campos=$3 guion=$4
	[ "$campos" -gt "$MAXF" ] && campos=$MAXF
	if [ ! -f "$ROMS/$rom" ]; then
		echo "$nombre: sin $rom, se omite"
		return
	fi
	case $guion in
	auto) pad=$(auto_pad "$campos") ;;
	nada) pad="" ;;
	*) pad=$(cat "$ROOT/docs/pads/$guion.pad") ;;
	esac
	SALSA64_GCHK_SECONDS=$SECS SALSA64_GCHK_LOG="$OUT/$nombre.tsv" \
		"$BIN" -q --lle=all --lle-check-gfx --fields "$campos" ${pad:+--pad "$pad"} \
		--save-dir "$OUT/$nombre.saves" "$ROMS/$rom" >"$OUT/$nombre.log" 2>&1 || true
	echo "$nombre: $(grep 'lle-check-gfx: [0-9]* tareas' "$OUT/$nombre.log" | head -1 | sed 's/^salsa64-run: lle-check-gfx: //')"
}

lista="$*"
while read -r nombre rom campos guion; do
	[ -n "$nombre" ] || continue
	if [ -n "$lista" ]; then
		case " $lista " in *" $nombre "*) ;; *) continue ;; esac
	fi
	# Hasta $JOBS escenas a la vez.
	while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 1; done
	escena "$nombre" "$rom" "$campos" "$guion" &
done <<FIN
$ESCENAS
FIN
wait
