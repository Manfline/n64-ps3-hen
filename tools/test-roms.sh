#!/bin/sh
# Pruebas con ROMs homebrew en el host: compila salsa64-run (gcc nativo, ASan +
# UBSan) y corre nemu64-test hasta su resumen por ISViewer, vitest (ROM
# propia del VI, tests/roms/src/vitest) hasta que se verifica a sí misma, y
# gfxtest (ROM 3D propia, tests/roms/src/gfxtest) por la HLE gráfica, con su
# imagen verificada en el rasterizador de referencia (build/gfxtest.ppm).
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
# del último campo queda en build/vitest.ppm.
VLOG=build/logs/vitest.run.log
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
	-u 'vitest: OK' -f 'vitest: FALLO' --frame build/vitest.ppm \
	tests/roms/homebrew/salsa64-vitest.z64 2>&1" >"$VLOG" 2>&1
vrc=$?
printf '%-12s %s\n' vitest "$(grep 'vitest:' "$VLOG" | tail -1)"
[ $vrc -eq 0 ] || { echo "             falló (código $vrc, $VLOG)"; fail=1; }

GLOG=build/logs/gfxtest.run.log
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
	-u 'gfxtest: OK' -f 'gfxtest: FALLO' --gfx-frame build/gfxtest.ppm --gfxtest-check \
	tests/roms/homebrew/salsa64-gfxtest.z64 2>&1" >"$GLOG" 2>&1
grc=$?
printf '%-12s %s\n' gfxtest "$(grep 'gfxtest: [OF]' "$GLOG" | head -1)"
grep -E 'gfx:|gfxtest: imagen|gfxtest: \(' "$GLOG" | sed 's/^salsa64-run: /             /'
[ $grc -eq 0 ] || { echo "             falló (código $grc, $GLOG)"; fail=1; }
exit $fail
