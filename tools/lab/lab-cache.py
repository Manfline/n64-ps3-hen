#!/usr/bin/env python3
"""Modelo de las cachés de instrucciones del PPE sobre una traza de entradas a bloques
(docs/lab-recompilador.md, «Caché de instrucciones»).

Entradas: el .lab y el .visits que escribe salsa64-run-lab con SALSA64_LAB_TRACE
(lab-trace.c): una palabra por entrada a un bloque, con la última instrucción MIPS que
corrió en esa visita. De cada visita se sacan las líneas de 128 bytes que el PPE lee: el
cuerpo caliente del bloque hasta donde llegó (las secciones frías no se leen salvo la
línea que comparten con el final del cuerpo) y las líneas del preámbulo de las rutinas
compartidas que ese cuerpo llama (FPU, ktlb, exit_jr…).

Modelo (docs/cache-ppe.md, sección 1): L1I de 32 KB, 2 vías, líneas de 128 bytes, LRU; L2
de 512 KB, 8 vías (con --l2-kb se achica para dejar sitio a los datos); ERAT de
instrucciones de 64 entradas, 2 vías, páginas de 4 KB. Costos: fallo de L1 que acierta
en L2 36 ciclos, fallo de L2 ~380, fallo de ERAT 24. Precarga: con --prefetch 1, un
fallo de L1 trae también la línea siguiente sin costo (cola IPFQ de dos líneas), que es
el techo de lo que puede hacer; sin ella, el piso.

Colocaciones (qué dirección de la caché de código ocupa cada bloque):
  lineal    como el recompilador hoy: cada bloque (caliente + frío) alineado a 128 bytes,
            en el orden de su primera visita
  separada  los cuerpos calientes juntos (alineados a 32 bytes) y las secciones frías en
            otra zona: lo que pide docs/cache-ppe.md 3.1.1
  peso      los cuerpos calientes juntos y ordenados por entradas: el mejor caso de una
            zona caliente construida con el perfil
"""
import argparse
import array
import bisect
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lab_data as D    # noqa: E402
import lab_metrics as M  # noqa: E402

PRE_LINES = 256          # el preámbulo ocupa las primeras líneas (palabras 0–4095)
CODE_BASE = 256          # línea donde empieza el código de los bloques


class BlockGeo:
    """Lo que la simulación necesita de un bloque, calculado una vez."""
    __slots__ = ("hot", "total", "unit_ends", "unit_k", "words_upto", "pre_upto", "n")


def geometry(lab, rt, blk):
    g = BlockGeo()
    g.hot, g.total, g.n = blk.hot, blk.total, blk.n
    units = M.mnem_units(blk)
    w = M.Walker(lab, rt, blk)
    w.run()
    g.unit_k = [u[0] for u in units]
    g.words_upto = [u[3] for u in units]   # palabras calientes hasta el final de la unidad
    cum = set()
    pre = []
    for ui in range(len(units)):
        cum = cum | w.rtn_lines.get(ui, set())
        pre.append(tuple(sorted(cum)))
    g.pre_upto = pre
    return g


def place(lab, order, geos, policy):
    """Palabra de la caché (en la zona de código) del cuerpo caliente de cada bloque."""
    addr = {}
    if policy == "lineal":
        cur = 0
        for b in order:
            addr[b] = cur
            cur += ((geos[b].total + 31) // 32) * 32
    elif policy in ("separada", "peso"):
        seq = list(order)
        if policy == "peso":
            seq.sort(key=lambda b: -lab.blocks[b].count)
        cur = 0
        for b in seq:
            addr[b] = cur
            cur += ((geos[b].hot + 7) // 8) * 8
    else:
        raise SystemExit("colocación desconocida: " + policy)
    return addr


def simulate(lab, geos, visits, addr, a):
    sets1 = a.l1_kb * 1024 // 128 // 2
    l1 = [[-1, -1] for _ in range(sets1)]
    sets2 = a.l2_kb * 1024 // 128 // 8
    l2 = [[] for _ in range(sets2)]
    erat = [[-1, -1] for _ in range(32)]
    l1_miss = l2_miss = erat_miss = acc = insns = 0
    counting = False
    warm = int(len(visits) * a.warmup)
    pf = a.prefetch
    cb = CODE_BASE
    for vi, rec in enumerate(visits):
        if vi == warm:
            counting = True
            l1_miss = l2_miss = erat_miss = acc = insns = 0
        b = rec & 0xFFFFFF
        k = rec >> 24
        g = geos[b]
        # unidad que contiene la instrucción k (la última que corrió)
        ui = bisect.bisect_right(g.unit_k, k) - 1
        if ui < 0:
            ui = 0
        end_words = g.words_upto[ui] if k < g.n else g.hot
        if k >= g.unit_k[-1]:
            end_words = g.hot
        base = addr[b]
        first = cb + (base >> 5)
        last = cb + ((base + max(end_words, 1) - 1) >> 5)
        lines = list(range(first, last + 1))
        if g.pre_upto[ui]:
            lines += g.pre_upto[ui]
        insns += k + 1
        for ln in lines:
            acc += 1
            # ERAT (páginas de 4 KB = 32 líneas)
            pg = ln >> 5
            e = erat[pg & 31]
            if e[0] == pg:
                pass
            elif e[1] == pg:
                e[0], e[1] = e[1], e[0]
            else:
                if counting:
                    erat_miss += 1
                e[1] = e[0]
                e[0] = pg
            s = l1[ln & (sets1 - 1)] if sets1 & (sets1 - 1) == 0 else l1[ln % sets1]
            if s[0] == ln:
                continue
            if s[1] == ln:
                s[0], s[1] = s[1], s[0]
                continue
            # fallo de L1
            if counting:
                l1_miss += 1
            t = l2[ln % sets2]
            if ln in t:
                t.remove(ln)
                t.insert(0, ln)
            else:
                if counting:
                    l2_miss += 1
                t.insert(0, ln)
                if len(t) > 8:
                    t.pop()
            s[1] = s[0]
            s[0] = ln
            if pf:  # la línea siguiente llega con ella (sin costo)
                n1 = ln + 1
                s2 = l1[n1 % sets1]
                if s2[0] != n1 and s2[1] != n1:
                    s2[1] = s2[0]
                    s2[0] = n1
                    t2 = l2[n1 % sets2]
                    if n1 in t2:
                        t2.remove(n1)
                        t2.insert(0, n1)
                    else:
                        t2.insert(0, n1)
                        if len(t2) > 8:
                            t2.pop()
    return dict(insns=insns, acc=acc, l1=l1_miss, l2=l2_miss, erat=erat_miss)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lab")
    ap.add_argument("visits")
    ap.add_argument("--max-visits", type=int, default=8000000)
    ap.add_argument("--warmup", type=float, default=0.3, help="fracción inicial de visitas que solo calienta las cachés")
    ap.add_argument("--placement", default="lineal,separada,peso")
    ap.add_argument("--prefetch", type=int, default=1)
    ap.add_argument("--l1-kb", type=int, default=32)
    ap.add_argument("--l2-kb", type=int, default=512)
    ap.add_argument("--c-l2", type=float, default=36.0)
    ap.add_argument("--c-mem", type=float, default=380.0)
    ap.add_argument("--c-erat", type=float, default=24.0)
    a = ap.parse_args()

    lab = D.parse(a.lab)
    rt = M.Rtn(lab)
    raw = array.array("I")
    with open(a.visits, "rb") as f:
        raw.fromfile(f, min(a.max_visits, os.path.getsize(a.visits) // 4))
    visits = raw
    print("%s: %d bloques, %d entradas en la traza (de %s); L1I %d KB, L2 %d KB, precarga %s" %
          (os.path.basename(a.lab), len(lab.blocks), len(visits), a.visits, a.l1_kb, a.l2_kb, "sí" if a.prefetch else "no"))
    order = []
    seen = set()
    for rec in visits:
        b = rec & 0xFFFFFF
        if b not in seen:
            seen.add(b)
            order.append(b)
    geos = {b: geometry(lab, rt, lab.blocks[b]) for b in order}
    hot_kb = sum(((g.hot + 7) // 8) * 32 for g in geos.values()) / 1024.0
    tot_kb = sum(((g.total + 31) // 32) * 128 for g in geos.values()) / 1024.0
    print("bloques distintos en la traza: %d; cuerpos calientes %.0f KB, con lo frío %.0f KB" % (len(order), hot_kb, tot_kb))
    rows = []
    for pol in a.placement.split(","):
        addr = place(lab, order, geos, pol)
        r = simulate(lab, geos, visits, addr, a)
        ins = max(r["insns"], 1)
        stall = (r["l1"] - r["l2"]) * a.c_l2 + r["l2"] * (a.c_l2 + a.c_mem) + r["erat"] * a.c_erat
        rows.append((pol, "%.1f" % (1000.0 * r["acc"] / ins), "%.2f" % (1000.0 * r["l1"] / ins), "%.2f" % (1000.0 * r["l2"] / ins),
                     "%.2f" % (1000.0 * r["erat"] / ins), "%.2f" % (stall / ins)))
    print()
    heads = ["colocación", "líneas/1000 instr", "fallos L1I/1000", "fallos L2/1000", "fallos ERAT/1000", "espera (ciclos/instr)"]
    widths = [max(len(str(r[i])) for r in rows + [tuple(heads)]) for i in range(len(heads))]
    fmt = "  ".join(("%%-%ds" if i == 0 else "%%%ds") % widths[i] for i in range(len(heads)))
    print(fmt % tuple(heads))
    for r in rows:
        print(fmt % r)


if __name__ == "__main__":
    main()
