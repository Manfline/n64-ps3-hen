# Estados guardados (save states)

Toda la máquina en un archivo, para seguir exactamente desde ahí. Diez
ranuras por juego en la PS3; en el host, `salsa64-run --state-save` y
`--state-load`. Decidido y hecho el 2026-10-11.

| Pieza | Dónde |
|---|---|
| Formato, guardar y cargar (portable) | `src/core/state.h`, `src/core/state.c` |
| Recoger la tarea del RSP en vuelo | `salsa64_rsp_quiesce` (`src/core/rsp.c`) |
| Estado de la HLE gráfica entre tareas | `src/rsp/gfx/state.c` (registros del RDP) y `sync.c` (con la TMEM) |
| Estado de la HLE de audio entre tareas | `src/rsp/audio/state.c` |
| En los SPE | trabajos `GFX_JOB_STATE_*` (el SPE copia tal cual el tramo del RDP; el PPE arma el formato, `src/platform/ps3/gfx_spu.c`) y `AUD_JOB_STATE_*` (`src/spu/gfx/main.c`, `src/spu/audio/main.c`) |
| Archivos de las ranuras y miniatura (PS3) | `src/frontend/states.c` |
| Barrera entre los dos hilos, atajos, aviso | `src/frontend/main.c` (`emu_park`, `state_barrier`, `pad_combos`), `src/frontend/osd.c` |
| Host | `src/host/run.c` (`--state-save CAMPO:ARCHIVO`, `--state-load ARCHIVO`, `--fields N`) |
| Pruebas | `tests/unit/test_state.c`, `tools/test-roms.sh` (`estados`), `tools/test-rec.sh` |

## Qué se guarda

Lo que hace falta para continuar ciclo a ciclo:

- **CPU0** — VR4300: GPR, HI/LO, ciclos, PC, siguiente PC, retardo (delay
  slot y salto), LLbit, todo COP0 (con Count/Compare como desplazamiento de
  Count y el ciclo de referencia de Random), la TLB de 32 entradas, los 32
  registros de la FPU, FCR31, el latch de COP2 y los ciclos saltados en
  bucles de espera (el RSP los compara entre sondeos).
- **SCHD** — el tiempo de cada evento del planificador (Compare, VI,
  fin de tarea del RSP, DP, AI). Los números de `enum salsa64_event` quedan
  fijos: uno nuevo va al final.
- **RCP0** — MI, registros del SP y su PC, la tarea del RSP en curso (tipo,
  microcódigo, ABI, si ya se recogió y con qué resultado), DP, VI (registros,
  reloj, línea, campo, campos, inicio y duración del campo), AI (registros,
  cola de dos DMA, acarreo, lo que falta del DMA en curso), PI, RI, SI y los
  registros de la RDRAM.
- **SPME** — DMEM e IMEM (8 KB).
- **PIF0** — los controles como los vio el PIF, las pulsaciones retenidas,
  los intercambios preparados y la PIF RAM.
- **CART** — el guardado del cartucho en memoria (EEPROM o SRAM).
- **HLEG** — lo que la HLE gráfica conserva entre tareas, como el RDP de la
  consola: othermode, combiner, colores, scissor, color/z/texture image,
  las 8 tiles y la TMEM (4 KB).
- **HLEA** — lo que la HLE de audio conserva entre tareas (como
  mupen64plus-rsp-hle): su DMEM con el margen, el estado de ABI1, nead y
  naudio (libro de ADPCM, envolventes, volúmenes) y la ventana de síntesis
  del MP3.
- **THMB** — miniatura de 160 × 120 (RGBA5551) de la imagen presentada, para
  un futuro menú de estados; va primero, el menú no tiene que leer el resto.
- **RDRM** — la RDRAM entera (4 u 8 MB), sin comprimir, al final.

No se guarda lo que no cambia el resultado o se recalcula: punteros y
backends, configuración (duraciones de las tareas del RSP), estadísticas,
las tablas de bucles de espera descartados (solo velocidad), modo y
permisos del CPU (salen de Status) y la caché del recompilador (se vacía al
cargar).

## Formato

Big-endian y campo por campo, igual en la PS3 y en el host: un estado de la
consola se reproduce en `salsa64-run` y al revés.

- Cabecera de 64 bytes: magia `SLS64ST\0`, versión del formato, tamaño de la
  cabecera y del archivo, CRC1 y CRC2 de la cabecera de la ROM, tamaño de la
  ROM, título (20 bytes), tamaño de la RDRAM, campo del VI y banderas.
- Secciones «etiqueta (4 bytes) + largo (4 bytes) + datos».
- Cada sección se describe con una sola función para los dos sentidos
  (`sec_cpu`, `sec_rcp`…). Una sección más corta que lo que espera la
  versión que carga deja el resto en cero: así se agregan campos al final
  sin romper los estados viejos. Las etiquetas desconocidas se saltan.
- Un cambio incompatible sube `SALSA64_STATE_VERSION`. Los estados con una
  versión anterior a `SALSA64_STATE_MIN_VERSION` o posterior a la del
  programa se rechazan con un mensaje claro; también los de otro juego (CRC)
  y los de otro tamaño de RDRAM.
- Al cargar, la cabecera se valida antes de tocar nada, las secciones chicas
  se leen a un búfer de paso de 128 KB y se aplican solo después de leer la
  RDRAM entera. Si la lectura falla a mitad de la RDRAM la máquina queda
  inservible: en la PS3 se avisa y se cierra el juego.
- Los estados de las HLE son bloques del formato de cada backend (`state_size`
  y `save_state`/`load_state` en `salsa64_gfx_backend` y
  `salsa64_audio_backend`); si el tamaño no coincide, la HLE sigue con lo que
  tenía (casi siempre da igual: los juegos ponen el estado del RDP en cada
  cuadro).

Sin compresión: 8 MB de RDRAM se escriben de y hacia su búfer sin copias,
y comprimir costaría tiempo del PPE. Tamaño en disco: ~8,1 MB por ranura
(RDRAM de 8 MB más ~70 KB de secciones y miniatura), ~81 MB por juego con
las diez ranuras llenas (docs/memory-budget.md).

## Cuándo es seguro: la barrera

Solo en el límite de un campo y con la emulación quieta:

1. Presentación (SELECT + R1 o L1) deja la petición (`state_req`).
2. Emulación, al terminar el campo, recoge la tarea del RSP en vuelo
   (`salsa64_rsp_quiesce`: espera al SPE gráfico o de audio y guarda el
   resultado), encola el campo marcado (`FIELD_STATE`) y se queda esperando
   (`emu_park`, dormida).
3. Presentación presenta lo que quedaba en la cola (del campo marcado sale
   la miniatura, antes de dibujar el aviso), traduce todas las tareas
   gráficas, espera al RSX (`rsxkit_finish`) y guarda o carga.
4. Al cargar: la caché del recompilador se vacía (`salsa64_rec_flush`, dentro
   de `salsa64_state_load`), los destinos del RSX se sueltan
   (`salsa64_rdp_drop_targets`: lo dibujado ya no es de esta máquina y el
   número de campo retrocede; las colas y la numeración de las tareas siguen,
   porque el SPE es el mismo), la cola de audio se vacía con su transición
   (`audio_out_flush`) y se olvidan el último campo encolado y las ventanas
   que cuentan campos. El VI se relee solo en el campo siguiente.
5. Presentación baja la petición; emulación sigue y reinicia el ritmo.

**La tarea recogida no adelanta su fin.** La tarea del RSP «dura» un tiempo
emulado (rsp.c); recogerla antes solo adelanta el trabajo del backend: su
fin llega a la misma hora con el resultado guardado (`collected`), sin
volver a llamar al backend. Por eso el estado se puede guardar con una
tarea a medias y la continuación es idéntica.

Funciona con el intérprete y con el recompilador y con los dos hilos del
PPE. Con el RSP a bajo nivel (`lle`) no hay estados: el microcódigo corre en
un SPE sin forma de pararlo en un punto; se avisa.

## Determinismo: la prueba

De corrido hasta el campo M contra guardar en N, cargar en otra corrida y
seguir hasta M: los dos estados en M iguales byte por byte (CPU, RDRAM, RCP,
planificador, HLE) y las mismas imágenes (VI y HLE gráfica).

- `tests/unit/test_state.c`: un programa de prueba con la máquina de las
  unitarias (también con el recompilador en `tools/test-rec.sh`).
- `tools/test-roms.sh`: gfxtest (150 → 300, con la imagen de la HLE),
  vitest (120 → 240) y audiotest (100 → 200).
- `tools/test-rec.sh`: gfxtest con `--rec`, comparado además con el
  intérprete; con `--lockstep`, también la corrida que arranca de un estado.
- Con respaldos propios (fuera del repositorio, 2026-10-11): Zelda OoT
  (intro 1500 → 1800 y Kokiri 3400 → 3600), Super Mario 64 (900 → 1200 y el
  castillo 4100 → 4300), GoldenEye (1200 → 1500 y la presa, código mapeado
  por la TLB, 7200 → 7500) y Mario Kart 64 (900 → 1200): todo idéntico.

En la PS3 la emulación no es determinista ciclo a ciclo (los SPE terminan
cuando terminan y el fin de tarea se sondea), así que allí se prueba que
guardar y cargar funcione, no que dos corridas coincidan.

## Lo que se ve al cargar

- La imagen del primer campo puede salir negra: los destinos del RSX se
  soltaron y la RDRAM no tiene lo que dibujó la HLE; el campo siguiente ya
  lo dibuja el juego.
- El guardado del cartucho vuelve al del momento del estado y se escribe al
  disco con el ritmo de siempre (es lo que el juego cree que tiene; si no,
  podría corromperlo).
- El sonido que esperaba en cola se desvanece y sigue lo del estado.
