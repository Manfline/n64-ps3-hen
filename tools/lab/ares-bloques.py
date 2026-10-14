#!/usr/bin/env python3
"""De las muestras de PC de ares (tools/lab/ares-perfil.py) a los bloques que rec-lab traduce.

Un PC muestreado cae en medio de un bloque; el recompilador compila por bloques que
empiezan en un destino de salto o justo después de un salto y su delay slot. Con el código
del volcado de la RDRAM (y de la ROM, si el juego la mapea con la TLB) se buscan esos
«líderes» alrededor de cada muestra y la muestra se suma al último líder que no la pasa. El
peso de un bloque, en «entradas», es sus muestras entre las instrucciones que se estima que
ejecuta cada vez (hasta el primer salto): el muestreo es por tiempo, no por entradas. Lo
que sale es la entrada de `rec-lab --pcs` (pc y entradas relativas ×1000).

Dos sesgos conocidos (docs/lab-recompilador.md): ares no se detiene dentro de un delay slot
(en 3 245 muestras de Perfect Dark, 3 caían ahí; con 1 de cada ~6 instrucciones lo esperable
son ~500), así que su tiempo se reparte entre el salto y el destino; y los bucles de espera
de libultra (el CPU esperando) pesan lo que dura la espera en ares, mientras que el
intérprete y el recompilador los saltan (--sin-espera quita las muestras de bucles de menos
de 12 instrucciones que vuelven sobre sí mismos).

    tools/lab/ares-bloques.py muestras-pcs.tsv volcado.bin --map 7f000000:220000 --map 70000000:0 \\
        -o pcs.tsv
    build/lab/bin/rec-lab --rom juego.z64 --rdram volcado.bin --map … --pcs pcs.tsv -o juego.lab
"""
import argparse
import collections
import struct


class Code:
    def __init__(self, ram, maps, rom=None, romwin=None):
        self.ram, self.maps, self.rom, self.romwin = ram, maps, rom, romwin

    def pa(self, va):
        if 0x80000000 <= va < 0xC0000000:
            return va & 0x1FFFFFFF
        for (v, p, ln) in self.maps:
            if v <= va < v + ln:
                return p + va - v
        return None

    def word(self, va):
        if self.romwin and self.romwin[0] <= va < self.romwin[0] + self.romwin[2]:
            off = self.romwin[1] + va - self.romwin[0]
            if self.rom and 0 <= off + 4 <= len(self.rom):
                return struct.unpack(">I", self.rom[off:off + 4])[0]
        p = self.pa(va)
        if p is None or p + 4 > len(self.ram):
            return None
        return struct.unpack(">I", self.ram[p:p + 4])[0]


def branch_target(w, pc):
    """Destino fijo de un salto del MIPS, o None si no lo es / es por registro."""
    op = w >> 26
    if op in (2, 3):
        return ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
    if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 16, 17, 18, 19)) or \
            (op == 0x11 and ((w >> 21) & 31) == 8):
        off = w & 0xFFFF
        off = off - 0x10000 if off & 0x8000 else off
        return (pc + 4 + (off << 2)) & 0xFFFFFFFF
    return None


def is_branch(w):
    op = w >> 26
    if op in (2, 3, 4, 5, 6, 7, 20, 21, 22, 23):
        return True
    if op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 16, 17, 18, 19):
        return True
    if op == 0x11 and ((w >> 21) & 31) == 8:
        return True
    return op == 0 and (w & 0x3F) in (8, 9)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("muestras")
    ap.add_argument("volcado")
    ap.add_argument("--map", action="append", default=[], help="VA:PA[:LARGO] hex: código mapeado por la TLB a la RDRAM")
    ap.add_argument("--rom")
    ap.add_argument("--romwin", help="VA:OFFSET[:LARGO] hex: código mapeado a la ROM (GoldenEye 7f000000:34b30)")
    ap.add_argument("--sin-espera", action="store_true", help="quita las muestras de bucles de espera de pocas instrucciones")
    ap.add_argument("--escala", type=float, default=1000.0)
    ap.add_argument("-o", required=True)
    a = ap.parse_args()

    ram = open(a.volcado, "rb").read()
    maps = []
    for m in a.map:
        t = [int(x, 16) for x in m.split(":")]
        maps.append((t[0], t[1], t[2] if len(t) > 2 else 0x400000))
    rom = open(a.rom, "rb").read() if a.rom else None
    rw = None
    if a.romwin:
        t = [int(x, 16) for x in a.romwin.split(":")]
        rw = (t[0], t[1], t[2] if len(t) > 2 else 0x1000000)
    code = Code(ram, maps, rom, rw)

    samples = collections.Counter()
    for line in open(a.muestras):
        if line[0] != "#" and line.strip():
            pc, c = line.split()[:2]
            samples[int(pc, 16)] += int(c)
    total = sum(samples.values())

    # líderes alrededor de las muestras: instrucciones que siguen a un salto + delay slot, y destinos fijos
    pages = {pc >> 12 for pc in samples} | {(pc >> 12) - 1 for pc in samples} | {(pc >> 12) + 1 for pc in samples}
    leaders = set()
    words = {}
    for pg in pages:
        for q in range(0, 4096, 4):
            va = (pg << 12) + q
            w = code.word(va)
            if w is None:
                continue
            words[va] = w
            if is_branch(w):
                leaders.add((va + 8) & 0xFFFFFFFF)
                t = branch_target(w, va)
                if t is not None:
                    leaders.add(t)
    sorted_leaders = sorted(leaders)
    import bisect

    def leader_of(pc):
        i = bisect.bisect_right(sorted_leaders, pc) - 1
        if i < 0:
            return max(pc - 64 * 4, 0)
        l = sorted_leaders[i]
        if pc - l > 64 * 4:   # un bloque no pasa de 64 instrucciones
            l = pc - ((pc - l) % (64 * 4))
        return l

    def est_len(l):
        n = 0
        va = l
        while n < 64:
            w = words.get(va)
            if w is None:
                break
            n += 1
            va += 4
            if is_branch(w):
                n += 1   # el delay slot
                break
        return max(n, 1)

    dropped = 0
    weight = collections.Counter()
    for pc, c in samples.items():
        if code.word(pc) is None:
            dropped += c
            continue
        l = leader_of(pc)
        weight[l] += c
    if a.sin_espera:
        keep = collections.Counter()
        for l, c in weight.items():
            # bucle de espera: el bloque termina en un salto cuyo destino es su propio inicio y es corto
            n = est_len(l)
            w = words.get(l + 4 * (n - 2)) if n >= 2 else None
            t = branch_target(w, l + 4 * (n - 2)) if w is not None else None
            if t == l and n <= 12:
                dropped += c
                continue
            keep[l] = c
        weight = keep

    with open(a.o, "w") as f:
        f.write("# pc entradas relativas (muestras de ares / instrucciones por entrada x %g); %d muestras, %d sin código o espera\n" %
                (a.escala, total, dropped))
        for l, c in sorted(weight.items(), key=lambda kv: -kv[1]):
            f.write("%08x %d\n" % (l, max(1, round(a.escala * c / est_len(l)))))
    print("%d muestras → %d bloques (%d muestras sin código o de bucles de espera) → %s" % (total, len(weight), dropped, a.o))


if __name__ == "__main__":
    main()
