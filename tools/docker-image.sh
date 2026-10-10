#!/bin/sh
# Construye la imagen del toolchain (cell64-toolchain). La primera vez tarda
# del orden de una hora: compila binutils y GCC para PPU y SPU.
#   tools/docker-image.sh [args extra para docker build]
set -eu
cd "$(dirname "$0")/.."
IMAGE="${CELL64_IMAGE:-cell64-toolchain:dev}"
mkdir -p build/logs
LOG=build/logs/docker-image.log
echo "Construyendo $IMAGE (log en $LOG)"
docker build --progress=plain -f docker/Dockerfile -t "$IMAGE" "$@" . >"$LOG" 2>&1 || {
	echo "Falló la construcción de $IMAGE. Últimas líneas:" >&2
	tail -n 40 "$LOG" >&2
	exit 1
}
echo "Listo: $IMAGE"
tail -n 5 "$LOG"
