#!/bin/sh
# Pruebas con ROMs homebrew en el host: compila salsa64-run (gcc nativo, ASan +
# UBSan) y corre nemu64-test hasta su resumen por ISViewer, vitest (ROM
# propia del VI, tests/roms/src/vitest) hasta que se verifica a sí misma, y
# savetest (ROM propia del guardado, tests/roms/src/savetest) dos veces por
# tipo de guardado.
#
# La lista de pruebas de nemu64-test que fallan se compara con la línea base
# tests/roms/nemu64-test.fallas (lo que falta emular: RSP, cachés, bus PI…).
# Una falla nueva es una regresión y hace fallar el script; una que deja de
# fallar se avisa para actualizar la línea base:
#   tools/test-roms.sh --actualizar
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

if [ "${1:-}" = "--actualizar" ]; then
	[ $rc -eq 0 ] || { echo "nemu64-test no terminó (código $rc, $LOG)" >&2; exit 1; }
	cp "$NOW" "$BASE"
	echo "línea base actualizada: $(wc -l <"$BASE" | tr -d ' ') pruebas con fallas"
	echo "$summary"
	exit 0
fi

printf '%-12s %s\n' nemu64-test "${summary:-sin resumen (código $rc, $LOG)}"
grep '^salsa64-run:' "$LOG" | tail -1 | sed 's/^/             /'
fail=0
[ $rc -eq 0 ] || fail=1
new=$(LC_ALL=C comm -13 "$BASE" "$NOW")
gone=$(LC_ALL=C comm -23 "$BASE" "$NOW")
if [ -n "$new" ]; then
	echo "REGRESIÓN: pruebas que antes pasaban y ahora fallan:"
	echo "$new" | sed 's/^/  /'
	fail=1
fi
if [ -n "$gone" ]; then
	echo "Mejoras: estas pruebas ya no fallan (tools/test-roms.sh --actualizar):"
	echo "$gone" | sed 's/^/  /'
fi

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

# savetest: guardado del cartucho (EEPROM de 4 y 16 Kbit, SRAM de 256 Kbit;
# tests/roms/src/savetest). Dos arranques por tipo con el mismo directorio
# de guardado, vacío al empezar: el primero escribe y relee; el segundo
# además comprueba que persistió lo del primero.
SDIR=build/saves-test
rm -rf "$SDIR"
for t in eeprom4k eeprom16k sram; do
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
exit $fail
