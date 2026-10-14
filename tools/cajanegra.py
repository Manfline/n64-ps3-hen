#!/usr/bin/env python3
"""Lee los archivos de la caja negra de la PS3 (src/rdp/blackbox.h, docs/cajanegra.md).

  tools/cajanegra.py info  caja.s4bb
  tools/cajanegra.py list  caja.s4bb
  tools/cajanegra.py campos caja.s4bb            (un renglón por campo presentado)
  tools/cajanegra.py extract caja.s4bb DIR       (tareas, campos y notas en archivos)

`extract` deja en DIR: `tarea-NNNNN.bin` (el volcado de cada tarea, el mismo
formato de gfxdump.bin: tools/gfxdump.py lo lista y lo edita, `salsa64-run
--raster-dump` lo dibuja), `campos.txt`, `tareas.txt` y `nota-ETIQUETA-N.bin`.
`salsa64-run --raster-dump caja.s4bb:NUMERO` dibuja una tarea sin extraerla.
"""
import argparse
import os
import struct
import sys

MAGIC = 0x53344242
END = b"END "
SPUKIT_LZ_BLOCK = 65536
STORED = 0x80000000

FLD_NAMES = ["campo", "banderas", "tareas", "traducidas", "vi_tipo", "vi_origen", "vi_ancho", "vi_alto",
             "vi_paso", "vi_bpp"]
VI_REGS = ["CONTROL", "ORIGIN", "WIDTH", "V_INTR", "V_CURRENT", "BURST", "V_SYNC", "H_SYNC", "H_SYNC_LEAP",
           "H_VIDEO", "V_VIDEO", "V_BURST", "X_SCALE", "Y_SCALE"]
FLD_FLAGS = [(1, "mostrado"), (2, "saltado"), (4, "RSX"), (8, "RDRAM"), (16, "sin imagen")]
KINDS = {0: "verdadero", 1: "intermedio", 2: "repetido"}
NONE = 0xFFFFFFFF


def lz4_block(src, n):
    """Descomprime un bloque de LZ4 a exactamente n bytes."""
    out = bytearray()
    i = 0
    while True:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0 or off > len(out):
            raise ValueError("desplazamiento inválido")
        for _ in range(ml):
            out.append(out[-off])
    if len(out) != n:
        raise ValueError(f"bloque de {len(out)} bytes, se esperaban {n}")
    return bytes(out)


class Rec:
    def __init__(self, kind, length, flags, rid, payload, nbytes):
        self.kind, self.len, self.flags, self.id = kind, length, flags, rid
        self.payload, self.nbytes = payload, nbytes

    def content(self):
        if not self.flags & 1:
            return self.payload[:self.len]
        out = bytearray()
        pos = 0
        for off in range(0, self.len, SPUKIT_LZ_BLOCK):
            n = min(SPUKIT_LZ_BLOCK, self.len - off)
            (hdr,) = struct.unpack_from(">I", self.payload, pos)
            clen = hdr & ~STORED
            body = self.payload[pos + 4:pos + 4 + clen]
            out += body if hdr & STORED else lz4_block(body, n)
            pos += 4 + clen
        return bytes(out)


def parse(data):
    if len(data) < 64 or struct.unpack_from(">I", data, 0)[0] != MAGIC:
        sys.exit("no es un archivo de la caja negra de Salsa64")
    _, ver, hsize, flags, field, tasks, fields, notes, crc1, crc2 = struct.unpack_from(">10I", data, 0)
    title = data[40:60].split(b"\0")[0].decode("latin-1")
    dropped = struct.unpack_from(">I", data, 60)[0]
    info = dict(version=ver, flags=flags, field=field, tasks=tasks, fields=fields, notes=notes,
                crc1=crc1, crc2=crc2, title=title, dropped=dropped)
    recs = []
    pos = hsize
    while pos + 16 <= len(data):
        kind = data[pos:pos + 4]
        if kind == END:
            break
        length, rflags, rid = struct.unpack_from(">III", data, pos + 4)
        pos += 16
        if rflags & 1:
            start = pos
            for off in range(0, length, SPUKIT_LZ_BLOCK):
                (hdr,) = struct.unpack_from(">I", data, pos)
                pos += 4 + (hdr & ~STORED)
            nbytes = pos - start
            payload = data[start:pos]
        else:
            nbytes = length
            payload = data[pos:pos + length]
            pos += length
        recs.append(Rec(kind.decode("latin-1"), length, rflags, rid, payload, nbytes))
    return info, recs


def words(buf, n):
    return struct.unpack_from(">" + "I" * n, buf, 0)


def field_row(w):
    names = [name for bit, name in FLD_FLAGS if w[1] & bit]
    return dict(campo=w[0], banderas="+".join(names) or "-", tareas=w[2], traducidas=w[3],
                vi=f"{w[6]}x{w[7]} origen {w[5]:06x} paso {w[8]} bpp {w[9]} tipo {w[4]}",
                regs=dict(zip(VI_REGS, w[10:24])), tipo=KINDS.get(w[24], str(w[24])),
                t=struct.unpack(">f", struct.pack(">I", w[25]))[0], k=w[26], adelante=w[27],
                hora_us=w[28] << 32 | w[29], tarea=w[30], anterior=w[31])


def cmd_info(data):
    info, recs = parse(data)
    print(f"juego: {info['title']!r} CRC {info['crc1']:08X}-{info['crc2']:08X}")
    print(f"pedida en el campo {info['field']}; versión {info['version']}; "
          f"{'comprimida' if info['flags'] & 1 else 'sin comprimir'}")
    print(f"{info['tasks']} tareas, {info['fields']} campos, {info['notes']} notas; "
          f"{info['dropped']} tareas descartadas por falta de lugar en el anillo")
    raw = sum(r.len for r in recs)
    print(f"{len(data)} bytes en el archivo ({raw} sin comprimir)")


def cmd_list(data):
    info, recs = parse(data)
    for r in recs:
        extra = ""
        if r.kind == "TASK":
            c = r.content()
            w = words(c, 8)
            extra = (f" campo {w[1]} anterior {'-' if w[2] == NONE else w[2]}"
                     f"{' (inmediata)' if w[3] & 1 else ''} dibujos {w[4]} triángulos {w[5]}")
        print(f"{r.kind} id {r.id if r.kind != 'NOTE' else bytes.fromhex('%08x' % r.id).decode('latin-1')} "
              f"{r.len} bytes ({r.nbytes} en el archivo){extra}")


def cmd_campos(data):
    info, recs = parse(data)
    for r in recs:
        if r.kind != "FILD":
            continue
        f = field_row(words(r.content(), 32))
        print(f"campo {f['campo']:>7} {f['banderas']:<14} {f['tipo']:<10} t={f['t']:.3f} k={f['k']} "
              f"tareas {f['tareas']} traducidas {f['traducidas']} dibujó {'-' if f['tarea'] == NONE else f['tarea']} "
              f"anterior {'-' if f['anterior'] == NONE else f['anterior']} {f['vi']} "
              f"adelante {f['adelante']} t={f['hora_us'] / 1e6:.4f} s")


def cmd_extract(data, outdir):
    info, recs = parse(data)
    os.makedirs(outdir, exist_ok=True)
    tareas, campos = [], []
    nn = 0
    for r in recs:
        c = r.content()
        if r.kind == "TASK":
            w = words(c, 8)
            name = f"tarea-{r.id:05d}.bin"
            open(os.path.join(outdir, name), "wb").write(c[32:])
            tareas.append(f"{name} campo {w[1]} anterior {'-' if w[2] == NONE else w[2]}"
                          f"{' inmediata' if w[3] & 1 else ''} dibujos {w[4]} triángulos {w[5]} rectángulos {w[6]}")
        elif r.kind == "FILD":
            f = field_row(words(c, 32))
            regs = " ".join(f"{k}={v:08x}" for k, v in f["regs"].items())
            campos.append(f"campo {f['campo']} {f['banderas']} {f['tipo']} t={f['t']:.4f} k={f['k']} "
                          f"traducidas {f['traducidas']} tarea {'-' if f['tarea'] == NONE else f['tarea']} "
                          f"anterior {'-' if f['anterior'] == NONE else f['anterior']} {f['vi']} "
                          f"adelante {f['adelante']} hora_us {f['hora_us']} | {regs}")
        elif r.kind == "NOTE":
            tag = bytes.fromhex("%08x" % r.id).decode("latin-1")
            nn += 1
            open(os.path.join(outdir, f"nota-{tag}-{nn}.bin"), "wb").write(c)
    open(os.path.join(outdir, "tareas.txt"), "w").write("\n".join(tareas) + "\n")
    open(os.path.join(outdir, "campos.txt"), "w").write("\n".join(campos) + "\n")
    print(f"{len(tareas)} tareas, {len(campos)} campos y {nn} notas en {outdir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["info", "list", "campos", "extract"])
    ap.add_argument("file")
    ap.add_argument("outdir", nargs="?")
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    if a.cmd == "info":
        cmd_info(data)
    elif a.cmd == "list":
        cmd_list(data)
    elif a.cmd == "campos":
        cmd_campos(data)
    else:
        if not a.outdir:
            sys.exit("extract necesita la carpeta de salida")
        cmd_extract(data, a.outdir)


if __name__ == "__main__":
    main()
