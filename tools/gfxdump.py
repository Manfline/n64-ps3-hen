#!/usr/bin/env python3
"""Lista y edita volcados de la HLE gráfica (gfxdump.bin, gfx.h: gfx_dump_header).

Sirve para aislar un dibujo que sale mal en el RSX: se edita el volcado en la
Mac y se repite con `tools/rpcs3-run.py --replay` o `salsa64-run --raster-dump`.

  tools/gfxdump.py list gfxdump.bin
  tools/gfxdump.py edit gfxdump.bin out.bin --keep 3,7 --set 7:blend=0 --white 7

--keep deja solo esos dibujos (los demás quedan con 0 vértices; destinos y
borrados se conservan). --set N:campo=valor cambia un campo de un dibujo
(cycle, cull, depth, blend, atest, aref, fog, ntex, tex0.flags...). --white N
pinta de blanco opaco la textura 0 del dibujo N. --k N:t,j=r,g,b,a cambia un
coeficiente de la combinación lineal; --normw N divide los vértices entre w.
"""
import argparse
import struct
import sys

MAGIC = 0x53344744
OPS = {0: "END", 1: "TARGET", 2: "ZCLEAR", 3: "DRAW"}
U8_FIELDS = ["cycle", "cull", "depth", "blend", "atest", "aref", "fog", "ntex"]
BLEND = ["OFF", "ALPHA", "ADD", "KEEP"]


def detect_endian(data):
    for e in (">", "<"):
        if struct.unpack_from(e + "I", data, 0)[0] == MAGIC:
            return e
    sys.exit("no es un volcado de Salsa64")


class Dump:
    def __init__(self, data):
        self.e = detect_endian(data)
        _, cb, vb, tb = struct.unpack_from(self.e + "4I", data, 0)
        self.cmd = bytearray(data[16:16 + cb])
        self.vtx = bytearray(data[16 + cb:16 + cb + vb])
        self.tex = bytearray(data[16 + cb + vb:16 + cb + vb + tb])

    def save(self, path):
        hdr = struct.pack(self.e + "4I", MAGIC, len(self.cmd), len(self.vtx), len(self.tex))
        with open(path, "wb") as f:
            f.write(hdr + self.cmd + self.vtx + self.tex)

    def records(self):
        off = 0
        while off + 16 <= len(self.cmd):
            op, size = struct.unpack_from(self.e + "HH", self.cmd, off)
            yield off, op, size
            if op == 0 or size < 16:
                return
            off += size

    def draws(self):
        n = 0
        for off, op, _ in self.records():
            if op == 3:
                yield n, off
                n += 1

    def tex_at(self, off, i):
        base = off + 48 + 32 * i
        o, w, h, pitch, flags, _, key = struct.unpack_from(self.e + "IHHHBBI", self.cmd, base)
        xf = struct.unpack_from(self.e + "4f", self.cmd, base + 16)
        return base, dict(offset=o, width=w, height=h, pitch=pitch, flags=flags, key=key, xf=xf)

    def describe(self, n, off):
        e = self.e
        first, count = struct.unpack_from(e + "II", self.cmd, off + 4)
        zimg = struct.unpack_from(e + "I", self.cmd, off + 16)[0]
        sc = struct.unpack_from(e + "4H", self.cmd, off + 20)
        f = dict(zip(U8_FIELDS, self.cmd[off + 28:off + 36]))
        fogc, kind, tile = struct.unpack_from(e + "3I", self.cmd, off + 36)
        s = (f"#{n:3d} off {off:6d} v {first}+{count} kind {kind} cyc {f['cycle']} "
             f"cull {f['cull']} depth {f['depth']} blend {BLEND[f['blend']] if f['blend'] < 4 else f['blend']} "
             f"atest {f['atest']}/{f['aref']} fog {f['fog']} ntex {f['ntex']} z {zimg:x} "
             f"sc {[x / 4 for x in sc]}")
        for i in range(f["ntex"]):
            _, t = self.tex_at(off, i)
            s += (f"\n      tex{i} off {t['offset']} {t['width']}x{t['height']} pitch {t['pitch']} "
                  f"flags {t['flags']:#x} xf {[round(x, 5) for x in t['xf']]}")
        k = struct.unpack_from(e + "60f", self.cmd, off + 112)
        names = ["T0", "T0a", "T1", "T1a", "1"]
        for t in range(5):
            row = k[t * 12:(t + 1) * 12]
            if any(abs(x) > 1e-6 for x in row):
                s += f"\n      {names[t]:3s} k0 {fmt4(row[0:4])} kC {fmt4(row[4:8])} kCa {fmt4(row[8:12])}"
        return s


def fmt4(v):
    return "(" + " ".join(f"{x:.3f}" for x in v) + ")"


def cmd_list(args):
    d = Dump(open(args.dump, "rb").read())
    print(f"comandos {len(d.cmd)} B, vértices {len(d.vtx)} B ({len(d.vtx) // 32}), texturas {len(d.tex)} B")
    for off, op, size in d.records():
        if op == 3:
            continue
        a, b, c = struct.unpack_from(d.e + "3I", d.cmd, off + 4)
        print(f"     off {off:6d} {OPS.get(op, op)} a {a:#x} b {b:#x} c {c:#x}")
    for n, off in d.draws():
        if args.draw is None or n in args.draw:
            print(d.describe(n, off))
    if args.vtx is not None:
        _, off = list(d.draws())[args.vtx]
        first, count = struct.unpack_from(d.e + "II", d.cmd, off + 4)
        for i in range(first, first + count):
            x, y, z, w, s, t = struct.unpack_from(d.e + "6f", d.vtx, i * 32)
            r, g, b, a = d.vtx[i * 32 + 24:i * 32 + 28]
            print(f"      v{i}: x {x:9.2f} y {y:9.2f} z {z:9.2f} w {w:9.2f}  s {s:7.2f} t {t:7.2f}  "
                  f"rgba {r} {g} {b} {a}  ndc {x / w:6.3f} {y / w:6.3f} {z / w:6.4f}")


def ints(s):
    return {int(x) for x in s.split(",") if x}


def cmd_edit(args):
    d = Dump(open(args.dump, "rb").read())
    draws = dict(d.draws())
    if args.keep is not None:
        for n, off in draws.items():
            if n not in args.keep:
                struct.pack_into(d.e + "I", d.cmd, off + 8, 0)
    for spec in args.set:
        n, kv = spec.split(":", 1)
        key, val = kv.split("=", 1)
        off = draws[int(n)]
        if key in U8_FIELDS:
            d.cmd[off + 28 + U8_FIELDS.index(key)] = int(val, 0)
        elif key.startswith("tex") and "." in key:
            i, field = key[3:].split(".", 1)
            base, _ = d.tex_at(off, int(i))
            if field != "flags":
                sys.exit(f"campo desconocido: {key}")
            d.cmd[base + 10] = int(val, 0)
        else:
            sys.exit(f"campo desconocido: {key}")
    for spec in args.k:
        n, rest = spec.split(":", 1)
        tj, vals = rest.split("=", 1)
        t, j = (int(x) for x in tj.split(","))
        v = [float(x) for x in vals.split(",")]
        struct.pack_into(d.e + "4f", d.cmd, draws[int(n)] + 112 + 48 * t + 16 * j, *v)
    for n in args.normw:
        off = draws[n]
        first, count = struct.unpack_from(d.e + "II", d.cmd, off + 4)
        for i in range(first, first + count):
            x, y, z, w = struct.unpack_from(d.e + "4f", d.vtx, i * 32)
            struct.pack_into(d.e + "4f", d.vtx, i * 32, x / w, y / w, z / w, 1.0)
    for n in args.white:
        _, t = d.tex_at(draws[n], 0)
        for y in range(t["height"]):
            row = t["offset"] + y * t["pitch"]
            d.tex[row:row + 4 * t["width"]] = b"\xff" * (4 * t["width"])
    d.save(args.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("dump")
    p.add_argument("--draw", type=ints, help="solo estos dibujos")
    p.add_argument("--vtx", type=int, help="vértices de este dibujo")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("edit")
    p.add_argument("dump")
    p.add_argument("out")
    p.add_argument("--keep", type=ints)
    p.add_argument("--set", action="append", default=[])
    p.add_argument("--white", type=int, action="append", default=[])
    p.add_argument("--k", action="append", default=[], help="N:t,j=r,g,b,a: término t, coeficiente j")
    p.add_argument("--normw", type=int, action="append", default=[], help="vértices de N con w = 1")
    p.set_defaults(fn=cmd_edit)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
