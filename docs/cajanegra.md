# Caja negra

Para depurar en la Mac lo que solo pasa en la PS3 (el agua que se sale de su
cauce al interpolar cuadros en Zelda, por ejemplo): la consola guarda en un
anillo en memoria la salida de la HLE gráfica de las últimas tareas y, por
cada campo, lo que se presentó; con un atajo, todo el anillo se escribe a
disco, comprimido y en segundo plano, para bajarlo por FTP y repetirlo en el
host. Decidido y hecho el 2026-10-13.

| Pieza | Dónde |
|---|---|
| El anillo, el formato del archivo y su lectura (portable) | `src/rdp/blackbox.h`, `src/rdp/blackbox.c` |
| El volcado de una tarea (el mismo de `gfxdump.bin`), en memoria | `src/rdp/gfxdump.h`, `src/rdp/gfxdump.c` |
| Gancho en la traducción de cada tarea | `salsa64_rdp_set_task_hook` (`src/rdp/rdp.h`) |
| Registro por campo, atajo, escritura | `src/frontend/cajanegra.c`, `present_field` y `pad_combos` en `src/frontend/main.c` |
| Hilo de baja prioridad, E/S y compresión | `src/platform/ps3/bgjob.c`, `bgio.c`; compresor en `lib/spukit` (`lz.h`, `lzstream.h`, `spu/lz.c`) |
| Lectura en la Mac | `tools/cajanegra.py`, `salsa64-run --raster-dump caja.s4bb[:TAREA]` |
| Pruebas | `tests/unit/test_blackbox.c`, `test_gfxdump.c` |

## Uso

Ver «Caja negra» en `docs/uso-ps3.md`: se enciende con el archivo
`/dev_hdd0/salsa64/cajanegra` (su contenido, opcional, es el tamaño del anillo
en MB: 12 por omisión, de 2 a 48), se pide con **SELECT + L2** y deja
`/dev_hdd0/salsa64/cajanegra-<campo>/caja.s4bb`. Apagada no reserva memoria ni
cuesta nada. Con un segundo número en el archivo (`12 150`) se escribe sola en
ese campo: es para las pruebas automáticas (`tools/rpcs3-run.py --bandera
"cajanegra=4 100"`).

## Qué guarda

Registros, del más viejo al más nuevo; al no caber uno nuevo se tira el más
viejo. Todo el anillo son 12 MB: unas 40 tareas de GoldenEye en la presa
(~300 KB cada una con sus texturas) o centenares de las de Zelda.

- **TASK** — la salida de la HLE de una tarea gráfica: comandos, vértices y
  texturas en el formato del volcado de `gfxdump.bin` (`gfx.h`:
  `gfx_dump_header`), autosuficiente (las texturas de la caché que usan los
  dibujos van copiadas detrás del arena y los dibujos apuntan ahí; las que
  se leen de un destino, decodificadas de la RDRAM de ese momento). Va
  precedido de 8 palabras: número de tarea (la numeración de `rdp.c`), campo
  que presentación procesaba, **tarea registrada antes** (y si es la
  inmediata, `BB_TASK_ADJ`: para repetir el «cuadro anterior» de la
  interpolación basta el registro de al lado), dibujos, triángulos,
  rectángulos y las banderas de `gfx_result`.
- **FILD** — un campo (32 palabras): el campo, banderas (presentado, saltado,
  de qué salió la imagen: RSX o RDRAM, sin imagen), tareas terminadas y
  traducidas hasta ahí, la imagen que leyó el VI (tipo, origen, ancho, alto,
  paso, bytes por píxel), **los 14 registros del VI** al terminar el campo,
  el **tipo de cuadro** (verdadero, intermedio, repetido), su **t** (float),
  los campos por cuadro del juego `k`, cuántos campos llevaba de ventaja la
  emulación, la hora de la PS3 (µs), la tarea que dibujó lo presentado y,
  para un intermedio, la tarea anterior del emparejado.
- **NOTE** — lo que otro módulo quiera dejar: una etiqueta de 4 letras y
  bytes opacos (`cajanegra_note`).

## Formato del archivo («S4BB», todo big-endian)

Cabecera de 64 bytes: magia `S4BB`, versión (1), tamaño de la cabecera,
banderas (bit 0: comprimido), campo al pedirlo, tareas, campos y notas,
CRC1/CRC2 y título del juego, tareas descartadas. Después, registros de 16
bytes (tipo, largo sin comprimir, banderas, id) y su contenido, hasta un
registro `END `. Con la bandera 1 del registro, el contenido son
`ceil(largo/64 KB)` bloques de `spukit/lz.h` (formato de bloque de LZ4) con su
encabezado de 4 bytes; las tareas y las notas de 4 KB o más van así, los
campos tal cual. Lo lee `tools/cajanegra.py` (descompresor propio en Python).

## Cómo se escribe

`SELECT + L2` (hilo de presentación) deja una petición; en la vuelta
siguiente se **congela** el anillo (ni las tareas ni los campos nuevos entran
mientras se escribe: la copia no cambia por debajo) y un trabajo de
`bgjob` (prioridad 1500) recorre los registros, comprime cada tarea en el SPE
libre (`spukit_lzs`, con respaldo en el PPE) y escribe a un `.tmp` que se
renombra al final. El aviso en pantalla (en los seis idiomas) y el log dicen
cuándo empieza y cuánto tardó en el disco y en el SPE. Si un estado se está
escribiendo, espera su turno (el hilo es uno solo).

## Costo (por medir en la PS3)

Registrar una tarea copia sus comandos, vértices y las texturas que usa (por
tarea, ~30 µs en gfxtest en RPCS3, que no mide el Cell); registrar un campo,
unos 2 µs. Todo en el hilo de presentación. El log al cerrar el juego dice el
tiempo medio y el peor por tarea y por campo. Memoria: el anillo (12 MB por
omisión), reservado al abrir el juego solo si hay archivo `cajanegra`
(docs/memory-budget.md).

## Para la rama de interpolación

La caja negra ya trae lo necesario para repetir un cuadro intermedio en el
host: la tarea nueva y su anterior (registros TASK vecinos, con `PREV` y
`ADJ`), los campos en que se presentó cada cosa y su t. Lo que la rama debe
llamar (en el hilo de presentación, antes de que `present_field` registre el
campo):

```c
cajanegra_field_interp(BB_KIND_INTERP /* o BB_KIND_REPEAT */, t, k, tarea_dibujada, tarea_anterior);
cajanegra_note(BB_FOURCC('V', 'M', 'A', 'T'), datos, largo);   /* decisiones de vmatch */
```

`cajanegra_field_interp` vale para un solo campo (el siguiente que se
registre); sin llamarla, el campo sale como verdadero con t = 0. Las notas son
opacas (la rama define su contenido; se sugiere un encabezado con versión, el
número de tarea y un registro por grupo con su motivo de rechazo, como el
volcado de grupos) y se guardan en el orden en que se llaman, intercaladas con
las tareas y los campos. Si el formato de las notas cambia, basta cambiar la
etiqueta o su versión: el lector ignora lo que no conoce.
