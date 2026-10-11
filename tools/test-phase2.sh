#!/bin/sh
# Pruebas de la Fase 2 (parte gráfica): la HLE en el host (pruebas unitarias
# y gfxtest con el rasterizador de referencia), lib/spukit en RPCS3
# (spu-jobs) y el frontend con gfxtest en RPCS3: HLE en el SPE, dibujo en
# el RSX y la pantalla verificada en los puntos de
# tests/roms/src/gfxtest/expect.h, con la ROM embebida (F3DEX2) y con su
# variante F3D. El tiempo de RPCS3 no es concluyente.
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

GLOG=build/logs/gfxtest.run.log
tools/build.sh host >build/logs/build-host.log 2>&1 || { echo "falló salsa64-run"; exit 1; }
tools/build.sh --sh "ASAN_OPTIONS=detect_leaks=0 build/host/salsa64-run -c 400000000 \
	-u 'gfxtest: OK' -f 'gfxtest: FALLO' --gfx-frame build/gfxtest.ppm --gfxtest-check \
	tests/roms/homebrew/salsa64-gfxtest.z64 2>&1" >"$GLOG" 2>&1
grc=$?
printf '%-10s %s\n' gfxtest "$(grep 'gfxtest: imagen' "$GLOG" | sed 's/^salsa64-run: //')"
[ $grc -eq 0 ] || { echo "           falló (código $grc, $GLOG)"; fail=1; }

if ! tools/build.sh phase2 frontend >build/logs/build-phase2.log 2>&1; then
	echo "falló la compilación PS3 (build/logs/build-phase2.log)"
	exit 1
fi
tools/rpcs3-run.py build/salsa64-spu-jobs/salsa64-spu-jobs.elf >build/logs/rpcs3-spu-jobs.log 2>&1
rc=$?
printf '%-10s %s\n' spu-jobs "$(grep 'RESULTADO' build/logs/rpcs3-spu-jobs.log | tail -1 | sed 's/.*RESULTADO //')"
[ $rc -eq 0 ] || fail=1

tools/rpcs3-run.py build/salsa64/salsa64.elf --embedded gfxtest >build/logs/rpcs3-gfxtest.log 2>&1
rc=$?
printf '%-10s %s\n' frontend "$(grep 'RESULTADO' build/logs/rpcs3-gfxtest.log | tail -1 | sed 's/.*RESULTADO //')"
grep -E 'gfxtest:|gfx:' build/logs/rpcs3-gfxtest.log | sed 's/^\[[^]]*\] /           /' | head -8
[ $rc -eq 0 ] || fail=1

# La variante F3D, desde dev_hdd0/salsa64 de RPCS3 (file:).
R3DATA="$HOME/Library/Application Support/rpcs3/dev_hdd0/salsa64"
mkdir -p "$R3DATA"
cp tests/roms/homebrew/salsa64-gfxtest-f3d.z64 "$R3DATA/"
tools/rpcs3-run.py build/salsa64/salsa64.elf --rom salsa64-gfxtest-f3d.z64 >build/logs/rpcs3-gfxtest-f3d.log 2>&1
rc=$?
printf '%-10s %s\n' f3d "$(grep 'RESULTADO' build/logs/rpcs3-gfxtest-f3d.log | tail -1 | sed 's/.*RESULTADO //')"
grep -E 'gfxtest:|gfx:' build/logs/rpcs3-gfxtest-f3d.log | sed 's/^\[[^]]*\] /           /' | head -8
[ $rc -eq 0 ] || fail=1

# El menú (sin embedded): abre desde la lista gfxtest, audiotest y la
# variante F3D, vuelve al menú después de cada una (cierre completo: el
# segundo gfxtest prueba que el RDP, los SPEs y las colas quedaron limpios)
# y sale por «Salir al XMB». Entre medias, «Aplicar ahora» de 720p a 720p con
# escala 2× (el RSX conserva su contexto; la variante F3D se verifica ya con
# la escala nueva) y de vuelta a la automática.
tools/rpcs3-run.py build/salsa64/salsa64.elf --timeout 600 --menu tests/roms/homebrew/salsa64-gfxtest.z64 \
	tests/roms/homebrew/salsa64-audiotest.z64 "@video 720 2" tests/roms/homebrew/salsa64-gfxtest-f3d.z64 \
	"@video 720 0" \
	>build/logs/rpcs3-menu.log 2>&1
rc=$?
printf '%-10s %s\n' menú "$(grep 'RESULTADO' build/logs/rpcs3-menu.log | tail -1 | sed 's/.*RESULTADO //')"
grep -E 'prueba del juego:|cierre: (estado|ATENCI)|video:|«@video' build/logs/rpcs3-menu.log | sed 's/^\[[^]]*\] /           /' | head -14
[ $rc -eq 0 ] || fail=1
exit $fail
