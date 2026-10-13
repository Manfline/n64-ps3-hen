#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Revisa el ensamblador NV40 que Cg le da a cgcomp (`cgcomp -d`, archivo
<salida>.dump) en busca del choque de la salida con R0.

Cg trata `oCol` (result.color) como un registro aparte, pero el ensamblador
de cgcomp lo escribe en R0 (R0 son también H0 y H1), que Cg usa como
temporal. Si después de escribir un componente de oCol una instrucción lee
ese componente de R0, o escribe en él como temporal, el programa hace otra
cosa en el RSX: en la pasada de puntos del CRT, `oCol.x = A` pisaba la
energía que la instrucción siguiente leía de R0.x (2026-10-13). Lo seguro
es armar el color final en un temporal y escribirlo con una sola
instrucción al final.

  tools/cg-salida.py programa.fpo.dump
Sale con error (y dice dónde) si hay choque.

  tools/cg-salida.py programa.fpo.dump --corrige programa.asm
Escribe el ensamblador listo para `cgcomp -a`: si hay choque, las
escrituras de oCol van a un temporal nuevo y un `MOVR oCol` al final lo
copia (una instrucción más). Además corrige otro error del ensamblador de
cgcomp: un valor absoluto con swizzle de un componente (`-|R0.z|`) lo
codifica como `.zxxx` en vez de `.zzzz` (visto en el GLSL que registra
RPCS3 y en el .fpo); se escribe con los cuatro (`-|R0.zzzz|`).
"""
import re
import sys

ALL = "xyzw"
# Fuentes escalares: leen solo el primer componente de su swizzle.
SCALAR = {"RCP", "RSQ", "EX2", "LG2", "COS", "SIN", "POW", "DIV"}
DOT = {"DP2": 2, "DP3": 3, "DP4": 4, "DP2A": 2}


def mask_of(op):
    m = re.match(r"^-?\|?(\w+)(?:\[[^\]]*\])?(?:\.([xyzw]{1,4}))?", op.strip())
    if not m:
        return None, ""
    return m.group(1), m.group(2) or ""


def check(path):
    insns = []
    for l in open(path):
        l = l.split("#")[0].strip().rstrip(";")
        m = re.match(r"^([A-Z0-9_]+)\s+(.*)$", l)
        if not m or m.group(1) in ("TEMP", "OUTPUT", "PARAM", "OPTION", "SHORT", "END", "ATTRIB"):
            continue
        insns.append((m.group(1), [x.strip() for x in m.group(2).split(",")]))
    written = set()
    bad = []
    for n, (opc, ops) in enumerate(insns, 1):
        base = re.sub(r"(_SAT|R|H|X|C)+$", "", opc)
        dreg, dmask = mask_of(ops[0])
        dmask = dmask or ALL
        if dreg is None:
            continue
        reads = set()
        for src in ops[1:]:
            reg, sw = mask_of(src)
            if reg not in ("R0", "H0", "H1"):
                continue
            if reg != "R0":
                reads.add("*")
                continue
            sw = sw or ALL
            if len(sw) == 1:
                sw = sw * 4
            if base in SCALAR:
                reads.add(sw[0])
            elif base in DOT:
                reads.update(sw[:DOT[base]])
            elif base == "TEX":
                reads.update(sw[:3])
            else:
                for c in dmask:
                    reads.add(sw[ALL.index(c)])
        if written and ("*" in reads or reads & written):
            bad.append((n, opc, ops, "lee"))
        if dreg in ("H0", "H1") and written:
            bad.append((n, opc, ops, "escribe"))
        if dreg == "R0" and set(dmask) & written:
            bad.append((n, opc, ops, "escribe"))
        if dreg == "oCol":
            written |= set(dmask)
    return bad


def fix(path, out):
    text = open(path).read()
    used = [int(n) for n in re.findall(r"\bR(\d+)\b", text)]
    tmp = "R%d" % (max(used + [0]) + 1)
    lines = []
    for l in text.splitlines():
        code = l.split("#")[0]
        m = re.match(r"^(\s*[A-Z0-9_]+\s+)oCol(\.[xyzw]{1,4})?(\s*,.*)$", code)
        if m and not code.strip().startswith("OUTPUT"):
            l = m.group(1) + tmp + (m.group(2) or "") + m.group(3)
        elif code.strip() == "END":
            lines.append("MOVR  oCol, %s;" % tmp)
        elif code.strip().startswith("TEMP RC"):
            lines.append("TEMP %s;" % tmp)
        lines.append(l)
    open(out, "w").write("\n".join(lines) + "\n")


ABS1 = re.compile(r"\|(\s*-?)([A-Za-z]\w*(?:\[\d+\])?)\.([xyzw])\|")


def fix_abs(path):
    """|R.c| → |R.cccc| (cgcomp completa el swizzle con x)."""
    text = open(path).read()
    fixed = ABS1.sub(lambda m: "|%s%s.%s|" % (m.group(1), m.group(2), m.group(3) * 4), text)
    open(path, "w").write(fixed)


def main():
    path = sys.argv[1]
    bad = check(path)
    if "--corrige" in sys.argv:
        out = sys.argv[sys.argv.index("--corrige") + 1]
        if bad:
            fix(path, out)
            if check(out):
                print("%s: no se pudo corregir el choque de oCol con R0" % path, file=sys.stderr)
                sys.exit(1)
        else:
            open(out, "w").write(open(path).read())
        fix_abs(out)
        return
    if bad:
        for n, opc, ops, what in bad:
            print("%s: instrucción %d %s en R0 un componente de oCol ya escrito: %s %s"
                  % (path, n, what, opc, ", ".join(ops)), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
