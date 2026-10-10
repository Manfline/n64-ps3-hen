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
