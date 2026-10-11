# Efectos de framebuffer

Estado al 2026-10-10. Código: `src/rdp/targets.c` (política, portable, con
pruebas), `src/rdp/rdp.c` (PS3), `src/host/raster.c` (el mismo modelo en la
Mac), `lib/rsxkit/src/target.c` (lo genérico del RSX), `src/rsp/gfx/tmem.c`
(procedencia de las texturas).

## Problema

Los juegos dibujan en destinos del RSX a resolución interna (3× a 720p) y
nada vuelve a la RDRAM. Cuando un juego usa como textura una imagen que él
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
   y dibuja el menú encima. Los iconos son color images de 64 de ancho
   sobre el framebuffer (`3da800/64`), que se dibujan y se leen como
   textura en la misma tarea (hacia otras de 64: `289600`, `36ce60`), y
   alguna que el CPU escribe después (14 336 escrituras en `289600`).

**Mario Kart 64**: en 50 s de carrera en Luigi Raceway (pad
`mk64-luigi.pad`) ninguna carga a la TMEM viene de un destino y el CPU no
toca los framebuffers. Las «pantallas gigantes» no están en esa escena:
pendiente de saber dónde las vio el autor.

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
a 3×. Si es el destino ligado (Zelda lee lo que está dibujando), se copia
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

**Cebado.** La N64 dibuja siempre encima de lo que hay en la RDRAM. Una
superficie nueva, o sin dibujar desde hace más de `RDP_TARGETS_FRESH`
tareas, se pinta primero con lo que hay en la RDRAM (`rsxkit_target_fill`
desde un búfer mapeado), para que lo que el CPU dibujó ahí (iconos de
Zelda) no se pierda cuando el RDP dibuja encima.

**VISCVG.** No se dibuja: se deja «cobertura plena» (0xFF) en la RDRAM del
rectángulo; el filtro del CPU salta los píxeles cubiertos del todo y el
fondo queda nítido (en la N64 solo se filtran los bordes).

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

- Mario Kart: localizar la escena de las pantallas.
- Perfect Dark y Conker: copias del z-buffer (rectángulos que leen la z
  image como color image), sombra y desenfoque de Conker; color images de
  32 bits y de 8 bits (CI8 de Conker) no se copian ni se ceban.
- Reducir 3× → 1× con filtro de caja (hoy bilineal del RSX: promedia 2 × 2
  de cada 3 × 3) y, si hiciera falta por costo, la conversión en un SPE.
