#!/bin/sh
# Lanza una instancia propia de ares (v148) con un respaldo y su servidor GDB en
# un puerto aparte (por omisión 9140; 9123 es la ventana del autor y otras sesiones
# usan 9124 y 9150) y espera a que escuche. La instancia es una copia de ares.app
# con el ejecutable renombrado (ares-lab): otras sesiones cierran ares con
# `pkill -x ares` y se llevarían la del laboratorio. Los guardados de ares caen
# junto al respaldo, así que se copia a build/lab/ares-roms/ antes.
#   tools/lab/ares-lanzar.sh /ruta/al/respaldo.z64 [puerto]
#   pkill -x ares-lab        # para cerrarla
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ROM="$1"
PORT="${2:-9140}"
APP="$ROOT/build/lab/ares-lab.app"
BML="$ROOT/build/lab/ares/ares-$PORT.bml"
BASE="${ARES_SETTINGS:-}"
mkdir -p "$ROOT/build/lab/ares" "$ROOT/build/lab/ares-roms"
[ -n "$BASE" ] && [ -f "$BASE" ] || { echo "ARES_SETTINGS debe apuntar a una configuración de ares con DebugServer habilitado (Enabled: true, Port: N)" >&2; exit 1; }
if [ ! -d "$APP" ]; then
	cp -R /Applications/ares.app "$APP"
	mv "$APP/Contents/MacOS/ares" "$APP/Contents/MacOS/ares-lab"
	/usr/libexec/PlistBuddy -c "Set :CFBundleExecutable ares-lab" -c "Set :CFBundleIdentifier dev.ares.ares-lab" "$APP/Contents/Info.plist"
	codesign --force --deep -s - "$APP" >/dev/null 2>&1
	xattr -cr "$APP"
fi
sed "s/  Port: [0-9]*/  Port: $PORT/" "$BASE" > "$BML"
cp -n "$ROM" "$ROOT/build/lab/ares-roms/" 2>/dev/null || true
open -n -a "$APP" --args --settings-file "$BML" --system "Nintendo 64" --no-file-prompt "$ROOT/build/lab/ares-roms/$(basename "$ROM")"
for _ in $(seq 1 60); do
	lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1 && { echo "ares-lab escucha en [::1]:$PORT"; exit 0; }
	sleep 0.5
done
echo "ares no abrió el puerto $PORT" >&2
exit 1
