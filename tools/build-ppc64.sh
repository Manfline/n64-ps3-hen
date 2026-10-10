#!/bin/sh
# Compila el núcleo para Linux ppc64 big-endian (la ABI de la PS3) en
# build/ppc64: salsa64-unit y salsa64-run, estáticos, para correrlos con
# qemu (tools/qemu-ppc64.sh). Imágenes: tools/docker-image-ppc64.sh.
#   tools/build-ppc64.sh [unit|run|all]
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSA64_PPC64_IMAGE:-salsa64-ppc64-cross:dev}"
what="${1:-all}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "No existe la imagen $IMAGE: correr tools/docker-image-ppc64.sh" >&2
	exit 1
fi

# Sin --platform: con la imagen local amd64, Docker (colima) dice que no la
# encuentra; sin la opción la usa y solo avisa (el aviso va a stderr).
# El gcc emulado como amd64 a veces muere con "internal compiler error:
# Segmentation fault" y al repetir compila bien: se reintenta hasta tres
# veces (make retoma donde quedó).
mk1() {
	docker run --rm -i --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ROOT:/src" -w /src "$IMAGE" \
		make --no-print-directory -C "$1" CC=powerpc64-linux-gnu-gcc SAN= OUT=/src/build/ppc64 \
		EXTRA_CFLAGS="-mcpu=cell -mno-relative-jumptables" EXTRA_LDFLAGS=-static all
}
mk() {
	for try in 1 2 3; do
		mk1 "$1" && return 0
		echo "reintento $try: $1" >&2
	done
	mk1 "$1"
}

case "$what" in
unit) mk tests/unit ;;
run)  mk src/host ;;
all)  mk tests/unit && mk src/host ;;
*)    echo "uso: $0 [unit|run|all]" >&2; exit 3 ;;
esac
