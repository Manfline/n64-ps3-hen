#!/usr/bin/env python3
"""Perfil estadístico de un juego en ares por su servidor GDB (docs/lab-recompilador.md).

ares (v148) escucha el protocolo GDB en [::1]:9123 (la ventana del autor) o en
el puerto de la instancia propia (tools/lab/ares-lanzar.sh). Una sola conexión:
«?» (ares detiene el CPU al conectar), «c» y, a intervalos aleatorios, el byte
0x03 (interrupción), «g» (el PC es el registro 37) y «c» otra vez: ~100
muestras por segundo, con esperas aleatorias para no sincronizarse con los 60
Hz. (La variante --reconectar abre una conexión por muestra, ~10 por segundo:
sirve si el servidor se detiene en cada excepción del CPU, como pasa con
algunos juegos con un cliente conectado.)
Además vuelca la RDRAM entera en los momentos pedidos (el código, overlays
incluidos: lo que hace falta para traducir los bloques con rec-lab).

    tools/lab/ares-perfil.py --port 9124 --segundos 120 --volcados 10,60,110 -o build/lab/ares/pd

Escribe <o>-pcs.tsv («pc cuenta», hex), <o>-rdram-<k>.bin y <o>-meta.txt.
Trampas de ares: «qSupported» cierra la conexión; Z0 solo con direcciones de
32 bits. Los volcados e histogramas llevan código del juego: no se versionan.
"""
import argparse
import binascii
import collections
import random
import socket
import time


class Gdb:
    def __init__(self, port, host="::1", timeout=30):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buf = b""

    @staticmethod
    def pk(cmd):
        q = cmd.encode()
        return b"$" + q + b"#" + b"%02x" % (sum(q) & 0xFF)

    def reply(self):
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    body = self.buf[i + 1:j]
                    self.buf = self.buf[j + 3:]
                    self.sock.sendall(b"+")
                    return body.decode("latin1")
            d = self.sock.recv(1 << 20)
            if not d:
                raise EOFError
            self.buf += d

    def ask(self, cmd):
        self.sock.sendall(self.pk(cmd))
        return self.reply()

    def hello(self):
        self.sock.sendall(b"+")
        return self.ask("?")

    def pc(self):
        g = self.ask("g")
        return int(g[37 * 16:38 * 16], 16) & 0xFFFFFFFF

    def mem(self, addr, size, step=0x4000):
        out = bytearray()
        for o in range(0, size, step):
            out += binascii.unhexlify(self.ask("m%x,%x" % (addr + o, min(step, size - o))))
        return bytes(out)

    def cont(self):
        self.sock.sendall(self.pk("c"))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect(port, tries=50):
    for _ in range(tries):
        try:
            return Gdb(port)
        except OSError:
            time.sleep(0.05)
    raise OSError("no se puede conectar a ares en el puerto %d" % port)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9140)
    ap.add_argument("--segundos", type=float, default=60.0, help="duración del muestreo")
    ap.add_argument("--min-ms", type=float, default=3.0, help="espera mínima entre muestras")
    ap.add_argument("--max-ms", type=float, default=10.0, help="espera máxima entre muestras")
    ap.add_argument("--volcados", default="", help="segundos (desde el inicio del muestreo) de los volcados de la RDRAM, separados por comas")
    ap.add_argument("--max-muestras", type=int, default=0)
    ap.add_argument("--reconectar", action="store_true",
                    help="una conexión por muestra (lento: ~10 muestras/s); por omisión, una conexión y el byte 0x03")
    ap.add_argument("-o", required=True, help="prefijo de salida")
    a = ap.parse_args()

    dumps = sorted(float(x) for x in a.volcados.split(",") if x)
    hist = collections.Counter()
    n = errors = next_dump = 0
    t0 = time.time()
    lat = 0.0
    print("muestreando ares en el puerto %d durante %.0f s…" % (a.port, a.segundos), flush=True)
    g = None
    while time.time() - t0 < a.segundos and (not a.max_muestras or n < a.max_muestras):
        try:
            if g is None:
                g = connect(a.port)
                g.hello()
                g.cont()
                g.sock.settimeout(5)
            time.sleep(random.uniform(a.min_ms, a.max_ms) / 1000.0)
            t1 = time.time()
            g.sock.sendall(b"\x03")
            g.reply()
            lat += time.time() - t1
            pc = g.pc()
            if next_dump < len(dumps) and time.time() - t0 >= dumps[next_dump]:
                td = time.time()
                with open("%s-rdram-%d.bin" % (a.o, next_dump), "wb") as f:
                    f.write(g.mem(0x80000000, 0x800000))
                print("volcado %d a los %.0f s (%.1f s), pc %08x" % (next_dump, time.time() - t0, time.time() - td, pc), flush=True)
                next_dump += 1
            hist[pc] += 1
            n += 1
            g.cont()
            if a.reconectar:
                time.sleep(0.01)
                g.close()
                g = None
        except (OSError, EOFError, ValueError) as e:
            errors += 1
            if g is not None:
                g.close()
            g = None
            if errors > 50:
                print("demasiados errores (%s): se guarda lo muestreado" % e, flush=True)
                break
            time.sleep(0.2)
    if g is not None:
        try:
            g.cont()
        except OSError:
            pass
        g.close()
    el = time.time() - t0
    with open(a.o + "-pcs.tsv", "w") as f:
        f.write("# pc cuenta (muestras de ares, %d en %.1f s, %d errores)\n" % (n, el, errors))
        for pc, c in sorted(hist.items(), key=lambda kv: -kv[1]):
            f.write("%08x %d\n" % (pc, c))
    with open(a.o + "-meta.txt", "w") as f:
        f.write("muestras=%d segundos=%.1f errores=%d pcs_distintos=%d latencia_ms=%.1f\n" % (n, el, errors, len(hist), 1000 * lat / max(n, 1)))
    print("%d muestras en %.1f s (%.0f/s), %d PC distintos, %d errores, latencia media %.1f ms → %s-pcs.tsv" %
          (n, el, n / el, len(hist), errors, 1000 * lat / max(n, 1), a.o))


if __name__ == "__main__":
    main()
