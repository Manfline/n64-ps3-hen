#!/bin/sh
# Compila una ROM de prueba propia (tests/roms/src/<ROM>, por omisión vitest)
# en un contenedor reproducible (docker/vitest.Dockerfile) y la deja en
# tests/roms/homebrew/. La primera vez instala Rust y compila nust64: tarda
# unos minutos.
#   tools/build-vitest.sh [args extra para docker build]
#   ROM=gfxtest TITLE="SALSA64 GFXTEST" tools/build-vitest.sh   (o tools/build-gfxtest.sh)
set -eu
cd "$(dirname "$0")/.."
ROM="${ROM:-vitest}"
TITLE="${TITLE:-SALSA64 VITEST}"
OUT=build/$ROM
DEST=tests/roms/homebrew
mkdir -p build/logs "$DEST"
LOG=build/logs/$ROM.log
rm -rf "$OUT"
echo "Compilando $ROM (log en $LOG)"
docker build --progress=plain -f docker/vitest.Dockerfile --target rom \
	--build-arg ROM="$ROM" --build-arg TITLE="$TITLE" \
	--output "type=local,dest=$OUT" "$@" tests/roms/src >"$LOG" 2>&1 || {
	echo "Falló la compilación de $ROM. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
cp "$OUT/salsa64-$ROM.z64" "$OUT/salsa64-$ROM.buildinfo" "$DEST/"
(cd "$DEST" && shasum -a 256 "salsa64-$ROM.z64" >"salsa64-$ROM.z64.sha256")
echo "Listo: $DEST/salsa64-$ROM.z64"
cat "$DEST/salsa64-$ROM.z64.sha256"
