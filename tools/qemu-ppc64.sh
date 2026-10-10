#!/bin/sh
# Corre un programa de build/ppc64 con qemu-ppc64. El directorio de trabajo
# es la raíz del repo; QEMU_TIMEOUT (segundos, 600 por omisión) corta las
# corridas colgadas. stderr se junta con stdout dentro del contenedor (Docker
# no conserva el orden entre los dos). Pasa al programa las variables
# SALSA64_TEST, SALSA64_REC_DUMP, SALSA64_REC_FAR, SALSA64_REC_PIN,
# SALSA64_REC_EAGER y SALSA64_REC_RARE_EXEC.
#
# CPU power7: con -cpu 970 (el más parecido al PPE, Power ISA 2.01) qemu 7.2
# da resultados erróneos (el intérprete falla 15 pruebas que pasan en la PS3
# y con power7/power8), y power5+ se cuelga. power7 acepta instrucciones que
# el PPE no tiene: el emisor (src/cpu/ppc.h) se limita a mano a Power ISA
# 2.02 y el C se compila con -mcpu=cell.
#
# Respaldos de juegos (fuera del repo, nunca en él): SALSA64_ROMS con una
# carpeta que colima comparta (no todas lo están) se
# monta de solo lectura en /roms.
#   tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec
#   SALSA64_ROMS=/ruta/a/respaldos \
#     tools/qemu-ppc64.sh build/ppc64/salsa64-run --lockstep /roms/goldeneye.z64
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${SALSA64_QEMU_IMAGE:-salsa64-qemu:dev}"
exec docker run --rm -i --user "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src \
	${SALSA64_ROMS:+-v} ${SALSA64_ROMS:+"$SALSA64_ROMS:/roms:ro"} \
	-e SALSA64_TEST -e SALSA64_REC_DUMP -e SALSA64_REC_FAR -e SALSA64_REC_PIN -e SALSA64_REC_EAGER \
	-e SALSA64_REC_RARE_EXEC "$IMAGE" \
	sh -c 'exec timeout "$0" qemu-ppc64-static -cpu power7 "$@" 2>&1' "${QEMU_TIMEOUT:-600}" "$@"
