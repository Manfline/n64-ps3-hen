# Compatibilidad

Censo de los juegos objetivo (tabla de docs/proyecto.md) y algunos extra. Los
juegos se prueban con respaldos propios, fuera del repositorio; aquí solo hay
datos de compatibilidad.

Las entradas antiguas conservan sus métodos de prueba como historia.

## 2026-10-17 — Diddy Kong Racing con HLE (en el host; falta la PS3)

Microcódigo de Rare sin texto (F3DDKR, `ucode_size` 0; CRC-32 de sus 4 KB
`3BFF208D` en el respaldo NTSC-U rev. 1). Con la HLE de `src/rsp/gfx/f3ddkr.c`
el juego se ve y se juega en el host (`salsa64-run`, respaldo propio): logos,
intro con los personajes, título, selección de jugadores, «Game Select», elegir
pista y vehículo, «Get ready» y una carrera en Ancient Lake, y la isla de
Timber en el modo Adventure (`docs/pads/dkr-pista.pad` llega a la carrera
desde el arranque). 0 comandos sin implementar y 0 errores en 16 000 campos.

| Comparación | Resultado |
|---|---|
| ares (consola) contra la HLE, volcados de la RDRAM en cuadros coherentes | PSNR en 4×4 de 31 a 41 dB en los logos y la intro (0–10 % de píxeles a más de 8/255); 24 a 35 dB en el título y las demos de personajes en pista (6–48 %) |
| Microcódigo real (`--lle=all --lle-speed 8`) contra la HLE, mismo guion desde el arranque | 0,01–0,44 % de píxeles distintos tras tolerar 1 px en los campos 250–1750 (logos, intro, demos de personajes) y 0,5–2,5 % jugando en la isla de Timber (el HUD, que la LLE no dibuja, cuenta ~1 %); en otros tramos (título, cinemáticas) se separan por el movimiento y por los textos, que la LLE del host tampoco dibuja (la consola sí) |

Lo que queda: bordes de polígonos con antialiasing y detalle de las
texturas (el rasterizador del host; docs/referencia-ares.md), y en el título y
algunas demos el suelo sale ~9 % más claro que en la consola (240 contra 219
de media en una franja del título) sin que se sepa de dónde: la LLE da los
mismos colores que la HLE, así que no viene del microcódigo.
Variante **no probada** (sin respaldo): otras versiones de DKR (CRC de
GLideN64 `6E6FC893`). Jet Force Gemini y Mickey's Speedway USA (F3DJFG) tienen
su sección debajo.

## 2026-10-17 — Jet Force Gemini y Mickey's Speedway USA con HLE completa (en el host; falta la PS3)

El microcódigo de los dos (F3DJFG, CRC-32 de GLideN64 `BDE9D1FB`) es el de
Diddy Kong Racing con otro conteo de vértices y un comando más, G_DMA_TEX_OFFSET
(0x02), que la HLE no tenía: sin él daban 746 y 2548 comandos sin implementar en
los títulos y, en Mickey's Speedway, las luces del semáforo de la salida no
cambiaban nunca. Ahora quedan en **0 comandos sin implementar** y en el host:

| Juego | Hasta dónde llega (`salsa64-run`, respaldo propio) | Comandos sin implementar |
|---|---|---|
| Jet Force Gemini | título, «Select Game» con el teclado del nombre, intro de la partida nueva (el planeta, la nave, el puente de mando, una aldea en la selva: escenas en 3D) y selección de Juno (`docs/pads/jfg-3d.pad`, hasta el campo ~14 000) | 27 981 → 0 |
| Mickey's Speedway USA | título, cuarto de Mickey, personaje, «Motor Way Mania», la historieta, la parrilla y el principio de la carrera (`docs/pads/mickey-semaforo.pad`, campos ~5 600 a 12 000) | 3 242 → 0 |

Contra el microcódigo real (`--lle=all`, desde estados del juego, mismos
campos): los G_SETTIMG que manda al RDP coinciden uno por uno (26 029 de
Mickey's Speedway y 13 550 de Jet Force Gemini); los alfas de los vértices que
carga coinciden salvo ±1 en los de niebla (la HLE redondea). La imagen del VI
difiere 0,1–3 % en tres de las cuatro escenas probadas de Jet Force Gemini y
3,7–8,9 % en las de Mickey's Speedway, casi todo el HUD y los textos que la
LLE del host no dibuja; las luces del semáforo pasan de apagadas a iguales a
las del microcódigo real. La LLE del host tampoco aplica la niebla del
blender (`build_state` solo la pone en los triángulos de la HLE): en la
escena de la selva de Jet Force Gemini, con mucha niebla, las dos imágenes se
separan 45,6 % y con la niebla apagada en la HLE solo 1,0 %.
Variantes sin respaldo: otras versiones de Mickey's Speedway y de Jet Force
Gemini.

## 2026-10-17 — Killer Instinct Gold: dibuja con la HLE; la segunda pelea se congela en la PS3

| Juego | Antes | Ahora |
|---|---|---|
| Killer Instinct Gold (NKIE) | PS3: 0 tareas gráficas en todas las versiones (solo dibujaba con `lle` encendido); host: dibuja | RPCS3 sin `lle`: 1409 tareas gráficas en 1800 campos (logo y fichas de los personajes); host: la pelea completa, sin esperas |

El juego da al RDP listas escritas por el CPU (`DPC_START`/`DPC_END`, sin RSP:
el fondo del escenario y los marcos) y no manda tareas gráficas hasta recibir
la interrupción del DP de su `SYNC_FULL`. Esas listas las corre ahora el mismo
SPE que las tareas (con otro estado del RDP sobre la misma caché de texturas
los fondos salían rotos); detalle en [docs/estado.md](estado.md).

**Sin resolver (PS3 real):** con la primera versión del arreglo la primera
pelea va bien y al empezar la segunda (hacia 50 campos después de «READY») el
juego deja de leer los controles y de mandar tareas gráficas, con la música
sonando; con el RSP a bajo nivel en todas las tareas (en una versión de
prueba de su recompilador) pasa en la primera. No se reproduce en el host. La
versión siguiente avisa y vuelca la RDRAM (`cuelgue-<campo>.rdram` en
`/dev_hdd0/salsa64/`) para ver en qué espera cada hilo.

## 2026-10-16 — Audio de MusyX (Factor 5): HLE nueva, verificada en el host

Los juegos de MusyX corrían mudos: su tarea de audio no es una lista de
comandos y la HLE la daba por mala. Ahora hay HLE de MusyX v1 y v2
(`src/rsp/audio/musyx.c`, SPE 1 en la PS3). Cada tarea se comparó, byte a
byte, con el microcódigo real corriendo en el intérprete del RSP
(`salsa64-run --lle=all --lle-check-audio`); detalle y método en
[docs/estado.md](estado.md). **Falta oírlos en la PS3 real**: aquí solo hay
medición en el host.

| Juego | MusyX | Tareas de audio idénticas al microcódigo real (96 s de consola, con START y A) | Observaciones |
|---|---|---:|---|
| Hydro Thunder | v1 | 5740 de 5740 | Sin eco en la prueba. |
| Rush 2049 | v1 | 5728 de 5728 | Sin eco en la prueba. |
| NBA Showtime | v1 | 5740 de 5740 | Sin eco en la prueba. |
| Resident Evil 2 | v1 | 2856 de 2856 | Con eco (reverberación) en todas las subtramas. Solo el audio: sus gráficos no tienen HLE. |
| Star Wars: Rogue Squadron | v1 (otra compilación) | 5713 de 5713 | Con eco, con la envolvente del envío en rampa. Solo el audio: sus gráficos no tienen HLE. |
| Indiana Jones | v2 | 5675 de 5724 (las otras 49, +1 en 2 de cada 8 muestras del canal izquierdo) | Solo el audio: sus gráficos no tienen HLE. |

Sin respaldo para probar: Gauntlet Legends, Tarzan, The World Is Not Enough,
Rugrats in Paris, Polaris SnoCross y Battle for Naboo.

## 2026-10-16 — Candidata de la v0.2.7 en la PS3 real

PS3 con HEN, respaldos NTSC. La rc5 (v0.2.6-47-gd01e523, la que se publica
como v0.2.7) abrió estos juegos en una sola corrida; la velocidad es el rango
de las ventanas de 5 s del log (sin la primera y la última de cada juego) y
cuántas bajaron de 58.

| Juego | Estado | Velocidad | Notas |
|---|---|---:|---|
| Doom 64 | Jugable | ~60 campos/s; 2 de 98 ventanas bajo 58 (53,5 la menor) | Era ~40 % (las esperas de libultra por el planificador ahora se saltan). |
| NFL Blitz | Jugable | 59,2–60,2 campos/s | Era ~26 campos/s. |
| 1080 Snowboarding | Jugable | ~60 campos/s; 2 de 43 bajo 58 (51,8 la menor) | Con el Controller Pak; sin él se quedaba leyendo el control. |
| Mario Kart 64 | Jugable | ~60 campos/s; 6 de 130 bajo 58 (53,7 la menor) | Lee y escribe su `.mpk` (fantasmas). |
| Pokémon Stadium | Jugable | ~60 campos/s; 6 de 84 bajo 58 (54,6 la menor) | «Game Pak Check» completo (también al volver del título), «Please Select» sin fantasma, VS sin caerse. |
| Pokémon Snap | Jugable | ~60 campos/s; 2 de 13 bajo 58 (55,6 la menor) | Queda el orden de dibujo de la v0.2.6. |
| Wave Race 64 | Jugable | — | Variante F3DWR del microcódigo (probado con la rc4). |
| Star Wars Episode I: Racer | Jugable, lento | 43–47 campos/s en carrera | Medido con la app de perfilado de la misma ronda del CPU (era ~32). |

## 2026-10-15 — Censo en la PS3 real (v0.2.6)

Regresión de la versión candidata y comprobación de la rc1 en la PS3 real.
La rc1 abrió más de 15 juegos en varias corridas; la tabla recoge los títulos
con resultado o incidencia documentados. Sin una medición explícita no se
asigna velocidad.

| Juego | Estado | Velocidad | Notas |
|---|---|---:|---|
| Banjo-Tooie | Jugable | 59,7 campos/s de mediana en la sesión de perfilado | Prueba larga; escenas pesadas limitadas por el camino lento del recompilador. |
| F-Zero X | Jugable; carrera y «SELECT COURSE» superados | — | A 5×. Quedan objetos del fondo dibujados sobre la pista. |
| Pokémon Snap | Jugable | — | Orden de dibujo incorrecto: la correa de la mochila queda delante y la playa delante de las montañas. |
| Zelda: Ocarina of Time | Jugable | — | Se probaron salas con fondos prerrenderizados; el registro de la sesión informa 149–150 fondos S2DEX por ventana. |
| GoldenEye 007 | Jugable | — | Incluido en la regresión; sin medición de velocidad atribuible a esta prueba en el censo. |
| Perfect Dark | Jugable | — | Incluido en la regresión; sin medición de velocidad atribuible a esta prueba en el censo. |
| Kirby 64 | Jugable | — | Incluido en la regresión. |
| Mario Kart 64 | Jugable | — | Se verificó la interpolación. |
| Conker's Bad Fur Day | Abre después de otros juegos | — | Se comprobó la apertura de una ROM grande fuera del heap. |
| Super Mario Odyssey 64 | Inestable | — | Se registró una caída durante una prueba anterior de interpolación; queda pendiente una causa. |
| Star Fox 64 | Jugable, con fallo gráfico | — | Las opciones del menú se repiten. |
| Wave Race 64 | Fallo gráfico | — | Las texturas aparecen «como rayos desde el centro». |
| Yoshi's Story | No funciona | — | Fallo conocido. |
| FIFA 99 | La intro funciona; el partido no arranca | — | Fallo conocido. |
| Banjo-Kazooie | Jugable, con fallo gráfico | — | Las piezas del rompecabezas salen negras. |
| Diddy Kong Racing | Jugable en el host | — | HLE de F3DDKR desde el 2026-10-17 (sección de arriba); sin probar en la PS3. |
| Jet Force Gemini | Solo los títulos, con comandos sin implementar | Dibuja con todos sus comandos en el host | G_DMA_TEX_OFFSET (2026-10-17, sección de arriba); sin probar en la PS3. |
| Mickey's Speedway USA | Solo los títulos; el semáforo de la salida no cambia | Dibuja con todos sus comandos en el host; el semáforo cambia de luz | G_DMA_TEX_OFFSET (2026-10-17, sección de arriba); sin probar en la PS3. |
| Indiana Jones | No jugable | — | Sin HLE de su microcódigo gráfico. |
| Star Wars: Rogue Squadron | No jugable | — | Sin HLE de su microcódigo gráfico. |
| Resident Evil 2 | No jugable | — | Sin HLE de su microcódigo gráfico. |

Las pruebas de rendimiento detalladas de Banjo-Tooie corresponden a la sesión
de perfilado indicada en [docs/perf-log.md](perf-log.md); no representan una
mediana de la rc1.

## 2026-10-12 — Censo en la PS3 real (v0.2.0 a v0.2.5)

PS3 con HEN, respaldos NTSC, salida a 1080p con escala 5×. Este censo queda
como antecedente; el de 2026-10-15 es el más reciente.

| Juego | Estado | Notas |
|---|---|---|
| Zelda: Ocarina of Time | velocidad completa | sin tirón en las salas prerrenderizadas desde v0.2.1 |
| Zelda: Majora's Mask | velocidad completa | guardado en FlashRAM; la iluminación de algunas zonas salta entre fuentes de luz |
| Super Mario 64, Mario Kart 64 | velocidad completa | |
| Donkey Kong 64 | velocidad completa | cámara estable desde v0.2.1; tirones breves al entrar a zonas nuevas |
| Paper Mario, Mario Party, Mario Party 3 | velocidad completa | |
| Perfect Dark | velocidad completa en la intro y menús | 43–55 cuadros/s en las escenas más pesadas (CPU) |
| GoldenEye 007 | 45–60 cuadros/s | la presa es la zona más pesada; líneas azules finas en algunas uniones |
| Conker's Bad Fur Day | ~60 en juego | **v0.2.1 y v0.2.2: se queda en negro tras «For Mature»** (usar v0.2.0 o v0.2.3); desde v0.2.3 arranca sin los 25–30 s de negro; sombras cuadradas |
| Star Fox 64, Banjo-Kazooie | velocidad completa | Star Fox: ruido de colores arriba durante el logo «Nintendo presents» hasta v0.2.5 (el relleno de arranque llegaba antes que los framebuffers); corregido en el árbol, por confirmar en la PS3. Banjo: las pantallas del rompecabezas salen en negro |
| Kirby 64, Pokémon Stadium 2 | ~97–100 % | algunas texturas con ruido o en ajedrez |
| Pokémon Snap | jugable | texturas estiradas en algunos polígonos |
| Super Smash Bros. (y Smash Remix) | velocidad completa | desde v0.2.4 pasa los KO y el Castillo de Hyrule se ve bien (confirmado en la PS3 real); en v0.2.3 y anteriores se congelaba en el primer KO (`/dev_hdd0/salsa64/fbfx.cfg` con un `1` lo evitaba) |
| Doom 64 | ~40 % | limitado por el CPU |
| Banjo-Tooie | jugable en la PS3 (primera prueba) | desafío del CIC 6105 de la PIF; confirmado en la PS3 real el 2026-10-13 |
| F-Zero X | jugable en la PS3 (v0.2.6) | bus abierto del PI (sondeo del 64DD), el microcódigo que recarga a mitad de lista (F3DEX → F3DFLX) y los brillos de los autos por tabla; a 5× se colgaba en «SELECT COURSE» hasta la v0.2.6; quedan objetos del fondo pintados sobre la pista (sombra afín del RDP tras el recorte) |
| Diddy Kong Racing | pantalla negra | microcódigo propio de Rare; en desarrollo con el RSP a bajo nivel |
| Indiana Jones, Resident Evil 2 | no jugables | microcódigos de Factor 5 / Capcom |

Las versiones europeas (PAL) y japonesas no se han probado.

## 2026-10-10 (3) — microcódigos gráficos de Rare (Perfect Dark y Conker)

Código medido: rama `competent-haslett-3bad2a` (main 3270538 con la
rama de arranque y `osCicId`; luego main 53fb223 fusionado), mismo método: 3 minutos de consola
sin tocar el control.

| Juego | Gráficos (HLE) | A los 3 minutos |
|---|---|---|
| Perfect Dark | **F3DPD** (`src/rsp/gfx/f3dpd.c`), reconocido por el CRC-32 del código (c543d0a8, 0x1000 bytes): 6914 tareas, **0 comandos sin implementar**, 0 errores; 1 combiner aproximado (abajo) | Pantalla de identificación (alta resolución, 576×474), logo de Nintendo, cubo de N64 que se vuelve el logo de Rare, vuelo por la ciudad y Joanna en la azotea |
| Conker's Bad Fur Day | **F3DEXBG** (`src/rsp/gfx/f3dexbg.c`), por su texto: 4468 tareas, **0 comandos sin implementar**, 0 combiners aproximados, 0 errores | Pantalla legal, logo de Nintendo, Conker con la motosierra, logo de Rareware, la taberna y el menú «GAME 1» |

- **PS3 real** (paquete «Salsa64 Rare» con main ce8351d): Perfect Dark se
  ve y suena bien, al 100 % de NTSC salvo la intro en alta resolución.
  Conker arranca y suena, pero en el RSX el 3D sale quemado y parpadea (la
  HLE del SPE es correcta: su volcado dibujado en la Mac sale bien); falla
  de presentación con su destino CI de 8 bits y 64 de ancho a mitad de
  cuadro, pasada al trabajo de presentación.
- Con el control (`--pad`: START y luego A cada 5 s): Perfect Dark llega a
  «Enter Agent Name» (7782 tareas en 150 s) y Conker al comienzo del juego
  (6830 tareas en 260 s), los dos con 0 comandos sin implementar y 0
  errores.
- **Perfect Dark** es Fast3D salvo los vértices: G_VTX de 12 bytes con el
  color en una tabla aparte (G_VTXCOLORBASE, 0x07); G_TRI4 en 0xB1 como
  GoldenEye. Su intro es de alta resolución: el rasterizador del host
  (320×240) la ve completa con `EXTRA_CFLAGS="-DRASTER_W=640
  -DRASTER_H=480"`.
- **Conker** quedaba mudo de imagen porque su G_LOAD_UCODE (0xDD) no
  cambia de GBI: carga la iluminación avanzada y la lista sigue. La HLE de
  F3DEX2 terminaba la tarea ahí, sin llegar al G_RDPFULLSYNC, y el juego
  esperaba para siempre la interrupción del DP. El 0x14 era su G_TRI4
  (0x10–0x1F).
- Combiner aproximado de Perfect Dark: `lerp(ENV, TEXEL0, SHADE_ALPHA) ·
  SHADE` (2 ciclos), el mismo de GoldenEye (motor de Rare): hace falta un
  término S·Sa en la combinación del RSX (Fase 4). 40 905 dibujos en 3
  minutos.
- Conker, en la escena del logo de Rareware, muestra la boca abierta con el
  interior oscuro (su textura de 32×32: fondo blanco y la boca en el
  centro); parece correcto, pendiente de confirmar en la PS3.
- Efectos de framebuffer de estos juegos, fuera de esta tarea (Fase 4).
  Perfect Dark: la cámara espía y los monitores (imagen dibujada y leída
  como textura) y la lectura del z-buffer con rectángulos texturizados
  (GLideN64: `hack_rectDepthBufferCopyPD`, `hack_clearAloneDepthBuffer`,
  `texturedRectDepthBufferCopy`). Conker: el desenfoque de la pausa, la
  sombra de Conker, la copia del z-buffer y rectángulos que copian la
  imagen sobre sí misma (`hack_blurPauseScreen`,
  `hack_rectDepthBufferCopyCBFD`, `hack_fbTextureOffset`,
  `texturedRectCopyToItself`).

## 2026-10-10 (2) — main 3270538 (F3D integrado) + osCicId

Código medido: esta rama (arranque del 6105 y `osCicId`) con main 3270538
(audio en el SPE 1, bucles de espera mapeados, F3D 2.0D/2.0G y F3DEX
0.95/1.x, el DP se cierra tras un microcódigo sin HLE). Antes de integrar
F3D se midió con su rama fusionada aparte: los mismos números.

| Juego | Región / versión | CIC | Guardado | Gráficos (HLE) | Audio (HLE) | A los 3 minutos |
|---|---|---|---|---|---|---|
| Super Mario 64 | NTSC-U, NSME v0 | 6102 | EEPROM 4K | F3D 2.0D: 5252 tareas, 0 comandos sin implementar | ABI 1: 10757 tareas, 0 errores | **Título** («PRESS START» con la cara de Mario) |
| Zelda: Ocarina of Time | NTSC-U 1.2, CZLE v2 | 6105 | SRAM 256K | F3DZEX 2.06H + S2DEX 2.05: 3724 tareas | nead (OoT): 10494 tareas, 0 errores | Demos del título (escena 3D de bosque) |
| Mario Kart 64 | NTSC-U, NKTE v0 | 6102 | EEPROM 4K | F3DEX 0.95: 5101 tareas | nead (MK64): 10707 tareas, 0 errores | Intro (bandera de cuadros) |
| Star Fox 64 | NTSC-U, NFXE v0 | 6101 | EEPROM 4K | F3DEX 1.22 NoN: 5375 tareas | nead (SF64): 10654 tareas, 0 errores | Intro («We need your help, Star Fox!») |
| GoldenEye 007 | NTSC-U, NGEE v0 | 6102 | EEPROM 4K | F3D 2.0G (Rare): 10127 tareas | ABI 1 (GoldenEye): 5362 tareas, 0 errores | Créditos de la intro; VI de 440×326 |
| Perfect Dark | NTSC-U, NPDE v0 | 6105 | EEPROM 16K | **Sin HLE**: 6761 tareas con microcódigo desconocido (sin texto) | naudio con MP3: 5208 tareas, **todas con error** | Corre (VI 320×218 en el Expansion Pak), no dibuja ni suena |
| Banjo-Kazooie (extra) | NTSC-U, NBKE v0 | 6103 | EEPROM 4K | F3DEX 1.21: 4604 tareas | naudio (BK): 5365 tareas, **todas con error** | Demo de juego (Banjo en un nivel) |
| Conker's Bad Fur Day (extra) | NTSC-U, NFUE v0 | 6105 | EEPROM 16K | F3DEXBG.NoN fifo 2.08 (variante de F3DEX2): **1 tarea** a los 29,5 s, 2 comandos sin implementar (último 0x14), y no manda más | naudio (Conker): 5381 tareas, **todas con error** | VI 292×213; espera tras la primera tarea gráfica |

Todos corren 3 minutos sin excepciones raras (interrupciones, coprocesador
no usable por el cambio perezoso de la FPU y, en GoldenEye y Conker, fallos
de TLB porque paginan código desde la ROM). Bucles de espera: 62 % (Perfect
Dark) a 98 % (Conker); GoldenEye pasó de 0 % a 86 % con los bucles mapeados de
main.

Código fuera de KSEG0 (PC muestreado al final de cada bloque): 100 % en
GoldenEye, Perfect Dark y Conker (código en 0x7000xxxx, 0x1000xxxx y
0x1500xxxx por la TLB); 0 % en los demás. Con el recompilador: Perfect Dark
6289 de 6294 bloques en 10,7 s de consola (qemu, `--lockstep` sin
diferencias) y 6078 de 6083 en la PS3 real.

### Conker: `osCicId`

Sin este commit, Conker se quedaba a los 88 mil ciclos en su hilo ocioso
sin configurar el VI ni mandar tareas: su hilo ocioso crea el hilo principal
pero **solo lo arranca si `osCicId` (0x80000310) vale 6105**, y el arranque
HLE no escribía esa palabra. El IPL3 del 6105 deja ahí su número y el del
6103, el suyo (Banjo-Kazooie y Diddy Kong Racing comprueban 6103; Perfect
Dark lo comprueba al cerrar puertas; DK64 también lo espera). Ahora el
arranque escribe 6103 o 6105 según la variante (docs/cpu-vr4300.md).

Después corre su lógica y su audio, configura el VI y a los 29,5 s manda su
primera tarea gráfica (microcódigo «F3DEXBG.NoN fifo 2.08», ucode en
0xBEBB0 con ucode_size 0, data en 0xC0050). La HLE la toma como F3DEX2, deja
2 comandos sin implementar (el último 0x14) y el juego no manda otra en los
3 minutos: le toca al trabajo de F3D. Su audio usa naudio de Conker
(`ucode_data` 00000000 1ab0140c 1fa41340 f0000f00).

### Perfect Dark

- Antes del arranque del 6105: `b .` en 0x700017F0 a los 15 M ciclos,
  porque 0xA00002E8 ≠ 0xC86E2000 (la palabra la deja el IPL3 del 6105). 201
  bloques en la PS3.
- Ahora: monta 0x7F000000 en la TLB, configura el VI, manda su primera
  tarea gráfica en el ciclo 186 M (ucode en 0x5A0B0, data en 0x89AD0, flags
  2 = DP_WAIT, sin texto: variante de Rare que GLideN64 llama F3DPD) y audio
  desde el ciclo 585 M. Con el DP cerrado tras el microcódigo desconocido
  (rama de F3D) sigue corriendo y alterna búferes de 320×218 en el Expansion
  Pak.
- PS3 real (frontend «Salsa64 arranque», SLSA00078, con el recompilador,
  antes de la rama de F3D): pasa la comprobación; 59,8 campos/s, 93,7 MIPS,
  5,8 ms de emulación por campo, pantalla negra.
- Sus otras comprobaciones del «bootloader» (ROM 0xA5C, 0x454, 0xDC0, según
  `docs/piracychecks.md` del decomp) leen la ROM del cartucho (0xB0000A5C):
  no dependen del arranque. La de `osCicId` (al cerrar puertas) queda
  cubierta por este commit.
- El desafío del CIC 6105 por la PIF durante el juego no apareció en 3
  minutos.

### Identificación de los microcódigos

| Juego | Gráfico: texto en `ucode_data`; CRC-32 de ucode / data | Audio: `ucode_data` en 0x00, 0x10, 0x28, 0x30 |
|---|---|---|
| SM64 | «RSP SW Version: 2.0D, 04-01-96»; 11d4da6c / b0de6837 | 00000001 11181470 1e24138c f0000f00 (ABI 1) |
| OoT | «RSP Gfx ucode F3DZEX.NoN fifo 2.06H» y «RSP Gfx[Safe] S2DEX fifo 2.05»; 72fea817 / 56159df6 | 00000001 1f681230 1b341190 17841830 (nead de OoT/MM) |
| MK64 | «RSP Gfx ucode F3DEX 0.95»; cd02d1ad / 270ed8eb | 00000001 11181350 1a18126c 1a8c1adc (nead de Mario Kart) |
| SF64 | «RSP Gfx ucode F3DEX.NoN 1.22»; cc888e6c / 5b69be26 | 00000001 110412cc 1b2c120c 180418b0 (nead de Star Fox) |
| GoldenEye | «RSP SW Version: 2.0G, 09-30-96»; 9cba9d04 / 2a9b4b97 | 00000001 11181470 1dc8138c f0000f00 (ABI 1 de GoldenEye; ucode_size = 0) |
| Perfect Dark | sin texto; c543d0a8 / 49fd74ec | 00000000 1ae8143c 1fb01358 f0000f00 (naudio de Banjo-Tooie/JFG/PD) |
| Banjo-Kazooie | «RSP Gfx ucode F3DEX 1.21»; 9fc2ac6e / 85d611d3 | 10f013a0 00001280 00010010 00020004 (naudio de Banjo-Kazooie) |
| Conker | «RSP Gfx ucode F3DEXBG.NoN fifo 2.08»; ucode_size 0 / 1c5c25dc | 00000000 1ab0140c 1fa41340 f0000f00 (naudio de Conker) |
| Diddy Kong Racing | sin texto; ucode_size 0, 3bff208d (4 KB del código) / c432d0ea | 00000001 111814a4 1e3c1390 f0000f00 (ABI 1 de Blast Corps) |

Los nombres de las variantes de audio siguen la identificación de
mupen64plus-rsp-hle (por esas mismas palabras; solo consulta). Los CRC de
`ucode_data` del audio cambian entre tareas en algunos juegos: los datos
tienen partes que el microcódigo escribe; las cuatro palabras son estables.

### Lo que falta, por prioridad

| # | Qué | Desbloquea | Quién |
|---|---|---|---|
| 1 | HLE de naudio: la de Banjo-Kazooie, la de Conker y la de Perfect Dark (con MP3); hoy toda tarea da error | Sonido en los tres juegos de Rare (y Banjo-Tooie, JFG) | trabajo de audio (`src/rsp/audio`, SPE 1) |
| 2 | ~~HLE del microcódigo de Perfect Dark~~ hecho: F3DPD (sección 3) | Perfect Dark | trabajo de Rare |
| 3 | ~~F3DEXBG 2.08 de Conker~~ hecho: G_TRI4 en 0x10–0x1F y G_LOAD_UCODE que no corta la lista (sección 3) | Conker | trabajo de Rare |
| 4 | Probar con el control (pasar los títulos; `--pad` de la rama de F3D) y medir en la PS3 las escenas de docs/proyecto.md | Mediciones de la tabla de juegos | tras 1–3 |
| 5 | ~~Desafío del CIC 6105 por la PIF~~ hecho 2026-10-13 (`cic6105_challenge`); Banjo-Tooie lo pide al cargar datos | Banjo-Tooie (arreglado), Perfect Dark y Conker si lo piden | — |
| 6 | Zelda: en una captura de la demo el cielo aparece en la parte de abajo; revisar si es el encuadre o geometría que falta | Zelda | trabajo de F3D |

## 2026-10-10 (1) — primera pasada, sin F3D ni audio

Con el arranque del 6105 y antes de main f698afe: solo Zelda dibujaba. Los
demás mandaban una tarea gráfica con un microcódigo sin HLE y se quedaban
esperando la interrupción del DP, porque esa tarea terminaba con TASKDONE
pero sin G_RDPFULLSYNC. Un experimento local (disparar el DP tras esa tarea)
mostró que los seis seguían 3 minutos sin otro bloqueo; la rama de F3D hace
ahora eso mismo. Tareas del RSP en esos 3 minutos: SM64 10765 de audio,
OoT 3724 gráficas y 10494 de audio, MK64 10763, SF64 5384, GoldenEye 5362,
Perfect Dark 5203, Banjo-Kazooie 5365 (de audio, más la gráfica sin HLE).
