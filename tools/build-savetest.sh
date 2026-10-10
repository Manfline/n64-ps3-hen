#!/bin/sh
# Compila la ROM de prueba del guardado (tests/roms/src/savetest) igual que
# vitest y saca tres variantes que solo difieren en el título y en la
# cabecera de homebrew (n64brew.dev: ROM Header, "Advanced Homebrew ROM
# Header": "ED" en 0x3C–0x3D y el tipo de guardado en los 4 bits altos de
# 0x3F): EEPROM de 4 Kbit, de 16 Kbit y SRAM de 256 Kbit. nust64 no escribe
# esa cabecera; se parchea después. El encabezado no entra en ninguna suma
# de verificación (nust64 deja CRC1/CRC2 en cero y el arranque HLE no los
# mira), así que el resultado sigue siendo reproducible.
#   tools/build-savetest.sh [args extra para docker build]
set -eu
cd "$(dirname "$0")/.."
DEST=tests/roms/homebrew
BASE=$DEST/salsa64-savetest.z64

ROM=savetest TITLE="SALSA64 SAVETEST" tools/build-vitest.sh "$@" >/dev/null

# variante <sufijo> <título> <byte 0x3F en octal>
variant() {
	out=$DEST/salsa64-savetest-$1.z64
	cp "$BASE" "$out"
	printf '%-20s' "$2" | dd of="$out" bs=1 seek=32 count=20 conv=notrunc 2>/dev/null
	printf "ED\\000\\$3" | dd of="$out" bs=1 seek=60 count=4 conv=notrunc 2>/dev/null
	(cd "$DEST" && shasum -a 256 "${out##*/}" >"${out##*/}.sha256")
	echo "Listo: $out"
}
variant eeprom4k "SALSA64 SAVE EEP4K" 020   # 0x10
variant eeprom16k "SALSA64 SAVE EEP16K" 040 # 0x20
variant sram "SALSA64 SAVE SRAM" 060        # 0x30
rm -f "$BASE" "$BASE.sha256"
cat "$DEST"/salsa64-savetest-*.sha256
