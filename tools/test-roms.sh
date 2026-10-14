#!/bin/sh
# Pruebas con ROMs homebrew en el host: compila salsa64-run (gcc nativo, ASan +
# UBSan) y corre nemu64-test hasta su resumen por ISViewer, vitest (ROM
# propia del VI, tests/roms/src/vitest) hasta que se verifica a sí misma, y
# savetest (ROM propia del guardado, tests/roms/src/savetest) dos veces por
# tipo de guardado.
# gfxtest (ROM 3D propia, tests/roms/src/gfxtest) por la HLE gráfica, con su
# imagen verificada en el rasterizador de referencia (build/gfxtest.ppm), en
# sus variantes F3DEX2, F3D y F3DEX (build/gfxtest-f3d.ppm, …).
# audiotest (ROM propia del AI y de la HLE de audio, tests/roms/src/audiotest):
# se verifica sola y lo que sale por el AI se compara muestra por muestra con
# su tono (build/audiotest.wav).
# Estados guardados con gfxtest, vitest y audiotest: guardar en un campo,
# cargar y seguir da el mismo estado y la misma imagen que de corrido.
#
# La lista de pruebas de nemu64-test que fallan se compara con la línea base
# tests/roms/nemu64-test.fallas (lo que falta emular: RSP sin LLE, cachés,
# bus PI…). Una falla nueva es una regresión y hace fallar el script; una que
# deja de fallar se avisa para actualizar la línea base:
#   tools/test-roms.sh --actualizar
# nemu64-test corre dos veces: con la HLE (línea base de arriba) y con el
# RSP a bajo nivel (--lle; línea base tests/roms/nemu64-test-lle.fallas, sin
# las pruebas del RSP).
set -u
cd "$(dirname "$0")/.."
ROM=tests/roms/homebrew/nemu64-test.z64
BASE=tests/roms/nemu64-test.fallas
LOG=build/logs/nemu64-test.run.log
NOW=build/logs/nemu64-test.fallas
mkdir -p build/logs

if [ ! -f "$ROM" ]; then
	echo "falta $ROM: correr tools/build-nemu64-test.sh" >&2
	exit 1
fi
if ! tools/build.sh host >build/logs/build-host.log 2>&1; then
	echo "falló la compilación de salsa64-run (build/logs/build-host.log)" >&2
	exit 1
fi

# ~190 M ciclos en la versión actual; el límite deja margen de sobra.
# stderr se une a stdout dentro del contenedor: así el resumen queda al final
# (Docker no conserva el orden entre los dos flujos).
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 4000000000 \
	-u 'success rate)' -f 'Exception storm' -f 'panicked at' $ROM 2>&1" >"$LOG" 2>&1
rc=$?
summary=$(grep 'success rate)' "$LOG" | tail -1)
grep "^Test '" "$LOG" | sed "s/^Test '\([^']*\)'.*/\1/" | LC_ALL=C sort -u >"$NOW"

# Lo mismo con el RSP a bajo nivel.
LBASE=tests/roms/nemu64-test-lle.fallas
LLOG=build/logs/nemu64-test-lle.run.log
LNOW=build/logs/nemu64-test-lle.fallas
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 4000000000 --lle \
	-u 'success rate)' -f 'Exception storm' -f 'panicked at' $ROM 2>&1" >"$LLOG" 2>&1
lrc=$?
lsummary=$(grep 'success rate)' "$LLOG" | tail -1)
grep "^Test '" "$LLOG" | sed "s/^Test '\([^']*\)'.*/\1/" | LC_ALL=C sort -u >"$LNOW"

if [ "${1:-}" = "--actualizar" ]; then
	[ $rc -eq 0 ] || { echo "nemu64-test no terminó (código $rc, $LOG)" >&2; exit 1; }
	[ $lrc -eq 0 ] || { echo "nemu64-test --lle no terminó (código $lrc, $LLOG)" >&2; exit 1; }
	cp "$NOW" "$BASE"
	cp "$LNOW" "$LBASE"
	echo "línea base actualizada: $(wc -l <"$BASE" | tr -d ' ') pruebas con fallas; con --lle $(wc -l <"$LBASE" | tr -d ' ')"
	echo "$summary"
	echo "$lsummary"
	exit 0
fi

fail=0
compara() {  # nombre resumen log rc base ahora
	printf '%-12s %s\n' "$1" "${2:-sin resumen (código $4, $3)}"
	grep '^salsa64-run: rsp lle' "$3" | tail -1 | sed 's/^/             /'
	[ $4 -eq 0 ] || fail=1
	new=$(LC_ALL=C comm -13 "$5" "$6")
	gone=$(LC_ALL=C comm -23 "$5" "$6")
	if [ -n "$new" ]; then
		echo "REGRESIÓN: pruebas que antes pasaban y ahora fallan:"
		echo "$new" | sed 's/^/  /'
		fail=1
	fi
	if [ -n "$gone" ]; then
		echo "Mejoras: estas pruebas ya no fallan (tools/test-roms.sh --actualizar):"
		echo "$gone" | sed 's/^/  /'
	fi
}
compara nemu64-test "$summary" "$LOG" $rc "$BASE" "$NOW"
compara "nemu64 lle" "$lsummary" "$LLOG" $lrc "$LBASE" "$LNOW"

# vitest: temporización e interrupción del VI vistas desde la ROM; la imagen
# del último campo queda en build/vitest.ppm. También identifica los
# controles por el SI (salsa64-run deja uno conectado en el puerto 1) y lee
# el control 1 en cada cuadro.
VLOG=build/logs/vitest.run.log
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
	-u 'vitest: OK' -f 'vitest: FALLO' --frame build/vitest.ppm \
	tests/roms/homebrew/salsa64-vitest.z64 2>&1" >"$VLOG" 2>&1
vrc=$?
printf '%-12s %s\n' vitest "$(grep 'vitest: OK\|vitest: FALLO' "$VLOG" | tail -1)"
grep '^salsa64-run:.*ciclos' "$VLOG" | tail -1 | sed 's/^/             /'
[ $vrc -eq 0 ] || { echo "             falló (código $vrc, $VLOG)"; fail=1; }
pads='vitest: controles: 1=0x0500/0x02 2=no 3=no 4=no'
pad1='vitest: control 1: botones 0x0000 x 0 y 0'
if grep -q "^$pads\$" "$VLOG" && grep -q "^$pad1\$" "$VLOG"; then
	printf '%-12s %s\n' controles "$pads; $pad1"
else
	printf '%-12s %s\n' controles "FALLO: se esperaba \"$pads\" y \"$pad1\" ($VLOG)"
	fail=1
fi

# savetest: guardado del cartucho (EEPROM de 4 y 16 Kbit, SRAM de 256 Kbit,
# FlashRAM de 1 Mbit; tests/roms/src/savetest). Dos arranques por tipo con
# el mismo directorio de guardado, vacío al empezar: el primero escribe y
# relee; el segundo además comprueba que persistió lo del primero.
SDIR=build/saves-test
rm -rf "$SDIR"
for t in eeprom4k eeprom16k sram flashram; do
	for n in 1 2; do
		SLOG=build/logs/savetest-$t-$n.run.log
		tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
			-u 'savetest: OK' -f 'savetest: FALLO' --save-dir $SDIR \
			tests/roms/homebrew/salsa64-savetest-$t.z64 2>&1" >"$SLOG" 2>&1
		src=$?
		line=$(grep 'savetest: ' "$SLOG" | tail -1)
		printf '%-12s %s\n' savetest "${line:-sin resultado (código $src, $SLOG)}"
		want="arranque $n ("
		[ $n -eq 2 ] && want="arranque 2 (persistió el arranque 1;"
		if [ $src -ne 0 ] || ! echo "$line" | grep -qF "$want"; then
			echo "             falló: se esperaba \"$want\" (código $src, $SLOG)"
			fail=1
		fi
	done
done

# gfxtest en sus tres variantes (las mismas escenas con los comandos de
# F3DEX2, F3D y F3DEX 1.x): misma imagen y ningún comando sin implementar.
for v in gfxtest gfxtest-f3d gfxtest-f3dex; do
	GLOG=build/logs/$v.run.log
	tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
		-u 'gfxtest: OK' -f 'gfxtest: FALLO' --gfx-frame build/$v.ppm --gfxtest-check \
		tests/roms/homebrew/salsa64-$v.z64 2>&1" >"$GLOG" 2>&1
	grc=$?
	printf '%-12s %s\n' $v "$(grep 'gfxtest: [OF]' "$GLOG" | head -1)"
	grep -E 'gfx:|gfxtest: imagen|gfxtest: \(' "$GLOG" | sed 's/^salsa64-run: /             /'
	[ $grc -eq 0 ] || { echo "             falló (código $grc, $GLOG)"; fail=1; }
	if ! grep -q 'gfx:.* 0 comandos sin implementar' "$GLOG"; then
		echo "             falló: hay comandos sin implementar ($GLOG)"
		fail=1
	fi
	# Los cuadros N y O (textura × textura, sombreado × sombreado) van por el
	# generador de fragment programs: ningún combiner se aproxima.
	if ! grep -q 'gfx:.* 0 combiners aproximados' "$GLOG"; then
		echo "             falló: hay combiners aproximados ($GLOG)"
		fail=1
	fi
	# Cada lista lleva un tramo de otro microcódigo (saltado) y la del cuadro
	# GFXTEST_CUT_FRAME termina dentro de él: una sola tarea cortada (F3D no
	# tiene G_LOAD_UCODE: sin tramo).
	if [ $v != gfxtest-f3d ] && ! grep -q 'gfx:.* [1-9][0-9]* de otro microcódigo saltados.* 1 tareas cortadas' "$GLOG"; then
		echo "             FALLO: se esperaba un tramo saltado por lista y 1 tarea cortada ($GLOG)"
		fail=1
	fi
done

# Estados guardados (core/state.c): de corrido hasta el campo M contra
# guardar en N, cargar en otra corrida y seguir hasta M. Los dos estados en M
# tienen que ser iguales byte por byte (CPU, RDRAM, RCP, planificador, HLE)
# y las imágenes del VI y de la HLE, también.
estados() {  # nombre rom N M [gfx: también la imagen de la HLE gráfica] [zip: el estado en N con la RDRAM comprimida]
	d=build/states/$1
	ELOG=build/logs/estados-$1.run.log
	rm -rf "$d"
	mkdir -p "$d"
	R="ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -q"
	g1="" gn="" g2="" zf=""
	[ "${6:-}" = zip ] && zf="--state-zip"
	if [ "${5:-}" = gfx ]; then
		# El rasterizador en las tres corridas: con él la HLE copia destinos
		# a la RDRAM (efectos de framebuffer) y eso es parte del estado.
		g1="--gfx-frame $d/m-gfx.ppm" gn="--gfx-frame $d/n-gfx.ppm" g2="--gfx-frame $d/m2-gfx.ppm"
	else
		: >"$d/m-gfx.ppm"; : >"$d/m2-gfx.ppm"
	fi
	tools/build.sh --sh "$R --fields $4 --state-save $4:$d/m.st --frame $d/m.ppm $g1 $2 && \
		$R --fields $3 $zf --state-save $3:$d/n.st $gn $2 && \
		$R --state-load $d/n.st --fields $4 --state-save $4:$d/m2.st --frame $d/m2.ppm $g2 $2" \
		>"$ELOG" 2>&1
	erc=$?
	if [ $erc -ne 0 ]; then
		printf '%-12s %s\n' estados "$1: FALLO (código $erc, $ELOG)"
		fail=1
	elif ! cmp -s "$d/m.st" "$d/m2.st"; then
		printf '%-12s %s\n' estados "$1: FALLO, los estados en el campo $4 difieren ($(cmp "$d/m.st" "$d/m2.st" | head -1))"
		fail=1
	elif ! cmp -s "$d/m.ppm" "$d/m2.ppm" || ! cmp -s "$d/m-gfx.ppm" "$d/m2-gfx.ppm"; then
		printf '%-12s %s\n' estados "$1: FALLO, las imágenes en el campo $4 difieren ($d)"
		fail=1
	else
		printf '%-12s %s\n' estados "$1: guardado en el campo $3 y cargado = de corrido hasta el $4 ($(wc -c <"$d/m.st" | tr -d ' ') bytes)"
	fi
}
estados gfxtest tests/roms/homebrew/salsa64-gfxtest.z64 150 300 gfx
estados vitest tests/roms/homebrew/salsa64-vitest.z64 120 240
estados audiotest tests/roms/homebrew/salsa64-audiotest.z64 100 200
# Lo mismo con el estado de N guardado comprimido (formato 2, como la PS3):
# cargarlo deja la máquina idéntica y el estado sin comprimir en M sale igual.
estados gfxtest-zip tests/roms/homebrew/salsa64-gfxtest.z64 150 300 gfx zip
estados vitest-zip tests/roms/homebrew/salsa64-vitest.z64 120 240 - zip

ALOG=build/logs/audiotest.run.log
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 600000000 \
	-u 'audiotest: HLE OK' -f 'audiotest: FALLO' --audio-wav build/audiotest.wav --audiotest-check \
	tests/roms/homebrew/salsa64-audiotest.z64 2>&1" >"$ALOG" 2>&1
arc=$?
printf '%-12s %s\n' audiotest "$(grep '^audiotest: ' "$ALOG" | head -1)"
grep -E '^audiotest: HLE|audiotest: audio|salsa64-run: audio:' "$ALOG" | sed 's/^salsa64-run: /             /; s/^audiotest: HLE/             audiotest: HLE/'
[ $arc -eq 0 ] || { echo "             falló (código $arc, $ALOG)"; fail=1; }
exit $fail
