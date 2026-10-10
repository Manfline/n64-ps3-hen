#!/bin/sh
# Pruebas de la Fase 1: unitarias del núcleo en el host (gcc nativo, ASan +
# UBSan) y las mismas en RPCS3 (ppu-gcc, big-endian), más el micro-benchmark,
# y el frontend en RPCS3 con la ROM vitest embebida (framebuffer del VI en la
# RDRAM y en la pantalla del RSX, en RGBA5551 y RGBA8888), y save-tests dos
# veces (guardado del cartucho: la segunda comprueba que persistió).
# El tiempo de RPCS3 no es concluyente (ver docs/proyecto.md).
set -u
cd "$(dirname "$0")/.."
mkdir -p build/logs
fail=0

if tools/build.sh test-host >build/logs/test-host.log 2>&1; then
	printf '%-10s %s\n' host "$(grep '^total' build/logs/test-host.log)"
else
	printf '%-10s FALLO (build/logs/test-host.log)\n' host
	grep '^FALLO' build/logs/test-host.log | head -20
	fail=1
fi

if ! tools/build.sh phase1 frontend >build/logs/build-phase1.log 2>&1; then
	echo "falló la compilación PS3 (build/logs/build-phase1.log)"
	exit 1
fi
tools/rpcs3-run.py cpu-tests >build/logs/rpcs3-cpu-tests.log 2>&1
rc=$?
res=$(grep 'RESULTADO' build/logs/rpcs3-cpu-tests.log | tail -1 | sed 's/.*RESULTADO //')
printf '%-10s %s\n' rpcs3 "${res:-sin RESULTADO (código $rc)}"
grep -E 'FALLO [a-z]|bench:' build/logs/rpcs3-cpu-tests.log | sed 's/^\[[^]]*\] /           /' | head -20
[ $rc -eq 0 ] || fail=1

tools/rpcs3-run.py build/salsa64/salsa64.elf >build/logs/rpcs3-frontend.log 2>&1
rc=$?
res=$(grep 'RESULTADO' build/logs/rpcs3-frontend.log | tail -1 | sed 's/.*RESULTADO //')
printf '%-10s %s\n' frontend "${res:-sin RESULTADO (código $rc)}"
grep -E 'selftest:|campos/s|rom: ' build/logs/rpcs3-frontend.log | sed 's/^\[[^]]*\] /           /' | head -12
[ $rc -eq 0 ] || fail=1

# Guardado: se borran los de save-tests en el dev_hdd0 de RPCS3 y se abre dos
# veces; la primera escribe (arranque 1) y la segunda lo encuentra.
rm -f "$HOME/Library/Application Support/rpcs3/dev_hdd0/salsa64/saves"/SALSA64_SAVE_*
for n in 1 2; do
	tools/rpcs3-run.py save-tests >build/logs/rpcs3-save-tests-$n.log 2>&1
	rc=$?
	res=$(grep 'RESULTADO' build/logs/rpcs3-save-tests-$n.log | tail -1 | sed 's/.*RESULTADO //')
	printf '%-10s %s\n' guardado$n "${res:-sin RESULTADO (código $rc)}"
	grep -E 'guardado: escrito|volcado diferido|FALLO' build/logs/rpcs3-save-tests-$n.log |
		sed 's/^\[[^]]*\] /           /' | head -8
	case "$res" in
	"save-tests: OK (arranque $n "*) [ $rc -eq 0 ] || fail=1 ;;
	*) fail=1 ;;
	esac
done
exit $fail
