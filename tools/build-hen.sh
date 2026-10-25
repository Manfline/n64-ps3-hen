#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compila este fork con título e identificador propios en el XMB.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

case "${1:-}" in
    -h|--help)
        echo "Uso: sh tools/build-hen.sh [opciones de make, por ejemplo -j4]"
        echo "Requisito: construir primero el toolchain con tools/docker-image.sh."
        echo "Salida: build/n64-ps3-hen/n64-ps3-hen.pkg"
        exit 0
        ;;
esac

exec "$ROOT/tools/build.sh" -C src/frontend \
    PROGRAM=n64-ps3-hen TITLE="N64 PS3 HEN - Salsa64" APPID=NPSH00064 pkg "$@"
