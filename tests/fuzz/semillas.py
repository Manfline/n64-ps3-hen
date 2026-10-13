# SPDX-License-Identifier: GPL-2.0-or-later
"""Salsa64 — semillas del fuzzer del frontend (tests/fuzz/fuzz.c).

Escribe semillas.h con PNG sintéticos (degradados generados aquí, de todos
los tipos de color y profundidades, entrelazados o no, con los cinco
filtros y zlib en bloques guardados, Huffman fijo, dinámico, RLE y solo
Huffman, a veces en varios IDAT), .cht inventados (RetroArch y Project64,
con opciones «??») e índices de descarga (lista y JSON). Nada sale de un
juego: los CRC, direcciones y nombres son de mentira.

    python3 -I tests/fuzz/semillas.py > tests/fuzz/semillas.h
"""
import struct
import sys
import zlib


def chunk(kind, body):
    crc = zlib.crc32(kind + body) & 0xFFFFFFFF
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)


def bits_row(samples, depth):
    """Muestras de 1, 2, 4, 8 o 16 bits a bytes de una línea."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    out, acc, n = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s
        n += depth
        if n == 8:
            out.append(acc)
            acc, n = 0, 0
    if n:
        out.append(acc << (8 - n))
    return bytes(out)


CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def sample(x, y, c, depth):
    top = (1 << depth) - 1
    return (x * 37 + y * 11 + c * 53 + (x * y) % 7) % (top + 1)


def filt(row, prev, bpp, kind):
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if prev and i >= bpp else 0
        if kind == 0:
            p = 0
        elif kind == 1:
            p = a
        elif kind == 2:
            p = b
        elif kind == 3:
            p = (a + b) >> 1
        else:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            p = a if pa <= pb and pa <= pc else b if pb <= pc else c
        out.append((v - p) & 0xFF)
    return bytes([kind]) + bytes(out)


def raw_image(w, h, ctype, depth, interlace):
    ch = CHANNELS[ctype]
    bpp = max(1, depth * ch // 8)
    passes = ADAM7 if interlace else [(0, 0, 1, 1)]
    raw = bytearray()
    k = 0
    for x0, y0, dx, dy in passes:
        xs = list(range(x0, w, dx))
        ys = list(range(y0, h, dy))
        if not xs or not ys:
            continue
        prev = None
        for y in ys:
            row = bits_row([sample(x, y, c, depth) for x in xs for c in range(ch)], depth)
            raw += filt(row, prev, bpp, k % 5)
            prev = row
            k += 1
    return bytes(raw)


def compress(raw, mode):
    if mode == "guardado":
        return zlib.compress(raw, 0)
    strategy = {"fijo": zlib.Z_FIXED, "dinamico": zlib.Z_DEFAULT_STRATEGY, "rle": zlib.Z_RLE,
                "huffman": zlib.Z_HUFFMAN_ONLY}[mode]
    co = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy)
    return co.compress(raw) + co.flush()


def png(w, h, ctype, depth, interlace, mode, split):
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    if ctype == 3:
        n = 1 << depth
        out += chunk(b"PLTE", bytes((i * 29 + c * 71) & 0xFF for i in range(n) for c in range(3)))
        out += chunk(b"tRNS", bytes((i * 97) & 0xFF for i in range(min(n, 5))))
    elif ctype == 0:
        out += chunk(b"tRNS", struct.pack(">H", 1))
    elif ctype == 2:
        out += chunk(b"tRNS", struct.pack(">HHH", 1, 2, 3))
    z = compress(raw_image(w, h, ctype, depth, interlace), mode)
    step = max(1, len(z) // split)
    for i in range(0, len(z), step):
        out += chunk(b"IDAT", z[i:i + step])
    return out + chunk(b"IEND", b"")


PNGS = [
    png(9, 7, 0, 1, 0, "guardado", 1), png(13, 5, 0, 2, 1, "fijo", 2), png(17, 9, 0, 4, 0, "dinamico", 1),
    png(16, 16, 0, 8, 1, "rle", 3), png(7, 11, 0, 16, 0, "huffman", 1), png(12, 8, 2, 8, 0, "dinamico", 2),
    png(10, 9, 2, 16, 1, "fijo", 1), png(15, 6, 3, 1, 0, "dinamico", 1), png(11, 11, 3, 2, 1, "guardado", 2),
    png(19, 4, 3, 4, 0, "rle", 1), png(8, 8, 3, 8, 1, "dinamico", 4), png(14, 10, 4, 8, 0, "huffman", 1),
    png(6, 13, 4, 16, 1, "dinamico", 1), png(20, 12, 6, 8, 0, "dinamico", 3), png(9, 9, 6, 16, 1, "fijo", 1),
    png(400, 3, 6, 8, 0, "dinamico", 2), png(3, 500, 2, 8, 1, "rle", 1),
]

CHTS = [
    b'cheats = 3\n\ncheat0_desc = "Vidas infinitas"\ncheat0_code = "8100A6C4 0063+8100A6C6 0063"\n'
    b'cheat0_enable = false\ncheat1_desc = "Prueba \xc3\xb1and\xc3\xba"\ncheat1_code = "D0001000 0001+80001001 00FF"\n'
    b'cheat1_enable = true\ncheat2_desc = "Repetidor"\ncheat2_code = "50000402 0001 80002000 0010"\n',
    b"[11111111-22222222-C:45]\nName=Juego de prueba (U)\n\n$Grupo\\Vidas\n80001000 0009\n81001002 FFFF\n"
    b"$Grupo\\Nivel\n800010?? 00??\n00 Uno\n01 Dos\n1F Tres\n$Otro\nD1001004 0001\n80001006 0001\n"
    b"[33333333-44444444-C:50]\nName=Otra region (E)\n$Vidas\n80002000 0009\n",
    b"cheat0_desc = sin codigo\ncheat7_code = 80000000 0001\ncheat99999999_desc = x\ncheat3_enable = 1\n",
]

SEL = b"Vidas infinitas\nGrupo > Nivel: Dos\nRepetidor\n"

INDICES = [
    b"Juego de prueba (USA).png\nJuego de prueba (Europe).png\nOtro juego (Japan).png\nindex.txt\n",
    b'[{"name": "Juego de prueba (USA).cht", "path": "Config/Cheats/Juego de prueba (USA).cht"},'
    b'{"name": "Caf\\u00e9 \\"64\\" (Europe).cht", "path": "a/b/../x.cht", "size": 12},'
    b'{"name": "\\u0000.png\\n../x.png", "type": "file"}]',
]


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("\t" + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    return "static const unsigned char %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def table(kind, items):
    out = "".join(c_array("%s_%d" % (kind, i), d) for i, d in enumerate(items))
    out += "static const struct seed %s_seeds[] = {\n" % kind
    out += "".join("\t{ %s_%d, sizeof %s_%d },\n" % (kind, i, kind, i) for i in range(len(items)))
    return out + "};\n"


def main():
    w = sys.stdout.write
    w("/* Generado por tests/fuzz/semillas.py: no editar a mano. */\n")
    w("struct seed {\n\tconst unsigned char *d;\n\tunsigned long n;\n};\n")
    w(table("png", PNGS))
    w(table("cht", CHTS))
    w(table("sel", [SEL]))
    w(table("idx", INDICES))


main()
