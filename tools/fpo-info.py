#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resumen de un fragment program compilado por cgcomp (.fpo): instrucciones,
lecturas de textura, precisión de cada operación y las constantes (uniforms)
con cuántas copias tiene cada una en el microcódigo. El costo de un fragment
program en el RSX lo manda el número de instrucciones por píxel.

  tools/fpo-info.py build/.../programa.fpo [-v] [--sin-condiciones]

Con --sin-condiciones sale con error si alguna instrucción escribe los
códigos de condición o se ejecuta bajo una condición: Cg los usa para `?:`
y `sign`, y el ensamblador de cgcomp no los codifica bien (el resultado en
el RSX es otro; docs/rsx-notes.md).

Formato (docs/rsx-notes.md): instrucciones de 4 palabras de 32 bits con las
mitades de 16 bits intercambiadas; una fuente constante (tipo 2) agrega 16
bytes con su valor.
"""
import struct
import sys

OPS = {
    0x00: "NOP", 0x01: "MOV", 0x02: "MUL", 0x03: "ADD", 0x04: "MAD", 0x05: "DP3", 0x06: "DP4",
    0x07: "DST", 0x08: "MIN", 0x09: "MAX", 0x0A: "SLT", 0x0B: "SGE", 0x0C: "SLE", 0x0D: "SGT",
    0x0E: "SNE", 0x0F: "SEQ", 0x10: "FRC", 0x11: "FLR", 0x12: "KIL", 0x13: "PK4B", 0x14: "UP4B",
    0x15: "DDX", 0x16: "DDY", 0x17: "TEX", 0x18: "TXP", 0x19: "TXD", 0x1A: "RCP", 0x1B: "RSQ",
    0x1C: "EX2", 0x1D: "LG2", 0x1E: "LIT", 0x1F: "LRP", 0x20: "STR", 0x21: "SFL", 0x22: "COS",
    0x23: "SIN", 0x24: "PK2H", 0x25: "UP2H", 0x26: "POW", 0x27: "PK4UB", 0x28: "UP4UB",
    0x29: "PK2US", 0x2A: "UP2US", 0x2C: "PK16", 0x2D: "UP16", 0x2E: "DP2A", 0x2F: "TXL",
    0x31: "TXB", 0x33: "TEXBEM", 0x34: "TXPBEM", 0x35: "BEMLUM", 0x36: "REFL", 0x37: "TIMESWTEX",
    0x38: "DP2", 0x39: "NRM", 0x3A: "DIV", 0x3B: "DIVSQ", 0x3C: "LIF", 0x3D: "FENCT", 0x3E: "FENCB",
    0x40: "BRK", 0x41: "CAL", 0x42: "IFE", 0x43: "LOOP", 0x44: "REP", 0x45: "RET",
}
TEXOPS = {"TEX", "TXP", "TXD", "TXL", "TXB"}


def halves(w):
    return ((w << 16) | (w >> 16)) & 0xFFFFFFFF


def main():
    path = sys.argv[1]
    verbose = "-v" in sys.argv[2:]
    strict = "--sin-condiciones" in sys.argv[2:]
    cond = 0
    data = open(path, "rb").read()
    (magic, _pad, num_regs, num_attr, num_const, num_insn, attr_off, const_off, ucode_off,
     fp_control, texcoords, tc2d, tc3d, _p1) = struct.unpack(">HHHHHHIIIIHHHH", data[:36])
    ucode = data[ucode_off:]
    n = 0
    i = 0
    hist = {}
    prec = {0: 0, 1: 0, 2: 0, 3: 0}
    tex = 0
    const_at = {}
    lines = []
    while i + 16 <= len(ucode):
        w = [halves(struct.unpack(">I", ucode[i + 4 * k:i + 4 * k + 4])[0]) for k in range(4)]
        op_hi = (w[2] >> 31) & 1  # bit 6 del opcode en SRC1 (instrucciones de flujo)
        op = ((w[0] >> 24) & 0x3F) | (op_hi << 6)
        name = OPS.get(op, "op%02x" % op)
        p = (w[0] >> 22) & 3
        has_const = any(((w[s] & 3) == 2) for s in (1, 2, 3))
        # Bit 8 de la palabra 0: escribe los códigos de condición; bits
        # 18-20 de la palabra 1: condición (7 = siempre).
        if (w[0] >> 8) & 1 or ((w[1] >> 18) & 7) != 7:
            cond += 1
        n += 1
        hist[name] = hist.get(name, 0) + 1
        if name in TEXOPS:
            tex += 1
        else:
            prec[p] += 1
        if verbose:
            srcs = []
            for k in (1, 2, 3):
                t = w[k] & 3
                sw = "".join("xyzw"[(w[k] >> (9 + 2 * c)) & 3] for c in range(4))
                neg = "-" if (w[k] >> 17) & 1 else ""
                srcs.append(neg + ("R%d" % ((w[k] >> 2) & 0x3F) if t == 0 else "in" if t == 1 else "c") + "." + sw)
            # Valor absoluto: bit 29 de la palabra 1 (fuente 0) y bit 18 de
            # las palabras 2 y 3 (fuentes 1 y 2), como en nvfx_shader.h.
            absf = [(w[1] >> 29) & 1, (w[2] >> 18) & 1, (w[3] >> 18) & 1]
            srcs = ["|%s|" % x if a else x for x, a in zip(srcs, absf)]
            lines.append("%4d %-5s %s → %s%d.%s%s   %s" % (n, name, ("fp32", "fp16", "fx12", "?")[p],
                                                     "H" if (w[0] >> 7) & 1 else "R", (w[0] >> 1) & 0x3F,
                                                     "".join(c for b, c in enumerate("xyzw") if (w[0] >> (9 + b)) & 1),
                                                     " sat" if w[0] >> 31 else "", ", ".join(srcs)))
        if has_const:
            const_at[ucode_off + i + 16 - ucode_off] = True
        end = w[0] & 1
        i += 32 if has_const else 16
        if end:
            break
    print("%s: %d instrucciones (%d de textura), %d registros, entradas 0x%04x, control 0x%08x"
          % (path, n, tex, num_regs, texcoords, fp_control))
    print("  precisión de las operaciones: fp32 %d, fp16 %d, fx12 %d; con códigos de condición %d"
          % (prec[0], prec[1], prec[2], cond))
    print("  " + ", ".join("%s %d" % (k, v) for k, v in sorted(hist.items(), key=lambda kv: -kv[1])))
    for c in range(num_const):
        off = const_off + c * 28
        name_off, index, ctype, internal, count = struct.unpack(">IIBBB", data[off:off + 11])
        name = data[name_off:data.index(b"\0", name_off)].decode()
        copies = struct.unpack(">I", data[index:index + 4])[0] if index else 0
        print("  constante %-16s copias en el microcódigo %d%s" % (name, copies, " (interna)" if internal else ""))
    for l in lines:
        print(l)
    if strict and cond:
        print("%s: %d instrucciones con códigos de condición (cgcomp no los ensambla bien)" % (path, cond),
              file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
