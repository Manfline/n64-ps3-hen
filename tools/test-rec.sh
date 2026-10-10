#!/bin/sh
# Pruebas del recompilador en el host (docs/rec-ppe.md): el núcleo compilado
# para Linux ppc64 big-endian y ejecutado con qemu.
#   1. pruebas unitarias con el intérprete y con el recompilador (modo
#      estricto), con llamadas a C por `bl` y por mtctr/bctrl;
#   2. nemu64-test con --rec: las fallas tienen que ser las de la línea base;
#   3. vitest con --rec;
#   4. con --lockstep (lento, ~10 min): nemu64-test y vitest comparados
#      bloque a bloque contra el intérprete.
#   tools/test-rec.sh [--lockstep]
# Imágenes: tools/docker-image-ppc64.sh. Rendimiento: solo en la PS3 real.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/logs
fail=0
NEMU=tests/roms/homebrew/nemu64-test.z64
VITEST=tests/roms/homebrew/salsa64-vitest.z64

if ! tools/build-ppc64.sh >build/logs/build-ppc64.log 2>&1; then
	echo "falló la compilación ppc64 (build/logs/build-ppc64.log)"
	exit 1
fi

# Llamadas por bl y por mtctr, y con registros fijos entre bloques.
for mode in plain far pin; do
	log=build/logs/rec-unit-$mode.log
	far=""; pin=""
	[ $mode = far ] && far=1
	[ $mode = pin ] && pin=1
	SALSA64_REC_FAR=$far SALSA64_REC_PIN=$pin tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec >"$log" 2>&1
	rc=$?
	printf '%-14s %s\n' "unit ($mode)" "$(grep '^total' "$log" | tail -1)"
	grep '^FALLO' "$log" | head -10
	[ $rc -eq 0 ] || fail=1
done

run() {
	log=$1
	shift
	tools/qemu-ppc64.sh "$@" >"$log" 2>&1
}

for pin in "" 1; do
	log=build/logs/rec-nemu${pin:+-pin}.log
	SALSA64_REC_PIN=$pin run "$log" build/ppc64/salsa64-run -c 4000000000 \
		-u 'success rate)' -f 'Exception storm' -f 'panicked at' --rec $NEMU
	rc=$?
	grep "^Test '" "$log" | sed "s/^Test '\([^']*\)'.*/\1/" | LC_ALL=C sort -u >build/logs/rec-nemu.fallas
	new=$(LC_ALL=C comm -13 tests/roms/nemu64-test.fallas build/logs/rec-nemu.fallas)
	gone=$(LC_ALL=C comm -23 tests/roms/nemu64-test.fallas build/logs/rec-nemu.fallas)
	printf '%-14s %s\n' "nemu64-test${pin:+ (pin)}" "$(grep 'success rate)' "$log" | tail -1)"
	grep 'recompilador:' "$log" | tail -1 | sed 's/^/               /'
	[ $rc -eq 0 ] || fail=1
	if [ -n "$new" ] || [ -n "$gone" ]; then
		echo "DIFERENCIA con la línea base del intérprete:"
		echo "$new" | sed 's/^/  nueva: /'
		echo "$gone" | sed 's/^/  ya no falla: /'
		fail=1
	fi
done

run build/logs/rec-vitest.log build/ppc64/salsa64-run -c 400000000 \
	-u 'vitest: OK' -f 'vitest: FALLO' --rec $VITEST
rc=$?
printf '%-14s %s\n' vitest "$(grep 'vitest:' build/logs/rec-vitest.log | tail -1)"
[ $rc -eq 0 ] || fail=1

if [ "${1:-}" = "--lockstep" ]; then
	for rom in $NEMU $VITEST; do
		name=$(basename "$rom" .z64)
		log=build/logs/lock-$name.log
		if [ "$rom" = "$NEMU" ]; then
			QEMU_TIMEOUT=1800 run "$log" build/ppc64/salsa64-run -c 4000000000 -q \
				-u 'success rate)' -f 'Exception storm' -f 'panicked at' --lockstep $rom
		else
			QEMU_TIMEOUT=1800 run "$log" build/ppc64/salsa64-run -c 400000000 \
				-u 'vitest: OK' -f 'vitest: FALLO' --lockstep=1000000 $rom
		fi
		rc=$?
		if [ $rc -eq 0 ]; then
			printf '%-14s %s\n' "lockstep" "$name: sin diferencias"
		else
			printf '%-14s %s\n' "lockstep" "$name: FALLO (código $rc, $log)"
			tail -50 "$log"
			fail=1
		fi
	done
fi
exit $fail
