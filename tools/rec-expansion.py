#!/usr/bin/env python3
"""Expansión del código generado: palabras de PPC por instrucción MIPS.

Lee un log de salsa64-run con SALSA64_REC_DUMP=1 (una línea «bloque <pc> …
<N> palabras (<H> calientes): …» por bloque compilado) y, si está, el
informe final del perfil por bloque (SALSA64_REC_PROF=N, con
SALSA64_REC_PROF_TOP para listar más bloques: «bloque <pc> <e> ejecuciones
… <n> instr. …»), y da:

  - para todo el volcado: bloques, palabras totales, calientes y frías, y
    la media por bloque;
  - pesado por ejecuciones (los bloques del informe, por pc; un pc
    compilado varias veces toma su último volcado): palabras calientes y
    frías por instrucción MIPS, que es lo que toca la caché de
    instrucciones del PPE por instrucción emulada;
  - los bloques más ejecutados con sus palabras por instrucción.

Con dos logs compara (antes → después). `--counter N` descuenta N palabras
calientes por bloque (el contador del perfil por bloque que el host emite
para contar ejecuciones y la PS3 no lleva: 8 con SALSA64_REC_PROF). Uso:
  tools/rec-expansion.py build/exp/base.log [build/exp/nuevo.log] [--top N] [--counter 8]
"""
import re
import sys

DUMP = re.compile(r"^bloque ([0-9a-f]{16})( \(recortado\))? entrada \d+, (\d+) palabras(?: \((\d+) calientes\))?:")
PROF = re.compile(r"^bloque ([0-9a-f]{8}) (\d+) ejecuciones \(([0-9.]+) %\) (\d+) instr\. (\d+) cop1 (\d+) mem palabras:((?: [0-9a-f]{8})*)")


def read(path):
    blocks = {}   # pc (32 bits) -> (palabras, calientes) del último volcado
    prof = {}     # pc -> (ejecuciones, instrucciones, cop1, mem)
    total = hot = n = 0
    with open(path, errors="replace") as f:
        for line in f:
            m = DUMP.match(line)
            if m:
                if m.group(2):
                    continue  # recortado (modo estricto): no cuenta
                words, h = int(m.group(3)), int(m.group(4) or 0)
                pc = int(m.group(1), 16) & 0xFFFFFFFF
                blocks[pc] = (words, h)
                total += words
                hot += h
                n += 1
                continue
            m = PROF.match(line)
            if m:
                pc = int(m.group(1), 16)
                prof[pc] = (int(m.group(2)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
    return dict(blocks=blocks, prof=prof, total=total, hot=hot, n=n)


COUNTER = 0  # palabras calientes por bloque que se descuentan (--counter)


def weighted(d):
    """Palabras (calientes, frías) y ejecuciones×instrucciones pesadas por
    las ejecuciones del perfil; también cuántos bloques del perfil tienen
    volcado."""
    wh = wc = wi = 0
    found = 0
    for pc, (hits, insns, _, _) in d["prof"].items():
        b = d["blocks"].get(pc)
        if not b:
            continue
        found += 1
        words, h = b
        wh += hits * (h - COUNTER)
        wc += hits * (words - h)
        wi += hits * insns
    return wh, wc, wi, found


def report(d, top):
    n, total, hot = d["n"], d["total"], d["hot"]
    print(f"volcado: {n} bloques, {total} palabras ({total * 4 / 1048576:.1f} MB), "
          f"{hot} calientes ({100.0 * hot / total if total else 0:.0f} %), "
          f"media {total / n if n else 0:.0f} por bloque ({hot / n if n else 0:.0f} calientes)")
    if not d["prof"]:
        print("sin informe del perfil (SALSA64_REC_PROF=N y SALSA64_REC_PROF_TOP=M): no se pesa por ejecuciones")
        return
    wh, wc, wi, found = weighted(d)
    hits = sum(p[0] for p in d["prof"].values())
    print(f"perfil: {len(d['prof'])} bloques listados ({found} con volcado), {hits} ejecuciones; "
          f"pesado por ejecuciones: {(wh + wc) / wi if wi else 0:.2f} palabras por instrucción MIPS "
          f"({wh / wi if wi else 0:.2f} calientes, {wc / wi if wi else 0:.2f} frías)")
    rows = sorted(d["prof"].items(), key=lambda kv: -kv[1][0])[:top]
    th = tc = ti = 0
    for pc, (hits, insns, cop1, mem) in rows:
        b = d["blocks"].get(pc)
        if not b:
            continue
        words, h = b
        th += h - COUNTER
        tc += words - h
        ti += insns
    print(f"los {len(rows)} más ejecutados: {ti} instrucciones en {th + tc} palabras, "
          f"{(th + tc) / ti if ti else 0:.1f} por instrucción ({th / ti if ti else 0:.1f} calientes, {tc / ti if ti else 0:.1f} frías)")
    print("  pc        ejec.      instr cop1 mem  palabras calientes frías  por instr.")
    for pc, (hits, insns, cop1, mem) in rows:
        b = d["blocks"].get(pc)
        if not b:
            print(f"  {pc:08x} {hits:10d} {insns:6d} {cop1:4d} {mem:3d}  (sin volcado)")
            continue
        words, h = b
        print(f"  {pc:08x} {hits:10d} {insns:6d} {cop1:4d} {mem:3d}  {words:8d} {h:9d} {words - h:5d}  "
              f"{words / insns:5.1f} ({h / insns:4.1f} + {(words - h) / insns:4.1f})")


def compare(a, b):
    wa = weighted(a)
    wb = weighted(b)
    def per(w):
        wh, wc, wi, _ = w
        return (wh / wi if wi else 0, wc / wi if wi else 0)
    ha, ca = per(wa)
    hb, cb = per(wb)
    print(f"antes → después, pesado por ejecuciones: calientes {ha:.2f} → {hb:.2f} ({100 * (hb - ha) / ha if ha else 0:+.0f} %), "
          f"frías {ca:.2f} → {cb:.2f} ({100 * (cb - ca) / ca if ca else 0:+.0f} %), "
          f"total {ha + ca:.2f} → {hb + cb:.2f} ({100 * (hb + cb - ha - ca) / (ha + ca) if ha + ca else 0:+.0f} %)")
    print(f"volcado: {a['total'] * 4 / 1048576:.1f} MB → {b['total'] * 4 / 1048576:.1f} MB "
          f"({a['n']} → {b['n']} bloques; media {a['total'] / a['n'] if a['n'] else 0:.0f} → {b['total'] / b['n'] if b['n'] else 0:.0f} por bloque)")
    # Los mismos bloques (por pc) en los dos: cuánto cambia cada uno.
    common = [pc for pc in a["blocks"] if pc in b["blocks"] and pc in a["prof"]]
    common.sort(key=lambda pc: -a["prof"][pc][0])
    print("  pc        ejec.(antes) instr  palabras antes → después  (calientes antes → después)")
    for pc in common[:25]:
        wa_, ha_ = a["blocks"][pc]
        wb_, hb_ = b["blocks"][pc]
        insns = a["prof"][pc][1]
        print(f"  {pc:08x} {a['prof'][pc][0]:11d} {insns:5d}  {wa_:5d} → {wb_:5d} ({100 * (wb_ - wa_) / wa_:+.0f} %)  ({ha_} → {hb_})")


def main(argv):
    global COUNTER
    top = 25
    paths = []
    k = 1
    while k < len(argv):
        if argv[k] == "--top":
            top = int(argv[k + 1])
            k += 2
            continue
        if argv[k] == "--counter":
            COUNTER = int(argv[k + 1])
            k += 2
            continue
        paths.append(argv[k])
        k += 1
    if not paths or len(paths) > 2:
        print(__doc__)
        return 2
    a = read(paths[0])
    print(f"== {paths[0]}")
    report(a, top)
    if len(paths) == 2:
        b = read(paths[1])
        print(f"== {paths[1]}")
        report(b, top)
        print("== comparación")
        compare(a, b)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
