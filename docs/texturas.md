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

## Decisión: SPE 0 o SPE 2–3

Pendiente de las mediciones en la PS3 (docs/perf-log.md).
