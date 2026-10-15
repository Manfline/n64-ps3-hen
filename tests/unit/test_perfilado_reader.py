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


if __name__ == "__main__":
    unittest.main()
