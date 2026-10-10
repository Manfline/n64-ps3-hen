#!/bin/sh
# Corre en RPCS3 las cuatro pruebas de la Fase 0 y resume. Recompila antes.
# jit-ppe usa el intérprete del PPU y relanza tras cada caída; en RPCS3 su
# resultado no es concluyente (docs/rpcs3.md), solo valida que el programa
# completa su ciclo.
set -u
cd "$(dirname "$0")/.."
mkdir -p build/logs
tools/build.sh >build/logs/build.log 2>&1 || { echo "falló la compilación (build/logs/build.log)"; exit 1; }

fail=0
tools/udplog.py --out build/logs/udp-phase0.log --timeout 600 >/dev/null 2>build/logs/udp-phase0.summary &
UDP=$!
for p in udplog spu-compute rsx-triangle jit-ppe; do
	extra=""
	if [ "$p" = jit-ppe ]; then
		rm -f "$HOME/Library/Application Support/rpcs3/dev_hdd0/retro64/jit_state.txt"
		extra="--relaunch 10 --config tools/rpcs3/retro64-interp.yml"
	fi
	tools/rpcs3-run.py "$p" $extra >"build/logs/rpcs3-$p.log" 2>&1
	rc=$?
	res=$(grep 'RESULTADO' "build/logs/rpcs3-$p.log" | tail -1 | sed 's/.*RESULTADO //')
	printf '%-13s %s\n' "$p" "${res:-sin RESULTADO (código $rc)}"
	[ $rc -eq 0 ] || fail=1
done
kill $UDP 2>/dev/null; wait $UDP 2>/dev/null
grep '^resumen' build/logs/udp-phase0.summary | sed 's/^resumen/udp/'
exit $fail
