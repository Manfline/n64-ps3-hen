#!/usr/bin/env python3
"""Genera la ventana de síntesis del MP3 de la HLE de audio
(src/rsp/audio/mp3_lut.c).

No es la tabla del microcódigo: sale de la ventana estándar de MPEG-1
(ISO/IEC 11172-3, tabla B.3: D[i]), escalada para que el pico (D[256] =
1,144989014) valga 32767 y ordenada como la recorre mp3.c (el orden de
InnerLoop de mupen64plus-rsp-hle):

  - 33 bloques de 32 entradas: los 16 coeficientes del bloque y una copia
    detrás, para que mp3.c lea 16 seguidos desde cualquier posición del
    anillo de la ventana (offset 1–16) sin dar la vuelta.
  - bloques 0–16:  T[j][k] = D[32k + j]            (salidas 0–16)
  - bloques 17–32: T[j][k] = T[32 − j][(14 − k) mod 16]  (salidas 17–31,
    simétricas de las primeras)

D[0..256] multiplicada por 65536 y redondeada es `ff_mpa_enwindow` de
FFmpeg (libavcodec/mpegaudiodsp_data.c, LGPL 2.1 o posterior; ver
docs/THIRD_PARTY.md); D[512 − i] = −D[i] salvo en los múltiplos de 64, como
arma FFmpeg la ventana completa.

  tools/audio-mp3-lut.py > src/rsp/audio/mp3_lut.c
"""

# FFmpeg: ff_mpa_enwindow[257], D[i] · 65536 redondeado (ISO 11172-3, B.3).
# libavcodec/mpegaudiodata.c, copyright (c) 2002 Fabrice Bellard; This file is
# part of FFmpeg, GNU Lesser General Public License 2.1 o posterior.
ENWINDOW = [
    0, -1, -1, -1, -1, -1, -1, -2,
    -2, -2, -2, -3, -3, -4, -4, -5,
    -5, -6, -7, -7, -8, -9, -10, -11,
    -13, -14, -16, -17, -19, -21, -24, -26,
    -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154,
    -161, -169, -176, -183, -190, -196, -202, -208,
    213, 218, 222, 225, 227, 228, 228, 227,
    224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36,
    -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919,
    -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
    -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
    2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535,
    1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006,
    -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597,
    -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
    -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750,
    -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082,
    70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
    -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189,
    -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137,
    -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420,
    -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038,
]


def window() -> list:
    d = [0] * 512
    for i, v in enumerate(ENWINDOW):
        d[i] = v
        if i:
            d[512 - i] = -v if i & 63 else v
    return d


def table() -> list:
    d = window()
    t = [[d[32 * k + j] for k in range(16)] for j in range(17)]
    t += [[t[32 - j][(14 - k) % 16] for k in range(16)] for j in range(17, 33)]
    out = []
    for blk in t:
        # Redondeo al más cercano (mitades hacia arriba) del valor escalado.
        q = [max(-32768, min(32767, (2 * v * 32767 + 75038) // (2 * 75038))) for v in blk]
        out += q + q
    return out


def main() -> None:
    lut = table()
    assert len(lut) == 0x420
    print("""/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — ventana de síntesis del MP3 (33 bloques × 2 × 16, Q15). Sale
 * de la ventana estándar de MPEG-1 (ISO/IEC 11172-3, D[i], vía
 * ff_mpa_enwindow de FFmpeg, LGPL 2.1+), escalada a 32767 en el pico y en
 * el orden en que la recorre mp3.c; no es la tabla del microcódigo. La
 * genera tools/audio-mp3-lut.py (no editar a mano).
 *
 * ff_mpa_enwindow: FFmpeg, libavcodec/mpegaudiodata.c («MPEG Audio common
 * tables», copyright (c) 2002 Fabrice Bellard; hoy en
 * libavcodec/mpegaudiodsp_data.c). This file is part of FFmpeg: libre bajo
 * la GNU Lesser General Public License 2.1 o posterior, que permite
 * distribuirla bajo la GPL v2 o posterior como el resto de Salsa64.
 */
#include "rsp/audio/hle.h"

const int16_t aud_mp3_window[0x420] = {""")
    for i in range(0, len(lut), 16):
        print("\t" + ", ".join(f"{v:6d}" for v in lut[i:i + 16]) + ",")
    print("};")


if __name__ == "__main__":
    main()
