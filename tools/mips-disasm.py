#!/usr/bin/env python3
"""Desensamblador MIPS III (VR4300) mínimo, para leer los bloques calientes
que lista `salsa64-run` con SALSA64_REC_PROF (líneas "bloque <pc> ... palabras: ...")
o palabras sueltas.

    tools/mips-disasm.py 8c450018 24a50004 ...          # palabras
    tools/mips-disasm.py --pc 7f0a1b2c 8c450018 ...     # con la dirección
    grep '^bloque' build/logs/x.log | tools/mips-disasm.py   # bloques del perfil

Escrito desde la documentación pública de la ISA; solo lo que usan los juegos.
"""
import sys

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]
COP0 = {0: "Index", 1: "Random", 2: "EntryLo0", 3: "EntryLo1", 4: "Context", 5: "PageMask", 6: "Wired",
        8: "BadVAddr", 9: "Count", 10: "EntryHi", 11: "Compare", 12: "Status", 13: "Cause", 14: "EPC",
        15: "PRId", 16: "Config", 17: "LLAddr", 18: "WatchLo", 19: "WatchHi", 20: "XContext", 26: "PErr",
        27: "CacheErr", 28: "TagLo", 29: "TagHi", 30: "ErrorEPC"}
SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 12: "syscall",
           13: "break", 15: "sync", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 20: "dsllv", 22: "dsrlv",
           23: "dsrav", 24: "mult", 25: "multu", 26: "div", 27: "divu", 28: "dmult", 29: "dmultu", 30: "ddiv",
           31: "ddivu", 32: "add", 33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor",
           42: "slt", 43: "sltu", 44: "dadd", 45: "daddu", 46: "dsub", 47: "dsubu", 48: "tge", 49: "tgeu",
           50: "tlt", 51: "tltu", 52: "teq", 54: "tne", 56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32",
           62: "dsrl32", 63: "dsra32"}
REGIMM = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 8: "tgei", 9: "tgeiu", 10: "tlti", 11: "tltiu",
          12: "teqi", 14: "tnei", 16: "bltzal", 17: "bgezal", 18: "bltzall", 19: "bgezall"}
OPS = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti",
       11: "sltiu", 12: "andi", 13: "ori", 14: "xori", 15: "lui", 20: "beql", 21: "bnel", 22: "blezl",
       23: "bgtzl", 24: "daddi", 25: "daddiu", 26: "ldl", 27: "ldr", 32: "lb", 33: "lh", 34: "lwl", 35: "lw",
       36: "lbu", 37: "lhu", 38: "lwr", 39: "lwu", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 44: "sdl",
       45: "sdr", 46: "swr", 47: "cache", 48: "ll", 49: "lwc1", 52: "lld", 53: "ldc1", 55: "ld", 56: "sc",
       57: "swc1", 60: "scd", 61: "sdc1", 63: "sd"}
FPU = {0: "add", 1: "sub", 2: "mul", 3: "div", 4: "sqrt", 5: "abs", 6: "mov", 7: "neg", 8: "round.l",
       9: "trunc.l", 10: "ceil.l", 11: "floor.l", 12: "round.w", 13: "trunc.w", 14: "ceil.w", 15: "floor.w",
       32: "cvt.s", 33: "cvt.d", 36: "cvt.w", 37: "cvt.l"}
COND = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]
FMT = {16: "s", 17: "d", 20: "w", 21: "l"}


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def dis(w, pc=None):
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm, simm = w & 0xFFFF, sx16(w & 0xFFFF)
    R = GPR
    br = lambda: ("0x%08x" % ((pc + 4 + simm * 4) & 0xFFFFFFFF)) if pc is not None else "%+d" % (simm * 4 + 4)
    if w == 0:
        return "nop"
    if op == 0:
        m = SPECIAL.get(fn, "special?%d" % fn)
        if fn in (0, 2, 3, 56, 58, 59, 60, 62, 63):
            return "%s %s, %s, %d" % (m, R[rd], R[rt], sa)
        if fn in (4, 6, 7, 20, 22, 23):
            return "%s %s, %s, %s" % (m, R[rd], R[rt], R[rs])
        if fn == 8:
            return "jr %s" % R[rs]
        if fn == 9:
            return "jalr %s, %s" % (R[rd], R[rs]) if rd != 31 else "jalr %s" % R[rs]
        if fn in (12, 13, 15):
            return m
        if fn in (16, 18):
            return "%s %s" % (m, R[rd])
        if fn in (17, 19):
            return "%s %s" % (m, R[rs])
        if 24 <= fn <= 31:
            return "%s %s, %s" % (m, R[rs], R[rt])
        if 48 <= fn <= 54:
            return "%s %s, %s" % (m, R[rs], R[rt])
        return "%s %s, %s, %s" % (m, R[rd], R[rs], R[rt])
    if op == 1:
        m = REGIMM.get(rt, "regimm?%d" % rt)
        if 8 <= rt <= 14:
            return "%s %s, %d" % (m, R[rs], simm)
        return "%s %s, %s" % (m, R[rs], br())
    if op in (2, 3):
        t = (w & 0x3FFFFFF) << 2
        if pc is not None:
            t |= (pc + 4) & 0xF0000000
        return "%s 0x%08x" % (OPS[op], t)
    if op in (4, 5, 20, 21):
        return "%s %s, %s, %s" % (OPS[op], R[rs], R[rt], br())
    if op in (6, 7, 22, 23):
        return "%s %s, %s" % (OPS[op], R[rs], br())
    if op in (8, 9, 10, 11, 24, 25):
        return "%s %s, %s, %d" % (OPS[op], R[rt], R[rs], simm)
    if op in (12, 13, 14):
        return "%s %s, %s, 0x%x" % (OPS[op], R[rt], R[rs], imm)
    if op == 15:
        return "lui %s, 0x%x" % (R[rt], imm)
    if op == 16:
        if rs == 0:
            return "mfc0 %s, %s" % (R[rt], COP0.get(rd, "$%d" % rd))
        if rs == 1:
            return "dmfc0 %s, %s" % (R[rt], COP0.get(rd, "$%d" % rd))
        if rs == 4:
            return "mtc0 %s, %s" % (R[rt], COP0.get(rd, "$%d" % rd))
        if rs == 5:
            return "dmtc0 %s, %s" % (R[rt], COP0.get(rd, "$%d" % rd))
        if rs == 16:
            return {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 24: "eret"}.get(fn, "cop0?%d" % fn)
        return "cop0?%08x" % w
    if op == 17:
        if rs == 0:
            return "mfc1 %s, f%d" % (R[rt], rd)
        if rs == 1:
            return "dmfc1 %s, f%d" % (R[rt], rd)
        if rs == 2:
            return "cfc1 %s, fcr%d" % (R[rt], rd)
        if rs == 4:
            return "mtc1 %s, f%d" % (R[rt], rd)
        if rs == 5:
            return "dmtc1 %s, f%d" % (R[rt], rd)
        if rs == 6:
            return "ctc1 %s, fcr%d" % (R[rt], rd)
        if rs == 8:
            return "%s %s" % (["bc1f", "bc1t", "bc1fl", "bc1tl"][rt & 3], br())
        f = FMT.get(rs, "?%d" % rs)
        if fn >= 48:
            return "c.%s.%s f%d, f%d" % (COND[fn & 15], f, rd, rt)
        m = FPU.get(fn, "fpu?%d" % fn)
        if fn in (0, 1, 2, 3):
            return "%s.%s f%d, f%d, f%d" % (m, f, sa, rd, rt)
        return "%s.%s f%d, f%d" % (m, f, sa, rd)
    if op == 47:
        return "cache 0x%x, %d(%s)" % (rt, simm, R[rs])
    if op in (49, 53, 57, 61):
        return "%s f%d, %d(%s)" % (OPS[op], rt, simm, R[rs])
    if op in OPS and op >= 26:
        return "%s %s, %d(%s)" % (OPS[op], R[rt], simm, R[rs])
    return "?%08x" % w


def main():
    args = sys.argv[1:]
    pc = None
    if args and args[0] == "--pc":
        pc = int(args[1], 16)
        args = args[2:]
    if args:
        for k, a in enumerate(args):
            w = int(a, 16)
            p = pc + 4 * k if pc is not None else None
            print("%s%08x  %s" % (("%08x: " % p) if p is not None else "", w, dis(w, p)))
        return
    for line in sys.stdin:
        if not line.startswith("bloque"):
            continue
        head, _, words = line.partition("palabras:")
        print("== " + head.strip())
        parts = head.split()
        bpc = int(parts[1], 16) if len(parts) > 1 else None
        for k, a in enumerate(words.split()):
            w = int(a, 16)
            p = bpc + 4 * k if bpc is not None else None
            print("   %s%08x  %s" % (("%08x: " % p) if p is not None else "", w, dis(w, p)))


if __name__ == "__main__":
    main()
