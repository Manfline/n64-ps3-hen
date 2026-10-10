# Registro de rendimiento

Cada optimización lleva medición antes/después **en PS3 real** (RPCS3 no
reproduce los tiempos del Cell). Formato: fecha, commit, prueba, entorno,
antes, después, notas.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| | | | | | | |

## Líneas base de la Fase 0 (pendientes de PS3 real)

- `spu-compute`: 6 MB, PPE escalar contra 6 SPUs (tiempos en el log).
- `rsx-triangle`: 120 cuadros con vsync.
- `jit-ppe`: costo de limpiar 64 KB de caché (dcbst + icbi).

## Líneas base de la Fase 1

Micro-benchmark del intérprete (`tests/unit/bench.c`): bucle de 10
instrucciones (LW, ADDIU, SW, ADDU, SLL, XOR, ADD.S, ADDIU, BNE, NOP) desde
KSEG0, 20 M instrucciones.

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | (este) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 237 MIPS | Solo referencia del host |
| 2026-10-10 | (este) | RPCS3 0.0.43, PPU LLVM | 35,6 MIPS | **No concluyente** |
| | | PS3 real | pendiente | `retro64-cpu-tests` |
| 2026-10-10 | (arranque HLE) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 217 MIPS | −8 %: la FPU calcula ahora Inexact y los modos de redondeo con exactitud (TwoSum/fma); el bucle tiene un ADD.S cada 10 instrucciones. Costo de corrección, no regresión |
| 2026-10-10 | (arranque HLE) | RPCS3 0.0.43, PPU LLVM | 29,0 MIPS | **No concluyente** |

Corrida completa de nemu64-test (`retro64-run`, ≈189 M ciclos del VR4300):
0,75 s en el host sin sanitizers (253 MIPS); 2,1 s con ASan + UBSan.
