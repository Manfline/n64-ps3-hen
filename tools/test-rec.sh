#!/bin/sh
# Pruebas del recompilador en el host (docs/rec-ppe.md): el núcleo compilado
# para Linux ppc64 big-endian y ejecutado con qemu.
#   1. pruebas unitarias con el intérprete y con el recompilador (modo
#      estricto), con llamadas a C por `bl` y por mtctr/bctrl;
#   2. nemu64-test con --rec: las fallas tienen que ser las de la línea base;
#   3. vitest con --rec;
#   4. estados guardados con --rec (gfxtest): guardar, cargar y seguir da lo
#      mismo que de corrido y que el intérprete;
#   5. el perfilado (--perfilado): mismo estado que sin él y una sesión que
#      el lector (tools/perfilado) entiende;
#   6. con --lockstep (lento, ~10 min): nemu64-test y vitest comparados
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

# Llamadas por bl y por mtctr, y con las opciones (registros fijos,
# escritura temprana, la FPU residente experimental).
for mode in plain far pin eager fpures; do
	log=build/logs/rec-unit-$mode.log
	far=""; pin=""; eager=""; fpures=""
	[ $mode = far ] && far=1
	[ $mode = pin ] && pin=1
	[ $mode = eager ] && eager=1 && pin=1
	[ $mode = fpures ] && fpures=1 && pin=1
	SALSA64_REC_FAR=$far SALSA64_REC_PIN=$pin SALSA64_REC_EAGER=$eager SALSA64_REC_FPU_RES=$fpures \
		tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec >"$log" 2>&1
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
	SALSA64_REC_PIN=$pin SALSA64_REC_EAGER=$pin run "$log" build/ppc64/salsa64-run -c 4000000000 \
		-u 'success rate)' -f 'Exception storm' -f 'panicked at' --rec $NEMU
	rc=$?
	grep "^Test '" "$log" | sed "s/^Test '\([^']*\)'.*/\1/" | LC_ALL=C sort -u >build/logs/rec-nemu.fallas
	new=$(LC_ALL=C comm -13 tests/roms/nemu64-test.fallas build/logs/rec-nemu.fallas)
	gone=$(LC_ALL=C comm -23 tests/roms/nemu64-test.fallas build/logs/rec-nemu.fallas)
	printf '%-14s %s\n' "nemu64-test${pin:+ (pin+eager)}" "$(grep 'success rate)' "$log" | tail -1)"
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

# fastmem (docs/fastmem.md): los accesos de KSEG0/KSEG1 sin prueba sobre la
# ventana; nemu64-test suma las fallas de tests/roms/nemu64-test.fastmem.fallas
# (direcciones no alineadas o sin extensión de signo, que en la N64 son
# excepciones y con fastmem no) y nada más.
log=build/logs/rec-nemu-fastmem.log
SALSA64_REC_FASTMEM=1 SALSA64_REC_PIN=1 run "$log" build/ppc64/salsa64-run -c 4000000000 \
	-u 'success rate)' -f 'Exception storm' -f 'panicked at' --rec $NEMU
rc=$?
grep "^Test '" "$log" | sed "s/^Test '\([^']*\)'.*/\1/" | LC_ALL=C sort -u >build/logs/rec-nemu-fastmem.fallas
LC_ALL=C sort -u tests/roms/nemu64-test.fallas tests/roms/nemu64-test.fastmem.fallas >build/logs/rec-nemu-fastmem.base
new=$(LC_ALL=C comm -13 build/logs/rec-nemu-fastmem.base build/logs/rec-nemu-fastmem.fallas)
gone=$(LC_ALL=C comm -23 build/logs/rec-nemu-fastmem.base build/logs/rec-nemu-fastmem.fallas)
printf '%-14s %s\n' "nemu64 fastmem" "$(grep 'success rate)' "$log" | tail -1)"
grep 'fastmem:' "$log" | tail -1 | sed 's/^/               /'
[ $rc -eq 0 ] || fail=1
if [ -n "$new" ] || [ -n "$gone" ]; then
	echo "DIFERENCIA con la línea base de fastmem:"
	echo "$new" | sed 's/^/  nueva: /'
	echo "$gone" | sed 's/^/  ya no falla: /'
	fail=1
fi
SALSA64_REC_FASTMEM=1 run build/logs/rec-vitest-fastmem.log build/ppc64/salsa64-run -c 400000000 \
	-u 'vitest: OK' -f 'vitest: FALLO' --rec $VITEST
rc=$?
printf '%-14s %s\n' "vitest fastmem" "$(grep 'vitest:' build/logs/rec-vitest-fastmem.log | tail -1)"
[ $rc -eq 0 ] || fail=1

# Estados guardados con el recompilador (gfxtest): guardar en el campo 150,
# cargar en otra corrida (la caché del recompilador se vacía) y seguir hasta
# el 300 da el mismo estado que de corrido, y el mismo que el intérprete.
G=tests/roms/homebrew/salsa64-gfxtest.z64
SD=build/states/rec
rm -rf $SD
mkdir -p $SD
R="build/ppc64/salsa64-run -q"
if run build/logs/rec-estados-a.log $R --rec --fields 300 --state-save 300:$SD/m.st $G &&
   run build/logs/rec-estados-b.log $R --rec --fields 150 --state-save 150:$SD/n.st $G &&
   run build/logs/rec-estados-c.log $R --rec --state-load $SD/n.st --fields 300 --state-save 300:$SD/m2.st $G &&
   run build/logs/rec-estados-i.log $R --fields 300 --state-save 300:$SD/mi.st $G; then
	if cmp -s $SD/m.st $SD/m2.st && cmp -s $SD/m.st $SD/mi.st; then
		printf '%-14s %s\n' estados "gfxtest con --rec: guardar en 150 y cargar = de corrido hasta 300 = intérprete"
	else
		printf '%-14s %s\n' estados "FALLO: con carga $(cmp $SD/m.st $SD/m2.st | head -1); intérprete $(cmp $SD/m.st $SD/mi.st | head -1)"
		fail=1
	fi
else
	printf '%-14s %s\n' estados "FALLO (build/logs/rec-estados-*.log)"
	fail=1
fi

# Perfilado (docs/perfilado.md): con todo encendido (una ventana de la traza
# por segundo) el estado al campo 300 es el mismo que sin perfilar, y el
# lector entiende la sesión.
PD=build/perfilado-test
rm -rf $PD
if SALSA64_PERFILADO_CFG="traza_cada_s=1" run build/logs/rec-perfilado.log $R --rec --fields 300 \
	--state-save 300:$SD/mp.st --perfilado $PD $G &&
   python3 tools/perfilado/perfilado.py resumen $PD >build/logs/rec-perfilado-resumen.log 2>&1 &&
   grep -q -- '-- traza: [1-9]' build/logs/rec-perfilado-resumen.log &&
   grep -q -- '-- recompilador: [1-9]' build/logs/rec-perfilado-resumen.log; then
	if cmp -s $SD/m.st $SD/mp.st; then
		printf '%-14s %s
' perfilado "gfxtest con --perfilado: mismo estado en el campo 300; $(grep -- '-- traza:' build/logs/rec-perfilado-resumen.log | cut -c4-90)"
	else
		printf '%-14s %s
' perfilado "FALLO: el estado cambia con el perfilado ($(cmp $SD/m.st $SD/mp.st | head -1))"
		fail=1
	fi
else
	printf '%-14s %s
' perfilado "FALLO (build/logs/rec-perfilado*.log)"
	fail=1
fi

if [ "${1:-}" = "--lockstep" ]; then
	# Cargar un estado con --lockstep: la referencia carga el mismo.
	run build/logs/lock-estados.log $R --lockstep=100000 --state-load $SD/n.st --fields 300 $G
	rc=$?
	if [ $rc -eq 0 ]; then
		printf '%-14s %s\n' "lockstep" "gfxtest desde un estado cargado: sin diferencias"
	else
		printf '%-14s %s\n' "lockstep" "gfxtest desde un estado cargado: FALLO (código $rc, build/logs/lock-estados.log)"
		fail=1
	fi
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
