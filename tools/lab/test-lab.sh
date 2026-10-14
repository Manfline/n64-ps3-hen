#!/bin/sh
# Pruebas del laboratorio del recompilador (sin datos de juegos): compila rec-lab con el cc nativo y
# corre tools/lab/test_lab.py.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
"$ROOT/tools/lab/build.sh" >/dev/null
exec python3 "$ROOT/tools/lab/test_lab.py" "$@"
