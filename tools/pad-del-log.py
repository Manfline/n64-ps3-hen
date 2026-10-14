#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Las entradas de un juego en el log de la PS3, como guion de salsa64-run --pad.

  tools/pad-del-log.py LOG DESDE HASTA [--origen IP] [--desplazar N]

Toma las líneas «control 1: … (campo N)» que el frontend escribe cuando
cambia el mando (src/frontend/main.c) entre las horas DESDE y HASTA
(«16:13:40», como las pone tools/udplog.py; con --origen, solo las de esa
IP, para separar la consola de RPCS3) y escribe «campo:botones@x,y;…»
(salsa64-run --help). DESDE debe caer después de «menú: abro …» del juego:
el contador de campos empieza en 0 con cada juego. El stick va como lo leyó
el juego (−80…80) y solo cuando cambió bastante para el log, así que el
recorrido es aproximado; los botones son exactos. La emulación en el host
no tiene los tiempos de la PS3: el juego puede leer el mando en otro campo,
pero los menús y las escenas suelen llegar al mismo lugar.

  salsa64-run --pad "$(tools/pad-del-log.py udp.log 15:52:22 15:55:19)" rom.z64
"""
import argparse
import re
import sys

NAMES = {"A": "A", "B": "B", "Z": "Z", "START": "START", "L": "L", "R": "R",
         "ARRIBA": "DU", "ABAJO": "DD", "IZQUIERDA": "DL", "DERECHA": "DR",
         "C_ARRIBA": "CU", "C_ABAJO": "CD", "C_IZQUIERDA": "CL", "C_DERECHA": "CR"}
LINE = re.compile(r"control 1: (.*?)stick=\((-?\d+),(-?\d+)\) \(campo (\d+)\)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log")
    ap.add_argument("desde", help="hora de la primera línea (HH:MM:SS)")
    ap.add_argument("hasta", help="hora de la última línea (HH:MM:SS)")
    ap.add_argument("--origen", help="solo las líneas de esta IP")
    ap.add_argument("--desplazar", type=int, default=0, help="sumar N a cada campo")
    args = ap.parse_args()
    out, last = [], -1
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split(" ", 2)
            if len(parts) < 3 or not (args.desde <= parts[0] <= args.hasta + "~"):
                continue
            if args.origen and parts[1] != args.origen:
                continue
            if "PIF" in line:
                continue  # «PIF → juego»: lo que leyó el juego, repetido
            m = LINE.search(line)
            if not m:
                continue
            buttons = [NAMES[b] for b in m.group(1).split() if b in NAMES]
            x, y, field = int(m.group(2)), int(m.group(3)), int(m.group(4)) + args.desplazar
            field = max(field, last + 1)  # salsa64-run pide campos crecientes
            last = field
            spec = "+".join(buttons) if buttons else "-"
            if x or y:
                spec += "@%d,%d" % (x, y)
            out.append("%d:%s" % (field, spec))
    if not out:
        print("sin líneas «control 1» en ese tramo", file=sys.stderr)
        return 1
    print(";".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
