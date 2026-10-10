#!/bin/sh
# Desensambla los bloques que vuelca SALSA64_REC_DUMP=1 (una línea
# "bloque <pc> ... palabras: w0 w1 ..." por bloque) con el objdump ppc64 de
# la imagen cruzada.
#   SALSA64_REC_DUMP=1 SALSA64_TEST=... tools/qemu-ppc64.sh build/ppc64/salsa64-unit rec | tools/rec-disasm.sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/build/disasm"
mkdir -p "$TMP"
i=0
grep '^bloque ' | while read -r line; do
	i=$((i + 1))
	echo "== ${line%%palabras:*}palabras"
	echo "${line#*palabras: }" | tr ' ' '\n' | grep -v '^$' | while read -r w; do printf '%s' "$w"; done | xxd -r -p >"$TMP/b$i.bin"
	docker run --rm -v "$TMP:/d" salsa64-ppc64-cross:dev powerpc64-linux-gnu-objdump -D -b binary -m powerpc:common64 -EB "/d/b$i.bin" 2>/dev/null |
		sed -n '/<.data>:/,$p' | tail -n +2
done
