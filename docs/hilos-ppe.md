# Dos hilos en el PPE

El frontend de la PS3 (`src/frontend/main.c`) reparte el trabajo entre los
dos hilos de hardware del PPE. Decidido el 2026-10-10.

| Hilo | Prioridad | Qué hace |
|---|---|---|
| **Emulación** (`emu_thread`, creado con `sysThreadCreate`, pila de 256 KB) | 1000 | CPU de la N64 (`salsa64_n64_run_field`), planificador de eventos, PI/SI/AI/VI, arranque de tareas del RSP hacia el SPE 0 y espera de su fin, controles aplicados al PIF, copia de la imagen del VI, copia del guardado cuando toca volcarlo, medición A/B de vitest, texto del ISViewer |
| **Presentación** (el hilo principal del proceso, `SYS_PROCESS_PARAM`) | 1001 | Traducción de la salida de la HLE a comandos del RSX (`salsa64_rdp_flush`), `rsxkit_frame_begin/end` (flip en vsync), DualShock 3 (`ioPad*`), callbacks de sysutil, escritura del guardado en el disco, log de estadísticas, capturas, volcados de la HLE, verificación propia (selftest de vitest y gfxtest), salida y el menú para elegir ROM (entre juegos, sin hilo de emulación) |

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
| Imagen del VI (cuando la dibujó el CPU en la RDRAM) | **Copia** al terminar el campo, en una de 3 ranuras de 640×576×4 (4,4 MB) con estados LIBRE → ESCRIBIENDO → EN COLA → EN USO → LIBRE (`lwarx/stwcx.` para disputar EN COLA entre los dos hilos). Se copia siempre, también cuando el VI muestra un destino del RSX: cuesta 0,20 ms por campo (320×240×2) y 0,30 ms (320×240×4) en la PS3, y evita que el hilo de emulación tenga que saber qué destinos existen (pendiente: una pista del hilo de presentación para no copiar) | emulación → presentación |
| Controles | Buzón con seqlock: el hilo de presentación lee los DS3 una vez por vuelta (cada vuelta presenta solo lo encolado al entrar, así que lee al menos una vez por flip: 9–38 ms de hueco máximo en la PS3) y publica `struct salsa64_pad[4]` + «salir»; además hace OR atómico de los botones **recién pulsados** en `pressed[4]`, que emulación toma y pone en 0 (`__sync_fetch_and_and`), así una pulsación publicada y soltada entre dos campos no se pierde. Emulación los aplica al PIF justo antes de emular cada campo (después de la espera del ritmo), y el PIF retiene cada flanco de pulsación hasta entregarlo en una respuesta de estado (`held`, `src/core/pif.c`) | presentación → emulación |
| Flancos del PIF (log) | Cola de 32 entradas: al terminar cada campo, emulación anota cada cambio de botones en lo que el PIF entregó al juego (`pif.sent`, `pif.reads`); presentación los escribe como «PIF → juego» junto al «control N: … (campo M)» de `update_pads` | emulación → presentación |
| Guardado del cartucho | `savefile_stage` (emulación): cuando `salsa64_save_due` dice que toca, copia los 32 KB a una zona de paso y marca `pending`; `savefile_write_staged` (presentación) los escribe al disco y limpia `pending`. Mientras está pendiente no se vuelve a copiar; la escritura nunca toca los datos vivos | emulación → presentación |
| Resultado del selftest / petición de medición | Banderas de un `int` (monótonas): `selftest.c` las pone desde los dos hilos (ISViewer y RDRAM desde emulación; pantalla desde presentación) y `selftest_state` las lee en presentación. `bench_request` de presentación a emulación | ambos |
| Estado guardado | `state_req` (presentación pide guardar o cargar; la baja al terminar) y `state_parked` (emulación quieta en el límite del campo). Mientras emulación espera, presentación toca la máquina sola (sección siguiente) | ambos |
| Contador de velocidad | Presentación lee `vi.fields` y `cpu.cycles` (32 y 64 bits alineados) y cuenta los cuadros que presenta y los cambios del framebuffer del VI de cada campo encolado; nada en el hilo de emulación | emulación → presentación |
| Estadísticas | Contadores de cada hilo en su propia estructura; el de presentación los lee cada 5 s sin lock (lecturas de 64 bits alineadas: atómicas en el PPE) | emulación → presentación |
| Salida | `running` (presentación lo pone en 0: START + SELECT, «Salir del juego», fin de la prueba automática); `emu_done` (emulación lo pone al terminar su bucle); `app_quit` (el callback de sysutil: «Salir del juego» cierra la aplicación aunque haya menú) | ambos |
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
- **Sin esperas largas:** si la referencia queda adelantada más de
  `PACER_MAX_AHEAD_NS` (~4 campos) también se reinicia; nunca se duerme
  más de eso de una vez. (La primera corrida en la PS3 durmió 70 s: la
  medición A/B avanzaba la referencia sin esperar.)
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
- **Capturas `screen-N.ppm`**: leer el búfer del RSX desde el PPE y
  escribir el PPM frena la presentación ~400 ms (medido en la PS3): se
  descartan 4 campos, el hilo de emulación espera ~250 ms por un juego
  de búferes y el ritmo se reinicia. Por eso las capturas periódicas son
  opcionales (`SALSA64_DATA_DIR/capturas`); en prueba automática van
  siempre.

## Fin de tarea sin bloquear (sondeo)

El CPU emulado no debe reusar la display list antes de que el SPE acabe
de leerla, así que TASKDONE no puede llegar antes de que el SPE termine.
Antes, el evento de fin de tarea esperaba al SPE (`spu_finish`): 10–27 ms
por tarea con Zelda, 20–53 % del hilo de emulación. Ahora (`src/core/rsp.c`):

- El backend tiene `done(ctx, idle)` (no bloquea). Al llegar el fin de la
  tarea (`gfx_cycles` después del arranque), si no terminó, el evento se
  vuelve a programar `poll_cycles` (10 000 ciclos, ~0,11 ms) más tarde y
  el CPU sigue emulando: el trabajo del juego entre tareas (lógica,
  audio, la display list siguiente) corre en paralelo con el SPE.
- Mientras tanto el RSP no está HALT; quitar HALT no arranca otra tarea
  (`starts_busy`), como en el hardware; un yield se ignora y la tarea
  termina con TASKDONE, que libultra acepta como fin.
- **Reloj emulado contra reloj real:** si el CPU está en un bucle de
  espera, la detección salta directo al sondeo siguiente, y sin más cada
  sondeo tardaría ~1 µs real por 0,11 ms emulados: la N64 correría 100×
  más rápido que el reloj mientras el SPE trabaja (campos del VI de más,
  interrupciones de más). Por eso el núcleo le dice al backend si el CPU
  saltó ciclos desde el sondeo anterior (`idle`), y el backend del SPE
  duerme entonces 100 µs antes de contestar (`sysUsleep`, nunca espera
  activa: el PPE es SMT). Si el CPU trabaja de verdad, no duerme.
- Es genérico: el backend de audio (SPE 1) puede dar su `done` y el
  mismo evento lo sondea.
- `finish` sigue recogiendo el resultado (y bloquea si un backend no
  tiene `done`, como el síncrono del host).

## Volver al menú: cerrar un juego y abrir otro

Sin `embedded`, START + SELECT termina la sesión de juego (`run_game`) y
vuelve al menú en vez de salir. Cada sesión arranca y se cierra completa,
en el hilo de presentación y con el de emulación ya terminado
(`game_close`, cada paso al log con el prefijo «cierre:»):

1. `running = 0`; se drenan las colas hasta que emulación pone `emu_done`
   (no la deja esperando un juego de búferes) y se hace `sysThreadJoin`.
2. `savefile_close`: lo pendiente al disco y el guardado desconectado.
3. `audio_out_shutdown`: sumidero del AI, HLE de audio (espera la tarea en
   vuelo y para el SPE), hilo y puerto de audio; cada resultado al log.
4. `salsa64_rsp_set_gfx(NULL)` (espera la tarea gráfica en vuelo; su fin
   llama a `wake()`, por eso el semáforo vive todo el programa), el LLE y
   su SPE, y el SPE de la HLE gráfica.
5. `salsa64_rec_detach`: el siguiente `salsa64_rec_init` empieza con la
   caché vacía.
6. Último `drain` (sin presentar) y `salsa64_rdp_reset`: destinos y
   profundidades del RSX liberados, colas de tareas y estadísticas en
   cero; la memoria mapeada se conserva (no se puede devolver).
7. Comprobación: hilo terminado, cola de campos vacía, ranuras del VI
   libres, ninguna tarea sin traducir, SPEs de la HLE detenidos y el RSP
   sin backends → «cierre: estado limpio» (o «ATENCIÓN, quedó algo en
   vuelo» con lo que sea).

`game_reset` pone en cero lo demás antes del siguiente juego: índices de las
colas (incluido el del último campo encolado, que antes era un estático de
`post_field`), ranuras, contadores y ventanas de estadísticas, banderas de
las pruebas, capturas, ritmo, la línea a medias del ISViewer. La RDRAM se
borra antes de `salsa64_n64_init`: un juego arranca igual venga después del
que venga. Lo que dura todo el programa: rsxkit, el RDP (su memoria
mapeada), el framebuffer 2D, las ranuras del VI, la RDRAM, el semáforo, los
DualShock y el callback de sysutil.

El menú corre en el hilo principal sin hilo de emulación (no le quita
nada a la emulación: no existe mientras se ve el menú): lee los mandos,
arma unos miles de vértices (columna, panel, texto, ondas; `menu_frame`)
que dibuja el RSX (`rsxkit/ui.h`) y presenta un cuadro por vsync
(`rsxkit_frame_end` lo frena). La prueba automática (`tools/rpcs3-run.py
--menu`) abre varias ROMs de prueba seguidas y cada una tiene que pasar su
verificación después del cierre de la anterior.

## Estados guardados: la barrera

Guardar o cargar un estado (docs/estados.md) necesita la máquina quieta y
las colas vacías:

1. SELECT + R1 o L1 (presentación, `pad_combos`) pone `state_req`.
2. Emulación, al terminar el campo y antes de encolarlo, recoge la tarea del
   RSP en vuelo (`salsa64_rsp_quiesce`: espera al SPE, su fin emulado no se
   adelanta), encola el campo con `FIELD_STATE` (no se salta) y duerme en
   `emu_park` (`sysUsleep` de 1 ms) hasta que `state_req` baje.
3. Presentación ve `state_parked`: presenta lo encolado (del campo marcado
   sale la miniatura), traduce las tareas, `rsxkit_finish` y guarda o carga
   (`state_barrier`); con emulación quieta usa también los SPE de la HLE
   (trabajos de estado) y la caché del recompilador.
4. Al cargar suelta los destinos del RSX y vacía la cola de audio; baja
   `state_req` y emulación sigue con el ritmo reiniciado.

Si se sale del juego mientras emulación espera, `running = 0` la suelta.

## Lo que no cambia

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
