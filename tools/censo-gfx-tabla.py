#!/usr/bin/env python3
"""Tablas de docs/gfx-censo.md a partir de los informes de tools/censo-gfx.sh.

    tools/censo-gfx-tabla.py [carpeta]      (por omisión build/censo-gfx)

Lee NOMBRE.tsv (una línea por tarea: SALSA64_GCHK_LOG) y NOMBRE.log (el
informe de salsa64-run --lle-check-gfx) de cada juego y escribe en la salida
estándar, en Markdown: la tabla por juego, la tabla por microcódigo, los
comandos que la HLE no implementa y la peor tarea de cada juego.

Una tarea es «válida» si el microcódigo real dibujó algo comparable: si la
HLE dibuja al menos el 2 % de la pantalla y el LLE menos de la décima parte
de eso, el camino LLE → RDP falló en esa tarea (microcódigos que el
intérprete del RSP o dpc.c no resuelven todavía) y la comparación no dice
nada de la HLE; esas tareas se cuentan aparte.
"""
import collections
import glob
import os
import re
import sys

ruta = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "censo-gfx")

AREA = 320 * 240
GRANDE = AREA * 30000 // 1000000  # 3 % del área (gfxcheck.h, GCHK_BIG_BAD_PPM)

OPS = re.compile(r"^gchk:   comandos sin implementar en (la HLE|el RDP del LLE): (.*)$")
KEYLINE = re.compile(r"^gchk: \[(.*?)\] tareas \d+, con HLE")
EST = re.compile(r"^gchk:   estado final del RDP distinto en: (.*)$")


def corto(clave):
    """Familia y versión del microcódigo, sin los blancos del texto."""
    clave = re.sub(r"\s+", " ", clave)
    clave = clave.replace("RSP Gfx ucode ", "").replace("Yoshitaka Yasumoto", "").replace("Nintendo.", "")
    return re.sub(r" +", " ", clave).strip(" .")


juegos = collections.OrderedDict()   # juego -> {clave -> Counter}
ejemplos = {}                        # (juego, clave) -> tarea válida con más píxeles distintos
ops_h = collections.defaultdict(collections.Counter)
ops_l = collections.defaultdict(collections.Counter)
estado = collections.defaultdict(collections.Counter)

for tsv in sorted(glob.glob(os.path.join(ruta, "*.tsv"))):
    nombre = os.path.basename(tsv)[:-4]
    por = collections.OrderedDict()
    with open(tsv, encoding="utf-8", errors="replace") as f:
        for linea in f:
            c = linea.rstrip("\n").split("\t")
            if len(c) < 8:
                continue
            clave = corto(c[2])
            t = por.setdefault(clave, collections.Counter())
            t["tareas"] += 1
            clase = c[3]
            if clase == "sin HLE":
                t["sinhle"] += 1
                continue
            t["conhle"] += 1
            drawn_l, drawn_h = int(c[6]), int(c[7])
            nbad = int(c[16]) if len(c) > 16 and c[16].isdigit() else 0
            if drawn_h * 50 > AREA and drawn_l * 10 < drawn_h:
                t["vacia"] += 1
                continue
            t["validas"] += 1
            t[{"idéntica": "ident", "menor": "menor", "distinta": "dist"}[clase]] += 1
            if clase == "distinta":
                if nbad > GRANDE:
                    t["grandes"] += 1
                t["tipo:" + c[12]] += 1
                k = (nombre, clave)
                if k not in ejemplos or int(c[4]) > ejemplos[k][4]:
                    ejemplos[k] = (c[0], c[1], c[12], c[13], int(c[4]), drawn_l, drawn_h)
    if por:
        juegos[nombre] = por
    log = os.path.join(ruta, nombre + ".log")
    cur = None
    if os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            for linea in f:
                linea = linea.rstrip("\n")
                m = KEYLINE.match(linea)
                if m:
                    cur = corto(m.group(1))
                    continue
                if cur is None:
                    continue
                m = OPS.match(linea)
                if m:
                    dst = ops_h if m.group(1) == "la HLE" else ops_l
                    for mm in re.finditer(r"0x([0-9A-F]{2}) ×(\d+)", m.group(2)):
                        dst[cur][mm.group(1)] += int(mm.group(2))
                m = EST.match(linea)
                if m:
                    for mm in re.finditer(r"(\S+) ×(\d+)", m.group(1)):
                        estado[cur][mm.group(1).rstrip(",")] += int(mm.group(2))


def pct(a, b):
    return "%.1f %%" % (100.0 * a / b) if b else "—"


def fila(t):
    v = t["validas"]
    return "%d | %d | %d | %d (%s) | %d (%s) | %d (%d) (%s)" % (
        t["tareas"], t["conhle"], t["vacia"], t["ident"], pct(t["ident"], v), t["menor"], pct(t["menor"], v), t["dist"],
        t["grandes"], pct(t["dist"], v))


ENC = "Tareas | Con HLE | LLE vacía (excluidas) | Idénticas | Menores | Distintas (grandes) | Sin HLE"
print("## Por juego\n")
print("| Juego | " + ENC + " | Microcódigos |")
print("|---|---:|---:|---:|---:|---:|---:|---:|---|")
total = collections.Counter()
for nombre, por in juegos.items():
    t = collections.Counter()
    for v in por.values():
        t.update(v)
    total.update(t)
    uc = "; ".join(sorted({k.split(":")[0] for k in por}))
    print("| %s | %s | %d | %s |" % (nombre, fila(t), t["sinhle"], uc))
print("| **Total** | %s | %d | |" % (fila(total), total["sinhle"]))
print("\nLos porcentajes son sobre las tareas válidas (con HLE y con el LLE dibujando).\n")

print("## Por microcódigo\n")
print("| Microcódigo | Juegos | " + ENC + " |")
print("|---|---|---:|---:|---:|---:|---:|---:|---:|")
poruc = collections.OrderedDict()
for nombre, por in juegos.items():
    for clave, t in por.items():
        poruc.setdefault(clave, []).append((nombre, t))
for clave, lista in sorted(poruc.items()):
    t = collections.Counter()
    for _, v in lista:
        t.update(v)
    print("| %s | %s | %s | %d |" % (clave.replace("|", "/"), ", ".join(sorted({n for n, _ in lista})), fila(t), t["sinhle"]))

print("\n## Tipo de las tareas distintas (válidas)\n")
print("| Microcódigo | La HLE no dibuja | La HLE dibuja de más | Otro color |")
print("|---|---:|---:|---:|")
for clave, lista in sorted(poruc.items()):
    t = collections.Counter()
    for _, v in lista:
        t.update(v)
    if t["dist"]:
        print("| %s | %d | %d | %d |" % (clave, t["tipo:la HLE no dibuja"], t["tipo:la HLE dibuja de más"], t["tipo:otro color"]))

print("\n## Comandos sin implementar, por microcódigo\n")
vacio = True
for clave in sorted(set(ops_h) | set(ops_l)):
    for etiqueta, d in (("HLE", ops_h), ("RDP del LLE", ops_l)):
        if d[clave]:
            vacio = False
            print("- %s, %s: %s" % (clave, etiqueta, ", ".join("0x%s ×%d" % (o, n) for o, n in d[clave].most_common())))
if vacio:
    print("Ninguno: la HLE contó 0 comandos sin implementar (`unknown_cmds`) en todas las escenas.")

print("\n## Registros del RDP que difieren al final de la tarea\n")
for clave, c in sorted(estado.items()):
    print("- %s: %s" % (clave, ", ".join("%s ×%d" % kv for kv in c.most_common(8))))

print("\n## Peor tarea válida distinta de cada juego y microcódigo\n")
print("| Juego | Microcódigo | Tipo | Tarea (campo) | Píxeles distintos | Dibujados LLE / HLE | Caja |")
print("|---|---|---|---|---:|---|---|")
for (nombre, clave), (tarea, campo, tipo, caja, nd, dl, dh) in sorted(ejemplos.items()):
    print("| %s | %s | %s | %s (%s) | %d | %d / %d | (%s) |" % (nombre, clave.split(":")[0], tipo, tarea, campo, nd, dl, dh, caja))
