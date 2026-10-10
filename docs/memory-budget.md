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
| Caché del recompilador | 16 | | Fase 3: cola NOBITS del segmento de texto (docs/jit-ppe.md); 32 si hace falta |
| Caché de texturas (lado PPE/SPE) | 8 | 8 | `src/rdp`: región mapeada al RSX que administra la HLE del SPE (texturas RGBA8888 decodificadas, entre tareas); directorio de 1024 texturas en el local store. Se puede subir a 16 si el directorio crece |
| Búfer de comandos RSX + memoria mapeada | 16 | 16 | `rsxkit_init` por omisión (rsx-triangle usa 32) |
| ROM del cartucho | 64 | | máximo; el frontend la lee entera a memoria al arrancar |
| Salida de la HLE gráfica SPU → RSX | 20 | 20 | `src/rdp`: 3 juegos de vértices 2 MB + comandos 512 KB + arena de texturas 4 MB, mapeados al RSX (`rsxkit_map_main`) junto con la caché de texturas (28 MB en total). Con la caché el arena casi no se usa: se podría achicar |
| Audio | 2 | | |
| Código, pila, heap de newlib | 16 | | |
| Bloques de control de spukit | 0,01 | 0,01 | ~2,3 KB por trabajador |
| **Total previsto** | **150** | | con una ROM de 64 MB |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers 1280×720 ×2 + Z | 11 | 11 | X8R8G8B8 + Z24S8 (`rsxkit_init`) |
| Framebuffer 2D del VI | 3 | 2,8 | `rsxkit_fb_create(640, 576)`: 2 copias × 640 × 576 × 4 |
| Destinos de render de la N64 (Fase 2) | 16 | | `src/rdp`: hasta 4 color images a 3× (960×720×4 = 2,6 MB c/u) y 2 profundidades; a 1080p, 4× (1280×960: 4,7 MB c/u) |
| Render interno 2x/3x | 60 | | Fase 5 (incluye lo anterior) |
| Texturas | 64 | | Fase 4 |

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
| `spu-jobs/spu/scale.c` | 3,6 KB | 0,8 KB + 65 KB | ~70 KB | `spu-size` |
| `src/spu/gfx` (HLE de F3DEX2 + RDP, SPE 0) | 55 KB | 0,8 KB + 144 KB | ~200 KB (+ pila ~56 KB) | `spu-size`: gfx_state 53 KB (de ellos el directorio de la caché de texturas 32 KB), texturas 2 × 32 KB, vértices 2 × 8 KB, comandos 2 × 4 KB, display list 2 × 1 KB, lecturas 5,8 KB |
