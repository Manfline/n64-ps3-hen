# Registro de rendimiento

Cada optimización lleva medición antes/después **en PS3 real** (RPCS3 no
reproduce los tiempos del Cell). Formato: fecha, commit, prueba, entorno,
antes, después, notas.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| 2026-10-10 | 419731a → c14268c | Intérprete, `bench` mixto | PS3 real: Slim CECH-3011A, HFW 4.91 + HEN | 8,7 MIPS | 17,1 MIPS (×2,0) | Estado del bucle en registros, sin llamadas por instrucción, r0 sin escribir, tablas de `switch` sin `lwax`, FPU rápida sin pasar valores por memoria. Desglose en «Líneas base de la Fase 1» |
| 2026-10-10 | c14268c → 147d5ac | Intérprete, `bench` mixto | PS3 real (mismo equipo) | 17,1 MIPS | 21,2 MIPS (×1,24; ×2,4 desde 8,7) | Un solo despacho (SPECIAL en la misma tabla), FPU rápida sin comparaciones de punto flotante para clasificar, movimientos de COP1 en el bucle |
| 2026-10-10 | 940f6f8 | Frontend con vitest, emulación por campo | PS3 real (mismo equipo) | 65,6 ms (15,2 campos/s) | 35,0 ms (28,3 campos/s; ×1,9) | Detección de bucles de espera (`src/cpu/idle.c`): 45,6 % del tiempo saltado. Medición A/B automática del frontend, 10 s cada una, dos corridas iguales (65,56/65,66 → 34,97/34,84 ms). Lo que queda es el dibujo de vitest con el CPU |
| 2026-10-10 | 147d5ac → 940f6f8 | Intérprete, `bench` mixto | PS3 real (mismo equipo) | 21,2 MIPS | 21,0 MIPS (−1 %) | Costo del chequeo de bucle de espera en cada salto tomado. Ver «Bucles de espera» abajo |
| 2026-10-10 | f7b655d | **Recompilador** (Fase 3), nemu64-test completo | PS3 real (mismo equipo) | intérprete: 7921 ms (23,9 MIPS) | **1817 ms (104,2 MIPS; ×4,36)** | Mismas 462 fallas, mismo hash de nombres y mismo ciclo final (189 431 098). 28 694 bloques, 9,2 MB de caché, 139 440 entradas desde C. Código real: llamadas, retornos, FPU, E/S. Ver «Recompilador» abajo |
| 2026-10-10 | f7b655d | **Recompilador**, `bench` mixto | PS3 real (mismo equipo) | intérprete: 21,3 MIPS | **156,6 MIPS (×7,4)** | Bucle de 10 instrucciones con ADD.S nativa; ver desglose abajo |
| 2026-10-10 | f7b655d → f18b392 | Recompilador, escritura temprana inmediata | PS3 real (mismo equipo) | nemu64-test 1817 ms; bench enteros 749,9 MIPS | 1948 ms; 473,7 MIPS (**peor**) | Un `std` justo después de la instrucción que da el valor detiene el pipeline (~3 ciclos por almacenamiento). Pasa a opción y diferida (1fe8ebc) |
| 2026-10-10 | f18b392 | Recompilador con registros fijos (`OPT_PIN`), nemu64-test | PS3 real (mismo equipo) | 1948 ms (97,2 MIPS) | **1817 ms (104,2 MIPS; ×4,45 del intérprete)** | sp, ra, v0, v1, a0–a3 en r18–r25 entre bloques; +7 %. Por omisión en el frontend |
| 2026-10-10 | f18b392 | **Frontend con vitest**, recompilador | PS3 real (mismo equipo) | intérprete: 65,6 / 35,0 ms por campo (sin / con bucles de espera) | **5,38 / 2,90 ms (×12); 59,94 campos/s** | Velocidad completa: el resto del campo es la espera del vsync |
| 2026-10-10 | f416bee | Recompilador, cuatro variantes, nemu64-test | PS3 real (mismo equipo) | intérprete 8098 ms | sin opciones 1941 ms (×4,17); escritura temprana diferida 1992; **registros fijos 1821 (×4,45)**; ambas 1849 | La escritura temprana no compensa ni diferida: queda apagada. Con registros fijos el bench enteros cae de 750 a 128 MIPS (4 registros por bloque): préstamo en bucles (siguiente fila) |
| 2026-10-10 | b11861c | Recompilador con código en KSEG1 y por TLB (rec_tlb.c), cuatro variantes, nemu64-test y bench | PS3 real (mismo equipo) | intérprete 7972 ms; f416bee: sin opciones 1941, temprana 1992, fijos 1821, ambas 1849 | sin opciones 1973 ms (×4,04); temprana 2014; **fijos 1856 (×4,29)**; ambas 1897. Bench enteros con registros fijos **760 MIPS** (antes 128: el préstamo en bucles funciona) | Mismas 446 fallas, hash y ciclo que el intérprete; 232/232 unitarias en cada variante. 623 de 29 442 bloques fuera de KSEG0 (antes al intérprete). 1–3 % más lento que f416bee, dentro de lo que varió entre corridas anteriores (sin opciones 1817 → 1941); nemu64-test casi no tiene código mapeado, la medición que importa es GoldenEye/Perfect Dark |
| 2026-10-10 | 8c27863 | **GoldenEye (NTSC)** en el frontend, recompilador con código mapeado por la TLB | PS3 real (mismo equipo), respaldo del usuario | antes: el código mapeado lo corría el intérprete (~24–33 MIPS en nemu64-test) | **59,8 campos/s, 93,7 MIPS (velocidad completa), emulación 5,6 ms por campo** | 3512 de 3518 bloques mapeados, 5050 traducciones (todas en el arranque), ~0,08 % interpretado. El juego no pasa del arranque gráfico (microcódigo F3D sin soporte: ruido en pantalla, igual con el intérprete solo), así que es carga de CPU sin dibujar y sin bucles de espera. Perfect Dark: 5,1 ms por campo, también atorado en el arranque con el intérprete |
| 2026-10-10 | bd8ac59 → (esta rama) | Bucles de espera en código mapeado por la TLB (`idle.c`), GoldenEye y Perfect Dark 21,3 s de consola con el intérprete | Mac (salsa64-run nativo), respaldos del usuario | GoldenEye 0,0 % en bucles de espera, 16,2 s; Perfect Dark 0,0 % | **GoldenEye 97,6 %, 0,8 s; Perfect Dark 99,3 %** | Mismos registros, log e imagen del VI que sin el cambio. GoldenEye: su hilo ocioso es un `b .` mapeado en 0x7000071c (no dibuja por el microcódigo F3D). Pendiente medir en la PS3 |
| 2026-10-10 | be8f573 (main ce8351d) | **Perfect Dark (NTSC)** en el frontend con la HLE de Rare (F3DPD) y naudio, intro sin tocar el control | PS3 real (mismo equipo), respaldo del usuario | antes: no dibujaba (microcódigo sin HLE) | **59,9 campos/s (100 % de NTSC)**; la intro en alta resolución 50,7–56,3 campos/s (84–94 %); HLE en el SPE 1,4–8 ms por tarea, emulación 5,8–8,8 ms por campo; audio naudio con MP3 en el SPE 1,7–4,7 ms por tarea | 0 tareas cortadas, 0 comandos de otro microcódigo saltados, audio sin bloques tarde. Mario Kart 64 con el mismo paquete: 100 % de NTSC, sin regresión |
| 2026-10-10 | 524d80e | Bucles de espera en código mapeado, **GoldenEye** en el frontend con el recompilador | PS3 real (mismo equipo), respaldo del usuario | 59,8 campos/s, 93,7 MIPS, 0 % en bucles de espera, emulación 5,6 ms por campo | **59,8 campos/s, 1,2 MIPS, 99 % en bucles de espera, emulación 0,6 ms por campo** (0,5 ms es la copia del VI) | Su hilo ocioso es un `b .` mapeado en 0x7000071c; el juego no dibuja todavía (microcódigo F3D) |
| 2026-10-10 | 697922c → b3ba375 | **Caché de texturas** entre tareas (HLE gráfica en el SPE 0), Zelda OoT con el recompilador, ventanas fijas de 100 campos | PS3 real (mismo equipo), respaldo del usuario | HLE en el SPE 14,0–30,6 ms por tarea; escenas pesadas a 38–48 campos/s; peor campo 36–43 ms | **5,1–9,6 ms por tarea (×2,8–3,3); 59,8–60,2 campos/s en todas; peor campo 17–27 ms** | Decodificar texturas: 6,7–17,2 → 0,00–0,09 ms (0–1 textura decodificada por tarea); identificarlas (hash) 0,03–0,35 ms; cargas a la TMEM 3,7–7,3 → 1,4–3,0 ms (copia en bloque). Mismas capturas byte a byte. Ver «Caché de texturas» abajo |
| 2026-10-10 | f416bee | **Zelda OoT (NTSC) en el frontend**, recompilador con registros fijos, HLE gráfica en el SPE | PS3 real (mismo equipo), respaldo del usuario | intérprete: 15,4–17,5 campos/s, emulación 56–64 ms por campo | **41–60 campos/s (69–100 %), emulación 4–14 ms por campo** | Logo e intro a 58–60; título y escenas con muchos triángulos 41–55, ahora limitadas por la espera a la HLE gráfica del SPE (10–30 ms por tarea), no por el CPU. 86–88 % del tiempo del CPU en bucles de espera. ~10 500 bloques, 7,9 MB de caché, 1,7 % de instrucciones interpretadas. Se ve igual que con el intérprete (el autor) |
| 2026-10-10 | e210aeb | **Recompilador: conversiones de la FPU, LWL/LWR y MFC0 en nativo**, nemu64-test con registros fijos | PS3 real (mismo equipo), cpu-tests SLSA00077 | mismo binario con `RARE_EXEC` (como antes): 1894 ms (×4,29 del intérprete, 8118 ms) | **1816 ms (104,1 MIPS; ×4,47); −4,1 %** | Mismas 446 fallas, hash y ciclo. Entradas desde C 140 909 → 110 074; llamadas al intérprete 2,15 M → 1,77 M. Bench: cvt 4,2 → 40,7 MIPS, no alineadas 20,5 → 218,5, mfc0 9,8 → 701, espera por Count 8,3 → 488 (salidas a C 2 M → 8). Ver «Conversiones de la FPU…» abajo |
| 2026-10-10 | 136dff4 (PS3: ad14e4b con main 5a0a1a3) | **Generador de fragment programs** (Fase 4), GoldenEye NTSC en Dam, ~3 min de juego | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00087 «Salsa64 fpgen», respaldo del usuario | combinación lineal sola: 36–50 campos/s en Dam (60–84 %), 97 449 combiners aproximados en 80 s de host | **34–55 campos/s (57–93 %), emulación 18–29 ms por campo; 0 combiners aproximados**; 4 programas generados en toda la corrida (0 vaciados), hasta 131 de 350 dibujos por tarea con programa generado, 7–23 cambios de programa por tarea, traducción al RSX 0,3–1,8 ms por tarea | La niebla por luz (lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE) exacta en las capturas de la presa. El cuello sigue en el CPU y la HLE del SPE (5–15 ms por tarea), no en el RSX. Zelda OoT 53–60 campos/s, SM64 54–60, Mario Kart 60: 0 programas generados (todo cabe en la combinación lineal), sin regresión visual. Ver «Generador de fragment programs» abajo |
| 2026-10-11 | 3b95776 | **HLE de la tarea JPEG de Zelda OoT** en la PS3, dos salas (tienda Kokiri y casa de Link) | PS3 real (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00091 «Salsa64 Kokiri», respaldo del usuario | antes: fondo sin dibujar (malla en colores planos) | **67,8 y 67,3 ms del PPE por sala**; ventana de la carga 57,3–57,6 campos/s, peor campo 37–39 ms de emulación | ~110 veces lo de la Mac: flotantes y conversiones a entero por memoria en el PPE. Se queda en el PPE (una vez por sala, durante la transición); pendiente pasarlo a punto fijo |
| 2026-10-11 | (rama fervent-hodgkin) | **HLE de la tarea JPEG de Zelda OoT** (fondos prerrenderizados), una sala: 75 tareas, 300 macrobloques | Mac (salsa64-run nativo, Apple Silicon) | antes: las tareas terminaban sin hacer nada (fondo basura) | **0,61 ms por sala** en el hilo del CPU, una vez al cargarla | Corre en el PPE (`core/rsp.c`); IDCT separable en flotante sin optimizar. El frontend escribe «JPEG: N tareas, M macrobloques en X ms del PPE» al terminar cada tanda: con esa medida se decide si se queda en el PPE o va a un SPE. El fondo se copia en el SPE 0 (40 tiras por cuadro, reutilizadas por la caché de texturas) |

## Efectos de framebuffer (2026-10-11)

Rama hopeful-dirac (docs/framebuffer.md). Lo que cuesta cada mecanismo lo
escribe el frontend cada 5 s («gfx: framebuffer: N copias a la RDRAM (ms
c/u en presentación), la emulación esperó N tareas (ms c/u, N tarde), N
copias espejo, N cuadros copiados; texturas desde un destino: del RSX, de
la RDRAM, del ligado; destinos cebados; copias perdidas, destinos sin
superficie, color images imposibles»). Interruptor:
`SALSA64_DATA_DIR/fbfx.cfg` (`0` apaga todo, `1` solo render a textura,
`2` todo, por omisión).

**RPCS3** (0.0.43; no vale para tiempos): gfxtest 35 de 35; 122 copias de
320 × 240 en 120 cuadros, 2,05 ms cada una en presentación. Sirve solo
para saber que el mecanismo funciona de punta a punta.

**PS3 real** (2026-10-11, Slim CECH-3011A, HFW 4.91 + HEN, app SLSA00089
«Salsa64 framebuffer», 720p a 3×; logs fuera del repo):

| Juego | Escena | Campos/s | Copias a la RDRAM (esperadas por la emulación) | Espejos | Notas |
|---|---|---|---|---|---|
| gfxtest | 35 de 35 puntos | — | 1 por cuadro (la imagen de 64 que lee el CPU): 6,7–6,9 ms c/u en presentación; la emulación esperó 7,4–7,9 ms por tarea | 2 por cuadro | Un caso artificial que copia en cada cuadro: el peor costo posible |
| Zelda OoT | bosque Kokiri, un minuto | 59,8 (100 %) | 0 | 0 | Emulación 4,0 ms por campo, igual que antes; 225–290 cuadros copiados por 5 s (espejo del framebuffer, 0,0 ms medidos) |
| Zelda OoT | abrir la pausa | — | el fondo (320 × 240): 16 ms; y 4 chicas (64 × 112): Link, su z, su copia y la cobertura | — | Una vez por pausa; la N64 tarda ~1 s en filtrar el fondo |
| Zelda OoT | pausa abierta (paquete con 16 superficies) | 59,9 (100 %) | 2 por tarea (Link y su z, redibujados cada tarea aunque nadie los lea): 7,4–8,4 ms c/u; la emulación esperó 16–24 ms por tarea | 0 | 44 texturas por tarea desde la RDRAM (las franjas del fondo y Link). El paquete final espera esas copias solo las dos primeras tareas: por medir |
| Mario Kart 64 | Luigi Raceway, túnel | 60 | 0 | 0 | La cámara del túnel se ve gracias al espejo del framebuffer por campo |
| SM64 | castillo exterior, un minuto | 59,8 (100 %) | 1 al arrancar (12 ms) | 0 | |
| GoldenEye | Dam, un minuto | 45,8 (76 %) | 0 | 0 | Dentro del rango previo del nivel (34–55: va por el CPU); 225–229 cuadros copiados por 5 s, 0,0 ms por campo en presentación |
| Banjo-Kazooie | intro y Spiral Mountain | 56–57 | 0 | 0 | Igual que antes |
| Conker | intro, 3 minutos | 59,8 | 1–2 por ventana de 5 s (una color image de 292 que se dibuja cada tarea y no se presenta: la primera se espera, las demás van sin esperar) | muchas | Igual que antes |

## Caché de texturas (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00070 «Salsa64
texturas», Zelda OoT NTSC (respaldo del usuario, fuera del repo), sin
tocar el control: arranque, logo, intro y título. Las dos compilaciones
son de un hilo con el recompilador y solo difieren en la HLE gráfica:
«antes» = main 697922c más la instrumentación (ventanas fijas de 100
campos, capturas en campos fijos, salida en el campo 3510; parche en la
rama, sin cambios de comportamiento); «después» = b3ba375 (caché de
texturas, docs/texturas.md). La emulación es determinista: misma ventana,
misma escena; las 7 capturas (campos 500, 1000, …, 3500) son **idénticas
byte a byte**. Por tarea gráfica (~33 tareas cada 100 campos):

| Campos | Escena | Campos/s | Peor campo | Dibujos | Texturas decodificadas | HLE en el SPE | Decodificar | Cargas a la TMEM | PPE esperando al SPE |
|---|---|---|---|---|---|---|---|---|---|
| 0–99 | Logo de N64 | 59,5 → 57,9 | 29,6 → 71,5 | 22 | 18,0 → 0,2 | 1,04 → 0,51 ms | 0,47 → 0,01 | 0,12 → 0,04 | 0,66 → 0,13 |
| 500–599 | Intro nocturna | 60,2 → 60,0 | 24,8 → 17,2 | 110 | 83,3 → 0,7 | 14,04 → 5,09 ms | 6,75 → 0,06 | 3,73 → 1,43 | 13,56 → 4,62 |
| 1300–1399 | Intro (Epona) | 59,8 → 60,0 | 32,0 → 18,5 | 137 | 111,5 → 0,0 | 19,75 → 6,17 ms | 11,03 → 0,00 | 4,82 → 2,22 | 19,28 → 5,71 |
| 1700–1799 | Intro, la más pesada | **41,3 → 59,9** | 211,9 → 21,8 | 248/244 | 170,0 → 0,7 | 28,31 → 8,92 ms | 15,82 → 0,09 | 6,54 → 2,82 | 27,81 → 8,44 |
| 2000–2099 | Título con el castillo | 60,4 → 60,5 | 29,2 → 17,1 | 147 | 91,3 → 0,1 | 21,03 → 6,43 ms | 11,47 → 0,01 | 5,63 → 2,42 | 20,55 → 5,94 |
| 2200–2299 | Escena con más dibujos | **45,2 → 60,1** | 42,7 → 21,8 | 272/268 | 182,7 → 0,0 | 30,62 → 9,33 ms | 17,21 → 0,00 | 7,10 → 2,96 | 30,13 → 8,84 |
| 2500–2599 | | **45,2 → 60,4** | 37,5 → 20,0 | 201/200 | 130,5 → 0,0 | 25,38 → 8,38 ms | 13,06 → 0,00 | 6,96 → 2,93 | 24,91 → 7,90 |
| 3100–3199 | | **45,1 → 60,0** | 34,9 → 20,0 | 163/161 | 137,0 → 0,0 | 22,39 → 6,79 ms | 12,85 → 0,00 | 5,34 → 2,54 | 21,91 → 6,31 |

Tiempos del decrementador del SPE (`gfx_result.t_*`); el peor campo es el
intervalo más largo entre dos campos en el bucle (con la presentación). Las
ventanas que terminan en un múltiplo de 500 (400–499, 900–999, …) tienen
una captura (~400 ms) y no se comparan; las de 0–99 incluyen el arranque.

- La decodificación desaparece: 0 a 1,1 texturas decodificadas por tarea
  (se reusan 36–320 por tarea, la mitad de tareas anteriores); en los 3510
  campos no hubo desalojos ni texturas al arena (cabe todo en 8 MB y 1024
  entradas). Identificar las texturas (hash de lo que leen en la TMEM y
  búsqueda) cuesta 0,03–0,35 ms por tarea.
- Las cargas a la TMEM bajan ×2,4 por copiar en bloque (antes `memcpy` de 8
  bytes por palabra y 4 más para intercambiar mitades). Ahora son lo más
  caro de la HLE junto con el estado: 2250–2570 lecturas de la RDRAM por
  tarea para 260–450 cargas (LOADTILE lee fila por fila, cada una con un
  DMA que espera). Siguiente paso: pedir las filas por adelantado o con
  una lista de DMA.
- Menos dibujos (248 → 244): la misma textura cargada dos veces en una
  tarea antes daba dos copias en el arena con distinta dirección y cortaba
  el dibujo; ahora es la misma entrada.
- El PPE sigue esperando al SPE 4,6–9,1 ms por tarea en la intro y el
  título, pero ya cabe en el campo: todas las ventanas sin captura llegan a
  60 campos/s.

## Dos hilos en el PPE (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00073 «Salsa64
hilos», recompilador activo en los dos casos. «Antes» = main 697922c (un
hilo: emulación, presentación y flip en vsync en el mismo bucle).
«Después» = ee5e917 + 5ee9f40 (hilo de emulación + hilo de presentación,
ritmo de la tele emulada; docs/hilos-ppe.md). Ventanas de 5 s del log del
frontend; los campos por segundo son los **emulados**.

### vitest (ROM propia, pantalla 320×240 por el CPU)

| | Antes (un hilo) | Después (dos hilos, con ritmo) | Después sin límite (medición A/B) |
|---|---|---|---|
| Campos/s | 59,9 (tope del flip en vsync) | 59,8–60,0 (ritmo) | **187 → 363** sin/con detección de bucles de espera |
| Emulación por campo | 2,5–5,2 ms | 2,3–2,4 ms | 5,02 → 2,45 ms (×2,1) |
| Copia del VI | — | 0,30 ms (300 KB) | 0,30 ms |
| Presentación por campo | 11,4–14,1 ms (casi todo el flip) | 0,2–0,3 ms + flip 0,0–0,1 ms | 0,4 ms + flip 14,6 ms, a 60/s; 1 100–1 500 campos saltados por ventana |
| Espera del ritmo | — | 14,0 ms por campo | — |

La medición A/B de vitest antes daba 59,94 → 59,94 campos/s (el flip
tapaba todo); ahora mide de verdad: 187 → 363.

### Zelda: Ocarina of Time (NTSC, respaldo del usuario, fuera del repo)

Campos/s por escena (ventana de 5 s que contiene el campo):

| Escena (campo) | Antes (un hilo) | Después (dos hilos) |
|---|---|---|
| Logo de N64 (~150–590) | 57,8–59,0 | 58,4 |
| Intro nocturna (~590–1200) | 59,0–60,0 | 57,9–59,4 |
| Link en Epona (~1200–1750) | 58,6–59,4 | 57,1–59,4 |
| Título con el castillo (~1750–2700) | **52,3 → 50,9 → 47,3 → 45,0 → 41,3** | **57,0 / 59,4 / 57,0 / 59,3 / 59,9** |
| Después del título (~2700–4700) | 41,6–59,9 | 57,4–59,7 |

Desglose por campo en el título (ventana del campo ~2250): antes
emulación 12,2 ms + presentación 8,8 ms (flip) = 21 ms → 47 campos/s;
después emulación 13,7 ms, ritmo 2,8 ms, presentación 0,0 ms, flip 8,9 ms
en el otro hilo → 57–59 campos/s. Las ventanas a 57 son las que tienen una
captura `screen-N.ppm`: cada captura frena la presentación ~400 ms (leer
el búfer del RSX desde el PPE), descarta 4 campos, hace esperar al hilo
de emulación ~250 ms por un juego de búferes y reinicia el ritmo. Por eso
las capturas periódicas pasaron a ser opcionales (`capturas`).

Lo que queda dentro de «emulación»: la espera del hilo de emulación al
SPE al terminar cada tarea gráfica, 10–27 ms por tarea con ~20 tareas/s
(20–53 % del tiempo del hilo). Es el cuello siguiente: que el evento del
RSP se vuelva a programar en vez de bloquear (`src/core/rsp.c`), o acortar
la HLE en el SPE (caché de texturas). La traducción al RSX cuesta
0,15–0,88 ms por tarea y la copia del VI 0,20 ms por campo (se copia
aunque el VI muestre un destino del RSX; se puede evitar).

### Guardado por hilos

savetest (SRAM) dos veces en la PS3: «persistió el arranque 1», volcado
diferido escrito por el hilo de presentación a los ~2 s (257 escrituras
del juego), salida limpia. START + SELECT y «Salir del juego» funcionan;
los controles llegan por el buzón entre hilos (vitest los identifica).

## Audio: naudio y MP3 en el SPE 1 (2026-10-10)

**PS3 real** (mismo equipo), rama `charming-allen-a8e4b3` con main
485a747, recompilador. Paquete «Salsa64 naudio» (SLSA00080). Respaldos
propios; ventanas de 5 s.

| | Banjo-Kazooie | Perfect Dark | Conker |
|---|---|---|---|
| ABI | naudio (Banjo-Kazooie) | naudio con MP3 | naudio (Conker) |
| Corrida | 89 s, del logo a jugar | 179 s, sin imagen (falta su microcódigo gráfico) | 96 s, pantalla negra (se detiene tras la primera tarea gráfica) |
| Tareas | ~30/s, 0 sin implementar, 0 errores | ~30/s, 0 sin implementar, 0 errores | 30/s, 0 sin implementar, 0 errores |
| HLE en el SPE 1, por tarea | mediana **2,92 ms** (ventanas 1,24–3,65; la más larga 5,68) | mediana **5,70 ms** (2,09–7,83; la más larga 10,38) | **1,56 ms** (solo el bus de efectos: el juego no dispara voces) |
| PPE esperando al SPE | 0,000 ms | 0,000 ms | 0,000 ms |
| Emulación | 100 % de media (mín. 88 %) | 97 % (mín. 78 %: el CPU) | 100 % |
| Salida | 1,1 % en silencio (el llenado), 6,6 % estirado, 13 faltas breves, 0 bloques tarde | 2,3 % en silencio, 20,5 % estirado (las bajadas del CPU), 28 faltas breves, 0 bloques tarde | normal |
| Hilo de audio | 1,9 % | 2,2 % | 1,7 % |
| Oído (el autor) | «suena bien» (el video parpadea: de la HLE gráfica) | «suena bien» | silencio, lo esperado |

Listas de prueba al arrancar (SPE contra PPE, ahora 5: nead OoT y MK, ABI1,
naudio con MP3 y Banjo-Kazooie): iguales, 2,9 ms.

El SPE 1 pasa ~9 % del tiempo en Banjo y ~17 % en Perfect Dark (más
comandos por tarea: ~875 contra ~380, y el MP3). Código escalar; los
candidatos a SIMD siguen siendo ENVMIXER, RESAMPLE y ADPCM, y ahora la
ventana del MP3.

## Audio: AI, HLE de audio en el SPE 1 y salida (2026-10-10)

**PS3 real** (mismo equipo), rama `amazing-varahamihira-3027c8`
con main 298b18d (recompilador, dos hilos con ritmo, fin de tarea por
sondeo). Paquete «Salsa64 audio» (SLSA00072). Ventanas de 5 s.

audiotest (ROM propia; tono por el CPU y luego por una tarea de audio de
14 comandos nead cada 16 ms):

| | PS3 real |
|---|---|
| Velocidad | 59,8 campos/s (100 % de NTSC), 96 % del tiempo en bucles de espera |
| HLE de audio en el SPE, por tarea | **0,057 ms** (máx. 0,057), PPE esperando 0,000 ms, 62,5 tareas/s |
| Listas de prueba al arrancar (SPE contra PPE, 3 listas) | iguales, 1,0 ms (en RPCS3, 1,3 s: compila el programa del SPE) |
| Salida | colchón 78–98 ms, régimen normal (sin estirar), 0 faltas, 0 silencios, 0 saltos, 0 bloques tarde; 2 % de silencio solo en los primeros 5 s (llenado del colchón) |
| Hilo de audio | 1,9 % del tiempo de pared |
| Oído (el autor) | suena bien: el afinador marca 440 Hz y cada nota sale de su lado |
| Corrida completa | 96 s, 3 007 488 muestras del AI, 4 510 464 a la salida (4 392 en silencio: el llenado inicial), 17 619 bloques; START + SELECT cierra el audio y sale normal |

Zelda OoT NTSC (respaldo propio), recompilador, del logo al juego;
corrida de 535 s (105 ventanas) más una de 120 s con el mismo resultado:

| | PS3 real |
|---|---|
| ABI | nead (Zelda OoT): 0 comandos sin implementar, 0 errores, ~60 tareas/s |
| HLE de audio en el SPE 1, por tarea | mediana **2,42 ms**, ventanas de 5 s entre 0,30 (arranque) y 2,83 ms; la tarea más larga 4,02 ms |
| PPE esperando al SPE | **0,000 ms** (fin de tarea por sondeo; el SPE termina antes de que el núcleo pregunte) |
| Salida | emulación al 99,9 % de media, régimen normal; 0,06 % del tiempo en silencio (el llenado inicial), 0,4 % estirado (los arranques de escena), 3 faltas breves en 9 min, 0 silencios, 0 saltos, 0 bloques tarde |
| Hilo de audio | 2,0 % del tiempo de pared |
| Oído (el autor) | «se escucha perfecto» |

Super Mario 64 NTSC: sin F3D (otro trabajo) el juego no pasa del arranque y
no hay imagen ni sonido audible, pero sus tareas de ABI1 corren en el SPE
1 sin errores ni comandos desconocidos: **0,115 ms** por tarea, 60/s.
Pendiente oírlo cuando F3D esté integrado.

El costo de la HLE de Zelda (2,4 ms por tarea, código escalar) cabe de
sobra en el SPE (14 % de un campo) y no frena al PPE. Si hace falta, los
bucles de ENVMIXER, RESAMPLE y ADPCM son candidatos claros a SIMD.


## Fin de tarea del RSP por sondeo (2026-10-10)

**PS3 real** (mismo equipo), recompilador y caché de texturas en los dos
casos, dos hilos. «Antes» = main 4a4d98c (el evento de fin de tarea
espera al SPE); «después» = 16a544f + 0cb8f63 (sondeo: el evento se
reprograma cada 10 000 ciclos; si el CPU está en bucle de espera, el
backend duerme 100 µs por sondeo). Zelda OoT NTSC, 52 s desde el logo
hasta pasado el título; ventanas de 5 s.

| | Antes (espera) | Después (sondeo) |
|---|---|---|
| Campos/s | 58,5–60,0 | 58,5–59,9 |
| Emulación por campo | 3,2–7,5 ms | 3,2–7,2 ms |
| Emulación esperando al SPE, por tarea | 2,3–8,2 ms (100 esperas / 100 tareas) | **0,00 ms (0 esperas)** |
| Sondeos por tarea / dormida en bucles de espera | — | 33–75 / 2,9–6,9 ms |
| Ritmo (margen por campo) | 9,0–13,6 ms | 9,3–13,6 ms |

Lectura: la espera bloqueante desapareció, pero en Zelda no hay nada que
hacer mientras el RSP trabaja (el CPU está en el bucle ocioso de
libultra el 86–88 % del tiempo): el tiempo que antes bloqueaba ahora lo
duerme el sondeo en pasos de 100 µs, y la emulación por campo queda
igual. La ganancia aparecerá donde el CPU tenga trabajo durante el RDP
(lógica del juego después de mandar la tarea; Super Mario 64 con F3D es
el candidato). Sin regresiones: 60 campos/s, 0 descartados, salida
limpia; gfxtest OK en la PS3.

gfxtest en la PS3: OK (22 de 22) antes y después; 38 → 36–37 campos/s
(26 → 27 ms por campo: la ROM espera al RSP con un bucle que no es de
espera pura y ahora lee Count cada 1024 vueltas). La primera versión de
la ROM con espera por tiempo leía Count en cada vuelta y el recompilador
salía a C en cada iteración (64 M salidas, 319 M instrucciones
interpretadas, 9 campos/s).

## Líneas base de la Fase 0 (pendientes de PS3 real)

- `spu-compute`: 6 MB, PPE escalar contra 6 SPUs (tiempos en el log).
  **PS3 real** (2026-10-10, CECH-3011A, HEN): PPE 13 000 µs; 6 SPUs 889 µs
  incluyendo arranque y join (≈14,6×). Los 6 SPUs están disponibles con HEN.
- `rsx-triangle`: 120 cuadros con vsync.
  **PS3 real** (2026-10-10, CECH-3011A, HEN): verificación de píxeles OK a
  1280×720; 120 cuadros en 2067 ms (≈58 cps; el cuadro de verificación espera
  al RSX). La inicialización del RSX tarda ≈2 s.
- `jit-ppe`: costo de limpiar 64 KB de caché (dcbst + icbi).

## Líneas base de la Fase 1

Micro-benchmark del intérprete (`tests/unit/bench.c`): bucle de 10
instrucciones (LW, ADDIU, SW, ADDU, SLL, XOR, ADD.S, ADDIU, BNE, NOP) desde
KSEG0, 20 M instrucciones.

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | (este) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 237 MIPS | Solo referencia del host |
| 2026-10-10 | (este) | RPCS3 0.0.43, PPU LLVM | 35,6 MIPS | **No concluyente** |
| 2026-10-10 | (arranque HLE) | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 217 MIPS | −8 %: la FPU calcula ahora Inexact y los modos de redondeo con exactitud (TwoSum/fma); el bucle tiene un ADD.S cada 10 instrucciones. Costo de corrección, no regresión |
| 2026-10-10 | (arranque HLE) | RPCS3 0.0.43, PPU LLVM | 29,0 MIPS | **No concluyente** |
| 2026-10-10 | 81da001 | **PS3 real**: Slim CECH-3011A, HFW 4.91 + HEN, ppu-gcc -O2 -mcpu=cell | **8,8 MIPS** | Primer número real. 154/154 pruebas pasan en big-endian. Muy lejos de los ~60 MIPS que pide la N64 a velocidad completa: perfilar antes de seguir (la sospecha del FPSCR resultó falsa: ver la sección del perfilado) |
| 2026-10-10 | 419731a | **PS3 real** (mismo equipo) | 8,7 MIPS | Mismo intérprete con el benchmark en variantes: repite la línea base |
| 2026-10-10 | c14268c | **PS3 real** (mismo equipo), + `-mno-relative-jumptables` | **17,1 MIPS** | ×2,0. Desglose abajo |
| 2026-10-10 | c14268c | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 294 MIPS | Solo referencia del host |
| 2026-10-10 | 147d5ac | **PS3 real** (mismo equipo) | **21,2 MIPS** | Segunda ronda. Desglose abajo |
| 2026-10-10 | 147d5ac | Host: Mac Studio, Docker arm64, gcc 12 -O2 | 297 MIPS | Solo referencia del host |

### Desglose por clase de instrucción (2026-10-10)

Variantes de `tests/unit/bench.c` (bucles de 10 instrucciones con la misma
cola ADDIU + BNE + delay slot). Ciclos del PPE = ns × 3,2 (3,2 GHz). PS3
real (CECH-3011A, HFW 4.91 + HEN); paquete `salsa64-cpu-tests`, 154/154
pruebas en ambas corridas.

| Variante | Original (419731a) | Ronda 1 (c14268c) | Ronda 2 (147d5ac) | Ciclos/instr. | Total |
|---|---|---|---|---|---|
| mixto | 8,7 MIPS | 17,1 | 21,2 | 364 → 187 → 150 | ×2,4 |
| nop | 12,6 | 38,2 | 34,4 | 254 → 84 → 93 | ×2,7 |
| enteros | 12,8 | 23,5 | 25,6 | 248 → 136 → 124 | ×2,0 |
| memoria (LW/SW) | 14,0 | 22,1 | 20,9 | 227 → 144 → 152 | ×1,5 |
| mov.s | 8,6 | 13,3 | 23,5 | 369 → 240 → 136 | ×2,7 |
| add.s (inexacto) | 2,6 | 7,6 | 14,0 | 1194 → 416 → 227 | ×5,3 |

Host (Mac Studio, Docker arm64, sin sanitizers), solo referencia: mixto
231 → 294 MIPS. RPCS3 dio 21,4 MIPS después (antes 29): no concluyente, su
recompilador trata distinto la función grande del bucle.

Diagnóstico (antes):
- La sospecha del FPSCR no aplicaba: `cop1.c` nunca cambia el modo de
  redondeo del host (calcula al más cercano y corrige con TwoSum/fma).
- El bucle costaba ~250 ciclos por instrucción aun para un NOP: volvía a
  leer pc, next_pc, cycles y las banderas de salto justo después de
  escribirlos (load-hit-store), llamaba a `execute()` (que guardaba y
  restauraba registros en la pila: otro load-hit-store), ponía `gpr[0] = 0`
  y lo leía en la instrucción siguiente, y cada `switch` usaba `lwax`
  (microcódigo).
- ADD.S costaba ~1340 ciclos más que un NOP: cada valor pasaba entre
  registros enteros y de punto flotante por la pila (el PPE no tiene
  movimientos directos), unas cinco veces por instrucción, más una cadena
  de llamadas.

Lo que queda (después), en ciclos por instrucción por encima de un NOP:
- ALU de SPECIAL ~75 y LW/SW ~76: segundo despacho (`switch` de funct) y
  dependencias entre instrucciones a través de `cpu->gpr` en memoria (la
  siguiente lee el registro que la anterior acaba de escribir). SLL/SRL/SRA
  usan desplazamientos variables (microcódigo).
- MOV.S ~223: todo lo que no cubre el camino rápido de la FPU pasa por la
  llamada a `salsa64_vr4300_cop1` y dos `switch` más.
- ADD.S ~475 en el camino rápido: se presume que son las comparaciones de
  punto flotante seguidas de salto (unas diez), que en el PPE esperan toda
  la latencia de la FPU. Siguiente paso: clasificar entradas con los bits
  en registros enteros (cargados de memoria, sin transferencia) y una franja
  de exponentes que garantice que no hay desbordamiento ni subdesbordamiento.

Segunda ronda (147d5ac), en ciclos por encima de un NOP: ALU de SPECIAL
75 → 45, MOV.S 223 → 61, ADD.S 475 → 192; LW/SW igual (~74). El índice
del despacho único (64 + funct, sin salto) alarga la cadena de despacho de
todas las instrucciones: NOP y memoria, que no usaban el segundo despacho,
perdieron ~9 ciclos. Para una mezcla de juego con ~1/3 de SPECIAL queda a
favor; se podría acortar la cadena calculando el índice ya escalado y
directo desde la palabra de la instrucción (unos 2–4 ciclos).

Lo que queda: el bucle cuesta ~93 ciclos aun para un NOP (~45
instrucciones del PPE). Es sobre todo la cadena dependiente de cada
instrucción: cargar la palabra de la RDRAM → extraer el índice → cargar la
dirección de la tabla → mtctr → bctr, más la latencia de cada carga en un
procesador in-order. Bajarlo de verdad pide decodificar por adelantado
(caché de instrucciones predecodificadas o despacho en tubería: decodificar
la siguiente mientras corre la actual), y la dependencia entre
instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. Es el
terreno del recompilador (Fase 3).

Reglas del PPE que salieron de aquí (aplican también al recompilador de la
Fase 3): no releer lo recién escrito; nada de funciones cortas con marco
de pila en el camino caliente; evitar `lwax`/`lha` (cargas con extensión
de signo), desplazamientos variables y formas con punto (`rldicl.`,
`srdi.`) en código caliente; no mover valores entre registros enteros y de
punto flotante; clasificar valores de punto flotante con sus bits en
enteros y no con comparaciones de punto flotante seguidas de salto;
`-mno-relative-jumptables` en todo el código del PPU.

Frontend con vitest (`salsa64.pkg`, ROM propia: bucles de dibujo con
enteros y espera activa a la interrupción del VI, sin FPU):

| Fecha | Commit | Entorno | Resultado | Notas |
|---|---|---|---|---|
| 2026-10-10 | d27735e | RPCS3 0.0.43, PPU LLVM | 24,0 campos/s, 37,7 MIPS | **No concluyente** |
| 2026-10-10 | d27735e | **PS3 real**: Slim CECH-3011A, HFW 4.91 + HEN | **8,7 campos/s (14 % de NTSC), 13,6 MIPS** | Emulación 114,6 ms por campo, presentación (subida de textura + dibujo en rsxkit) 0,3 ms. 13,6 MIPS sin FPU contra 8,8 del bench con un ADD.S cada 10 instrucciones, medido antes del perfilado (el costo de la FPU era el paso de valores por la pila, no el FPSCR) |
| 2026-10-10 | 940f6f8, sin detección | **PS3 real** (mismo equipo) | **15,2 campos/s (25 % de NTSC), 23,8 MIPS** | Intérprete optimizado (147d5ac) sin saltar bucles de espera: emulación 65,6 ms por campo. ×1,75 frente a d27735e |
| 2026-10-10 | 940f6f8, con detección | **PS3 real** (mismo equipo) | **28,3 campos/s (47 % de NTSC), 24,1 MIPS** | Emulación 35,0 ms por campo; 45,6 % del tiempo de la consola se saltó en el bucle de espera al VI. El tope de 60 campos/s lo pone el flip en vsync de rsxkit |

### Bucles de espera (2026-10-10)

Medición A/B en la misma corrida (`salsa64.pkg` de 940f6f8): tras
verificarse con vitest, el frontend corre 10 s sin detección y 10 s con
ella. Dos corridas seguidas dieron lo mismo con 0,2 % de diferencia.

| | Sin detección | Con detección |
|---|---|---|
| Emulación por campo | 65,6 ms | 35,0 ms (×1,9) |
| Campos/s | 15,2 | 28,3 |
| MIPS (instrucciones ejecutadas) | 23,8 | 24,1 |
| Tiempo de la consola saltado | 0 % | 45,6 % |

En el host (salsa64-run con ASan) vitest hasta su verificación: 1,70 →
0,93 s. nemu64-test no espera nunca (0 % saltado, mismo tiempo).

Costo en el intérprete (`salsa64-cpu-tests`, bench de 10 instrucciones con
BNE tomado cada 10). El log anterior de cpu-tests en la consola repetía
exactamente los números de 147d5ac, así que la variación entre corridas
parece menor a 0,1 MIPS:

| Variante | 147d5ac | 940f6f8 |
|---|---|---|
| mixto | 21,2 | 21,0 |
| nop | 34,4 | 33,9 |
| enteros | 25,6 | 25,3 |
| memoria | 20,9 | 20,6 |
| mov.s | 23,5 | 23,2 |
| add.s | 14,0 | 14,0 |

Unos 15 ciclos más por salto tomado (el BNE del bench salta 36 bytes
atrás: solo hace la resta y la comparación, no consulta la tabla). Se
revisó el código del PPU: **ppu-gcc no pone indicaciones estáticas en los
saltos** (bits a/t de BO en 0 en los 337 saltos condicionales del bucle,
igual que antes; los `+`/`-` de objdump siguen la convención antigua del
bit «y» y no son predicciones del PPE). Así que no es una predicción
estática equivocada; queda la cadena dependiente (destino → resta →
comparación → salto) y el cambio de disposición del código. Posible
mejora: decidir con el inmediato de la instrucción (desplazamiento entre
−8 y −1), que no depende del destino calculado. Medir antes de adoptarla.

Corrida completa de nemu64-test (`salsa64-run`, ≈189 M ciclos del VR4300):
0,75 s en el host sin sanitizers (253 MIPS); 2,1 s con ASan + UBSan.

## Recompilador (Fase 3)

Primera corrida en la PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 +
HEN), paquete `salsa64-cpu-tests` de f7b655d (JR en línea y FPU nativa;
sin la escritura temprana ni los registros fijos de 632a796). Caché en la
cola NOBITS de 16 MB del segmento de texto, llamadas a C con `bl`.

- Pruebas unitarias con el recompilador en modo estricto (cada `t_step`
  ejecuta código generado): 163/163. El código generado corre, se
  reescribe y se invalida en el hardware (dcbst/sync/icbi/isync).
- nemu64-test completo, intérprete contra recompilador: mismas 462
  fallas, mismo hash de nombres (`fdfdf160a2fe7ad8`), mismo ciclo final.

| `bench` (`tests/unit/bench.c`) | Intérprete | Recompilador | Ciclos del PPE por instr. (rec) |
|---|---|---|---|
| mixto | 21,3 MIPS | 156,6 MIPS (×7,4) | 20 |
| nop | 32,6 | 1547,0 (×47) | 2,1 |
| enteros | 25,3 | 749,9 (×30) | 4,3 |
| memoria (LW/SW) | 20,9 | 159,7 (×7,6) | 20 |
| mov.s | 23,6 | 538,4 (×23) | 5,9 |
| add.s (inexacto) | 13,9 | 35,7 (×2,6) | 90 |

Los bucles del bench son bloques que vuelven sobre sí mismos sin escribir
memoria ni salir a C: miden el mejor caso. "memoria" queda en ~20 ciclos
por instrucción por el load-hit-store propio del bucle (LW de lo que el SW
de la vuelta anterior acaba de escribir) y por la prueba de rango. ADD.S
inexacto cuesta ~90 ciclos: la FPU hace la suma, el redondeo a simple y la
comparación de punto flotante seguida de salto (espera toda la latencia),
más la actualización condicional de FCR31.

nemu64-test es la medición con código real: 104 MIPS. Con el modelo actual
de un ciclo por instrucción, la velocidad completa pide hasta 93,75 MIPS
emulados por segundo, menos lo que se salta en bucles de espera (casi la
mitad del campo en escenas ligeras). Pendiente: medir con juegos (tabla de
docs/proyecto.md) en el frontend, y con 632a796 (escritura temprana, registros
fijos: cpu-tests mide nemu64-test con y sin ellos).

### Conversiones de la FPU, LWL/LWR y MFC0 en nativo (2026-10-10)

Antes iban al intérprete (una llamada a C por instrucción, con su
escritura y recarga de registros y la revisión de eventos al volver) y MFC0
terminaba el bloque. Ahora en nativo: SQRT, CVT.D.S/CVT.S.D,
ROUND/TRUNC/CEIL/FLOOR/CVT a W y L, CVT.S.W/CVT.D.W, CFC1/CTC1,
LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR (los pares LWL+LWR y SWL+SWR byte a byte) y
MFC0/DMFC0 (docs/rec-ppe.md). `SALSA64_REC_OPT_RARE_EXEC` repite lo de
antes con el mismo binario: con ella, salsa64-run da exactamente las
cifras de la línea base anterior (139 861 entradas, 1 989 273
interpretadas en nemu64-test).

Host (qemu ppc64, registros fijos; cuenta de eventos, no tiempo):

| Prueba | Antes (`RARE_EXEC`) | Después | |
|---|---|---|---|
| nemu64-test: llamadas al intérprete desde el código | 2 154 355 | 1 773 580 | −18 % |
| nemu64-test: entradas desde C | 139 861 | 109 026 | −22 % |
| nemu64-test: instrucciones del intérprete en el despachador | 1 989 273 | 1 591 007 | −20 % |
| vitest: llamadas al intérprete | 4 428 | 125 | −97 % |
| vitest: entradas desde C | 925 | 803 | −13 % |

Lo que queda en nemu64-test son sobre todo sus pruebas de COP0 (MTC0, TLB,
ERET), traps y CACHE.

Bench del recompilador en qemu (10 M instrucciones por caso, registros
fijos; cuentas, no tiempo):

| Caso | Antes (`RARE_EXEC`) | Después |
|---|---|---|
| `espera` (bucle de 5 que sondea Count, como la ROM gfxtest) | 2 000 000 entradas desde C y 2 000 000 llamadas al intérprete (una por vuelta) | 8 entradas, 0 llamadas |
| `mfc0` (Count, Status, Cause, EPC) | 1 000 000 entradas, 1 000 000 llamadas | 7 entradas, 0 llamadas |
| `cvt` | 6 999 958 llamadas | 0 |
| `no alin.` | 5 999 964 llamadas | 0 |

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; cpu-tests
SLSA00077 «Salsa64 CPU FPU» de e210aeb, mismo binario con y sin
`RARE_EXEC`, registros fijos):

| | Antes (`RARE_EXEC`) | Después | |
|---|---|---|---|
| nemu64-test | 1894 ms (99,9 MIPS, ×4,29) | **1816 ms (104,1 MIPS, ×4,47)** | −4,1 % |
| nemu64-test: entradas desde C | 140 909 | 110 074 | −22 % |
| nemu64-test: llamadas al intérprete | 2 154 724 | 1 773 947 | −18 % |
| bench `cvt` (CVT.S.W, TRUNC.W.S, CVT.D.S, CVT.S.D, SQRT.S, CFC1, CVT.W.S) | 4,2 MIPS | **40,7 MIPS** | ×9,7 |
| bench `no alin.` (pares LWL/LWR y SWL/SWR) | 20,5 MIPS | **218,5 MIPS** | ×10,7 |
| bench `mfc0` (Count, Status, Cause, EPC) | 9,8 MIPS | **700,7 MIPS** | ×71 |
| bench `espera` (bucle que sondea Count) | 8,3 MIPS; 2 000 000 salidas a C | **487,8 MIPS; 8 salidas** | ×59 |

Las demás variantes del bench no cambian (mixto 156 MIPS). Con el
intérprete solo, `cvt` corre a 4,4 MIPS: el camino completo de las
conversiones es caro. En nemu64-test la ganancia es chica porque lo que
queda interpretado son sus pruebas de COP0 (MTC0, TLB, ERET), traps y
CACHE; en un juego pesan las conversiones (cada `(int)f` es un TRUNC.W.S),
las copias no alineadas y las esperas por Count. En Zelda no se vería en
los campos/s todavía: donde no llega a 60 espera a la HLE gráfica del SPE.

El RESULTADO de esa corrida dice FALLO por 4 verificaciones de cobertura
(«corrió en nativo») que fallan a propósito en la variante `RARE_EXEC`;
las comparaciones de resultados pasaron en las tres variantes. Corregido
después (las verificaciones no aplican con esa opción).

### Recompilador: perfil por causa y salidas a C (2026-10-11)

Perfil nuevo (`src/cpu/rec_prof.h`, docs/rec-ppe.md «Perfil por causa»).
Primero las **cuentas** en qemu (no miden el Cell, pero dicen cuántas veces
pasa cada cosa), GoldenEye NTSC hasta la presa con `docs/pads/goldeneye-dam.pad`
(`SALSA64_REC_PROF=600`, 12 000 M ciclos = 128 s de consola), ventana de
campos 4800–5400 (en juego), por campo:

| | Antes (main 23a3289) | Después | |
|---|---|---|---|
| Entradas desde C al código generado | 1591 | 841 | −47 % |
| Llamadas a C desde el código | 2109 (CACHE 1006, COP0 690, FPU 160, E/S 254) | 2589 (COP0 2173, FPU 162, E/S 254) | los COP0 que antes corría el intérprete «a mitad» ahora son llamadas |
| Instrucciones interpretadas en el despachador | 13 872 (13 381 «a mitad», tras un COP0) | 507 (359 fallas de TLB al buscar código, 112 bloques sin compilar) | −96 % |
| Traducciones de la TLB (`vt_probes`) | ~14 000 (109 M en 128 s) | 360 revisiones selectivas (3,5 M en 128 s) | |
| Salidas por JR sin bloque a mano | 824 | 348 | la cadena de xhash y el prellenado de `ktlb` |
| Salidas por ERET/excepción | 37 | 397 | el ERET ya no lo corre el intérprete |
| Tiempo de qemu, 128 s de consola | 181,6 s | 165,4 s | orientativo |

Perfect Dark (intro, 64 s de consola), total por campo: 3163 cargas y 201
almacenamientos por C a direcciones mapeadas por la TLB (sus datos en una
página de 4 MB en 0x70000000), 770 CACHE, 200 COP0.

**Bloques, código y vaciados** (misma corrida de GoldenEye, 128 s; el
perfil por bloque pesa por ejecuciones):

| Estado | Bloques compilados | Instrucciones por bloque | Vaciados de la caché | Compilaciones por campo en la presa |
|---|---|---|---|---|
| main (antes) | 101 910 | 5,3 | 7 (16 MB) | 22 |
| + bloques desprendidos | 40 851 (28 206 enganchados de vuelta) | 5,3 | 3 | 0,7 |
| + superbloques | 131 228 | 11,8 | 17 (16 MB: ~900 bytes de PPC por bloque) | — |
| + rutinas de ktlb y caché de 24 MB | 26 243 | 11,8 | 2 | — |
| + camino lento compartido | 24 901 | 11,9 | 2 (10,3 MB al final; el segundo al cargar la presa) | 0,6 |

Perfect Dark con lo mismo (64 s): 16,1 instrucciones por bloque (5,2 de
memoria), 2 vaciados con 24 MB antes del camino lento compartido.

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN; paquete SLSA00088 «Salsa64
JIT» con main fusionado, `perfil` puesto; el A/B es con el mismo binario:
`jit-viejo` repite el comportamiento anterior). GoldenEye NTSC en la presa,
~2 min de juego cada uno (ventanas de 5 s del log, resumidas con
`tools/jit-perfil.py`; las escenas no son idénticas, por eso también el
tiempo por instrucción ejecutada):

| | Camino viejo (`jit-viejo`) | Nuevo | |
|---|---|---|---|
| Campos/s en la presa | 33–59, media **43,0** (72 % de NTSC) | 41–59, media **49,4** (83 %) | +15 % |
| MIPS (sin bucles de espera) | 37,5 | 50,6 | +35 % |
| Emulación por campo | 22,7 ms | 20,1 ms | |
| … nativo | 13,7 | 16,0 | igual por instrucción (15,7 vs 15,6 ns) |
| … llamadas a C | 0,6 | 1,9 | los COP0 que antes corría el intérprete «a mitad» |
| … intérprete | 2,8 | 0,8 | |
| … eventos | 0,5 | 0,5 | |
| … compilar | 0,3 | 0,3 | |
| … ktlb (revisar tras la TLB) | 3,8 | 1,0 | |
| … despachador | 1,0 | 0,0 | |
| Tiempo por instrucción ejecutada | 26,0 ns | 19,6 ns | −25 % |
| Bloques compilados / vaciados (≈110 s) | 67 988 / 2 | 35 913 / 2 | |
| Traducciones de la TLB | 132,7 M | 4,4 M | |
| Instrucciones interpretadas | 68,8 M | 7,7 M | |

Lo que dice el desglose nuevo: **el código nativo es el 80 % del tiempo y
cuesta ~50 ciclos del PPE por instrucción MIPS** (15,6 ns); los
superbloques no bajaron ese costo por instrucción. Lo demás ya es chico:
llamadas a C 1,9 ms (COP0 del refill: 2200 por campo), ktlb 1,0 ms (las
~360 escrituras de la TLB por campo: la sincronía selectiva compara 32
entradas y prellena con la búsqueda lineal; se puede traducir directo
desde la entrada escrita), intérprete 0,8. Los 2 vaciados en ~110 s son
por tamaño (23 MB de los 24): cada uno recompila el conjunto vivo. Siguiente
objetivo: el costo por instrucción del nativo (FPU por memoria, registros
entre bloques).

Sin regresión en los ligeros con el mismo paquete (un minuto o más de
juego cada uno, PS3 real): Zelda OoT 59,4 campos/s de media (99–100 %,
emulación 7,7 ms por campo, 58 ventanas), Super Mario 64 59,5 (100 %, 5,4
ms, 70 ventanas), Mario Kart 64 59,4 (100 %, 5,9 ms, 34 ventanas); los tres
con «salida normal» y «cierre: estado limpio». Con `jit-viejo`, la primera
corrida se quedó en negro: el preámbulo cortaba antes de las rutinas
compartidas nuevas (arreglado; el camino viejo queda validado en qemu con
nemu64-test y GoldenEye).

### FPU residente, sin microcódigo, COP0 con rutinas y caché por tramos (2026-10-11, rama jovial-jones)

Lo que cambia en el código nativo (docs/rec-ppe.md «Lo que el PPE hace en
microcódigo», «FPU residente», «COP0 sin el intérprete», «Desalojo por
tramos»), y por qué debería bajar los ~50 ciclos por instrucción:

| Pieza | Antes (por instrucción afectada) | Ahora |
|---|---|---|
| ANDI de MIPS | `andi.` (microcódigo: ≥ 11 ciclos, vacía el pipeline) | `rlwinm` (1) o `li`+`ori`+`and` (3) |
| Entrada de un bloque con COP1 | `andis.` (≥ 11) | `rlwinm`+`cmpwi` (2) |
| Salida a C | `mtcrf 0x38` (microcódigo, 9 micropalabras) | 3 `mtocrf` (3) |
| ADD.S en la franja, inexacto | ~40 palabras, 2 cargas de bits + 2 de valor, `andi.`, FCR31 leído, modificado y guardado, TwoSum o prueba de exponentes: ~90 ciclos (bench) | ~27 palabras sin cargas ni almacenamientos de FCR31: fabs×2, 6 fcmpu, op, frsp, 2 fsub, 3 fcmpu, crops; FCR31 se escribe al salir del bloque |
| ADD.D | TwoSum: 5 operaciones dependientes (~50 ciclos de latencia) | dos restas independientes y dos comparaciones (~23) |
| LWC1 → operación dependiente | `stw` a casa + `lwz`/`lfs` de casa (load-hit-store) | `lfsx` directo de la RDRAM al registro residente |
| MTC0 EntryLo (×2 por refill) | exec_one + x_sync (~2800 ciclos cada llamada en la PS3, 2200 por campo) | 3 palabras |
| TLBWR | exec_one + comparar 32 entradas + compactar `live[]` | rutina propia: solo esa entrada |
| ERET | exec_one + redirect + despachador (buscar el bloque, entrar) | rutina propia + `exit_jr` al bloque destino |
| Caché llena | vaciado completo: ~7000 bloques recompilados de golpe (campos de 52 ms) | desalojo del tramo más viejo (1/8 de 32 MB) |

En qemu las cuentas del perfil (llamadas a C, instrucciones interpretadas)
no cambian con la FPU residente: el dominio del camino rápido es el mismo
y solo cambia lo que cuesta cada operación, que qemu no mide. GoldenEye
hasta la presa (128 s, `goldeneye-dam.pad`, caché de 24 MB del host), por
campo, antes → con los ayudantes de COP0 y el desalojo por tramos:

| | FPU residente (9d1eba0) | + COP0 y tramos (d719987) | + saltos internos (15d6724) |
|---|---|---|---|
| Llamadas a C | 1590 (COP0 1236, FPU 120, E/S 235) | 1016 (COP0 661, FPU 120) | 1016 |
| Entradas desde C | 482 | 265 (el ERET sigue por `exit_jr`) | 264 |
| Salidas por redirect (ERET, excepción) | 218 | 0 | 0 |
| Instrucciones interpretadas | 578 | 578 | 578 |
| Llamadas al intérprete en 128 s | 10,4 M | 6,0 M | 6,0 M |
| Entradas en 128 s | 3,69 M | 2,03 M | 2,02 M |
| Ejecuciones de bloques en 128 s | 643 M | 637 M | **509 M** (−21 %: los saltos internos ya no cortan) |
| Bloques compilados / vaciados / desalojos | 26 581 / 2 / — | 20 819 / 1 / 10 | 17 308 / 1 / 8 |
| Instrucciones por bloque | 11,9 | 12,0 | 12,3 |

**Pendiente: la PS3 real** (A/B con el mismo paquete SLSA00093 y los
archivos `jit-fpu-memoria`, `jit-cop0-viejo`, `jit-sin-tramos`, `jit-viejo`
y `rdram-1m`), en la escena fija «GoldenEye, presa, junto al camión»
(referencia del log de main `salsa64-1080-5x-final.log`, campos 7100–7699:
40–46 campos/s, 20–23 ms de emulación por campo, 3 vaciados y 44 345
invalidados en la partida, peores campos de 52 ms), Perfect Dark en un
nivel, y un minuto de Zelda, SM64 y Mario Kart (sin regresión). El bench de
cpu-tests (SLSA00094) da el costo por instrucción de add.s, mov.s y cvt.

### Bench del intérprete tras traer la Fase 2: 21 → 14,7 MIPS (diagnóstico, 2026-10-10)

En la PS3, el bench del intérprete bajó en todas las variantes al traer la
Fase 2 (f18b392 → 59fba13; nop 34 → 19,5 MIPS, unos 70 ciclos más por
instrucción), sin cambios en interp.c. Lo revisado:
- `salsa64_cpu_run` es el mismo código byte a byte en los dos ELF (solo
  cambian direcciones y desplazamientos del TOC); en la Mac el bench da lo
  mismo con los dos árboles (~305 MIPS mixto).
- En la misma corrida de la PS3, el bench del recompilador no cambió
  (156,7 MIPS), y usa la misma `t_n64` y la misma RDRAM.
- `t_n64` sigue alineada a 128 (0x118ed580 → 0x118f5600) y `t_rdram`
  también (0x110ed500 → 0x110f5580); la tabla de saltos del intérprete está
  dentro del código (0x21180 → 0x21e90).
- Lo que solo el intérprete toca en cada instrucción: la búsqueda en la
  RDRAM de pruebas (`t_rdram + 0x1000`) y `sched->next`. Con la Fase 2,
  `sched->next` quedó en otra página de 4 KB que el CPU (0x118f6028) y esa
  página comparte los bits bajos con la de la búsqueda (0x110f6580). No se
  encontró por qué eso costaría 70 ciclos (en el ERAT de datos de 2 vías
  caben las dos); queda como hipótesis.

Prueba en la PS3 (cpu-tests SLSA00077 de e210aeb): «bench alineado» corre
las mismas variantes con el intérprete en una máquina aparte con estructura
y RDRAM alineadas a 64 KB. Resultado: **en ese binario los dos dan lo de
antes** (mixto 21,1 y 21,1 MIPS, nop 33,1 y 32,7), con `t_n64` en
0x121a2f00 y `t_rdram` en 0x119a2e80. Ahí `sched->next` también queda en
otra página de 4 KB que el CPU, con los mismos bits bajos que la de la
búsqueda, así que esa hipótesis queda descartada. La caída a 14,7 depende
de cómo queda armado el binario y no se encontró el mecanismo; si vuelve a
aparecer, comparar contra este binario.

## Líneas base de la Fase 2 (parte gráfica)

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN):

| Prueba | Commit | Resultado | Notas |
|---|---|---|---|
| `spu-jobs` (lib/spukit) | fffc4b5 | Ida y vuelta PPE → SPE → PPE de un trabajo vacío: **1,76 µs** (promedio de 1000; 0,04 µs en el SPE). 24 trabajos de 1 MB en 2 SPEs: 74,8 ms incluida la verificación en el PPE | El timbre por mailbox es barato frente a un cuadro (16,7 ms): un trabajo por cuadro no pesa |
| Frontend con gfxtest | fffc4b5 | `RESULTADO frontend: OK`, 13 de 13 puntos de la pantalla bien. Por tarea (29 triángulos, 9 dibujos, 3 texturas): **HLE en el SPE 0,12 ms**, PPE esperando al SPE 0,00 ms (el SPE termina antes del fin de la tarea), traducción al RSX en el PPE 0,05 ms (0,22 ms la primera ventana, con los destinos nuevos) | La emulación del CPU (intérprete) se lleva 74,6 ms por campo: 13,4 campos/s. La imagen es idéntica a la de RPCS3 |

En RPCS3 la misma tarea tarda 18 ms en el SPE: no concluyente.

**Zelda: Ocarina of Time en PS3 real** (2026-10-10, respaldo del usuario,
NTSC, fuera del repo; commit 5ca3815, `salsa64-fase2.pkg`, HLE en el SPE 0,
intérprete del CPU). Ventanas de 5 s del log del frontend; los tiempos de
etapa salen del decrementador del SPE (`gfx_result.t_*`):

| Escena | Campos/s | Emulación por campo | Por tarea gráfica | HLE en el SPE | Vértices | Estado (texturas) | Cargas a la TMEM |
|---|---|---|---|---|---|---|---|
| Logo de N64, campo ~150 | 17,5 (29 %) | 56 ms | 22 dibujos, 131 triángulos | 1,03 ms | 0,09 ms | 0,69 ms (0,47) | 0,12 ms |
| Intro nocturna, campo ~500 | 16,0 (27 %) | 62 ms | 59 dibujos, 320 triángulos | 10,3 ms | 1,12 ms | 4,51 ms (3,93) | 3,76 ms |
| Link en Epona bajo la luna, campo ~800 | 15,4 (26 %) | 64 ms | 136 dibujos, 1155 triángulos | 14,4 ms | 1,11 ms | 8,24 ms (6,82) | 3,77 ms |
| Título con el castillo, campo ~2000 | 15,4 (26 %) | 64 ms | 146 dibujos, 575 triángulos | 20,9 ms | 1,23 ms | 12,85 ms (11,34) | 5,66 ms |

- El cuello es el CPU (intérprete, 24 MIPS): la emulación se lleva 56–64 ms
  por campo. El PPE espera al SPE 13–19 ms por tarea, pero hay ~5 tareas
  por segundo, así que pesa menos que el CPU.
- En el SPE manda la decodificación de texturas (cada tarea vuelve a
  decodificar todo a RGBA8888) y luego las cargas a la TMEM: primero una
  caché de texturas entre tareas por contenido, después decodificación por
  formato con SIMD.
- La traducción al RSX en el PPE cuesta 0,2–0,4 ms por tarea.
- Misma escena en RPCS3: 16 ms por tarea en el SPE (no concluyente).

**Super Mario 64 en PS3 real** (2026-10-10, respaldo del usuario, USA/NTSC,
fuera del repo; commit d0bc043, paquete «Salsa64 F3D» SLSA00071, HLE de F3D
en el SPE 0, intérprete del CPU). Ventanas de 5 s del log del frontend:

| Escena | Campos/s | Emulación por campo | Por tarea gráfica | HLE en el SPE | Vértices | Estado (texturas) | Cargas a la TMEM |
|---|---|---|---|---|---|---|---|
| Arranque y logo, campos 0–~290 | 57,1 (95 %) | 3,0 ms | 7 dibujos, 1002 triángulos | 1,49 ms | 0,45 ms | 0,36 ms (0,34) | 0,09 ms |
| Paso del logo a la cara, campos ~290–460 | 32,7 (55 %) | 22,1 ms | 36 dibujos, 877 triángulos | 4,55 ms | 1,02 ms | 0,74 ms (0,52) | 1,86 ms |
| Cara de Mario, campos ~460–660 | 40,0 (67 %) | 23,8 ms | 57 dibujos, 1324 triángulos | 6,89 ms | 1,44 ms | 1,04 ms (0,65) | 3,07 ms |
| Selección de archivo | 60,1 (100 %) | 5,5 ms | 86 dibujos, 328 triángulos | 2,81 ms | 0,21 ms | 1,58 ms (1,06) | 0,56 ms |

- A diferencia de Zelda, el CPU solo pesa en la cara de Mario (la anima el
  CPU: 15 MIPS y 76 % en bucles de espera): 67 % de NTSC. El logo y la
  selección de archivo van a velocidad completa con el intérprete.
- En el SPE pesan las cargas a la TMEM de la cara (3 ms por tarea) y el
  estado de dibujo con muchas texturas (selección de archivo); la
  traducción al RSX en el PPE, 0,1–0,2 ms por tarea.
- gfxtest en F3D en la misma consola: 0,18 ms por tarea en el SPE, como en
  F3DEX2.

### Zelda OoT con el recompilador (2026-10-10)

Respaldo del usuario (`oot.z64`, NTSC, fuera del repositorio), frontend de
f416bee con el recompilador (registros fijos) y la HLE gráfica de la Fase 2
en el SPE 0. Ventanas de 5 s del log, desde el arranque:

| Tramo | Campos/s | Emulación por campo | HLE en el SPE por tarea | Triángulos por tarea |
|---|---|---|---|---|
| Logo de N64 | 58,1–59,9 | 3,8–7,6 ms | 1,2–10,7 ms | 132–459 |
| Intro | 54,3–59,8 | 9,3–11,7 ms | 13,9–22,1 ms | 300–1321 |
| Título y escenas con más dibujos | 41,4–50,3 | 11,7–14,5 ms | 20,0–29,8 ms | 900–1700 |
| Escenas ligeras (luego) | 58,9–59,9 | 7,1–8,9 ms | 10,3–14,2 ms | 165–400 |

Con el intérprete (Fase 2, misma consola): 15,4–17,5 campos/s y 56–64 ms
de emulación por campo. El CPU dejó de ser el cuello: donde no llega a 60,
el PPE espera a la tarea gráfica del SPE (caché de texturas pendiente en la
Fase 2). Lo interpretado (~190 000 instrucciones por segundo, 1,7 %) no
está desglosado: el intérprete termina el bloque antes de cada evento y
después de cada interrupción, y corre lo que el recompilador no toma
(código fuera de KSEG0, un salto en el delay slot de otro). Pendiente
medirlo por causa si llega a pesar.

## RSP a bajo nivel (Fase 6, prototipo; 2026-10-10)

Intérprete del microcódigo real (`src/rsp/lle`, docs/rsp-lle.md).

**Host** (Mac, clang nativo sin sanitizadores, `salsa64-run -c 1500000000`
= 16 s de consola de Zelda OoT, respaldo del usuario fuera del repo):

| Modo | Tiempo real | Instrucciones del RSP | Por instrucción |
|---|---|---|---|
| HLE (gráficos y audio) | 0,95 s | — | — |
| `--lle=all` (todo el microcódigo real) | 3,58 s | 211 M (1379 tareas, 153 000 por tarea, la más larga 542 000) | 12,5 ns (~40 ciclos del Mac) |

- nemu64-test: 2,0 s sin LLE, 3,1 s con `--lle` (5,9 M instrucciones del
  RSP en 240 000 arranques: la suite arranca el RSP por cada prueba).
- Super Mario 64, 9,6 s de consola: 161 M instrucciones (858 tareas).

**PS3 real** (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; paquete «Salsa64
LLE» SLSA00082 con main bd9fcd3 fusionado; Zelda OoT, respaldo del usuario,
`lle` = `all`: todo el microcódigo real en un SPE, el RDP en el PPE).
Ventanas de 5 s del log («rsp lle:»):

| Tramo | Tareas (trabajos) | Instr. por tarea | SPE por tarea | ns por instrucción | MIPS del RSP emulado | Campos/s |
|---|---|---|---|---|---|---|
| Logo y título (F3DZEX2 + nead) | 251 (251) | 39 000 | 8,8 ms | 226 | 4,4 | 55 |
| Título, más geometría | 207 (209) | 59 000 | 15,4 ms | 260 | 3,8 | 24 |
| Intro, campo de Hyrule | 153 (153) | 96 000 | 27,0 ms | 282 | 3,6 | 30 |
| Intro, escenas grandes | 97 (97) | 170 000 | 48,2 ms | 283 | 3,5 | 38 |
| Intro, fondos S2DEX (DMA de muchas filas) | 53 (224) | 814 000 | 91 ms | 112 | 9,0 | — |

- El RSP real hace ~62,5 M instrucciones/s: el intérprete en C en el SPE
  va **14–18 veces más lento** (3,5–4,4 MIPS) en el microcódigo gráfico y
  de audio, y 7 veces más lento donde casi todo es DMA. Mucho peor que la
  estimación previa (0,3–1×): el SPU no tiene unidad escalar ni predicción
  de saltos, el `switch` de despacho es un salto indirecto por instrucción
  y cada acceso a COP0 que vive en el bus es un DMA atómico de ~1 µs.
- Tráfico de COP0 por tarea (Zelda): SP_STATUS 180–430 lecturas, semáforo
  2–190 tomas y sueltas (F3DEX2 lo usa como cerrojo del DMA), DMA 60–150
  filas de entrada y 30–70 de salida, DP 25–60 lecturas, 0 reintentos
  atómicos. Solo los accesos atómicos son 1,5–2,5 ms de los 8,8 ms por
  tarea del primer tramo.
- Con el mismo paquete y sin `lle` (HLE por omisión): Zelda 106 s al
  100 % de NTSC, audio 0 faltas, «salida normal»; GoldenEye intro y
  menús al 99–100 % y **en el nivel (Dam) 36–50 campos/s** (60–84 % de
  NTSC: HLE gráfica en el SPE 8–10 ms por tarea con 3 000–3 700
  triángulos, CPU a ~40 MIPS con 24–46 % en bucles de espera; el audio se
  estira). Es la primera medición del nivel en la PS3 (las anteriores eran
  de la intro); no hay A/B con el paquete de main.

## Generador de fragment programs (2026-10-10)

**PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN), app SLSA00087 «Salsa64
fpgen» (136dff4; la tercera y cuarta corridas con ad14e4b, main 5a0a1a3
fusionado), respaldos del usuario fuera del repo, capturas cada 10 s
(`capturas`). Ventanas de 5 s del log («gfx: fragment programs:»).

| Juego | Escenas | Campos/s | Emulación por campo | Dibujos con programa generado por tarea | Programas generados | Cambios de programa por tarea |
|---|---|---|---|---|---|---|
| GoldenEye (238 s) | título, carpetas, Dam ~3 min | intro 56–60; Dam 34–55 | 3,5–5,7 ms; Dam 18–29 ms | 0; Dam 4–131 (de 56–351) | 4 (1 en la intro, 3 en Dam; 0 vaciados) | intro 2–6; Dam 7–23 |
| Zelda OoT (144 s) | título, Kokiri | 53–60 | — | 0 | 0 | 8–23 |
| Super Mario 64 (166 s) | castillo exterior | 54–60 | — | 0 | 0 | 8–10 |
| Mario Kart 64 (81 s) | título, menús, pista | 60 | — | 0 | 0 | 8–9 |

- Los «cambios de programa» cuentan también los precompilados (sin
  textura, una, dos): es el costo de siempre, ~20 palabras del búfer de
  comandos cada uno. El generador no agrega cambios en los juegos que no
  lo usan.
- GoldenEye es el único de los cuatro que necesita programas generados:
  los dos combiners de niebla por luz del nivel (3 términos: {}, {T0, C},
  {C}) y uno de la intro. La caché de 128 nunca se llena.
- No hay medición del tiempo del RSX por cuadro (sin contadores de GPU):
  la traducción en el PPE y el tiempo de emulación por campo no cambian
  frente a la medición anterior de Dam (36–50 campos/s); el cuello sigue
  en el CPU (~40 MIPS con 43 % en bucles de espera) y en la HLE del SPE
  (hasta 15 ms por tarea con 3 500–3 900 triángulos).
- Host (Mac, salsa64-run nativo, 7 500 M ciclos hasta Dam con
  `docs/pads/goldeneye-dam.pad`): 97 449 → 0 combiners aproximados.
