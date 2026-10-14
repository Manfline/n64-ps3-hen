#!/usr/bin/env python3
"""Pruebas del laboratorio del recompilador (tools/lab, docs/lab-recompilador.md): el decodificador
de PowerPC contra codificaciones conocidas, y de punta a punta con un programa MIPS sintético (sin
datos de ningún juego): rec-lab lo traduce y lab-analiza lo lee. Se corre con tools/lab/test-lab.sh."""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import lab_data as D     # noqa: E402
import lab_metrics as M  # noqa: E402
import lab_ppc as P      # noqa: E402

REC_LAB = os.environ.get("REC_LAB", os.path.join(ROOT, "build", "lab", "bin", "rec-lab"))


class Decoder(unittest.TestCase):
    CASES = [
        (0x38000083, "li", P.ALU, False), (0x3c008006, "lis", P.ALU, False), (0x4e800020, "blr", P.BR, False),
        (0x60000000, "nop", P.NOP, False), (0x7c0802a6, "mflr", P.SPR, False), (0x7c0903a6, "mtctr", P.SPR, False),
        (0x80df02d0, "lwz", P.LOAD, False), (0xf81f0110, "std", P.STORE, False), (0xe9ff00c0, "ld", P.LOAD, False),
        (0x7d3f182e, "lwzx", P.LOAD, False), (0x7c3f1c2e, "lfsx", P.FLOAD, False), (0x7c7f652e, "stfsx", P.FSTORE, False),
        (0x552c4e3e, "rlwinm", P.ROT, False), (0x5520007e, "rlwinm", P.ROT, False), (0x28800078, "cmplwi", P.CMP, False),
        (0x2c8b0000, "cmpwi", P.CMP, False), (0x7e357000, "cmpd", P.CMP, False), (0xfc01102a, "fadd", P.FPU, False),
        (0x7c20d840, "cmpld", P.CMP, False), (0x4bffc0b9, "bl", P.BR, False), (0x28000000 | 5, "cmplwi", P.CMP, False),
        (0x70000001, "andi.", P.ALU, True), (0x7c000030 | (3 << 21) | (4 << 16) | (5 << 11), "slw", P.SHIFT, True),
        (0x7c0001d2 | (3 << 21) | (4 << 16) | (5 << 11), "mulld", P.MUL, False),
    ]

    def test_names_classes_microcode(self):
        for w, name, cls, micro in self.CASES:
            ins = P.decode(w)
            self.assertEqual((ins.name, ins.cls, ins.micro), (name, cls, micro), "%08x" % w)

    def test_memory_fields(self):
        i = P.decode(0xf81f0110)  # std r0,272(r31)
        self.assertEqual((i.ra, i.imm, i.size, i.mem), (31, 272, 8, "d"))
        self.assertTrue(i.is_store() and not i.is_load())


@unittest.skipUnless(os.path.exists(REC_LAB), "falta build/lab/bin/rec-lab (tools/lab/build.sh)")
class EndToEnd(unittest.TestCase):
    PROG = [0x24080005,  # addiu t0, zero, 5
            0x01084821,  # addu  t1, t0, t0
            0x8faa0000,  # lw    t2, 0(sp)
            0xafa90004,  # sw    t1, 4(sp)
            0x15000002,  # bne   t0, zero, +2
            0x00000000,  # nop (delay slot)
            0x00000000,
            0x03e00008,  # jr ra
            0x00000000]

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        ram = bytearray(8 << 20)
        for i, w in enumerate(self.PROG):
            struct.pack_into(">I", ram, 0x1000 + 4 * i, w)
        self.ram = os.path.join(self.tmp, "ram.bin")
        open(self.ram, "wb").write(ram)
        self.pcs = os.path.join(self.tmp, "pcs.tsv")
        open(self.pcs, "w").write("80001000 7\n80001010 3\n")
        self.out = os.path.join(self.tmp, "t.lab")
        r = subprocess.run([REC_LAB, "--rdram", self.ram, "--pcs", self.pcs, "-o", self.out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_blocks(self):
        lab = D.parse(self.out)
        self.assertEqual([b.vpc for b in lab.blocks], [0x80001000, 0x80001010])
        b = lab.blocks[0]
        self.assertEqual(b.count, 7)
        self.assertEqual(b.mips[:6], self.PROG[:6])
        self.assertGreaterEqual(b.n, 6)
        self.assertTrue(0 < b.hot < b.total)
        self.assertEqual(len(b.words), b.total)
        self.assertEqual(b.marks_h[0], b.marks_h[0])   # la primera marca existe
        self.assertEqual(b.marks_h[-1], b.hot)         # la última es el final del cuerpo caliente
        # las marcas calientes no bajan
        ms = [m for m in b.marks_h if m is not None]
        self.assertEqual(ms, sorted(ms))
        # hay una carga y un almacenamiento del MIPS en el cuerpo (a la RDRAM, no a cpu->…)
        dec = b.decoded(lab)
        mem = [d for d in dec[:b.hot] if d is not None and d.is_mem()]
        self.assertTrue(any(d.is_load() for d in mem) and any(d.is_store() for d in mem))

    def test_metrics(self):
        lab = D.parse(self.out)
        m = M.compute(lab)
        self.assertEqual(m["bloques"], 2)
        self.assertEqual(m["entradas"], 10)
        self.assertGreater(m["ejec_por_instr"], 1.0)
        self.assertGreater(m["total_por_instr"], m["hot_por_instr"])

    def test_report_runs(self):
        r = subprocess.run([sys.executable, os.path.join(HERE, "lab-analiza.py"), "reporte", self.out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Palabras de PowerPC por instrucción MIPS", r.stdout)
        r = subprocess.run([sys.executable, os.path.join(HERE, "lab-analiza.py"), "bloque", self.out, "80001000"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("MIPS 0: addiu", r.stdout)


if __name__ == "__main__":
    unittest.main()
