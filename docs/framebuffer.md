# Efectos de framebuffer

Estado al 2026-10-11. Código: `src/rdp/targets.c` (política, portable, con
pruebas), `src/rdp/rdp.c` (PS3), `src/host/raster.c` (el mismo modelo en el
host), `lib/rsxkit/src/target.c` (lo genérico del RSX), `src/rsp/gfx/tmem.c`
(procedencia de las texturas). Confirmado en la PS3 real: fondo y Link de
la pausa de Zelda OoT, la cámara del túnel de Mario Kart 64, gfxtest 35/35.

## Problema

Los juegos dibujan en destinos del RSX a resolución interna (3× a 720p con
la escala automática; cada destino con su propia escala, ver «Escala por
destino») y nada vuelve a la RDRAM. Cuando un juego usa como textura una imagen que él
mismo dibujó, o el CPU la lee y la modifica, en la RDRAM hay basura o la
imagen vieja: el fondo de la pausa de Zelda salía mal y faltaba Link en la
pantalla de equipo.

## Qué hacen los juegos (medido en el host con `--trace-fb`)

**Zelda OoT, al pausar** (tarea 5864 de `docs/pads/zelda-pausa-nueva.pad`,
campo 16107), en una sola tarea:

1. Dibuja el cuadro en el framebuffer (`3da800`, 320 de ancho).
2. Copia ese framebuffer, en 40 franjas de 320 × 6 (`LOADTILE` + `TEXRECT`),
   a una color image de 320 en la dirección del z-buffer (`12c700`,
   `fbufSave`).
3. Vuelve al framebuffer y lo copia **sobre sí mismo** en otras 40 franjas
   (`G_RM_PASS`) y después un rectángulo con `G_RM_VISCVG` (IM_RD |
   FORCE_BL, blender IN·0 + BL·A_MEM): el RDP escribe en cada píxel su
   cobertura, para el filtro de bordes del CPU.
4. Entre esa tarea y la siguiente (64 campos después), el CPU lee el
   framebuffer 1,2 millones de veces (16 por píxel: el filtro de
   antialiasing/divot sobre `fbufSave` guiado por la cobertura) y escribe el
   resultado en `fbufSave`.
5. Cada cuadro de la pausa restaura el fondo desde `fbufSave` (40 franjas)
   y dibuja el menú encima. Link se dibuja en **cada tarea** de la pausa
   en una color image de 64 × 112 en los primeros bytes del framebuffer
   que toca (`3da800/64` o `3b5000/64`, con su z-buffer 14 KB después:
   `3de000/64`, `3b8800/64`), aunque nadie lo lea; al abrir la pausa y al
   cambiar de equipo el RDP lo copia en 4 rectángulos a un búfer del heap
   (`fbufSave` de Link, `279f00/64` o `289600/64` según la sala), deja su
   cobertura con VISCVG y la copia a otro búfer **de 8 bits de intensidad**
   (`cvgSave`, `35d760/64`), y el CPU filtra ese búfer leyendo los dos. La
   pantalla de equipo lee a Link desde el búfer del heap, como textura,
   durante todo el tiempo que esté abierta. Dos color images imposibles
   (anchos 1495 y 3770) aparecen en las tareas de apertura: un comando mal
   interpretado por la HLE; se ignoran. La cobertura del fondo también es
   de 8 bits y vive en la dirección del framebuffer (`3da800/320 siz 1`).

**Mario Kart 64**: la «pantalla gigante» es la cámara sobre la entrada
del túnel de Luigi Raceway. El volcado de la PS3 frente al túnel tiene una
sola color image (el framebuffer) y ninguna textura con origen en un
destino: el CPU lee el framebuffer y lo reduce a la textura de la pantalla
(en 50 s de carrera lejos del túnel, con `mk64-luigi.pad`, no lo hace). De
paso: todas las texturas del volcado salían sin origen porque libultra
manda ancho 1 en `SETTIMG` para `LOADBLOCK`; ahora un bloque son texels
seguidos y el paso lo da la tile que lee.

**Conker y Perfect Dark**: nada en 3 minutos de intro (los efectos están
dentro del juego: pausa, sombra, cámara espía).

## Diseño

Tres mecanismos, decididos con la tabla de destinos (`targets.c`):

**Procedencia de las texturas.** La HLE anota las últimas cargas a la TMEM
(`gfx_load`: tramo escrito, dirección en la RDRAM, filas) y, al resolver una
tile, deja en `gfx_tex.src_*` la dirección del texel (0, 0) de la textura en
la imagen de origen, su paso en texels y su formato. Se asigna después de la
búsqueda en la caché: dos tiles con el mismo contenido comparten textura
pero no origen. Cuesta un registro de 32 bytes por carga.

**Render a textura.** Si el origen cae en un destino vivo
(`rdp_targets_texture`): dibujado hace `RDP_TARGETS_FRESH` (3) tareas o
menos y no copiado a la RDRAM, la textura se lee de su superficie del RSX
(`rsxkit_target_texture`: filas enteras desde `y0`, la columna va en `xf`),
a la escala de ese destino. Si es el destino ligado (Zelda lee lo que está dibujando), se copia
antes la superficie a un scratch (`rsxkit_target_copy`, una vez por tarea)
y se lee de ahí. Antes del primer dibujo que lee una superficie dibujada en
el mismo tramo, `rsxkit_texture_sync` (espera al RSX e invalida su caché de
texturas). Si el destino es viejo o se copió a la RDRAM, vale la textura
decodificada por la HLE desde la RDRAM, como antes.

**Copia RSX → RDRAM.** Solo para las color images de 16 bits del ancho del
VI que no son ninguno de los framebuffers recientes (`rdp_readback_wanted`
con el anillo de orígenes del VI): un cuadro guardado aparte, que el CPU
va a leer. Lo decide `done` en el hilo de emulación (recorre las cabeceras
de los registros de la tarea, sin tabla), despierta al hilo de presentación
y espera a que la copia esté en la RDRAM (`readback_done`), para que
cuando llegue la interrupción del DP la RDRAM esté al día; si tarda más
de 200 ms, sigue y lo cuenta (`tarde`). El hilo de presentación, al
traducir la tarea, manda al RSX reducir el destino a la resolución de la
N64 (`rsxkit_target_read`: un quad con el shader del blit sobre una
superficie R5G6B5 **en memoria principal mapeada**, filtro lineal), espera
la barrera y convierte a RGBA5551 escribiendo en la RDRAM
(`rsxkit_pixels_565_to_5551`, 4 píxeles por palabra). Desde entonces el
destino es «del CPU» (`cpu_owned`) hasta que el RDP lo vuelva a dibujar.
Qué copias espera la emulación (`readbacks_wanted`, hilo de emulación,
recorriendo los registros de la tarea): las imágenes auxiliares de 16
bits o de 8 de intensidad (las que no son un framebuffer reciente del VI,
de cualquier ancho razonable) cuando **acaban de aparecer** (no se pidieron
en la tarea anterior: el fondo de la pausa de Zelda y los búferes de Link,
una vez por pausa o por cambio de equipo), y las **chicas (≤ 64 KB) que
ningún dibujo de la tarea lee como textura**, pero solo las dos primeras
tareas seguidas: su único lector posible es el CPU, y una imagen que se
redibuja en cada tarea (Link en la pausa) la recibe con una tarea de
retraso sin que se note, mientras que esperarla costaba ~22 ms por tarea.
Las demás van sin esperar: las chicas que lee el RDP (iconos), y las
grandes repetidas (la intro de Conker dibuja cada tarea una color image de
292 que no se presenta y el RDP no lee: 150 copias esperadas en 5 s, 16–18
ms cada una, frenaban), que se copian en un búfer grande y el CPU recibe
el cuadro anterior.

**Espejo del framebuffer.** Lo que el RDP dibuja en un framebuffer se
reduce en el RSX a un búfer y, pasada su barrera, el PPE lo escribe en la
RDRAM (sin esperas), **una sola vez por dibujo**: al terminar la tarea que
lo dibujó si el VI ya lo mostró alguna vez (16 bits, del ancho del VI) y,
si no, al presentarlo por primera vez (solo las filas que muestra el VI).
Mario Kart fabrica con el CPU la textura de la cámara del túnel de Luigi
Raceway leyendo el framebuffer: sin el espejo la pantalla salía negra. En
la PS3 cuesta 0,0 ms medidos por campo en presentación (GoldenEye en Dam
sigue a 45,8 campos/s, su rango). El espejo no pisa durante 32 tareas la
memoria de una copia reciente que se solape con el framebuffer (Link en sus
primeros 14 KB; la cobertura plena de VISCVG, que el filtro del CPU lee
durante ~1 s): sin eso, el CPU copiaba la esquina de la pantalla de pausa
en vez de a Link.

Antes (bf836f2) se copiaba el framebuffer presentado **en cada campo**, y
eso pisa lo que el CPU o un DMA escriban en esa memoria después de que el
RDP la dibujó: Super Mario 64, al pasar del logo a la cara de Mario (campo
~343), dibuja un framebuffer (tarea 95), carga por DMA ~200 KB de datos de
la cara encima de dos de sus framebuffers (`390000`–`3c2780`) mientras el
VI todavía muestra uno, y lo presenta un campo más antes de apagar el VI;
la copia pisaba esos datos y el juego no volvía a mandar tareas gráficas
(en el host se quedaba en 96 tareas con `SALSA64_FBFX=2`). Copiarlo al
presentarlo «una vez» tampoco basta (ese framebuffer se presenta por
primera vez *después* del DMA): por eso la copia sigue al dibujo, como en
la N64. En el host la copia es síncrona; en la PS3 llega a la RDRAM en una
traducción o presentación posterior (uno o dos campos), así que un DMA
sobre un framebuffer en el mismo campo en que el RDP lo terminó todavía
podría quedar debajo de la copia (en SM64 pasan ~4 campos entre la tarea
95 y el DMA).

Ese caso existe: **Star Fox 64** limpia la pantalla con una tarea (un
FILLRECT) y, antes de que el RSP termine, carga por DMA 113 KB de datos
MIO0 encima de ese mismo framebuffer (`38f800`, en trozos de 256 bytes). En
la consola el RDP acaba en una fracción de ms y el DMA le pasa por encima;
aquí la copia llegaba después (en el host, al terminar la tarea; en la PS3,
ms más tarde) y pisaba la cabecera del MIO0: el juego descomprimía basura y
todos sus hilos quedaban esperando (letras 5 s y negro). Por eso cada
escritura a la RDRAM que no pasa por el CPU (`salsa64_bus_rdram_written`:
DMA del PI, del SP, del SI, la HLE de audio) se anota en un anillo
(`rdp_dma_ring`, targets.h) con la tarea gráfica en vuelo, y **ninguna copia
de la tarea T (espejo del framebuffer, copia esperada, espejo chico) escribe
un tramo que un DMA tocó desde que T empezó**: el dato del DMA es más nuevo.
Un DMA anterior a la tarea sí se pisa, como en la N64. Las escrituras del
propio CPU no se siguen (solo con el build `SALSA64_WATCH_MEM`).

Las imágenes chicas (iconos, pantallas) no se esperan: al terminar la tarea
que las dibujó se encola una **copia espejo** (solo las filas que llegaron
las tijeras, hasta 64 KB, cuatro en vuelo) y se convierte a la RDRAM en
una traducción posterior, cuando el RSX pasó su barrera, sin marcar el
destino como del CPU. Así, mientras son recientes se leen del RSX a 3× y,
cuando un dibujo las lee ya viejas (Link en la pantalla de equipo de Zelda
se dibuja en `289600/64` al abrir la pausa y se lee 395 tareas después), la
RDRAM tiene lo que el RDP dejó. Las copias a la RDRAM escriben solo las
filas dibujadas: el alto de un destino es una conjetura y pasarse
pisaría datos del juego.

**Lo que rompía a Link en la PS3** (y que el host no reproducía porque
copiaba todos los destinos de cada tarea sin llamar a
`rdp_targets_present`; ya lo llama): `rsxkit` admitía 8 superficies y la
pausa tiene 9 color images vivas más el scratch, así que la novena no se
creaba y lo dibujado en ella se perdía (el log cuenta «destinos sin
superficie» y «copias perdidas»; ahora 16); la copia pedida se buscaba
solo por dirección y `3da800` es el framebuffer y el búfer de Link a la
vez; con más de dos copias esperadas por tarea la tercera pisaba el búfer
de la primera; las color images imposibles reciclaban ranuras; la
cobertura de 8 bits no se copiaba; y con dos profundidades los tres
z-buffers de la pausa se destruían y creaban a cada tarea (ahora cuatro).
Copias de 8 bits: solo de intensidad (`G_IM_FMT_I`), con el rojo del
RGB565 como valor; una CI8 no se reconstruye desde el RSX.

**Cebado.** La N64 dibuja siempre encima de lo que hay en la RDRAM. Una
superficie nueva, o sin dibujar desde hace más de `RDP_TARGETS_FRESH`
tareas, se pinta primero con lo que hay en la RDRAM (`rsxkit_target_fill`
desde un búfer mapeado), para que lo que el CPU dibujó ahí (iconos de
Zelda) no se pierda cuando el RDP dibuja encima.

**VISCVG.** No se dibuja: se deja «cobertura plena» (0xFF) en la RDRAM del
rectángulo; el filtro del CPU salta los píxeles cubiertos del todo y el
fondo queda nítido (en la N64 solo se filtran los bordes).

## Escala por destino

La escala interna (1×–5×, `video.cfg` y «Escala interna» del menú) es un
objetivo de 240 · g líneas, y cada destino tiene su propia escala
(`rdp_target_scale`, guardada en `surf_scale`): con la objetivo 3×, el
framebuffer de 320 × 240 va a 3×, Link (64 × 112) a 5× y un framebuffer de
640 × 480 a 1×. Los efectos de framebuffer nacieron con una escala global;
al juntarse, todo lo que pasa de coordenadas lógicas a píxeles de una
superficie usa la escala de **esa** superficie (`rdp_surface_rect`,
`rdp_target_tex_map` en `targets.c`, con pruebas):

- **Copias RSX → RDRAM** (esperadas, grandes sin esperar, espejos chicos y
  el espejo del framebuffer, una vez por dibujo, al terminar la tarea o al
  presentarlo): la región leída es `lw · s × filas · s` con la `s` del
  destino copiado, y el RSX la reduce a `lw × filas`. Con la escala objetivo en su lugar, la copia de Link a 5× leía
  solo la esquina de la superficie, y la de un destino a 1× se salía de
  ella (rsxkit la rechazaba).
- **Render a textura**: se ligan las filas `y0 · s … (y0 + alto) · s` del
  destino leído, con su `s`; u y v van normalizadas, así que el destino en
  que se dibuja puede tener otra escala sin cambiar nada (con la objetivo
  3×, Zelda copia el framebuffer a fbufSave, los dos a 3×; Link, a 5×, se
  copia a su búfer del heap, también a 5×, y la pantalla de equipo lo
  dibuja sobre el framebuffer a 3×).
- **Scratch** (el destino ligado leído como textura): toma el tamaño de la
  superficie copiada y, con él, su escala (`scratch_scale`); si en la
  misma tarea esa superficie se rehace (crece), el scratch deja de valer.
- **Profundidades**: en píxeles de la superficie del destino con que se
  dibujan (rsxkit las pide al menos de ese tamaño). Si una z image ya tiene
  profundidad y no alcanza (la usó un destino de otra escala), se rehace en
  la misma entrada con el mayor tamaño de los dos en cada lado: antes
  quedaban dos profundidades con la misma dirección (zclear borraba la
  vieja) y, entre dos destinos que se turnan, se destruía y creaba a cada
  cambio. Los borrados van en coordenadas lógicas del destino ligado.
- **Cobertura (VISCVG) y cebado**: no dependen de la escala (la cobertura
  se escribe en la RDRAM en puntos lógicos; el cebado estira la imagen de
  la RDRAM sobre la superficie entera).
- **Presentación**: la región en la escala del destino presentado. Se
  presenta a un múltiplo entero solo si cabe y llena al menos 3/4 del alto
  de la pantalla (`rdp_present_integer`: 3× en 720p, 4× en 1080p, 1× por
  3); si quedaría chica (2× en 720p, un framebuffer de 640 × 480 a 1×) se
  ajusta como antes del recorte del overscan.
- **Memoria**: el scratch cuenta en el tope de 112 MiB con los destinos,
  las profundidades y la profundidad de la pantalla agrandada; el peor caso
  son 14 superficies (8 + scratch + 4 + 1) de 320 · g × 240 · g, 102,5 MiB
  a 5× (`docs/memory-budget.md`).
- **Destinos ligados sin profundidad propia** (cebado, scratch, dibujos
  sin z image): van con la profundidad de la pantalla, que a 4×–5× en 720p
  era más chica que el destino. Ahora se agranda al mayor destino posible
  al elegir la escala (`rsxkit_depth_reserve`, `rdp_zeta_reserve`) y
  `rsxkit_bind`, `rsxkit_target_copy`, `rsxkit_target_fill` y
  `rsxkit_target_read` se niegan con una zeta más chica (`RSXKIT_EDEPTH`).
  Elección explicada en `src/rdp/rdp.h`.
- **«Aplicar ahora»** (`salsa64_rdp_set_scale`, entre juegos): suelta
  destinos, scratch y profundidades y descarta las copias en vuelo; la
  memoria mapeada de las copias se conserva (no depende de la escala).

La reducción a 1× es bilineal en el centro de cada bloque de s × s: a
escala impar toma el píxel del centro; a escala par, el promedio de los
2 × 2 centrales (`targets_readback_scaled` lo simula en el host a 1×–5×).

El host (`src/host/raster.c`) sigue la misma política con la misma tabla:
lee texturas de sus imágenes, copia a la RDRAM y ceba, de modo que la
pausa de Zelda en la Mac muestra lo que mostrará la PS3 (sin los tiempos).

## Interruptor y costo

`SALSA64_DATA_DIR/fbfx.cfg`: `0` apaga todo (como antes), `1` solo render
a textura, `2` también copias y cebado (por omisión). En el host,
`SALSA64_FBFX`. El log escribe cada 5 s «gfx: framebuffer: N copias (ms
c/u), la emulación esperó N tareas (ms c/u, N tarde), texturas desde un
destino: del RSX, de la RDRAM, del ligado; destinos cebados». Mediciones en
la PS3 en `docs/perf-log.md`; memoria en `docs/memory-budget.md`.

## Pendiente

- Polígono transitorio (~1 s) al abrir la pausa de Zelda en la PS3:
  confirmar que las cuatro profundidades lo quitan.
- Conker, intro: qué es la color image de 292 que dibuja cada tarea sin
  presentarla ni leerla (el host no la reproduce ni con START y A; en la
  PS3 sí): un `gfxdump` en esa escena lo diría.
- Perfect Dark y Conker: copias del z-buffer (rectángulos que leen la z
  image como color image), sombra y desenfoque de Conker; color images de
  32 bits y CI8 (Conker) no se copian ni se ceban.
- Reducir s× → 1× con filtro de caja (hoy bilineal del RSX en el centro de
  cada bloque: el píxel central a escala impar, 2 × 2 a escala par) y, si
  hiciera falta por costo, la conversión en un SPE.
- Confirmar en la PS3 real la pausa y la pantalla de equipo de Zelda y el
  túnel de Mario Kart a 1×, 2× y 5× (en RPCS3, gfxtest 35/35 a 1×, 3× y 5×).
- La HLE emite dos color images imposibles al abrir la pausa de Zelda
  (anchos 1495 y 3770): de qué comando salen.
