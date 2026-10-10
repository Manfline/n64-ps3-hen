#!/bin/sh
# Construye las imágenes para probar el recompilador en el host
# (docker/ppc64.Dockerfile): compilador cruzado ppc64 y qemu-user.
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/logs
LOG=build/logs/docker-image-ppc64.log
for target in cross qemu; do
	image=salsa64-ppc64-cross:dev
	[ "$target" = qemu ] && image=salsa64-qemu:dev
	echo "Construyendo $image (log en $LOG)"
	docker build --progress=plain -f docker/ppc64.Dockerfile --target "$target" -t "$image" . >>"$LOG" 2>&1 || {
		echo "Falló la construcción de $image. Últimas líneas:" >&2
		tail -n 30 "$LOG" >&2
		exit 1
	}
done
echo "Listo: salsa64-ppc64-cross:dev, salsa64-qemu:dev"
