#!/bin/sh
# cgcomp de PSL1GHT compilado para x86_64, porque carga libCg.so (NVIDIA Cg
# Toolkit, solo x86_64) con dlopen(). En hosts que no son x86_64 se ejecuta
# con qemu-user usando la raíz mínima de /opt/cg-amd64.
ROOT=/opt/cg-amd64
LIBS=/usr/lib64:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu

if [ "$(uname -m)" = "x86_64" ]; then
	LD_LIBRARY_PATH="$ROOT/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
		exec "$ROOT/opt/cell64/bin/cgcomp" "$@"
fi

exec qemu-x86_64-static -L "$ROOT" -E LD_LIBRARY_PATH="$LIBS" \
	"$ROOT/opt/cell64/bin/cgcomp" "$@"
