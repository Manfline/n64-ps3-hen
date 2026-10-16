#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prueba de lectura de los registros nuevos, sin archivos de juego."""
import importlib.util
import io
import os
import struct
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../.."))
SPEC = importlib.util.spec_from_file_location("perfilado", os.path.join(ROOT, "tools/perfilado/perfilado.py"))
PERFILADO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PERFILADO)


def record(kind, body):
    return struct.pack(">I", kind << 24 | len(body)) + body


class SessionReaderTest(unittest.TestCase):
    def test_slow_and_compile_records(self):
        slow = struct.pack(">IHH", 5000, 24, 16)
        for k in range(24):
            slow += struct.pack(">IQ", 3 if k == 0 else 0, 798000 if k == 0 else 0)
        for k in range(16):
            slow += struct.pack(">IQI", 0x80001000 if k == 0 else 0, 399000 if k == 0 else 0,
                                2 if k == 0 else 0)
        comp = struct.pack(">IHHII", 5000, 5, 0, 4, 167)
        for k in range(5):
            comp += struct.pack(">IQI", 4 if k == 2 else 0, 1596000 if k == 2 else 0,
                                120 if k == 2 else 0)
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"S64P" + struct.pack(">HHQ", 1, 16, 0) + record(15, slow) + record(16, comp))
            f.flush()
            ses = PERFILADO.Session(f.name)
        self.assertEqual(ses.slow[0]["causas"][0]["llamadas"], 3)
        self.assertEqual(ses.slow[0]["pcs"][0]["pc"], 0x80001000)
        self.assertEqual(ses.compiles[0]["motivos"][2]["palabras"], 120)
        self.assertEqual((ses.compiles[0]["max_campo"], ses.compiles[0]["campo_max"]), (4, 167))
        out = io.StringIO()
        PERFILADO.summary(ses, out)
        self.assertIn("Camino lento por causa", out.getvalue())
        self.assertIn("Compilaciones por motivo", out.getvalue())

    def test_fpu_record_and_old_session(self):
        # ADD.S (índice 0): 4 llamadas, 2 por RM y 2 por resultado; MUL.D (64+2): 1 por inexacto.
        body = struct.pack(">IHHI", 9000, 2, 4, 0)
        body += struct.pack(">IQ", 5, 798000) + struct.pack(">IQ", 0, 0)
        body += struct.pack(">I", 16)
        for k in range(16):
            body += struct.pack(">IQI", 0x80004000 if k == 0 else 0, 79800 if k == 0 else 0, 7 if k == 0 else 0)
        body += struct.pack(">HHQ4I", 0, 0, 79800 * 4, 2, 0, 0, 2)
        body += struct.pack(">HHQ4I", 66, 0, 79800, 0, 1, 0, 0)
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"S64P" + struct.pack(">HHQ", 1, 16, 0) + record(17, body))
            f.flush()
            ses = PERFILADO.Session(f.name)
        self.assertEqual(ses.fpu["entradas"][66]["razones"], [0, 1, 0, 0])
        self.assertEqual(ses.fpu["pcs"][0]["pc"], 0x80004000)
        out = io.StringIO()
        PERFILADO.summary(ses, out)
        text = out.getvalue()
        self.assertIn("FPU al intérprete por instrucción", text)
        self.assertIn("ADD.S", text)
        self.assertIn("MUL.D", text)
        self.assertIn("FPU al intérprete por razón", text)
        self.assertEqual(PERFILADO.fpu_name(2 * 64 + 0x20), "CVT.S.W")
        self.assertEqual(PERFILADO.fpu_name(13), "TRUNC.W.S")
        # Una sesión sin el registro (formato viejo) no imprime la tabla.
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"S64P" + struct.pack(">HHQ", 1, 16, 0))
            f.flush()
            old = PERFILADO.Session(f.name)
        out = io.StringIO()
        PERFILADO.summary(old, out)
        self.assertNotIn("FPU al intérprete", out.getvalue())

    def test_cop0_record_and_wrapped_window_pcs(self):
        # MTC0 Status (32+12) y MI_INTR_MASK escrito (134+7), en dos ventanas.
        def cop0(ms, entries):
            body = struct.pack(">IHH", ms, len(entries), 0)
            for idx, calls, ticks in entries:
                body += struct.pack(">HHIQ", idx, 0, calls, ticks)
            return record(18, body)
        # Una ventana de antes del arreglo: la resta de dos tablas dio -2 (2³² - 2).
        slow = struct.pack(">IHH", 5000, 0, 2)
        slow += struct.pack(">IQI", 0x80042968, 79800, 10) + struct.pack(">IQI", 0x8004294C, 0, (1 << 32) - 2)
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"S64P" + struct.pack(">HHQ", 1, 16, 0) + record(15, slow) +
                    cop0(5000, [(44, 3, 79800), (141, 2, 798)]) + cop0(10000, [(44, 1, 0)]))
            f.flush()
            ses = PERFILADO.Session(f.name)
        self.assertEqual(ses.cop0[0]["entradas"][44], (3, 79800))
        self.assertEqual(ses.slow[0]["pcs"][1]["llamadas"], -2)
        self.assertEqual(PERFILADO.cop0_name(44), "MTC0 Status")
        self.assertEqual(PERFILADO.cop0_name(9), "MFC0 Count")
        self.assertEqual(PERFILADO.cop0_name(132), "ERET")
        self.assertEqual(PERFILADO.cop0_name(141), "MI_INTR_MASK (escritura)")
        out = io.StringIO()
        PERFILADO.summary(ses, out)
        text = out.getvalue()
        self.assertIn("COP0 y MI en el camino lento", text)
        self.assertRegex(text, r"MTC0 Status\s+4\s")
        self.assertNotIn("4294967294", text)


if __name__ == "__main__":
    unittest.main()
