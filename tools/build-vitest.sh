#!/bin/sh
# Compila una ROM de prueba propia (tests/roms/src/<ROM>, por omisión vitest)
# en un contenedor reproducible (docker/vitest.Dockerfile) y la deja en
# tests/roms/homebrew/. La primera vez instala Rust y compila nust64: tarda
# unos minutos.
#   tools/build-vitest.sh [args extra para docker build]
#   ROM=gfxtest TITLE="SALSA64 GFXTEST" tools/build-vitest.sh   (o tools/build-gfxtest.sh)
# GBI (variante de gfxtest) y NAME (nombre de la ROM, por omisión
# salsa64-<ROM>) pasan al Dockerfile.
set -eu
cd "$(dirname "$0")/.."
ROM="${ROM:-vitest}"
TITLE="${TITLE:-SALSA64 VITEST}"
NAME="${NAME:-salsa64-$ROM}"
OUT=build/$NAME
DEST=tests/roms/homebrew
mkdir -p build/logs "$DEST"
LOG=build/logs/$NAME.log
rm -rf "$OUT"
echo "Compilando $NAME (log en $LOG)"
docker build --progress=plain -f docker/vitest.Dockerfile --target rom \
	--build-arg ROM="$ROM" --build-arg TITLE="$TITLE" \
	--build-arg GBI="${GBI:-}" --build-arg NAME="$NAME" \
	--output "type=local,dest=$OUT" "$@" tests/roms/src >"$LOG" 2>&1 || {
	echo "Falló la compilación de $NAME. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
cp "$OUT/$NAME.z64" "$OUT/$NAME.buildinfo" "$DEST/"
(cd "$DEST" && shasum -a 256 "$NAME.z64" >"$NAME.z64.sha256")
echo "Listo: $DEST/$NAME.z64"
cat "$DEST/$NAME.z64.sha256"
