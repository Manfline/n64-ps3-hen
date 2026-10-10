# Presupuesto de memoria

PS3: 256 MB XDR (principal) + 256 MB GDDR3 (RSX). El juego tiene ~213 MB de
XDR disponibles; el resto es del sistema. Cifras en MB; "medido" se llena en
PS3 real.

## Memoria principal (XDR)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| RDRAM emulada | 8 | | 4 MB + Expansion Pak |
| Caché del recompilador | 32 | | Fase 3; depende de docs/jit-ppe.md |
| Caché de texturas (lado PPE/SPE) | 16 | | Fase 4 |
| Búfer de comandos RSX + memoria mapeada | 32 | | `HOST_SIZE` de rsx-triangle |
| Listas de triángulos SPU → RSX | 8 | | doble búfer por cuadro |
| Audio | 2 | | |
| Código, pila, heap de newlib | 16 | | |
| **Total previsto** | **114** | | |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers 1280×720 ×2 + Z | 11 | | X8R8G8B8 + Z24S8 |
| Render interno 2x/3x | 60 | | Fase 5 |
| Texturas | 64 | | Fase 4 |

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
