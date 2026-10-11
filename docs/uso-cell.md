# Uso del Cell y del RSX por juego

Qué tanto usa Salsa64 cada componente de la PS3 en cada juego, con datos de la
PS3 real (Slim CECH-3011A, HEN), y los límites del hardware contra los que
se compara. Los números salen de las líneas de estadísticas del log (cada 5 s,
en `build/ps3-logs/`); aquí se resumen como **mediana de las ventanas de 5 s
(p90 entre paréntesis)**. Los porcentajes de ocupación son aproximados: se
calculan como tareas por segundo × milisegundos por tarea.

Marcas: **[pub]** = dato de documentación pública del hardware; **[med]** =
medido en este proyecto; **[est]** = estimado a partir de otros datos.

## 1. Límites del hardware

### PPE (PowerPC de uso general del Cell)

| Dato | Valor | Origen |
|---|---|---|
| Reloj | 3,2 GHz | [pub] |
| Núcleos / hilos | 1 núcleo, 2 hilos SMT; ejecución en orden | [pub] |
| Caché | L1 32 KB instrucciones + 32 KB datos; L2 512 KB (compartida por los dos hilos) | [pub] |
| Load-hit-store | Una carga de una dirección escrita hace pocos ciclos no se reenvía desde el almacén: la tubería se detiene decenas de ciclos (orden de 30–40) | [pub] |
| Costo visto aquí | Bucle de prueba del recompilador, solo memoria: ~20 ciclos por instrucción por el load-hit-store propio del bucle; el intérprete antiguo gastaba ~250 ciclos por instrucción (load-hit-store, llamada con marco de pila por instrucción) | [med] (docs/perf-log.md) |
| SMT | Los dos hilos comparten el núcleo: lo que hace el hilo 1 le quita ciclos al hilo 0; por eso ningún hilo hace espera activa (siempre `sysUsleep`) | [med] (docs/hilos-ppe.md) |

### SPEs (6 disponibles para homebrew)

| Dato | Valor | Origen |
|---|---|---|
| Cantidad | 8 en el chip, 7 activos en la PS3, 1 reservado por el sistema: 6 para la aplicación | [pub] |
| Local store | 256 KB por SPE (código + datos + pila); sin caché, sin acceso directo a memoria principal | [pub] |
| Cómputo | SIMD de 128 bits, 128 registros de 128 bits, 3,2 GHz | [pub] |
| DMA | Por comandos del MFC, hasta 16 KB por transferencia, alineada a 16 B (preferible 128 B); latencia de cientos de ciclos, se oculta con doble búfer | [pub] |
| Presupuesto actual | HLE gráfica ~200 KB + pila ~56 KB; HLE de audio ~47 KB + pila ~208 KB | [med] (docs/memory-budget.md) |

### Memoria y buses

| Dato | Valor | Origen |
|---|---|---|
| XDR (principal) | 256 MB, 25,6 GB/s | [pub] |
| XDR utilizable por la aplicación | ~213 MB; con HEN se midieron 214 016 KB (209 MB) de memoria de usuario, menos los segmentos del ELF | [med] (docs/memory-budget.md) |
| EIB | Anillo del Cell que une PPE, SPEs, controlador de memoria y FlexIO; 4 anillos de 16 B; pico teórico de unos 200–300 GB/s según el modo de cuenta, ~25,6 GB/s por puerto de elemento | [pub] |
| RSX | NV47 (familia G70/NV40), 500 MHz, 256 MB de GDDR3 a ~20 GB/s | [pub] |
| Cell ↔ GDDR3 | El RSX lee y escribe su GDDR3 rápido; el PPE o un SPE que **lee** la GDDR3 la ve a decenas de MB/s (cifra pública: ~16 MB/s), porque va por FlexIO sin caché. Escribir hacia la GDDR3 es mucho más barato (varios GB/s) | [pub] |
| Costo visto aquí | Leer el búfer del RSX desde el PPE para una captura frena la presentación ~400 ms; por eso la imagen del VI se **copia en el PPE a memoria XDR mapeada** y el RSX la lee desde ahí | [med] (docs/hilos-ppe.md) |

## 2. Asignación de Salsa64

| Unidad | Qué corre | Fuente |
|---|---|---|
| **PPE hilo 0** (emulación, prioridad 1000) | CPU VR4300 con el recompilador a PowerPC (más intérprete para lo raro), planificador de eventos, MI/PI/SI/AI/VI, arranque y sondeo de las tareas del RSP hacia los SPEs, controles aplicados al PIF, copia de la imagen del VI (0,20 ms por campo), JPEG de Zelda en el PPE, ritmo (`pacer`) | `src/frontend/main.c`, docs/hilos-ppe.md |
| **PPE hilo 1** (presentación, prioridad 1001) | Traducción de la salida de la HLE a comandos del RSX (`salsa64_rdp_flush`, 0,2–1,0 ms por tarea), `rsxkit_frame_begin/end` (flip en vsync), DualShock 3, sysutil, escritura del guardado, log, capturas. El hilo de audio (salida al puerto de audio de la PS3, 16 bloques de 256 muestras a 48 kHz) también cuelga del PPE | idem |
| **SPE 0** | HLE gráfica: F3DEX2 y demás microcódigos, transformación de vértices, estado del RDP, cargas a la TMEM, hash y decodificación de texturas | `src/spu/gfx` |
| **SPE 1** | HLE de audio (nead, ABI1, naudio con MP3): mezcla, remuestreo, ADPCM, efectos | `src/spu/audio` |
| **SPE 2 a 5** | Libres. (El prototipo de LLE del RSP usa un SPE solo cuando se activa con `lle`.) Pensados para decodificación de texturas y posprocesado (MLAA, xBR) | docs/proyecto.md |
| **RSX** | Rasteriza lo que traduce el hilo 1: render interno a 3× (720p) o 4× (1080p), fragment programs generados en tiempo de ejecución, mezcla, z-buffer, escalado a la salida | `src/rdp`, `lib/rsxkit` |

La N64 va a 60 campos/s en NTSC, así que el presupuesto por campo es
16,7 ms (el PAL sería 20 ms; ningún log reciente es PAL). Los juegos no
generan una tarea gráfica por campo: Zelda emite 20 por segundo, SM64 y MK64 30, GoldenEye
hasta 60.

## 3. Tablas por juego

Todos los juegos: NTSC, ROM completa en memoria, caché del recompilador de
24 MB (los logs de hoy), render a 3× (720p). Datos de la corrida más
reciente de cada uno. El «PPE hilo 0 ocupado» descuenta de la «emulación» el
tiempo que el hilo duerme esperando al SPE 0 en bucles de espera
(`emulación − dormida × tareas por campo`), porque `ms por campo` del log
incluye esas dormidas de `sysUsleep`.

### Zelda OoT

Log: `build/ps3-logs/salsa64-jit-zelda.log`, 2026-10-11 (compilación con
perfil), 57 ventanas de 5 s entre 10 s y 294 s (17 396 campos), con
menús, la aldea Kokiri y cambios de sala.

| Componente | Medido |
|---|---|
| Velocidad | **59,8 campos/s** (mín. 55,7), 100 % de NTSC (p90 100 %) |
| PPE hilo 0 | Emulación 8,2 ms por campo (8,7) + 8,4 de espera del ritmo = 16,6. **Ocupado ≈ 6,5 ms (39 % de 16,7**; p90 42 %). Desglose del perfil JIT: código nativo 4,91, llamadas a C 0,26, intérprete 0,10, eventos 2,69 (casi todo dormida del sondeo del SPE 0: RSP 2,67), compilar 0,02 (p90 0,32; picos de 1,3 al entrar a salas nuevas), despachador 0,06, copia del VI 0,20. 19,0 MIPS emulados, 80 % de los ciclos en bucles de espera |
| PPE hilo 1 | Traducción al RSX 0,77 ms por tarea × 20 tareas/s ≈ **1,6 %**; flip 6,0 ms por campo (espera de vsync, no es CPU); hilo de audio **2,0 %** (máx. 3,7) |
| SPE 0 | **6,25 ms por tarea** (7,6; máx. 8,95) × 20 tareas/s ≈ **12 %** (p90 15 %, máx. 18 %). Por tarea: 150 dibujos, 1275 triángulos (p90 1712; máx. 3926). Parte medida: vértices 1,55 ms, estado 1,12, cargas a la TMEM 1,31, texturas 0,00 (la caché las reusa) |
| SPE 1 | **2,07 ms por tarea** (máx. 4,5) × 59,6 tareas/s ≈ **12 %** (p90 16 %) |
| RSX | Sin medición. Indicio: flip 6,0 ms (p90 9,2) = el hilo 1 espera el vsync, la cola de campos está en 0,00 (máx. 0 en las ventanas): el RSX no es el límite |
| JPEG (fondos prerrenderizados) | 75 tareas, 300 macrobloques en 67,9 ms del PPE hilo 0 (~0,9 ms por tarea), una vez por sala, unos 4 campos de CPU |
| Memoria | XDR ≈ 140 MB [est]: ROM de 32 MB + ~108 de base (ver abajo). Caché del recompilador usada: 14,1 MB de 24 [med] |
| **Cuello de botella** | Ninguno: 100 % de NTSC con 61 % de holgura en el PPE hilo 0. El juego mismo va a 20 fps |

### Super Mario 64

Log: `build/ps3-logs/salsa64-jit-sm64.log`, 2026-10-11, 69 ventanas entre 10 s
y 353 s (21 080 campos).

| Componente | Medido |
|---|---|
| Velocidad | **59,8 campos/s** (mín. 56,0), 100 % de NTSC |
| PPE hilo 0 | Emulación 5,5 ms por campo (6,4) + 11,1 de espera del ritmo. **Ocupado ≈ 4,1 ms (25 %**; p90 30 %). Perfil JIT: nativo 3,00, llamadas a C 0,16, intérprete 0,08, eventos 2,08 (RSP 2,06, dormida), compilar 0,04, despachador 0,05. 12,1 MIPS, 87 % en bucles de espera |
| PPE hilo 1 | Traducción 0,27 ms por tarea × 30/s ≈ **0,8 %**; flip 2,0 ms; hilo de audio **2,0 %** (p90 3,1) |
| SPE 0 | **3,10 ms por tarea** (3,8; máx. 5,2) × 30 tareas/s ≈ **9 %** (p90 11 %, máx. 15 %). 57 dibujos, 916 triángulos por tarea (1203). Vértices 1,27, estado 0,29, TMEM 0,37 |
| SPE 1 | **1,38 ms por tarea** (máx. 3,3; 3,7 en la más larga) × 59,8/s ≈ **8 %** (p90 19 %) |
| RSX | Sin medición. Flip 2,0 ms (p90 6,3): el hilo 1 espera poco |
| Memoria | XDR ≈ 116 MB [est]; caché del recompilador usada 19,9 MB de 24 (27 581 bloques) |
| **Cuello de botella** | Ninguno. El más ligero de los tres de nivel 1 en CPU; la caché del recompilador es la que más se acerca a su tope (19,9 de 24 MB) |

### Mario Kart 64

Log: `build/ps3-logs/salsa64-jit-mk64.log`, 2026-10-11, 33 ventanas entre
10 s y 173 s (10 393 campos): menús y pistas.

| Componente | Medido |
|---|---|
| Velocidad | **59,8 campos/s** (mín. 56,0), 100 % de NTSC |
| PPE hilo 0 | Emulación 6,0 ms por campo (6,6) + 10,5 de espera. **Ocupado ≈ 4,7 ms (28 %**; p90 31 %). Nativo 3,42, llamadas a C 0,20, intérprete 0,08, eventos 2,11 (RSP 2,10), compilar 0,08 (p90 0,29). 14,7 MIPS, 84 % en bucles de espera |
| PPE hilo 1 | Traducción 0,36 ms × 30/s ≈ **1,1 %**; flip 3,1 ms; audio **1,85 %** |
| SPE 0 | **2,89 ms por tarea** (3,9; máx. 5,1) × 30/s ≈ **8 %** (p90 11 %, máx. 15 %). 82 dibujos, 397 triángulos (671; máx. 3429). Vértices 0,34, estado 0,57, TMEM 0,93, texturas 0,10 |
| SPE 1 | **1,93 ms por tarea** (máx. 3,4) × 59,8/s ≈ **12 %** |
| RSX | Sin medición. Flip 3,1 ms |
| Memoria | XDR ≈ 120 MB [est]; caché del recompilador 16,4 MB de 24 |
| **Cuello de botella** | Ninguno. Con 4 jugadores o más triángulos aún no se probó |

### GoldenEye (la presa)

Log: `build/ps3-logs/salsa64-jit-nuevo.log`, 2026-10-11 (recompilador de hoy,
camino nuevo), 17 ventanas entre 28 s y 108 s (la presa; antes de 27 s son
el menú y el arranque, 3 ventanas, 59,4 campos/s con el CPU a 20 %). El
camino anterior, en `salsa64-jit-viejo.log`, da 33–60 campos/s con picos mucho más bajos;
la comparación completa está en docs/perf-log.md (43,0 → 49,4 campos/s,
+35 % MIPS).

| Componente | Medido |
|---|---|
| Velocidad | **46,6 campos/s** (p90 57,8; mín. 40,6) = **78 % de NTSC** (p90 97 %). Las ventanas más lentas son las de más carga de CPU (50–60 MIPS emulados) |
| PPE hilo 0 | Emulación 21,1 ms por campo (22,5; máx. 24,2), sin espera de ritmo (0,2). **Ocupado ≈ 20,6 ms = 123 % del presupuesto de 16,7 ms: saturado.** Perfil JIT: nativo **16,6 (79 %)**, llamadas a C 1,85, intérprete 0,75, eventos 0,13, compilar 0,16 (máx. 1,18), despachador 0,00. 50,8 MIPS emulados, solo 28 % del tiempo en bucles de espera. Casi todos los bloques (35 890 de 35 913) están mapeados por la TLB, no en KSEG0 |
| PPE hilo 1 | Traducción 0,99 ms por tarea × 45 tareas/s ≈ **4,4 %** (p90 5,9 %); flip 0,6 ms (casi no espera: no hay campos de sobra); hilo de audio **4,1 %** |
| SPE 0 | **9,15 ms por tarea** (11,6; máx. 15,9) × 45 tareas/s ≈ **44 %** (p90 53 %, máx. 59 %). 189 dibujos (268), 2366 triángulos (3195; máx. 3396). Vértices 1,31, estado **4,26** (5,6; máx. 8,0), TMEM 1,14. El PPE no espera al SPE (0,00 ms) |
| SPE 1 | **3,64 ms por tarea** (máx. 5,5; 6,5) × 23 tareas/s ≈ **9 %** (el juego pide ~0,5 tareas de audio por campo y la emulación va lenta; el audio llega a «emulación al 79 %» y se estira) |
| RSX | Sin medición. Flip 0,6 ms (p90 4,8): el hilo 1 casi no espera; no hay indicio de que el RSX sea el límite |
| Memoria | XDR ≈ 120 MB [est]; caché del recompilador usada 23,4 MB de 24 (2 vaciados, 33 819 bloques invalidados): casi llena |
| **Cuello de botella** | **PPE hilo 0** (código nativo emulando el CPU, 79 % del tiempo). Segundo: el SPE 0, con 44 % de ocupación media pero hasta 16 ms por tarea en escenas de ~3400 triángulos (no frena, porque se solapa con el CPU) |

### Perfect Dark

Log: `build/ps3-logs/perfectdark-recorte.log` (también en
`salsa64-evidencia/2026-10-11-goldeneye-lineas-azules/`), 2026-10-11 09:27,
39 ventanas hasta 205 s: intro y menús, **antes** de las compilaciones con
perfil (caché de 16 MB, sin líneas de perfil JIT). **No hay un nivel medido.**

| Componente | Medido |
|---|---|
| Velocidad | **59,3 campos/s** (mín. 45,6) = 99 % de NTSC |
| PPE hilo 0 | Emulación 12,6 ms por campo (17,9; máx. 20,3) + 4,2 de ritmo. **Ocupado ≈ 11,0 ms (66 %**; p90 95 %, máx. 121 %). 32,4 MIPS (p90 58), 67 % en bucles de espera. Sin desglose JIT |
| PPE hilo 1 | Traducción 0,42 ms × 30/s ≈ **1,3 %** (la ventana más pesada: 2,3 ms por tarea); flip 7,1 ms; audio **2,0 %** |
| SPE 0 | **4,14 ms por tarea** (7,8; máx. 8,7) × 30/s ≈ **12 %** (p90 25 %, máx. 40 %). 75 dibujos, 848 triángulos (1573; máx. 3845) |
| SPE 1 | **5,45 ms por tarea** (máx. 9,7) × 29,8/s ≈ **16 %** (el más alto de todos: naudio con MP3, código escalar). Coincide con el ~17 % del 2026-10-10 |
| RSX | Sin medición |
| Memoria | XDR ≈ 140 MB [est]: ROM de 32 MB + ~108 |
| **Cuello de botella** | PPE hilo 0 en las ventanas pesadas (hasta 76 % de NTSC); SPE 1 el segundo en importancia (MP3) |

### Banjo-Kazooie y Conker

**No hay un log reciente con estadísticas de estos dos** (ni en `build/ps3-logs/` ni
en `salsa64-evidencia/`). Lo único medido está en docs/perf-log.md y docs/estado.md
(2026-10-10, antes del recompilador de hoy):

| Dato | Banjo-Kazooie | Conker |
|---|---|---|
| Campos/s | sin dato | 57 en escenas de ~7600 triángulos (paquete SLSA00084) |
| SPE 1 | 2,92 ms por tarea × ~30/s ≈ **9 %** (ventanas 1,24–3,65) | 1,56 ms × 30/s ≈ **5 %** |
| Hilo de audio | 1,9 % | 1,7 % |
| Emulación de audio | 100 % (mín. 88 %) | 100 % |
| Retraso del destino | 1,5 campos | 2,5 campos |
| PPE hilo 0, SPE 0, RSX | sin dato | sin dato |
| Memoria | XDR ≈ 124 MB [est] con ROM de 16 MB | XDR ≈ 172 MB [est] con ROM de 64 MB, la más alta |
| Cuello de botella | sin dato | sin dato |

### Menú

Sin datos: el menú corre sin hilo de emulación, sin SPEs y no escribe líneas
de estadísticas. Memoria: 2,1 MB de búferes de UI (docs/memory-budget.md).

### Memoria: lo fijo y lo que cambia por juego

Base común (docs/memory-budget.md, valores «previstos» salvo los marcados): caché
del recompilador 24 MB (la tabla de memory-budget dice 16; las corridas de hoy usan 24), estado del
recompilador ~8 (medido 6,3 + 1,6 de bloques desprendidos), RDRAM emulada 8,
caché de texturas 8 y salida de la HLE 20 (ambas mapeadas al RSX), búfer de
comandos del RSX 16, copias del VI 4,4, audio 0,3, menú 2,1, código, pila y
heap 16: **≈ 108 MB**. Se suma la ROM entera (8 MB SM64; 12 MB MK64 y GoldenEye;
16 MB Banjo; 32 MB Zelda y Perfect Dark; 64 MB Conker) para un total de
116–172 MB de ~213 MB disponibles. Todo «previsto»: el total medido con
`sizeof` solo existe para el estado del recompilador. Memoria del RSX
(GDDR3): 11 MB de framebuffers de 720p + 2,8 del framebuffer 2D + destinos de
render (hasta 27 MB a 3×, 44 MB a 4× / 1080p) ≈ 41–58 MB de 256 MB; sin
medición por juego.

## 4. Lo que no se mide hoy y cómo medirlo

| Falta | Por qué importa | Cómo medirlo (propuesta, sin implementar) |
|---|---|---|
| **Tiempo del RSX por cuadro** | Hoy el flip (espera del vsync) es solo un indicio: no distingue «el RSX acabó pronto» de «el hilo 1 llega tarde». A 1080p y 4× el relleno puede pasar a ser el cuello | Escribir una **etiqueta del RSX** (`rsxSetWriteBackendLabel`/semáforo de GCM) al principio y otra al final de cada cuadro, y el RSX anota el reloj de GPU (`GCM_TIMESTAMP`/«report» de tiempo) en ambos; el PPE lee las dos etiquetas desde memoria mapeada (XDR, lectura barata) y el log suma «RSX x ms por cuadro» |
| Coste por tipo de dibujo del RSX | Saber si pesan los fragment programs generados, el z-buffer o el escalado | Misma técnica por tareas gráficas (una etiqueta por juego de búferes, que ya hay) para tener ms por tarea en el RSX, y un modo de prueba con el render a 1× contra 3× |
| Uso real de la **EIB** y del ancho de banda de XDR | Hoy no hay dato de si el DMA de los SPEs o la copia del VI compiten | Contadores de rendimiento del Cell (PMU/«Performance Monitor» del EIB, accesible desde PSL1GHT con permisos de HEN) o, más barato, bytes movidos por DMA contados en cada programa SPE y registrados en el log por tarea |
| **Ocupación de los SPEs**, no solo ms por tarea | Hoy se deduce multiplicando tareas/s × ms/tarea; no cuenta la espera por DMA ni la cola | En cada SPE, leer el decrementer al despertar y al dormir (`spu_read_decrementer`) y acumular «tiempo ocupado / tiempo total» y «tiempo esperando DMA» en el bloque de control; el PPE lo imprime con el mismo bloque de estadísticas |
| **Ocupación real de los hilos del PPE** | Hoy se estima con `emulación − dormida`; el hilo 1 ni siquiera mide su traducción dentro de «presentación» (siempre 0,0 ms) | Contar con `sysGetSystemTime` las dormidas y los trabajos de cada hilo y log «hilo 0: x % ocupado, hilo 1: y %»; el contador de ciclos de PMU del PPE da la ocupación del núcleo entero |
| Load-hit-store y fallos de caché del código generado | El 79 % de GoldenEye es código nativo; no se sabe cuánto es LHS o fallos de L2 | Contadores del PMU del PPE (ciclos de detención por LHS, fallos de L1/L2) sobre una corrida; o la muestra de PC por temporizador con volcado de histograma de bloques |
| Banjo-Kazooie, Conker y un nivel de Perfect Dark con el recompilador de hoy | Sin esto la tabla está incompleta | Una corrida de 2–3 minutos por juego con la compilación con perfil (`perfil JIT`), guardando el log en `build/ps3-logs/` |
| Memoria **medida** por juego | La tabla de memoria es «previsto» | Registrar al arrancar cada juego el uso real de la ROM, la caché de texturas y los destinos creados (ya hay contadores de destinos y de bloques) y la memoria de usuario libre de `sysMemoryGetUserMemorySize` |
