# Registro de rendimiento

Cada optimización lleva medición antes/después **en PS3 real** (RPCS3 no
reproduce los tiempos del Cell). Formato: fecha, commit, prueba, entorno,
antes, después, notas.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| | | | | | | |

## Líneas base de la Fase 0 (pendientes de PS3 real)

- `spu-compute`: 6 MB, PPE escalar contra 6 SPUs (tiempos en el log).
  **PS3 real** (2026-10-10, CECH-3011A, HEN): PPE 13 000 µs; 6 SPUs 889 µs
  incluyendo arranque y join (≈14,6×). Los 6 SPUs están disponibles con HEN.
- `rsx-triangle`: 120 cuadros con vsync.
  **PS3 real** (2026-10-10, CECH-3011A, HEN): verificación de píxeles OK a
  1280×720; 120 cuadros en 2067 ms (≈58 cps; el cuadro de verificación espera
  al RSX). La inicialización del RSX tarda ≈2 s.
- `jit-ppe`: costo de limpiar 64 KB de caché (dcbst + icbi).

## Líneas base de la Fase 1

Micro-benchmark del intérprete (`tests/unit/bench.c`): bucle de 10
instrucciones (LW, ADDIU, SW, ADDU, SLL, XOR, ADD.S, ADDIU, BNE, NOP) desde
KSEG0, 20 M instrucciones.

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | (este) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 237 MIPS | Solo referencia del host |
| 2026-10-10 | (este) | RPCS3 0.0.43, PPU LLVM | 35,6 MIPS | **No concluyente** |
| 2026-10-10 | (arranque HLE) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 217 MIPS | −8 %: la FPU calcula ahora Inexact y los modos de redondeo con exactitud (TwoSum/fma); el bucle tiene un ADD.S cada 10 instrucciones. Costo de corrección, no regresión |
| 2026-10-10 | (arranque HLE) | RPCS3 0.0.43, PPU LLVM | 29,0 MIPS | **No concluyente** |
| 2026-10-10 | 81da001 | **PS3 real**: Slim CECH-3011A, HFW 4.91 + HEN, ppu-gcc -O2 -mcpu=cell | **8,8 MIPS** | Primer número real. 154/154 pruebas pasan en big-endian. Muy lejos de los ~60 MIPS que pide la N64 a velocidad completa: perfilar antes de seguir (sospecha: cambios de modo de redondeo/FPSCR en cada ADD.S, que serializan el PPE) |

Corrida completa de nemu64-test (`retro64-run`, ≈189 M ciclos del VR4300):
0,75 s en el host sin sanitizers (253 MIPS); 2,1 s con ASan + UBSan.
