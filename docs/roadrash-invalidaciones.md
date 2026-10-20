<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Road Rash 64: descartes de espera e invalidaciones

La causa de las retraducciones masivas no era una escritura de la RDRAM.
`rec_idle_exit` invalidaba todos los bloques que cubrían el salto cuando
el detector decidía que un bucle no era de espera, para traducirlos otra
vez sin esa consulta. El contador histórico `invalidated` sumaba además
los bloques retirados al desalojar tramos de caché y los recortes por CU1.
El aviso de invalidación masiva restaba solo las escrituras del CPU: el
resto aparecía como «DMA u otros», incluidos estos descartes internos.

Dos saltos de Road Rash, `8003ae0c` y `8003b824`, comparten la ranura **49**
de `SALSA64_IDLE_SLOT`: el descarte de uno sustituía al del otro. Al
retraducirse el código volvía a introducir la consulta para el primer salto.
En el recorrido reproducido hasta el campo 2100, los bloques `8003ad84`,
`8003adf0`, `8003b788` y `8003b7d8` se tradujeron **10 420 veces cada uno**,
con los mismos bytes MIPS. No es evidencia de overlays ni de código
automodificable: se estaba cambiando una decisión del optimizador.

## Cambio

Cada candidato conserva desde la primera traducción tanto la salida a la
consulta como el camino nativo normal. La salida informa el id del bloque
en la mitad alta de `rec_site` (la baja sigue llevando la instrucción MIPS).
Si el detector descarta el bucle, se sustituye por `NOP` **solo la palabra
PPC que salta a la consulta**, con la sincronización de cachés habitual.
El cuerpo, sus enlaces entrantes, los ciclos, el delay slot y las pruebas
de eventos permanecen válidos. Los otros bloques que consulten ese salto
quitan su propia consulta cuando la ejecuten. No se guarda una decisión
nueva en la tabla del intérprete ni se modifica la RDRAM.

Las escrituras reales siguen invalidando los rangos de instrucciones
solapados, con los gemelos FR y los bloques mapeados como antes. No se
reutiliza una traducción basándose en una huella. Los temporales del modo
estricto, que no tienen id persistente, conservan la invalidación anterior.

## Comparación en la PS3

Por omisión: `parche de una palabra PPC`. Crear el archivo vacío
`/dev_hdd0/salsa64/jit-invalidacion-vieja` y volver a abrir el juego para
volver a `recompilacion de rangos`. Quitar el archivo y volver a abrirlo
para activar el parche. El arranque y las líneas `espera:` nombran el modo.
En qemu, `SALSA64_REC_IDLE_RECOMPILE=1` selecciona la comparación anterior.

Cada 300 campos emulados se registran por separado CPU, DMA del PI, otros
escritores y descartes de espera; coincidencias de huella, palabras
parcheadas, retirados por desalojo, repeticiones de la misma dirección,
largo y huella, y ticks/ms del emisor. Al cerrar se añaden los ocho sitios
con más invalidaciones y traducciones. `sin sitio` y `sin compilacion`
cuentan los detalles que no cupieron: los totales por causa no se pierden.
Las tablas son fijas y no se asigna memoria en el bucle. Las huellas sirven
solo para diagnóstico; no se guardan instrucciones del juego en el log.

Repetir la misma carrera en ambos modos y guardar el log completo al volver
al menú: % de NTSC, ms de emulación, peor campo, compilaciones y su tiempo,
desalojos, espera al SPE y traducción al RSX. No hay todavía una medición del
paquete en hardware. Menos compilaciones no permite prometer velocidad
completa: las tareas gráficas y el código nativo siguen costando tiempo.

## Reproducción

Respaldo NTSC propio, RDRAM de 8 MB, Controller Pak nuevo en memoria;
`docs/pads/roadrash-carrera.pad` recoge los botones y posiciones registradas
en el recorrido de la consola (las posiciones son aproximadas).

```
SALSA64_REC_PIN=1 SALSA64_ROMS=/ruta/a/respaldos \
  tools/qemu-ppc64.sh build/ppc64/salsa64-run -q --rec --fields 2100 \
  --pad "$(cat docs/pads/roadrash-carrera.pad)" /roms/roadrash.z64
```

Para paridad, cambiar `--rec` por `--lockstep`. No usar `--gfx-frame` en la
medición del CPU: activa el rasterizador de software para todas las tareas.
Una captura se puede producir aparte desde el estado final en el host.
Los números y las pruebas están en `docs/perf-log.md` y `docs/estado.md`.
