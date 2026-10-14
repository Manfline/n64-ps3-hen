#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Salsa64 — lector de las sesiones de perfilado (docs/perfilado.md).

Lee los archivos <n>.sesion que deja la app «Salsa64 perfil» en
/dev_hdd0/salsa64/perfilado/<juego>-<CRC>/ (o salsa64-run --perfilado) y
los resume en texto: velocidad, tirones y sus causas, el recompilador
(palabras por instrucción y por clase, bloques que más tiempo se llevan,
retraducciones), JR y la pila de regresos simulada sobre las trazas, lo raro
de los gráficos y el audio. También compara dos sesiones (antes y después)
para encontrar regresiones, y exporta las trazas para el laboratorio de la
caché del PPE.

  tools/perfilado/perfilado.py resumen RUTA...          una sesión, o todas las de una carpeta
  tools/perfilado/perfilado.py comparar ANTES DESPUÉS   dos sesiones (o las últimas de dos carpetas)
  tools/perfilado/perfilado.py juegos RAÍZ [--antes F] [--despues F]
                                                        por juego: la última sesión contra la anterior
  tools/perfilado/perfilado.py traza RUTA [-o ARCHIVO]  las ventanas de la traza, una línea por entrada
  tools/perfilado/perfilado.py json RUTA                todo lo leído, en JSON

Solo la biblioteca estándar. El archivo es big-endian (el de la PS3).
"""
import argparse
import json
import os
import statistics
import struct
import sys

MAGIC = b"S64P"

R_CONTEXT, R_NAMES, R_WINDOW, R_STALL, R_BLOCKS, R_EVICT, R_TRACE, R_JR, R_SAMPLES, R_TASKS, R_DUMP, R_END, \
    R_LOST, R_EVENT = range(1, 15)

CLASSES = ["entrada", "alu", "memoria", "fpu", "saltos", "salida", "otras"]
ENDS = ["tope", "jr ra", "jr", "jalr", "jal", "j", "condicional", "likely", "a C"]
END_LOOP = 0x80
SMP_MARKS = ["nativo", "camino lento", "despachador", "intérprete", "eventos", "compilar", "espera", "ritmo", "otro"]
EXIT_KINDS = {1: "enlace", 2: "a mitad", 3: "salto calculado", 4: "espera", 5: "en la entrada", 6: "fin (C)",
              7: "Status.FR", 8: "excepción/ERET"}
M_EXIT, M_COMPILE, M_INTERP, M_EVENT, M_IDLE = 0x81, 0x82, 0x83, 0x84, 0x85
GFX_RES_CUT = 0x02 | 0x04 | 0x08 | 0x10 | 0x40
EVICT_KINDS = {0: "tramo frío", 1: "mitad caliente", 2: "vaciado"}
FLUSH_CAUSES = ["ids", "enlaces", "RDRAM", "bucles", "ROM", "estado", "otros"]
DUMP_WHY = {1: "comando desconocido", 2: "combiner aproximado nuevo", 3: "tarea cortada"}


def end_name(e):
    name = ENDS[e & 0x7F] if (e & 0x7F) < len(ENDS) else "?"
    return name + (" (bucle)" if e & END_LOOP else "")


def parse_kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


class Session:
    def __init__(self, path):
        self.path = path
        self.context = {}
        self.names = {}
        self.windows = []
        self.stalls = []
        self.blocks = []
        self.evicts = []
        self.traces = []
        self.jr = []
        self.jr_full = 0
        self.samples = None
        self.tasks = []
        self.dumps = []
        self.end = {}
        self.events = []
        self.lost = 0
        self.truncated = False
        self._read()

    # ---- lectura ------------------------------------------------------------------
    def _read(self):
        with open(self.path, "rb") as f:
            data = f.read()
        if len(data) < 16 or data[:4] != MAGIC:
            raise ValueError(f"{self.path}: no es una sesión de perfilado (falta «S64P»)")
        version, header = struct.unpack_from(">HH", data, 4)
        if version != 1:
            raise ValueError(f"{self.path}: versión {version} desconocida")
        pos = header
        pending = []
        while pos + 4 <= len(data):
            (word,) = struct.unpack_from(">I", data, pos)
            kind, length = word >> 24, word & 0xFFFFFF
            if pos + 4 + length > len(data):
                self.truncated = True
                break
            pending.append((kind, data[pos + 4:pos + 4 + length]))
            pos += 4 + length
        if pos != len(data):
            self.truncated = True
        # Los nombres primero: las ventanas los necesitan.
        for kind, body in pending:
            if kind == R_NAMES:
                for k, v in parse_kv(body[4:].decode("utf-8", "replace")).items():
                    self.names[k] = v.split(",")
        for kind, body in pending:
            try:
                self._record(kind, body)
            except struct.error:
                self.truncated = True

    def _values(self, body, names):
        ms, n = struct.unpack_from(">IH", body, 0)
        vals = struct.unpack_from(f">{n}I", body, 8)
        d = {"ms": ms}
        for k, v in enumerate(vals):
            d[names[k] if k < len(names) else f"campo{k}"] = v
        return d

    def _record(self, kind, b):
        if kind == R_CONTEXT:
            self.context.update(parse_kv(b[4:].decode("utf-8", "replace")))
        elif kind == R_WINDOW:
            self.windows.append(self._values(b, self.names.get("ventana", [])))
        elif kind == R_STALL:
            self.stalls.append(self._values(b, self.names.get("tiron", [])))
        elif kind == R_BLOCKS:
            size, count = struct.unpack_from(">HH", b, 0)
            for k in range(count):
                self.blocks.append(self._block(b, 4 + k * size))
        elif kind == R_EVICT:
            ms, field, kind2, seg, _, killed = struct.unpack_from(">IIBBHI", b, 0)
            self.evicts.append({"ms": ms, "campo": field, "tipo": kind2, "tramo": seg, "muertos": killed})
        elif kind == R_TRACE:
            ms, field, cycles, n, ntab, tab_full = struct.unpack_from(">IIQIII", b, 0)
            entries = list(struct.unpack_from(f">{n}I", b, 28))
            tab = {}
            off = 28 + 4 * n
            for k in range(ntab):
                bid, nn, end, vpc, code, hot, cold, target, nbr = struct.unpack_from(">HBBIIHHIB", b, off + 24 * k)
                tab[bid] = {"vpc": vpc, "code": code, "n": nn, "end": end, "hot": hot, "cold": cold,
                            "destino": target, "saltos": nbr}
            self.traces.append({"ms": ms, "campo": field, "ciclos": cycles, "entradas": entries, "tabla": tab,
                                "tabla_llena": tab_full})
        elif kind == R_JR:
            ms, n, full = struct.unpack_from(">III", b, 0)
            self.jr_full = full
            for k in range(n):
                vpc, count, rep, end, nn, _ = struct.unpack_from(">IIIBBH", b, 12 + 16 * k)
                self.jr.append({"vpc": vpc, "jr": count, "repetidos": rep, "end": end, "n": nn})
        elif kind == R_SAMPLES:
            ms, ticks, nm = struct.unpack_from(">IQI", b, 0)
            marks = list(struct.unpack_from(f">{nm}I", b, 16))
            off = 16 + 4 * nm
            unknown, full, n = struct.unpack_from(">III", b, off)
            off += 12
            table = []
            for k in range(n):
                pc, smp, nn, words, hot, _ = struct.unpack_from(">IIHHHH", b, off + 16 * k)
                table.append({"vpc": pc, "muestras": smp, "n": nn, "palabras": words, "calientes": hot})
            self.samples = {"ticks": ticks, "marcas": marks, "sin_id": unknown, "sin_lugar": full, "tabla": table}
        elif kind == R_TASKS:
            size, count = struct.unpack_from(">HH", b, 0)
            for k in range(count):
                self.tasks.append(self._task(b, 4 + k * size))
        elif kind == R_DUMP:
            ms, field, task, why, what = struct.unpack_from(">IIIII", b, 0)
            self.dumps.append({"ms": ms, "campo": field, "tarea": task, "motivo": DUMP_WHY.get(why, str(why)),
                               "que": what, "archivo": b[20:].decode("utf-8", "replace")})
        elif kind == R_END:
            self.end = parse_kv(b[4:].decode("utf-8", "replace"))
        elif kind == R_LOST:
            ring, n = struct.unpack_from(">II", b, 0)
            self.lost += n
        elif kind == R_EVENT:
            (ms,) = struct.unpack_from(">I", b, 0)
            self.events.append({"ms": ms, "texto": b[4:].decode("utf-8", "replace")})

    @staticmethod
    def _block(b, o):
        ms, field, vpc, pa, code, ticks, h = struct.unpack_from(">IIIIIIQ", b, o)
        bid, hot, cold, n, end, seg, flags = struct.unpack_from(">HHHBBBB", b, o + 32)
        ins = list(b[o + 42:o + 49])
        whot = list(struct.unpack_from(">7H", b, o + 50))
        wcold = list(struct.unpack_from(">7H", b, o + 64))
        target, nbr = struct.unpack_from(">IB", b, o + 80)
        return {"ms": ms, "campo": field, "vpc": vpc, "pa": pa, "code": code, "ticks": ticks, "huella": h, "id": bid,
                "hot": hot, "cold": cold, "n": n, "end": end, "seg": seg, "flags": flags, "ins": ins, "whot": whot,
                "wcold": wcold, "destino": target, "saltos": nbr}

    @staticmethod
    def _task(b, o):
        f = struct.unpack_from(">IIIIIHHHBBHHHHHHHHBB", b, o)
        keys = ["ms", "campo", "tarea", "comandos", "triangulos", "dibujos", "texturas", "desconocidos",
                "ultimo_desconocido", "flags", "aproximados", "dibujos_aproximados", "saltados", "errores_dl",
                "spe_us", "traduccion_us", "espera_juego_us", "fp_cambios", "copias", "nuevos"]
        return dict(zip(keys, f))

    # ---- cuentas --------------------------------------------------------------------
    def name(self):
        c = self.context
        return f"{c.get('juego', '?')} ({c.get('carpeta', '?')}) sesión {os.path.basename(self.path)}"

    def duration_s(self):
        if "duracion_ms" in self.end:
            return int(self.end["duracion_ms"]) / 1000
        if self.windows:
            return self.windows[-1]["ms"] / 1000
        return 0.0


# ---- tirones ------------------------------------------------------------------------

STALL_CAUSES = [
    ("compilar_us", "traducción"),
    ("espera_spe_us", "espera al SPE"),
    ("espera_juego_us", "espera al RSX (juego de búferes)"),
    ("copias_espera_us", "copia a la RDRAM"),
    ("copia_vi_us", "copia del VI"),
    ("jpeg_us", "JPEG"),
]


def stall_cause(s):
    """La causa que más µs explica del campo; si ninguna llega al 30 %, el CPU
    (con la categoría del muestreo que más pesó, si lo hay)."""
    emu = max(s.get("emu_us", 1), 1)
    best, best_us = None, 0
    for key, name in STALL_CAUSES:
        v = s.get(key, 0)
        if v > best_us:
            best, best_us = name, v
    if best and best_us >= 0.3 * emu:
        return best, best_us
    marks = [(s.get("m_" + k, 0), k) for k in ["nativo", "lento", "despachador", "interprete", "eventos",
                                                 "compilar", "espera", "otro"]]
    if s.get("m_total"):
        top = max(marks)
        if top[0]:
            return f"CPU ({top[1]})", 0
    return "CPU", 0


def stall_summary(ses):
    by = {}
    for s in ses.stalls:
        cause, _ = stall_cause(s)
        d = by.setdefault(cause, [0, 0.0])
        d[0] += 1
        d[1] += s.get("emu_us", 0) / 1000
    return by


# ---- recompilador ----------------------------------------------------------------------

def latest_blocks(ses):
    """El último bloque compilado en cada vpc (lo que pesa el muestreo)."""
    out = {}
    for b in ses.blocks:
        if b["code"]:
            out[b["vpc"]] = b
    return out


def class_totals(blocks, weights=None):
    ins = [0] * 7
    hot = [0] * 7
    cold = [0] * 7
    for b in blocks:
        w = 1 if weights is None else weights.get(b["vpc"], 0)
        if not w:
            continue
        for k in range(7):
            ins[k] += b["ins"][k] * w
            hot[k] += b["whot"][k] * w
            cold[k] += b["wcold"][k] * w
    return ins, hot, cold


# ---- JR y pila de regresos -------------------------------------------------------------

def ras_from_traces(ses, depths=(4, 8, 16, 1 << 30)):
    """Simula una pila de regresos sobre las ventanas: un bloque que termina en
    JAL/JALR apila vpc + 4·n (la vuelta tras su delay slot); uno que termina
    en «jr ra» compara la entrada siguiente con la cima. Un bloque con
    condicionales internos pudo salir antes de su salto final: un JAL cuenta
    solo si la entrada siguiente es su destino, un JALR o un «jr ra» solo si el
    bloque no tiene otros saltos (los demás «jr ra» van aparte, como
    dudosos). Las vueltas a C en medio no cortan la cuenta (el destino es el
    mismo). Un retorno con la pila vacía (de una llamada anterior a la
    ventana) no se juzga. Devuelve {profundidad: [aciertos, jr ra juzgados,
    con la pila vacía]}, los dudosos y [JR al mismo destino que la vez
    anterior, JR]."""
    res = {d: [0, 0, 0] for d in depths}
    repeat = [0, 0]
    doubtful = 0
    for t in ses.traces:
        tab = t["tabla"]
        stacks = {d: [] for d in depths}
        last_target = {}
        prev = None
        for e in t["entradas"]:
            if e >> 24 >= 0x80 or not e:
                continue
            blk = tab.get(e)
            if prev is not None and blk is not None:
                pend = prev["end"] & 0x7F
                clean = prev.get("saltos", 1) <= 1
                call = (pend == 4 and blk["vpc"] == prev.get("destino")) or (pend == 3 and clean)
                if call:
                    for d in depths:
                        st = stacks[d]
                        st.append(prev["vpc"] + 4 * prev["n"])
                        if len(st) > d:
                            st.pop(0)
                elif pend == 1 and clean:  # jr ra
                    for d in depths:
                        st = stacks[d]
                        if not st:
                            res[d][2] += 1
                            continue
                        res[d][0] += st.pop() == blk["vpc"]
                        res[d][1] += 1
                elif pend == 1:
                    doubtful += 1
                if pend in (1, 2, 3) and clean:
                    key = prev["vpc"]
                    repeat[1] += 1
                    repeat[0] += last_target.get(key) == blk["vpc"]
                    last_target[key] = blk["vpc"]
            prev = blk
    return res, doubtful, repeat


def trace_stats(ses):
    entries = exits = compiles = interp = 0
    distinct = {}
    for t in ses.traces:
        for e in t["entradas"]:
            top = e >> 24
            if top == M_EXIT:
                exits += 1
            elif top == M_COMPILE:
                compiles += 1
            elif top == M_INTERP:
                interp += e & 0xFFFFFF
            elif e and top < 0x80:
                entries += 1
        for bid, b in t["tabla"].items():
            distinct[(b["vpc"], b["code"])] = b
    foot = sum(4 * b["hot"] for b in distinct.values())
    foot_all = sum(4 * (b["hot"] + b["cold"]) for b in distinct.values())
    return entries, exits, compiles, interp, len(distinct), foot, foot_all


# ---- gráficos ---------------------------------------------------------------------------------

def gfx_summary(ses):
    t = ses.tasks
    out = {"tareas": len(t)}
    if not t:
        return out
    ops = {}
    for x in t:
        if x["desconocidos"]:
            ops[x["ultimo_desconocido"]] = ops.get(x["ultimo_desconocido"], 0) + 1
    out["desconocidos"] = ops
    out["con_aproximados"] = sum(1 for x in t if x["aproximados"])
    out["dibujos_aproximados"] = sum(x["dibujos_aproximados"] for x in t)
    out["saltados"] = sum(x["saltados"] for x in t)
    out["errores_dl"] = sum(x["errores_dl"] for x in t)
    out["cortadas"] = sum(1 for x in t if x["flags"] & GFX_RES_CUT)
    out["esperas_juego"] = sum(1 for x in t if x["espera_juego_us"])
    out["espera_juego_ms"] = sum(x["espera_juego_us"] for x in t) / 1000
    out["copias"] = sum(x["copias"] for x in t)
    for k in ("dibujos", "triangulos", "spe_us", "traduccion_us", "fp_cambios"):
        out[k + "_medio"] = sum(x[k] for x in t) / len(t)
    out["traduccion_us_max"] = max(x["traduccion_us"] for x in t)
    out["aproximados_nuevos"] = sorted(e["texto"].split()[0].split("=")[1] for e in ses.events
                                       if e["texto"].startswith("aproximado="))
    out["desconocidos_nuevos"] = sorted(e["texto"].split()[0].split("=")[1] for e in ses.events
                                        if e["texto"].startswith("desconocido="))
    return out


# ---- métricas para comparar ---------------------------------------------------------------------

def pct(values, q):
    if not values:
        return 0.0
    v = sorted(values)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def metrics(ses):
    """Lo que compara «comparar»: una cifra por renglón."""
    w = [x for x in ses.windows if x.get("campos")]
    cps = [x["cps_x100"] / 100 for x in w]
    m = {}
    m["duración (s)"] = ses.duration_s()
    m["campos/s, mediana"] = statistics.median(cps) if cps else 0
    m["campos/s, p10"] = pct(cps, 0.1)
    m["campos/s, mínimo"] = min(cps) if cps else 0
    m["MIPS, mediana"] = statistics.median([x["mips_x10"] / 10 for x in w]) if w else 0
    m["emulación ms/campo, mediana"] = statistics.median([x["emu_us_campo"] / 1000 for x in w]) if w else 0
    dur = max(ses.duration_s(), 1e-9)
    m["tirones por minuto"] = len(ses.stalls) * 60 / dur
    m["tirones: ms totales"] = sum(s.get("emu_us", 0) for s in ses.stalls) / 1000
    for cause, (n, ms) in sorted(stall_summary(ses).items()):
        m[f"tirones por {cause}"] = n
    m["esperas por juego de búferes"] = sum(x.get("esperas_juego", 0) for x in w)
    m["espera por juego de búferes (ms)"] = sum(x.get("espera_juego_us", 0) for x in w) / 1000
    m["copias a la RDRAM"] = sum(x.get("copias_rdram", 0) for x in w)
    m["copias esperadas por la emulación"] = sum(x.get("copias_esperadas", 0) for x in w)
    m["espera por copias (ms)"] = sum(x.get("copias_espera_us", 0) for x in w) / 1000
    m["tareas cortadas"] = sum(x.get("tareas_cortadas", 0) for x in w)
    m["comandos saltados (otro microcódigo)"] = sum(x.get("comandos_saltados", 0) for x in w)
    g = gfx_summary(ses)
    m["tareas con desconocidos"] = sum(g.get("desconocidos", {}).values())
    m["tareas con combiners aproximados"] = g.get("con_aproximados", 0)
    m["combiners aproximados distintos"] = len(g.get("aproximados_nuevos", []))
    m["audio: % en silencio"] = (sum(x.get("audio_silencio_pm", 0) * x.get("audio_salida_ms", 0) for x in w) /
                                 max(1, sum(x.get("audio_salida_ms", 0) for x in w))) / 10
    m["audio: faltas"] = sum(x.get("audio_faltas", 0) for x in w)
    m["presentados/s"] = sum(x.get("presentados", 0) for x in w) / dur
    blocks = latest_blocks(ses)
    ins, hot, cold = class_totals(blocks.values())
    m["palabras por instrucción (todas)"] = (sum(hot) + sum(cold)) / max(1, sum(ins))
    m["retraducciones"] = sum(1 for b in ses.blocks if b["flags"] & 1)
    m["desalojos de tramos"] = sum(1 for e in ses.evicts if e["tipo"] != 2)
    m["bytes perdidos"] = ses.lost
    return m


# ---- texto ----------------------------------------------------------------------------------------

def fmt(v):
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def summary(ses, out, top=15):
    p = lambda *a: print(*a, file=out)
    c = ses.context
    p(f"== {ses.name()}")
    p(f"   {c.get('app', '?')} {c.get('version', '?')} ({c.get('appid', '?')}); {c.get('tv', '?')} región "
      f"{c.get('region', '?')}, escala {c.get('escala', '?')}, filtro {c.get('filtro', '?')}, recompilador "
      f"{c.get('recompilador', '?')}, traza {c.get('traza', '?')}")
    p(f"   {ses.duration_s():.0f} s, {ses.end.get('campos', '?')} campos, salida {ses.end.get('salida', '?')}"
      + (", ARCHIVO CORTADO (se leyó hasta el último registro completo)" if ses.truncated else "")
      + (f", {ses.lost} bytes perdidos en los anillos" if ses.lost else ""))

    w = [x for x in ses.windows if x.get("campos")]
    if w:
        cps = [x["cps_x100"] / 100 for x in w]
        p(f"-- velocidad ({len(w)} ventanas): campos/s mediana {statistics.median(cps):.1f}, p10 {pct(cps, .1):.1f}, "
          f"mín {min(cps):.1f}; MIPS {statistics.median([x['mips_x10'] / 10 for x in w]):.1f}; en espera "
          f"{statistics.median([x['espera_pm'] / 10 for x in w]):.0f} %; emulación "
          f"{statistics.median([x['emu_us_campo'] / 1000 for x in w]):.1f} ms/campo (peor "
          f"{max(x.get('emu_peor_us', 0) for x in w) / 1000:.0f} ms)")
        pres = sum(x.get("presentados", 0) for x in w)
        p(f"   presentados {pres}, saltados {sum(x.get('saltados', 0) for x in w)}, descartados "
          f"{sum(x.get('descartados', 0) for x in w)}; cola {statistics.median([x.get('cola_x100', 0) / 100 for x in w]):.2f}"
          f" campos, antigüedad {statistics.median([x.get('edad_x100', 0) / 100 for x in w]):.2f}; hilo 1 "
          f"{statistics.median([x.get('hilo1_pm', 0) / 10 for x in w]):.0f} %")
        out_ms = sum(x.get("audio_salida_ms", 0) for x in w)
        if out_ms:
            p(f"-- audio: {sum(x.get('audio_silencio_pm', 0) * x.get('audio_salida_ms', 0) for x in w) / out_ms / 10:.1f} % "
              f"en silencio, {sum(x.get('audio_estirado_pm', 0) * x.get('audio_salida_ms', 0) for x in w) / out_ms / 10:.1f} % "
              f"estirado, {sum(x.get('audio_faltas', 0) for x in w)} faltas, {sum(x.get('audio_saltos', 0) for x in w)} "
              f"saltos")

    p(f"-- tirones: {len(ses.stalls)} campos de más de {ses.context.get('perfilado', '').split('tiron_ms=')[-1].split()[0] if 'tiron_ms=' in ses.context.get('perfilado', '') else '?'} ms")
    for cause, (n, ms) in sorted(stall_summary(ses).items(), key=lambda kv: -kv[1][1]):
        p(f"   {cause:38s} {n:6d} campos, {ms:9.0f} ms")
    for s in sorted(ses.stalls, key=lambda s: -s.get("emu_us", 0))[:top]:
        cause, us = stall_cause(s)
        extra = []
        if s.get("compilados"):
            extra.append(f"{s['compilados']} bloques compilados en {s.get('compilar_us', 0) / 1000:.1f} ms"
                         + (f" ({s['retrad']} retraducciones)" if s.get("retrad") else ""))
        if s.get("desalojos") or s.get("vaciados"):
            extra.append(f"{s.get('desalojos', 0)} desalojos, {s.get('vaciados', 0)} vaciados")
        for key, name in STALL_CAUSES[1:]:
            if s.get(key, 0) >= 1000:
                extra.append(f"{name} {s[key] / 1000:.1f} ms")
        if s.get("interp", 0) > 10000:
            extra.append(f"{s['interp']} instr. interpretadas")
        if s.get("pi_dma_kb", 0) >= 64:
            extra.append(f"{s['pi_dma_kb']} KB por DMA del cartucho")
        if s.get("invalidados", 0) >= 16:
            extra.append(f"{s['invalidados']} bloques invalidados")
        p(f"   campo {s.get('campo', 0):7d}  {s.get('emu_us', 0) / 1000:6.1f} ms  {cause}: " + "; ".join(extra))

    if ses.blocks:
        blocks = latest_blocks(ses)
        retr = sum(1 for b in ses.blocks if b["flags"] & 1)
        words = sum(b["hot"] + b["cold"] for b in ses.blocks)
        p(f"-- recompilador: {len(ses.blocks)} compilaciones ({len(blocks)} direcciones vivas al final), {retr} "
          f"retraducciones ({100 * retr / max(1, len(ses.blocks)):.1f} %), {words * 4 / 1048576:.1f} MB generados; "
          f"{sum(1 for e in ses.evicts if e['tipo'] == 0)} desalojos de tramos, "
          f"{sum(1 for e in ses.evicts if e['tipo'] == 2)} vaciados")
        tick_hz = 79.8e6
        ct = [b["ticks"] for b in ses.blocks if b["ticks"]]
        if ct:
            p(f"   compilar un bloque: mediana {statistics.median(ct) / tick_hz * 1e6:.0f} µs, p99 "
              f"{pct(ct, .99) / tick_hz * 1e6:.0f} µs (timebase de la PS3)")
        ins, hot, cold = class_totals(blocks.values())
        p(f"   palabras por instrucción (bloques vivos, sin pesar): {(sum(hot) + sum(cold)) / max(1, sum(ins)):.1f} "
          f"({sum(hot) / max(1, sum(ins)):.1f} calientes)")
        weights = None
        if ses.samples:
            weights = {e["vpc"]: e["muestras"] for e in ses.samples["tabla"]}
            wins, whot, wcold = class_totals(blocks.values(), weights)
        p("   por clase          instr.   palabras calientes/instr.  frías/instr." +
          ("   | pesado por tiempo: instr. %  calientes/instr." if weights else ""))
        for k, name in enumerate(CLASSES):
            line = (f"   {name:16s} {ins[k]:8d}   {hot[k] / max(1, ins[k]) if k not in (0, 5) else hot[k] / max(1, sum(ins)):8.1f}"
                    f"{'':17s} {cold[k] / max(1, ins[k]) if k not in (0, 5) else cold[k] / max(1, sum(ins)):6.1f}")
            if weights:
                tot = max(1, sum(wins))
                line += (f"   | {100 * wins[k] / tot:6.1f}  "
                         f"{whot[k] / max(1, wins[k]) if k not in (0, 5) else whot[k] / tot:8.1f}")
            p(line)
        p("   (entrada y salida van por instrucción del bloque entero)")
    if ses.samples:
        s = ses.samples
        tot = max(1, s["ticks"])
        other = sum(s["marcas"][1:])
        p(f"-- muestreo: {s['ticks']} muestras; nativo {100 * (tot - other) / tot:.1f} %" +
          "".join(f", {SMP_MARKS[k]} {100 * s['marcas'][k] / tot:.1f}" for k in range(1, len(s["marcas"]))
                  if k < len(SMP_MARKS)))
        blocks = latest_blocks(ses)
        for e in sorted(s["tabla"], key=lambda e: -e["muestras"])[:top]:
            b = blocks.get(e["vpc"])
            p(f"   {e['vpc']:08x}  {100 * e['muestras'] / tot:5.2f} %  {e['n']:3d} instr.  {e['palabras']:5d} palabras "
              f"({e['calientes']} calientes, {e['palabras'] / max(1, e['n']):.1f}/instr.)" +
              (f"  termina en {end_name(b['end'])}" if b else ""))

    if ses.jr:
        total = sum(j["jr"] for j in ses.jr)
        rep = sum(j["repetidos"] for j in ses.jr)
        cyc = int(ses.end.get("ciclos", 0))
        busy = cyc - int(ses.end.get("ciclos_espera", 0))
        rate = (f"{total / (cyc / 93.75e6):.0f} por segundo de consola, {1000 * total / busy:.1f} por mil instrucciones"
                if cyc and busy > 0 else f"{total / max(ses.duration_s(), 1e-9):.0f} por segundo")
        p(f"-- JR: {total} ({rate}), {len(ses.jr)} sitios; al mismo destino que la vez "
          f"anterior {100 * rep / max(1, total):.1f} %" + (f"; {ses.jr_full} sin lugar en la tabla" if ses.jr_full else ""))
        for j in sorted(ses.jr, key=lambda j: -j["jr"])[:top]:
            p(f"   {j['vpc']:08x}  {j['jr']:10d}  repetido {100 * j['repetidos'] / max(1, j['jr']):5.1f} %  "
              f"{end_name(j['end'])}, {j['n']} instr.")
    if ses.traces:
        entries, exits, compiles, interp, distinct, foot, foot_all = trace_stats(ses)
        p(f"-- traza: {len(ses.traces)} ventanas, {entries} entradas a bloques, {exits} vueltas a C "
          f"({1000 * exits / max(1, entries):.0f} por mil entradas), {compiles} compilaciones, {interp} instr. "
          f"interpretadas; {distinct} bloques distintos: {foot / 1024:.0f} KB calientes ({foot_all / 1024:.0f} KB con "
          f"lo frío)")
        ras, doubtful, rep = ras_from_traces(ses)
        line = ", ".join(f"{'sin tope' if d > 1000 else d}: {100 * h / max(1, n):.1f} % ({e} con la pila vacía)"
                         for d, (h, n, e) in sorted(ras.items()))
        n = ras[max(ras)][1]
        p(f"   pila de regresos simulada sobre {n} «jr ra» de bloques sin otros saltos ({doubtful} dudosos aparte), "
          f"aciertos por profundidad: {line}; JR al destino anterior {100 * rep[0] / max(1, rep[1]):.1f} %")

    g = gfx_summary(ses)
    if g.get("tareas"):
        p(f"-- gráficos: {g['tareas']} tareas; por tarea {g['dibujos_medio']:.0f} dibujos, {g['triangulos_medio']:.0f} "
          f"triángulos, SPE {g['spe_us_medio'] / 1000:.2f} ms, traducción al RSX {g['traduccion_us_medio'] / 1000:.2f} ms "
          f"(peor {g['traduccion_us_max'] / 1000:.1f}), {g['fp_cambios_medio']:.1f} cambios de programa")
        p(f"   esperas por juego de búferes: {g['esperas_juego']} tareas, {g['espera_juego_ms']:.0f} ms; "
          f"{g['copias']} copias a la RDRAM al traducir; {g['cortadas']} tareas cortadas; {g['saltados']} comandos "
          f"de otro microcódigo saltados; {g['errores_dl']} errores de display list")
        if g["desconocidos"]:
            p("   comandos desconocidos (último de la tarea, tareas): " +
              ", ".join(f"0x{op:02X} ×{n}" for op, n in sorted(g["desconocidos"].items())))
        p(f"   combiners aproximados: {g['con_aproximados']} tareas, {g['dibujos_aproximados']} dibujos, "
          f"{len(g['aproximados_nuevos'])} distintos" +
          (": " + " ".join(g["aproximados_nuevos"][:12]) + (" …" if len(g["aproximados_nuevos"]) > 12 else "")
           if g["aproximados_nuevos"] else ""))
    if ses.dumps:
        p(f"-- volcados automáticos ({len(ses.dumps)}; contienen datos del juego: no se comparten):")
        for d in ses.dumps:
            p(f"   {d['archivo']}: {d['motivo']} (campo {d['campo']}, tarea {d['tarea']}, 0x{d['que']:X})")
    other = [e for e in ses.events if not e["texto"].startswith(("aproximado=", "desconocido="))]
    if other:
        p(f"-- eventos: " + "; ".join(f"{e['ms'] / 1000:.0f} s {e['texto']}" for e in other[:20]))
    p("")


def sessions_in(path):
    if os.path.isdir(path):
        files = []
        for root, _, names in os.walk(path):
            for n in names:
                if n.endswith(".sesion"):
                    files.append(os.path.join(root, n))
        return sorted(files, key=lambda f: (os.path.dirname(f), int(os.path.basename(f).split(".")[0])))
    return [path]


def last_session(path):
    files = sessions_in(path)
    if not files:
        raise SystemExit(f"{path}: no hay sesiones")
    return files[-1]


def compare(a, b, out):
    p = lambda *x: print(*x, file=out)
    ma, mb = metrics(a), metrics(b)
    p(f"== antes:   {a.name()}  ({a.context.get('version', '?')})")
    p(f"   después: {b.name()}  ({b.context.get('version', '?')})")
    keys = list(ma) + [k for k in mb if k not in ma]
    worse_up = ("tirones", "espera", "esperas", "copias esperadas", "tareas cortadas", "desconocidos",
                "aproximados", "audio", "emulación", "palabras", "retraducciones", "bytes perdidos")
    p(f"   {'':42s} {'antes':>12s} {'después':>12s} {'cambio':>9s}")
    for k in keys:
        va, vb = ma.get(k, 0), mb.get(k, 0)
        ch = (vb - va) / va * 100 if va else (0 if not vb else float("inf"))
        bad = (ch > 10 and any(w in k for w in worse_up)) or (ch < -10 and k.startswith(("campos/s", "MIPS", "presentados")))
        p(f"   {k:42s} {fmt(va):>12s} {fmt(vb):>12s} {ch:+8.0f}%{'  ←' if bad else ''}")
    ga, gb = gfx_summary(a), gfx_summary(b)
    new_ops = sorted(set(gb.get("desconocidos", {})) - set(ga.get("desconocidos", {})))
    gone_ops = sorted(set(ga.get("desconocidos", {})) - set(gb.get("desconocidos", {})))
    if new_ops:
        p("   NUEVOS comandos desconocidos: " + ", ".join(f"0x{o:02X}" for o in new_ops))
    if gone_ops:
        p("   ya no aparecen: " + ", ".join(f"0x{o:02X}" for o in gone_ops))
    new_cc = sorted(set(gb.get("aproximados_nuevos", [])) - set(ga.get("aproximados_nuevos", [])))
    if new_cc:
        p(f"   combiners aproximados que antes no aparecían: {len(new_cc)}: " + " ".join(new_cc[:16]))
    p("")


def export_trace(ses, out):
    p = lambda *x: print(*x, file=out)
    p("# ventana campo ciclos | entrada: B id vpc code n end hot cold, o M tipo valor")
    for k, t in enumerate(ses.traces):
        p(f"V {k} {t['campo']} {t['ciclos']}")
        for e in t["entradas"]:
            top = e >> 24
            if top >= 0x80:
                p(f"M {top:02x} {e & 0xFFFFFF}")
            elif e:
                b = t["tabla"].get(e)
                if b:
                    p(f"B {e} {b['vpc']:08x} {b['code']} {b['n']} {b['end']} {b['hot']} {b['cold']}")
                else:
                    p(f"B {e} ? ? ? ? ? ?")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s1 = sub.add_parser("resumen")
    s1.add_argument("rutas", nargs="+")
    s1.add_argument("--top", type=int, default=15)
    s2 = sub.add_parser("comparar")
    s2.add_argument("antes")
    s2.add_argument("despues")
    s3 = sub.add_parser("juegos")
    s3.add_argument("raiz")
    s4 = sub.add_parser("traza")
    s4.add_argument("ruta")
    s4.add_argument("-o", "--salida")
    s5 = sub.add_parser("json")
    s5.add_argument("ruta")
    a = ap.parse_args(argv)
    out = sys.stdout
    if a.cmd == "resumen":
        for r in a.rutas:
            for f in sessions_in(r):
                summary(Session(f), out, a.top)
    elif a.cmd == "comparar":
        compare(Session(last_session(a.antes)), Session(last_session(a.despues)), out)
    elif a.cmd == "juegos":
        for g in sorted(os.listdir(a.raiz)):
            d = os.path.join(a.raiz, g)
            files = sessions_in(d) if os.path.isdir(d) else []
            if len(files) >= 2:
                compare(Session(files[-2]), Session(files[-1]), out)
            elif files:
                summary(Session(files[-1]), out)
    elif a.cmd == "traza":
        ses = Session(last_session(a.ruta))
        if a.salida:
            with open(a.salida, "w") as f:
                export_trace(ses, f)
        else:
            export_trace(ses, out)
    elif a.cmd == "json":
        ses = Session(last_session(a.ruta))
        d = {k: v for k, v in vars(ses).items()}
        json.dump(d, out, default=str)
        out.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
