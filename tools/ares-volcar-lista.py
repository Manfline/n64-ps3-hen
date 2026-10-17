#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Salsa64 — volcados de ares coherentes: la RDRAM justo cuando el CPU empieza a armar una display list.

Un volcado a un instante cualquiera (tools/ares-comparar.sh, docs/referencia-ares.md) suele salir
con la lista de dos cuadros después y la imagen del cuadro anterior: los juegos arman el cuadro N + 2
en la misma lista que dibujó el N. Aquí se pone un punto de parada de escritura (Z2 del GDB de ares)
en el primer comando de cada una de las dos listas del juego: ares se detiene en la primera
escritura del CPU a la lista, que es cuando la que se acaba de escribir ya la terminó de dibujar el
RSP y todavía está entera, con su color image completa. Se guarda entonces la RDRAM, la DMEM del RSP
y los registros del VI como lo hace la herramienta de volcados de docs/referencia-ares.md, más
PREFIJO-NN.watch con la dirección de la lista que se estaba escribiendo (a esa lista y a su color
image, no a la otra, hay que compararlas con `salsa64-run --ares`).

    tools/ares-volcar-lista.py PUERTO N PAUSA PREFIJO LISTA1 LISTA2 [PRIMERO]

PUERTO es el del GDB de ares (DebugServer.Port de su configuración), N el número de volcados, PAUSA
los segundos entre uno y otro (con una variación al azar de la mitad), y LISTA1 y LISTA2 las
direcciones KSEG0 en hexadecimal de las dos listas (Diddy Kong Racing: 801fda00 8021ee10; salen del
`data_ptr` de las OSTask de `salsa64-run --trace-rsp`). Conectarse detiene a ares: se reconecta en
cada volcado.
"""
import binascii
import random
import socket
import sys
import time


def snap(port, lists, prefix):
    k = socket.create_connection(("::1", port), timeout=30)
    buf = [b""]

    def pk(c):
        q = c.encode()
        return b"$" + q + b"#" + b"%02x" % (sum(q) & 0xFF)

    def reply():
        while True:
            b = buf[0]
            i = b.find(b"$")
            if i >= 0:
                j = b.find(b"#", i)
                if j >= 0 and len(b) >= j + 3:
                    buf[0] = b[j + 3:]
                    k.sendall(b"+")
                    return b[i + 1:j].decode("latin1")
            d = k.recv(1 << 20)
            if not d:
                raise EOFError
            buf[0] += d

    def ask(c):
        k.sendall(pk(c))
        return reply()

    def mem(a, size, step=0x1000):
        o = bytearray()
        for x in range(0, size, step):
            o += binascii.unhexlify(ask("m%x,%x" % (a + x, min(step, size - x))))
        return bytes(o)

    k.sendall(b"+")
    ask("?")
    for a in lists:
        ask("Z2,%s,4" % a)
    which = None
    for _ in range(400):  # ares también se detiene en cada excepción del CPU
        st = ask("c")
        if "watch:" in st:
            which = st.split("watch:")[1].split(";")[0]
            break
    for a in lists:
        ask("z2,%s,4" % a)
    open(prefix + "-rdram.bin", "wb").write(mem(0x80000000, 0x800000))
    open(prefix + "-rsp-dmem.bin", "wb").write(mem(0xA4000000, 0x1000))
    vi = mem(0xA4400000, 0x38, 0x38)
    open(prefix + "-vi.bin", "wb").write(vi)
    open(prefix + ".watch", "w").write("%s\n" % which)
    k.sendall(pk("c"))
    time.sleep(0.05)
    k.close()
    return which, vi


def main():
    if len(sys.argv) < 7:
        print(__doc__)
        return 2
    port, n, gap, out = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
    lists = [sys.argv[5], sys.argv[6]]
    first = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    for i in range(first, first + n):
        t = time.time()
        which, vi = snap(port, lists, "%s-%02d" % (out, i))
        print(i, which, "VI_ORIGIN %08x (%.1f s)" % (int.from_bytes(vi[4:8], "big"), time.time() - t), flush=True)
        if i < first + n - 1:
            time.sleep(gap * (0.5 + random.random()))
    return 0


sys.exit(main())
