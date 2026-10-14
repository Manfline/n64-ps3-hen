"""Métricas del código que emite el recompilador, sobre un .lab (lab_data.py).

Todo se pondera por las entradas a cada bloque (blk.count: las del perfil de
la PS3 o las muestras de ares). Las cifras de «ejecutado» son un modelo
estático (docs/lab-recompilador.md): camino sin salto tomado por el cuerpo
caliente, más las rutinas compartidas que el cuerpo llama con `bl`; no hay
caché ni predicción de saltos aquí (ver cache_model).
"""
import collections
import importlib.machinery
import importlib.util
import os

import lab_ppc as P

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_loader(
    "mdis", importlib.machinery.SourceFileLoader("mdis", os.path.join(_HERE, "..", "mips-disasm.py")))
mdis = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mdis)

R_CPU = 31

BRANCHES = ("beq", "bne", "bgtz", "blez", "bltz", "bgez", "beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bc1f",
            "bc1t", "bc1fl", "bc1tl", "j", "jal", "jr", "jalr", "bltzal", "bgezal", "b", "bal", "beqz", "bnez", "beqzl",
            "bnezl")

HOW = {0: "nativa", 1: "intérprete", 2: "excepción", 3: "MTC0", 4: "TLBW", 5: "ERET", 255: "fin"}


def mips_mnem(w):
    if w == 0:
        return "nop"
    return mdis.dis(w, 0).split()[0]


# ---- Rutinas compartidas ---------------------------------------------------------------------

LIKELY = (7, 15)       # BO con pista «tomado»


class Rtn:
    """Camino probable (según las pistas de predicción de ppc.h) de las rutinas del preámbulo."""

    def __init__(self, lab):
        self.lab = lab
        self.paths = {}
        self.names = {}
        for k, v in lab.rtn.items():
            self.names.setdefault(v, k)
        self.hword = {v for k, v in lab.rtn.items() if k.startswith("h_")}

    def name(self, word):
        return self.names.get(word)

    def is_c(self, word):
        return word in self.hword

    def path(self, word):
        """Lista de (palabra, Insn) del camino probable desde `word` hasta el retorno. Las llamadas
        anidadas quedan como un `bl` (se expanden al recorrer)."""
        p = self.paths.get(word)
        if p is None:
            p = self._follow(word)
            self.paths[word] = p
        return p

    def _follow(self, word):
        pre = self.lab.pre
        out = []
        i = word
        seen = 0
        while 0 <= i < len(pre) and seen < 400:
            seen += 1
            ins = P.decode(pre[i])
            out.append((i, ins))
            if ins.cls == P.BR:
                if ins.name == "blr" or ins.name == "bctr":
                    break
                if ins.name == "bclr":
                    if ins.bo == 20 or ins.bo in LIKELY:
                        break
                elif ins.name == "b":
                    t = i + ins.imm // 4
                    if t == i or not (0 <= t < len(pre)):
                        break
                    i = t
                    continue
                elif ins.name == "bc":
                    t = i + ins.imm // 4
                    if (ins.bo in LIKELY or ins.bo == 20) and 0 <= t < len(pre) and t > i:
                        i = t
                        continue
            i += 1
        return out

    def length(self, word):
        return len(self.path(word))


def rtn_group(nm):
    """Familia de una rutina compartida por su nombre (L de lab_data)."""
    if nm is None:
        return "otra"
    if nm.startswith("fpu"):
        return "FPU"
    if nm.startswith("ktlb"):
        return "ktlb"
    return nm


class Walker:
    """Recorre el cuerpo caliente de un bloque por el camino sin salto tomado, con las rutinas
    compartidas que llama expandidas, llevando las constantes de los registros (para saber qué
    desplazamiento de cpu->… tocan los accesos indexados) y las últimas escrituras a memoria."""

    WINDOWS = (8, 16, 32)
    SIMPLE = ("li", "lis", "addi", "addis", "ori", "oris", "mr")

    def __init__(self, lab, rt, blk):
        self.lab, self.rt, self.blk = lab, rt, blk
        self.const = {}
        self.ver = collections.defaultdict(int)
        self.stores = []        # (pos, clave)
        self.pos = 0
        self.exec_words = 0     # del cuerpo
        self.callee_words = 0
        self.c_calls = 0
        self.link_sites = 0
        self.micro = collections.Counter()
        self.slow = collections.Counter()
        self.calls = collections.Counter()
        self.cwords = collections.Counter()
        self.loads = self.stores_n = self.state_ld = self.state_st = 0
        self.lhs = {w: 0 for w in self.WINDOWS}          # cpu->… y memoria del juego
        self.lhs_state = {w: 0 for w in self.WINDOWS}    # solo cpu->…
        self.lhs_by_unit = collections.Counter()
        self.state_unknown = 0
        self.lhs_region = collections.Counter()   # región del cpu->… de las cargas que alcanzaron un almacenamiento (ventana de 32)
        self.sregion = collections.Counter()    # (región de cpu->…, 'ld'|'st') → accesos
        self.unit = 0
        self.unit_words = []
        self.unit_callee = []
        self.rtn_lines = collections.defaultdict(set)   # unidad → líneas de 128 bytes del preámbulo que toca

    def _transfer(self, ins):
        n = ins.name
        c = self.const
        v = None
        if n == "li":
            v = ins.imm
        elif n == "lis":
            v = ins.imm << 16
        elif n in ("addi", "addis"):
            base = c.get(ins.ra) if ins.ra else 0
            if base is not None:
                v = base + (ins.imm << 16 if n == "addis" else ins.imm)
        elif n in ("ori", "oris"):
            base = c.get(ins.rt)
            if base is not None:
                v = base | (ins.imm << (16 if n == "oris" else 0))
        elif n == "mr":
            v = c.get(ins.rt)
        for r in ins.writes:
            self.ver[r] += 1
            c[r] = v if (n in self.SIMPLE) else None
        if n == "add" and ins.rt in c and ins.rb in c and c.get(ins.ra) is None:
            pass

    def region(self, off):
        o = self.lab.off
        if not o:
            return "cpu"
        if o["gpr"] <= off < o["gpr"] + 256:
            return "gpr"
        if o["hi"] <= off < o["hi"] + 16:
            return "hi/lo"
        if o["fpr"] <= off < o["fpr"] + 256:
            return "fpr"
        if "fcr31" in o and o["fcr31"] <= off < o["fcr31"] + 4:
            return "fcr31"
        if "rec_fw" in o and o["rec_fw"] <= off < o["rec_fw"] + 256:
            return "rec_fw"
        return "otros"

    def _key(self, ins):
        """(clase, base, lo, hi) del acceso: clase 'S' = cpu->…, 'G' = memoria del juego."""
        if ins.ra == R_CPU:
            off = ins.imm if ins.mem == "d" else self.const.get(ins.rb)
            if off is None:
                self.state_unknown += 1
                return None
            return ("S", None, off, off + ins.size)
        if ins.mem == "d":
            return ("G", (ins.ra, self.ver[ins.ra]), ins.imm, ins.imm + ins.size)
        return ("G", (ins.ra, self.ver[ins.ra], ins.rb, self.ver[ins.rb]), 0, ins.size)

    def _access(self, ins):
        key = self._key(ins)
        if ins.is_load():
            self.loads += 1
            if ins.ra == R_CPU:
                self.state_ld += 1
                if key:
                    self.sregion[(self.region(key[2]), "ld")] += 1
        else:
            self.stores_n += 1
            if ins.ra == R_CPU:
                self.state_st += 1
                if key:
                    self.sregion[(self.region(key[2]), "st")] += 1
        if key is None:
            return
        if ins.is_store():
            self.stores.append([self.pos, key, False])
            if len(self.stores) > 64:
                del self.stores[:32]
            return
        kind, base, lo, hi = key
        # Un almacenamiento detiene solo a la primera carga que lo alcanza (las siguientes ya lo
        # encuentran escrito): se cuenta un evento por almacenamiento alcanzado.
        for st in reversed(self.stores):
            p, (k2, b2, lo2, hi2), hit = st
            dist = self.pos - p
            if dist > self.WINDOWS[-1]:
                break
            if k2 != kind or b2 != base or not (lo < hi2 and lo2 < hi):
                continue
            if hit:
                break
            st[2] = True
            for w in self.WINDOWS:
                if dist <= w:
                    self.lhs[w] += 1
                    if kind == "S":
                        self.lhs_state[w] += 1
            if kind == "S":
                self.lhs_region[self.region(lo)] += 1
            self.lhs_by_unit[(self.unit, kind)] += 1
            break

    def step(self, ins, in_callee):
        self.pos += 1
        if in_callee:
            self.callee_words += 1
        else:
            self.exec_words += 1
        if ins.micro:
            self.micro[ins.name] += 1
        if ins.slow:
            self.slow[ins.name] += 1
        if ins.is_mem():
            self._access(ins)
        if ins.cls == P.BR and ins.name == "bctrl":
            self.c_calls += 1
        self._transfer(ins)

    def expand(self, tgt, depth):
        """Ejecuta la rutina compartida en `tgt` por su camino probable (las llamadas anidadas, también)."""
        if self.rt.is_c(tgt):
            self.c_calls += 1
            return
        if tgt >= self.blk.entry or depth > 3:
            return
        for (i, rins) in self.rt.path(tgt):
            self.step(rins, True)
            self.rtn_lines[self.unit].add(i >> 5)
            if rins.cls == P.BR and rins.name == "bl":
                self.expand(i + rins.imm // 4, depth + 1)

    def run(self):
        blk, lab = self.blk, self.lab
        dec = blk.decoded(lab)
        starts = {}
        units = mnem_units(blk)
        for ui, u in enumerate(units):
            starts[u[2]] = ui
        self.unit_words = [0] * len(units)
        self.unit_callee = [0] * len(units)
        exit_link = lab.rtn.get("exit_link")
        for i in range(blk.hot):
            ui = starts.get(i)
            if ui is not None:
                self.unit = ui
            ins = dec[i]
            if ins is None:
                continue
            w0, c0 = self.exec_words, self.callee_words
            tgt = lab.abs_target(blk, i) if ins.cls == P.BR and ins.name in ("b", "bl") else None
            if tgt is not None and ins.name == "bl" and tgt == exit_link:
                self.link_sites += 1       # una vez parcheado, el nop previo es el `b`: este `bl` no corre
                continue
            self.step(ins, False)
            if tgt is not None and tgt < blk.entry:
                nm = self.rt.name(tgt)
                if ins.name == "bl" or nm == "exit_jr":
                    before = self.callee_words
                    self.expand(tgt, 0)
                    self.calls[rtn_group(nm)] += 1
                    self.cwords[rtn_group(nm)] += self.callee_words - before
            if self.unit < len(self.unit_words):
                self.unit_words[self.unit] += self.exec_words - w0
                self.unit_callee[self.unit] += self.callee_words - c0


class BlockStats:
    __slots__ = ("hot_words", "cold_words", "prologue", "data_words", "walker")


def analyze_block(lab, rt, blk):
    s = BlockStats()
    s.hot_words, s.cold_words = blk.hot, blk.total - blk.hot
    s.prologue = blk.marks_h[0] if blk.marks_h and blk.marks_h[0] is not None else 0
    s.data_words = sum(1 for d in blk.decoded(lab) if d is None)
    w = Walker(lab, rt, blk)
    w.run()
    s.walker = w
    return s


def mnem_units(blk):
    """Unidades de emisión: (k, k2, hot_lo, hot_hi, cold_lo, cold_hi, how)."""
    ks = [k for k, h in enumerate(blk.marks_h) if h is not None]
    out = []
    for a, b in zip(ks, ks[1:]):
        out.append((a, b, blk.marks_h[a], blk.marks_h[b], blk.marks_c[a], blk.marks_c[b], blk.how[a]))
    return out


# ---- Todo el .lab ----------------------------------------------------------------------------

def compute(lab):
    """Métricas ponderadas. Con líneas H (lab-trace) los pesos son exactos: cada unidad de emisión
    cuenta las veces que corrió su primera instrucción; sin ellas (perfil de la PS3, muestras de
    ares) cada bloque corre entero en cada entrada."""
    rt = Rtn(lab)
    m = collections.OrderedDict()
    tot_entries = sum(b.count for b in lab.blocks)
    m["bloques"] = len(lab.blocks)
    m["entradas"] = tot_entries
    m["exacto"] = all(b.icount for b in lab.blocks) and bool(lab.blocks)
    m["instr_estaticas"] = sum(b.n for b in lab.blocks)
    m["palabras_total"] = sum(b.total for b in lab.blocks)
    m["palabras_calientes"] = sum(b.hot for b in lab.blocks)
    acc = collections.defaultdict(float)
    micro = collections.Counter()
    slow = collections.Counter()
    calls = collections.Counter()
    cwords = collections.Counter()
    by_op = collections.defaultdict(lambda: collections.defaultdict(float))
    by_how = collections.defaultdict(lambda: collections.defaultdict(float))
    tot_insns = 0.0
    sreg = collections.Counter()
    lhsreg = collections.Counter()
    for b in lab.blocks:
        s = analyze_block(lab, rt, b)
        wk = s.walker
        w = b.count
        units = mnem_units(b)
        uw = wk.unit_words
        if b.icount:
            reps = [b.icount[u[0]] if u[0] < len(b.icount) else 0 for u in units]
            insn_w = sum(b.icount)
            full = sum(uw) + sum(wk.unit_callee)
            ex = sum(r * (x + y) for r, x, y in zip(reps, uw, wk.unit_callee))
            frac = ex / (w * full) if w and full else 1.0
        else:
            reps = [w] * len(units)
            insn_w = w * b.n
            frac = 1.0
        tot_insns += insn_w
        wf = w * frac
        acc["hot_w"] += w * s.hot_words
        acc["lines_hot_w"] += w * ((s.hot_words + 31) // 32)
        acc["lines_all_w"] += w * ((b.total + 31) // 32)
        acc["cold_w"] += w * s.cold_words
        acc["exec_w"] += sum(r * x for r, x in zip(reps, uw))
        acc["callee_w"] += sum(r * y for r, y in zip(reps, wk.unit_callee))
        acc["prologue_w"] += w * s.prologue
        acc["data_words_static"] += s.data_words
        acc["link_sites_w"] += w * wk.link_sites
        acc["c_calls_w"] += wf * wk.c_calls
        acc["loads_w"] += wf * wk.loads
        acc["stores_w"] += wf * wk.stores_n
        acc["state_ld_w"] += wf * wk.state_ld
        acc["state_st_w"] += wf * wk.state_st
        acc["state_unknown_w"] += wf * wk.state_unknown
        for k, v in wk.sregion.items():
            sreg[k] += wf * v
        for k, v in wk.lhs_region.items():
            lhsreg[k] += wf * v
        for k, v in wk.lhs.items():
            acc["lhs%d_w" % k] += wf * v
            acc["lhs%d_blocks" % k] += w * (v > 0)
        for k, v in wk.lhs_state.items():
            acc["lhsS%d_w" % k] += wf * v
        for k, v in wk.micro.items():
            micro[k] += wf * v
            acc["micro_w"] += wf * v
        for k, v in wk.slow.items():
            slow[k] += wf * v
        for k, v in wk.calls.items():
            calls[k] += wf * v
        for k, v in wk.cwords.items():
            cwords[k] += wf * v
        if b.marks_c and b.marks_c[-1] is not None:
            acc["cold_fixed_static"] += (b.total - b.hot) - b.marks_c[-1]
            acc["cold_body_static"] += b.marks_c[-1]
        for ui, (k, k2, h0, h1, c0, c1, how) in enumerate(units):
            name = mips_mnem(b.mips[k])
            ninsn = k2 - k
            if ninsn == 2 and name in BRANCHES:
                name = name + "+ds"
            r = reps[ui]
            ninsn_exec = sum(b.icount[k:k2]) if b.icount else w * ninsn
            row = by_op[name]
            row["static"] += ninsn
            row["w"] += ninsn_exec
            row["hot_w"] += r * (h1 - h0)
            row["cold_w"] += r * (c1 - c0)
            row["exec_w"] += r * uw[ui]
            row["callee_w"] += r * wk.unit_callee[ui]
            row["lhs_state_w"] += r * wk.lhs_by_unit.get((ui, "S"), 0)
            row["lhs_guest_w"] += r * wk.lhs_by_unit.get((ui, "G"), 0)
            row["how"] = how
            hrow = by_how[HOW.get(how, str(how))]
            hrow["static"] += ninsn
            hrow["w"] += ninsn_exec
            hrow["hot_w"] += r * (h1 - h0)
    m["instr_ponderadas"] = tot_insns
    m["e1_entradas"] = 0.0
    for k in ("hot_w", "cold_w", "exec_w", "callee_w", "prologue_w", "link_sites_w", "c_calls_w", "loads_w", "stores_w",
              "state_ld_w", "state_st_w", "state_unknown_w", "micro_w", "lines_hot_w", "lines_all_w") + \
             tuple("lhs%d_w" % k for k in Walker.WINDOWS) + tuple("lhsS%d_w" % k for k in Walker.WINDOWS) + \
             tuple("lhs%d_blocks" % k for k in Walker.WINDOWS):
        acc.setdefault(k, 0.0)
    m.update(acc)
    m["micro"] = micro
    m["sregion"] = sreg
    m["lhsreg"] = lhsreg
    m["calls"] = calls
    m["cwords"] = cwords
    m["slow"] = slow
    m["by_op"] = by_op
    m["by_how"] = by_how
    m["rtn"] = rt
    if tot_insns:
        m["hot_por_instr"] = acc["hot_w"] / tot_insns
        m["total_por_instr"] = (acc["hot_w"] + acc["cold_w"]) / tot_insns
        m["ejec_por_instr"] = (acc["exec_w"] + acc["callee_w"] + acc["prologue_w"]) / tot_insns
        m["ejec_cuerpo_por_instr"] = (acc["exec_w"] + acc["prologue_w"]) / tot_insns
        m["callee_por_instr"] = acc["callee_w"] / tot_insns
        m["entrada_por_bloque"] = acc["prologue_w"] / tot_entries
        m["lineas_cuerpo_por_instr"] = acc["lines_hot_w"] / tot_insns
        m["lineas_todo_por_instr"] = acc["lines_all_w"] / tot_insns
    st_insns = m["instr_estaticas"]
    if st_insns:
        m["total_por_instr_estatico"] = m["palabras_total"] / st_insns
        m["hot_por_instr_estatico"] = m["palabras_calientes"] / st_insns
    return m


# Modelo de tiempo calibrado con la PS3 (docs/lab-recompilador.md, «Calibración»): el cambio relativo de
# ns por instrucción emulada ≈ A × cambio relativo de las palabras de PPC que corren por instrucción
# (cuerpo, prólogo y rutinas compartidas) + B × el de las líneas de 128 bytes del cuerpo caliente por
# instrucción. Ajustado a los dos pasos medidos de GoldenEye (ancho → compacto → base en caché + FPU
# compacta); orden de magnitud, no más (±50 % en cada paso).
MODELO_A = 0.58
MODELO_B = 0.08


def estima_dt(ma, mb):
    """Cambio relativo estimado de los ns por instrucción de a a b (negativo: b es más rápido)."""
    da = mb["ejec_por_instr"] / ma["ejec_por_instr"] - 1.0
    dl = mb["lineas_cuerpo_por_instr"] / ma["lineas_cuerpo_por_instr"] - 1.0
    return MODELO_A * da + MODELO_B * dl
