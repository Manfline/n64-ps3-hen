#!/usr/bin/env python3
"""Resume el log del frontend de la PS3 (salsa64.log) con el perfil del
recompilador: por cada ventana de estadísticas (5 s) imprime campos/s, ms de
emulación por campo y el desglose de «perfil JIT por campo» (nativo,
llamadas a C, intérprete, eventos, compilar, ktlb, despachador), y al final
el promedio de las ventanas que se pidan.

    tools/jit-perfil.py build/ps3-logs/salsa64-jit.log           # todas las ventanas
    tools/jit-perfil.py --desde 30 --hasta 200 salsa64.log       # ventanas 30–200 (la presa)
"""
import re
import sys


def main():
    args = sys.argv[1:]
    lo, hi = 0, 10**9
    while args and args[0].startswith("--"):
        if args[0] == "--desde":
            lo = int(args[1])
        elif args[0] == "--hasta":
            hi = int(args[1])
        args = args[2:]
    if not args:
        print(__doc__)
        return
    stats = re.compile(r"(\s|^)([\d.]+) campos/s \((\d+) % de \w+\), ([\d.]+) MIPS, (\d+) % en bucles de espera; "
                       r"emulación ([\d.]+) ms por campo")
    prof = re.compile(r"perfil JIT por campo: emulación ([\d.]+) ms = nativo ([\d.]+) \((\d+) %\), llamadas a C ([\d.]+) "
                      r"\((\d+) %\), intérprete ([\d.]+) \((\d+) %\), eventos ([\d.]+) \((\d+) %\), compilar ([\d.]+), "
                      r"vtlb ([\d.]+), bucles de espera ([\d.]+), despachador ([\d.]+)")
    windows = []
    cur = None
    for line in open(args[0], errors="replace"):
        m = stats.search(line)
        if m:
            cur = {"fps": float(m.group(2)), "pct": int(m.group(3)), "mips": float(m.group(4)),
                   "idle": int(m.group(5)), "emu": float(m.group(6))}
            windows.append(cur)
            continue
        m = prof.search(line)
        if m and cur is not None:
            keys = ["total", "nativo", "nativo%", "c", "c%", "interp", "interp%", "ev", "ev%", "compilar", "ktlb",
                    "idle_ms", "desp"]
            for k, v in zip(keys, m.groups()):
                cur[k] = float(v)
    print("ventana  campos/s   %   MIPS  espera%  emu ms | nativo   C   interp  eventos  compilar  ktlb  desp (ms por campo)")
    sel = []
    for i, w in enumerate(windows):
        if not (lo <= i <= hi):
            continue
        sel.append(w)
        base = "%7d  %7.1f %3d  %5.1f  %6d  %6.2f" % (i, w["fps"], w["pct"], w["mips"], w["idle"], w["emu"])
        if "nativo" in w:
            base += " | %6.2f %5.2f %6.2f %7.2f %8.2f %5.2f %5.2f" % (w["nativo"], w["c"], w["interp"], w["ev"],
                                                                     w["compilar"], w["ktlb"], w["desp"])
        print(base)
    with_prof = [w for w in sel if "nativo" in w]
    if sel:
        n = len(sel)
        print("promedio de %d ventanas: %.1f campos/s, %.1f MIPS, %d %% en espera, emulación %.2f ms por campo" %
              (n, sum(w["fps"] for w in sel) / n, sum(w["mips"] for w in sel) / n,
               sum(w["idle"] for w in sel) // n, sum(w["emu"] for w in sel) / n))
    if with_prof:
        n = len(with_prof)
        avg = {k: sum(w[k] for w in with_prof) / n for k in ["nativo", "c", "interp", "ev", "compilar", "ktlb", "desp"]}
        print("perfil medio: nativo %.2f, llamadas a C %.2f, intérprete %.2f, eventos %.2f, compilar %.2f, ktlb %.2f, "
              "despachador %.2f ms por campo" % (avg["nativo"], avg["c"], avg["interp"], avg["ev"], avg["compilar"],
                                                avg["ktlb"], avg["desp"]))


if __name__ == "__main__":
    main()
