#!/usr/bin/env python3
"""Hilos de libultra en un volcado de la RDRAM (salsa64-run --ram-dump).

Busca estructuras OSThread por su forma (libultra, documentada en
n64brew.dev y en los encabezados públicos de libultra: next, priority,
queue, tlnext, state, flags, id, fp, thprof y el contexto con pc en +0x11C)
y muestra, por hilo, estado, prioridad, pc, ra, sp y la cola de mensajes en
la que espera (OSMesgQueue: mtqueue, fullqueue, validCount, first,
msgCount, msg). Sirve para ver en qué espera cada hilo cuando un juego se
queda quieto. No depende de símbolos del juego.

    tools/libultra-threads.py build/host-mac/juego.ram [--all]
"""
import argparse
import struct
import sys

STATES = {1: "parado", 2: "listo", 4: "corriendo", 8: "esperando"}


def rd32(ram, off):
    return struct.unpack_from(">I", ram, off)[0]


def is_ptr(v, size):
    return v == 0 or (0x80000000 <= v < 0x80000000 + size and v % 4 == 0)


def scan(ram):
    size = len(ram)
    found = []
    for off in range(0, size - 0x130, 4):
        state = struct.unpack_from(">H", ram, off + 0x10)[0]
        if state not in STATES:
            continue
        nxt, pri, queue, tlnext = struct.unpack_from(">IiII", ram, off)
        flags, tid, fp = struct.unpack_from(">HiI", ram, off + 0x12)
        if not (0 <= pri <= 255 and 0 <= tid <= 255 and flags <= 3 and fp <= 1):
            continue
        if not (is_ptr(nxt, size) and is_ptr(queue, size) and is_ptr(tlnext, size)):
            continue
        sr, pc = struct.unpack_from(">II", ram, off + 0x118)
        if not (0x80000000 <= pc < 0x80000000 + size) or (sr >> 28) not in (0, 2, 3):
            continue
        found.append(off)
    return found


def thread_info(ram, off):
    size = len(ram)
    nxt, pri, queue, tlnext = struct.unpack_from(">IiII", ram, off)
    state, flags, tid = struct.unpack_from(">HHi", ram, off + 0x10)
    sp = struct.unpack_from(">Q", ram, off + 0x20 + 26 * 8)[0]
    ra = struct.unpack_from(">Q", ram, off + 0x20 + 28 * 8)[0]
    sr, pc, cause, badvaddr = struct.unpack_from(">IIII", ram, off + 0x118)
    line = "hilo id %3d pri %3d %-9s en 0x%08X: pc %08X ra %08X sp %08X sr %08X" % (
        tid, pri, STATES[state], 0x80000000 + off, pc, ra & 0xFFFFFFFF, sp & 0xFFFFFFFF, sr)
    if state == 8 and queue:
        # Espera en una OSMesgQueue: &mq->mtqueue (recibir) o &mq->fullqueue (enviar).
        for base, what in ((queue, "recibir"), (queue - 4, "enviar")):
            o = base - 0x80000000
            if 0 <= o <= size - 0x18:
                mt, full, valid, first, count, msg = struct.unpack_from(">IIiiiI", ram, o)
                if 0 <= valid <= count <= 64 and 0 <= first < max(count, 1) and is_ptr(msg, size):
                    line += "; %s en la cola 0x%08X (%d de %d mensajes, msg en %08X)" % (
                        what, base, valid, count, msg)
                    break
        else:
            line += "; espera en 0x%08X" % queue
    elif state == 8:
        line += "; esperando (cola 0)"
    return tid, line


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ram")
    ap.add_argument("--all", action="store_true", help="también los hilos parados")
    args = ap.parse_args()
    ram = open(args.ram, "rb").read()
    rows = [thread_info(ram, off) for off in scan(ram)]
    if not args.all:
        rows = [r for r in rows if "parado" not in r[1]]
    for _, line in sorted(rows):
        print(line)
    if not rows:
        print("no se encontraron hilos", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
