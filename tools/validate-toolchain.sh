#!/bin/sh
# Valida la imagen del toolchain (criterio de la Fase 0):
#   1. compila todos los samples de PSL1GHT (mismo commit que la imagen);
#   2. compila con cgcomp los shaders .vcg/.fcg de la rama ps3 del port de
#      SM64 (solo shaders: sin assets ni ROM).
# Las fuentes se descargan a build/validate/ (ignorado por git). Log completo
# en build/logs/validate.log; en la terminal solo el resumen.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${RETRO64_IMAGE:-retro64-toolchain:dev}"
SM64_REF=b0fb69da3158eb26fb8747e9284219017e532950
PSL1GHT_REF=$(docker image inspect -f '{{index .Config.Labels "retro64.psl1ght.ref"}}' "$IMAGE")
mkdir -p "$ROOT/build/logs"
LOG="$ROOT/build/logs/validate.log"

docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp -e SM64_REF="$SM64_REF" -e REF="$PSL1GHT_REF" \
	-v "$ROOT:/src" -w /src "$IMAGE" sh -s >"$LOG" 2>&1 <<'INNER' || true
set -u
W=/src/build/validate
rm -rf "$W" && mkdir -p "$W"

echo "== samples de PSL1GHT $REF"
curl -fsSL "https://github.com/ps3dev/PSL1GHT/archive/$REF.tar.gz" | tar -xz -C "$W"
ok=0; fail=0
for mk in $(find "$W"/PSL1GHT-*/samples -mindepth 3 -maxdepth 3 -name Makefile | sort); do
	d=$(dirname "$mk"); name=${d#*/samples/}
	if make -C "$d" >"$W/sample.log" 2>&1; then
		echo "SAMPLE OK    $name"; ok=$((ok+1))
	else
		echo "SAMPLE FALLO $name: $(grep -m1 -E 'error|Error' "$W/sample.log")"; fail=$((fail+1))
	fi
done
echo "SAMPLES: $ok OK, $fail FALLO"

echo "== shaders de sm64-port (ps3) $SM64_REF"
git -c advice.detachedHead=false clone -q --filter=blob:none --no-checkout https://github.com/fgsfdsfgs/sm64-port "$W/sm64"
git -C "$W/sm64" sparse-checkout set --no-cone ps3/shaders
git -C "$W/sm64" checkout -q "$SM64_REF"
ok=0; fail=0
for f in "$W"/sm64/ps3/shaders/*.vcg "$W"/sm64/ps3/shaders/*.fcg; do
	case "$f" in *.vcg) t=-v; o="${f%.vcg}.vpo";; *) t=-f; o="${f%.fcg}.fpo";; esac
	if cgcomp $t "$f" "$o" >"$W/cg.log" 2>&1 && [ -s "$o" ]; then
		ok=$((ok+1))
	else
		echo "SHADER FALLO $(basename "$f"): $(head -c 300 "$W/cg.log")"; fail=$((fail+1))
	fi
done
echo "SHADERS: $ok OK, $fail FALLO"
INNER

grep -E '^(SAMPLES|SHADERS|SAMPLE FALLO|SHADER FALLO|==)' "$LOG" || true
echo "(log completo: build/logs/validate.log)"
