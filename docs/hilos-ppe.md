# Dos hilos en el PPE

El frontend de la PS3 (`src/frontend/main.c`) reparte el trabajo entre los
dos hilos de hardware del PPE. Decidido el 2026-10-10.

| Hilo | Prioridad | Qué hace |
|---|---|---|
| **Emulación** (`emu_thread`, creado con `sysThreadCreate`, pila de 256 KB) | 1000 | CPU de la N64 (`salsa64_n64_run_field`), planificador de eventos, PI/SI/AI/VI, arranque de tareas del RSP hacia el SPE 0 y espera de su fin, controles aplicados al PIF, copia de la imagen del VI, copia del guardado cuando toca volcarlo, medición A/B de vitest, texto del ISViewer |
| **Presentación** (el hilo principal del proceso, `SYS_PROCESS_PARAM`) | 1001 | Traducción de la salida de la HLE a comandos del RSX (`salsa64_rdp_flush`), `rsxkit_frame_begin/end` (flip en vsync), DualShock 3 (`ioPad*`), callbacks de sysutil, escritura del guardado en el disco, log de estadísticas, capturas, volcados de la HLE, verificación propia (selftest de vitest y gfxtest), salida. Más adelante: audio y menú |

Los dos hilos del PPE comparten el núcleo (SMT): lo que hace el hilo de
presentación no sale gratis, le quita ciclos a la emulación. Por eso el
hilo de presentación **duerme** cuando no tiene nada que hacer
(semáforo con tiempo de espera; nunca una espera activa) y las esperas
de rsxkit (flip, barreras) son con `sysUsleep`.

Numeración de docs/proyecto.md: «hilo 0» es el de emulación y «hilo 1» el de
presentación; cuál de los dos es el hilo principal del proceso da igual.
El principal queda como presentación porque ahí viven rsxkit, ioPad y
sysutil desde el arranque.

## Qué cruza entre los hilos

Todo preasignado al arrancar; sin locks por comando: un lote por campo.
Un solo productor y un solo consumidor por cola; las barreras son
`lwsync` (como en `lib/spukit`).

| Dato | Mecanismo | Sentido |
|---|---|---|
| Salida de una tarea gráfica (vértices, comandos, texturas) | Los 3 juegos de búferes de `src/rdp` mapeados al RSX, en ronda. `acquire` (hilo de emulación) toma el siguiente; `done` deja su `gfx_result` en una cola de 3 entradas; `salsa64_rdp_flush` (presentación) traduce en orden y pone una barrera del RSX por juego. Un juego vuelve a estar libre cuando se tradujo **y** el RSX pasó su barrera (`rsxkit_fence_done`, que solo lee la etiqueta y por eso puede llamarse desde el otro hilo) | emulación → presentación |
| Campo terminado | Cola de 8 mensajes (`struct field_msg`): número de campo, tareas gráficas terminadas hasta ahí (para que el flush no traduzca tareas posteriores al campo), descripción del VI, ranura de la copia, banderas (verificar pantalla) | emulación → presentación |
| Imagen del VI (cuando la dibujó el CPU en la RDRAM) | **Copia** al terminar el campo, en una de 3 ranuras de 640×576×4 (4,4 MB) con estados LIBRE → ESCRIBIENDO → EN COLA → EN USO → LIBRE (`lwarx/stwcx.` para disputar EN COLA entre los dos hilos). Se copia siempre, también cuando el VI muestra un destino del RSX: cuesta ~50 µs por campo (320×240×2) y evita que el hilo de emulación tenga que saber qué destinos existen | emulación → presentación |
| Controles | Buzón con seqlock: el hilo de presentación lee los DS3 y publica `struct salsa64_pad[4]` + «salir»; el de emulación los aplica al PIF al empezar cada campo | presentación → emulación |
| Guardado del cartucho | `savefile_stage` (emulación): cuando `salsa64_save_due` dice que toca, copia los 32 KB a una zona de paso y marca `pending`; `savefile_write_staged` (presentación) los escribe al disco y limpia `pending`. Mientras está pendiente no se vuelve a copiar; la escritura nunca toca los datos vivos | emulación → presentación |
| Resultado del selftest / petición de medición | Banderas de un `int` (monótonas): `selftest.c` las pone desde los dos hilos (ISViewer y RDRAM desde emulación; pantalla desde presentación) y `selftest_state` las lee en presentación. `bench_request` de presentación a emulación | ambos |
| Estadísticas | Contadores de cada hilo en su propia estructura; el de presentación los lee cada 5 s sin lock (lecturas de 64 bits alineadas: atómicas en el PPE) | emulación → presentación |
| Salida | `running` (presentación lo pone en 0: START + SELECT, «Salir del juego», fin de la prueba automática); `emu_done` (emulación lo pone al terminar su bucle) | ambos |
| Log | `salsa64_log` con un lwmutex alrededor de la línea (`src/platform/ps3/log.c`) | ambos |

## Ritmo: la emulación al paso de la tele emulada

Con un solo hilo, el flip en vsync frenaba a la emulación; ahora no la
frena nada, y con el recompilador (Fase 3) los juegos ligeros irían más
rápido que la consola real. `src/frontend/pacer.c` (pieza aparte,
portable, con pruebas en `tests/unit/test_pacer.c`) pone el ritmo en el
hilo de emulación:

- La referencia es la hora a la que debe empezar el próximo campo. Antes
  de emular un campo, si la hora no llegó, el hilo duerme hasta ella
  (`sysUsleep`; nunca espera activa); si ya pasó, no espera.
- Después de cada campo la referencia avanza lo que duró el campo en
  ciclos del VR4300 (32/3 ns por ciclo, sin perder el resto): el ritmo
  medio es exacto (NTSC 59,83 campos/s, PAL 49,92; campos de 262 y 263
  líneas con su duración propia) aunque cada espera tenga jitter.
- **Sin deuda:** si el atraso pasa de `PACER_MAX_BEHIND_NS` (~4 campos),
  la referencia se reinicia a ahora. Un tirón no se «recupera» corriendo
  rápido después; cada reinicio se cuenta (`reinicios` en el log).
- La presentación sigue saltando campos como antes (si alguna vez la
  emulación llega adelantada a la cola, se muestra el más nuevo).
- `SALSA64_DATA_DIR/turbo` presente: sin límite (para medir el máximo).
  La medición A/B de vitest corre siempre sin límite.

Punto de enganche para el audio: la función de dormir es de quien llama
(se puede cambiar por una espera al llenado del búfer de audio), y
`pacer_nudge` mueve la referencia unos ns para un control fino (corregir
la deriva entre el reloj y el audio, o ajustar el remuestreo). Si el
audio prefiere marcar el ritmo él solo, desactiva el pacer.

## Si un hilo va más rápido que el otro

- **Emulación más rápida que la tele** (futuro recompilador, o ya con la
  detección de bucles de espera en escenas vacías): el flip en vsync
  frena solo al hilo de presentación. Si al encolar un campo hay otro sin
  consumir, el viejo se marca `skip` y su ranura se libera (presentación
  lo descarta sin dibujar): se muestra siempre el más nuevo, con un campo
  de retraso como máximo. Si la cola de campos está llena (8), el campo se
  descarta (`campos saltados` en el log). **La emulación nunca espera al
  vsync.**
- **Presentación más lenta en una ráfaga** (traducción de una tarea grande,
  captura, volcado del guardado): la cola absorbe hasta 8 campos. La
  única espera posible del hilo de emulación es por un juego de búferes de
  la HLE: los 3 están en vuelo (encolados, sin traducir o sin pasar su
  barrera). Se cuenta (`esperas al RSX`) y se mide; con 3 juegos y tareas
  de ~1 por campo no debería ocurrir.
- **Mensajes con verificación** (selftest de vitest, gfxtest) no se
  saltan: la prueba necesita ver esa pantalla.

## Lo que no cambia

- La espera del hilo de emulación al SPE al terminar cada tarea
  (`spu_finish`, en el evento del planificador) sigue ahí: el CPU emulado
  no debe reusar la display list antes de que el SPE acabe de leerla. Con
  Zelda son 13–19 ms por tarea (docs/perf-log.md). Quitarla pide cambiar
  el evento del RSP (`src/core/rsp.c`: volver a programarse si el SPE no
  terminó, en vez de bloquear), que es terreno de la Fase 3.
- Verificación propia del frontend, capturas `screen-N.ppm`, volcado
  (`gfxdump`) y repetición (`dump:<nombre>`) funcionan igual; la
  repetición es de un solo hilo y llama a `salsa64_rdp_flush` ella misma.
- `lib/rsxkit`: sigue siendo de un hilo. Lo único que se documenta como
  seguro desde otro hilo es `rsxkit_fence_done` (lee una etiqueta del RSX).

## Lugar para el audio

`src/frontend/audio_out.c` (trabajo de audio) tendrá tres llamadas en
`main.c`, marcadas con `audio_out:` en el código: al arrancar (después de
`spukit_init`), en cada campo en el hilo de presentación (en
`present_field`, antes del flip) y al cerrar (antes de
`salsa64_rsp_set_gfx(NULL)`). Si necesita algo del hilo de emulación
(por ejemplo, el fin de cada tarea de audio), el sitio es `emu_field`,
después de `salsa64_n64_run_field`.

## Memoria nueva

| Uso | Tamaño |
|---|---|
| Ranuras de la copia del VI (3 × 640 × 576 × 4) | 4,4 MB |
| Zona de paso del guardado | 32 KB |
| Pila del hilo de emulación | 256 KB |
| Colas (campos, tareas) y buzones | < 2 KB |

Registrado en docs/memory-budget.md.

## Mediciones

En docs/perf-log.md, «Dos hilos en el PPE»: vitest y Zelda en las mismas
escenas, antes y después, en la PS3 real.
