"""Decodificador de las instrucciones PowerPC que emite src/cpu/ppc.h, para el
laboratorio del recompilador (tools/lab/lab-analiza.py).

Solo lo que emite el recompilador (Power ISA 2.02, lo que tiene el PPE del
Cell); lo demás sale como «op31.<xo>» y similares. Cada palabra decodifica a
un `Insn` inmutable (con caché: el código generado repite mucho).

`micro` marca lo que el PPE ejecuta en microcódigo (manual del Cell, tabla
A-3: ≥ 11 ciclos y vacía la tubería): formas con punto, desplazamientos y
rotaciones por registro, lwa/lha, mtcrf de varios campos, mcrfs, mtfsf*,
lmw/stmw. `slow` marca las que no son microcódigo pero cuestan mucho más que
un ciclo (mfcr ~34, mulld/mulh ~11–15, divide decenas, mffs 11–28).
"""

# Clases (para histogramas y para decidir qué es memoria, salto, etc.)
ALU, ROT, SHIFT, CMP, LOAD, STORE, FLOAD, FSTORE, FPU, CROP, BR, SPR, MUL, DIV, SYNC, NOP, OTHER = (
    "alu", "rot", "shift", "cmp", "load", "store", "fload", "fstore", "fpu", "cr", "br", "spr", "mul", "div",
    "sync", "nop", "other")


class Insn:
    __slots__ = ("w", "name", "cls", "micro", "slow", "rt", "ra", "rb", "imm", "rc", "size", "mem", "bo", "bi",
                 "link", "aa", "reads", "writes", "wcr", "rcr")

    def __init__(self, w, name, cls, **kw):
        self.w = w
        self.name = name
        self.cls = cls
        self.micro = kw.get("micro", False)
        self.slow = kw.get("slow", 0)
        self.rt = kw.get("rt", -1)
        self.ra = kw.get("ra", -1)
        self.rb = kw.get("rb", -1)
        self.imm = kw.get("imm", 0)
        self.rc = kw.get("rc", 0)
        self.size = kw.get("size", 0)      # bytes de un acceso a memoria
        self.mem = kw.get("mem", None)     # "d" (base + desplazamiento) o "x" (base + índice)
        self.bo = kw.get("bo", -1)
        self.bi = kw.get("bi", -1)
        self.link = kw.get("link", 0)
        self.aa = kw.get("aa", 0)
        self.reads = kw.get("reads", ())   # registros enteros leídos
        self.writes = kw.get("writes", ())
        self.wcr = kw.get("wcr", ())       # campos de CR escritos
        self.rcr = kw.get("rcr", ())       # campos de CR leídos

    def is_mem(self):
        return self.cls in (LOAD, STORE, FLOAD, FSTORE)

    def is_load(self):
        return self.cls in (LOAD, FLOAD)

    def is_store(self):
        return self.cls in (STORE, FSTORE)

    def __repr__(self):
        return "%s(%08x)" % (self.name, self.w)


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def _s26(v):
    return v - 0x4000000 if v & 0x2000000 else v


def _r(*regs):
    """Registros enteros distintos de «cero»: en addi/addis/cargas ra=0 es el valor 0."""
    return tuple(regs)


_D_LOADS = {32: ("lwz", 4, LOAD), 34: ("lbz", 1, LOAD), 40: ("lhz", 2, LOAD), 48: ("lfs", 4, FLOAD), 50: ("lfd", 8, FLOAD)}
_D_STORES = {36: ("stw", 4, STORE), 38: ("stb", 1, STORE), 44: ("sth", 2, STORE), 52: ("stfs", 4, FSTORE),
             54: ("stfd", 8, FSTORE)}
_X31 = {
    # xo10: (nombre, clase, kw)
    0: ("cmp", CMP, {}), 32: ("cmpl", CMP, {}),
    8: ("subfc", ALU, {}), 9: ("mulhdu", MUL, {"slow": 11}), 10: ("addc", ALU, {}), 11: ("mulhwu", MUL, {"slow": 6}),
    19: ("mfcr", SPR, {"slow": 34}), 20: ("lwarx", LOAD, {"size": 4, "mem": "x"}),
    21: ("ldx", LOAD, {"size": 8, "mem": "x"}), 23: ("lwzx", LOAD, {"size": 4, "mem": "x"}),
    24: ("slw", SHIFT, {"micro": True}), 26: ("cntlzw", ALU, {}), 27: ("sld", SHIFT, {"micro": True}),
    28: ("and", ALU, {}), 40: ("subf", ALU, {}), 54: ("dcbst", OTHER, {}), 58: ("cntlzd", ALU, {}),
    60: ("andc", ALU, {}), 73: ("mulhd", MUL, {"slow": 11}), 75: ("mulhw", MUL, {"slow": 6}),
    86: ("dcbf", OTHER, {}), 87: ("lbzx", LOAD, {"size": 1, "mem": "x"}), 104: ("neg", ALU, {}),
    124: ("nor", ALU, {}), 136: ("subfe", ALU, {}), 138: ("adde", ALU, {}),
    149: ("stdx", STORE, {"size": 8, "mem": "x"}), 151: ("stwx", STORE, {"size": 4, "mem": "x"}),
    215: ("stbx", STORE, {"size": 1, "mem": "x"}), 233: ("mulld", MUL, {"slow": 11}), 235: ("mullw", MUL, {"slow": 6}),
    266: ("add", ALU, {}), 279: ("lhzx", LOAD, {"size": 2, "mem": "x"}), 284: ("eqv", ALU, {}),
    316: ("xor", ALU, {}), 341: ("lwax", LOAD, {"size": 4, "mem": "x", "micro": True}),
    343: ("lhax", LOAD, {"size": 2, "mem": "x", "micro": True}), 407: ("sthx", STORE, {"size": 2, "mem": "x"}),
    412: ("orc", ALU, {}), 444: ("or", ALU, {}), 457: ("divdu", DIV, {"slow": 60}), 459: ("divwu", DIV, {"slow": 30}),
    489: ("divd", DIV, {"slow": 60}), 491: ("divw", DIV, {"slow": 30}),
    535: ("lfsx", FLOAD, {"size": 4, "mem": "x"}), 536: ("srw", SHIFT, {"micro": True}),
    539: ("srd", SHIFT, {"micro": True}), 598: ("sync", SYNC, {}), 599: ("lfdx", FLOAD, {"size": 8, "mem": "x"}),
    663: ("stfsx", FSTORE, {"size": 4, "mem": "x"}), 727: ("stfdx", FSTORE, {"size": 8, "mem": "x"}),
    792: ("sraw", SHIFT, {"micro": True}), 794: ("srad", SHIFT, {"micro": True}), 824: ("srawi", SHIFT, {}),
    826: ("sradi", SHIFT, {}), 827: ("sradi", SHIFT, {}), 922: ("extsh", ALU, {}), 954: ("extsb", ALU, {}),
    986: ("extsw", ALU, {}), 982: ("icbi", OTHER, {}),
}
_RC_FORMS = {ALU, SHIFT, MUL, DIV}   # con Rc = 1: microcódigo
_F63X = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 40: "fneg", 72: "fmr", 264: "fabs",
         583: "mffs", 711: "mtfsf", 814: "fctid", 815: "fctidz", 846: "fcfid", 38: "mtfsb1", 70: "mtfsb0"}
_F63A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 25: "fmul", 28: "fmsub", 29: "fmadd", 30: "fnmsub",
         31: "fnmadd", 26: "frsqrte", 24: "fres", 23: "fsel"}
_CR19 = {0: "mcrf", 33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand", 257: "crand", 289: "creqv",
         417: "crorc", 449: "cror", 150: "isync"}

_cache = {}


def decode(w):
    i = _cache.get(w)
    if i is None:
        i = _decode(w)
        _cache[w] = i
    return i


def _decode(w):
    op = w >> 26
    rt = (w >> 21) & 31
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    rc = w & 1
    imm = _s16(w & 0xFFFF)
    if w == 0x60000000:
        return Insn(w, "nop", NOP)
    if op == 14:
        return Insn(w, "li" if ra == 0 else "addi", ALU, rt=rt, ra=ra, imm=imm, reads=_r(ra) if ra else (), writes=(rt,))
    if op == 15:
        return Insn(w, "lis" if ra == 0 else "addis", ALU, rt=rt, ra=ra, imm=imm, reads=_r(ra) if ra else (), writes=(rt,))
    if op == 24:
        return Insn(w, "ori", ALU, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,))
    if op == 25:
        return Insn(w, "oris", ALU, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,))
    if op == 26:
        return Insn(w, "xori", ALU, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,))
    if op == 27:
        return Insn(w, "xoris", ALU, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,))
    if op == 28:
        return Insn(w, "andi.", ALU, micro=True, rc=1, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,), wcr=(0,))
    if op == 29:
        return Insn(w, "andis.", ALU, micro=True, rc=1, rt=rt, ra=ra, imm=w & 0xFFFF, reads=(rt,), writes=(ra,), wcr=(0,))
    if op == 7:
        return Insn(w, "mulli", MUL, slow=5, rt=rt, ra=ra, imm=imm, reads=(ra,), writes=(rt,))
    if op == 8:
        return Insn(w, "subfic", ALU, rt=rt, ra=ra, imm=imm, reads=(ra,), writes=(rt,))
    if op in (10, 11):
        bf = rt >> 2
        name = ("cmpldi" if rt & 1 else "cmplwi") if op == 10 else ("cmpdi" if rt & 1 else "cmpwi")
        return Insn(w, name, CMP, rt=rt, ra=ra, imm=imm if op == 11 else w & 0xFFFF, reads=(ra,), wcr=(bf,))
    if op in _D_LOADS:
        name, size, cls = _D_LOADS[op]
        return Insn(w, name, cls, rt=rt, ra=ra, imm=imm, size=size, mem="d", reads=_r(ra) if ra else (),
                    writes=(rt,) if cls == LOAD else ())
    if op in _D_STORES:
        name, size, cls = _D_STORES[op]
        return Insn(w, name, cls, rt=rt, ra=ra, imm=imm, size=size, mem="d",
                    reads=(_r(ra) if ra else ()) + ((rt,) if cls == STORE else ()))
    if op == 58:
        sub = w & 3
        return Insn(w, {0: "ld", 1: "ldu", 2: "lwa"}.get(sub, "op58"), LOAD, rt=rt, ra=ra, imm=_s16(w & 0xFFFC),
                    size=8 if sub < 2 else 4, mem="d", micro=(sub == 2), reads=_r(ra) if ra else (), writes=(rt,))
    if op == 62:
        sub = w & 3
        return Insn(w, "std" if sub == 0 else "stdu", STORE, rt=rt, ra=ra, imm=_s16(w & 0xFFFC), size=8, mem="d",
                    reads=(rt,) + (_r(ra) if ra else ()), writes=(ra,) if sub == 1 else ())
    if op == 21:
        return Insn(w, "rlwinm." if rc else "rlwinm", ROT, micro=bool(rc), rc=rc, rt=rt, ra=ra, imm=(rb << 10) | (((w >> 6) & 31) << 5) | ((w >> 1) & 31),
                    reads=(rt,), writes=(ra,))
    if op == 20:
        return Insn(w, "rlwimi." if rc else "rlwimi", ROT, micro=bool(rc), rc=rc, rt=rt, ra=ra, reads=(rt, ra), writes=(ra,))
    if op == 23:
        return Insn(w, "rlwnm", ROT, micro=True, rt=rt, ra=ra, rb=rb, reads=(rt, rb), writes=(ra,))
    if op == 30:
        sub = (w >> 2) & 7
        if sub == 4:
            sub2 = (w >> 1) & 1
            return Insn(w, "rldcr" if sub2 else "rldcl", ROT, micro=True, rt=rt, ra=ra, rb=rb, reads=(rt, rb), writes=(ra,))
        name = {0: "rldicl", 1: "rldicr", 2: "rldic", 3: "rldimi"}[sub]
        return Insn(w, name + ("." if rc else ""), ROT, micro=bool(rc), rc=rc, rt=rt, ra=ra,
                    reads=(rt,) + ((ra,) if sub == 3 else ()), writes=(ra,))
    if op == 16:
        return Insn(w, "bc", BR, bo=rt, bi=ra, imm=_s16(w & 0xFFFC), link=w & 1, aa=(w >> 1) & 1,
                    rcr=((ra >> 2),) if not (rt & 0x10) else ())
    if op == 18:
        return Insn(w, "bl" if w & 1 else "b", BR, imm=_s26(w & 0x03FFFFFC), link=w & 1, aa=(w >> 1) & 1, bo=20)
    if op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:
            return Insn(w, "blr" if rt == 20 else "bclr", BR, bo=rt, bi=ra, link=w & 1, rcr=((ra >> 2),) if not (rt & 0x10) else ())
        if xo == 528:
            return Insn(w, "bctr" if rt == 20 else "bcctr", BR, bo=rt, bi=ra, link=w & 1)
        name = _CR19.get(xo)
        if name:
            return Insn(w, name, CROP if xo != 150 else SYNC, rt=rt, ra=ra, rb=rb,
                        wcr=(rt >> 2,) if xo not in (0, 150) else (), rcr=((ra >> 2), (rb >> 2)) if xo not in (0, 150) else ())
        return Insn(w, "op19.%d" % xo, OTHER)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 339 or xo == 467:  # mfspr / mtspr
            spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
            nm = {8: "lr", 9: "ctr", 1: "xer"}.get(spr, "spr%d" % spr)
            if xo == 339:
                return Insn(w, "mf" + nm, SPR, rt=rt, writes=(rt,), slow=2 if spr == 8 else 0)
            return Insn(w, "mt" + nm, SPR, rt=rt, reads=(rt,))
        if xo == 144:  # mtcrf / mtocrf
            fxm = (w >> 12) & 0xFF
            single = bool(w & (1 << 20))
            multi = bin(fxm).count("1") > 1
            return Insn(w, "mtocrf" if single else "mtcrf", SPR, micro=multi and not single, rt=rt, reads=(rt,),
                        imm=fxm, wcr=tuple(i for i in range(8) if fxm & (0x80 >> i)))
        if xo == 19:
            return Insn(w, "mfocrf" if w & (1 << 20) else "mfcr", SPR, slow=34, rt=rt, writes=(rt,),
                        rcr=tuple(range(8)))
        if xo == 54 or xo == 86 or xo == 982:
            return Insn(w, _X31[xo][0], OTHER, ra=ra, rb=rb, reads=(ra, rb))
        t = _X31.get(xo)
        if t:
            name, cls, kw = t
            kw = dict(kw)
            micro = kw.pop("micro", False)
            if rc and cls in _RC_FORMS | {ROT}:
                micro, name = True, name + "."
            if cls in (LOAD, STORE, FLOAD, FSTORE):
                reads = ((ra,) if ra else ()) + (rb,)
                if cls in (STORE, FSTORE):
                    reads = reads + ((rt,) if cls == STORE else ())
                return Insn(w, name, cls, micro=micro, rt=rt, ra=ra, rb=rb, reads=reads,
                            writes=(rt,) if cls == LOAD else (), **kw)
            if cls == CMP:
                return Insn(w, ("cmpd" if rt & 1 else "cmpw") if xo == 0 else ("cmpld" if rt & 1 else "cmplw"), CMP, rt=rt, ra=ra, rb=rb,
                            reads=(ra, rb), wcr=(rt >> 2,))
            if name in ("neg", "extsw", "extsh", "extsb", "cntlzw", "cntlzd"):
                return Insn(w, name, cls, micro=micro, rc=rc, rt=rt, ra=ra, reads=(rt if name != "neg" else ra,), writes=(rt if name == "neg" else ra,), **kw)
            if cls in (SHIFT, ALU, MUL, DIV) and name in ("and", "andc", "or", "orc", "xor", "nor", "eqv", "slw", "srw", "sld", "srd", "sraw", "srad"):
                if name == "or" and rt == rb:
                    name = "mr"
                return Insn(w, name, cls, micro=micro, rc=rc, rt=rt, ra=ra, rb=rb, reads=(rt, rb), writes=(ra,), **kw)
            if name in ("srawi", "sradi"):
                return Insn(w, name, cls, micro=micro, rc=rc, rt=rt, ra=ra, reads=(rt,), writes=(ra,), **kw)
            if cls == SYNC:
                return Insn(w, name, cls)
            return Insn(w, name, cls, micro=micro, rc=rc, rt=rt, ra=ra, rb=rb, reads=(ra, rb), writes=(rt,), **kw)
        return Insn(w, "op31.%d" % xo, OTHER)
    if op in (59, 63):
        x10 = (w >> 1) & 0x3FF
        xo5 = (w >> 1) & 31
        sfx = "s" if op == 59 else ""
        if op == 63 and x10 in _F63X:
            name = _F63X[x10]
            if name == "mffs":
                return Insn(w, name, FPU, slow=20, micro=False)
            if name in ("mtfsf", "mtfsb1", "mtfsb0"):
                return Insn(w, name, FPU, micro=True)
            if name in ("fcmpu", "fcmpo"):
                return Insn(w, name, FPU, wcr=((w >> 23) & 7,))
            return Insn(w, name, FPU)
        if xo5 in _F63A:
            return Insn(w, _F63A[xo5] + sfx, FPU, slow=0 if xo5 not in (18, 22) else 20)
        return Insn(w, "op%d.%d" % (op, x10), OTHER)
    return Insn(w, "op%d" % op, OTHER)


def branch_target(ins, idx):
    """Índice (en palabras, respecto de la misma sección) al que salta una b/bc relativa."""
    return idx + ins.imm // 4
