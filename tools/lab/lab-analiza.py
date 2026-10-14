#!/usr/bin/env python3
"""Análisis del código que emite el recompilador MIPS → PowerPC64 sobre un .lab
(lo escribe tools/lab/rec-lab; docs/lab-recompilador.md).

    tools/lab/lab-analiza.py reporte  juego.lab [--top N]       informe completo
    tools/lab/lab-analiza.py compara  antes.lab despues.lab     el mismo perfil con dos emisores
    tools/lab/lab-analiza.py tabla    a.lab b.lab …              una columna por archivo (calibración contra la PS3)
    tools/lab/lab-analiza.py bloque   juego.lab 7f0b05c0        un bloque desensamblado, con sus unidades
    tools/lab/lab-analiza.py ops      juego.lab                  palabras por instrucción MIPS, por opcode
    tools/lab/lab-analiza.py cache    juego.lab                  huella de código y modelo de la L1I/L2
    tools/lab/lab-analiza.py rutina   juego.lab fpu0 [n]          una rutina compartida del preámbulo
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lab_data as D      # noqa: E402
import lab_metrics as M   # noqa: E402
import lab_ppc as P       # noqa: E402

REG = ["r%d" % i for i in range(32)]


def pct(a, b):
    return 100.0 * a / b if b else 0.0


# ---- Desensamblado ----------------------------------------------------------------------------

def fmt(lab, blk, i, ins):
    """Texto de la palabra i del bloque (aproximado: para leer, no para ensamblar)."""
    w = blk.words[i]
    if ins is None:
        return ".dato  %08x   (destino del enlace)" % w
    n = ins.name
    if ins.cls == P.BR:
        if n in ("b", "bl", "bc"):
            tgt = lab.abs_target(blk, i)
            where = lab.routine_at(tgt) if tgt < blk.entry else ("+%d" % (tgt - blk.entry))
            if tgt < blk.entry and not where:
                where = "pre+%d" % tgt
            if n == "bc":
                return "bc      %d,%d,%s" % (ins.bo, ins.bi, where)
            return "%-7s %s" % (n, where)
        return n
    a, rt, ra, rb = ins, ins.rt, ins.ra, ins.rb
    if ins.cls in (P.LOAD, P.STORE) or ins.cls in (P.FLOAD, P.FSTORE):
        reg = ("f%d" if ins.cls in (P.FLOAD, P.FSTORE) else "r%d") % rt
        if ins.mem == "d":
            return "%-7s %s,%d(r%d)" % (n, reg, ins.imm, ra)
        return "%-7s %s,r%d,r%d" % (n, reg, ra, rb)
    if n in ("addi", "addis", "li", "lis", "mulli", "subfic"):
        return "%-7s r%d,%s%d" % (n, rt, "r%d," % ra if ra and n not in ("li", "lis") else "", ins.imm)
    if n in ("ori", "oris", "xori", "xoris", "andi.", "andis."):
        return "%-7s r%d,r%d,0x%x" % (n, ra, rt, ins.imm)
    if n in ("cmpwi", "cmpdi", "cmplwi", "cmpldi"):
        return "%-7s cr%d,r%d,%d" % (n, rt >> 2, ra, ins.imm)
    if n in ("cmpw", "cmpd", "cmplw", "cmpld"):
        return "%-7s cr%d,r%d,r%d" % (n, rt >> 2, ra, rb)
    if n.startswith("rlw"):
        return "%-7s r%d,r%d,%d,%d,%d" % (n, ra, rt, (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31)
    if n.startswith("rld"):
        sh = ((w >> 11) & 31) | (((w >> 1) & 1) << 5)
        mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
        return "%-7s r%d,r%d,%d,%d" % (n, ra, rt, sh, mb)
    if n in ("mflr", "mtlr", "mtctr", "mfctr", "mfcr", "mtcrf", "mtocrf"):
        return "%-7s r%d" % (n, rt)
    if ins.cls == P.CROP:
        return "%-7s %d,%d,%d" % (n, rt, ra, rb)
    if ins.cls == P.FPU:
        return "%-7s f%d,f%d,f%d" % (n, rt, ra, rb)
    if ins.rb >= 0 and n in ("add", "subf", "and", "or", "xor", "nor", "andc", "eqv", "mulld", "mulhd", "mulhdu",
                             "divd", "divdu", "slw", "srw", "sld", "srd", "sraw", "srad", "orc"):
        d, s1, s2 = (rt, ra, rb) if n in ("add", "subf", "mulld", "mulhd", "mulhdu", "divd", "divdu") else (ra, rt, rb)
        return "%-7s r%d,r%d,r%d" % (n, d, s1, s2)
    return "%-7s %08x" % (n, w)


def cmd_bloque(lab, args):
    vpc = int(args[0], 16)
    sel = [b for b in lab.blocks if b.vpc == vpc]
    if not sel:
        sys.exit("no hay un bloque en %08x" % vpc)
    for b in sel:
        dec = b.decoded(lab)
        print("bloque %08x: %d instrucciones MIPS (cubre %d bytes), %d palabras PPC (%d calientes + %d frías), FR=%d, entradas %d" %
              (b.vpc, b.n, b.bytes, b.total, b.hot, b.total - b.hot, b.fr, b.count))
        units = M.mnem_units(b)
        starts_h = {u[2]: u for u in units}
        starts_c = {u[4]: u for u in units}
        print("--- caliente")
        for i in range(b.hot):
            u = starts_h.get(i)
            if u is not None and u[2] != u[3]:
                k = u[0]
                print("   ; MIPS %d: %s%s" % (k, M.mdis.dis(b.mips[k], b.vpc + 4 * k), "  [%s]" % M.HOW.get(u[6], u[6]) if u[6] else ""))
            elif i == 0:
                print("   ; entrada")
            print("  %3d  %08x  %s" % (i, b.words[i], fmt(lab, b, i, dec[i])))
        print("--- fría")
        for i in range(b.hot, b.total):
            j = i - b.hot
            u = starts_c.get(j)
            if u is not None and u[4] != u[5]:
                print("   ; tras MIPS %d" % u[0])
            print("  %3d  %08x  %s" % (j, b.words[i], fmt(lab, b, i, dec[i])))
        print()


# ---- Informe -----------------------------------------------------------------------------------

def table(rows, heads, widths=None):
    widths = widths or [max(len(str(r[i])) for r in rows + [heads]) for i in range(len(heads))]
    line = "  ".join(("%%-%ds" % widths[0]) if i == 0 else ("%%%ds" % widths[i]) for i in range(len(heads)))
    print(line % tuple(heads))
    for r in rows:
        print(line % tuple(r))


def section(t):
    print()
    print("== " + t)


def cmd_reporte(lab, args):
    top = 25
    if "--top" in args:
        top = int(args[args.index("--top") + 1])
    m = M.compute(lab)
    print("laboratorio del recompilador —", lab.header)
    for s in lab.summary:
        print("  " + s)
    tot_entries = m["entradas"]

    section("Cobertura y tamaño")
    print("bloques traducidos: %d; entradas del perfil: %s; instrucciones MIPS ejecutadas (ponderadas): %s" %
          (m["bloques"], format(int(tot_entries), ","), format(int(m["instr_ponderadas"]), ",")))
    print("media %.1f instrucciones MIPS por entrada a un bloque (ponderada), %.1f por bloque (estática)" %
          (m["instr_ponderadas"] / tot_entries, m["instr_estaticas"] / m["bloques"]))
    print("código generado: %.1f MB (%d palabras); calientes %.0f %%; %.1f palabras por bloque" %
          (m["palabras_total"] * 4 / 1048576.0, m["palabras_total"], pct(m["palabras_calientes"], m["palabras_total"]),
           m["palabras_total"] / m["bloques"]))

    section("Palabras de PowerPC por instrucción MIPS")
    rows = [
        ("estático: todo el bloque", "%.2f" % m["total_por_instr_estatico"], "%.2f" % m["hot_por_instr_estatico"]),
        ("ponderado por entradas: todo el bloque", "%.2f" % m["total_por_instr"], "%.2f" % m["hot_por_instr"]),
        ("ponderado: lo que corre (cuerpo sin enlaces ni datos)", "%.2f" % m["ejec_cuerpo_por_instr"], ""),
        ("ponderado: más las rutinas compartidas (FPU, ktlb, camino lento, exit_jr)", "%.2f" % m["ejec_por_instr"], ""),
    ]
    table(rows, ["", "total", "calientes"])
    print("  (rutinas compartidas que llama el cuerpo: %.2f palabras por instrucción; entradas de bloque: %.1f palabras de prólogo por entrada)" %
          (m["callee_por_instr"], m["entrada_por_bloque"]))
    print("  llamadas a C desde el cuerpo caliente: %.1f por cada 1000 instrucciones; sitios de enlace: %.1f por entrada" %
          (1000.0 * m["c_calls_w"] / m["instr_ponderadas"], m["link_sites_w"] / tot_entries))

    section("Rutinas compartidas que llama el cuerpo (por cada 1000 instrucciones MIPS, ponderado)")
    rows = []
    for k, v in sorted(m["cwords"].items(), key=lambda kv: -kv[1]):
        c = m["calls"][k]
        rows.append((k, "%.1f" % (1000.0 * c / m["instr_ponderadas"]), "%.1f" % (v / c if c else 0), "%.2f" % (v / m["instr_ponderadas"])))
    table(rows, ["rutina", "llamadas", "palabras c/u", "palabras por instr."])

    section("Cómo se traducen las instrucciones (ponderado)")
    rows = []
    for k, v in sorted(m["by_how"].items(), key=lambda kv: -kv[1]["w"]):
        rows.append((k, "%.2f %%" % pct(v["w"], m["instr_ponderadas"]), "%.2f" % (v["hot_w"] / v["w"] if v["w"] else 0)))
    table(rows, ["modo", "instrucciones", "calientes/instr"])

    section("Accesos a memoria del código generado (por cada 100 instrucciones MIPS, ponderado)")
    ip = m["instr_ponderadas"]
    print("cargas %.1f (de ellas %.1f a cpu->…), almacenamientos %.1f (a cpu->…: %.1f); del MIPS: %.1f cargas + %.1f almacenamientos" %
          (100 * m["loads_w"] / ip, 100 * m["state_ld_w"] / ip, 100 * m["stores_w"] / ip, 100 * m["state_st_w"] / ip,
           100 * sum(v["w"] for k, v in m["by_op"].items() if k in ("lw", "lh", "lb", "lbu", "lhu", "lwu", "ld", "lwc1", "ldc1")) / ip,
           100 * sum(v["w"] for k, v in m["by_op"].items() if k in ("sw", "sh", "sb", "sd", "swc1", "sdc1")) / ip))

    print("  a cpu->… por región (por 100 instrucciones: cargas / almacenamientos): " +
          ", ".join("%s %.1f / %.1f" % (r, 100 * m["sregion"][(r, "ld")] / ip, 100 * m["sregion"][(r, "st")] / ip)
                    for r in ("gpr", "hi/lo", "fpr", "fcr31", "rec_fw", "otros", "cpu")
                    if m["sregion"][(r, "ld")] or m["sregion"][(r, "st")]))

    section("Microcódigo del PPE y otras instrucciones lentas (por cada 1000 instrucciones MIPS, ponderado)")
    mic = m["micro"]
    slow = m["slow"]
    if not mic and not slow:
        print("ninguna")
    for k, v in mic.most_common(12):
        print("  microcódigo  %-9s %6.2f" % (k, 1000.0 * v / ip))
    for k, v in slow.most_common(8):
        print("  lenta        %-9s %6.2f" % (k, 1000.0 * v / ip))
    print("  total microcódigo: %.2f por 1000 instrucciones (≥ 11 ciclos cada una: ~%.3f ciclos por instrucción MIPS)" %
          (1000.0 * m["micro_w"] / ip, 11.0 * m["micro_w"] / ip))

    section("Carga tras almacenamiento dentro del cuerpo (un evento por almacenamiento alcanzado; rutinas compartidas expandidas)")
    print("  eventos por 100 instrucciones MIPS (ventana de 8 / 16 / 32 instrucciones del PPE): a cpu->… %.2f / %.2f / %.2f" %
          (100 * m["lhsS8_w"] / ip, 100 * m["lhsS16_w"] / ip, 100 * m["lhsS32_w"] / ip))
    print("  entradas con alguno: %.1f %% / %.1f %% / %.1f %%; accesos a cpu->… de dirección desconocida: %.2f por 100 instrucciones" %
          (pct(m["lhs8_blocks"], tot_entries), pct(m["lhs16_blocks"], tot_entries), pct(m["lhs32_blocks"], tot_entries),
           100 * m["state_unknown_w"] / ip))
    rows = []
    totl = sum(v["lhs_state_w"] for v in m["by_op"].values())
    for k, v in sorted(m["by_op"].items(), key=lambda kv: -kv[1]["lhs_state_w"])[:10]:
        if v["lhs_state_w"]:
            rows.append((k, "%.3f" % (v["lhs_state_w"] / v["w"] if v["w"] else 0), "%.1f %%" % pct(v["lhs_state_w"], totl)))
    print("  por región de cpu->… (ventana de 32, por 100 instrucciones): " +
          ", ".join("%s %.2f" % (r, 100 * v / ip) for r, v in m["lhsreg"].most_common()))
    print("  qué instrucciones MIPS cargan lo que acaban de almacenar a cpu->… (ventana de 32), por instrucción ejecutada")
    print("  (la memoria del juego no se modela: el código generado calcula cada dirección en registros nuevos):")
    table(rows, ["opcode", "eventos/instr", "del total"])
    cross_boundary(lab, m)

    section("Por instrucción MIPS (las %d que más palabras calientes pesan)" % top)
    rows = []
    totw = sum(v["hot_w"] for v in m["by_op"].values())
    for k, v in sorted(m["by_op"].items(), key=lambda kv: -kv[1]["hot_w"])[:top]:
        rows.append((k, int(v["static"]), "%.2f %%" % pct(v["w"], ip), "%.1f" % (v["hot_w"] / v["w"] if v["w"] else 0),
                     "%.1f" % (v["exec_w"] / v["w"] if v["w"] else 0), "%.1f" % (v["cold_w"] / v["w"] if v["w"] else 0),
                     "%.1f %%" % pct(v["hot_w"], totw)))
    table(rows, ["opcode", "estáticas", "ejecutadas", "calientes", "corren", "frías", "% de lo caliente"])

    section("Bloques que más pesan (entradas × palabras calientes)")
    rows = []
    blocks = sorted(lab.blocks, key=lambda b: -b.count * b.hot)[:top]
    for b in blocks:
        rows.append(("%08x" % b.vpc, b.count, b.n, b.hot, b.total - b.hot, "%.1f" % (b.hot / b.n)))
    table(rows, ["bloque", "entradas", "instr", "calientes", "frías", "cal./instr"])

    section("Huella de código")
    cache_summary(lab)
    return m


def cross_boundary(lab, m):
    """Almacenamientos del final de un bloque y cargas de la entrada del siguiente que tocan lo mismo,
    por las aristas de lab-trace (E) o, sin ellas, por los sitios de salida del perfil de la PS3 (T)."""
    pairs = []
    if lab.edges:
        for (a, b, n) in lab.edges:
            if a < len(lab.blocks) and b < len(lab.blocks):
                pairs.append((lab.blocks[a], lab.blocks[b], n))
        src = "aristas de la traza"
    else:
        by_pc = {}
        for b in lab.blocks:
            by_pc.setdefault(b.vpc, b)
        for a in lab.blocks:
            for (k, kind, tgt, taken, nt) in a.sites:
                if kind != 1 and taken and tgt in by_pc:
                    pairs.append((a, by_pc[tgt], taken))
        src = "sitios de salida del perfil de la PS3"
    if not pairs:
        print("  (sin aristas ni sitios de salida: no hay análisis de fronteras)")
        return
    inter = tot_w = 0.0
    dist_hist = collections.Counter()
    ndist = 0
    n_cmp = 0
    tot_regs = common_regs = 0.0
    top = sorted(pairs, key=lambda t: -t[2])[:4000]    # las aristas que más pesan bastan
    for (a, b, w) in top:
        decA, decB = a.decoded(lab), b.decoded(lab)
        st = {}
        for i in range(max(0, a.hot - 40), a.hot):   # almacenamientos a cpu->… en las últimas 40 palabras de A
            ins = decA[i]
            if ins is not None and ins.cls == P.STORE and ins.ra == M.R_CPU and ins.mem == "d":
                st[ins.imm] = i
        pro = b.marks_h[0] if b.marks_h and b.marks_h[0] is not None else 0
        common = 0
        mind = None
        loads_b = 0
        for i in range(pro):                          # cargas de cpu->… en la entrada de B
            ins = decB[i]
            if ins is not None and ins.cls == P.LOAD and ins.ra == M.R_CPU and ins.mem == "d":
                loads_b += 1
                if ins.imm in st:
                    common += 1
                    d = (a.hot - st[ins.imm]) + i
                    mind = d if mind is None else min(mind, d)
        tot_w += w
        tot_regs += w * loads_b
        common_regs += w * common
        if common:
            inter += w
            dist_hist[min(mind, 40) // 4 * 4] += w
    print("  fronteras (%s): %d aristas medidas, %.1f %% de las entradas llegan por una arista donde el bloque anterior acaba de" %
          (src, len(top), pct(inter, tot_w)))
    print("  escribir a cpu->… algo que el siguiente lee en su entrada; %.2f registros en ese caso por arista (el siguiente carga %.2f en total)" %
          (common_regs / tot_w, tot_regs / tot_w))
    if dist_hist:
        print("  distancia (palabras de PPC) entre el almacenamiento y la carga: " +
              ", ".join("%d–%d: %.0f %%" % (k, k + 3, pct(v, inter)) for k, v in sorted(dist_hist.items())))
    # sesgo de las aristas: cuánto de las salidas de un bloque va a su sucesor más frecuente
    if lab.edges:
        out = collections.defaultdict(list)
        for (a, b, n) in lab.edges:
            out[a].append(n)
        tw = dom = 0
        for a, ns in out.items():
            tw += sum(ns)
            dom += max(ns)
        print("  el sucesor más frecuente de un bloque recibe el %.1f %% de sus salidas (ponderado por entradas)" % pct(dom, tw))


def cache_summary(lab):
    m_blocks = sorted(lab.blocks, key=lambda b: -b.count * 1.0)
    total_e = sum(b.count for b in lab.blocks)
    total_cost = sum(b.count * b.hot for b in lab.blocks)
    acc_e = acc_c = 0
    acc_bytes = 0
    marks = [(32, "L1I 32 KB"), (64, "64 KB"), (128, "128 KB"), (256, "256 KB (alcance del ERAT)"), (512, "L2 512 KB"), (1024, "1 MB"),
             (4096, "4 MB")]
    mi = 0
    print("  los bloques de más entradas, con su cuerpo caliente alineado a líneas de 128 bytes (frías aparte):")
    for b in m_blocks:
        acc_bytes += ((b.hot * 4 + 127) // 128) * 128
        acc_e += b.count
        acc_c += b.count * b.hot
        while mi < len(marks) and acc_bytes >= marks[mi][0] * 1024:
            print("    %-26s cubren %5.1f %% de las entradas y %5.1f %% del costo (%d bloques)" %
                  (marks[mi][1], pct(acc_e, total_e), pct(acc_c, total_cost), m_blocks.index(b) + 1))
            mi += 1
    # líneas por instrucción
    lines_hot = sum(b.count * ((b.hot * 4 + 127) // 128) for b in lab.blocks)
    lines_all = sum(b.count * ((b.total * 4 + 127) // 128) for b in lab.blocks)
    ins = sum(b.count * b.n for b in lab.blocks)
    print("  líneas de 128 bytes por entrada (cuerpo caliente solo / bloque entero): %.2f / %.2f; por instrucción MIPS: %.3f / %.3f" %
          (lines_hot / total_e, lines_all / total_e, lines_hot / ins, lines_all / ins))


def cmd_rutina(lab, args):
    """Desensambla una rutina compartida del preámbulo por su nombre (L en el .lab) o su palabra."""
    name = args[0]
    word = lab.rtn.get(name)
    if word is None:
        word = int(name, 0)
    class Pseudo:
        pass
    pb = Pseudo()
    pb.words = lab.pre
    pb.entry = 0
    n = int(args[1]) if len(args) > 1 else 80
    for i in range(word, min(word + n, len(lab.pre))):
        ins = P.decode(lab.pre[i])
        print("  %4d  %08x  %s" % (i, lab.pre[i], fmt(lab, pb, i, ins)))


def cmd_ops(lab, args):
    m = M.compute(lab)
    ip = m["instr_ponderadas"]
    rows = []
    for k, v in sorted(m["by_op"].items(), key=lambda kv: -kv[1]["w"]):
        rows.append((k, int(v["static"]), "%.2f %%" % pct(v["w"], ip), "%.1f" % (v["hot_w"] / v["w"] if v["w"] else 0),
                     "%.1f" % (v["exec_w"] / v["w"] if v["w"] else 0), "%.1f" % (v["cold_w"] / v["w"] if v["w"] else 0)))
    table(rows, ["opcode", "estáticas", "ejecutadas", "calientes", "corren", "frías"])


def cmd_cache(lab, args):
    cache_summary(lab)


# ---- Comparación -----------------------------------------------------------------------------

def lines_per_entry(lab):
    e = sum(b.count for b in lab.blocks)
    ins = sum(b.count * b.n for b in lab.blocks)
    hot = sum(b.count * ((b.hot * 4 + 127) // 128) for b in lab.blocks)
    allb = sum(b.count * ((b.total * 4 + 127) // 128) for b in lab.blocks)
    return hot / ins, allb / ins


def cmd_tabla(args):
    """Una columna por .lab con las métricas que se comparan con las medidas de la PS3."""
    cols = []
    for path in args:
        lab = D.parse(path)
        m = M.compute(lab)
        ip = m["instr_ponderadas"]
        lh, la = m["lineas_cuerpo_por_instr"], m["lineas_todo_por_instr"]
        cols.append((os.path.basename(path).replace(".lab", ""), lab.header.split()[2], m, ip, lh, la))
    rows = [
        ("palabras totales / instr.", lambda m, ip, lh, la: "%.2f" % m["total_por_instr"]),
        ("  calientes / instr.", lambda m, ip, lh, la: "%.2f" % m["hot_por_instr"]),
        ("  frías / instr.", lambda m, ip, lh, la: "%.2f" % (m["total_por_instr"] - m["hot_por_instr"])),
        ("corren (cuerpo) / instr.", lambda m, ip, lh, la: "%.2f" % m["ejec_cuerpo_por_instr"]),
        ("corren (con rutinas) / instr.", lambda m, ip, lh, la: "%.2f" % m["ejec_por_instr"]),
        ("líneas de 128 B / instr. (cuerpo)", lambda m, ip, lh, la: "%.3f" % lh),
        ("líneas de 128 B / instr. (todo)", lambda m, ip, lh, la: "%.3f" % la),
        ("cargas a cpu->… por 100 instr.", lambda m, ip, lh, la: "%.1f" % (100 * m["state_ld_w"] / ip)),
        ("almacenam. a cpu->… por 100 instr.", lambda m, ip, lh, la: "%.1f" % (100 * m["state_st_w"] / ip)),
        ("pares ld-tras-st cpu->… (ventana 16) /100", lambda m, ip, lh, la: "%.2f" % (100 * m["lhsS16_w"] / ip)),
        ("pares ld-tras-st todos (ventana 16) /100", lambda m, ip, lh, la: "%.2f" % (100 * m["lhs16_w"] / ip)),
        ("microcódigo por 1000 instr.", lambda m, ip, lh, la: "%.2f" % (1000 * m["micro_w"] / ip)),
        ("llamadas a C por 1000 instr.", lambda m, ip, lh, la: "%.2f" % (1000 * m["c_calls_w"] / ip)),
    ]
    heads = ["métrica"] + [c[0] for c in cols]
    out = []
    for (name, f) in rows:
        out.append([name] + [f(c[2], c[3], c[4], c[5]) for c in cols])
    table([tuple(r) for r in out], heads)
    if len(cols) > 1:
        print()
        print("efecto estimado en ns/instr respecto a la primera columna: " +
              ", ".join("%s %+.1f %%" % (c[0], 100 * M.estima_dt(cols[0][2], c[2])) for c in cols[1:]))


def cmd_compara(args):
    a, b = D.parse(args[0]), D.parse(args[1])
    ma, mb = M.compute(a), M.compute(b)
    print("antes:   %s  (%s)" % (args[0], a.header))
    print("después: %s  (%s)" % (args[1], b.header))
    # bloques en común (por vpc y FR)
    ka = {(x.vpc, x.fr): x for x in a.blocks}
    kb = {(x.vpc, x.fr): x for x in b.blocks}
    common = [k for k in ka if k in kb]
    print("bloques: %d antes, %d después, %d en común" % (len(ka), len(kb), len(common)))

    def row(name, fa, fb, fmtstr="%.2f"):
        d = fb - fa
        return (name, fmtstr % fa, fmtstr % fb, ("%+.1f %%" % (100.0 * d / fa)) if fa else "")

    ip_a, ip_b = ma["instr_ponderadas"], mb["instr_ponderadas"]
    rows = [
        row("palabras totales por instr. (estático)", ma["total_por_instr_estatico"], mb["total_por_instr_estatico"]),
        row("palabras calientes por instr. (estático)", ma["hot_por_instr_estatico"], mb["hot_por_instr_estatico"]),
        row("totales por instr. (ponderado)", ma["total_por_instr"], mb["total_por_instr"]),
        row("calientes por instr. (ponderado)", ma["hot_por_instr"], mb["hot_por_instr"]),
        row("corren por instr. (cuerpo)", ma["ejec_cuerpo_por_instr"], mb["ejec_cuerpo_por_instr"]),
        row("corren por instr. (con rutinas)", ma["ejec_por_instr"], mb["ejec_por_instr"]),
        row("prólogo por entrada", ma["entrada_por_bloque"], mb["entrada_por_bloque"]),
        row("llamadas a C por 1000 instr.", 1000 * ma["c_calls_w"] / ip_a, 1000 * mb["c_calls_w"] / ip_b),
        row("microcódigo por 1000 instr.", 1000 * ma["micro_w"] / ip_a, 1000 * mb["micro_w"] / ip_b),
        row("cargas+almacenamientos a cpu->… por 100 instr.", 100 * (ma["state_ld_w"] + ma["state_st_w"]) / ip_a,
            100 * (mb["state_ld_w"] + mb["state_st_w"]) / ip_b),
        row("pares carga-tras-almacén (ventana 16) por 100 instr.", 100 * ma["lhs16_w"] / ip_a, 100 * mb["lhs16_w"] / ip_b),
        row("líneas de 128 B del cuerpo por instr.", ma["lineas_cuerpo_por_instr"], mb["lineas_cuerpo_por_instr"], "%.3f"),
        row("tamaño del código (MB)", ma["palabras_total"] * 4 / 1048576.0, mb["palabras_total"] * 4 / 1048576.0),
    ]
    table(rows, ["métrica", "antes", "después", "cambio"])
    print()
    print("Efecto estimado en los ns por instrucción emulada: %+.1f %%   (modelo de 2 parámetros calibrado con la PS3; ±50 %% por paso, docs/lab-recompilador.md)" %
          (100 * M.estima_dt(ma, mb)))

    # por opcode
    print()
    print("Por instrucción MIPS (palabras calientes por instrucción ejecutada; los cambios más grandes ponderados por entradas)")
    names = set(ma["by_op"]) | set(mb["by_op"])
    deltas = []
    for n in names:
        va, vb = ma["by_op"].get(n), mb["by_op"].get(n)
        if not va or not vb or not va["w"] or not vb["w"]:
            continue
        ha, hb = va["hot_w"] / va["w"], vb["hot_w"] / vb["w"]
        weight = (va["w"] + vb["w"]) / 2
        deltas.append((abs(hb - ha) * weight, n, ha, hb, weight / ip_a))
    rows = [(n, "%.1f" % ha, "%.1f" % hb, "%+.1f" % (hb - ha), "%.2f %%" % (100 * sh)) for (_, n, ha, hb, sh) in sorted(deltas, reverse=True)[:15]]
    table(rows, ["opcode", "antes", "después", "cambio", "de las instr."])

    # por bloque (mismo perfil)
    print()
    print("Bloques que más cambian (entradas × palabras calientes)")
    d = []
    for k in common:
        xa, xb = ka[k], kb[k]
        d.append(((xb.hot - xa.hot) * xa.count, k, xa, xb))
    rows = []
    for (v, k, xa, xb) in sorted(d, key=lambda t: t[0])[:6] + sorted(d, key=lambda t: -t[0])[:6]:
        rows.append(("%08x" % k[0], xa.count, xa.n, xa.hot, xb.hot, xa.total - xa.hot, xb.total - xb.hot, "%+d" % ((xb.hot - xa.hot))))
    table(rows, ["bloque", "entradas", "instr", "cal. antes", "cal. después", "frías antes", "frías después", "Δ calientes"])


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "compara":
        cmd_compara(args)
        return
    if cmd == "tabla":
        cmd_tabla(args)
        return
    lab = D.parse(args[0])
    {"reporte": cmd_reporte, "bloque": cmd_bloque, "ops": cmd_ops, "cache": cmd_cache, "rutina": cmd_rutina}[cmd](lab, args[1:])


if __name__ == "__main__":
    main()
