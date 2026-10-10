#!/bin/sh
# Compila nemu64-test en un contenedor reproducible (docker/nemu64-test.Dockerfile)
# y deja el ROM en tests/roms/homebrew/. La primera vez descarga el toolchain de
# Rust y compila core/alloc: tarda varios minutos.
#   tools/build-nemu64-test.sh [args extra para docker build]
set -eu
cd "$(dirname "$0")/.."
OUT=build/nemu64-test
DEST=tests/roms/homebrew
mkdir -p build/logs "$DEST"
LOG=build/logs/nemu64-test.log
rm -rf "$OUT"
echo "Compilando nemu64-test (log en $LOG)"
docker build --progress=plain -f docker/nemu64-test.Dockerfile --target rom \
	--output "type=local,dest=$OUT" "$@" docker >"$LOG" 2>&1 || {
	echo "Falló la compilación de nemu64-test. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
cp "$OUT/nemu64-test.z64" "$OUT/nemu64-test.LICENSE" "$OUT/nemu64-test.NOTICES" "$DEST/"
[ -s "$OUT/nemu64-test.crates" ] && cp "$OUT/nemu64-test.crates" "$DEST/"
(cd "$DEST" && shasum -a 256 nemu64-test.z64 >nemu64-test.z64.sha256)
echo "Listo: $DEST/nemu64-test.z64"
cat "$DEST/nemu64-test.z64.sha256"
