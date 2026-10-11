#!/bin/sh
# Genera tests/unit/fpgen_ref.h: los fragment programs que compila cgcomp
# (en la imagen Docker) de los shaders de la combinación lineal de
# lib/rsxkit y de los de tests/unit/fpgen, como arreglos de bytes (el
# archivo .fpo completo). test_fpgen.c los interpreta con el mismo
# intérprete con que prueba al generador: así el intérprete queda anclado
# a programas que ya corren en el RSX, y el generador se compara contra
# cgcomp sin salir del host.
#   tools/fpgen-ref.sh
set -eu
cd "$(dirname "$0")/.."
OUT=tests/unit/fpgen_ref.h
TMP=build/fpgen-ref
mkdir -p "$TMP"
SHADERS="lib/rsxkit/shaders/rsxkit_lincomb0.fcg lib/rsxkit/shaders/rsxkit_lincomb1.fcg \
	lib/rsxkit/shaders/rsxkit_lincomb2.fcg $(ls tests/unit/fpgen/*.fcg)"
for s in $SHADERS; do
	n=$(basename "$s" .fcg)
	tools/build.sh --sh "cgcomp -f $s $TMP/$n.fpo" >/dev/null
done
{
	echo "/* Generado por tools/fpgen-ref.sh: fragment programs compilados por cgcomp"
	echo " * (archivos .fpo completos, big-endian). No editar a mano. */"
	echo "#ifndef SALSA64_FPGEN_REF_H"
	echo "#define SALSA64_FPGEN_REF_H"
	echo "#include <stdint.h>"
	for s in $SHADERS; do
		n=$(basename "$s" .fcg)
		printf 'static const uint8_t fpgen_ref_%s[] = {\n' "$n"
		xxd -i <"$TMP/$n.fpo"
		echo "};"
	done
	echo "#endif"
} >"$OUT"
echo "escrito $OUT"
