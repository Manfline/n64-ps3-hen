#!/bin/sh
# Ejecuta make (o cualquier comando con --sh) dentro de la imagen del toolchain.
#   tools/build.sh                 make all (todos los programas de la Fase 0)
#   tools/build.sh -C tests/phase0/jit-ppe pkg
#   tools/build.sh --sh 'ppu-gcc --version'
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${RETRO64_IMAGE:-retro64-toolchain:dev}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "No existe la imagen $IMAGE: correr tools/docker-image.sh" >&2
	exit 1
fi

TTY=""
[ -t 0 ] && [ -t 1 ] && TTY="-t"

run() {
	docker run --rm -i $TTY --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ROOT:/src" -w /src "$IMAGE" "$@"
}

if [ "${1:-}" = "--sh" ]; then
	shift
	run sh -c "$*"
else
	run make "$@"
fi
