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
| Guardado del cartucho (`struct salsa64_save`) | 0,13 | | 131 328 bytes (128 KB de datos alineados a 128 y el estado del chip de FlashRAM, 140 bytes), estático en el frontend: EEPROM (512 B / 2 KB), SRAM (32 KB) o FlashRAM (128 KB). Antes de la FlashRAM (2026-10-11) eran 32 896 |
| Lectura de archivos de guardado (`savefile.c`) | 0,13 | | 128 KB + 1, estático; solo al abrir una ROM |
| Caché del recompilador | 32 | 32 | Fase 3: cola NOBITS del segmento de texto (`mk/rec-cache.ld`, docs/rec-ppe.md); 16 hasta el 2026-10-11, 24 después y 32 desde el desalojo por tramos (es el techo: los saltos al preámbulo alcanzan ±32 MB). nemu64-test entero usa ~9 MB; GoldenEye en la presa ~17 MB |
| Estado del recompilador (`struct salsa64_rec`) | 8,5 | 6,3 | Mapa de bloques (4 MB: uint16_t por palabra de 8 MB), banderas de código (64 KB), 65 535 bloques (1 MB), 131 072 enlaces (1 MB), búferes de compilación (0,2 MB), bloques fuera de KSEG0 y TLB del recompilador (2,2 MB: `xb` 512 KB, `ktlb` 64 KB, `live` 32 KB, `xhash` 32 KB, `snap` 0,75 KB, y los bloques desprendidos: copia de bytes MIPS 1 MB, `copy_off` y `saved_code` 256 KB cada uno, `state` 64 KB); en .bss. 7 172 408 bytes medidos con sizeof el 2026-10-10, +74 KB por la `ktlb` de 16 384 páginas y +1,6 MB por los bloques desprendidos (2026-10-11) |
| Caché de texturas (lado PPE/SPE) | 8 | 8 | `src/rdp`: región mapeada al RSX que administra la HLE del SPE (texturas RGBA8888 decodificadas, entre tareas); directorio de 1024 texturas en el local store. Se puede subir a 16 si el directorio crece |
| Búfer de comandos RSX + memoria mapeada | 16 | 16 | `rsxkit_init` por omisión (rsx-triangle usa 32) |
| ROM del cartucho | 64 | | máximo; el frontend la lee entera a memoria al abrir el juego, en un búfer exactamente de su tamaño (alineado a 128 bytes), igual con `embedded` que con el menú. Con el menú se reserva al abrir cada juego, fuera del bucle de emulación, y se libera al cerrarlo: en el menú no queda ninguna ROM reservada (antes, 64 MB fijos mientras el menú estaba abierto; con Zelda de 32 MB, el juego usa 32 MB menos y el menú libera los 64 MB). Los juegos seguidos se turnan en la misma zona del heap sin fragmentarlo: al liberar, el bloque se junta con su vecino libre |
| Menú para elegir ROM | 2,1 | | 2 MB de memoria principal mapeada al RSX para el 2D (`rsxkit_ui_init`: atlas de la fuente 441 KB y dos búferes de 4096 quads, 768 KB cada uno), reservados al entrar al menú y para siempre; lista de 256 ROMs (74 KB) y texto de guardado.cfg (4 KB), estáticos. Sin memoria local del RSX: dibuja sobre la pantalla |
| Salida de la HLE gráfica SPU → RSX | 20 | 20 | `src/rdp`: 3 juegos de vértices 2 MB + comandos 768 KB + arena de texturas 3,75 MB, mapeados al RSX (`rsxkit_map_main`) junto con la caché de texturas (28 MB en total). Con la caché el arena casi no se usa: se podría achicar más. Los comandos crecieron de 512 a 768 KB cuando `gfx_draw` pasó de 352 a 480 bytes (términos del generador de fragment programs, 2026-10-10) y a 496 con el origen de cada textura (efectos de framebuffer) |
| Efectos de framebuffer (copias RSX → RDRAM, espejos y cebado) | 3 | 3 | `src/rdp`: en el mismo pool mapeado, 2 búferes de 640 × 480 × 2 (RGB565 a la resolución de la N64, 600 KB c/u) para las copias que la emulación espera, 4 de 64 KB para las copias espejo de imágenes chicas y uno de 600 KB para cebar superficies desde la RDRAM; 2,0 MB redondeados a 3 (docs/framebuffer.md) |
| Copias de la imagen del VI entre hilos | 4,4 | 4,4 | Frontend (docs/hilos-ppe.md): 3 ranuras de 640 × 576 × 4; el hilo de emulación copia el framebuffer al terminar el campo y el de presentación lo sube al RSX |
| Zona de paso del guardado + pilas de los hilos de emulación y del latido | 0,4 | | 128 KB en `struct savefile` (131 648 bytes en total), 256 KB de pila del hilo de emulación y 64 KB del latido (`sysThreadCreate`; docs/hilos-ppe.md) |
| Audio | 2 | 0,3 | `src/frontend/audio_out.c`: anillo de `audio_sync` 256 KB (65 536 muestras estéreo de 16 bits, ~2 s a 32 kHz) + estirador y remuestreo 12 KB, estáticos; HLE en el PPE (si no arranca el SPE) 9,2 KB; prueba del arranque (SPE contra PPE) 2 × 16 KB + 9,2 KB; pila del hilo de audio 64 KB; el puerto de audio (16 bloques × 256 × 2 flotantes = 32 KB) es memoria del sistema. Con `volcado_s` en audio.cfg, 200 KB por segundo (hasta 6 MB), reservados al arrancar |
| Estados guardados | 0,4 | | `src/frontend/states.c`: búfer de paso de 256 KB (cabecera y secciones chicas, que con la FlashRAM de 128 KB y la miniatura llegan a ~200 KB; antes del 2026-10-11, 128 KB; la RDRAM va directo de y hacia su búfer, sin copia) y miniatura de 160 × 120 × 2 (37,5 KB), estáticos; imagen de gfx_state del SPE para el estado de la HLE gráfica (48 KB, `gfx_spu.c`: el SPE copia ahí el tramo del RDP) y búfer de paso del de audio (11,75 KB, `audio_spu.c`). docs/estados.md |
| Código, pila, heap de newlib | 16 | | |
| Bloques de control de spukit | 0,01 | 0,01 | ~2,3 KB por trabajador |
| **Total previsto** | **162,5** | | con una ROM de 64 MB (150 de la Fase 2 con la caché de texturas de 8 MB + 6,8 del estado del recompilador + 4,7 de los dos hilos + 0,4 de los estados guardados + 0,3 que agregó la FlashRAM al guardado del cartucho) |

## Memoria local del RSX (GDDR3)

| Uso | Previsto | Medido | Notas |
|---|---|---|---|
| Framebuffers 1280×720 ×2 + Z | 11 | 11 | X8R8G8B8 + Z24S8 (`rsxkit_init`). La Z crece con `rsxkit_depth_reserve` al mayor destino de la N64 cuando es más grande que la pantalla (4× y 5× en 720p, 5× en 1080p): a 5× en 720p, 1600×1200×4 = 7,7 MB (4 MB más). Ese agrandamiento se cuenta en el tope de los destinos (fila siguiente) |
| Framebuffer 2D del VI | 3 | 2,8 | `rsxkit_fb_create(640, 576)`: 2 copias × 640 × 576 × 4 |
| Destinos de render de la N64 (Fase 2) | 27 | | `src/rdp/targets.c`: hasta 8 color images y 4 profundidades, con escala por destino según la escala interna (3× a 720p, 4× a 1080p, o 1×–5× por «Escala interna»): 960×720×4 = 2,8 MB c/u a 3×; 1280×960: 4,9 MB a 4×; 1600×1200: 7,7 MB a 5×. Las auxiliares de 64 × 112 de Zelda en pausa van a 5× con la objetivo 3× (320×560×4 = 0,7 MB). Tope total de 112 MiB con el scratch (fila siguiente) y la Z agrandada. Lo normal son 2–4 vivas. Política y peor caso abajo |
| Scratch de los efectos de framebuffer | 2,8 | | `src/rdp`: una copia del destino ligado cuando un dibujo lo lee como textura (la pausa de Zelda), del tamaño (y la escala) del destino copiado: 960×720×4 a 3×, 7,7 MB a 5×; se crea la primera vez que hace falta y cuenta en el tope de 112 MiB |
| Render interno 2x/3x | 60 | | Fase 5 (incluye lo anterior) |
| Texturas | 64 | | Fase 4 |

### Escala interna y tope de los destinos (2026-10-11)

La escala elegida (`salsa64_rdp_init`, 0 = automática, 1–5) es un **objetivo de líneas**: 240 × escala (5× = 1200). Cada destino usa la escala entera que lleva su imagen a ese objetivo sin pasarse (`rdp_target_scale`, mínimo 1, máximo 5, y 4096 por lado): 320×240 a 5×, 640×480 a 2×, una auxiliar de 64 de ancho (48 líneas) a 5× (con tope). La superficie guarda su escala (`surf_scale` en `rdp.c`), la profundidad usa la del destino de color con que se pide, y todo lo que pasa de coordenadas lógicas a píxeles de una superficie (presentación, copias RSX → RDRAM, espejo del framebuffer, render a textura, scratch) usa la escala de esa superficie (`docs/framebuffer.md`, «Escala por destino»); `rsxkit_bind` ya trabaja con tamaño lógico, así que lib/rsxkit no cambió.

| Destino a la escala-objetivo | 1× | 3× | 4× | 5× |
|---|---|---|---|---|
| 320×240 (escala del destino) | 0,3 MB (1×) | 2,8 (3×) | 4,9 (4×) | 7,7 (5×) |
| 640×480 | 1,2 MB (1×) | 1,2 (1×) | 4,9 (2×) | 4,9 (2×) |
| 64×112 (Link en la pausa de Zelda) | 0,1 MB (2×) | 0,7 (5×) | 0,7 (5×) | 0,7 (5×) |

- **Peor caso** (`rdp_vram_worst`): 14 superficies (`RDP_SURFACES_MAX`: 8 color images, el scratch, 4 profundidades y la profundidad de la pantalla agrandada, que se cuenta entera aunque la de la pantalla ya existiera). Cada una mide a lo más 320·g × 240·g (o 640×480 nativa si es mayor), en cada lado: el alto lógico de un destino es al menos 3/4 del ancho y `rdp_target_scale` no pasa de 240·g líneas, así que el ancho no pasa de 320·g; el scratch es la copia de un destino y una profundidad compartida por dos destinos crece al mayor de los dos en cada lado (las pruebas de `targets` lo recorren para todos los tamaños). Total: 1× y 2× 17,2 MB; 3× 38,7 MB; 4× 68,8 MB; **5× 107,5 MB (102,5 MiB)**. rsxkit admite 16 destinos y 16 profundidades; el RDP usa a lo más 9 y 4.
- **Tope total:** `RDP_VRAM_BUDGET` = **112 MiB** (2026-10-11; antes 96) para destinos, scratch, profundidades y la Z agrandada. Con 13 superficies el peor caso a 5× (95,2 MiB) entraba en 96 MiB con 0,8 MiB de sobra y `make_room` no llegaba a reciclar; con la Z agrandada son 14 y 102,5 MiB, que con 96 MiB habrían bajado 5× a 4× al arrancar: se subió a 112 MiB, con 9,5 MiB de sobra. Los 256 MB locales a 1080p: pantallas 24,9 MB, 2D del VI 2,8 MB, menú y fragment programs unos MB; con los 112 MiB quedan ~100 MB para la fragmentación del montón de `rsxMemalign` y las texturas de la Fase 4.
- **Destinos sin z image** (rectángulos, 2D, cebado, scratch): se ligan con la Z de la pantalla, con prueba y escritura apagadas. El RSX nunca recibe una zeta más chica que el color: `apply_scale` (`rdp.c`, al iniciar y con «Aplicar ahora») agranda la Z de la pantalla al mayor destino posible a la escala (`rdp_zeta_reserve`; si no hay memoria, baja la escala) y `rsxkit_bind` se niega a ligar si no la cubre (el dibujo se salta y el log lo cuenta como «dibujos sin ligar»). Razones de la elección en `src/rdp/rdp.h`.
- **Al arrancar:** `rdp_scale_pick` baja el objetivo mientras el peor caso pase del tope; hoy 5× entra, así que no se reduce nunca, y la regla protege si cambia el máximo o el tope. El log dice «RDP listo: escala-objetivo N× …, pedida|automática[, reducida por memoria del RSX]».
- **En el juego:** `rdp.c` suma los bytes vivos (destinos, scratch y profundidades). Para crear una superficie que pasaría del tope, `make_room` suelta primero el scratch (si no es del destino que lo pide) y después recicla el destino más viejo (menor `last_field`; nunca el presentado en el último campo, el que se está creando o para el que se pide la profundidad, ni uno dibujado hace `RDP_TARGETS_FRESH` tareas o menos, que puede ser la textura del dibujo en curso) hasta que quepa, y cuenta `targets_evicted` (en el log de cada 5 s, «reciclados por memoria»). Si ni así cabe, o rsxkit no tiene memoria, esa color image se queda en la copia de la RDRAM (no se dibuja en el RSX), una profundidad que falta dibuja sin z-buffer y un scratch que falta lee la textura de la RDRAM: nunca falla una creación sin camino de respaldo. Con el peor caso dentro del tope no debería verse nunca.
- **Presentación:** `rsxkit_target_present` lee el destino como textura con filtro lineal; lo más que reduce es 5× en 720p (1600×1200 → 960×720, 1,67×), sin aliasing apreciable. Si la región cabe en la pantalla y un múltiplo entero llena al menos 3/4 del alto (`rdp_present_integer`: 3× en 720p, 4× en 1080p, 1× por 3 en 720p), se presenta a ese múltiplo; si quedaría chica (2× en 720p, 3× en 1080p, un framebuffer de 640×480 a 1×), se ajusta con filtro lineal.

## Local store de los SPEs (256 KB c/u)

| Programa | Código | Datos + búferes | Total | Fuente |
|---|---|---|---|---|
| `spu-compute/spu/mix.c` | 2,6 KB | 0,8 KB + 64 KB | ~69 KB | `spu-size` en el build |
| `spu-jobs/spu/scale.c` | 3,6 KB | 0,8 KB + 65 KB | ~70 KB | `spu-size` |
| `src/spu/audio` (HLE de las listas de audio, SPE 1) | 45 KB | 0,8 KB + 33,6 KB | ~80 KB (+ pila ~176 KB) | `spu-size` (2026-10-11): aud_state 11,7 KB (DMEM 4 KB + margen 4 KB, estado de ABI1, nead y naudio, MP3 2 KB), lecturas y escrituras 2 × 4 KB, bloque de la lista 1 KB, paso del estado guardado 11,75 KB |
| `src/spu/gfx` (HLE de F3DEX2 + RDP, SPE 0) | 99,0 KB | 0,8 KB + 146,3 KB | ~246 KB (+ pila ~9,9 KB) | `spu-size` (2026-10-11, con los efectos de framebuffer): gfx_state 48 KB (de ellos el directorio de la caché de texturas 32 KB), texturas 2 × 32 KB, vértices 2 × 8 KB, comandos 2 × 4 KB, display list 2 × 1 KB, lecturas 5,8 KB. Los estados guardados agregan 0,2 KB de código: el SPE copia tal cual el tramo del estado del RDP y el PPE arma el formato (sin ellos la pila era ~10,2 KB). La cadena más honda de la pila usa ~8 KB |

## Disco (HDD de la PS3)

| Uso | Tamaño | Notas |
|---|---|---|
| Un estado guardado | ~8,1 MB | `/dev_hdd0/salsa64/states/<título>-<CRC1>.stN`: cabecera 64 bytes, miniatura 37,5 KB, CPU y RCP ~3 KB, DMEM/IMEM 8 KB, guardado del cartucho (hasta 128 KB) y, con FlashRAM, el estado del chip (FLSH, 152 bytes), HLE gráfica 4,25 KB y de audio 11,75 KB, RDRAM 8 MB. Medido en el host (mismo formato): 8 415 818 bytes con las ROMs de prueba (sin miniatura), 8 448 602 con Zelda OoT (SRAM de 32 KB), 8 547 058 con Majora's Mask (FlashRAM de 128 KB); con miniatura, +38 416 |
| Diez ranuras de un juego | ~81 MB | Sin compresión (docs/estados.md) |
| Partidas del cartucho | ≤ 128 KB por juego (FlashRAM) | `saves/` |
