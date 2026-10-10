#!/bin/sh
# Compila la ROM de prueba del VI (tests/roms/src/vitest) en un contenedor
# reproducible (docker/vitest.Dockerfile) y la deja en tests/roms/homebrew/.
# La primera vez instala Rust y compila nust64: tarda unos minutos.
#   tools/build-vitest.sh [args extra para docker build]
set -eu
cd "$(dirname "$0")/.."
OUT=build/vitest
DEST=tests/roms/homebrew
mkdir -p build/logs "$DEST"
LOG=build/logs/vitest.log
rm -rf "$OUT"
echo "Compilando vitest (log en $LOG)"
docker build --progress=plain -f docker/vitest.Dockerfile --target rom \
	--output "type=local,dest=$OUT" "$@" tests/roms/src >"$LOG" 2>&1 || {
	echo "Falló la compilación de vitest. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
cp "$OUT/salsa64-vitest.z64" "$OUT/salsa64-vitest.buildinfo" "$DEST/"
(cd "$DEST" && shasum -a 256 salsa64-vitest.z64 >salsa64-vitest.z64.sha256)
echo "Listo: $DEST/salsa64-vitest.z64"
cat "$DEST/salsa64-vitest.z64.sha256"
