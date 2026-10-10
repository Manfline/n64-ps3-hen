# Presupuesto de memoria

PS3: 256 MB XDR (principal) + 256 MB GDDR3 (RSX). El juego tiene ~213 MB de
XDR disponibles; el resto es del sistema. Medido con HEN (docs/jit-ppe.md):
214 016 KB (209 MB) de memoria de usuario, y los segmentos del ELF (código,
datos y colas NOBITS) se descuentan de ahí. Cifras en MB; "medido" se llena
en PS3 real.

## Memoria principal (XDR)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| RDRAM emulada | 8 | | 4 MB + Expansion Pak; la pone quien llama a `salsa64_n64_init` |
| Estado de la máquina (`struct salsa64_n64`) | 0,01 | 0,01 | 10,4 KB: CPU 1,7 KB, bus 8,5 KB (incluye SP DMEM/IMEM) |
| Guardado del cartucho (`struct salsa64_save`) | 0,03 | | 32 896 bytes (32 KB de datos alineados a 128), estático en el frontend: EEPROM (512 B / 2 KB) o SRAM (32 KB). Crece a 128 KB con FlashRAM o SRAM de 1 Mbit |
| Lectura de archivos de guardado (`savefile.c`) | 0,03 | | 32 KB + 1, estático; solo al abrir una ROM |
| Caché del recompilador | 16 | 16 | Fase 3: cola NOBITS del segmento de texto (`mk/rec-cache.ld`, docs/rec-ppe.md); 32 si hace falta. nemu64-test entero usa ~9 MB |
| Estado del recompilador (`struct salsa64_rec`) | 6,3 | 6,3 | Mapa de bloques (4 MB: uint16_t por palabra de 8 MB), banderas de código (64 KB), 65 535 bloques (1 MB), 131 072 enlaces (1 MB), búferes de compilación (0,2 MB); en .bss |
| Caché de texturas (lado PPE/SPE) | 16 | | Fase 4 |
| Búfer de comandos RSX + memoria mapeada | 16 | 16 | `rsxkit_init` por omisión (rsx-triangle usa 32) |
| ROM del cartucho | 64 | | máximo; el frontend la lee entera a memoria al arrancar |
| Listas de triángulos SPU → RSX | 8 | | doble búfer por cuadro |
| Audio | 2 | | |
| Código, pila, heap de newlib | 16 | | |
| **Total previsto** | **152** | | con una ROM de 64 MB |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers 1280×720 ×2 + Z | 11 | 11 | X8R8G8B8 + Z24S8 (`rsxkit_init`) |
| Framebuffer 2D del VI | 3 | 2,8 | `rsxkit_fb_create(640, 576)`: 2 copias × 640 × 576 × 4 |
| Render interno 2x/3x | 60 | | Fase 5 |
| Texturas | 64 | | Fase 4 |

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
