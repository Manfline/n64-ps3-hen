#!/usr/bin/env python3
"""Análisis del código SPU que emite el recompilador del RSP (docs/lab-recompilador.md, «RSP → SPU»).

Lee un .rsplab (tools/lab/rsp-lab.c, con el traductor de src/rsp/jit) y da, por instrucción del RSP
y por familia: instrucciones del SPU, reparto entre las dos tuberías, nop/lnop de relleno, cargas,
saltos y sus pistas (hbr), y los ciclos de un modelo en orden de doble emisión sobre el camino recto
(latencias del manual del SPU: fijos simples 2, desplazamientos y rotaciones 4, multiplicaciones 7,
punto flotante 6, cargas 6, shufb y rotaciones de cuádruple 4; doble emisión solo con la de la tubería
par primero en una dirección múltiplo de 8 y la impar después, sin dependencia entre las dos).

    tools/lab/lab-spu.py reporte pd.rsplab [--top N]
    tools/lab/lab-spu.py trozo pd.rsplab 100       # un trozo desensamblado con ciclos
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lab_spu_ops import OPS  # noqa: E402

FMT_SHIFT = {"rr": (21, 11), "rrr": (28, 4), "ri7": (21, 11), "ri10": (24, 8), "ri16": (23, 9), "ri18": (25, 7)}
BY_OP = {}
for name, (fmt, op) in OPS.items():
    BY_OP.setdefault((fmt, op), name)
BY_OP[("rr", 0x201)] = "nop"
BY_OP[("rr", 0x001)] = "lnop"
BY_OP[("rr", 0x002)] = "sync"
BY_OP[("rr", 0x003)] = "dsync"
BY_OP[("rr", 0x000)] = "stop"
BY_OP[("rr", 0x1AC)] = "hbr"
BY_OP[("ri18", 0x09)] = "hbrr"
BY_OP[("ri18", 0x08)] = "hbra"

ODD = {"lqd", "lqx", "lqa", "stqd", "stqx", "stqa", "shufb", "rotqby", "rotqbi", "rotqbyi", "rotqbii", "rotqmby", "rotqmbyi",
       "rotqmbi", "rotqmbii", "shlqby", "shlqbyi", "shlqbi", "shlqbii", "gb", "gbb", "gbh", "fsm", "fsmb", "fsmh", "fsmbi",
       "cbd", "cbx", "chd", "chx", "cwd", "cwx", "cdd", "cdx", "br", "bra", "brsl", "brasl", "brnz", "brz", "brhnz", "brhz",
       "bi", "bisl", "biz", "binz", "bihz", "bihnz", "hbr", "hbrr", "hbra", "lnop", "sync", "dsync", "stop"}
BRANCH = {"br", "bra", "brsl", "brasl", "brnz", "brz", "brhnz", "brhz", "bi", "bisl", "biz", "binz", "bihz", "bihnz"}
COND = {"brnz", "brz", "brhnz", "brhz", "biz", "binz", "bihz", "bihnz"}
LOAD = {"lqd", "lqx", "lqa"}
STORE = {"stqd", "stqx", "stqa"}
LAT6 = LOAD
LAT4 = {"shufb", "rotqby", "rotqbi", "rotqbyi", "rotqbii", "rotqmby", "rotqmbyi", "rotqmbi", "rotqmbii", "shlqby", "shlqbyi",
        "shlqbi", "shlqbii", "gb", "gbb", "gbh", "fsm", "fsmb", "fsmh", "fsmbi", "cbd", "cbx", "chd", "chx", "cwd", "cwx", "cdd",
        "cdx", "shl", "shlh", "shli", "shlhi", "rot", "roth", "roti", "rothi", "rotm", "rothm", "rotmi", "rothmi", "rotma",
        "rotmah", "rotmai", "rotmahi", "cntb"}
LAT7 = {"mpy", "mpyu", "mpyh", "mpyhh", "mpyhhu", "mpys", "mpyi", "mpyui", "mpya", "mpyhha"}
MICRO = set()


def decode(w):
    """(nombre, rt, lee, escribe) de una palabra del SPU."""
    if w == 0x40200000:
        return "nop", None, [], []
    if w == 0x00200000:
        return "lnop", None, [], []
    for fmt, (sh, bits) in FMT_SHIFT.items():
        op = w >> sh
        n = BY_OP.get((fmt, op))
        if n is None:
            continue
        if fmt == "rr":
            rb, ra, rt = (w >> 14) & 127, (w >> 7) & 127, w & 127
        elif fmt == "rrr":
            rt, rb, ra, rc = (w >> 21) & 127, (w >> 14) & 127, (w >> 7) & 127, w & 127
        elif fmt in ("ri7", "ri10"):
            ra, rt = (w >> 7) & 127, w & 127
            rb = (w >> 14) & 127 if fmt == "ri7" else -1
        else:
            ra, rt, rb = (w >> 7) & 127, w & 127, -1
        reads, writes = [], []
        if n in STORE or n in COND or n in ("bi", "hbr") or n.startswith("bi") and n not in ("bisl",):
            # almacenamientos y saltos condicionales: leen rt
            if n in STORE or n in COND:
                reads.append(rt)
            if n in ("bi", "bisl", "biz", "binz", "bihz", "bihnz", "hbr"):
                reads.append(ra)
        elif n in ("hbrr", "hbra", "br", "bra", "stop", "sync", "dsync", "lnop", "nop"):
            pass
        elif fmt == "rr":
            reads += [ra] + ([rb] if rb >= 0 else [])
            writes.append(rt)
        elif fmt == "rrr":
            reads += [ra, rb, rc]
            writes.append(rt)
        elif fmt in ("ri7", "ri10"):
            reads.append(ra)
            if fmt == "ri7" and n in ("shufb",):
                reads.append(rb)
            writes.append(rt)
        elif fmt == "ri16":
            if n in ("iohl",):
                reads.append(rt)
            writes.append(rt)
        elif fmt == "ri18":
            writes.append(rt)
        if n in ("brsl", "brasl", "bisl"):
            writes.append(rt)
        if n == "iohl" or n.startswith("mpyhha"):
            reads.append(rt)
        if n in STORE:
            writes = []
        return n, rt, [r for r in reads if r is not None and r >= 0], writes
    return "?%08x" % w, None, [], []


def pipe(name):
    return "odd" if name in ODD else "even"


def latency(name):
    if name in LAT6:
        return 6
    if name in LAT7:
        return 7
    if name in LAT4:
        return 4
    return 2


class Sched:
    """Modelo en orden de doble emisión sobre un tramo recto, con la dirección de cada palabra."""

    def __init__(self):
        self.ready = collections.defaultdict(int)
        self.cycle = 0          # ciclo de la última emisión
        self.last = None        # (ciclo, ¿tubería par?, dirección) de la anterior
        self.dual = 0
        self.n = 0
        self.stall = 0

    def issue(self, addr, name, reads, writes):
        self.n += 1
        odd = name in ODD
        rdy = max([self.ready[r] for r in reads], default=0)
        t = self.cycle + 1 if self.last else 0
        if self.last:
            lc, lpar, laddr, lwrites = self.last
            if lpar and odd and laddr % 2 == 0 and addr % 2 == 1 and rdy <= lc and not (set(reads) & set(lwrites)):
                t = lc
                self.dual += 1
        t2 = max(t, rdy)
        self.stall += max(0, rdy - t) if t2 > t else 0
        t = t2
        self.cycle = t
        for w in writes:
            self.ready[w] = t + latency(name)
        # no puede ser el primero de una pareja si ya emitió con otra
        self.last = (t, not odd, addr, writes) if (t == self.cycle) else None
        return t


# ---- RSP: nombres de instrucción ----------------------------------------------------------------

SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 13: "break", 32: "add", 33: "addu",
           34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}
OPS_R = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi",
         13: "ori", 14: "xori", 15: "lui", 32: "lb", 33: "lh", 35: "lw", 36: "lbu", 37: "lhu", 40: "sb", 41: "sh", 43: "sw"}
VEC = {0: "vmulf", 1: "vmulu", 2: "vrndp", 3: "vmulq", 4: "vmudl", 5: "vmudm", 6: "vmudn", 7: "vmudh", 8: "vmacf", 9: "vmacu",
       10: "vrndn", 11: "vmacq", 12: "vmadl", 13: "vmadm", 14: "vmadn", 15: "vmadh", 16: "vadd", 17: "vsub", 19: "vabs",
       20: "vaddc", 21: "vsubc", 29: "vsar", 32: "vlt", 33: "veq", 34: "vne", 35: "vge", 36: "vcl", 37: "vch", 38: "vcr",
       39: "vmrg", 40: "vand", 41: "vnand", 42: "vor", 43: "vnor", 44: "vxor", 45: "vnxor", 48: "vrcp", 49: "vrcpl", 50: "vrcph",
       51: "vmov", 52: "vrsq", 53: "vrsql", 54: "vrsqh", 55: "vnop"}
LWC2 = {0: "lbv", 1: "lsv", 2: "llv", 3: "ldv", 4: "lqv", 5: "lrv", 6: "lpv", 7: "luv", 8: "lhv", 9: "lfv", 11: "ltv"}
SWC2 = {0: "sbv", 1: "ssv", 2: "slv", 3: "sdv", 4: "sqv", 5: "srv", 6: "spv", 7: "suv", 8: "shv", 9: "sfv", 10: "swv", 11: "stv"}


def rsp_name(w):
    op = w >> 26
    if op == 0:
        return SPECIAL.get(w & 63, "special%d" % (w & 63))
    if op == 1:
        return {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get((w >> 16) & 31, "regimm")
    if op == 16:
        return {0: "mfc0", 4: "mtc0"}.get((w >> 21) & 31, "cop0")
    if op == 18:
        if w & (1 << 25):
            return VEC.get(w & 63, "vec%d" % (w & 63))
        return {0: "mfc2", 2: "cfc2", 4: "mtc2", 6: "ctc2"}.get((w >> 21) & 31, "cop2")
    if op == 50:
        return LWC2.get((w >> 11) & 31, "lwc2")
    if op == 58:
        return SWC2.get((w >> 11) & 31, "swc2")
    return OPS_R.get(op, "op%d" % op)


def family(n):
    if n in ("vrcp", "vrcpl", "vrcph", "vrsq", "vrsql", "vrsqh"):
        return "vrcp/vrsq"
    if n.startswith("v"):
        return "vectorial"
    if n.endswith("v") and len(n) == 3 and n[0] in "ls":
        return "carga/almac. vectorial"
    if n in ("lb", "lh", "lw", "lbu", "lhu", "sb", "sh", "sw"):
        return "carga/almac. escalar"
    if n in ("j", "jal", "jr", "jalr", "beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"):
        return "salto"
    if n in ("mfc0", "mtc0"):
        return "cop0"
    if n in ("mfc2", "mtc2", "cfc2", "ctc2"):
        return "cop2 (movimientos)"
    return "escalar"


def parse(path):
    chunks = []
    stubs = []
    cur = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "SW":
            stubs = [int(x, 16) for x in t[1:]]
        elif t[0] == "C":
            cur = {"pc0": int(t[1], 16), "kv": dict(x.split("=") for x in t[2:])}
            chunks.append(cur)
        elif t[0] == "E":
            cur["entry"] = [int(x) for x in t[1:]]
        elif t[0] == "I":
            cur["rsp"] = [int(x, 16) for x in t[1:]]
        elif t[0] == "P":
            cur["code"] = [int(x, 16) for x in t[1:]]
    return stubs, chunks


def ranges(ch):
    """Palabras del SPU de cada instrucción del RSP: de su entrada a la siguiente mayor."""
    n = len(ch["code"])
    es = sorted(set(e // 4 for e in ch["entry"] if 0 <= e // 4 < n))
    nxt = {a: (es[i + 1] if i + 1 < len(es) else n) for i, a in enumerate(es)}
    out = []
    for e in ch["entry"]:
        a = e // 4
        out.append((a, nxt.get(a, a)))
    return out


def analyze_range(code, a, b, base_words=0):
    s = Sched()
    st = collections.Counter()
    for i in range(a, b):
        name, rt, reads, writes = decode(code[i])
        s.issue(i + base_words, name, reads, writes)
        st["words"] += 1
        st["odd" if name in ODD else "even"] += 1
        if name in ("nop", "lnop"):
            st["nops"] += 1
        if name in LOAD:
            st["loads"] += 1
        if name in STORE:
            st["stores"] += 1
        if name in BRANCH:
            st["branches"] += 1
        if name in ("hbr", "hbrr", "hbra"):
            st["hints"] += 1
        if name.startswith("?"):
            st["desconocidas"] += 1
    st["cycles"] = s.cycle + 1 if b > a else 0
    st["dual"] = s.dual
    st["stall"] = s.stall
    return st


def cmd_reporte(path, args):
    top = 25
    stubs, chunks = parse(path)
    tab = collections.defaultdict(collections.Counter)
    fam = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    for ch in chunks:
        rs = ranges(ch)
        seen = set()
        for i, (a, b) in enumerate(rs):
            if a in seen or a == b:
                continue
            seen.add(a)
            w = ch["rsp"][i]
            n = rsp_name(w)
            st = analyze_range(ch["code"], a, b)
            tab[n]["rsp"] += 1
            fam[family(n)]["rsp"] += 1
            for k, v in st.items():
                tab[n][k] += v
                fam[family(n)][k] += v
                total[k] += v
            total["rsp"] += 1
    print("%s: %d trozos, %d instrucciones del RSP traducidas, %d palabras del SPU (%.1f por instrucción), %d desconocidas" %
          (os.path.basename(path), len(chunks), total["rsp"], total["words"], total["words"] / max(total["rsp"], 1), total["desconocidas"]))
    print("tuberías: par %.1f %%, impar %.1f %%; nop/lnop %.1f %%; cargas %.1f %%, saltos %.1f %%, pistas (hbr) %d" %
          (100 * total["even"] / total["words"], 100 * total["odd"] / total["words"], 100 * total["nops"] / total["words"],
           100 * total["loads"] / total["words"], 100 * total["branches"] / total["words"], total["hints"]))
    print("modelo en orden (camino recto, sin saltos ni caché): %.1f ciclos por instrucción del RSP, %.2f instrucciones por ciclo, "
          "doble emisión en el %.1f %% de las palabras, %.1f ciclos de espera por dependencias por instrucción" %
          (total["cycles"] / total["rsp"], total["words"] / max(total["cycles"], 1), 100 * total["dual"] / total["words"],
           total["stall"] / total["rsp"]))
    taken = 0.5 * (total["branches"] - total["hints"])
    print("con 18 ciclos por salto tomado sin pista (la mitad de los %d saltos sin pista): %.1f ciclos por instrucción del RSP (%.1f ns a 3,2 GHz); "
          "medido en la PS3 con el paquete ps3-5: 80–86 ns, meta ≤ 16 ns" %
          (total["branches"] - total["hints"], (total["cycles"] + 18 * taken) / total["rsp"], (total["cycles"] + 18 * taken) / total["rsp"] / 3.2))
    print()
    print("%-26s %6s %8s %8s %7s %7s %7s" % ("familia", "instr", "SPU/instr", "ciclos", "par %", "nop %", "doble %"))
    for k, v in sorted(fam.items(), key=lambda kv: -kv[1]["words"]):
        print("%-26s %6d %8.1f %8.1f %6.0f%% %6.0f%% %6.0f%%" % (k, v["rsp"], v["words"] / v["rsp"], v["cycles"] / v["rsp"],
              100 * v["even"] / v["words"], 100 * v["nops"] / v["words"], 100 * v["dual"] / v["words"]))
    print()
    print("%-10s %6s %8s %8s %6s" % ("RSP", "n", "SPU/instr", "ciclos", "nop%"))
    for k, v in sorted(tab.items(), key=lambda kv: -kv[1]["words"])[:top]:
        print("%-10s %6d %8.1f %8.1f %5.0f%%" % (k, v["rsp"], v["words"] / v["rsp"], v["cycles"] / v["rsp"], 100 * v["nops"] / v["words"]))


def cmd_trozo(path, args):
    stubs, chunks = parse(path)
    pc0 = int(args[0], 16)
    ch = [c for c in chunks if c["pc0"] == pc0][0]
    s = Sched()
    rs = ranges(ch)
    start = {a: i for i, (a, b) in reversed(list(enumerate(rs)))}
    for i, w in enumerate(ch["code"]):
        name, rt, reads, writes = decode(w)
        t = s.issue(i, name, reads, writes)
        if i in start:
            print("; RSP %03x: %s" % (pc0 + 4 * start[i], rsp_name(ch["rsp"][start[i]])))
        print("  %4d c%-4d %-7s %s r%s <- %s  [%s]" % (i, t, name, "", writes, reads, pipe(name)))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    {"reporte": cmd_reporte, "trozo": cmd_trozo}[sys.argv[1]](sys.argv[2], sys.argv[3:])


if __name__ == "__main__":
    main()
