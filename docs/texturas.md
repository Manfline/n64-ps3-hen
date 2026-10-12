# Caché de texturas de la HLE gráfica

Estado al 2026-10-10. Código: `src/rsp/gfx/texcache.c`, `src/rsp/gfx/tmem.c`,
`src/rdp/rdp.c` (región y barreras), `src/spu/gfx/io_spu.c` (DMA).

## Problema

La HLE gráfica (F3DEX2 en el SPE 0) decodificaba en cada tarea todas las
texturas de la TMEM a RGBA8888 en el arena de la tarea, texel por texel. Con
Zelda OoT en la PS3 real eso era lo más caro de la HLE: 3,9–11,3 ms de los
10–21 ms por tarea en las escenas de la intro y el título (docs/perf-log.md),
y la caché que había solo valía dentro de una tarea.

## Diseño

**Identidad por contenido.** Cada textura se identifica por un hash de 64
bits de los bytes de la TMEM que lee su decodificación —las filas de texels
(de las dos mitades si es de 32 bits) y, si usa paleta, el banco de la
paleta (4 bits) o la paleta entera (8 bits)— y de los parámetros de la tile
que cambian el resultado (formato, tamaño, banco, tipo de paleta, `line`,
ancho y alto). No depende de la dirección de origen ni de cómo se cargó.

Se eligió el hash del contenido y no marcas por página de la RDRAM porque:
- Las escrituras del CPU no avisan a nadie (`salsa64_bus_rdram_written` solo
  cubre DMA, y es para el recompilador): marcar cada store del intérprete o
  del recompilador costaría en el camino caliente del CPU y tocaría
  `src/cpu`/`src/core`.
- Lo que hay que leer para el hash ya está en el local store (la TMEM son
  4 KB): con SIMD cuesta ~1 µs por textura de 4 KB, contra ~100 µs de
  decodificarla.
- Cubre de una vez la paleta (LOADTLUT), los cambios de la RDRAM por el CPU,
  por DMA del PI o por el RSP, y las cargas que pisan parte de la TMEM.

Hash: ocho carriles de 32 bits con la ronda de xxHash32 (dos bloques de 16
bytes por vuelta), plegados a 64 bits con su mezcla final. El SPE lo hace con
SIMD y el PPE y el host con C; dan lo mismo (el frontend lo comprueba al
arrancar: «hash de texturas en el SPE: igual al del PPE»; si no, apaga la
caché). Una tile que no cambió desde la última vez en la tarea (sin cargas a
la TMEM ni SETTILE/SETTILESIZE) ni siquiera se vuelve a hashear.

**Dónde viven.** En una región de 8 MB de memoria principal mapeada al RSX
(`src/rdp`, junto a los búferes de salida); el RSX lee las texturas de ahí
sin copiarlas. El directorio (1024 entradas de 28 bytes + índice de 2048
ranuras: 32 KB) vive en el local store del SPE 0, dentro de `gfx_state`.

**Reemplazo.** La región se llena como anillo: las entradas forman una cola en
el orden en que ocupan la región; se reserva en `head_off` y se desaloja lo
que haya delante. Una textura que el RSX aún puede leer no se toca: se salta
y pasa al final de la cola. Así, las texturas que se usan en cada cuadro
nunca se desalojan y el resto sale en orden de llegada. Si en una vuelta
entera no hay lugar (todo en uso), la textura va al arena de la tarea como
antes.

**Sincronización con el RSX.** Cada tarea lleva un número (`job->task`) y la
última tarea que el RSX ya terminó (`job->done_task`), que `src/rdp` saca de
las barreras de sus tres juegos de búferes; una textura usada después de
`done_task` está en uso. `done_task` nunca llega a la tarea anterior, para
que `salsa64_rdp_dump` pueda leer sus texturas mientras el SPE corre la
siguiente. El RSX invalida su caché de texturas al empezar cada tarea
(`rsxkit_vertices`), así que una ranura reescrita se lee bien.

**Volcados.** `salsa64_rdp_dump` copia al arena del volcado las texturas de
la caché que usa la tarea: el formato no cambia y la repetición
(`embedded` = `dump:…`) y `salsa64-run --raster-dump` siguen igual.

## Pruebas

- `tests/unit/test_gfx.c`: la misma textura en otra tarea sale de la caché;
  un texel cambiado en la RDRAM (el último byte del tramo cargado) o una
  entrada de la paleta cambiada dan otra textura; el anillo con el
  consumidor dos tareas atrás nunca desaloja ni pisa una textura en uso,
  no encima entradas y mantiene el orden (también con el directorio lleno);
  el hash ve exactamente el tramo pedido, también al dar la vuelta.
- gfxtest: L (textura RGBA16) y M (paleta de una CI4) que el CPU cambia en la
  RDRAM en el cuadro 60; se verifica la imagen después. Con un hash que no
  ve el último texel ni la paleta, fallan esos dos puntos.

## Medición en la PS3 real

Zelda OoT, intro y título, con el recompilador (docs/perf-log.md, «Caché de
texturas»): la HLE en el SPE baja de 14–31 ms a 5–9,6 ms por tarea; la
decodificación, de 6,7–17,2 ms a 0,00–0,09 ms (0–1 textura decodificada
por tarea, sin desalojos en 3510 campos); el hash y la búsqueda cuestan
0,03–0,35 ms. Las escenas que iban a 41–48 campos/s llegan a 60, y las
capturas en campos fijos son idénticas byte a byte a las de antes.

## Texturas que cambian en cada cuadro: Perfect Dark (2026-10-11)

En la PS3 real, la intro de Perfect Dark (la azotea de dataDyne, desde el
campo ~1800, sin tocar el control) bajaba a 50 campos/s: la HLE tardaba
40–58 ms por tarea, casi todo en decodificar 129–143 texturas por tarea,
cada una desalojando otra. Medido en el host con `salsa64-run --gfx-frame`
(el rasterizador hace las mismas copias a la RDRAM que la PS3) y
`SALSA64_TEXSTAT=100` (build con `-DSALSA64_GFX_TRACE`):

- **No es la caché.** Sin el rasterizador (sin copias a la RDRAM) la escena
  decodifica 0,1–0,5 texturas por tarea. Con él, 180 por tarea, todas con
  una clave nunca vista y desde un origen ya visto con otro contenido.
- **Es un desenfoque de movimiento.** Después de la escena, el juego copia
  el cuadro anterior (el otro framebuffer, `665dc0/320`, que el espejo del
  framebuffer deja en la RDRAM) sobre el nuevo, fila por fila: 180 veces
  `LOADBLOCK` de una fila (320 texels RGBA16) y un `TEXRECT` de 320 × 1
  con filtro puntual y alfa 0x17 (9 %). El contenido cambia en cada cuadro:
  ninguna caché lo evita.
- **Lo caro era el tamaño de la tile.** La tile es de 513 × 9 (línea de
  1280 bytes: más grande que la TMEM, da la vuelta), y la HLE decodificaba
  la tile entera: 4617 texels por franja para 320 que se muestrean,
  831 000 texels por tarea. En el SPE, el bucle genérico costaba ~88 ns por
  texel (lecturas y escrituras de 2 y 4 bytes sin alineación conocida:
  byte por byte, y cada escritura relee el quadword).

Arreglos (`src/rsp/gfx/rdp.c`, `tmem.c`):

1. **Solo lo que se muestrea.** `gfx_texrect` calcula hasta qué texel puede
   leer el RSX en cada eje (todo u dentro del rectángulo, también entre las
   muestras del RDP a más resolución, más el vecino con filtro) y, si la
   tile no cabe en la TMEM y el eje se limita (no repite), la textura se
   decodifica solo hasta ahí (`gfx_tile_texture(…, lim)`, que entra en la
   clave). Las franjas quedan en 321 × 2: 116 000 texels por tarea en vez
   de 831 000 (×7,2). Con tiles que caben en la TMEM no cambia nada: los
   rectángulos que comparten una tile (tiras de un fondo de S2DEX, letras)
   siguen en un dibujo y una textura.
2. **Decodificación sin accesos byte por byte.** RGBA16 (y CI16 sin paleta)
   va de palabra en palabra de 64 bits de la TMEM, cuatro texels por vuelta
   con las mitades intercambiadas en las filas impares, convertidos a la
   vez con los vectores de GCC y clang (el mismo código en el host, el PPE
   y el SPE: en el SPE, ~48 instrucciones por cuatro texels, ~10 ciclos por
   texel, contra ~280 antes); los demás formatos leen con accesos
   alineados; todo se escribe de cuatro en cuatro texels (en el SPE, un
   almacenamiento de 16 bytes). La salida es la misma byte a byte (prueba
   `gfx_tmem_decode_formats` contra una referencia aparte, con filas
   impares, vueltas de la TMEM y anchos que no son múltiplo de 4).

En el host, con el rasterizador: 136 cuadros de Perfect Dark, Zelda, SM64,
Mario Kart, GoldenEye, Banjo y Conker y gfxtest ×3 idénticos byte a byte;
en los demás juegos, las mismas texturas decodificadas y los mismos
dibujos.

Para que el código nuevo no se comiera la pila del SPE 0, el registro de
texturas del arena por tarea (para no decodificar dos veces la misma si
la caché está llena o apagada) bajó de 128 a 64 entradas (−3 KB).

**En la PS3 real** (c17e790, docs/perf-log.md): la azotea pasa de 50–51 a
59,8–60,3 campos/s; la HLE en el SPE, de 40–61 a 3,3–5,0 ms por tarea, y
decodificar, de 37–58 a 0,5–0,65 ms con 133–181 texturas nuevas por tarea
(3,6 µs por franja). Zelda, GoldenEye y Conker, igual que antes. Lo que
queda lento en la intro de Perfect Dark (45–53 campos/s en la escena
siguiente) es el CPU emulado, no la HLE.

## Decisión: SPE 0 o SPE 2–3

**La caché y la decodificación se quedan en el SPE 0.** Con la caché, lo
que queda de decodificar es 0–0,09 ms por tarea en promedio: mover eso a
otro SPE no ahorra nada y agregaría sincronización (la tarea necesita el
lugar de la textura para escribir el dibujo) y copias de la TMEM entre
SPEs. Lo que hoy cuesta en el SPE 0 son las cargas a la TMEM (1,4–3,0 ms,
DMA fila por fila), el estado de dibujo (1,3–2,9 ms) y los vértices
(1,1–1,6 ms), que no se reparten bien.

Los SPE 2–3 siguen reservados para lo que sí pesa por textura y no por
tarea: escalado de texturas (xBR/ScaleFX, Fase 5) y, si aparecen juegos
que cambian muchas texturas por cuadro (decodificación en ráfaga al
cambiar de escena), decodificación por adelantado. Se revisa si el peor
campo en un cambio de escena pasa de un campo de tele (16,7 ms) por
decodificar.

**Revisado con Perfect Dark (2026-10-11):** es el primer juego que cambia
muchas texturas en cada cuadro (180 franjas del cuadro anterior por
tarea), y aun así la decisión se mantiene. Decodificar solo lo que se
muestrea y sin accesos byte por byte deja 0,5–0,65 ms por tarea en el SPE
0, menos que lo que costaría repartir el trabajo y sincronizar otro SPE.
Si un juego decodificara en cada cuadro tiles enteras grandes (no franjas),
lo siguiente sería la conversión con SIMD del SPE en toda la fila y, solo
después, otro SPE.

## Pendiente

- Cargas a la TMEM: pedir por adelantado las filas de LOADTILE (hoy un DMA
  que espera por fila: ~2300 lecturas por tarea en Zelda).
- Decodificación por formato con SIMD y salidas de 16 bits del RSX
  (R5G5B5A1 tal cual, IA como G8B8) en `lib/rsxkit/` (MIT): solo importa
  cuando falla la caché; medir el peor campo en cambios de escena antes.
  RGBA16 ya va de 4 en 4 texels (vectores de GCC); los demás formatos,
  texel por texel con accesos alineados.
- El desenfoque de Perfect Dark lee el cuadro anterior desde la RDRAM (la
  copia a 1× del espejo del framebuffer): leerlo del destino del RSX
  (render a textura) lo vería a la resolución interna y no decodificaría
  nada, pero la procedencia de las franjas no se reconoce (`LOADBLOCK` de
  una fila con una tile de 1280 bytes por línea: «paso 640» contra un
  destino de 320) y el SPE tendría que saber de antemano que no hace falta
  decodificar.
- El arena de texturas por juego (4 MB × 3) casi no se usa: se puede
  achicar.
