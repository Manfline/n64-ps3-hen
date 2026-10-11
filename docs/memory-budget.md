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
| Estado del recompilador (`struct salsa64_rec`) | 8,5 | 6,3 | Mapa de bloques (4 MB: uint16_t por palabra de 8 MB), banderas de código (64 KB), 65 535 bloques (1 MB), 131 072 enlaces (1 MB), búferes de compilación (0,2 MB), bloques fuera de KSEG0 y TLB del recompilador (2,2 MB: `xb` 512 KB, `ktlb` 64 KB, `live` 32 KB, `xhash` 32 KB, `snap` 0,75 KB, y los bloques desprendidos: copia de bytes MIPS 1 MB, `copy_off` y `saved_code` 256 KB cada uno, `state` 64 KB); en .bss. 7 172 408 bytes medidos con sizeof el 2026-10-10, +74 KB por la `ktlb` de 16 384 páginas y +1,6 MB por los bloques desprendidos (2026-10-11) |
| Caché de texturas (lado PPE/SPE) | 8 | 8 | `src/rdp`: región mapeada al RSX que administra la HLE del SPE (texturas RGBA8888 decodificadas, entre tareas); directorio de 1024 texturas en el local store. Se puede subir a 16 si el directorio crece |
| Búfer de comandos RSX + memoria mapeada | 16 | 16 | `rsxkit_init` por omisión (rsx-triangle usa 32) |
| ROM del cartucho | 64 | | máximo; el frontend la lee entera a memoria al abrir el juego, en un búfer exactamente de su tamaño (alineado a 128 bytes), igual con `embedded` que con el menú. Con el menú se reserva al abrir cada juego, fuera del bucle de emulación, y se libera al cerrarlo: en el menú no queda ninguna ROM reservada (antes, 64 MB fijos mientras el menú estaba abierto; con Zelda de 32 MB, el juego usa 32 MB menos y el menú libera los 64 MB). Los juegos seguidos se turnan en la misma zona del heap sin fragmentarlo: al liberar, el bloque se junta con su vecino libre |
| Menú para elegir ROM | 2,1 | | 2 MB de memoria principal mapeada al RSX para el 2D (`rsxkit_ui_init`: atlas de la fuente 441 KB y dos búferes de 4096 quads, 768 KB cada uno), reservados al entrar al menú y para siempre; lista de 256 ROMs (74 KB) y texto de guardado.cfg (4 KB), estáticos. Sin memoria local del RSX: dibuja sobre la pantalla |
| Salida de la HLE gráfica SPU → RSX | 20 | 20 | `src/rdp`: 3 juegos de vértices 2 MB + comandos 768 KB + arena de texturas 3,75 MB, mapeados al RSX (`rsxkit_map_main`) junto con la caché de texturas (28 MB en total). Con la caché el arena casi no se usa: se podría achicar más. Los comandos crecieron de 512 a 768 KB cuando `gfx_draw` pasó de 352 a 480 bytes (términos del generador de fragment programs, 2026-10-10) |
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
| Destinos de render de la N64 (Fase 2) | 27 | | `src/rdp/targets.c`: hasta 8 color images y 2 profundidades, con escala por destino según la escala interna (3× a 720p, 4× a 1080p, o 1×–5× por «Escala interna»): 960×720×4 = 2,6 MB c/u a 3×; 1280×960: 4,7 MB a 4×; 1600×1200: 7,7 MB a 5×. Tope total de 96 MiB. Las auxiliares de 64 de ancho de Zelda en pausa pesan 0,1 MB. Lo normal son 2–4 vivas. Política y peor caso abajo |
| Render interno 2x/3x | 60 | | Fase 5 (incluye lo anterior) |
| Texturas | 64 | | Fase 4 |

### Escala interna y tope de los destinos (2026-10-11)

La escala elegida (`salsa64_rdp_init`, 0 = automática, 1–5) es un **objetivo de líneas**: 240 × escala (5× = 1200). Cada destino usa la escala entera que lleva su imagen a ese objetivo sin pasarse (`rdp_target_scale`, mínimo 1, máximo 5, y 4096 por lado): 320×240 a 5×, 640×480 a 2×, una auxiliar de 64 de ancho (48 líneas) a 5× (con tope). La superficie guarda su escala (`surf_scale` en `rdp.c`), la profundidad usa la del destino de color con que se pide, y la presentación convierte su región con la escala del destino; `rsxkit_bind` ya trabaja con tamaño lógico, así que lib/rsxkit no cambió. No hay otra copia RSX ↔ RDRAM que suponga la escala.

| Destino a la escala-objetivo | 1× | 3× | 4× | 5× |
|---|---|---|---|---|
| 320×240 (escala del destino) | 0,3 MB (1×) | 2,8 (3×) | 4,9 (4×) | 7,7 (5×) |
| 640×480 | 1,2 MB (1×) | 1,2 (1×) | 4,9 (2×) | 4,9 (2×) |

- **Peor caso** (`rdp_vram_worst`): 10 superficies (8 color images y 2 profundidades), cada una a lo más del peso de una de 320×240 a la escala-objetivo (o de 640×480 nativa si es mayor): 1× 12 MB; 3× 28 MB; 4× 49 MB; 5× 77 MB (73 MiB). Antes, con escala global, ocho de 640×480 a 5× eran 245 MB.
- **Tope total:** `RDP_VRAM_BUDGET` = 96 MiB para destinos y profundidades vivos (los 256 MB locales a 1080p: pantallas 24 MB, 2D del VI 2,8 MB, menú y shaders unos MB; quedan ~120 MB de margen para la fragmentación del montón de `rsxMemalign`).
- **Al arrancar:** `rdp_scale_pick` baja el objetivo mientras el peor caso pase del tope; con las cifras de arriba 5× entra (73 MiB), así que no se reduce nunca hoy, y la regla protege si cambia el máximo o el tope. El log dice «RDP listo: escala-objetivo N× …, pedida|automática[, reducida por memoria del RSX]».
- **En el juego:** `rdp.c` suma los bytes vivos. Para crear un destino o una profundidad que pasaría del tope, `make_room` recicla el destino más viejo (menor `last_field`; nunca el presentado en el último campo ni el que se está creando o para el que se pide la profundidad) hasta que quepa, y cuenta `targets_evicted`. Si ni así cabe, o rsxkit no tiene memoria, esa color image se queda en la copia de la RDRAM (no se dibuja en el RSX) y una profundidad que falta dibuja sin z-buffer: nunca falla una creación sin camino de respaldo.
- **Presentación:** `rsxkit_target_present` lee el destino como textura con filtro lineal; lo más que reduce es 5× en 720p (1600×1200 → 960×720, 1,67×), sin aliasing apreciable. A 1× la imagen se amplía con el mismo filtro lineal.

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
| `spu-jobs/spu/scale.c` | 3,6 KB | 0,8 KB + 65 KB | ~70 KB | `spu-size` |
| `src/spu/audio` (HLE de las listas de audio, SPE 1) | 27 KB | 0,8 KB + 19 KB | ~47 KB (+ pila ~208 KB) | `spu-size`: aud_state 9,2 KB (DMEM 4 KB + margen 4 KB, estado de ABI1 y nead), lecturas y escrituras 2 × 4 KB, bloque de la lista 1 KB |
| `src/spu/gfx` (HLE de F3DEX2 + RDP, SPE 0) | 55 KB | 0,8 KB + 144 KB | ~200 KB (+ pila ~56 KB) | `spu-size`: gfx_state 53 KB (de ellos el directorio de la caché de texturas 32 KB), texturas 2 × 32 KB, vértices 2 × 8 KB, comandos 2 × 4 KB, display list 2 × 1 KB, lecturas 5,8 KB |
