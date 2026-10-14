"""Lector de los archivos .lab que escribe tools/lab/rec-lab (docs/lab-recompilador.md).

Formato (texto, una línea por registro):
  # …           comentario / resumen
  O k=v …       desplazamientos de struct salsa64_cpu que usa el código generado
  L k=v …       palabra de la caché de cada rutina compartida del preámbulo
  R n / hex…    las n palabras del preámbulo y las rutinas (FPU compacta, ktlb, camino lento)
  B vpc cuenta n= bytes= fr= frdep= hot= total= entry= seg=
  M …           palabras MIPS del bloque (bytes / 4)
  K h/c …       marcas: palabra caliente / fría donde empieza el código de la instrucción k (n + 1)
  P …           palabras PPC: hot calientes y el resto frías
  T k clase destino tomado no_tomado     sitios de salida del perfil de la PS3 (opcional)
  H c0 c1 …     ejecuciones de cada instrucción MIPS del bloque (lab-trace; opcional)
  E desde hacia veces                    aristas entre bloques (lab-trace; índices de bloque)
  S vpc cuenta  entradas del perfil sin traducción;  X vpc cuenta  con otra huella
"""
import re
import sys

import lab_ppc as P

KV = re.compile(r"(\w+)=(-?\d+)")


class Block:
    __slots__ = ("vpc", "count", "n", "bytes", "fr", "frdep", "hot", "total", "entry", "seg", "mips", "marks_h",
                 "marks_c", "how", "words", "sites", "_dec", "idx", "loop", "icount")

    def decoded(self, lab):
        """Lista de Insn por palabra (None en las palabras de datos que siguen a un `bl exit_link`)."""
        if self._dec is None:
            out = []
            skip = False
            for i, w in enumerate(self.words):
                if skip:
                    out.append(None)
                    skip = False
                    continue
                ins = P.decode(w)
                out.append(ins)
                if ins.cls == P.BR and ins.name == "bl" and lab.abs_target(self, i) == lab.rtn.get("exit_link"):
                    skip = True  # la palabra siguiente es el destino (dato)
            self._dec = out
        return self._dec


class Lab:
    def __init__(self):
        self.header = ""
        self.off = {}
        self.rtn = {}
        self.pre = []
        self.blocks = []
        self.skipped = []   # (vpc, cuenta)
        self.mismatch = []
        self.summary = []
        self.edges = []     # (desde, hacia, veces): índices de bloque (lab-trace)
        self.rtn_names = {}  # palabra → nombre

    def abs_target(self, blk, i):
        """Palabra absoluta de la caché a la que salta la b/bl/bc relativa en la palabra i del bloque."""
        ins = P.decode(blk.words[i])
        return blk.entry + i + ins.imm // 4

    def routine_at(self, word):
        return self.rtn_names.get(word)


def parse(path):
    lab = Lab()
    cur = None
    pre_left = 0
    with open(path) as f:
        for line in f:
            if pre_left > 0 and re.match(r"^[0-9a-f]{8}", line):
                lab.pre.extend(int(t, 16) for t in line.split())
                continue
            tag = line[0]
            if tag == "#":
                if line.startswith("# rec-lab"):
                    lab.header = line[2:].strip()
                else:
                    lab.summary.append(line[2:].strip())
            elif tag == "O":
                lab.off = {k: int(v) for k, v in KV.findall(line)}
            elif tag == "L":
                lab.rtn = {k: int(v) for k, v in KV.findall(line)}
                for k, v in lab.rtn.items():
                    lab.rtn_names.setdefault(v, k)
            elif tag == "R":
                pre_left = int(line.split()[1])
            elif tag == "B":
                t = line.split()
                cur = Block()
                cur.vpc = int(t[1], 16)
                cur.count = int(t[2])
                kv = {k: int(v) for k, v in KV.findall(line)}
                cur.n, cur.bytes, cur.fr, cur.frdep = kv["n"], kv["bytes"], kv["fr"], kv["frdep"]
                cur.hot, cur.total, cur.entry, cur.seg = kv["hot"], kv["total"], kv["entry"], kv["seg"]
                cur.sites = []
                cur.loop = kv.get("loop", 0)
                cur.icount = None
                cur._dec = None
                cur.idx = len(lab.blocks)
                lab.blocks.append(cur)
            elif tag == "M":
                cur.mips = [int(t, 16) for t in line.split()[1:]]
            elif tag == "K":
                hs, cs, hw = [], [], []
                for t in line.split()[1:]:
                    if t == "-":
                        hs.append(None)
                        cs.append(None)
                        hw.append(None)
                    else:
                        hc, how = t.split(":")
                        h, c = hc.split("/")
                        hs.append(int(h))
                        cs.append(int(c))
                        hw.append(int(how))
                cur.marks_h, cur.marks_c, cur.how = hs, cs, hw
            elif tag == "P":
                cur.words = [int(t, 16) for t in line.split()[1:]]
            elif tag == "H":
                cur.icount = [int(x) for x in line.split()[1:]]
            elif tag == "T":
                t = line.split()
                cur.sites.append((int(t[1]), int(t[2]), int(t[3], 16), int(t[4]), int(t[5])))
            elif tag == "E":
                t = line.split()
                lab.edges.append((int(t[1]), int(t[2]), int(t[3])))
            elif tag == "S":
                t = line.split()
                lab.skipped.append((int(t[1], 16), int(t[2])))
            elif tag == "X":
                t = line.split()
                lab.mismatch.append((int(t[1], 16), int(t[2])))
    return lab


if __name__ == "__main__":
    lab = parse(sys.argv[1])
    print(lab.header, len(lab.blocks), "bloques")
