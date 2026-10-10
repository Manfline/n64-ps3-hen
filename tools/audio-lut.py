#!/usr/bin/env python3
"""Genera la tabla de interpolación de RESAMPLE de la HLE de audio
(src/rsp/audio/lut.c).

No es la tabla del microcódigo de Nintendo: es un diseño propio con el mismo
formato (64 fases × 4 coeficientes en Q15). Para la fase p = i/64, la
muestra de salida cae en la posición 1 + p de las cuatro de entrada y cada
coeficiente es un sinc de corte 0,8 (pasabajos suave, para no realzar el
aliasing al subir el tono) por una ventana de Hann de ancho 4, normalizados
para que la fila sume 32768 (ganancia 1 en continua).

  tools/audio-lut.py > /tmp/lut.txt
"""
import math

FC = 0.8
PHASES = 64


def sinc(x: float) -> float:
    return 1.0 if x == 0 else math.sin(math.pi * x) / (math.pi * x)


def hann(x: float) -> float:
    return 0.5 + 0.5 * math.cos(math.pi * x / 2) if abs(x) < 2 else 0.0


def row(phase: int) -> list:
    p = phase / PHASES
    w = [FC * sinc(FC * (k - 1 - p)) * hann(k - 1 - p) for k in range(4)]
    s = sum(w)
    q = [round(32768 * v / s) for v in w]
    # El redondeo se corrige en el coeficiente más grande.
    big = max(range(4), key=lambda k: abs(q[k]))
    q[big] += 32768 - sum(q)
    return q


def main() -> None:
    for i in range(PHASES):
        print("\t" + ", ".join(f"{v:6d}" for v in row(i)) + ",")


if __name__ == "__main__":
    main()
