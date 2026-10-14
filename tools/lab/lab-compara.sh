#!/bin/sh
# Compara dos emisores sobre el mismo perfil (docs/lab-recompilador.md, «Antes y después»).
#   tools/lab/lab-compara.sh REV_A REV_B -- <argumentos de rec-lab sin -o>
# REV_x es cualquier revisión de git, o WORK para el árbol de trabajo. Cada una se
# extrae a build/lab/rev/<sha> (solo src/) y, si no trae los ganchos del laboratorio
# (SALSA64_REC_LAB en cpu/rec.h), se les aplica tools/lab/lab-ganchos.patch. Después
# se compila rec-lab de cada una, se traduce el mismo perfil y se imprime
# lab-analiza.py compara. Ejemplo (GoldenEye en la presa, perfil de la PS3):
#   tools/lab/lab-compara.sh HEAD~3 WORK -- --rom gold.z64 --state dam.state --prf GOLDENEYE.prf
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
A="$1"
B="$2"
shift 2
[ "${1:-}" = "--" ] && shift
prep() {
	rev="$1"
	if [ "$rev" = WORK ]; then
		echo "$ROOT"
		return
	fi
	sha="$(git -C "$ROOT" rev-parse --short "$rev")"
	dir="$ROOT/build/lab/rev/$sha"
	if [ ! -d "$dir/src" ]; then
		mkdir -p "$dir"
		git -C "$ROOT" archive "$rev" src | tar -x -C "$dir"
		if ! grep -q SALSA64_REC_LAB "$dir/src/cpu/rec.h"; then
			patch -s -p1 -d "$dir" <"$ROOT/tools/lab/lab-ganchos.patch" || {
				echo "lab-compara: los ganchos no aplican a $rev: resolver a mano en $dir" >&2
				exit 1
			}
		fi
	fi
	echo "$dir"
}
SA="$(prep "$A")"
SB="$(prep "$B")"
OUT="$ROOT/build/lab/comparas"
mkdir -p "$OUT"
for x in A B; do
	eval "src=\$S$x"
	"$ROOT/tools/lab/build.sh" "$src" "$src/build-lab" >/dev/null
	"$src/build-lab/rec-lab" "$@" -o "$OUT/$x.lab" 2>&1 | tail -1
done
python3 "$ROOT/tools/lab/lab-analiza.py" compara "$OUT/A.lab" "$OUT/B.lab"
