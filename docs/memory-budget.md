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
| Estado del recompilador (`struct salsa64_rec`) | 6,8 | 6,3 | Mapa de bloques (4 MB: uint16_t por palabra de 8 MB), banderas de código (64 KB), 65 535 bloques (1 MB), 131 072 enlaces (1 MB), búferes de compilación (0,2 MB), bloques fuera de KSEG0 y TLB del recompilador (0,56 MB: `xb` 512 KB, `xhash` 32 KB, `vtlb` 16 KB); en .bss. 7 172 408 bytes (medido con sizeof el 2026-10-10) |
| Caché de texturas (lado PPE/SPE) | 8 | 8 | `src/rdp`: región mapeada al RSX que administra la HLE del SPE (texturas RGBA8888 decodificadas, entre tareas); directorio de 1024 texturas en el local store. Se puede subir a 16 si el directorio crece |
| Búfer de comandos RSX + memoria mapeada | 16 | 16 | `rsxkit_init` por omisión (rsx-triangle usa 32) |
| ROM del cartucho | 64 | | máximo; el frontend la lee entera a memoria al arrancar. Con `embedded`, a su medida; con el menú, un búfer de 64 MB reservado al entrar al menú y reusado por cada juego (sin asignaciones al cambiar de juego) |
| Menú para elegir ROM | 2,1 | | 2 MB de memoria principal mapeada al RSX para el 2D (`rsxkit_ui_init`: atlas de la fuente 441 KB y dos búferes de 4096 quads, 768 KB cada uno), reservados al entrar al menú y para siempre; lista de 256 ROMs (74 KB) y texto de guardado.cfg (4 KB), estáticos. Sin memoria local del RSX: dibuja sobre la pantalla |
| Salida de la HLE gráfica SPU → RSX | 20 | 20 | `src/rdp`: 3 juegos de vértices 2 MB + comandos 768 KB + arena de texturas 3,75 MB, mapeados al RSX (`rsxkit_map_main`) junto con la caché de texturas (28 MB en total). Con la caché el arena casi no se usa: se podría achicar más. Los comandos crecieron de 512 a 768 KB cuando `gfx_draw` pasó de 352 a 480 bytes (términos del generador de fragment programs, 2026-10-10) y a 496 con el origen de cada textura (efectos de framebuffer) |
| Efectos de framebuffer (copias RSX → RDRAM, espejos y cebado) | 3 | 3 | `src/rdp`: en el mismo pool mapeado, 2 búferes de 640 × 480 × 2 (RGB565 a la resolución de la N64, 600 KB c/u) para las copias que la emulación espera, 4 de 64 KB para las copias espejo de imágenes chicas y uno de 600 KB para cebar superficies desde la RDRAM; 2,0 MB redondeados a 3 (docs/framebuffer.md) |
| Copias de la imagen del VI entre hilos | 4,4 | 4,4 | Frontend (docs/hilos-ppe.md): 3 ranuras de 640 × 576 × 4; el hilo de emulación copia el framebuffer al terminar el campo y el de presentación lo sube al RSX |
| Zona de paso del guardado + pila del hilo de emulación | 0,3 | | 32 KB en `struct savefile` y 256 KB de pila (`sysThreadCreate`) |
| Audio | 2 | 0,3 | `src/frontend/audio_out.c`: anillo de `audio_sync` 256 KB (65 536 muestras estéreo de 16 bits, ~2 s a 32 kHz) + estirador y remuestreo 12 KB, estáticos; HLE en el PPE (si no arranca el SPE) 9,2 KB; prueba del arranque (SPE contra PPE) 2 × 16 KB + 9,2 KB; pila del hilo de audio 64 KB; el puerto de audio (16 bloques × 256 × 2 flotantes = 32 KB) es memoria del sistema. Con `volcado_s` en audio.cfg, 200 KB por segundo (hasta 6 MB), reservados al arrancar |
| Código, pila, heap de newlib | 16 | | |
| Bloques de control de spukit | 0,01 | 0,01 | ~2,3 KB por trabajador |
| **Total previsto** | **162** | | con una ROM de 64 MB (150 de la Fase 2 con la caché de texturas de 8 MB + 6,8 del estado del recompilador + 4,7 de los dos hilos) |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers 1280×720 ×2 + Z | 11 | 11 | X8R8G8B8 + Z24S8 (`rsxkit_init`) |
| Framebuffer 2D del VI | 3 | 2,8 | `rsxkit_fb_create(640, 576)`: 2 copias × 640 × 576 × 4 |
| Destinos de render de la N64 (Fase 2) | 27 | | `src/rdp/targets.c`: hasta 8 color images a 3× (960×720×4 = 2,6 MB c/u; las auxiliares de 64 de ancho de Zelda en pausa pesan 0,25 MB) más un scratch y 4 profundidades (rsxkit admite 16 superficies y 16 profundidades); a 1080p, 4× (1280×960: 4,7 MB c/u, 44 MB en total). Lo normal son 2–4 vivas |
| Scratch de los efectos de framebuffer | 2,6 | | `src/rdp`: una copia del destino ligado cuando un dibujo lo lee como textura (la pausa de Zelda), del tamaño del destino (960×720×4); se crea la primera vez que hace falta |
| Render interno 2x/3x | 60 | | Fase 5 (incluye lo anterior) |
| Texturas | 64 | | Fase 4 |

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
| `spu-jobs/spu/scale.c` | 3,6 KB | 0,8 KB + 65 KB | ~70 KB | `spu-size` |
| `src/spu/audio` (HLE de las listas de audio, SPE 1) | 27 KB | 0,8 KB + 19 KB | ~47 KB (+ pila ~208 KB) | `spu-size`: aud_state 9,2 KB (DMEM 4 KB + margen 4 KB, estado de ABI1 y nead), lecturas y escrituras 2 × 4 KB, bloque de la lista 1 KB |
| `src/spu/gfx` (HLE de F3DEX2 + RDP, SPE 0) | 55 KB | 0,8 KB + 144 KB | ~200 KB (+ pila ~56 KB) | `spu-size`: gfx_state 53 KB (de ellos el directorio de la caché de texturas 32 KB), texturas 2 × 32 KB, vértices 2 × 8 KB, comandos 2 × 4 KB, display list 2 × 1 KB, lecturas 5,8 KB |
