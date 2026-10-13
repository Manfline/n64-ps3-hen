# Estado

## 2026-10-12 (noche) — Recompilador: perfil persistente por juego y zona caliente, medidos en la PS3 (rama `fervent-noyce-db464c`, paquete SLSA00104 «Salsa64 perfil»)

Objetivo: la localidad del código del recompilador (GoldenEye en la presa y
la segunda escena de la intro de Perfect Dark, limitadas por el CPU: ~47
ciclos del PPE por instrucción MIPS con ~7 MB de bloques vivos contra 512 KB
de L2). Hecho y medido; diseño en docs/rec-ppe.md («Perfil persistente y
bloques calientes» y «Siguientes pasos»), números en docs/perf-log.md.

- **Perfil por juego** (`src/cpu/rec_profile.c`): cada bloque cuenta sus
  entradas (cuatro palabras a la entrada, no en la vuelta de un bucle), la
  cuenta se suma entre campos a una tabla por (dirección, huella de las
  instrucciones, cantidad) y se guarda en `perfiles/<juego>.prf` al cerrar
  el juego (también al volver al menú) y cada 2 min por trozos desde el
  hilo de presentación. Identidad por contenido: un bloque reescrito en la
  misma dirección es otra entrada; los gemelos por FR comparten la suya.
  El archivo lleva código de cartucho y CRC de la ROM, no contiene ni un
  byte del juego ni código traducido (direcciones, huellas de un solo
  sentido, cuentas y banderas), y si falta, está corrupto o es de otro
  juego el perfil arranca vacío. Entre sesiones, una sesión larga hace
  decaer a la mitad lo que no corrió; una corta solo suma.
- **Zona caliente**: 1 MB al principio de la caché en dos mitades que se
  turnan; al cargar el perfil se marcan las entradas de mayor puntaje
  hasta tres cuartos de la zona y sus bloques se compilan ahí.
- **Sitios de salida de los bloques calientes** (pedido de main para los
  pasos siguientes): cada salida con destino fijo cuenta cuántas veces se
  pasa; de ahí salen tomado/no tomado de cada condicional y los dos
  sucesores más frecuentes de cada bloque, con la huella del bloque del
  destino. Solo se registra.
- Interruptores: `perfil-activo` (apagado por omisión; en la medición de
  abajo el paquete lo traía encendido y `sin-perfil` lo apagaba) y
  `perfil.cfg` (`caliente = KB`, `saltos = si|no`), documentados en
  docs/uso-ps3.md; en el host,
  `salsa64-run --profile` y `SALSA64_REC_HOT_KB`/`SALSA64_REC_SITES`.

Validado: unitarias host 440/440 y ppc64 con recompilador 440/440 (plain,
pin, caché chica; suite `perfil` nueva: tabla, mezcla, marcado, archivo con
corrupción e identidad, y con el recompilador contador, zona caliente,
huella distinta, rotación de mitad y sitios); nemu64-test 433 fallas (línea
base) con perfil nuevo y cargado, también con caché de 3 MB y zona de 256 KB
(rotaciones); `--lockstep` sin diferencias en nemu64-test (las dos cachés),
vitest y Perfect Dark 3 G ciclos con el perfil cargado; test-roms en verde;
build de la PS3 sin avisos. GoldenEye hasta la presa en lockstep con el
perfil cargado: en curso con el binario anterior al formato 2 (se repite con
el final antes de integrar).

**Medido en la PS3 (el autor, una sola ejecución de la app):** la zona
caliente **no da ganancia**: GoldenEye en la presa 15,54 → 15,80 ns por
instrucción de nativo entre la primera sesión (perfil vacío) y la segunda
(762 bloques calientes juntos, 1592 sitios), Perfect Dark 12,73 → 12,39:
ruido. Contar cuesta ~5 % en GoldenEye (base con `sin-perfil` 14,82 ns de
nativo y 17,15 en total contra 15,54 y 18,19). SM64, Zelda y Majora sin
regresión (59–60 campos/s). La conclusión es que lo que manda no es dónde
está cada bloque sino cuántas líneas toca por instrucción: en qemu, los 25
bloques más ejecutados de GoldenEye son 21,3 palabras de PPC por instrucción
MIPS (9,9 en la sección que se ejecuta, 11,4 en caminos lentos y salidas);
cada acceso a datos mapeados cuesta ~10 palabras calientes y ~35 frías, y
la entrada de un bloque 16–20. Las palancas (un camino lento por bloque,
escritura de sucios por máscara, camino de la ktlb en línea, entrada más
corta) y los usos del perfil (pistas de salto: en GoldenEye 612 de 931
condicionales se toman más del 80 % de las veces y 184 menos del 20 %;
sucesor contiguo; traducción por adelantado y JIT por niveles en el hilo 1
del PPE, que duerme el 85 %) están en docs/rec-ppe.md, «Siguientes pasos».

Decidido con main: el perfil queda **apagado por omisión** (sin el archivo
`perfil-activo` no hay tabla, contador ni zona caliente: costo cero en el
hilo 0); se enciende para recoger datos o cuando los pasos siguientes lo
usen. Anotada la idea de contar gratis por muestreo desde el hilo 1 (lee
cada N µs el bloque en curso) y dejar la instrumentación solo para sitios y
sucesores. `caliente = 256` (lo que cabría en la L2 con los datos) quedó sin
medir. Los perfiles de la consola están en `build/ps3-logs/prf/` de la rama
(y en main, `build/ps3-logs/perfil-2026-10-12/`); con las huellas main vio
404 bloques idénticos en tres o más juegos (8–12 % del tiempo de cada uno;
Zelda y Majora comparten 973, ~31 %; GoldenEye y Perfect Dark 807, ~13 %):
libultra, base para las funciones nativas.

## 2026-10-12 (noche) — v0.2.0 publicada y censo de compatibilidad

Publicada la **v0.2.0** (pre-release, etiqueta en ad72ebf, con
`salsa64-v0.2.0.pkg`; notas en el release y en `CHANGELOG.md`). Pasada final
en la PS3 real sin cuelgues: Paper Mario, Donkey Kong 64 (zipper incluido),
Perfect Dark y Zelda.

Censo con el candidato (PS3 real, respaldos NTSC del autor), para después de
v0.2.0:

| Juego | Resultado | Pista |
|---|---|---|
| Zelda OoT, SM64, MK64, Majora, Paper Mario, Mario Party 1 y 3 | bien | Majora: iluminación de algunas zonas |
| Donkey Kong 64 | 100 % | tirones al entrar a zonas nuevas (traducción) |
| Perfect Dark, GoldenEye, Conker | jugables | CPU en las escenas pesadas; Conker: sombras cuadradas, escena pesada de la intro a 49–55 (combiner: guardar 2–4 resultados o forzar en línea lo caliente) |
| Star Fox 64 | bien | falla breve en el logo inicial (falta descripción exacta); la estela del menú es del juego |
| Super Smash Bros. (y Smash Remix) | se congela en el primer KO | causa en la entrada de arriba (z-buffer como color image) |
| Kirby 64, Pokémon Stadium 2 | ~97–100 %, texturas con ruido o ajedrez | estado de Stadium 2 en la ranura 0 |
| Pokémon Snap | jugable, texturas estiradas | ¿recorte del RSP, como MK64? |
| Doom 64 | ~40 % | ~4,5 M instrucciones interpretadas por segundo: algo que el recompilador no traduce |
| Banjo-Tooie | negro, sin tareas gráficas | arranque (CIC 6105) o protección de Rare |
| F-Zero X | negro, CPU quieto desde el arranque | CIC 6106, entrada en 0x80067000 |
| Diddy Kong Racing | negro | microcódigo propio de Rare sin HLE |
| Indiana Jones | negro, 18 % | microcódigo de Factor 5 (LLE del RSP) |
| Resident Evil 2 | parpadeo y texturas rotas desde el inicio | estado en la ranura 0 |

Además: cadencia de la presentación (se pierde un campo de cada dos cuando
la emulación va sobrada; 5,8 % saltados en la intro de Perfect Dark a 5×),
las uniones azules de GoldenEye y el rompecabezas de Banjo.

## 2026-10-12 (noche) — Smash: el KO se cae con las copias a la RDRAM (acotado, pendiente; rama `infallible-wozniak-aa210c`)

Se acotó en la PS3 (con fbfx.cfg = 1 no se cae; en v0.1.0 ya pasaba
con Smash Remix). En el host nativo se reproduce con `--gfx-frame` (demo,
las tareas gráficas paran en el campo 1424): sin el espejo del framebuffer
se cuelga igual; sin las copias esperadas corre toda la demo (12 987
tareas); saltando solo la copia del destino `0a3940/320`, también. Ese
destino es el z-buffer: en el KO el juego hace `ZCLEAR 0a3940` +
`TARGET 0a3940/320` (limpia el z con un FILLRECT como color image), dibuja
128 triángulos ahí (la silueta) y el CPU lee esa memoria cada campo (273
lecturas por campo desde `800c7840`, en +0x23f00). La HLE convierte el
relleno en ZCLEAR (solo profundidad), la color image nunca recibe el
relleno, y la copia esperada escribe el contenido viejo de la superficie
más los triángulos (y alfa siempre 1); el hardware deja 0xFFFC con la
silueta encima. Arreglo pendiente: con ZCLEAR sobre una color image del
mismo destino, rellenar también la superficie de color con el color de
relleno (host y PS3) y no forzar alfa 1 en la copia; prueba en test_raster.

## 2026-10-12 (noche) — Star Fox 64: la «estela» del menú es del juego (rama `infallible-wozniak-aa210c`)

En el menú principal, al elegir una opción, las etiquetas se ven repetidas
(«se replican muchas veces»). No es un error: en cada cuadro de esa
animación el primer dibujo es un rectángulo a toda pantalla (8..312 ×
8..232) sin textura, combiner G_CC_PRIMITIVE con PRIM = 0x0000002c (negro,
alfa 17 %) y mezcla alfa, sin borrar antes el framebuffer: oscurece un 17 %
lo que había en ese búfer (el cuadro de hace tres, triple búfer) y dibuja
encima: el afterimage de ese menú. En el host se reproduce igual (3–4
copias de «Main Game» apagándose). GLide64 en mupen64plus no lo muestra
porque no conserva el framebuffer entre cuadros; el hardware sí, como
nosotros.

## 2026-10-12 (noche) — Interbloqueo de la PS3 con más de SETS tareas gráficas en un campo (Paper Mario, Mario Party, Mario Party 3; rama `infallible-wozniak-aa210c`)

Salió en el candidato de v0.2.0: PM se queda quieto al pulsar START
en el telón (campo 688), MP tras los logos (681) y MP3 (516): «RSP ocupado»,
MIPS 0, «presentación en espera», y en esa ventana «1 esperas por un juego
de búferes». En el host nativo (intérprete + HLE) los tres pasan con las
pulsaciones exactas; en qemu con el recompilador también: es de los hilos
de la PS3.

Causa: `acquire()` (hilo de emulación) espera sin tope a que la tarea
n − SETS + 1 esté traducida para reusar su juego de búferes; la
presentación solo traduce hasta las tareas «visibles» que la emulación
publica al cerrar cada campo (df45ac1, temblor de Banjo). Si un juego manda
más de SETS (3) tareas gráficas dentro de un campo, la emulación se bloquea
en acquire sin cerrar el campo, no publica las visibles y la presentación
nunca traduce esa tarea. Desde 98def88 las tareas ya no se frenan 200 ms en
`done()`, así que es más fácil que caigan varias en un campo (por eso PM
pasaba por la mañana). Arreglo igual al de 98def88 para las copias:
`acquire` publica en `rdp.set_waiting` la tarea que espera y despierta a
presentación; `salsa64_rdp_flush` traduce hasta ella aunque no sea visible.
Sin prueba unitaria (son los hilos de la PS3); por confirmar en la consola.
## 2026-10-12 — El desenfoque de Perfect Dark desde el destino del RSX; copias esperadas a 200 ms y el «estado» del SPE al triple (rama `wonderful-jepsen-ebf467`, paquete SLSA00101 «Salsa64 desenfoque»)

- **Desenfoque (docs/framebuffer.md, «Texturas desde un destino sin
  decodificar»):** las 180 franjas por tarea del cuadro anterior se
  reconocen como de su framebuffer (un `LOADBLOCK` del que se muestrea una
  fila toma el paso de `SETTIMG`) y la HLE no las decodifica
  (`GFX_TEX_TARGET`); el consumidor las lee del RSX a la resolución interna
  o, si no puede, las decodifica en el PPE. PS3 real: PD a 59,7–60,2
  campos/s en la azotea con 0,0–0,5 texturas decodificadas por tarea (antes
  133–181) y el desenfoque «sutil pero visible»; el baile del Espantapájaros
  de Majora's Mask sale por el mismo camino (96 texturas por tarea leídas del
  RSX), «sublime».
- **Copias esperadas a 200 ms (98def88, en main):** desde que la presentación
  traduce solo las tareas «visibles» (temblor de Banjo-Kazooie), la tarea
  cuya copia a la RDRAM espera la emulación no se traducía hasta el tope de
  `RB_WAIT_US`: la intro de PD a 4,9 campos/s en la PS3 (también con main
  sin esta rama). `salsa64_rdp_flush` traduce siempre esa tarea.
- **Pila del SPE 0 y combiner:** la pila no alcanzaba en main (holgura
  −3 008 bytes: docs/perf-log.md, «Pila del SPE 0»); el arreglo compiló el
  combiner con `-Os`, y en la PS3 eso triplicó el «estado» del SPE en todos
  los juegos. Ahora el resultado del combiner se memoriza con sus entradas
  (b8d9041), S2DEX va con `-Os` y el directorio de la caché de texturas
  baja a 896 entradas: holgura 3 520 bytes, comprobada en el build.
- **Pendiente: el «estado» de Conker.** En la escena pesada de su intro
  (campos 4400–4999) va a 49,3–55,4 campos/s con el SPE en 30,4–34,9 ms
  por tarea y el «estado» en 4,1–7,6 ms; antes del `-Os` iba a 51,8–58,3
  (SPE 28,7–31,1, estado 2,5–3,9) y con el `-Os` sin memorizar a 44,1–50,5
  (SPE 33,7–39,0, estado 7,5–11,8). Conker cambia de combiner o de PRIM/ENV
  entre dibujos más que PD y un solo resultado guardado no alcanza.
  Opciones, a medir en la PS3: (a) guardar 2–4 resultados (~0,4 KB cada
  uno); (b) dejar `-Os` pero forzar inline lo caliente del combiner
  (`p_term`, sumas de polinomios): en el SPU cada llamada sin pista de
  salto cuesta ~18 ciclos.
- **PS3 real con todo (ece6049):** Zelda «sublime» (59,8 campos/s, estado
  1,38 ms), GoldenEye igual que antes (limita el CPU en Dam), PD a 59,8 en
  la azotea con el estado en 0,70 ms; sus tirones son campos saltados por la
  presentación (5,8 % contra 0,9 % del paquete anterior, con la emulación a
  60), que vinieron con 99686a7 y siguen en la rama de los arreglos
  gráficos (docs/perf-log.md, «Pila del SPE 0»).

## 2026-10-12 (noche) — Donkey Kong 64: logos entrelazados y «zoom» en el juego (presentación; rama `infallible-wozniak-aa210c`)

Salió en la PS3 real con la rama del recompilador (DK64 al 100 %):
1. **Logos (campos ~100–460)**: el VI muestra 640 × 237 por campo con
   stride 2560 (VI_WIDTH 1280) y el origen alterna dos líneas seguidas: un
   búfer de 640 × 480 del que cada campo muestra una línea de cada dos. En
   el host el destino es `034000/640` (y `0ca000/640`) de 480 filas. La
   presentación tomaba 237 líneas seguidas desde el origen: la mitad de
   arriba estirada, y saltando una línea entre campos. Ahora
   `rdp_targets_present` ve que el stride es k veces la línea del destino y
   presenta k × 237 líneas desde la primera (alineada a k): el cuadro
   entero, estable (prueba `targets_present_interlaced`).
2. **Juego: «zoom» cada 2 campos** (VI de 320 × 237 fijo, dos
   framebuffers `034000/320` y `059800/320` alternando). El primero
   comparte dirección con el `034000/640` de los logos, y la superficie de
   cada destino lleva su propia escala (presupuesto de VRAM y
   `rdp_target_scale` al crearla); el modo 1:1 de la presentación usaba el
   tamaño en píxeles de la superficie, así que un búfer a 2× y otro a 3× se
   veían de tamaños distintos. Ahora la decisión y el tamaño en pantalla
   salen del tamaño lógico por la escala objetivo. El log de la PS3 anota
   cada destino al crearlo (`gfx: destino …: lw × lh a N×`). Confirmado en
   la PS3 real (el autor, 64b37ba): logos completos y sin zoom; el log
   muestra la causa: `02a200/320: 320 × 459 a 2×` y `04fa00/320: 320 × 240
   a 5×`. El 459 viene de un FILLRECT de DK64 sobre dos búferes seguidos
   (como el de 720 de Star Fox, pero sin pasar de 480): ahora una color
   image de más de pantalla y media (`rdp_targets_tall`: lh > 9/8 del
   ancho, o > 480) también se trata como alta y su relleno se reparte
   entre los destinos que empiezan dentro (el último puede asomar por
   abajo), así los dos búferes quedan a la misma escala (en el host ya no
   aparece el destino de 459). Zelda y SM64 bien con el modo 1:1 nuevo.
   **Corrección:** eso dejó negra «la escena del cierre» (el autor, con
   main 5182004): esa misma color image de 320 × 459 lleva a veces dibujos
   de verdad, no solo rellenos, y el CPU la copia después (en el log:
   «imposible … copia 02a200/320 esperada PERDIDA»). Ahora una color image
   es «alta» solo si todos sus dibujos hasta el próximo TARGET son
   rellenos (`fills_only`, host y PS3); con dibujos reales se crea tal cual
   (a la escala que le toque, que la presentación ya no convierte en
   zoom). Prueba: `raster_tall_fill` cubre los dos casos. En el host no se
   llegó a esa escena con los guiones probados; por confirmar en la PS3.
## 2026-10-12 — Donkey Kong 64 al 20 %: el recompilador vaciaba la caché en cada cambio de Status.FR (rama `zealous-wilbur-cec8b5`, paquete SLSA00102 «Salsa64 DK64»)

- **Síntoma (PS3 real, main 8c876ac):** DK64 a 12–13 campos/s con 1,4 MIPS
  útiles y 65–75 ms de emulación por campo; el log decía 445 864 bloques,
  959 vaciados y 1 desalojo en 30 s. La HLE gráfica (1 ms por tarea) y el
  audio no pesaban.
- **Causa (confirmada en el host con un contador en la escritura de
  Status):** DK64 corre hilos con FR = 1 y otros con FR = 0, y cada cambio
  de hilo cambia el bit al reponer SR: ~260 cambios por segundo de consola
  (4 100 en 16 s). El recompilador compilaba toda la caché para un FR y la
  vaciaba entera al cambiar (`salsa64_rec_run`): en qemu, 4 173 vaciados y
  1,28 M bloques compilados en 16 s de consola (1 340 por campo), 236 s de
  reloj. No era el DMA del PI, ni CACHE, ni la TLB, ni los ids: los
  vaciados ahora se cuentan por causa (`stats.flushes_by[]`, en la línea
  «recompilador:» del frontend y de salsa64-run).
- **Arreglo (docs/rec-ppe.md, «Status.FR: gemelos en vez de vaciar»):**
  solo depende de FR un bloque que accede a un registro impar de la FPU
  (`fr_dependent`, por instrucción); su entrada revisa CU1 y FR de una vez
  (una instrucción más que antes) y, si no coinciden, sale por `exit_fr` y
  el despachador busca o compila el gemelo para el otro FR (`twin[]` en
  KSEG0, filtro por FR en `xhash` fuera); los demás bloques valen con los
  dos. `salsa64_rec_invalidate` mata a los dos gemelos. +128 KB
  (docs/memory-budget.md).
- **Host (qemu, DK64 16 s de consola, `--rec`):** 236 → 7,5 s de reloj,
  1 284 166 → 8 867 bloques, 4 173 → 0 vaciados, 256 bloques dependen de FR
  y **0 gemelos** (los hilos con FR = 0 no tocan registros impares: el
  mismo código no corre con los dos FR).
- **La cuenta de «93 % en bucles de espera» es coherente:** DK64 ya da 81 %
  con el intérprete en el host; con 12 campos/s el CPU emula 19 M ciclos
  por segundo y el 7 % útil son 1,4 MIPS. Lo que estaba mal era el costo de
  ese 7 %, no la cuenta.
- **Validación en el host:** unitarias 426/426 con el recompilador en los
  cinco modos (nueva `fr_switch_keeps_code_cache`), nemu64-test con las
  433 fallas de la línea base, vitest, estados y `--lockstep` de
  nemu64-test y vitest sin diferencias (`tools/test-rec.sh --lockstep`);
  GoldenEye hasta la presa (7,5 G ciclos) y la intro de Perfect Dark (3 G)
  en `--lockstep` sin diferencias. Build de PS3 sin avisos.
- **PS3 real (paquete SLSA00102 «Salsa64 DK64», ad18cce + main da2adcd;
  el autor):** DK64 **59,6–60,0 campos/s (100 % de NTSC)** en la intro, la
  isla y el juego, emulación 4–6,5 ms por campo (antes 65–75), 11 908
  bloques y **0 vaciados** en 2 min (antes 445 864 y 959 en 30 s); una sola
  ventana a 48 campos/s al cargar la isla. 756 bloques dependen de FR y 0
  gemelos: el código con registros impares no corre con los dos FR. Zelda
  (con estado cargado y pausa) y SM64 al 100 %, un vaciado cada uno (el de
  cargar el estado); Perfect Dark al 7 % por la espera de copias de
  da2adcd, ya arreglada en main (98def88). Quedan dos defectos de
  presentación en DK64 que no son del recompilador y pasan a la rama de
  gráficos: los logos en alta resolución entrelazada (640×474, VI_WIDTH
  1280, origen y V_VIDEO alternando por campo) salen como la mitad de
  arriba estirada, y en el juego la imagen hace «zoom» cada 2 campos con
  el VI fijo (320×237, dos búferes alternos): escala distinta por destino.

## 2026-10-12 (noche) — Star Fox 64 colgado a los 5 s: el espejo del framebuffer pisaba un DMA (rama `infallible-wozniak-aa210c`)

Salió en la PS3 real (Rev 1 y Rev 0: letras unos 5 s y negro, el
CPU al 100 % sin bucles de espera reconocidos). En el host se reproduce
solo con el espejo del framebuffer (`--gfx-frame`, que lo activa; con
`SALSA64_FBFX=0` corre): el juego se queda en la tarea gráfica 67 con todos
sus hilos de libultra esperando mensajes (`tools/libultra-threads.py`) y el
hilo ocioso corriendo. No es el CIC 6101 ni el PIF ni la EEPROM (sigue
leyendo el control; la EEPROM se inicializa bien).

Causa (traza del PI y de las tareas, mismo campo 151): la tarea 67 limpia
el framebuffer `38f800` con un FILLRECT y, antes de que el RSP termine
(30 000 ciclos de HLE), el juego carga por DMA 113 KB de datos MIO0 encima
(`38f800`–`3ab1d0`, en trozos de 256 bytes). En la N64 el RDP acaba antes
y el DMA pasa por encima; en el host el espejo «al terminar la tarea» se
escribía después del DMA y pisaba la cabecera del MIO0 (en la PS3, ms más
tarde, pisaba más). Arreglo general (docs/framebuffer.md): un anillo de
DMA (`rdp_dma_ring`, targets.c, lo alimenta `salsa64_bus_rdram_written`
con la tarea gráfica en vuelo) y ninguna copia de la tarea T (espejo,
copia esperada, espejo chico; host y PS3) pisa un tramo que un DMA tocó
desde que T empezó. Pruebas: `targets_dma_ring` y `raster_mirror_after_dma`
(falla sin el arreglo). Host: Star Fox Rev 1 con el espejo, 590 tareas en
20 s y una sola copia descartada (la de la tarea 67). En la PS3 real (el
autor, con 0b4108e): **ya arranca**, pero con «cuadros negros y ruido tipo
estática en la parte superior» durante todo el juego, a velocidad 100 %.

**Estática arriba:** Star Fox limpia sus tres framebuffers seguidos
(`38f800`, `3b5000`, `3da800`) con un solo FILLRECT de 320 × 720 sobre una
color image en el primero (al arrancar, campo 3, y al pasar a la escena,
campo 158); después cada cuadro solo limpia 8..312 × 8..232 y el borde de
8 px queda como lo dejó ese relleno. La PS3 rechazaba esa color image
(«imposible», alto > 480) y el borde nunca se limpiaba: la superficie se
ceba con la RDRAM, donde las filas 0–7 de `38f800` traen los datos MIO0
cargados por DMA (en el host: basura de colores en la fila 0 del campo
300; con el arreglo, negra). Ahora un relleno sobre una color image más
alta que 480 se reparte entre los destinos existentes que caben enteros
dentro (`rdp_targets_inside`, host y PS3; pruebas `targets_inside` y
`raster_tall_fill`), sin crear un destino de 720.

**Cuadros negros:** no reproducidos ni en el host ni en RPCS3 (capturas de
cada campo hasta el 445 y cada 3 hasta el 1800 sin saltos de brillo;
presentación toda del RSX, edad 2,5–3 campos). Para verlos en la consola,
el frontend traza ahora la presentación por campo si existe
`/dev_hdd0/salsa64/trace-vi` (con un número N de campos; por omisión 300):
origen del VI, de dónde salió la imagen (RSX, RDRAM o nada), destino,
ranura, campos desde que se dibujó y cuánto va por delante la emulación.
PS3 real con 8c8e3cd (el autor): jugando ya no parpadea; queda el logo
«1997 Nintendo presents». La traza por campo lo explica: los 1200 campos
salen del RSX con 1–3 campos de edad, pero entre el campo 96 y el 107 no
hay campos: la emulación esperó 200 ms (el tope) la copia a la RDRAM de la
tarea 5 (3da800, «esperada», 1 tarde) porque con el VI apagado la
presentación no la traducía. Es lo que arregla main en 98def88 (la
presentación traduce siempre la tarea cuya copia espera la emulación),
traído a la rama en 129a6bb. `trace-vi` queda renombrado a `trace-vi.off`
en la consola.

## 2026-10-12 (tarde) — Líneas azules de GoldenEye (cobertura AA), túnica de Link (máscara con clamp) y el negro de la intro de Banjo (niebla en el blender) (rama `infallible-wozniak-aa210c`)

Con el volcado del autor en el túnel de Dam (SELECT + R2), las líneas azules
salen también en el raster del Mac: no son del RSX. `tools/gfxdump.py pixel`
(nuevo, junto con `tex`; el script ya lee el `gfx_draw` actual) muestra que
ningún triángulo cubre el centro de esos píxeles: es una unión en T entre el
piso (dibujo 5, con un vértice a media arista) y la pared (dibujo 7, la
arista entera). Ese vértice cae un píxel fuera de la recta de la pared y deja
una cuña de hasta 0,56 px por la que se ve el relleno negro con el tinte
azul del rectángulo final de la escena. La N64 la tapa porque con AA_EN el
RDP escribe todo píxel con alguna de sus 8 muestras de cobertura dentro.

`gfx_draw.aa` marca los triángulos opacos sin prueba de alfa con AA_EN
(prueba `gfx_aa_flag`), pero las dos formas de reproducir la cobertura
fallaron en la PS3 real (el autor, 2026-10-12):
- POLYGON_SMOOTH del RSX: no hace nada (el volcado 2529 traía la marca en
  100 de 109 dibujos y las líneas seguían). Ya está quitado.
- Ensanchar los triángulos en la HLE (cada arista corrida lo que llegan las
  8 muestras, vértices nuevos sobre el plano del triángulo): sí tapaba las
  líneas de Dam, pero a escala interna alta engorda todos los bordes (0,5 px
  de N64 son 1,5 px a 3×) y en mallas densas «los polígonos se superponen»,
  en Zelda y en los modelos de GoldenEye; «se ve peor todo lo demás». Quitado
  también, incluso como opción por juego. En el SPE costaba ~1 µs más por
  triángulo (sin que el PPE esperara).
Las líneas azules de GoldenEye quedan pendientes. Camino que falta probar:
multimuestreo del RSX (superficie con 4 muestras y resolución al presentar)
en rsxkit, que cubriría la cuña con la mezcla del fondo, como el VI de la N64
a 1×; cuesta memoria y relleno. O ensanchar solo las aristas que no comparte
ningún otro triángulo de la malla, lo que pide conocer la malla.

`salsa64-run --raster-dump` dejaba volteados por palabras los bytes de
`nterms` a `pad` de los volcados big-endian: los dibujos con combiners
generados salían mal en el host. Con eso corregido, la túnica de Link del
volcado del autor se ve igual en la PS3 y en el host.

**Túnica de Link (OoT):** el autor ve cortado el patrón de sombra del
pliegue. La textura del torso es una I8 de 16 texels de ancho en la TMEM
(línea de 16 bytes) en una tile de 32 con máscara 4 y espejo más clamp en s.
El RDP limita la coordenada a la tile y después la enmascara: las columnas
16–31 son el reflejo de las 0–15. `tmem.c` veía el clamp, ignoraba la
máscara y leía 32 texels seguidos (la mitad derecha salía de la fila
siguiente de la TMEM). Ahora, con clamp y una máscara más chica que la tile,
decodifica con la coordenada enmascarada, y eso entra en la llave de la
caché. HLE y LLE coincidían porque la LLE usa la misma decodificación.
Prueba: `gfx_tile_clamp_mirror_mask`. En siete juegos solo cambia OoT, y
por debajo del umbral.

**Pantalla negra de Banjo entre el logo de Rare y la cascada:** también en
el host (campos 992–1020 con `banjo-casa.pad`), con la HLE y con la LLE. El
juego funde con un rectángulo a pantalla completa con el blender
FOG·A_FOG + MEM·(1 − A_FOG) y el combiner en 0. El blender toma el color de
P, que aquí es la niebla, y eso no se hacía. Con la niebla blanca opaca
salía negro (el combiner); con alfa 0xD8 no pintaba. Ahora `blend_constant`
(src/rsp/gfx/rdp.c) pone la constante de P (niebla o blend) como color y el
alfa que elige A. En el host la transición queda blanca y después se funde
en la escena, sin el negro. Prueba: `gfx_blender_fog_color`.

**Rompecabezas de Banjo (sigue pendiente):** en esa escena todas las tareas
de Banjo terminan con el RDP congelado y la interrupción del DP sale al
descongelar. Una espera de la copia en el `done` del RDP no sirve: la tarea
congelada no se traduce hasta descongelar (si no, vuelve el temblor) y
esperarla bloquearía la emulación. La espera tiene que ir al descongelar
(`salsa64_rsp_dp_unfreeze`), con presentación traduciendo esa tarea en el
acto. Queda para después.

**PS3 real (el autor, 2026-10-12):** POLYGON_SMOOTH no hizo nada (las
líneas seguían; el volcado 2529 traía la marca en 100 de 109 dibujos). Con
el ensanchado, «ya no hay líneas azules en GoldenEye» y «la túnica sale
bien». Pero en Zelda, a escala interna alta, «los polígonos se superponen»:
el ensanchado engorda todos los bordes (0,5 px de N64 son 1,5 px a 3×) y en
mallas densas se nota. Queda como opción por juego (`GFX_JOB_AAGROW`,
sync.c, por el código "NGE" del encabezado): solo GoldenEye. En el SPE
costó ~1 µs más por triángulo en Dam (4,4 → 5,5), sin que el PPE esperara;
con recíprocos en vez de divisiones debería bajar (sin medir aún).

**PS3 real con 4e35681 (el autor, 2026-10-12):** túnica de Link bien;
Zelda y GoldenEye con los polígonos normales; en la intro de Banjo ya no
está el negro de medio segundo, queda un negro de uno o dos campos al
cambiar de escena que en el host no existe (323 campos seguidos sin saltos
de brillo): probablemente el VI muestra el framebuffer recién cambiado antes
de que el RDP lo dibuje. Menor; sin perseguir. Las líneas azules de
GoldenEye siguen pendientes (arriba).

## 2026-10-12 — Banjo-Kazooie (protección del CIC 6103), cara de Conker (K5) y rayas del logo de Zelda (rama `infallible-wozniak-aa210c`, paquete SLSA00099 «Salsa64 gráficos»)

Lo visto por el autor en la PS3 con v0.1.0 (modelos que tiemblan, cosas
dibujadas encima de otras, polígonos que faltan, el cielo) no era de la HLE
ni del RSX: el juego dibujaba así a propósito porque nuestro arranque HLE no
dejaba lo que deja el IPL3 del CIC 6103.

Diagnóstico (host, respaldo propio, `docs/pads/banjo-casa.pad`):
- La proyección de Banjo (`G_MTX` de la perspectiva y tres rotaciones)
  traía en toda escena cerca = 750 y lejos = 1250 (escala 0,5, perspNorm
  65). En la intro el fondo (árboles, cascada, cielo) quedaba detrás del
  plano lejano y la HLE rechaza los triángulos con los tres vértices ahí;
  Tooty, Bottles, la casa por dentro y el cielo de Spiral Mountain (una
  malla a w 35–135 alrededor de la cámara) quedaban delante del cercano y se
  recortaban; lo que quedaba en medio tenía su profundidad en una franja
  estrecha (z/w normalizada de 0,84 a 1,07 para Banjo): temblores y
  superposiciones. En el campo 2400 de la intro, 75,5 % de la imagen negra.
- El microcódigo real (LLE, desde el mismo estado) daba la misma imagen que
  la HLE: fondo negro y Tooty cortada. Así se descartaron `src/rsp/gfx`,
  rsxkit y el rasterizador.
- La decomp de Banjo-Kazooie (`viewport_setRenderPerspectiveMatrix`, bloque
  `ANTI_TAMPER`) lee la RDRAM física 0x1D8 antes de cada proyección; si no
  vale lo que deja el IPL3, cambia 30 y 4000 por 750 y 1250. Ejecutando en
  el emulador el IPL3 del propio respaldo (sin desensamblarlo) se midió lo
  que deja: su imagen de DMEM 0x4C0–0x764 en RDRAM 0x000–0x2A4 (0x1D8 = ROM
  0x698), y la DMEM y la IMEM en 0xFF salvo 6103 en IMEM 0x000.
- La segunda la comprueba `ucode_load` al iniciar: sin ella el juego no
  trae de la ROM (0xB70) el arranque del RSP y su búfer queda en ceros. Por
  eso `--lle=all` no arrancaba (pendiente del 2026-10-10: «la primera tarea
  corre 958 M instrucciones sin tocar COP0»).

Arreglo (`src/core/boot.c`, `boot_x103`, como el del 6105): copia de DMEM
0x4C0–0x764 (los bytes del cartucho del usuario) a RDRAM 0x000–0x2A4, DMEM e
IMEM en 0xFF y 6103 en IMEM 0x000. Fuentes en docs/THIRD_PARTY.md y tabla
en docs/cpu-vr4300.md («Arranque HLE»).

Medido en el host:
- Intro, campo 2400: 582 → 862 triángulos por tarea; píxeles negros 75,5 %
  → 0,4 %. Se ven el fondo, Tooty entera, la casa con Banjo en la cama, el
  cielo nocturno y Spiral Mountain con su cielo.
- HLE contra el microcódigo real (`--lle=all` desde el mismo estado, 40
  campos después; el LLE ahora arranca solo): 2109 y 3230 píxeles distintos
  de 76 800 en la intro (campos 2470, 2830), 5468 en la casa (11490), 1281 y
  1329 en juego frente a la casa (17000, 17600); 16 671 en la vista aérea
  (9490) con la cámara en movimiento. Lo distinto es movimiento (el LLE
  lleva otro ritmo), no geometría.
- Zelda, SM64, Mario Kart, GoldenEye, Perfect Dark y Conker: capturas cada
  500 campos idénticas a las de main (no pasan por el 6103).
- La protección no depende de la HLE gráfica: en la PS3 debería verse igual
  que en el host.

Herramientas: `--state-load` con `--lle` carga el estado (guardado con la
HLE) y sigue con el microcódigo real, para comparar las dos en la misma
escena; `docs/pads/banjo-casa.pad`.

Verificación (con main b445531): `tools/build.sh test-host` 406/406
(pruebas nuevas `boot_cic6103_leftovers`, `gfx_setconvert_k5` y
`gfx_texrect_two_tiles`), `tools/test-roms.sh` (gfxtest 35/35 ×3),
`tools/build.sh` sin avisos, `tools/test-rec.sh` y `tools/test-phase2.sh`.

**Cara de Conker (polígonos negros en la boca):** el hocico y la boca se
dibujan con `(SHADE − ENV)·K5 + PRIM` en el segundo ciclo y K5 = 255
(`G_SETCONVERT`); la HLE ignoraba ese comando y K5 valía 0. El microcódigo
real (LLE) daba la misma cara negra: era del RDP común. K4 y K5 (9 bits con
signo) van ahora al combiner y al estado del RDP, al final del formato de los
estados guardados (los viejos los cargan en cero). En el host, Conker frente
al logo de Rare: 1047 píxeles cambian (el hocico). Prueba
`gfx_setconvert_k5`.

**Rayas del logo de N64 de Zelda:** el texto «NINTENDO 64» va en
rectángulos de 2 filas en 2 ciclos: el texto en la tile 0 y un ruido I8 de
32 × 32 en la tile 1 con t × 32 (`shiftt` 11), que vuelve al mismo renglón
en cada fila y cambia de renglón por franja. El límite del TEXRECT se
calculaba con la tile 0 y se aplicaba también a la tile 1: el ruido quedaba
pegado a su primer renglón en una fila de cada dos (en el verde del trazo de
la «N», la fila de arriba de cada franja fija en 134). Ahora cada tile tiene
su límite. Además, a 2×–5× el RSX interpolaba t dentro de la fila y recorría
media textura de ruido por píxel de la N64: cuando una tile avanza media
textura o más por fila, el rectángulo sale en una tira por fila con la t de
esa fila. Prueba `gfx_texrect_two_tiles`. La «N» del logo sale más pálida
que en mupen64plus (GLide64), pero el microcódigo real da las mismas
coordenadas del mapa de entorno (82 píxeles distintos en todo el cuadro) y
el muestreo cae sobre el brillo de la textura: no hay evidencia de un error
nuestro.

**Mario Kart, negro arriba del muro de Luigi Raceway (resuelto):** era la
tapa del muro, una franja que el juego dibuja con la textura apagada y un
combiner TEXEL0·SHADE. La HLE dejaba ntex en 0 con los términos de textura:
el host toma la textura que falta como 1 (blanco) y el RSX muestreaba una
sin ligar (negro). Ahora con la textura apagada TEXEL0/TEXEL1 = 1 en la HLE
(`gfx_combine_untextured`). Hallado con volcados SELECT + R2 de la PS3 (la
captura del RSX con la franja negra, el mismo volcado en el host sin ella) y
`tools/gfxdump.py`; confirmado por el autor en la PS3. Antes, en el host el
cielo se veía siempre (no era el cielo: era esa franja). Para eso, **SELECT + R2** (`src/frontend/main.c`)
guarda `gfxdump-<campo>.bin` con la salida de la HLE del cuadro en pantalla
y `gfxdump-<campo>.ppm` con la captura del RSX de ese mismo cuadro: se
dibuja en la Mac con `salsa64-run --raster-dump` y se compara.

Comparación de capturas cada 500 campos contra main: solo cambian Banjo,
Conker y el logo de Zelda (SM64, Mario Kart, GoldenEye y Perfect Dark
idénticos).

**Temblor de Banjo (PS3, visto por el autor con el paquete de arriba):**
los modelos «avanzan y retroceden». Banjo arranca la tarea del cuadro
siguiente con el RDP congelado (FREEZE) sobre el búfer que el VI todavía
muestra y lo descongela al cambiar de búfer; el núcleo ya retenía la
interrupción del DP, pero presentación traducía la tarea al RSX en cuanto
terminaba: ese búfer mostraba el cuadro nuevo un campo antes y después el
otro búfer el anterior. Ahora cada campo cuenta como visibles solo las tareas
que el RDP pudo ejecutar (sin la retenida, `rsp.dp_held`). Confirmado por el
autor con su estado de la escena de la bruja: ya no tiembla.

**Rompecabezas de Banjo negro en la PS3 (pendiente, efectos de
framebuffer):** las piezas son texturas que el CPU copia del framebuffer en
la RDRAM (70 080 lecturas de 0x3a5d00, medido en el host). Al rearmarse, el
juego dibuja el título en un búfer que el VI no muestra y lo copia 2–4 campos
después; en el host el espejo del framebuffer es inmediato y las piezas
llevan el título, en la PS3 llega tarde y el CPU copia lo que había (negro, o
la imagen de antes). Reproducido con dos estados del autor: desde ellos el
host dibuja las piezas bien y la PS3 negras. Arreglo posible: esperar el
espejo antes de la interrupción del DP, como opción por juego (cuesta
milisegundos por cuadro); queda para después (decisión del autor).
La pantalla negra entre el logo de Rare y la cascada no está atribuida: en
el host ahí hay un fundido blanco (una nube 3D enorme pegada a la cámara);
falta un SELECT + R2 en la PS3.

Pendiente (PS3 real, paquete «Salsa64 gráficos» SLSA00099; confirma el
autor):
- [x] Banjo-Kazooie: se ven los modelos, las texturas y el cielo; ya no
      tiembla (el autor, 2026-10-12).
- [x] Conker: la cara (hocico y boca) en la intro con el logo de Rare:
      «ya sale bien» (el autor, 2026-10-12).
- [x] Zelda: el logo de N64 al arrancar sin las líneas de entrelazado: «ya
      sale bien» (el autor, 2026-10-12).
- [x] Mario Kart, Luigi Raceway: el muro sin la franja negra (el autor,
      2026-10-12).
- [ ] GoldenEye en el túnel de Dam y la túnica de Link: SELECT + R2 cuando
      se vean las líneas azules o el defecto de la túnica, y bajar
      `gfxdump-*.bin` y `.ppm` por FTP para dibujarlos en la Mac. En el
      host, Link en su casa (guion `zelda-kokiri.pad` con la partida del
      autor) no muestra un defecto visible en la túnica.
- [ ] Otros juegos del 6103 (Diddy Kong Racing, Paper Mario) no se
      probaron; el arranque nuevo reproduce lo que deja la consola.

## 2026-10-12 (madrugada) — Para retomar: pruebas pendientes en la PS3

Publicado el 2026-10-11: el repositorio es público y está el release
**v0.1.0** (pre-release, etiqueta en e0aa556, con `salsa64-v0.1.0.pkg`).
Después entraron en main la FlashRAM (7ede061: Majora's Mask jugable) y la
decodificación de texturas recortada y con vectores del SPE (5853614: intro de
Perfect Dark de 50 a 60 campos/s).

Ramas listas que esperan la prueba en la PS3, en este orden:
1. ~~`heuristic-agnesi-760f8c` (SLSA00098 «Salsa64 botón PS»)~~: confirmada en
   la PS3 real e integrada el 2026-10-12 (entrada de abajo).
2. `infallible-wozniak-aa210c` (SLSA00099 «Salsa64 gráficos»): Banjo-Kazooie
   (lo que deja el arranque del CIC 6103 en la RDRAM, DMEM e IMEM; sin eso su
   protección cambiaba los planos de la cámara) y la cara de Conker
   (`G_SETCONVERT`, K4/K5 en el estado del RDP).
3. `charming-spence-f80224` (SLSA00100 «Salsa64 Majora»): campanadas del fin
   del día (los destinos distinguen el tamaño de píxel, no solo dirección y
   ancho) y desenfoque de movimiento (baile del Espantapájaros, recuerdo de
   Zelda); estados de prueba en las ranuras 5 y 6. El telescopio necesita un
   estado guardado frente a él.

Pendientes: el telescopio de Majora; leer el cuadro anterior del desenfoque
desde el destino del RSX en alta resolución (hoy sale de la copia a 1×); el
cielo de Luigi Raceway; las líneas del logo y la túnica de Zelda; las líneas
de GoldenEye; la localidad del código del recompilador (GoldenEye en la presa y
la segunda escena de la intro de Perfect Dark, limitadas por el CPU); y un
release v0.2.0 cuando entren las tres ramas de arriba.

Después de cerrar lo anterior: revisar (solo lectura primero) el recompilador
de Wii64 y Not64 y el port `emukidid/wii64-ps3` (base de Cell64): asignación
de registros entre bloques, accesos a memoria y TLB, caché de código y FPU;
licencia archivo por archivo antes de copiar nada a `src/` (nunca a
`lib/rsxkit` ni `lib/spukit`). También ejecutar el IPL3 del cartucho al
arrancar, como mupen64plus, para no depender de reproducir lo que deja cada
CIC (protecciones de Rare). Y HLE de funciones de libultra en el CPU (como
UltraHLE): perfilar en GoldenEye y Perfect Dark cuánto tiempo va a funciones
conocidas (matrices, trigonometría, raíz, copias de memoria) y, si pesa,
reemplazarlas en el recompilador por versiones nativas con vectores del PPE.
Detección por comportamiento o por huella (hash) del código, sin copiar nada
de la biblioteca al repo; resultado exacto, porque la lógica del juego no
admite aproximaciones.

Anotado la tarde del 2026-10-12, para después de v0.2.0:
- **Sombras cuadradas en Conker** (PS3 real, desde antes de las ramas de hoy):
  las sombras redondas de los personajes salen como bloques. Probablemente la
  textura de la sombra (degradado con alfa) se dibuja sin su alfa o con el
  formato o el modo de ciclo equivocado; comparar contra mupen64plus en la
  misma escena.
- **Hacks** (reportes de usuarios): Smash Remix 2.0.1 arranca y corre bien,
  pero se cuelga cuando cae un personaje; The Sealed Palace (hack de Ocarina
  of Time) se queda en pantalla negra. Revisar con el log de la PS3 antes de
  suponer la causa (tamaño de ROM, CIC, microcódigo F3DEX3 u otro).

## 2026-10-12 — Botón PS y cuadro al salir: el juego se pausa (rama `heuristic-agnesi-760f8c`, paquete SLSA00098 «Salsa64 botón PS»)

Problema (PS3 real, v0.1.0): dentro de un juego, el botón PS abría el menú
del sistema y, al volver, Salsa64 ya no respondía y había que cerrarlo desde
el sistema (visto en Banjo-Kazooie). `sysutil_cb` solo atendía «Salir del
juego»: la emulación, el audio y los mandos seguían detrás del menú. No hay
log de ese cuelgue, así que la causa exacta no se vio.

Hecho (docs/hilos-ppe.md, «Botón PS y pausa»; docs/uso-ps3.md):
- **Pausa** con el menú del sistema abierto (`SYSUTIL_MENU_OPEN` … `CLOSE`,
  o el bit del sistema en `padInfo.info`): la emulación queda quieta al
  empezar el campo siguiente y al volver el ritmo empieza de nuevo; el audio
  calla sin perder lo encolado y sin que el hueco entre en la medición de
  velocidad (`audio_sync_pause`); se sigue presentando el último cuadro con
  flip (`present_still`, `salsa64_rdp_present_again`) y llamando a
  `sysUtilCheckCallback`. Mandos sueltos mientras los tiene el sistema; nada
  queda pegado al volver. Al log: «sistema: menú abierto/cerrado», «pausa: …»,
  «mandos: …».
- **Cuadro de START + SELECT** sobre la misma pausa: «¿Volver al menú?» (con
  `embedded`, «¿Salir al XMB?»), × sale, ○ o el atajo otra vez siguen; lo
  apretado al abrir o cerrar no llega al juego hasta soltarse. «Confirmar al
  salir» en Opciones (`confirmar = si|no` en menu.cfg, sí por omisión).
- **Latido**: un hilo que avisa en el log dónde se queda quieto cada hilo si
  uno no da vueltas en 2 s. `sin-pausa` (archivo de diagnóstico): el menú
  del sistema no pausa, como en v0.1.0, para ver con el latido qué colgaba.
- `pads_apply` ya no gira sin fin si el sistema desaloja a presentación a
  mitad de publicar los mandos.
- **Pruebas**: suite `pausectl` (9: menú del sistema, mandos tomados, cuadro
  con ○, ×, el atajo otra vez y sin confirmación, cuadro con el menú del
  sistema encima, geometría en 4:3 y 16:9, y el hueco de 6 s contra el
  ritmo: ni ráfaga ni esperas largas al volver) y dos en `sync` (con la
  pausa, el audio vuelve en < 150 ms sin estirar; sin ella, el hueco lo
  estiraba). test-host, test-roms, test-rec, test-phase2 (menú: 5 juegos
  abiertos y cerrados) y el build sin avisos, en verde, también después de
  traer main con la FlashRAM.

**En la PS3 real (2026-10-12)** la pausa sola no bastó: al volver del menú
del sistema la app seguía viva (eventos, mandos, flips) pero la pantalla
quedaba negra. Con el latido y leyendo píxeles del búfer trasero se vio la
causa, que seguramente es la de v0.1.0: el sistema **pisa el estado del
RSX en cada flip** de la aplicación mientras su menú está encima y un rato
después, y con el RSX parado en `gcmSetWaitFlip` cada flip tardaba 2,3–4,5 s.
Arreglo en `lib/rsxkit`: el flip anterior se espera en el PPE
(`rsxkit_frame_begin`) y después de cada flip se olvida todo el estado que
rsxkit daba por puesto (`rsxkit_forget_state`), más `rsxkit_after_system` y
`rsxkit_frame_ready` (docs/hilos-ppe.md). Confirmado: en el menú de Salsa64
y dentro de un juego, PS y volver tres veces cada uno (el XMB se ve con el
menú debajo, al volver todo sigue y responde; la emulación se detuvo y
retomó cada vez), el cuadro de salir con × y «Salir del juego».

También con Majora's Mask sonando: dos vueltas del botón PS (la salida
cuenta la pausa; en la ventana siguiente 0 % estirado y 0 faltas), el cuadro
con «No» y con ×, y cierre limpio. Criterio de cierre cumplido.

## 2026-10-12 — Majora's Mask: campanadas, recuerdo de Zelda y telescopio en «tablero de ajedrez» (rama `charming-spence-f80224`, paquete SLSA00100 «Salsa64 Majora»)

Problema (PS3 real, main 7ede061): en Zelda: Majora's Mask (NZSE) la
deformación de la pantalla con las campanadas del fin del día, el recuerdo
de Zelda enseñando la Canción del Tiempo y la vista del telescopio del
Observatorio se veían «como una matriz de píxeles muy pequeños
intercalados, como un tablero de ajedrez».

Medido (estados de la consola cargados en el host, log de la PS3; detalle
en docs/framebuffer.md, «Majora's Mask»):
- **Campanadas** (desde las ~5:36 hasta las 6:00): cada tarea copia el
  framebuffer a `784600/320` en 40 franjas y lo redibuja desplazado en
  56–60. La **pausa** usa esa dirección y ancho como imagen de 8 bits (la
  cobertura de su filtro). La tabla de destinos emparejaba solo dirección
  y ancho: después de abrir la pausa una vez, la ranura quedaba de 8 bits;
  las texturas de 16 bits no la encontraban (en el log de la consola, 40
  lecturas del RSX por tarea en vez de 100) y la copia a la RDRAM escribía
  bytes de intensidad en una imagen de 16 bits. En el host se reproduce
  igual (pausa, y después las campanadas: ruido) y sin la pausa sale bien.
- **Recuerdo de Zelda y baile del Espantapájaros**: desenfoque de
  movimiento con un acumulador en `3a92c0/320` que cada tarea mezcla con el
  cuadro (70 % de lo anterior) y lee en la siguiente. Se leía de su copia
  sin esperar en la RDRAM: una o dos tareas tarde, a 1× y en 16 bits; con
  la realimentación, el redondeo se acumula (en el host, sin retraso, la
  estela salía hasta 17 niveles más oscura) y, con el tramado del RSX en
  las copias a 16 bits, deja un patrón fijo por píxel.
- **Telescopio**: en el log de la consola, dos copias esperadas al entrar
  y ~39 lecturas por tarea de una imagen guardada en la RDRAM; falta un
  estado de esa escena para verlo en el host.

Hecho:
- `rdp_targets_get`: la misma dirección y ancho con otro tamaño de texel
  es otra imagen si la ranura no se dibujó en `RDP_TARGETS_FRESH` tareas
  (si está viva se comparte, como necesita la pausa de Zelda OoT).
- Las copias a la RDRAM sin esperar (la misma imagen tarea tras tarea) ya
  no pasan la imagen al CPU: el RDP la sigue leyendo del RSX mientras esté
  viva (`rdp_targets_readback(…, cpu_owns)`; el host modela lo mismo).
- `rsxkit_target_read` apaga el tramado (*dithering*) del RSX.
- El log ya no da contadores de texturas negativos tras cargar un estado.
- gfxtest, escena V: una color image de 8 bits que diez cuadros después es
  una de 16 bits leída como textura (36 puntos; sin el arreglo, negro).
- Host, contra main con las mismas entradas: pausa y equipo de Zelda OoT,
  túnel de Mario Kart, SM64 del logo a la cara e intros de Conker y
  Perfect Dark, cuadro por cuadro idénticos. Pruebas: 403 unitarias,
  `tools/test-roms.sh` (gfxtest 36/36 en sus tres variantes), compilación
  de la PS3 sin avisos.

Después, en la misma rama: **el host con la precisión de la N64.** Con el
acumulador leído del destino, guardar un estado a mitad del baile y
cargarlo daba 40 bytes de RDRAM distintos de la corrida de corrido (tras
cargar, lo dibujado sale de la RDRAM a 5 bits; de corrido, del búfer del
host a 8). Los destinos de 16 bits del host guardan ahora los 5 bits por
canal de la copia a la RDRAM: idéntico guardando en tres puntos del baile,
a mitad del desenfoque de Perfect Dark y en la pausa de Zelda OoT (16090 →
16300). La RDRAM no cambia: Zelda OoT, Mario Kart, SM64, Conker y Perfect
Dark, cuadro por cuadro iguales a main con la misma precisión.

**PS3 real (2026-10-12, paquete SLSA00100):** campanadas bien, también
con la pausa abierta antes (el autor las ve «con más resolución»: el efecto
lee la superficie del RSX a 3×); baile del Espantapájaros con la estela
limpia y fluida. Telescopio: ya sin tablero de ajedrez, pero faltaba la
máscara ovalada del lente y el sol salía seis veces. Con los estados del
autor en el host (docs/texturas.md):
- **Lente**: una I4 de 64 × 64 con espejo y clamp en una tile de
  128 × 128. La decodificación con la coordenada enmascarada (f393824, de
  la rama de gráficos) arregla el ancho; el alto no cabe en los 32 KB de
  textura del SPE: se decodifica un solo periodo y el eje va con «mirror
  once» del RSX.
- **Sol**: un atlas de dos soles dibujado sin corrección de perspectiva
  (`G_TP_NONE`): el RDP usa S·(1/W) tal cual y los microcódigos F3DEX2
  normalizan 1/W a la mitad, así que la textura se muestrea a la mitad de
  la coordenada (medido con el microcódigo real). La HLE lo hace ahora en
  los triángulos de la segunda generación.
- Las dos escenas quedan como con el RSP real (`--lle=all`), que es la
  referencia que confirmó ambas causas.

**PS3 real, segunda vuelta (2026-10-12, paquete 86d005f):** lente completo
(primer uso del MIRROR_ONCE del RSX), un solo sol, y sin regresión en la
pausa de Zelda OoT ni en el túnel de Mario Kart. Criterio de cierre
cumplido.

Falta:
- [ ] Cambios bruscos de iluminación en el alcantarillado de Ciudad Reloj
      (ranuras 2, 3 y 7 del autor: amarilla, gris y verde): el host da lo
      mismo que la consola, no es del RSX; comparar con mupen64plus.
- [ ] Escena de gfxtest para «mirror once» y para los triángulos sin
      perspectiva.

## 2026-10-11 — FlashRAM: Majora's Mask guarda (rama `clever-tereshkova-aa0089`, paquete SLSA00096 «Salsa64 FlashRAM»)

Problema (PS3 real, v0.1.0): en Majora's Mask, después de la escena inicial
con Skull Kid, Link moría y aparecía con tres ocarinas en los botones C. El
log decía «guardado: flashram (tabla por código de juego) todavía no se
emula: sin guardado»: el juego identifica el chip al arrancar y lee sus
partidas y el guardado del búho; sin chip leía ceros.

Hecho:
- **FlashRAM de 1 Mbit** (`src/core/flashram.c`, código propio desde
  n64brew.dev «Flash», cotejado con ares y mupen64plus-core; fuente de cada
  detalle en la cabecera y en docs/THIRD_PARTY.md): comandos en 0x08010000
  (borrado de chip y de sector, programación de página, modos búfer de
  página, estado, identificador y lectura), registro de estado (0x8C en
  reposo), identificador 11118001 00C2001E, DMA del PI en los dos sentidos
  y accesos de 32 bits del CPU. Un solo modelo, el MX29L1100 (direcciones de
  lectura por palabra de 16 bits, ráfagas que no salen de su bloque de
  32 KB): es el de Majora, Pokémon Snap y Puzzle League según ares y el que
  mupen64plus da a todos; libultra elige el direccionamiento por el
  identificador. Borrar y programar son instantáneos.
- **Archivo** `saves/<título>-<CRC1>.fla` de 128 KB con el volcado diferido
  de siempre; `SALSA64_SAVE_MAX` sube a 128 KB (búfer del guardado, zona de
  paso y lectura al abrir: +0,3 MB, docs/memory-budget.md). El panel del
  menú muestra «FlashRAM» sin «(sin emular)». Al cerrar, el log dice cuántos
  comandos recibió el chip.
- **Estados guardados**: los 128 KB en CART y el estado del chip en una
  sección nueva, FLSH, con versión propia (docs/estados.md). El búfer de
  paso sube de 128 a 256 KB.
- **Tabla por código**: ya marcaba FlashRAM para Majora (NZS), Paper Mario
  (NMQ), Pokémon Snap (NPF), Puzzle League (NPN), Stadium 1 y 2, Jet Force
  Gemini y otros 13; ahora las pruebas lo comprueban en varias regiones.
- **Pruebas**: suite `flashram` (10 pruebas: identificación, estado,
  lectura por palabras con la vuelta de 32 KB, borrado de sector y de chip,
  programación con AND y búfer a 0xFF, comandos raros, reset) y una de ida
  y vuelta de FLSH en `state`; 401 en total, en verde. `savetest` tiene una
  cuarta variante, `salsa64-savetest-flashram.z64`: identifica el chip,
  borra el chip, programa las 1024 páginas, relee, borra el último sector y
  comprueba que no toque el anterior; pasa sus dos arranques en
  `tools/test-roms.sh` y entra en `save-tests` (RPCS3 y PS3).
- **Majora's Mask en el host** (respaldo propio, `salsa64-run` nativo con
  `--pad`): al arrancar pide el estado y el identificador por DMA de 8
  bytes; con START en el título lee los sectores 0–3 en DMA de 8 KB, no
  encuentra partida y escribe los sectores 2–5; al crear el archivo 1
  («Please wait…») borra el sector 0 y programa sus 128 páginas. El archivo
  abre con 3 corazones, la escena inicial (bosque, Skull Kid, Epona) se
  pasa con A y Link queda jugable con 3 corazones, la espada en B y los
  botones C vacíos, vivo mil campos después. La partida persiste entre
  corridas (`--save-dir`). Para rastrear el chip:
  `EXTRA_CFLAGS=-DSALSA64_DEBUG_FLASH` y `--trace-from`.

- **PS3 real** (2026-10-11, paquete SLSA00096, salida a 1080p): el autor
  creó su archivo, pasó la escena inicial y jugó unos 18 minutos: «ya es
  jugable». En el log, el `.fla` nuevo, los dos volcados diferidos de la
  creación (512 y 128 escrituras del juego: sectores 2–5 y 0), 1959
  comandos al chip en la primera sesión (0 desconocidos) y «cerrado sin
  cambios pendientes»; al reabrir, los 128 KB leídos del `.fla` y el estado
  de la ranura 0 cargado (campo 270 → 39145); más tarde el juego volvió a
  escribir (256 escrituras, volcado diferido). Estados guardados y
  cargados varias veces (8384 KB, ~0,7 s al guardar, 0,18–0,53 s al
  cargar). 59,8 campos/s, CPU ~3,3 ms por campo. En el `.fla` bajado: la
  partida en el sector 0 («ZELDA3» en 0x24) y los sectores 2–5 escritos.
  El guardado con búho o con la Canción del Tiempo queda para más adelante
  (hace falta jugar bastante para llegar).

Falta:
- [ ] Guardar con un búho o con la Canción del Tiempo en la PS3, salir y
      reabrir (la creación del archivo y los estados ya están probados).
- [ ] Probar Paper Mario, Pokémon Snap y Puzzle League con respaldos
      propios. mupen64plus pone en 1 el bit 0 del estado al pedir el
      identificador «para Pokémon Puzzle League»; aquí, como en ares, una
      lectura de E/S en modo identificador da 0x11118001 (bit 0 en 1) y el
      estado vale 0x8C. Si Puzzle League no guarda, mirar eso primero.
- [ ] Otros modelos (MX29L1101 por byte, MN63F8MPN de Matsushita) solo si
      algún juego no se adapta al MX29L1100.

## 2026-10-11 — Estados guardados con 10 ranuras por juego y contador de FPS (rama `wonderful-blackwell-6f40d3`, paquete SLSA00094 «Salsa64 estados»)

Hecho (diseño y decisiones en docs/estados.md):

- **Núcleo portable** (`src/core/state.c`): cabecera de 64 bytes (magia,
  versión, CRC1/CRC2 y tamaño de la ROM, título, RDRAM, campo) y secciones
  «etiqueta + largo» descritas con una sola función para guardar y cargar,
  en big-endian: el mismo archivo sirve en la PS3 y en `salsa64-run`. CPU con
  COP0, TLB y FPU; planificador; MI, SP, DP, VI, AI, PI, RI, SI y la tarea del
  RSP en curso; DMEM/IMEM; PIF; guardado del cartucho; estado de las HLE
  gráfica (RDP + TMEM) y de audio (DMEM, ABI, MP3) entre tareas, también en
  los SPE (trabajos nuevos); miniatura de 160 × 120; RDRAM al final sin
  comprimir. Secciones cortas cargan con ceros (para crecer), versiones
  viejas o nuevas, otro juego u otra RDRAM se rechazan con un mensaje.
- **La tarea del RSP en vuelo** se recoge antes de guardar sin adelantar su
  fin emulado (`salsa64_rsp_quiesce`): el estado sale en cualquier campo y
  la continuación es idéntica.
- **PS3** (`src/frontend/states.c`, `main.c`): SELECT + R1 guarda, SELECT +
  L1 carga, SELECT + izquierda/derecha cambia de ranura (esos botones no
  llegan al juego con SELECT apretado); `/dev_hdd0/salsa64/states/<título>-
  <CRC1>.st0…st9`, escritos con sysFs a un .tmp y renombrados. Barrera en el
  límite del campo: emulación recoge la tarea, encola el campo y espera;
  presentación presenta lo que queda (miniatura), espera al RSX y guarda o
  carga; al cargar vacía la caché del recompilador, suelta los destinos del
  RSX (`salsa64_rdp_drop_targets`, sin tocar la numeración de las tareas) y
  vacía la cola de audio con su transición (`audio_sync_flush`). Aviso de
  ~2 s abajo en la pantalla (`src/frontend/osd.c`, rsxkit/ui).
- **Contador de FPS** (pedido de la coordinación): «VI 59,9 (100 %) · juego
  30 · pantalla 60», promedio de 0,5 s, contado solo en el hilo de
  presentación; SELECT + △ y fila «Mostrar FPS» en Opciones; se recuerda en
  menu.cfg (`fps = si|no`).
- **Host**: `salsa64-run --state-save CAMPO:ARCHIVO`, `--state-load ARCHIVO`
  (con `--lockstep` también la referencia) y `--fields N`.

Pruebas:
- Determinismo (guardar en N, cargar en otra corrida y seguir hasta M contra
  de corrido: estado byte por byte e imágenes iguales): unitarias
  (`test_state.c`, también con el recompilador), `tools/test-roms.sh`
  (gfxtest, vitest, audiotest), `tools/test-rec.sh` (gfxtest con `--rec` =
  intérprete) y respaldos propios en el host: Zelda (intro y Kokiri), SM64
  (intro y castillo), GoldenEye (intro y la presa) y Mario Kart: idénticos.
- RPCS3 (`tools/rpcs3-run.py --estados A,B`: guarda en A, carga en B y la ROM
  de prueba se tiene que verificar después): vitest, gfxtest y audiotest OK,
  cierre limpio (RPCS3 no sirve para medir los tiempos).
- `tools/build.sh test-host` 383/383 (con main); `tools/test-roms.sh` verde;
  `tools/test-rec.sh --lockstep` verde (unitarias en los cuatro modos,
  nemu64-test con la línea base, estados con `--rec` = intérprete, y
  `--lockstep` sin diferencias en nemu64-test, vitest y gfxtest desde un
  estado cargado); `tools/test-phase2.sh` verde; `tools/build.sh` (todos los
  programas) sin avisos. El SPE gráfico solo copia tal cual el tramo del
  estado del RDP y el PPE arma el formato: 0,2 KB de código, la pila queda en
  ~9,9 KB (main con los efectos de framebuffer: ~10,2 KB; la cadena más honda
  usa ~8 KB).
- Con los efectos de framebuffer de main: la barrera espera las copias RSX →
  RDRAM en vuelo y las escribe antes de guardar o cargar
  (`salsa64_rdp_settle`), y al cargar se olvidan también el scratch, los
  espejos y el historial de copias (`salsa64_rdp_drop_targets`). La prueba
  de determinismo de gfxtest corre con el rasterizador en las tres corridas
  (copias espejo y a la RDRAM leídas por el CPU en cada tarea): idéntica; en
  el host con respaldos, también la pausa de Zelda (16090 → 16300), el túnel
  de Mario Kart, la presa de GoldenEye y SM64 (del logo a la cara de Mario y
  el castillo, tras el arreglo de main de la copia del framebuffer, que
  esta prueba encontró: el rasterizador del host dejaba de dibujar SM64 a
  las 96 tareas). La marca de «ya copiado» de cada destino se reinicia al
  cargar; lo dibujado que todavía no se copió no entra en el estado
  (docs/estados.md, «Lo que no entra en un estado»).

**PS3 real (2026-10-11, noche): funciona.** Guardar y cargar en Zelda,
SM64, Mario Kart, GoldenEye (ranuras 0 y 1 independientes) y Perfect Dark;
cargar después de volver al menú y reabrir el juego; «No hay estado en la
ranura N» y «Ranura N (vacía)» donde corresponde; cierre limpio cada vez.
Guardar 568 ms de media (casi todo escribir 8 MB en el disco), cargar 171
ms (docs/perf-log.md, «Estados guardados»). El contador no cambia los
campos/s (GoldenEye en la presa 45–49 con él, como sin él).

La primera prueba colgó la consola entera al guardar: la traza por UDP
paró justo antes de crear la carpeta `states`. Las funciones `sysFs*` de
libsysfs son puentes al módulo cellFs del sistema, que hay que cargar con
sysModuleLoad; sin cargarlo, en RPCS3 andan igual y en la PS3 real cuelgan
todo. Ahora la E/S va con las llamadas al kernel (`sysLv2Fs*`,
`<sys/file.h>`), como newlib.

## 2026-10-11 — Recompilador: FPU residente, sin microcódigo del PPE, COP0 con rutinas propias y caché por tramos (rama `jovial-jones-2408ac`, paquete SLSA00093 «Salsa64 FPU»)

Objetivo: bajar los ~50 ciclos del PPE por instrucción MIPS del código
nativo (GoldenEye en la presa, 49,4 campos/s). Hecho en el host y en qemu;
la medición en la PS3 queda para cuando haya turno (cola: estados guardados,
después esta rama). Diseño en docs/rec-ppe.md.

- **Microcódigo del PPE** (manual del Cell, tabla A-1/A.1.3): toda forma con
  punto (`andi.`, `andis.`…), los desplazamientos por registro, `mcrfs` y
  `mtcrf` de varios campos cuestan ≥ 11 ciclos y vacían el pipeline; `mfcr`
  ~34. El emisor usaba `andi.` en cada ANDI de MIPS y en la compuerta de
  cada operación de la FPU, `andis.` a la entrada de cada bloque con COP1 y
  `mtcrf 0x38` en cada salida a C. Fuera (rlwinm, li+ori+and, mtocrf).
- **FPU residente** (`src/cpu/rec_emit_fpu.h`): los 12 `cpu->fpr[p]` más
  usados del bloque en f14–f25, casa siempre al día, clasificación con
  `fabs` y `fcmpu` contra constantes en f27–f31 (nada pasa por enteros),
  inexacto por `r − a == b ∧ r − b == a` en vez del TwoSum, FCR31 perezoso
  en cr2 (C, causa I, bandera I, compuerta) escrito al salir del bloque,
  LWC1/SWC1 directo entre la RDRAM y el registro, CVT.S.W/CVT.D.W con
  `fcfid`. Prueba nueva `executed_sequences_match_interp` (3000 secuencias
  de COP1 en un bloque contra el intérprete, con memoria de E/S), que
  encontró tres errores antes de medir (alias fd = fs en CVT.S.D y ADD,
  CTC1 sin escribir los enteros modificados, y el camino lento de LWC1
  cargando el registro residente de la RDRAM: Perfect Dark abortaba).
- **COP0 sin el intérprete**: MTC0 a EntryLo0/1, PageMask e Index nativos
  (el bloque sigue); TLBWI/TLBWR con `rec_h_tlbw` (ktlb al día solo para
  esa entrada, sin comparar las 32); ERET con `rec_h_eret` y salida por
  `exit_jr` como un JR. GoldenEye hacía 2200 llamadas al intérprete por
  campo por el refill (1,9 ms).
- **Caché de código por tramos**: 32 MB (el techo por el alcance de los
  saltos al preámbulo) en 8 tramos; llena, se desaloja el más viejo en vez
  de vaciar todo (los vaciados completos recompilaban ~7000 bloques de
  golpe: campos de 52 ms en el log de referencia de main). Cada bloque decide
  si llama a C con `bl` o `mtctr` según dónde cae. Las invalidaciones de
  GoldenEye (44 345 en el log de main) son en su mayoría páginas que vuelven
  en otro marco y ya se reutilizan desprendidas y enganchadas (perfil en
  qemu, 128 s: 30 105 invalidados, 29 171 desprendidos, 23 554 enganchados).
- **Páginas de 1 MB** (pedido de main): el heap de PSL1GHT ya sale de un
  mmapper de páginas de 1 MB (`ppu/librt/sbrk.c`), así que la RDRAM solo
  ganaría alineación; la caché de código no puede ir en memoria de 1 MB
  (con HEN no es ejecutable) y solo se alinea a 1 MB en `mk/rec-cache.ld`.
  El frontend anota al arrancar el tamaño de página real de RDRAM, rec, n64,
  caché y pila (syscall 351) y el archivo `rdram-1m` prueba
  `sysMemoryAllocate` de 1 MB para la RDRAM.
- Perfil de GoldenEye en qemu (128 s hasta la presa, por campo): 1590
  llamadas a C (COP0 1236, FPU 120, E/S 235), 11,9 instrucciones por bloque,
  0,80 de COP1 y 3,94 de memoria por bloque; registros más usados: v0 59 %,
  ra 57, a0 51, a1 42, sp 42, a2 33, v1 33, s0 31, at 30, a3 29, t6 26. Los
  dos bloques más calientes (3,1 % cada uno) eran un bucle de comparación
  partido porque un salto condicional apunta dentro del mismo bloque.
- **Saltos internos con destino dentro del bloque** («Saltos internos con
  destino dentro del bloque» en docs/rec-ppe.md): el camino tomado salta al
  código de la instrucción destino sin salir, reponiendo el estado de la FPU
  residente y de FCR31 que el código de ahí asume y ajustando `r29`. Y en
  un bloque sin JR/JALR, r26 es un registro más para el bloque (5 con los
  fijos). Más registros fijos entre bloques (s0 31 %, at 30 %, t6 26 %)
  queda pendiente: r27 y r28 están ocupados y liberarlos cuesta cargas por
  bloque; medir primero.

Validado en qemu-ppc64 (todo con el árbol final): unitarias 365/365 en los
cinco modos (incluida la FPU por memoria), nemu64-test 433 fallas (línea
base) con y sin registros fijos y con caché de 3 MB (93 desalojos), vitest
OK (también con 2 MB), `--lockstep` sin diferencias en nemu64-test (también
con caché de 3 MB y 92 desalojos), vitest y Perfect Dark 300 M ciclos;
`tools/build.sh test-host` 365/365 y `tools/test-roms.sh` en verde. El
lockstep de GoldenEye hasta la presa (7,5 G ciclos) pasó **sin
diferencias** con la FPU residente (9d1eba0: 283,6 M salidas comparadas) y
con el árbol final (15d6724: 204 M salidas, 1 vaciado y 6 desalojos); la
intro de Perfect Dark (3 G ciclos) también, con el árbol final. Dos
errores que solo el lockstep encontró: el camino lento de
LWC1 cargando el registro residente de la RDRAM (Perfect Dark abortaba) y
los saltos internos dejando basura en los registros escritos por las
instrucciones saltadas (nemu64-test, bloque 80014c84). `tools/build.sh`
sin advertencias.

**Medido en la PS3 real (2026-10-11, noche;** tablas en docs/perf-log.md,
«FPU residente, sin microcódigo…»; paquetes SLSA00093 «Salsa64 FPU» y
SLSA00095 «Salsa64 CPU FPU» —SLSA00094 es el de los estados guardados—,
con main 69051fc traído a la rama):
- GoldenEye, presa junto al camión: 17,5 ns por instrucción ejecutada
  contra 18,2 de main (−4 %), todo de los ayudantes de COP0 (llamadas a C
  1,56 → 0,73 ns, ktlb 0,78 → 0,20). **El código nativo sigue en ~47 ciclos
  por instrucción MIPS** con cualquier combinación de banderas: no lo
  limitan las instrucciones emitidas sino, con toda probabilidad, el tamaño
  del código (~7 MB de bloques vivos contra 512 KB de L2; la caché va en
  páginas de 64 KB). Ese es el siguiente trabajo.
- **La FPU residente resultó más lenta** en el bench del mismo paquete
  (add.s 40,0 → 30,9 MIPS, mixto 168 → 135) y en GoldenEye no se distingue:
  queda apagada por omisión (`SALSA64_REC_OPT_FPU_RES`, archivo
  `jit-fpu-residente`). Quitar el microcódigo sí suma: con la FPU por
  memoria, mixto 156,6 → 168,2, add.s 35,7 → 40,0, cvt 40,7 → 50,6.
- Zelda, SM64 y Mario Kart a 59,6 campos/s, sin regresión (emulación por
  campo 5–20 % menor que main). Perfect Dark baja a 49–50 campos/s en un
  nivel por espera del RSP (SPE de la HLE gráfica, 14 ms por campo, 131
  texturas decodificadas por tarea): lado gráfico, no el recompilador.
- Páginas: la RDRAM ya va en 1 MB; `rdram-1m` no se midió (nada que ganar).

A/B que siguen en el paquete (archivos vacíos en /dev_hdd0/salsa64/):
`jit-fpu-residente`, `jit-cop0-viejo`, `jit-sin-tramos`, `jit-viejo`,
`rdram-1m`, `perfil`.

## 2026-10-11 — SM64 se colgaba con los efectos de framebuffer: el espejo del framebuffer sigue al dibujo, no al VI

**Síntoma** (host, main 4baa6df): Super Mario 64 con `SALSA64_FBFX=2` dejaba
de mandar tareas gráficas en la 96 (campo ~343, paso del logo a la cara de
Mario; el audio seguía). Bisección con `--first-parent` en la rama de los
efectos: lo rompe b4d9d61 («Host: el modelo de copias sigue la política de
la consola»); ya había fallado en bf836f2 (espejo del framebuffer por
campo) y lo tapaba sin querer que el host copiara todos los framebuffers
al terminar cada tarea (su `vi_width` quedaba en 0).

**Causa** (docs/framebuffer.md, «Espejo del framebuffer»): el espejo
copiaba el framebuffer presentado en cada campo. SM64 dibuja un
framebuffer (tarea 95), carga por DMA ~200 KB de datos de la cara sobre dos
de sus framebuffers (`390000`–`3c2780`) mientras el VI todavía muestra uno,
y lo presenta un campo más: la copia pisaba esos datos (el código de la
cara los lee en el campo 354) y el juego se colgaba. Copiar «una vez al
presentar» no alcanza (ese framebuffer se presenta por primera vez después
del DMA). Además el host copiaba el alto conjeturado (`lh`) en vez de las
filas que muestra el VI: la intro de Conker se colgaba en la tarea 34 (en
la PS3, que copia las filas del VI, no).

**Arreglo**: lo dibujado en un framebuffer se copia a la RDRAM una vez por
dibujo, al terminar la tarea si el VI ya lo mostró alguna vez y si no al
presentarlo (`rdp_targets_mirror_at_task_end` / `rdp_targets_mirror_wanted`
en `src/rdp/targets.c`), igual en `src/rdp/rdp.c` (PS3) y en
`src/host/raster.c`. Pruebas: `targets_frame_mirror` (política) y
`raster_frame_mirror_sm64` (el rasterizador del host de punta a punta,
`tests/unit/test_raster.c`; falla con el raster.c de main).

**¿La PS3?** El camino existe ahí: `frame_mirror` de `rdp.c` copia el
presentado en cada campo desde bf836f2 y su copia llega un campo después,
o sea después del DMA del campo 343 (el host con ese retraso simulado
también se cuelga en la tarea 96). No hay registro de SM64 en la PS3 con el
espejo: `build/ps3-logs/sm64.log` no tiene «cuadros copiados» (anterior a
bf836f2). Hay que confirmarlo en la consola.

**Verificación** (host nativo, respaldos fuera del repo): SM64 660 tareas
en 1500 campos (como con `FBFX=1`) y hasta el castillo con el medidor de
energía igual que con `FBFX=1`; Conker 395 tareas (main 34; igual que
`FBFX=1`); Zelda: pausa (fondo difuminado) y pantalla de equipo con Link,
53 capturas idénticas a main (partida nueva «A» creada con el control);
Mario Kart en Luigi Raceway (`mk64-luigi.pad`) y GoldenEye idénticos a main
(el pad no llega al túnel); Banjo con la misma imagen de la HLE.
`test-host` 371/371, `test-roms` en verde (gfxtest 35/35 en las tres
variantes), `build.sh` sin avisos, `test-phase2` en verde (gfxtest 35/35
en RPCS3).

**Pendiente (PS3 real):** SM64 del logo a la cara de Mario y al castillo,
la cámara del túnel de Luigi Raceway, la pausa y la pantalla de equipo de
Zelda, Conker. Queda una carrera solo de la PS3: la copia llega a la RDRAM
uno o dos campos después de traducir la tarea, así que un DMA sobre un
framebuffer en el mismo campo en que el RDP lo terminó podría quedar
debajo (en SM64 pasan ~4 campos).

## 2026-10-11 — Efectos de framebuffer: render a textura y copias RSX → RDRAM (rama hopeful-dirac)

Diseño y hallazgos en `docs/framebuffer.md`; costos en `docs/perf-log.md`.
Resuelto y confirmado en la PS3 real con el paquete SLSA00089 («Salsa64
framebuffer»): el fondo de la pausa de Zelda OoT (nítido), Link en la
pantalla de equipo (dentro y fuera de la casa, cambiando de equipo), la
cámara del túnel de Luigi Raceway en Mario Kart 64 (el CPU fabrica esa
textura leyendo el framebuffer), gfxtest 35 de 35 (tres casos nuevos: render
a textura, copia a la RDRAM de una color image grande y de una chica que
solo lee el CPU) y sin regresiones: SM64 59,8, GoldenEye en Dam 45,8 (su
rango), Banjo 56–57, Conker 59,8 campos/s.

**Lo que hizo falta** (todo medido con datos de la consola, no a ciegas):

- Procedencia de las texturas en la HLE, tabla de destinos con política
  (`src/rdp/targets.c`), render a textura desde la superficie del RSX,
  copias RSX → RDRAM sincronizadas con la emulación, espejos asíncronos de
  las imágenes chicas y del framebuffer presentado cada campo, cebado de
  las superficies desde la RDRAM, VISCVG como cobertura plena.
- `rsxkit` solo admitía 8 superficies: la pausa de Zelda tiene 9 color
  images vivas más el scratch, la novena no se creaba y Link salía con
  basura del heap. Ahora 16, y el log cuenta «destinos sin superficie».
- La copia pedida se buscaba solo por dirección: `3da800` es el
  framebuffer (320) y el búfer de Link (64) a la vez y se copiaba el
  framebuffer entero (10 ms) en vez de Link.
- Con más de dos copias esperadas en una tarea, la tercera pisaba el
  búfer de la primera antes de que el RSX la leyera.
- La HLE emite dos color images imposibles al abrir la pausa (anchos de
  1495 y 3770): reciclaban ranuras buenas; ahora se ignoran.
- La cobertura que Zelda guarda para su filtro es una color image de 8
  bits de intensidad (`siz 1`): también se copia (las CI8 no).
- En la pausa alternan tres z-buffers; con dos profundidades se destruían
  y creaban a cada tarea (`rsxkit_finish` en cada una). Ahora cuatro.
- El modelo del host no llamaba a `rdp_targets_present`: copiaba todos los
  destinos de cada tarea y por eso «funcionaba» donde la consola no.

**Verificación (árbol final, con main traído):** `test-host` 369 pruebas
(617 939 verificaciones, 0 fallidas), `test-roms` en verde, `build.sh` sin
avisos, `test-phase2` en RPCS3 en verde (gfxtest incluido); `test-rec` en
tarea aparte (la parte unitaria 369/369 con el recompilador). En el host, gfxtest 35/35 con los tres microcódigos y la
pausa de Zelda en el bosque Kokiri y dentro de la casa (pads en
`docs/pads/`: `zelda-kokiri.pad` + `3600:START;…` llega a EQUIPMENT fuera
de la casa).

**Convivencia con la escala interna 1×–5× (fusión del mismo día).** La
escala interna llegó con escala por destino (`rdp_target_scale`,
`surf_scale`: con la objetivo 3×, el framebuffer de 320 × 240 va a 3×, Link
de 64 × 112 a 5× y un framebuffer de 640 × 480 a 1×), tope de memoria y
«Aplicar ahora»; los efectos de framebuffer suponían una escala global.
Al juntarlos (detalle en `docs/framebuffer.md`, «Escala por destino»):

- Todo lo que pasa de puntos lógicos a píxeles de una superficie usa la
  escala de esa superficie (`rdp_surface_rect`, `rdp_target_tex_map`):
  copias RSX → RDRAM (esperadas, grandes y espejos chicos), espejo del
  framebuffer presentado, render a textura (filas ligadas en la escala del
  destino leído; u y v normalizadas, así que el destino en que se dibuja
  puede tener otra) y el scratch, que toma tamaño y escala de la
  superficie copiada. Con la escala global, la copia de Link a 5× leía
  solo una esquina de su superficie y la de un destino a 1× se salía.
- Profundidades: si una z image la usan destinos de escalas distintas, se
  rehace en la misma entrada con el mayor tamaño de los dos (antes
  quedaban dos con la misma dirección y zclear borraba la vieja).
- Destinos ligados sin z image (cebado, scratch, dibujos sin z): iban
  con la profundidad de la pantalla, más chica que el destino a 4×–5× en
  720p (riesgo de que el RSX falle o recorte). Ahora la profundidad de la
  pantalla se agranda al mayor destino posible al elegir la escala
  (`rsxkit_depth_reserve`, `rdp_zeta_reserve`) y rsxkit se niega a ligar
  con una zeta más chica (`RSXKIT_EDEPTH`; el dibujo se salta y el log
  cuenta «dibujos sin ligar»). Elección explicada en `src/rdp/rdp.h`.
- Memoria: peor caso de 14 superficies (8 destinos, el scratch, 4
  profundidades y la profundidad de la pantalla agrandada), 102,5 MiB a
  5×: el tope pasó de 96 a 112 MiB (con 96, 5× se habría bajado a 4×). El
  scratch cuenta en el tope; `make_room` lo suelta primero y nunca recicla
  un destino que una textura pueda estar leyendo.
- Presentación: a múltiplo entero solo si llena al menos 3/4 del alto
  (`rdp_present_integer`); 2× en 720p salía a 640 × 480 en el centro.
- «Aplicar ahora» (`salsa64_rdp_set_scale`) suelta destinos, scratch y
  profundidades y descarta las copias en vuelo.

Verificación: `test-host` 374 pruebas (645 613 verificaciones, 0
fallidas; en `targets`, escala y peor caso, copia RSX → RDRAM simulada a
1×–5×, textura entre escalas y presentación entera), `test-roms` en verde
(gfxtest 35/35 con los tres microcódigos en el host), `build.sh` sin
avisos, `test-phase2` en RPCS3 en verde (gfxtest embebido y F3D 35/35; en
el menú, F3D a 2× tras «Aplicar ahora», 35/35) y gfxtest en RPCS3 con
`video.cfg` forzando la escala: 35/35 a 1×, 3× y 5×. Por confirmar en la
PS3 real: pausa y pantalla de equipo de Zelda y túnel de Mario Kart a 1×,
2× y 5×, y costo de las copias desde 5×. Con la profundidad de la
pantalla agrandada (commit aparte): `test-host` 375, `test-roms`,
`build.sh` y gfxtest en RPCS3 a 1× y 5× repetidos, todo en verde.

**Pendiente (otros trabajos):**

- [ ] Polígono transitorio (verde o amarillo, ~1 s) al abrir la pausa de
      Zelda en la PS3: volvió al caber los dos pares de búferes de Link;
      la causa más probable eran las dos profundidades (arreglado a cuatro
      en el último paquete, por confirmar en la consola).
- [ ] Las copias chicas que se repiten cada tarea sin que ningún dibujo
      las lea (Link redibujado en cada tarea de la pausa) se esperan solo
      las dos primeras veces y después van sin esperar: medir que la
      pausa baja de ~22 ms de espera por tarea a casi nada.
- [ ] Líneas del logo de N64 de Zelda: no era la presentación (el host
      también las dibuja): HLE del texto «NINTENDO 64» con la segunda
      textura de ruido.
- [ ] Túnica de Link con un defecto de textura (previo a la rama, también
      con `fbfx.cfg` en 0).
- [ ] Perfect Dark y Conker: copias del z-buffer (rectángulos que leen la
      z image como color image), sombra y desenfoque de Conker; color
      images de 32 bits no se copian ni se ceban.
- [ ] Reducir s× → 1× con filtro de caja (hoy bilineal del RSX en el
      centro de cada bloque) y, si pesa, la conversión en un SPE.
- [ ] Controles que a veces no responden en la pausa (también en SM64):
      arreglado en main (fba3a95), por confirmar aquí.

## 2026-10-11 — Recompilador: perfil por causa y menos salidas a C en GoldenEye y Perfect Dark (rama `upbeat-driscoll-1dd271`, paquete SLSA00088 «Salsa64 JIT»)

Hecho (sin la PS3: la noche del 10 al 11 se trabajó en qemu; la medición
en la consola queda para el día de pruebas):

- **Perfil del recompilador por causa** (`src/cpu/rec_prof.h/.c`, docs/rec-ppe.md
  «Perfil por causa»): tiempo con el timebase del PPE y cuentas de código
  generado, llamadas a C (cargas y almacenamientos lentos por tipo de
  dirección, instrucciones al intérprete por clase, excepciones), intérprete
  en el despachador por motivo, eventos por tipo (el del RSP incluye el
  sondeo y lo dormido esperando al SPE), compilar, `ktlb`, bucles de espera
  y salidas por tipo. En la PS3 lo enciende `/dev_hdd0/salsa64/perfil` y
  sale cada 5 s en ms por campo con las estadísticas; en el host,
  `SALSA64_REC_PROF=N` (cuentas cada N campos y, al final, los bloques más
  ejecutados con sus palabras y el uso de registros; `tools/mips-disasm.py`
  los lee).
- **Lo que las cuentas mandaron arreglar** (GoldenEye en la presa, qemu,
  por campo, antes → después): instrucciones interpretadas 13 900 → 500
  (cada COP0 terminaba el bloque y el intérprete seguía hasta el fin del
  bloque básico: ahora el bloque enlaza con el siguiente y `rec_h_exec`
  sincroniza la TLB del recompilador; tras un ERET tampoco se interpreta);
  traducciones de la TLB 14 000 → 360 revisiones (`x_sync` compara las 32
  entradas con una instantánea y borra solo las páginas de las que
  cambiaron; además prellena las de la entrada nueva); CACHE al intérprete
  1000 → 0 (nativo); salidas por JR 820 → 350 (`exit_jr` recorre la cadena
  de xhash); entradas desde C 1590 → 840. Perfect Dark tiene sus datos en
  una página de 4 MB mapeada (3200 cargas por campo por C en la intro): la
  `vtlb` pasó a ser `ktlb` (16 384 páginas, con el bit D) y las cargas y
  almacenamientos a direcciones mapeadas la prueban en línea («Datos
  mapeados por la TLB»). GoldenEye además compilaba cada (vpc, marco
  físico) como bloque nuevo al paginar su código (93 000 bloques y 6
  vaciados de la caché en 128 s): un bloque vivo de la misma vpc con los
  mismos bytes se reutiliza en la física nueva (`salsa64_rec_x_rekey`).
- **Bloques desprendidos** («Bloques desprendidos» en docs/rec-ppe.md):
  GoldenEye pagina su código y cada página vuelve en otro marco: compilaba
  cada (vpc, marco) de nuevo (101 900 bloques y 7 vaciados completos de la
  caché en 128 s). Un bloque mapeado invalidado se desprende con una copia
  de sus bytes y se engancha donde vuelva su página con los mismos bytes
  (parcheando la palabra de `ktlb` de su entrada): 40 850 bloques, 28 200
  enganchados, compilaciones por campo en la presa 22 → 0,7, vaciados 7 → 3.
- **Superbloques** («Saltos condicionales internos»): el perfil por bloque
  dio 5,3 instrucciones por bloque; un condicional hacia adelante ya no
  corta el bloque (su camino tomado sale enlazado, el no tomado sigue con
  los registros en el PPE). Los tres bucles más calientes de GoldenEye eran
  parejas de bloques.
- Caché de código de 16 → 24 MB (GoldenEye la llenaba cada ~40 s; ~900 bytes
  de PPC por bloque) y el camino de la `ktlb` en 8 rutinas compartidas del
  preámbulo (3 palabras por acceso en vez de ~22).
- Opción `SALSA64_REC_OPT_LEGACY` (`/dev_hdd0/salsa64/jit-viejo`,
  `SALSA64_REC_LEGACY=1`): el comportamiento anterior en el mismo binario,
  para el A/B en la consola. Bits `SALSA64_REC_OPTS` (host) para apagar una
  pieza a la vez al acotar una diferencia.
- **La referencia de `--lockstep` no tenía HLE** (gráfica ni de audio): no
  recibía la interrupción del DP de las tareas con HLE y GoldenEye y Perfect
  Dark, que ya dibujan, divergían igual con el camino viejo (bisect con los
  bits de arriba: todos fallaban en el mismo ciclo). Ahora lleva la misma
  HLE con su estado; con eso Perfect Dark pasa (200 M ciclos) y las corridas
  largas se anotan abajo.

**Para el día de pruebas en la PS3 (orden sugerido: al final, tras recorte →
líneas del logo → framebuffer):** la consola no respondía en la noche (FTP
sin conexión), así que el paquete está sin subir: desde este worktree
`PS3_IP=x.x.x.x tools/deploy-ps3.sh ftp build/salsa64-jit/salsa64-jit.pkg`
(el worktree no tiene `tools/ps3.conf`); instalarlo desde Package Manager
(SLSA00088 «Salsa64 JIT»), crear por FTP `/dev_hdd0/salsa64/perfil` (vacío:
`curl -T /dev/null ftp://x.x.x.x/dev_hdd0/salsa64/perfil`) y poner
`embedded` = `file:goldeneye.z64`.
1. Abrir «Salsa64 JIT», jugar la presa ~3 min (misma ruta que las
   mediciones anteriores), salir, bajar `/dev_hdd0/salsa64/salsa64.log`.
2. Crear `/dev_hdd0/salsa64/jit-viejo` (vacío) y repetir: es el «antes» con
   el mismo binario. Borrarlo después.
3. Si hay tiempo: Perfect Dark en un nivel (es el que más gana con los
   datos mapeados), y un minuto de Zelda, SM64 y Mario Kart (sin regresión).
Lo que hay que leer del log: la línea «campos/s … emulación X ms por campo»
y las de «perfil JIT por campo» (nativo, llamadas a C, intérprete, eventos,
compilar, ktlb, despachador) en el tramo de la presa; anotarlas en
docs/perf-log.md («Recompilador: perfil y salidas a C»).

Pruebas (todo con el árbol final, 2026-10-11):
- `tools/test-rec.sh --lockstep`: unitarias 351/351 en los cuatro modos,
  nemu64-test 433 fallas (línea base) con y sin pin+eager, vitest OK,
  `--lockstep` sin diferencias en nemu64-test y vitest.
- `tools/test-roms.sh`: nemu64-test 433 (HLE) y 136 (LLE), vitest,
  controles, savetest ×3, gfxtest ×3, audiotest: verde. `tools/build.sh
  test-host` 351/351.
- `--lockstep` de Perfect Dark (3 G ciclos, intro) y de GoldenEye con
  `goldeneye-dam.pad` hasta jugar en la presa (7,5 G ciclos, 283 M salidas
  comparadas, 1 h 46 min de qemu): **sin diferencias**.
- `tools/build.sh` (todos los programas) y el paquete: sin advertencias.
- **PS3 real (2026-10-11, tarde), A/B con el mismo paquete en la presa de
  GoldenEye:** 43,0 → 49,4 campos/s de media (+15 %), 37,5 → 50,6 MIPS
  (+35 %), 26,0 → 19,6 ns por instrucción ejecutada (−25 %); intérprete
  2,8 → 0,8 ms por campo, ktlb 3,8 → 1,0, despachador 1,0 → 0,0. **El
  nativo es el 80 % y cuesta ~50 ciclos del PPE por instrucción MIPS**, igual
  antes y después: ahí va el siguiente trabajo. Zelda, SM64 y Mario Kart
  a 59,4–59,5 campos/s (sin regresión). Tabla en docs/perf-log.md.

Pendiente / siguiente paso: con el desglose de la PS3 decidir el ataque al
código nativo (lo que no se ve en qemu): registros de la FPU residentes en
f14–f31 dentro del bloque (hoy cada operación pasa por `cpu->fpr`:
load-hit-store entre operaciones dependientes; GoldenEye 0,78 por bloque),
más registros fijos entre bloques (r26–r28 liberables; `at`, `s0`, `t6` son
los más usados sin fijar), MTC0 EntryLo/TLBWR/ERET con un ayudante propio
en vez de `exec_one` (GoldenEye: 2200 llamadas por campo del refill), y
seguir bajando el tamaño del código (la FPU y las salidas de los saltos
internos) para que la caché no se vacíe en la presa.

## 2026-10-11 — Pulsaciones perdidas del control: el bucle de presentación dejaba de leer los mandos (rama vibrant-hawking)

Reporte (el autor, PS3, 2026-10-10 y 11): a veces un botón no responde
hasta pulsarlo muchas veces (R en la pausa de Zelda, el stick en la
selección de ranura, también SM64); después «responde normal».

**Causa (log de la PS3 con los mandos instrumentados):** el hilo de
presentación pasó **7,4 s sin leer los mandos** en la selección de ranura
de Zelda (y 0,6–0,9 s en otros momentos) mientras presentaba 667 campos.
`drain()` consumía la cola hasta vaciarla, pero con el flip en vsync y la
emulación al mismo ritmo, mientras se presenta un campo llega el
siguiente: la cola no se vaciaba y el bucle no volvía a `update_pads`
hasta que los dos ritmos se desfasaban. Ahora `drain()` presenta solo lo
encolado al entrar. Lo que sí se leía llegaba siempre al juego (cada
«control 1» con su «PIF → juego» 1–3 campos después).

Medido en el host (`salsa64-run` nativo, mismas pulsaciones por campo):
- **Zelda lee el control en cada campo y acumula las pulsaciones**: en la
  pausa, R de 1, 2, 3 y 6 campos giran la página las 9 veces.
- **Zelda ignora R mientras gira la página** (~35 campos, 0,6 s): dos R
  separadas 15 o 30 campos dan un solo giro; a 45 o más, dos. Es del juego.
- **SM64 lee el control cada 2 campos**: una START de 1 campo en la fase sin
  lectura se perdía (3 de 6); con la retención del PIF, 6 de 6.

Hecho:
- **`drain()` acotado** (`src/frontend/main.c`): solo los campos que había
  al entrar; el bucle vuelve a leer los mandos en cada vuelta.
- **PIF** (`src/core/pif.c`): cada flanco de pulsación queda retenido
  (`held`) hasta salir en una respuesta de estado (comando 0x01); la
  identificación y el pak no lo gastan; desconectar o reiniciar lo olvida.
  Solo flancos: con todo lo pulsado, una A soltada 4 campos entre dos
  pulsaciones se perdía en SM64 (visto en la PS3). Guarda lo entregado
  (`sent`, `reads`) para el log. Prueba `pif_held_press`.
- **Buzón presentación → emulación**: además del seqlock, OR atómico de los
  botones recién pulsados (`pad_box.pressed`) que emulación toma y pone en
  0; los controles se aplican justo antes de emular el campo (antes, antes
  de la espera del ritmo: hasta ~12 ms más viejos).
- **Log de la PS3, solo en flancos**: «control N: … (campo M)» en
  `update_pads` y «PIF → juego: control N: … (campo M, lectura K)» cuando
  cambia lo que el PIF entregó (cola de 32 entre hilos, la escribe
  presentación). Cada 5 s: «controles: X lecturas de los mandos (hueco máx
  Y ms); el juego leyó el control 1 Z veces (W/s)». El host imprime el
  mismo «PIF → juego» con `--pad`.
- `docs/hilos-ppe.md` al día (fila de controles y de flancos del PIF).

**PS3 real** (paquete «Salsa64 control», SLSA00092; logs fuera de git):
- Antes de acotar `drain()`: huecos de 7402, 929, 831, 595 y 251 ms sin
  leer los mandos; el autor: el stick no bajaba en la selección de ranura.
- Después, Zelda: «ya funciona»; hueco máximo 19–31 ms en el juego (312 ms
  una vez, en la carga del título a la selección de ranura); 12 de 12
  pulsaciones entregadas.
- SM64 con la retención por flancos: «todo funciona perfecto»; hueco
  máximo 8,8–38 ms, el juego lee el control 30 veces/s; 75 de 75
  pulsaciones (A machacada, B, START) entregadas al juego.
- `embedded` quedó en `file:sm64.z64`.

Suites en verde con el código final: `tools/build.sh test-host` (356
pruebas), `tools/test-roms.sh`, `tools/build.sh` (sin avisos),
`tools/test-phase2.sh` (RPCS3) y `tools/test-rec.sh` (nemu64-test con las
433 fallas de siempre).

Pendiente:
- [ ] Integración en main.
- [ ] El hueco de 312 ms en la carga del título de Zelda (la presentación
      espera algo durante un campo de 170 ms de emulación): sin efecto en
      el juego; revisar si aparece en otra escena.

## 2026-10-11 — Kokiri y su tienda en colores planos: eran los fondos prerrenderizados (tarea JPEG y S2DEX)

Diagnóstico (rama `fervent-hodgkin-d38e1e`, en la Mac, sin PS3):
- **No era el combiner ni la HLE de F3DZEX.** La captura del host del
  campo 14400 (verde puro y un rectángulo azul) se reproduce bit a bit con
  la partida nueva del trabajo de framebuffer (`build/oot/pad4.txt` de su
  worktree, `MAX_PADS` subido a 2048 como en su rama). El píxel (100,150)
  lo escribe un dibujo **sin textura** de la propia sala: G_TEXTURE
  apagado, combiner SHADE·PRIM con PRIM = (0,161,0), iluminado; el azul es
  PRIM = (0,0,255) solo. La display list está en el archivo de la sala
  (segmento 3) y su cabecera de malla es de **tipo 1
  (ROOM_SHAPE_TYPE_IMAGE)**: imagen de 320 × 240 RGBA16 que dentro del
  archivo es un JPEG (`FFD8FFE0…JFIF`).
- La casa de Link (tras despertar, cámara cenital fija) y la tienda Kokiri
  son **salas prerrenderizadas**. El juego dibuja una malla «proxy» en
  colores PRIM planos solo para llenar el z-buffer y encima copia el fondo
  con S2DEX (`G_LOAD_UCODE` → `G_BG_COPY` con un `uObjBg` → vuelta a
  F3DZEX); el fondo lo decodifican al cargar la sala el CPU (Huffman) y 75
  tareas del RSP de tipo 4 (IDCT y YCbCr → RGBA5551). Salsa64 saltaba el
  tramo S2DEX (entrada del 2026-10-10 del cuelgue de Zelda) y las tareas
  JPEG terminaban sin hacer nada: se veía la proxy. Verde/azul en la casa,
  amarillo/naranja en la tienda.
- Referencias en la misma cámara: mupen64plus (GLide64 + rsp-hle, misma
  partida y pad) muestra el fondo de la casa; con `--lle=auto` (el
  microcódigo JPEG real) la RDRAM tiene la imagen bien decodificada y con
  la HLE de antes, basura.
- **El cierre del 2026-10-10 («sol cenital») vale para el bosque al aire
  libre** (balcón, campo ~3700): el piso hexagonal sale igual en
  mupen64plus. Lo que se reportó como «piso del bosque verde puro al salir
  de la casa» era la casa vista desde arriba.

Hecho:
- **HLE de la tarea JPEG de OoT** (`src/rsp/jpeg/`, nuevo): reconocida por
  el CRC-32 del microcódigo (4 KB, sin texto); por macrobloque, × tabla de
  cuantización ×16, zigzag, IDCT 8 × 8, YCbCr → RGBA5551 en su mismo lugar
  (modo 0 4:2:2 y modo 2 4:2:0). Algoritmo de `jpeg_decode_PS` de
  mupen64plus-rsp-hle (GPLv2+, `docs/THIRD_PARTY.md`), reescrito. Corre en
  el **PPE**, dentro del arranque de la tarea (`core/rsp.c`): 0,61 ms por
  sala en la Mac (300 macrobloques), una vez al cargarla.
- **S2DEX2 dentro de una tarea F3DEX2** (`src/rsp/gfx/s2dex.c`, nuevo): el
  `G_LOAD_UCODE` cuyos datos dicen «S2DEX … 2.x» ya no se salta; se
  interpretan G_DL/G_ENDDL, G_MOVEWORD de segmentos, othermode, comandos
  del RDP y **G_BG_COPY / G_BG_1CYC**: la imagen (con vuelta en x e y desde
  imageX/imageY) se carga a la TMEM por tiras con LOADTILE (las filas que
  caben en 4 KB, 2 KB con paleta; 6 filas de 320 en RGBA16) y cada tira es
  un TEXRECT (dos si da la vuelta en x), como hace el microcódigo: pasa por
  `rdp.c` como cualquier rectángulo, sin tocar `src/rdp` ni rsxkit. Las
  tiras se reutilizan entre cuadros por la caché de texturas (40 por
  cuadro, ~300 KB). Otros microcódigos ajenos se siguen saltando (gfxtest).
  Formato de `uObjBg` según GLideN64 (solo consulta).
- `salsa64-run --lle-check-jpeg`: las tareas JPEG al microcódigo real
  (implica `--lle=auto`) y la HLE sobre una copia de la RDRAM; compara al
  TASKDONE (no en el yield: el planificador de OoT interrumpe a veces una
  tarea JPEG para correr una gráfica). Estadísticas nuevas: `jpeg` (tareas,
  macrobloques, ms con `rsp.clock_us`) y `bg` (fondos de S2DEX) en el
  resumen del host y en el log de la PS3 («JPEG: N tareas … ms del PPE» al
  terminar cada tanda y «N fondos de S2DEX» en la línea `gfx:`).
- Receta de la tienda en el host (`docs/pads/README.md`): pad de Kokiri +
  `--poke` de la lista de salidas de la casa (0x0211 → 0x00C1).
- Pruebas: suite `jpeg` (5: DC por cuadrante, croma en modo 0, orientación
  del zigzag, IDCT contra una referencia en doble precisión, varios
  macrobloques y parámetros malos) y en `gfx` `gfx_s2dex_bg_copy`,
  `gfx_s2dex_bg_wrap` (vuelta en x e y, tres tiras) y `gfx_s2dex_bg_scaled`
  (G_BG_1CYC a escala 2; un S2DEX 1.x se sigue saltando).

Medido (host):
- **JPEG contra el microcódigo real**: casa (partida nueva, dos cargas, 150
  tareas, 153 600 píxeles) 99,0 % idénticos, 0,8 % a ±1 en un canal de 5
  bits; casa y tienda con la partida del autor, 99,2 % y el resto a ±1. El
  sesgo que quedaba en G (+1 en el 1,1 % de los píxeles) se quitó
  truncando cada producto de G como el punto fijo del RSP; lo que queda es
  ruido de la IDCT del RSP. 256 píxeles distintos de más en la partida
  nueva: un macrobloque que el **LLE** no escribe al reanudar una tarea JPEG
  interrumpida (yield); la HLE no se interrumpe. Pendiente del LLE, abajo.
- **Copia del fondo exacta**: en el campo 14400 el cuadro es la imagen
  RGBA16 de la RDRAM píxel a píxel salvo HUD, Link y Navi (94,7 % del
  cuadro). Contra mupen64plus (640 × 480 con filtro, reducido): mediana
  5,5/255, 91 % a ≤ 16/255.
- **Otros juegos sin cambios**: SM64 (pad del castillo), Mario Kart 64
  (demo), GoldenEye (pad de Dam) y Perfect Dark (intro): 48 capturas cada
  300 campos y los cuadros finales idénticos antes y después. En Zelda con
  el pad de Kokiri, idénticas dentro de la casa; en el balcón cambian
  Navi y las motas (378 píxeles): el juego diverge porque ahora su
  `gZBuffer` de la RDRAM (donde decodifica el JPEG) tiene la imagen y no
  basura, y el CPU lo lee.
- Local store del SPE 0: texto 97,0 KB (+4,8 KB de `s2dex.c`), pila libre
  ~13 KB y la cadena más honda ~8 KB (`-fstack-usage`).

Verificado:
- [x] `test-host` 363/363 (Docker, ASan/UBSan) y nativo; `test-roms`
      (nemu64-test 433 / 136 con LLE, vitest, controles, savetest,
      gfxtest 32/32 ×3, audiotest); `tools/build.sh` sin advertencias;
      `test-phase2` en RPCS3 (gfxtest 32/32 F3DEX2 y F3D con la HLE en el
      SPE, menú con cierre limpio); `test-rec`: ver abajo.
- [x] **PS3 real** (paquete «Salsa64 Kokiri», SLSA00091, 3b95776,
      `embedded` = `file:oot.z64`; el autor, 2026-10-11): la tienda Kokiri y
      la casa de Link «ya se ven bien». En el log, dos salas: «JPEG: 75
      tareas, 300 macrobloques en 67,8 / 67,3 ms del PPE», 0 con parámetros
      malos, y 14–47 «fondos de S2DEX» por ventana de 5 s. Las ventanas de
      100 campos con la carga de sala quedan en 57,3–57,6 campos/s con el
      peor campo en 37–39 ms de emulación (≈ 2 campos de tirón durante la
      transición; el resto, 5–8 ms por campo). Log en
      `build/ps3-logs/salsa64-kokiri.log` (fuera de git).
- **Decisión:** el JPEG se queda en el PPE (una vez por sala, durante la
  transición). Es ~110 veces más lento que en la Mac: flotantes con
  conversiones a entero (en el PPE pasan por memoria) y `lrintf`/`floorf`
  de la biblioteca; con una IDCT en punto fijo debería bajar a pocos ms
  sin pasar a un SPE (pendiente abajo).

Pendiente:
- [ ] HLE JPEG en el PPE: IDCT y conversión a RGBA5551 en punto fijo (sin
      flotantes ni `lrintf`/`floorf`), volver a medir contra el
      microcódigo real (`--lle-check-jpeg`) y en la PS3; meta < 10 ms por
      sala (hoy 68 ms).
- [ ] LLE: al reanudar una tarea JPEG interrumpida (OS_TASK_YIELDED) el
      último macrobloque queda sin escribir (tarea 49 de la partida nueva).
- [ ] LLE + RDP del host: los TEXRECT en modo copia que emite el S2DEX real
      salen negros con rayas (`--lle=all`, campo 14400); con la HLE el fondo
      sale bien.
- [ ] Otros usos de S2DEX: tareas que son S2DEX enteras (`SALSA64_GBI_S2DEX`,
      sprites) y S2DEX 1.x dentro de F3DEX 1.x siguen sin HLE.
- [ ] Majora's Mask y otros con tareas JPEG de otro microcódigo (Pokémon
      Stadium usa el mismo algoritmo con otro código: añadir su CRC al
      verlo).

## 2026-10-10 — Menú para elegir ROM (estilo XMB) y vuelta al menú con cierre limpio

Hecho (rama `intelligent-yalow-4dd14d`, desde main e7184b7):
- **Sin `embedded`, menú** (`src/frontend/menu.c`, `romlist.c`). Con
  `embedded`, todo arranca directo como antes (las pruebas, sin cambios).
  - Lista: `.z64/.v64/.n64` de `/dev_hdd0/salsa64` y
    `/dev_usb000…007/salsa64`, leyendo solo la cabecera (los tres órdenes de
    bytes). Ignora los «._*» de macOS. Título interno (0x20; si no hay, el
    nombre de archivo), código, región, tipo de guardado (tabla o
    guardado.cfg) y tamaño. Ordenada por título.
  - Disposición inspirada en el XMB, sin recursos de Sony: columna vertical
    con la fila elegida más grande y el nombre de archivo debajo, columna que
    se desliza con suavizado, panel de información que aparece al detenerse,
    fondo en degradado que cambia despacio con tres ondas animadas, texto con
    sombra, fundido de entrada y página de Opciones: fondo animado sí/no (en
    `SALSA64_DATA_DIR/menu.cfg`) y recorte del borde (el `overscan.cfg` del
    trabajo de presentación: Apagado, Normal 8/6, Amplio 16/12, o
    «Personalizado» si el archivo trae otros números; se relee al abrir cada
    juego). Íconos dibujados con rectángulos.
  - Controles: cruceta con repetición (pulsación nueva para dar la vuelta),
    izquierda/derecha para cambiar de página, X para abrir, triángulo para
    Opciones y círculo para volver. Recuerda la última ROM en
    `SALSA64_DATA_DIR/ultima-rom`. «Salir al XMB» va al final.
- **START + SELECT dentro del juego vuelve al menú.**
  - `main.c` quedó en sesiones de juego (`run_game`). `game_close` cierra
    en orden, con cada paso al log («cierre: …»): guardado al disco, audio,
    HLE gráfica y LLE con sus SPEs, recompilador, colas, destinos del RSX
    (`salsa64_rdp_reset`, que conserva la memoria mapeada). Termina en
    «cierre: estado limpio» o en «ATENCIÓN, quedó algo en vuelo».
  - `game_reset` pone en cero el resto (incluidos estáticos que antes
    sobrevivían: el último campo encolado, las ventanas de estadísticas, la
    línea del ISViewer). `audio_out` quedó reentrante. «Salir del juego» del
    sistema cierra la aplicación desde el juego o desde el menú.
- **lib/rsxkit (MIT):**
  - `rsxkit/text.h`: texto en un lienzo de la CPU. La fuente es la «fixed»
    8x13 de X11, de dominio público (`docs/THIRD_PARTY.md`).
  - `rsxkit/ui.h`: 2D en el RSX (rectángulos con degradado, texto con un
    atlas Scale2x y filtro lineal, ondas) sobre la pantalla del cuadro
    (`rsxkit_bind_frame`).
  - En RPCS3 la mezcla alfa dentro de **una sola** llamada de dibujo no ve
    lo dibujado antes en esa misma llamada (lo translúcido salía como sobre
    negro). Por eso va una llamada por cada elemento, con el estado en
    caché. Anotado en `lib/rsxkit/README.md`.
- Memoria: el menú reserva 2 MB mapeados para el 2D; la ROM va en un búfer
  a su medida, reservado al abrir cada juego y liberado al cerrarlo
  (`docs/memory-budget.md`).

Pruebas:
- Host: suite «menu», 15 pruebas: cabeceras en los tres órdenes, títulos,
  orden, directorio, última ROM, navegación, opciones, ajustes, atlas,
  geometría y cuadros del menú. Con `SALSA64_MENU_PPM` se obtiene una vista
  previa rasterizada como en el RSX (en `build/host/`).
- `tools/rpcs3-run.py --menu ROM…` (en `tools/test-phase2.sh`): el menú se
  maneja solo. Abre gfxtest, audiotest y gfxtest F3D seguidos y cada uno pasa
  su verificación tras el cierre del anterior, con «cierre: estado limpio».
  Deja una captura del menú en `dev_hdd0/salsa64/menu.ppm`.
- `test-host`, `test-roms` (nemu64-test 433 fallas con HLE y 136 con LLE,
  vitest, controles, savetest, gfxtest ×3, audiotest), build sin
  advertencias, `test-phase1`, `test-phase2` y `test-rec`: verdes.

- **PS3 real** (paquete «Salsa64 menú», SLSA00086, con main 5c2b00e
  fusionado): el autor confirma que funciona correctamente.
  - El menú listó los 8 respaldos de `/dev_hdd0/salsa64`.
  - Se abrió Mario Kart 64; START + SELECT volvió al menú, con todos los
    pasos del cierre en 0 y «cierre: estado limpio».
  - Perfect Dark escribió su EEPROM de 16 Kbit durante el juego («escrito
    diferido»), volvió al menú y, al reabrirlo, se leyeron sus 2048 bytes:
    el guardado se conserva.
  - «Salir al XMB» terminó en «salida normal» y volvió al XMB sin pantalla
    negra.
  - Cada juego arrancó con el recompilador vacío y el audio al 99–101 %
    (log en `build/ps3-logs/salsa64-menu.log`, fuera de git).

Falta (opcional):
- [ ] Medir el menú en la PS3 (cuadros por segundo con ondas; hoy no lo
      escribe al log).
- [ ] Probar el menú en una tele 4:3 o a 1080p (la vista previa del host
      cubre 4:3).

## 2026-10-10 — Mario Kart 64: polígonos estirados en los zooms (recorte del RSP en la HLE)

Reporte (el autor, PS3, «Salsa64 alfa» SLSA00085): en Mario Kart 64, al
pasar del título a lo que sigue, «polígonos mal calculados, donde se queda
una punta y se estira»; después precisó: «una pantalla que se hace pequeña,
como un gameplay que sale».

Diagnóstico (rama `reverent-meninsky-b3f79c`, en la Mac):
- No era del RSX ni de la vida de los juegos de búferes de src/rdp: con
  `salsa64-run` se simuló un SPE lento (`SALSA64_GFX_LATE=N` retrasa la HLE
  N sondeos; con `SALSA64_GFX_TEAR` la corre también al arrancar y compara)
  y en 245 tareas ninguna lectura de la RDRAM cambió durante la tarea; la
  invalidación de la caché de vértices de PSL1GHT (`rsxInvalidateVertexCache`
  escribe 0x1710 y 0x1714, como libgcm) está bien.
- El cuadro malo no es título → «GAME SELECT» (solo rectángulos, sale
  limpio) sino los **zooms de la demo**: entra creciendo desde una ventana
  (campos ~1106–1141 tras START en el 400) y al terminar se encoge
  (3300–3350 sin tocar nada). El juego los dibuja con un viewport chico que
  crece (`G_MV_VIEWPORT`), tijeras del RDP hasta (0, 0) y
  `gSPClipRatio(FRUSTRATIO_1)` cada cuadro. En el RSP real los triángulos se
  recortan contra el viewport por la clip ratio y contra el plano cercano; la
  HLE no hacía ninguna de las dos cosas: el suelo cruzaba el plano cercano
  (w → 0, x/w de −8000) y salía de la ventana como una punta hacia la
  esquina. Reproducido en el rasterizador de la Mac (campos 3310–3350 sin
  pulsar nada); la otra rama miró la transición a «GAME SELECT», que es de
  rectángulos, y por eso se vio limpia.

Arreglo (`src/rsp/gfx`; nada en src/rdp ni en lib/rsxkit):
- `G_MW_CLIP` (F3D, F3DEX, F3DEX2, el de Conker) guarda la clip ratio y
  cada tarea arranca con la de los datos del microcódigo (vuelven a la
  DMEM): **1** en la primera generación (F3D 2.0D/2.0G, F3DEX 0.95/1.21/1.22,
  el de Perfect Dark) y **2** en la segunda (F3DZEX), leído de los datos de
  los ucodes de los respaldos (offset 0x70 y 0x180: (1,0,0,r) (0,1,0,r)
  (1,0,0,−r) (0,1,0,−r)). La caja viewport × ratio se suma a las tijeras de
  los dibujos de triángulos (los rectángulos no pasan por el RSP); un
  cambio de viewport abre dibujo nuevo. GoldenEye y Perfect Dark tienen
  viewport = tijeras y no cambian.
- Recorte contra el plano cercano (z = −w) en `gfx_triangle`: un vértice
  detrás → dos triángulos, dos → uno, tres → nada; interpolación lineal en
  coordenadas de recorte, como el RSP. Los microcódigos «.NoN» (Zelda OoT
  F3DZEX.NoN, Star Fox 64 F3DEX.NoN, Conker F3DEXBG.NoN) no recortan: el
  núcleo lo lee del texto del ucode (`rsp->gfx_nonear`) y va en
  `GFX_JOB_NONEAR`. El de Perfect Dark (sin texto) se toma como NoN, **sin
  verificar contra hardware**: el autor ya lo jugó sin recorte y GLide64
  dibuja los muros cercanos de su intro que el recorte quitaría (cuadro
  3000, un muro a la izquierda).
- Efecto en otros juegos (rasterizador de la Mac, main contra la rama, un
  cuadro cada 100 campos): Zelda, Star Fox, Conker, Perfect Dark y los menús
  de Mario Kart idénticos. **SM64** en el castillo: el suelo cerca de la
  cámara (cruza el plano cercano) se descartaba entero y se veía el agua del
  foso debajo; ahora se dibuja, como en mupen64plus. **Banjo-Kazooie**: en
  la intro unos polígonos delante del plano cercano tapaban todo de negro;
  ahora se ve el logo con la abeja. **GoldenEye**: diferencias chicas dentro
  de modelos que cruzan el plano cercano (créditos), a la vista iguales.
- Pruebas: unitarias `f3dex_clip_ratio`, `f3d_near_clip`,
  `gfx_clip_ratio_f3dex2` y `rsp_gfx_nonear`; gfxtest con las escenas O
  (viewport chico + clip ratio 1 con tijeras de toda la pantalla) y R
  (cuadro que cruza el plano cercano): 30 de 30 puntos en el host (tres
  variantes) y en RPCS3 (F3DEX2 y F3D, pantalla del RSX); con main fallan
  4 puntos. Con main fusionado (generador de fragment programs, escenas P y
  Q): 329 unitarias y 32 de 32 puntos. `tools/build.sh test-host`,
  `tools/test-roms.sh`, `tools/build.sh` sin advertencias y
  `tools/test-phase2.sh`.
- La traza por dibujo de `salsa64-run` imprime además x/w, y/w, tijeras,
  viewport y la dirección de la textura.
- Respaldo propio de Mario Kart (`mk64.z64`) fuera del repo y en el
  dev_hdd0 de RPCS3.

Pendiente:
- [x] **PS3 real** con `build/salsa64/salsa64-recorte.pkg` (SLSA00090,
      «Salsa64 recorte»; el SLSA00086 que había elegido el autor ya lo usa
      «Salsa64 menú»): el autor confirmó que en Mario Kart «ya no se ve» lo
      que reportaba (log `build/ps3-logs/mk64-recorte.log`, fuera de git:
      sin errores, 60 campos/s).
- [x] PS3 real, SM64 en el castillo: «todo se ve bien» (el autor; log
      `build/ps3-logs/sm64-recorte.log`).
- [ ] **GoldenEye en la PS3 (noche del 2026-10-10): líneas azules finas en
      uniones de polígonos** que corren hacia la cámara (centro de la
      carretera del túnel de Dam, base de la barrera; fotos del autor). Sin
      atribuir. Lo que se descartó en el host: el punto de corte de una
      arista compartida se calcula en distinto orden en cada triángulo
      vecino, pero coincide hasta 0,001 px (`SALSA64_GFX_CUTCHECK`, 3572
      vértices cortados en 619 campos de Dam) y el rasterizador de la Mac no
      muestra ninguna grieta en las uniones con esta rama; las líneas no
      corren a lo largo del corte del plano cercano (sería horizontal, abajo).
      No se llegó al túnel sin manos (dos guiones chocan con paredes). El
      cian de GoldenEye ya conocido era otra cosa (soldados y armas, por el
      combiner). **Decide un A/B en la PS3**: «Salsa64 menú» o «Salsa64 alfa»
      (main sin este recorte) en el mismo túnel. Si también salen, pendiente
      aparte (no de esta rama); si no, el sospechoso es cómo el RSX
      rasteriza los triángulos cortados (volcado `gfxdump` del campo malo
      para dibujarlo en la Mac con `--raster-dump`).
      **A/B hecho el 2026-10-11 (el autor, PS3): las líneas salen igual con
      «Salsa64 menú» (main sin este recorte)** → no son de esta rama;
      pendiente aparte, fuera de ella (logs `build/ps3-logs/goldeneye-recorte-2.log`
      y `goldeneye-menu.log`, fuera de git). Fotos y logs guardados fuera
      del repo.
- [x] PS3 real, Perfect Dark con «Salsa64 recorte» (tomado como NoN): intro
      de la ciudad y un nivel, «se ve todo bien» (el autor, 2026-10-11; log
      `build/ps3-logs/perfectdark-recorte.log`).

Lo que se vio el 2026-10-11 en la PS3 (paquete «Salsa64 recorte», SLSA00090;
`embedded` en /dev_hdd0/salsa64/, se cambia por FTP; resultados arriba):
1. `file:goldeneye.z64` con «Salsa64 recorte» y luego la misma escena con
   «Salsa64 menú» o «Salsa64 alfa»: túnel de Dam con las barreras a rayas,
   mirando el centro de la carretera y la base de la barrera. Lo esperado si
   es de esta rama: líneas azules solo en «recorte».
2. `file:perfectdark.z64`: intro (vuelo por la ciudad) y un nivel. Se toma
   como NoN: lo esperado es verlo como antes; si hay puntas estiradas en
   muros cercanos, su microcódigo sí recorta (una línea en
   `src/rsp/gfx/sync.c`); si hay huecos negros, no.
3. Ya confirmados hoy: Mario Kart (demo al entrar y al encogerse, sin
   puntas) y SM64 en el castillo (suelo junto al foso), «todo se ve bien».
- [ ] En RPCS3 el modo de prueba automática sale a los 1800 campos y el
      zoom sin tocar nada cae en el 3300 (`capturas` "25 3500" no llega):
      dejar que el M de `capturas N M` mande sobre AUTO_FIELDS si hace falta
      verlo ahí.
- [ ] Banjo-Kazooie en el host se queda en el logo de Rare (tarea del campo
      68, luego solo audio): no es de esta rama.

## 2026-10-10 (noche) — Resumen del día y problemas abiertos

**Integrado y subido hoy** (main 8231305): controles y bucles de espera, guardado
del cartucho, Fase 2 gráfica (F3DEX2 en el SPE 0), recompilador (Fase 3) con TLB,
FPU/LWL/LWR/MFC0 nativos y bucles en código mapeado, dos hilos del PPE con
limitador, caché de texturas, fin de tarea del RSP por sondeo, audio en el SPE 1
(ABI1, nead y naudio con MP3; FILTER y MIXER contra el microcódigo real), F3D,
F3DEX 1.x/0.95, Fast3D 2.0G, menús de Mario Kart, presentación de GoldenEye,
arranque del CIC 6105/osCicId, censo (docs/compatibilidad.md), arreglo del
cuelgue de Zelda, microcódigos de Rare (Perfect Dark, Conker), prototipo de LLE
del RSP, generador de fragment programs (Fase 4), G_RM_TEX_EDGE (medidor de
SM64), candado de RPCS3, qemu con power9, logo y fondo del XMB.

**En la PS3 real:** Zelda, SM64, Mario Kart, GoldenEye y Perfect Dark se juegan
con audio; Zelda/SM64/MK64 a velocidad completa; GoldenEye en la presa 34–55
campos/s (emulación 18–29 ms por campo: el CPU es cuello); Perfect Dark al 100 %
en intro/menús (falta medir un nivel). Primer video público: GoldenEye.

**Integrado después del cierre:** presentación (`bold-neumann-794fc0`,
merge b166a06): Banjo sin parpadeo, Conker sin quemado, pausa de Zelda visible,
overscan. La «regresión» de GoldenEye (soldados cian, armas azul claro) era la
aproximación del combiner con ENV de la HLE vieja, idéntica en SLSA00064; con el
generador el autor vio GoldenEye normal en la PS3. Después, el menú estilo XMB
(`intelligent-yalow-4dd14d`, merge 00eba22), confirmado en la PS3.

**Integrado el 2026-10-11:** recorte del RSP (`reverent-meninsky-b3f79c`,
«Salsa64 recorte» SLSA00090): en la PS3, Mario Kart sin polígonos estirados,
SM64 y Perfect Dark bien. Las líneas azules de GoldenEye salen también sin el
recorte (A/B en la PS3): pendiente aparte, abajo. Después, el mismo día:
control (pulsaciones perdidas), fondos prerrenderizados de Zelda (JPEG + S2DEX),
recompilador con perfil por causa, efectos de framebuffer (4baa6df) y el arreglo
del espejo del framebuffer que colgaba SM64 en la cara de Mario (84ea55f; en la
PS3 sin confirmar).

**Integrado después, el 2026-10-11:** resolución 720p/1080p y escala
interna 1×–5× por destino con «Aplicar ahora» (rama
`dev-a1388b259074798ab`, 1647a25), estados guardados con contador de FPS
(`wonderful-blackwell-6f40d3`, SLSA00094) y FPU en registros, COP0 nativo y
caché por tramos (`jovial-jones-2408ac`, SLSA00093). Detalle en las entradas
de arriba y en docs/framebuffer.md.

**Problemas abiertos:**
- Líneas como de entrelazado en el logo de N64 de Zelda en la PS3; en el
  rasterizador de la Mac el cuadro sale limpio. Sospecha: el recorte del
  overscan escala 684 líneas a 720 con filtro lineal; probar con
  «Recorte del borde: Apagado» en el menú. Arreglo propuesto: tapar los bordes
  con negro sin escalar.
- **Líneas azules en uniones de GoldenEye** (PS3 real, túnel de Dam): línea fina
  del color del relleno de fondo a lo largo de uniones de polígonos que corren
  hacia la cámara (centro de la carretera, base de la barrera). Salen igual con
  «Salsa64 recorte» y con «Salsa64 menú» (A/B del 2026-10-11): no son del
  recorte, están en lo común (rsxkit, src/rdp o la HLE). En el host no se
  reproducen (sin grietas en el rasterizador de la Mac; los cortes de aristas
  vecinas coinciden a 0,001 px). Siguiente paso: gfxdump del campo malo en la
  PS3 y dibujarlo en la Mac con `--raster-dump`; si allí sale limpio, es la
  rasterización del RSX (reglas de cobertura o vértices que no caen en el mismo
  punto tras el redondeo). Evidencia: fotos y logs, fuera del repo por ser
  imágenes de un juego comercial.
- **Kokiri en colores planos, reabierto (2026-10-11).** *Resuelto en la rama
  `fervent-hodgkin-d38e1e` (entrada de arriba): eran las salas
  prerrenderizadas (fondo JPEG + S2DEX sin HLE); lo de abajo era la casa vista
  desde arriba, no el piso del bosque.* El cierre del 2026-10-10
  («es el sol cenital») no explica lo que se ve al salir de la casa de Link: en
  el host (campo 14400 con pad) el piso es verde puro, (0,71,0), (0,104,0),
  (0,161,0)…, con rojo y azul en 0, y un rectángulo azul liso. Ninguna textura
  × iluminación da eso; aquella comparación con mupen64plus fue en el balcón
  (campo ~3700) y midió colores con rojo y azul. El autor lo ve como error en la
  PS3. También en la tienda del bosque Kokiri (foto en la PS3, 2026-10-11):
  piso amarillo y paredes y mostrador naranja lisos, con los personajes y
  objetos texturizados; igual con `fbfx.cfg` = 0, así que es de la HLE. Afecta
  a las mallas de las salas, no a los actores. Evidencia fuera del repo.
  Siguiente: diagnosticar Kokiri y su tienda en colores planos (SLSA00091).
- **Pulsaciones perdidas** (PS3, 2026-10-11): resuelto en la rama
  vibrant-hawking (ver la entrada del 2026-10-11 arriba): el bucle de
  presentación dejaba de leer los mandos hasta 7,4 s; confirmado por
  el autor en Zelda y SM64 (SLSA00092).

**Siguientes pasos:**
- Recompilador: más velocidad del CPU en GoldenEye y Perfect Dark.
- Efectos de framebuffer (pendiente de abajo), sobre la presentación ya en main.
- HLE: **cara de Conker** (polígonos negros en la boca, visto en la PS3; no lo
  explicaba la presentación). Lo de Banjo (z/w > 1, modelos que tiemblan,
  cielo) era la protección anticopia del 6103 (entrada del 2026-10-12).
- Gráficos pendientes (actualizado 2026-10-11): líneas del logo de Zelda (la
  HLE muestrea mal la segunda textura de ruido del texto «NINTENDO 64»),
  defecto en la túnica de Link, líneas azules en uniones de GoldenEye.
- **Mario Kart, Luigi Raceway: negro arriba del muro de ladrillos** (PS3,
  2026-10-11, en todos los paquetes): debería verse el cielo en degradado y el
  fondo de la pista. Previo a las ramas de hoy; candidato: cómo se dibuja el
  cielo (rectángulo o triángulos con degradado al fondo). Reproducir en el host
  con docs/pads/mk64-luigi.pad y comparar contra mupen64plus.
- Zelda: fondos prerrenderizados hechos (2026-10-11); JPEG a punto fijo
  (68 ms por sala en el PPE, meta < 10 ms); luego Majora's Mask.
- Recompilador: FPU en registros, más registros fijos y refill de la TLB
  (en curso desde el 2026-10-11).
- **Recompilador, siguiente frente (medido en la PS3 el 2026-10-11):** el
  código nativo cuesta ~47 ciclos del PPE por instrucción MIPS con cualquier
  combinación de FPU, tramos y COP0; no lo limitan las instrucciones emitidas
  sino el tamaño y la localidad del código (~7 MB de bloques vivos en GoldenEye
  contra 512 KB de L2, caché en páginas de 64 KB). Trabajo: código más compacto
  y bloques calientes juntos. La FPU residente quedó apagada (más lenta).
- **Perfect Dark en un nivel a 49–50 campos/s por el lado gráfico:** 14 ms por
  campo esperando al SPE 0, ~4500 triángulos y ~131 texturas decodificadas por
  tarea (el CPU usa 4,5 ms). Candidato fuerte para pasar la conversión de
  texturas a los SPEs 2–3.
- **Quitarle trabajo al PPE** (decidido el 2026-10-11, después de cerrar los
  trabajos abiertos): el VR4300 se queda en el PPE (memoria, FPU IEEE y saltos
  no le van a un SPE), pero lo demás pasa a los SPEs libres (2–5) para que el
  hilo 0 tenga el núcleo: tarea JPEG de Zelda (68 ms en el PPE), conversión de
  texturas, lo que quede de la presentación del hilo 1. Ordenar con
  docs/uso-cell.md (uso medido por componente y por juego).
- Fase 5 (posprocesado, MSAA) después de presentación y menú; recompilador
  RSP→SPU para el LLE; conversión genérica de texturas en lib/rsxkit/spu.

**Notas:** el input lag de Zelda es antiguo y solo en Zelda (SM64 bien);
sospecha: los dos hilos más el retraso propio de Zelda; presentación agregó al
log la línea «retraso: cola … destino …» para medirlo.
`embedded` quedó en el último valor que dejó presentación. Con el menú en main:
si existe `/dev_hdd0/salsa64/embedded`, cualquier app (también la principal)
arranca directo esa ROM sin menú; el menú sale solo cuando el archivo no existe.

## 2026-10-10 — Fase 4: generador de fragment programs del RSX en tiempo de ejecución (rama compassionate-shirley)

Hecho:
- **`lib/rsxkit` emite microcódigo de fragment program** (familia NV40) sin
  compilador Cg en la consola: `include/rsxkit/fpgen.h` + `src/fpgen.c`
  (codificador, C puro, sin PSL1GHT) y `src/fpcache.c` (caché por hash de
  la descripción canónica, 128 programas en un pool de 96 KB de memoria
  local del RSX; si se llena, espera al RSX y se vacía). La descripción es
  genérica: `color = saturate(Σ_j (Π factores_j) · X_j)` con factores T0,
  T0a, T1, T1a, C (color del vértice) y Ca, hasta 7 términos (un varying
  TEXCOORD1..7 cada uno) de 4 factores; los `X_j = k0 + k1·C + k2·Ca` los
  sigue armando el vertex program (`rsxkit_lincomb.vcg`, ahora con 7 X, 27
  constantes, COLOR0 de salida y `fogsel` para sumar la niebla al término
  sin factores). La combinación lineal fija (`nterms = 0`) sigue como
  camino rápido con sus tres programas precompilados. API en
  `lib/rsxkit/README.md`; formato en `docs/rsx-notes.md` y en la cabecera
  de `fpgen.c` (fuentes MIT de cgcomp, `docs/THIRD_PARTY.md`).
- **Verificación en el host sin consola:** `tests/unit/test_fpgen.c` trae
  un intérprete del microcódigo (TEX, MOV, MUL, ADD, MAD, swizzle, H/R,
  saturación) anclado a los `.fpo` que cgcomp compila de los shaders de la
  combinación lineal y de tres shaders de referencia con productos
  (`tests/unit/fpgen/*.fcg`, `tools/fpgen-ref.sh` → `tests/unit/fpgen_ref.h`):
  primero los programas de cgcomp dan la fórmula de su shader, luego los
  generados dan la misma, y para las descripciones equivalentes leen los
  mismos TEXCOORD. Más pruebas de forma (fin de programa, un solo registro
  de entrada por instrucción, perspectiva, máscara, R0 saturado al final),
  canonización, hash y rechazo de descripciones inválidas.
- **`src/rsp/gfx/combine.c`** evalúa el combiner con monomios dispersos
  (cada textura a lo sumo una vez, dos texturas, hasta tres factores de
  sombreado) y, si el resultado no cabe en la combinación lineal (o si
  pide T0·T0a y T0 a la vez), lo agrupa en el menor número de términos
  (cada X_j absorbe un factor de sombreado) en orden canónico, con el alfa
  compartiendo términos con el color. `gfx_draw` lleva `nterms` y
  `factors` (480 bytes; `GFX_K_TERMS` = 7); `src/rdp` los pasa a rsxkit y
  el rasterizador del host los evalúa igual. El búfer de comandos de cada
  juego pasa de 512 a 768 KB a costa del arena de texturas.
- **gfxtest** tiene dos cuadros nuevos (N: TEXEL0·TEXEL1·SHADE; O:
  lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE, la niebla por luz de GoldenEye) con
  sus puntos en `expect.h`; las tres variantes dan 24/24 en el host y
  `tools/test-roms.sh` exige 0 combiners aproximados. ROMs recompiladas
  (SHA-256 nuevos).
- **GoldenEye en Dam (host):** 97 449 → **0 combiners aproximados** en la
  misma corrida (`--pad` con START y A pulsados y soltados cada 240
  campos, 7 500 M ciclos). En el nivel solo dos combiners
  (`159804:5ffedbf8` y `159a04:5ffefff8`) van por el generador: 3 términos
  {}, {T0, C}, {C}.
- Estadísticas nuevas en el log del frontend («gfx: fragment programs:
  cambios por tarea, dibujos con programa generado, programas en la
  caché»).

Decisiones:
- Una sola tabla combiner → programa, en rsxkit, por hash de la
  descripción: la estructura de los términos depende también de los
  valores de PRIM/ENV (un coeficiente en cero quita un término), así que
  una tabla por palabras de G_SETCOMBINE en src/rdp sería incorrecta.
- Operaciones en fp16 como las de cgcomp (entradas de 8 bits); R0 al
  final con saturación. Registros: H2 acumula, H3/H4 texturas, H5
  productos (`num_regs` ≤ 3).
- La niebla sigue en el vertex program: escala por (1 − C.a) y suma
  fog·C.a al primer término sin factores (`fogsel`); combine.c garantiza
  ese término cuando hay niebla.

Verificado:
- [x] RPCS3: `tools/test-phase2.sh`: gfxtest en el frontend (HLE en el SPE,
      RSX con programas generados) 24/24 puntos en F3DEX2 y F3D; 2
      programas generados, 9 cambios de programa por tarea.
- [x] `tools/test-rec.sh` (qemu ppc64, big-endian): 324/324 unitarias en
      los cuatro modos del recompilador (el intérprete del microcódigo y
      el generador también en big-endian), nemu64-test con las 433 fallas
      de la línea base, vitest OK.
- [x] Con main 5a0a1a3 fusionado (ad14e4b; gfxtest con el cuadro de
      G_RM_TEX_EDGE de main como N y los del generador como P y Q, 26
      puntos): test-host 325/325, test-roms 26/26 en las tres variantes,
      RPCS3 26/26 en F3DEX2 y F3D, compilación PS3 sin advertencias,
      test-rec en los cuatro modos del recompilador.

- [x] **PS3 real** (SLSA00087 «Salsa64 fpgen», 136dff4 y luego ad14e4b con
      main fusionado; el autor, 2026-10-10): GoldenEye 238 s hasta Dam con
      la niebla por luz exacta en las capturas (34–55 campos/s en el
      nivel, 4 programas generados, hasta 131 dibujos por tarea con
      programa generado, 0 vaciados); Zelda OoT (144 s, Kokiri), SM64
      (166 s, castillo; el medidor de energía ya sin fondo con main
      fusionado) y Mario Kart 64 (81 s, pista) sin regresión visual, 0
      programas generados y 54–60 campos/s. Números en
      `docs/perf-log.md`.

Pendiente:
- [ ] Perfect Dark en la PS3 con el generador (su combiner de niebla por
      luz es el de GoldenEye; en el host ya no se aproxima).
- [ ] Medir el tiempo del RSX por cuadro cuando haya contadores (hoy solo
      se mide el PPE).
## 2026-10-10 — Medidor de energía de SM64 con fondo opaco: G_RM_TEX_EDGE

Reporte (el autor, PS3, main de hoy): en Super Mario 64, al nadar, el
medidor de energía («POWER», rueda azul y mano) salía con un cuadro café
opaco detrás, del tamaño de su textura. Vidas, estrellas, agua y peces bien.

Diagnóstico (rama `modest-archimedes-e639a4`, en la Mac):
- Reproducido con `salsa64-run` nativo: `docs/pads/sm64-castillo.pad` lleva
  a Mario al puente del castillo y `--poke "4200:8033B21E=0x0400"` (opción
  nueva: escribe la RDRAM al empezar un campo) le baja la energía para que
  aparezca el medidor. El rasterizador de referencia lo dibujaba igual de
  opaco: era de la HLE, no de la traducción al RSX.
- Con la traza por dibujo (`-DSALSA64_GFX_TRACE`): el medidor son dos
  cuadros de 32×64 RGBA16 y la rueda de 32×32, en 1 ciclo, combiner
  TEXEL0/TEXEL0 y othermode L 0x0F0A7008 = `G_RM_TEX_EDGE`: CVG_X_ALPHA |
  ALPHA_CVG_SEL | **FORCE_BL** | AA_EN, blender in·0 + in·1. `blend_state`
  solo ponía la prueba de alfa de CVG_X_ALPHA sin FORCE_BL (las
  `G_RM_AA_*_TEX_EDGE`), así que salía sin prueba de alfa ni mezcla.
- En el RDP la cobertura pasa a ser cvg·α (cvg = α/32 con el píxel
  cubierto entero) y con AA_EN no se escriben los píxeles de cobertura 0.
- No venía de 8ec50de (TEXRECT sin costuras): la condición estaba así desde
  la primera HLE del RDP (87592d1).

Arreglo (`src/rsp/gfx/rdp.c`, `blend_state`; nada en src/rdp): con
CVG_X_ALPHA, FORCE_BL y AA_EN, prueba de alfa ≥ 32 (el RSX la recibe como
`alpha_ref` 32). Sin AA_EN la cobertura no depende del alfa y no se prueba;
sin FORCE_BL sigue el corte en 128 de antes.
- Prueba unitaria `gfx_cvg_x_alpha` (G_RM_AA_ZB_TEX_EDGE → 128,
  G_RM_TEX_EDGE → 32, sin AA_EN y G_RM_OPA_SURF → sin prueba) y escena N de
  gfxtest: cuadro con textura RGBA16 de alfa de 1 bit en G_RM_TEX_EDGE; sin
  el arreglo el texel con alfa 0 sale café (0xA55221). Ahora 24 de 24
  puntos en las tres variantes, en el host y en RPCS3 (pantalla del RSX).
- Sin cambios en otras escenas (comparación píxel a píxel del rasterizador
  de la Mac, antes y después): SM64 (título, selección de archivo, intro,
  castillo; solo cambia el recuadro del medidor), los menús de Mario
  Kart 64 hasta la selección de pista y las intros de GoldenEye, Banjo,
  Star Fox 64, Perfect Dark, Conker y Zelda OoT.
- Pruebas: `tools/build.sh test-host` (320, 0 fallidas), `tools/test-roms.sh`
  (nemu64-test 433 con HLE y 136 con LLE, gfxtest ×3, audiotest, vitest,
  controles, savetest), `tools/build.sh` sin advertencias del compilador y
  `tools/test-phase2.sh` en RPCS3 (24 de 24 en F3DEX2 y F3D).
- Paquete para la PS3: `salsa64-alfa.pkg` (SLSA00085, «Salsa64 alfa»).
- **PS3 real**: el autor confirmó el medidor de SM64 sin fondo. Log de la
  corrida sin errores (`build/ps3-logs/sm64-medidor.log`, fuera de git).

- **Mario Kart 64 en la PS3** con este paquete: menús y carrera bien según
  el autor (la pantalla gigante negra de la pista es el efecto de
  framebuffer ya pendiente, Fase 4). Vio además polígonos con «una punta que
  se queda y se estira» en la transición del título a «GAME SELECT». No es
  de esta rama: ninguna draw de Mario Kart pasa por la rama nueva de
  `blend_state` (55 026 estados rastreados, campos 300–1900). En la Mac la
  transición sale limpia: el fundido son 240 triángulos con w de 1940 a
  2900 (ninguno con w ≤ 0) y «GAME SELECT» son solo rectángulos. Hipótesis:
  el RSX lee vértices viejos (búfer que la tarea siguiente ya reescribió) en
  src/rdp.

Pendiente:
- [ ] Polígonos estirados en la transición título → «GAME SELECT» de
      Mario Kart 64 en la PS3 (tarea aparte: separar HLE de RSX con el
      volcado `gfxdump.bin` y `--raster-dump`; revisar la vida de los
      juegos de búferes de src/rdp/rdp.c).
- [ ] `embedded` de vuelta a `file:oot.z64` (como lo dejó el trabajo de
      presentación) cuando el autor termine con Mario Kart.

## 2026-10-10 — Presentación: Banjo-Kazooie (FREEZE del RDP y destino equivocado), pausa de Zelda y recorte del overscan

Rama `bold-neumann-794fc0` (trabajo de presentación). Los tres casos
pendientes de `src/rdp` y uno más que apareció en el camino.

**Banjo-Kazooie no pasaba de 2 tareas gráficas** (en la Mac, en qemu
ppc64 con y sin recompilador, y en RPCS3: pantalla fija con el audio
sonando; en la PS3 real la intro corría porque el paquete naudio era
anterior). Bisección por commits del host: lo rompió la integración del
LLE (ce8351d), que dio semántica a DPC_STATUS (antes una lectura devolvía
la última palabra escrita). Rastreando los accesos al DP: el juego usa el
bit FREEZE como semáforo: en el manejador de la interrupción del DP escribe
SET_FREEZE, arranca la tarea del cuadro siguiente con el RDP congelado y
la descongela después; la interrupción de esa tarea la espera *después* de
descongelar (en la consola el RDP congelado no ejecuta). Nuestro HLE la
daba 10 000 ciclos tras el fin de la tarea, aún congelado, y el juego la
perdía. Arreglo en el núcleo (`src/core/rsp.c`, `sp.c`): si DPC_STATUS
tiene FREEZE cuando toca la interrupción del SYNC_FULL, se retiene
(`dp_held`) y se programa cuando el CPU escribe CLR_FREEZE
(`salsa64_rsp_dp_unfreeze`). Mismo principio que mupen64plus-core (que
difiere la tarea entera mientras FREEZE). Banjo corre: 644 tareas en 27 s
en la Mac. Prueba `rsp_gfx_task` intacta (la interrupción sigue llegando
`dp_cycles` después del fin de la tarea cuando no hay freeze).

**Parpadeo de Banjo en la PS3 (a):** el juego alterna dos framebuffers de
292 × 216 a 584 × 216 bytes uno del otro, y `src/rdp` supone 3/4 del ancho
de alto (219 líneas): el primero «cubre» el origen del VI del segundo con
sus últimas 2 líneas. Entre los dos, se tomaba el dibujado hace menos, que
mientras se muestra uno es el otro (el RDP ya dibuja el cuadro siguiente):
se presentaban 2 líneas negras estiradas a toda la pantalla. Simulado con
la traza de la Mac (VI y destinos por tarea, 70 s): 566 de 4142 campos con
el destino equivocado; con la regla nueva (entre destinos del ancho del VI,
el que empieza más cerca del origen; luego el más reciente), 12, todos de
arranque o cambio de escena (el otro framebuffer aún sin dibujar).

**Conker en la PS3 (3D quemado y parpadeando, capturas del RSX negras;
pedido tras integrar la rama de Rare):** mismo error
que Banjo. La repetición del volcado de la PS3 (campo 3600) en RPCS3 con
`tools/rpcs3-run.py --replay` dibuja la escena bien: la traducción al RSX
es correcta. En la Mac (70 s con `--trace-vi` y `--trace-gfx`) Conker
alterna dos framebuffers de 292 × 216 (0x3b7600 y 0x3d6300, a 216 líneas
uno del otro) y a mitad de cada tarea dibuja en la color image CI8 de 64 de
ancho (0xde080) y vuelve: con la regla vieja 591 de 4144 campos presentaban
las 2 últimas líneas del otro framebuffer (negras, o una franja de color
estirada a toda la pantalla: lo «quemado»); con la nueva, 0. Además, el
alto supuesto de un destino (3/4 del ancho: 48 para la de 64) ahora crece
hasta donde llegan las tijeras de los dibujos que recibe (`rdp.c` mira los
dibujos que siguen a cada TARGET; la CI8 de Conker es de 64 × 64 y su
FILL va de 0 a 65): la superficie se crea del alto real y, si una tarea
posterior llega más abajo, se vuelve a crear más grande. El alto de la
presentación (`h`) también lo usa. (rsxkit ya recortaba tijeras y
borrados a la superficie: no se escribía fuera.)

**Tabla de destinos portable:** `src/rdp/targets.c` (sin rsxkit): qué
color image tiene superficie, cuál se recicla y cuál se presenta; `rdp.c`
solo guarda las superficies por ranura. Pruebas en
`tests/unit/test_targets.c` (7): dos framebuffers alternos en direcciones
distintas que se solapan en la cuenta (Banjo, Conker), la misma dirección
con dos anchos (GoldenEye 320/440), color images de 64 sobre el
framebuffer (Zelda en pausa), VI sostenido sin dibujar (GoldenEye
cargando), dibujado con el VI apagado (SM64), el orden de reciclado y el
alto por las tijeras (Conker).

**Pausa de Zelda negra en la PS3:** con el menú, cada tarea pone 3 color
images y alternando framebuffers son 6 distintas (0x3b8800/64,
0x3b5000/64, 0x3b5000/320, 0x3de000/64, 0x3da800/64, 0x3da800/320) para 4
ranuras: cada tarea tiraba 2 y entre ellas el framebuffer de 320. Ahora
8 ranuras (`RDP_TARGETS_MAX`; memoria del RSX en docs/memory-budget.md) y,
al reciclar, nunca la presentada en el último campo, primero las viejas
(30 campos), luego las de un ancho distinto al del VI (auxiliares) y al
final la más antigua. Reproducido en la Mac con `docs/pads/zelda-oot.pad`
(la partida del autor bajada de la PS3 por FTP a `build/saves/`, fuera de
git): los TARGET de la traza son esos; el rasterizador de la Mac no cambia.

**Recorte del overscan:** `SALSA64_DATA_DIR/overscan.cfg`, en píxeles de la
N64 a 320 × 240 por lado (un número: todos; dos: horizontal y vertical;
cuatro: izquierda, derecha, arriba, abajo; `0` lo apaga). **Encendido por
omisión con 8 y 6 (2,5 % por lado)**: es lo que una tele de tubo esconde y
donde los juegos dejan basura (las columnas azules de GoldenEye; los HUD
quedan dentro). Se escala con la imagen del VI (a 640 de ancho recorta
16). Vale para el destino del RSX y para la copia de la RDRAM; las ROMs de
prueba se presentan enteras (sus verificaciones leen la pantalla por
posición) y las capturas salen del tamaño recortado. En rsxkit,
`rsxkit_target_present` acepta ahora cualquier columna (la textura toma la
fila entera y la columna va en las coordenadas de textura; antes el offset
se redondeaba a 32 píxeles).

> **Corrección (2026-10-12):** el z/w > 1 y el domo fuera del plano lejano
> venían de la proyección de 750 y 1250 que pone la protección anticopia
> cuando falta lo que deja el IPL3 del 6103 (entrada del 2026-10-12). Además,
> rsxkit no descarta por z/w > 1: el RSX tiene zclamp y sin descarte de los
> planos cercano y lejano (`rsxSetZMinMaxControl`).

**Fondo negro de Banjo (c) — no es presentación:** en la Mac también sale
negro (cielo y paisaje) tras Banjo tocando. En la tarea del campo 2400 el
primer dibujo tras el borrado son 8 triángulos con textura, w de −88 a
1321 y z/w hasta 117 (el domo del cielo): el rasterizador de la Mac los
descarta por w ≤ 0 y en el RSX quedan fuera por z/w > 1 (el z normalizado
se pasa de 1 también en objetos normales: 1,05–1,36). Le toca a la HLE
(trabajo de F3D): la proyección o el recorte de esos triángulos. El síntoma
(b) (escena «desplazada», mitad izquierda vacía, pasto solo abajo) coincide
con la escena sin fondo: Banjo a la derecha sobre una franja de pasto y
todo lo demás negro. Volver a mirarlo en la PS3 con el fondo arreglado
antes de buscar otra cosa.

Herramientas (`salsa64-run`): `--pad "0:…"` conecta el control desde el
arranque (antes el primer estado entraba al terminar el campo 1; en la PS3
el puerto 1 siempre está); `--trace-exc` imprime los bits IP de Cause y
MI_INTR/MASK; variables `SALSA64_GFX_CYCLES`, `SALSA64_DP_CYCLES` y
`SALSA64_AUDIO_CYCLES` para probar otros tiempos del RSP; ganchos de
depuración `-DSALSA64_DEBUG_PI` (DMA del PI) y `-DSALSA64_DEBUG_DP`
(registros del DP, escrituras de SP_STATUS e interrupción del DP), como
`SALSA64_DEBUG_SPMEM`.

Verificación (con main 705f4ac fusionado): `tools/build.sh test-host` 326
(7 de targets); `tools/test-roms.sh` (nemu64-test 433 con HLE y 136 con
LLE, vitest, controles, savetest ×6, gfxtest ×3, audiotest); test-phase2 en
RPCS3 (gfxtest 22/22, spu-jobs, frontend F3DEX2 y F3D); `tools/test-rec.sh`
(unitarias big-endian con el recompilador ×4, nemu64-test 433, vitest);
`tools/build.sh` sin advertencias del compilador. Paquete subido a
`/dev_hdd0/packages/salsa64-presentacion.pkg`; los respaldos de Banjo y
Conker ya están en `/dev_hdd0/salsa64/`.

**En la PS3 real (el autor, paquete «Salsa64 presentación» SLSA00084 con
main 705f4ac):** Banjo ya sin parpadeo (destino de 1,5 campos de
antigüedad, cola 0); Conker «se ve muy bien, sin temblores» (57 campos/s con
escenas de 7600 triángulos); GoldenEye sin columnas azules; Zelda 100 % de
NTSC. Lo que queda no es de presentación: en Banjo los modelos tiemblan,
hay cosas dibujadas encima de otras y faltan polígonos (el rango de z, ver
arriba); en Conker polígonos negros en la boca; y en GoldenEye soldados
(y el plano frontal de Bond al entrar a Dam) teñidos de cian. Este último
se investigó a fondo porque parecía regresión de la rama: el volcado
(`gfxdump` en el campo 2400) de un cuadro con el soldado cian, dibujado en
la Mac, sale cian igual; sus dibujos (#22 y #25) llevan
`lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE` con ENV = (0,35, 0, 0), que la HLE
de 705f4ac aproxima como `T0·Ca + (0,353·(C.r − C.a), 0, 0)`: con C.a ≈ 1 y
C.r gris resta rojo. Repetido el mismo volcado en la PS3 con las dos apps
(SLSA00064 de main 705f4ac y la de esta rama), las imágenes son idénticas
píxel a píxel (0 de 75 840). El generador de fragment programs de main
(b32a6e6, fusionado aquí) hace ese combiner exacto. También se comprobó
que GoldenEye no escribe FREEZE en DPC_STATUS (solo 0x3c0, también en
Dam): el cambio del núcleo no lo toca. RPCS3 no sirve para GoldenEye: su
RSX muere («Verification failed») a los ~16 s con cualquier frontend.

**Retraso de lo que se ve (duda del autor sobre el input lag de Zelda):**
el log trae cada 5 s «retraso: cola X campos (máx N); destino Y campos de
antigüedad (máx M)»: la cola es cuántos campos lleva la emulación de
ventaja al presentar (lo nuestro: 0,00 en todos los juegos probados) y la
antigüedad cuántos campos hay entre el dibujo del destino y el campo que
lo muestra (lo del juego: 1,0 en GoldenEye, 1,5 en Banjo, 2,5 en Conker).
Zelda queda por medir con el menú de pausa y un minuto de juego.

**Con el paquete que lleva main b32a6e6 (generador de fragment
programs), el autor en la PS3:** GoldenEye «todo se ve normal»: soldados,
armas en primera persona y el plano de Bond sin tinte (80,9 dibujos por
tarea con programa generado). Zelda: el menú de pausa se ve (el mapa; los
errores que quedan en él son los efectos de framebuffer ya conocidos,
Fase 4), 100 % de NTSC, cola 0,00 y destino de 2,50 campos de antigüedad
(el juego va a 20 cps: cada cuadro se muestra 3 campos; GoldenEye 1,0 y
Banjo 1,5): la sensación de retraso de Zelda es del propio juego, no de
la cola de presentación. SM64 y Mario Kart, sin novedades. `embedded`
quedó en `file:oot.z64`.

Pendiente:
- [ ] Integración en main (la rama lleva main
      b32a6e6 fusionado y la verificación completa en verde).
- [x] HLE: el cielo y el paisaje de Banjo (arriba). No era de F3D: la
      protección anticopia del 6103 (entrada del 2026-10-12).
- [x] LLE: con `--lle=all` Banjo no arranca (la primera tarea corre 958 M
      instrucciones sin tocar COP0); no es del freeze. Era la misma
      protección: sin lo que deja el IPL3 el juego no trae el arranque del
      RSP (entrada del 2026-10-12).
- [ ] El VI de Banjo muestra 213 de 240 líneas (V_VIDEO 61–488): en una
      tele quedarían bandas; aquí se estira a 4:3. Colocar la imagen según
      la ventana del VI cuando haya un caso que lo necesite.

## 2026-10-10 — Kokiri «en colores planos»: es el sol cenital, no la HLE (referencia con mupen64plus)

> **Corrección (2026-10-11):** esto vale para el bosque al aire libre (el
> balcón, campo ~3700). Lo que el autor vio en la PS3 y el verde puro del
> host (casa de Link desde arriba, tienda Kokiri) son **salas
> prerrenderizadas**: faltaban la tarea JPEG y el fondo de S2DEX. Ver la
> entrada «Kokiri y su tienda en colores planos» del 2026-10-11.

Reporte (PS3 y rasterizador de la Mac): en el bosque
Kokiri de Zelda OoT el mundo sale en colores planos, «verde del shade», con
la HUD bien; la casa de Link sí se ve con texturas.

Diagnóstico (rama `frosty-golick-d1d71a`, en la Mac):
- Las texturas **sí** llegan y se decodifican bien: corteza CI8 con paleta,
  pasto RGBA16 (dos tiles, lerp por ENV_ALPHA), siluetas de troncos I4;
  los combiners son exactos (T0·SHADE·PRIM y lerp(T0,T1)·SHADE), `ntex` =
  1 o 2, las s/t por triángulo cubren ~20 texels. En el píxel (270,80) del
  tronco el rasterizador escribe T0 = 0,54 × X0 = 0,23 → 0,13.
- Lo que aplasta la textura es el **SHADE**: con `G_LIGHTING` los vértices
  verticales quedan en 80–120/255. Luces de la escena: sol blanco (255)
  con dirección (0, 0,99, 0,16), relleno (70,70,90) opuesto y ambiente
  80. El sol es cenital porque el juego fija `dayTime` = 0x8000 (mediodía
  exacto) al cargar la partida (leído de `gSaveContext` en el volcado de
  la RDRAM; OoT 1.2, 0x8011AC80). Las normales son normales de verdad
  (longitud ~120), no colores horneados.
- **Referencia**: mupen64plus 2.6 (brew) con GLide64 y rsp-hle, con la
  misma partida del autor, movido por `tools/m64p-ref.sh` (plugin de
  entrada propio que reproduce el `--pad`; `docs/pads/zelda-kokiri.pad`
  saca a Link al balcón). Mismos parches en 640×480: piso (56,56,3) vs
  Salsa64 (55,54,1); tronco (31,29,17) vs (29,27,15); pared (43,27,18) vs
  (37,23,14). Diferencias < 6/255 en todo: la escena **es así** a esa hora
  (el piso recibe el sol de lleno, pero sus texturas ya tienen media
  55/255). Imagen comparativa en `build/m64p-ref/kokiri-comparacion.png`
  (fuera de git).
- Única diferencia visible: zonas crema (arriba a la derecha y bajo el
  balcón) donde el rasterizador de la Mac descarta triángulos con algún
  w ≤ 0 (la pared lejana y el piso cruzan el plano de la cámara); GLide64
  los recorta. El RSX recorta en hardware, así que en la PS3 no deberían
  faltar; comprobarlo cuando la consola esté libre.
- **Confirmación con el microcódigo real** (GLide64
  también es HLE): con main ce8351d fusionado, `salsa64-run
  --lle=all` ejecuta el F3DZEX2 real (739 M instrucciones hasta el campo
  3712) y sus triángulos crudos, con los colores de vértice que calcula el
  RSP de verdad, dan los mismos parches en el campo 3700: piso (55,55,2),
  tronco (29,27,15), pared (37,23,14), túnica (53,65,28), frente a la HLE
  (55,54,1), (29,27,15), (37,23,14), (52,65,28). Las luces, normales y
  ambiente de la HLE coinciden con el microcódigo. Y en la imagen del LLE
  la pared lejana sí aparece (el microcódigo recorta contra el frustum
  antes de emitir los triángulos): las zonas crema de la HLE son solo el
  w ≤ 0 del rasterizador de la Mac.
- No se cambió nada de la HLE. Si en la PS3 la escena se ve distinta a la
  Mac (verde liso sin la corteza tenue), el problema sería de src/rdp o
  del RSX, no de la HLE: pendiente de confirmar con el autor.

Herramientas que quedan (sin cambios funcionales):
- Depuración de la HLE en el host con `EXTRA_CFLAGS=-DSALSA64_GFX_TRACE`
  (cabecera de `src/host/run.c`): estado del RDP por dibujo (combiner,
  othermode, tiles, luces, términos k), rango de s/t, vértices de un dibujo
  (`SALSA64_GFX_VDUMP`), texturas en PPM (`SALSA64_GFX_TEXDUMP`), dibujos
  pintados de magenta (`SALSA64_GFX_MARK`) y traza de un píxel
  (`SALSA64_GFX_PIX`) en el rasterizador. En `rdp.c` es solo un gancho bajo
  `#ifdef`; nada entra en el SPE.
- `tools/m64p-ref.sh` + `tools/m64p-pad/pad_plugin.c`: captura de
  referencia con mupen64plus (requiere `brew install mupen64plus`; el
  teclado de su plugin de entrada no funciona con sdl2-compat; el `.sra` de
  mupen64plus lleva los bytes de cada palabra de 32 bits invertidos; sus
  cuadros de `--testshots` no son campos del VI).
- El pad de Kokiri sirve de punto de partida para el menú de pausa y el
  resto de pendientes de Zelda.

Pendiente:
- [ ] PS3: confirmar que Kokiri se ve como en la Mac/mupen64plus (oscuro,
      textura tenue) y que no faltan la pared lejana ni el piso con w ≤ 0.
- [ ] Rasterizador de la Mac: recorte contra w = 0 (hoy descarta el
      triángulo entero), para que las capturas de referencia no tengan
      huecos crema.

## 2026-10-10 — Microcódigos gráficos de Rare: Perfect Dark y Conker

Hecho (rama `competent-haslett-3bad2a`, con main 3270538 y la rama
de arranque `amazing-noether-39a548` con `osCicId`, sin integrar):
- **Perfect Dark** (`src/rsp/gfx/f3dpd.c`): Fast3D de Rare. Solo cambian
  G_VTX (vértices de 12 bytes: x, y, z, banderas, offset del color, s, t) y
  G_VTXCOLORBASE (0x07, la tabla de colores; 4 bytes R, G, B, A o normal y
  A con G_LIGHTING); G_TRI4 en 0xB1 ya lo tenía f3d.c (como GoldenEye) y
  G_QUAD en 0xB5 con índices ×10. Cada vértice se arma en el formato de 16
  bytes y sigue el camino de siempre. El microcódigo no trae texto: se
  reconoce por el CRC-32 de su código (c543d0a8, 0x1000 bytes) solo si
  ningún texto lo identificó (`src/core/rsp.c`); `salsa64_crc32` pasó a una
  tabla de nibbles (dos pasos por byte en vez de ocho).
- **Conker** (`src/rsp/gfx/f3dexbg.c`, «F3DEXBG.NoN fifo 2.08», por su
  texto): F3DEX2 con G_TRI4 en 0x10–0x1F (índices de 5 bits; el 0x14 era
  eso), luces de 48 bytes con luz puntual (posición e intensidad),
  G_MW_COORD_MOD, tabla de normales aparte (G_MV_NORMALS) y la luz que
  multiplica el color del vértice. **Por qué no mandaba otra tarea:** su
  G_LOAD_UCODE no cambia de GBI (pasa a la iluminación avanzada y la lista
  sigue); la HLE de F3DEX2 terminaba la tarea ahí, sin llegar al
  G_RDPFULLSYNC, y el juego esperaba la interrupción del DP para siempre.
  Con main 53fb223 F3DEX2 salta el tramo de un microcódigo ajeno
  (`gfx_dl_skip_foreign`, el S2DEX de Zelda); en Conker eso perdería todo lo
  que dibuja después, así que su hook atiende el 0xDD antes que F3DEX2.
- Cambios en lo común, mínimos: las familias aceptan un *hook* con los
  comandos propios de una variante (`gfx_dl_f3d(g, hook)`,
  `gfx_dl_f3dex2(g, hook)`); geom.c separa la transformación de la lectura
  (`gfx_vertices`) y expone las luces en espacio del modelo y el texgen;
  `gfx_state.rare` (~250 bytes) con el estado de las dos variantes; dos GBI
  nuevos en rcp.h; F3DEX2, F3D y F3DEX no cambian (mismas pruebas y
  mismas imágenes de gfxtest).
- SPE 0: Perfect Dark 1,9 KB y Conker 4,4 KB de código; texto 73 KB (con main
  755cbaa), bss 145 KB, pila ~37 KB (antes ~46; `src/spu/gfx/main.c`). `GFX_MAX_LIGHTS`
  pasó de 8 a 11: Conker pide hasta 11 luces (`G_MW_NUMLIGHT` = 0x210).
- Pruebas: suite `gfx-rare` (9: colores por tabla, luz con la normal en la
  tabla, G_TRI4 y G_QUAD de PD; G_TRI4 de 5 bits, G_LOAD_UCODE que no corta
  la lista, luz básica y avanzada, 11 luces, COORD_MOD, lookat, normales y
  texgen de Conker) y en `rsp` la detección por texto y por CRC (4 KB
  sintéticos con 4 bytes calculados para dar el CRC de PD) y el despacho al
  backend: 297 en el host. Mutaciones: 6 de 6 detectadas.
- Host (respaldos propios, fuera del repo), 3 minutos sin tocar el
  control: **Perfect Dark** 6914 tareas, 0 comandos sin implementar, 0
  errores (identificación del producto en alta resolución, logo de
  Nintendo, cubo de N64 que se vuelve el logo de Rare, la ciudad, Joanna en
  la azotea); **Conker** 4468 tareas, 0 sin implementar, 0 aproximados
  (pantalla legal, logos, Conker con la motosierra, la taberna, menú
  «GAME 1»). Detalle en docs/compatibilidad.md.
- Con main 755cbaa fusionado: build sin advertencias, test-roms (nemu64-test
  446, vitest, controles, savetest, gfxtest ×3 con 22/22, audiotest),
  test-phase1 y test-phase2 en RPCS3 (297/297 big-endian antes de la última
  fusión), test-rec en qemu (nemu64-test con la línea base y vitest con el
  recompilador; las unitarias pasan hasta `gfx-rare` en los cuatro modos y
  se caen en `sync`, como en main). Sin regresión: SM64, Mario Kart 64,
  GoldenEye, Star Fox 64 y Banjo-Kazooie dan, con main y con esta rama, el
  mismo resumen gráfico y la misma imagen a los 60 s.
- Depuración en el host: `EXTRA_CFLAGS=-DSALSA64_DEBUG_DL` vuelca las
  display lists (`SALSA64_DEBUG_DL=N`, `SALSA64_DEBUG_DL_FROM=k`) y
  `-DRASTER_W=640 -DRASTER_H=480` agranda el rasterizador para pantallas de
  alta resolución.

Pendiente:
- [x] **PS3 real** (paquete «Salsa64 Rare», SLSA00081, be8f573 con main
      ce8351d): **Perfect Dark** se ve y suena bien (el autor: «se ve y suena
      espectacular»); 59,9 campos/s (100 % de NTSC) salvo la intro en alta
      resolución (84–94 %), HLE en el SPE 1,4–8 ms por tarea, 0 tareas
      cortadas, naudio con MP3 sin errores. **Mario Kart 64** (no
      regresión): 65 s al 100 % de NTSC, 0 tareas cortadas. `embedded`
      quedó como estaba (`file:oot.z64`).
- [x] **Conker en la PS3** (resuelto en la entrada «Presentación:
      Banjo-Kazooie…» de arriba, falta confirmar en la PS3): arranca y
      suena, pero el 3D sale quemado y parpadea y no se ve el logo de N64; las capturas del RSX de las
      escenas 3D (campos 3000, 3600, 4200) salen negras y el logo 2D de
      Nintendo sí. El volcado de la HLE hecho en el SPE (`gfxdump` del campo
      3600) dibujado en la Mac con `--raster-dump` sale bien e igual al
      host: la HLE es correcta y la falla es de la traducción o la
      presentación del RSX (`src/rdp/rdp.c`, lib/rsxkit). Lo propio de
      Conker: a mitad de cada cuadro dibuja en un destino CI de 8 bits y 64
      de ancho (0xDE080) y vuelve al principal (0x3D6300, 292×213 RGBA16, z
      en 0xE9D80). Pasado al trabajo de presentación (mismo tipo que la pausa
      de Zelda).
- [ ] Perfect Dark aproxima `lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE` (el de
      GoldenEye): término S·Sa en el RSX (Fase 4).
- [ ] Efectos de framebuffer de los dos (cámara espía y monitores de PD,
      copias del z-buffer, desenfoque de la pausa y sombra de Conker): Fase
      4; lista en docs/compatibilidad.md.
- [x] Sonido: naudio de los dos (trabajo de audio, en main desde ce8351d).
- [ ] La luz de Conker sigue a GLideN64 (en el modo básico la direccional
      no cuenta); contrastar con la PS3 cuando haya escenas de juego.

## Pendiente: efectos de framebuffer (anotado el 2026-10-10)

Va después de la presentación (se monta sobre su tabla de destinos).
Detectar cuándo el juego usa como textura o lee con el CPU una imagen dibujada en el RSX; copiar
RSX → RDRAM (bajando de 3× a la resolución de la N64) solo en esos casos, y para el render a
textura usar directo el destino del RSX. Casos:
- [ ] Zelda OoT: fondo difuminado de la pausa (lo difumina el CPU) y Link en la pantalla de equipo
      (render a textura); la pausa ya se ve con la presentación, pero con esos dos defectos
      (confirmado por el autor en la PS3).
- [ ] Mario Kart 64: pantallas gigantes que transmiten la carrera.
- [ ] Perfect Dark: cámara espía y otros; Majora's Mask (Canción del Vuelo, transformaciones).

## Pendiente: presentación (anotado el 2026-10-10)

Resuelto en la entrada «Presentación: Banjo-Kazooie…» de arriba (falta
confirmar en la PS3). Los tres casos estaban en `src/rdp/rdp.c` (qué
destino del RSX se presenta en cada campo):
- [x] **Banjo-Kazooie parpadea** muy fuerte (visto por el autor en la PS3;
      video de 1,6 s en la intro, Banjo tocando el banjo). Tres síntomas: (a)
      ~1 de cada 3 cuadros del video (30 cps) sale casi negro con solo la parte
      de arriba de la cabeza de Banjo: imagen vieja o a medio dibujar
      presentada entre las buenas; (b) toda la escena sale desplazada a la
      derecha y abajo (mitad izquierda vacía, Banjo cortado en el borde
      derecho, el pasto solo en un rectángulo abajo): revisar origen/escala del
      VI, viewport y scissor; (c) falta el fondo (cielo y paisaje negros).
      Revisar cómo alterna sus framebuffers y si usa efectos que leen el
      framebuffer.
- [x] **Menú de pausa de Zelda negro en la PS3:** el juego pone color images
      de 64 de ancho en la dirección del framebuffer y se pierde el destino de
      320 («0 del RSX»); en el rasterizador de la Mac se ve bien.
- [x] **Recorte del overscan:** GoldenEye deja franjas azules a los costados
      (su scissor 3D cubre las columnas 1–318 sobre un relleno azul).

## 2026-10-10 — Fase 6: prototipo de LLE del RSP (microcódigo real) en host y SPE

Hecho (rama `nervous-lewin-ae6137`, desde main 3270538; sin
integrar; diseño y mediciones en docs/rsp-lle.md):
- **Intérprete portable del RSP** (`src/rsp/lle/`: escalar, COP0, unidad
  vectorial completa con el acumulador de 48 bits y VCO/VCC/VCE, recíprocos
  por tabla, todas las cargas/almacenamientos vectoriales). Semántica de
  los casos raros de ares (ISC, registrado en THIRD_PARTY.md). Mismo código
  en host, PPE y SPE, sin `#ifdef` de orden de bytes.
- **nemu64-test con `--lle`: fallan 136 de 4637 (97 %) frente a 433 (90 %)
  sin LLE. Las 214 pruebas «RSP …» pasan todas**; quedan 4 «RDP STATUS»
  (el RDP no escribe el framebuffer en la RDRAM). Sin regresiones; 5
  mejoras en el camino HLE (pares de bits de SP_STATUS, DPC_START/END).
  `tools/test-roms.sh` corre nemu64 dos veces (líneas base
  `nemu64-test.fallas`, 289 nombres, y `nemu64-test-lle.fallas`, 71).
- **Enganche** (`rsp.c`: backend `salsa64_lle_backend`, modos
  `off/auto/all`; `sp.c`: sincronía, SP_STATUS atómico, DPC con semántica
  real) y **RDP a bajo nivel** (`src/rsp/gfx/dpc.c`: los comandos crudos
  de DPC_START..END, también por xbus, van a `gfx_rdp_command`/
  `gfx_rdp_triangle`, el camino de triángulos crudos existente).
- **Host**: `salsa64-run --lle[=auto|all]`, `--lle-speed`,
  `--lle-check-audio` (cada tarea de audio también en la HLE sobre una
  copia de la RDRAM, comparando lo escrito). SM64: el Fast3D real corre y
  el título con la cara de Mario sale igual que en HLE. Zelda: el nead
  real suena (211 M instrucciones en 16 s); las primeras 30 tareas de
  audio idénticas a la HLE byte a byte, y desde el primer sonido difiere
  solo FILTER, donde mupen64plus-rsp-hle promedia las tablas (`>> 1`) y el
  microcódigo real no → **la HLE de FILTER probablemente va a la mitad**;
  confirmar de oído.
- **SPE** (`src/spu/rsp/`, `src/platform/ps3/rsp_spu.c`): trabajo de
  spukit con DMEM/IMEM en el local store, SP_STATUS y semáforo por DMA
  atómico (getllar/putllc) contra los `__atomic_*` del PPE, DMA del SP
  entre la RDRAM y el local store, registros del DP en el SPE con captura
  de los comandos xbus para que el PPE los reproduzca (`salsa64_dp_submit`),
  continuaciones cada 200 000 instrucciones. Frontend: archivo
  `SALSA64_DATA_DIR/lle` con `auto` o `all`; líneas «rsp lle:» en el log.
- 12 pruebas unitarias nuevas (301 en total con main).
- Medido en el host: 12,5 ns por instrucción del RSP (docs/perf-log.md).
- **PS3 real**: sanidad con HLE por omisión bien (Zelda
  106 s al 100 % con audio; GoldenEye intro al 100 %, nivel a 36–50
  campos/s con el audio estirado: primera medición del nivel, no es de
  esta rama). Con `lle` = `all`, **Zelda se ve y suena con el microcódigo
  real en el SPE**: 226–283 ns por instrucción (3,5–4,4 MIPS; el RSP real
  ~62), 24–55 campos/s. Tres arreglos sobre la marcha (SP_PC pisado al
  recoger el trabajo, estado del DP entre trabajos, DMA de muchas filas de
  S2DEX en una transferencia) e instrumentación (contadores de COP0,
  latido del SPE). Queda una divergencia: tras los fondos S2DEX de la
  intro (~36 s) el microcódigo entra en un bucle sin fin que el host no
  reproduce (detalle y pista en docs/rsp-lle.md).

Pendiente:
- [ ] Divergencia del SPE tras S2DEX: modelo del protocolo de trabajos del
      SPE en el host (`lle_spu_run` con memcpy y cortes por `max_instr`)
      para reproducirla con trazas; sin turnos de consola.
- [ ] Confirmar de oído FILTER (HLE a la mitad vs microcódigo real).
- [ ] Comparar la imagen de Zelda en LLE con la HLE (host, `--gfx-frame`).
- [ ] Recompilador RSP → SPU (plan en docs/rsp-lle.md): es lo único que
      lleva de 0,06× a velocidad real; mover el consumidor del RDP al SPE.
- [ ] Homebrew de libdragon (rspq/rdpq) como prueba sin HLE.
- [ ] Limitaciones del SPE: escrituras del CPU en DMEM/IMEM durante una
      tarea, limpiar la interrupción del SP desde el microcódigo.

## 2026-10-10 — Zelda OoT se colgaba al elegir la ranura: G_LOAD_UCODE (S2DEX) y el RDP que nunca terminaba

Síntoma (PS3 real, main f698afe): al elegir una partida, pantalla negra con
una rayita amarilla arriba a la izquierda, el audio seguía (en silencio) y
no llegaba ninguna tarea gráfica más; 96 % en bucles de espera.

Diagnóstico (rama `fervent-antonelli-6904bf`, en la Mac con el
intérprete, sin PS3):
- Se reproduce igual con el intérprete y la HLE síncrona: no es del
  recompilador, ni del sondeo del SPE, ni del DMA del PI.
- Al cargar la primera sala (la casa de Link, con fondo prerrenderizado) el
  juego manda 75 tareas tipo 4 (su decodificador JPEG; sin HLE, terminan sin
  hacer nada) y una tarea gráfica cuya lista carga **S2DEX** con
  `G_LOAD_UCODE` para copiar el fondo (`uObjBg`) y después vuelve a F3DZEX.
  La HLE terminaba la tarea en el `G_LOAD_UCODE`, sin llegar al
  `G_RDPFULLSYNC`: TASKDONE sí, interrupción del DP no. El planificador de
  libultra nunca dio la tarea por terminada, el hilo gráfico de OoT esperó
  su mensaje y a los 3 s su vigilante declaró «RCP is HUNG UP»: el hilo de
  fallas dibuja el rectangulito amarillo (el de `Fault_DrawCornerRec`) y el
  juego queda muerto con el audio vivo. Visto con el volcado de la RDRAM:
  todos los hilos esperando en colas vacías.
- Nadie había pasado de la selección de archivo: no era regresión.

Arreglo:
- `src/rsp/gfx/f3dex2.c`: un `G_LOAD_UCODE` a otra dirección que la del
  microcódigo de la tarea (`job->ucode`, nuevo en `gfx_job`) ya no termina
  la lista: se saltan sus comandos (siguiendo `G_DL` y `G_ENDDL`, que la
  familia comparte) hasta el `G_LOAD_UCODE` que vuelve al de la tarea, y la
  lista sigue (Link, objetos, HUD y el FULLSYNC se ejecutan; el fondo
  prerrenderizado queda negro). `res->skipped` cuenta los saltados; la
  bandera `GFX_RES_LOAD_UCODE` marca que hubo un tramo ajeno.
- `src/core/rsp.c`: si la HLE corta una lista antes de su fin
  (`SALSA64_GFX_CUT`: búfer lleno, lista interminable, error de dirección;
  lo traduce `salsa64_gfx_task_flags` en sync.c y gfx_spu.c), la
  interrupción del DP llega de todos modos: el juego espera al RDP por cada
  tarea gráfica. Estadística `cut` (frontend: «tareas cortadas» y «comandos
  de otro microcódigo saltados» en la línea `gfx:`).
- Pruebas: `rsp_gfx_task_cut` (DP con lista cortada; sin DP si acabó en
  ENDDL sin FULLSYNC) y `gfx_load_ucode_skips_foreign` (mismo microcódigo
  sigue; tramo ajeno con subrutina saltado, 2 triángulos, FULLSYNC; tramo
  que nunca vuelve = cortado; FULLSYNC dentro del tramo cuenta). La ROM
  gfxtest lleva al final de cada lista un tramo de otro microcódigo con un
  FILLRECT rojo que taparía la escena si se ejecutara, y en el cuadro
  GFXTEST_CUT_FRAME (7) termina dentro de él: la ROM exige la interrupción
  del DP y test-roms.sh exige «1 tareas cortadas» y tramos saltados. 272
  unitarias en el host (clang nativo y Docker con ASan); test-roms
  (nemu64-test 446, vitest, controles, savetest, gfxtest 22/22, audiotest)
  OK; build sin advertencias.
- Mac: con el arreglo el juego sigue (tareas gráficas a 20/s, la escena del
  Árbol Deku con su texto); ~7 comandos de S2DEX saltados por cuadro.

Herramientas nuevas para esto (`salsa64-run`): `--pad
"CAMPO:BOTONES[@X,Y];…"` (control 1 programado por campo del VI, cada estado
hasta el siguiente; unificado con el `--pad` que traía main: misma sintaxis,
campo por campo y también en la referencia de `--lockstep`), `--trace-from CAMPO`,
`--trace-faults N` (excepciones sin las interrupciones), `--ram-dump
ARCHIVO`, y `tools/libultra-threads.py` (hilos de libultra en un volcado:
estado, pc y la cola en la que espera cada uno, sin símbolos). Con `--pad
"2200:START,2320:A,2440:A,2560:A"` y el `.sra` del autor en `--save-dir`,
Zelda entra al juego en el campo 2712.

- **PS3 real** (paquete «Salsa64 depuración», SLSA00079, ba9bdfc): Zelda
  entra al juego y el autor jugó 7 min (intro, casa de Link, Navi, bosque
  Kokiri) a 59,8 campos/s, 0 tareas cortadas, ~7 comandos de S2DEX
  saltados por cuadro en la casa; audio −4 a −11 dBFS en juego. El silencio
  de ~30 s entre elegir la partida y el vuelo de Navi es del juego: la
  Mac con sus mismas pulsaciones calla y suena en los mismos campos, y
  en ese tramo las listas de audio no traen voces (solo CLEARBUFF,
  LOADBUFF, MIXER, SAVEBUFF, DMEMMOVE, INTERLEAVE).

Con main 485a747 traído (F3D, arranque del CIC 6105 con osCicId): Zelda
sigue pasando la ranura en la Mac (el arranque nuevo no cambia nada aquí);
el salto de tramos ajenos vive ahora en `gfx_dl_skip_foreign` (dl.c) y lo
usan F3DEX2/F3DZEX y F3DEX 1.x (F3D no tiene G_LOAD_UCODE: la variante F3D
de gfxtest no lleva el tramo). Con `job->ucode` = 0 (tarea sin microcódigo
conocido, como en las pruebas de F3D) todo G_LOAD_UCODE es ajeno.

Visto en esa corrida, para otros trabajos (reproducible en la Mac con
`--pad "$(cat docs/pads/zelda-oot.pad)"`, las pulsaciones del autor
sacadas de su log; receta en la entrada de arriba):
- [x] **Texturas del mundo**: en el bosque Kokiri la geometría salía en
      colores planos (verde del shade), igual en el rasterizador de la Mac.
      No era error: es el sol cenital de mediodía (ver la entrada «Kokiri
      “en colores planos”», con la referencia de mupen64plus). Queda
      confirmar en la PS3 que se ve como en la Mac. *(2026-10-11: en las
      salas prerrenderizadas sí era error; resuelto con la tarea JPEG y
      S2DEX, entrada del 2026-10-11.)*
- [x] **Menú de pausa en negro** (frontend/rdp; resuelto por el trabajo de
      presentación, `src/rdp/targets.c`): con el menú abierto cada
      tarea (30/s) pone tres color images: dos de **64 de ancho** (0x3b8800
      y **0x3b5000, la misma dirección del framebuffer**) y luego
      0x3b5000/320; el VI sigue mostrando 0x3b5280/0x3daa80. En la PS3 el
      log dice «0 del RSX» y ~2 destinos creados por tarea: el destino de
      320 se pierde al crear uno de 64 en la misma dirección y la
      presentación cae a la copia de la RDRAM (negra, el RSX no la
      escribe). Le toca a src/rdp (destinos por dirección y ancho, o no
      tirar el de 320). Cerrar el menú vuelve al juego. Reproducible en la
      Mac: `--pad "$(cat docs/pads/zelda-oot.pad)" --trace-from 19100
      --trace-gfx 80` muestra los TARGET, y con `--gfx-frame-every 6` el
      rasterizador de referencia dibuja el menú (mapa del bosque Kokiri)
      bien: la HLE está bien, es solo la presentación.

Falta (para ver bien la casa de Link y las salas con fondo prerrenderizado):
- [x] HLE de S2DEX para el fondo (`G_BG_COPY`/`G_BG_1CYC` con `uObjBg` →
      rectángulos con textura por tiras) y detectar el microcódigo que
      carga `G_LOAD_UCODE` por el texto de sus datos (como en rsp.c).
      *Hecho el 2026-10-11 (`src/rsp/gfx/s2dex.c`).*
- [x] *(2026-10-11, `src/rsp/jpeg/`)* Tarea tipo 4 de OoT (decodificador JPEG `njpgdspMain`: IDCT y
      YUV→RGBA16 de los macrobloques que el CPU ya decodificó): sin ella el
      fondo sale negro aunque S2DEX lo copie. mupen64plus-rsp-hle la tiene
      (`jpeg.c`, GPLv2+; registrar en THIRD_PARTY si se adapta). Son 75
      tareas por sala, una vez (no por cuadro).

## 2026-10-10 — naudio: Banjo-Kazooie, Conker, Perfect Dark (con MP3) y Donkey Kong 64

Hecho (rama `charming-allen-a8e4b3`, con main 485a747):
- **naudio** en la HLE portable (`src/rsp/audio/abi.c`): las tablas de
  naudio, Banjo-Kazooie (la misma), Donkey Kong 64 (7 y 8 son MIXER) y los
  de MP3 (Perfect Dark, Banjo-Tooie, Jet Force Gemini y Conker: 0
  OVERLOAD, 7 MP3, 8 MP3ADDY, 0x0E filtro). Búferes fijos de 0x170 bytes
  desde 0x4F0; ADPCM y RESAMPLE con el estado en w1; SETVOL con sus tres
  formas y el 0x0E que salta a la mitad de SETVOL. IIR y OVERLOAD en
  `alist.c`. Codificación verificada contra `alist_naudio.c` de
  mupen64plus-rsp-hle (8a7a472a). La detección ya existía
  (`src/core/rsp_audio.c`, palabra [0x10] de `ucode_data`).
- **MP3** (`src/rsp/audio/mp3.c`): la síntesis de MPEG-1 Layer III que el
  microcódigo hace en el RSP (DCT de 32 y ventana de 512), adaptada de
  `mp3.c` del original e igual a él bit a bit (2304 muestras comparadas con
  el original compilado aparte, fuera del repo). La ventana **no** es la
  tabla del microcódigo: `tools/audio-mp3-lut.py` la genera desde la
  ventana estándar (ISO 11172-3, vía `ff_mpa_enwindow` de FFmpeg, LGPL);
  coincide con la del original en todas las entradas que se leen.
  Memoria de trabajo aparte de la DMEM de las listas (2 KB, persiste entre
  comandos como el anillo de la ventana). Las dos palabras de ganancia del
  comando: Perfect Dark manda +16 y −16; la segunda corrige el signo de
  las muestras 17–31 de las ranuras impares.
- **SPE 1**: text 43 456, data 832, bss 22 416 bytes (~66 KB de 256 KB; la
  cabecera de `src/spu/audio/main.c` lleva el desglose). Cabe sin
  overlays.
- **Pruebas** (suite audio, 23): cada comando naudio con listas sintéticas
  y resultados a mano; el IIR contra una referencia directa en dos bloques
  (sigue del estado); el MP3 contra la síntesis de MPEG-1 en coma flotante
  (escala 1,00002, error rms 4,6 LSB sobre 3066, máximo 19,6). Listas
  naudio con y sin MP3 en la autoprueba SPE contra PPE del arranque
  (iguales en RPCS3).
- **Mac** (`salsa64-run`, 3 min de consola, respaldos propios): Banjo-
  Kazooie 5373 tareas, Conker 5389 y Perfect Dark 5216, todas **sin
  errores ni comandos sin implementar** (el censo: todas con error). Banjo
  y Perfect Dark suenan con niveles normales (−14 a −24 dBFS); Perfect
  Dark manda 3166 comandos MP3 en 3 min y su PCM es continuo (sin saltos
  en las fronteras de las ranuras). Conker sale en silencio: sin su
  microcódigo gráfico se queda tras la primera tarea gráfica y no dispara
  voces (no manda ADPCM ni ENVMIXER; solo el bus de efectos). WAVs para
  escuchar en `build/naudio/` (fuera de git).
- Sin regresiones: `tools/build.sh` sin advertencias, test-host en
  verde (295 con main), `tools/test-roms.sh` (nemu64-test 446, vitest, controles,
  savetest, gfxtest ×3, audiotest) y test-phase1/2 en RPCS3 OK.

- **PS3 real** (paquete «Salsa64 naudio», SLSA00080): **Banjo-Kazooie y
  Perfect Dark suenan bien** (el autor), 0 errores y 0 comandos sin
  implementar; HLE en el SPE 1 por tarea: mediana 2,9 ms en Banjo y 5,7
  ms en Perfect Dark (la más larga 10,4 ms), el PPE nunca espera.
  Conker: pantalla negra y silencio como en la Mac, sus tareas sin
  errores (1,6 ms). Detalle en docs/perf-log.md. Respaldos subidos a
  `/dev_hdd0/salsa64/` como `banjo.z64` y `conker.z64`; `embedded` quedó
  en `file:perfectdark.z64`, como estaba al empezar.

- **Después, al integrar:**
  - `tools/test-rec.sh` se caía en la suite `sync` bajo qemu: SIGILL por
    `mffscrni` en `sin`/`cos`/`floor` de la glibc estática de ppc64 (Power
    ISA 3.0; qemu 7.2 solo lo acepta desde power9). `tools/qemu-ppc64.sh`
    usa `-cpu power9` (commit df3d652, rama `qemu-sync-fix` sobre
    main); test-rec en verde, también `--lockstep`, y con naudio y MP3
    (297 pruebas en big-endian).
  - **FILTER** (nead: Zelda, Majora…) corregido contra el microcódigo real
    con la herramienta del LLE (`--lle=all --lle-check-audio`, rama del
    LLE): la tabla de la nota no se escribe; la de detrás del estado pasa a
    `(anterior + nota + 1) >> 1` y con ella se filtra. La HLE dejaba el
    filtro a la mitad (−6 dB) y reescribía la tabla de la nota. Se oye en
    los primeros 15 s de Zelda (WAVs viejo/nuevo en `build/naudio/`).
    **PS3 real: Zelda «suena bien»** (el autor), SPE 2,1 ms por tarea.
  - **MIXER** redondea el producto como el microcódigo (+0x4000): con el
    LLE, Zelda pasa de 952 k a 210 k muestras distintas, Perfect Dark de
    1,80 M a 1,46 M, SM64 de 788 a 761 tareas distintas.
  - naudio contra el microcódigo real (LLE): Conker 476 tareas idénticas;
    Perfect Dark idéntico hasta la tarea 117 (cuando empieza a sonar). Lo
    que queda es RESAMPLE (tabla propia, a propósito) y el estado de
    ENVMIXER (formato propio: la HLE lee como suyo el que escribe el LLE,
    así que solo se compara la primera tarea de cada voz). Para medir
    naudio hace falta subir AUD_MAX_RANGES en el host (con 10 los tramos se
    juntan con memoria que el CPU escribe mientras tanto).

Notas:
- Banjo-Kazooie en la PS3: el **video parpadea** (uno de cada dos cuadros
  sale incompleto: sin suelo y con medio Banjo) y el fondo sale negro. Es
  de la HLE gráfica o la presentación, no del audio: para el trabajo de F3D.
- OVERLOAD recorre 0x170 muestras como el original (el doble que los
  demás comandos, que cuentan 0x170 bytes); ningún juego lo mandó en las
  escenas a las que llegamos, así que queda sin verificar.
- Conker: 4926 huecos entre DMA del AI en 3 min (el juego detenido); no es
  de la HLE.

Falta:
- [ ] Oír Conker cuando el trabajo de F3D lo haga avanzar (F3DEXBG).
- [ ] Probar Donkey Kong 64 (sin respaldo; la tabla es la del original).
- [ ] MusyX (Rogue Squadron, Resident Evil 2, Indiana Jones…).
- [ ] Opcional, con el LLE integrado: estado de ENVMIXER en el formato del
      microcódigo (para poder comparar más allá de la primera tarea de
      cada voz) y revisar los ±1 LSB que quedan en naudio.
- [ ] docs/compatibilidad.md: la columna de audio de Banjo, Conker y
      Perfect Dark (es del censo).
## 2026-10-10 — Arranque del CIC 6105 y censo de compatibilidad

Hecho (rama `amazing-noether-39a548`; detalle en docs/cpu-vr4300.md,
«Arranque HLE», y docs/compatibilidad.md):
- `src/core/boot.c`: con el CIC 6105/7105, el arranque HLE deja lo que su
  IPL3 deja además en la RDRAM: copia de DMEM 0x554–0x888 a 0x004–0x338
  (desde la ROM del usuario; de ahí sale 0x2E8 = 0xC86E2000), el efecto del
  DMA con paso de su microcódigo del RSP (24 filas de 8 bytes en 0x2FB1F0,
  cada 0xFF0, desde RDRAM 0x1E8 vía IMEM 0x120) y el tamaño de la RDRAM en
  0x3F0. Solo direcciones, cotejadas entre fuentes (docs/THIRD_PARTY.md) y
  verificadas con dos palabras del Perfect Dark del autor. No hay más
  efectos documentados por variante (6101/6102/6103/6106/7102: dirección
  de arranque y semilla, que ya estaban).
- `osCicId` (0x310) = 6103 o 6105 con esos IPL3: Conker solo arranca su
  hilo principal si vale 6105 (se quedaba en el hilo ocioso a los 88 mil
  ciclos); Banjo-Kazooie y Perfect Dark también lo comprueban.
- `salsa64_boot_hle_as` fuerza la variante; pruebas por variante y de los
  restos del 6105 (287/287 en el host con main 3270538).
- Perfect Dark pasa la comprobación en la Mac y en la **PS3 real**
  (frontend «Salsa64 arranque», SLSA00078): 6083 bloques (6078 fuera de
  KSEG0) en vez de 201, 693 traducciones de la TLB, 59,8 campos/s con 5,8
  ms por campo, pantalla negra. Se queda en su hilo ocioso esperando al DP
  tras su primera tarea gráfica (microcódigo de Rare sin texto, sin HLE).
  `--lockstep` en qemu: 10,7 s de consola sin diferencias.
- `salsa64-run`: `--trace-rsp` muestra el CRC-32 del microcódigo y de
  `ucode_data`, los textos de `ucode_data` en las tareas gráficas y las
  palabras que distinguen el audio; muestreo del PC por segmento al final
  (KSEG0/KSEG1/TLB). `--lockstep` da a la referencia una copia del
  guardado: sin ella su PIF respondía «sin dispositivo» en el canal de la
  EEPROM y salía una diferencia falsa (Perfect Dark, ciclo 254 M).
- `tools/censo.sh`: censo de compatibilidad con los ROMs del usuario
  desde su ruta (compila `salsa64-run` con el compilador de la Mac).
- Censo de 3 minutos de 8 juegos (los 6 objetivo, Banjo-Kazooie y Conker)
  sobre main 3270538 (F3D integrado): dibujan SM64 (título),
  OoT, MK64, SF64, GoldenEye y Banjo; suenan ABI 1 y nead. Perfect Dark
  corre sin dibujar ni sonar (su microcódigo de Rare y naudio con MP3 sin
  HLE); Conker manda una sola tarea gráfica (F3DEXBG 2.08, comando 0x14) y
  su naudio da error. Sin la rama de F3D, solo Zelda dibujaba.

Siguiente (lista priorizada en docs/compatibilidad.md):
- [ ] Audio: las tres naudio (Banjo-Kazooie, Conker, Perfect Dark con MP3).
- [ ] Microcódigo de Perfect Dark (por CRC; sin texto) y F3DEXBG de Conker.
- [ ] Probar con el control y medir en la PS3 las escenas de docs/proyecto.md.

## 2026-10-10 — F3D (Super Mario 64) y F3DEX 1.x

Hecho (rama `lucid-pare-4f93fb`, con main traído hasta f698afe; sin
integrar):
- HLE de la primera generación de microcódigos en `src/rsp/gfx/f3d.c`: F3D
  (Fast3D: búfer de 16 vértices, índices ×10, G_TRI1, G_MTX con sus
  banderas, G_MOVEMEM/G_MOVEWORD con sus índices, geometry mode en dos
  comandos con otros bits, G_SETOTHERMODE_* directo, TEXRECT con
  G_RDPHALF_2 + G_RDPHALF_CONT) y F3DEX 1.x (búfer de 32, índices ×2,
  G_TRI2, G_QUAD, G_MODIFYVTX, G_CULLDL, G_BRANCH_Z, G_LOAD_UCODE). Lo
  común salió de `f3dex2.c` a `dl.c` y `geom.c` sin cambiar F3DEX2 (mismas
  31566 verificaciones). `rsp.c` manda F3D y F3DEX al backend. En el SPE 0
  son 3,3 KB de código (presupuesto en `src/spu/gfx/main.c`).
- Pruebas: 11 nuevas en el host (`tests/unit/test_gfx_f3d.c`) más la
  detección y el despacho en `test_rsp.c`: 223 en total.
- gfxtest en tres variantes (`tools/build-gfxtest.sh`; `GBI=` en su
  Makefile): F3DEX2 (la ROM sale idéntica), F3D y F3DEX. En el host las tres
  dan los mismos puntos y la misma imagen (`tools/test-roms.sh`, que además
  falla si hay comandos sin implementar): 19 de 19 antes de traer main y
  22 de 22 con las escenas de la caché de texturas. La de F3D también en
  RPCS3 (`tools/test-phase2.sh`, 22 de 22) y en la **PS3 real** (19 de 19,
  `RESULTADO frontend: OK`, 0,18 ms por tarea en el SPE). El frontend verifica la pantalla de las
  variantes cargadas con `file:salsa64-gfxtest-*.z64` como la embebida.
- **SM64** (respaldo propio, USA, fuera del repo): en el host llega al
  título y a las demos (Bowser, Whomp's Fortress) sin comandos sin
  implementar ni errores en 2 minutos; en la **PS3 real** el autor confirmó
  el título y la selección de archivo («todo se ve bien»). Mediciones en
  docs/perf-log.md: velocidad completa en el logo y en la selección de
  archivo, 67 % de NTSC en la cara de Mario (la anima el CPU).
- Otros juegos del usuario en el host (respaldos fuera del repo), sin
  comandos sin implementar: **GoldenEye** usa Fast3D 2.0G con `G_TRI4`
  (0xB1, cuatro triángulos con índices de 4 bits; antes 140 223 comandos
  sin implementar, ahora la intro se ve con la orientación bien);
  **Mario Kart 64** trae «F3DEX 0.95», que ahora se trata como F3DEX 1.x
  (logo y carrera de demostración); **Star Fox 64** (F3DEX.NoN 1.22) y
  **Banjo-Kazooie** (F3DEX 1.21) corren con F3DEX 1.x.
- Una tarea gráfica con microcódigo sin HLE termina también del lado del
  DP (ningún juego se cuelga esperando al RDP) y el frontend la anota en
  el log con el texto del microcódigo (`salsa64_rsp_ucode_text`, también
  en `salsa64-run --trace-rsp`).
- **PS3 real con main fusionado** (recompilador, dos hilos, caché,
  sondeo): SM64 sin franjas en la transición y al 100 % de NTSC en el
  logo, la transición y la cara (antes 55–67 %); GoldenEye: la intro se
  ve, al 100 % de NTSC. El autor vio después dos errores, arreglados y
  confirmados por él en la PS3:
  - La cara del Mario 3D (de frente): el combiner de los ojos,
    (TEXEL0 − SHADE)·TEXEL0_ALPHA + SHADE, multiplica la textura por su
    alfa y combine.c lo aproximaba. Ahora pide la textura premultiplicada
    (RGB·A al decodificar) y queda exacto (2c4b3f4): 0 combiners
    aproximados en SM64.
  - El cielo de GoldenEye «infinito»: lo dibuja con triángulos crudos del
    RDP partidos en G_RDPHALF_1 + G_RDPHALF_CONT/2, que la HLE no
    dibujaba. Ahora se convierten en triángulos (2c53149).
- Después de integrar (main 3270538), confirmados por el autor en la PS3:
  - El audio de SM64, GoldenEye y Mario Kart 64 suena bien.
  - GoldenEye sin ruido entre el logo y las carpetas: el frontend elige
    el destino del ancho que muestra el VI (GE usa la misma dirección
    para 320 y 440; 66a3870) y sigue presentando el destino mientras el VI
    sostiene la imagen sin dibujar (un segundo de carga; antes salía la
    RDRAM con datos viejos; 10e869e).
  - Mario Kart 64 con título, menús y letreros: F3DEX acepta TEXRECT con
    G_RDPHALF_2 + G_RDPHALF_CONT, la secuencia de la libultra temprana
    (7343e2f), y los TEXRECT en tiras sin costuras a más resolución:
    limitar el eje si todo cae dentro de la tile y muestrear en la
    esquina del píxel con filtro bilineal (8ec50de).
- `salsa64-run --pad "CAMPO:BOTONES[@X,Y];…"`: control 1 por campos para
  llegar a escenas de juego en el host (SM64 hasta el castillo, GoldenEye
  hasta Dam).
- Arreglo en `src/rdp/rdp.c` (3729f8f): unos cuadros con franjas de
  colores en el paso del logo a la cara. SM64 apaga el VI 26 campos al
  cambiar de escena; lo dibujado entonces quedaba marcado con el campo de
  antes del apagado y el frontend, al volver la imagen, presentaba la RDRAM.
  Simulado con la traza del host: de 17 campos sin imagen del RSX a 0.

Pendiente:
- [ ] Confirmar en la PS3 el arreglo de la transición (paquete «Salsa64
      F3D», SLSA00071) y dejar `embedded` como estaba (`vitest`).
- [ ] Combiners aproximados en las demos de SM64 (cientos por minuto):
      ver cuáles son (`combine.c`).
- [ ] En la PS3: GoldenEye, Mario Kart 64 y Star Fox 64 con este
      paquete (en el host ya dibujan).
- [x] Confirmar en la PS3 la cara de Mario y el cielo de GoldenEye.
- [x] Mario Kart 64: título, menús y letreros (ver arriba). Quedan las
      pantallas gigantes de las pistas, que muestran el framebuffer
      anterior como textura (efecto de framebuffer, Fase 4).
- [ ] Perfect Dark: corre sin dibujar con un microcódigo de Rare sin
      texto (CRC c543d0a8). Conker: «F3DEXBG.NoN fifo 2.08» tomado como
      F3DEX2, deja 2 comandos sin implementar (el último 0x14) y no manda
      otra tarea.
- [ ] GoldenEye: dos combiners del nivel multiplican el sombreado por su
      alfa (lerp(ENV, TEXEL0, SHADE_ALPHA)·SHADE: niebla por luz
      precalculada) y se aproximan (75 000 en 52 s de Dam). Hace falta un
      término S·Sa por vértice en la combinación del RSX (rsxkit y
      src/rdp) o el generador de fragment programs de la Fase 4.
- [x] GoldenEye: las franjas azules de los costados son del juego (3D con
      scissor en las columnas 1–318 sobre un relleno 0x1199); en una tele
      quedan en el overscan. Opción de recorte en el frontend
      (`overscan.cfg`, trabajo de presentación).
- [ ] Star Fox 64: un cuadrito con textura abajo a la izquierda en el
      logo (host). Revisar.
- [ ] G_LINE3D de F3D (no lo usan SM64 ni GoldenEye) sin implementar.
- Para correr un respaldo en el host sin copiarlo: colima no comparte
  todas las carpetas del host, así que `salsa64-run` se compila nativo en la
  Mac (`make -C src/host OUT=$PWD/build/host-mac CC=clang SAN=`) y lee la
  ROM en su lugar.

## 2026-10-10 — Audio de punta a punta: AI, HLE de audio en el SPE 1 y salida en la PS3

Hecho (rama `amazing-varahamihira-3027c8`, con main cfa146b):
- **AI** (`src/core/ai.c`, desde n64brew.dev): cola de dos DMA, AI_STATUS
  (BUSY/FULL/ENABLED), AI_LENGTH que cuenta hacia abajo y los demás
  registros que se leen como él, frecuencia = reloj del VI de la región /
  (AI_DACRATE + 1), interrupción al **empezar** cada DMA, AI_CONTROL que
  detiene el DMA y el acarreo de los límites de 8 KB (libultra lo
  compensa). Un evento del planificador por DMA; las muestras van a un
  sumidero al empezar cada DMA.
- **HLE de audio** (`src/rsp/audio/`, adaptada de mupen64plus-rsp-hle,
  GPLv2+, avisos en cada archivo y docs/THIRD_PARTY.md): ABI1 (Super Mario
  64; variante de GoldenEye) y todas las variantes nead (Zelda OoT, Mario
  Kart 64, Star Fox 64…). DMEM en el orden de la N64 con índices de la N64
  (el original dependía del host little-endian en FILTER e INTERLEAVE),
  E/S explícita de la RDRAM, tabla de RESAMPLE **propia**
  (`tools/audio-lut.py`; la del original es del microcódigo), libro de
  códigos de 16 predictores (el original leía fuera de la tabla). El núcleo
  reconoce el ABI por las palabras de `ucode_data` (`rsp_audio.c`). ABIs
  por juego objetivo: SM64 ABI1, GoldenEye ABI1 (GE), Zelda OoT / Mario
  Kart 64 / Star Fox 64 nead, Perfect Dark naudio con MP3 (pendiente).
- **SPE 1** (`src/spu/audio/`, `src/platform/ps3/audio_spu.c`): una tarea
  por trabajo de lib/spukit, E/S por DMA (escrituras en trozos de
  alineación natural: nunca reescribe RDRAM vecina), fin de tarea por
  sondeo (`done`), aviso de lo escrito con `salsa64_bus_rdram_written`.
  Local store: 27 KB de código + 20 KB de datos. Al arrancar compara
  listas de prueba (`selftest.c`) en el SPE y en el PPE.
- **Salida** (`src/frontend/audio_out.c`, `audio_sync.c`): anillo sin
  bloqueos entre la emulación y un hilo de audio que llena el puerto de
  PSL1GHT (48 kHz, 16 bloques de 256, cuatro por delante). **Ritmo:** lo
  marca pacer.c (la tele emulada); el audio lo sigue con una corrección de
  ±0,5 % del remuestreo. **Política** (`audio.cfg`): «estirar» (por
  omisión: si la emulación se aparta más de un 3–5 % sigue su velocidad
  con WSOLA sin cambiar el tono; si se vacía repite lo reciente hasta 150
  ms y luego calla) o «silencio» (calla con 10 ms de desvanecido y sigue
  donde quedó). Toda transición pasa por la suma con ventana: sin
  chasquidos, verificado en el host con productores al 27 %, 100 % y 130 %.
  Diagnóstico: `volcado_s` (WAV de lo que sale por el AI, nunca al repo),
  `tarea_us`, pico del AI en dBFS en el log. Memoria: 0,3 MB.
- **ROM audiotest** (`tests/roms/src/audiotest`, propia, IPL3 libre): tono
  conocido por el CPU (La 4, Do izquierda, Mi derecha, Sol, Do 6) y luego
  por una tarea de audio («Salsa64 ABI NEAD»); se verifica sola y
  `salsa64-run --audiotest-check` compara lo que sale por el AI muestra por
  muestra. En `tools/test-roms.sh`.
- Pruebas: suites ai (8), audio (15) y sync (5); 269 en total en el host y
  en RPCS3 (big-endian). Pruebas de mutación: romper el acarreo, el orden
  de INTERLEAVE, la escala del ADPCM o el solapamiento del estirador las
  hace fallar. nemu64-test 446 fallas (sin cambios), vitest, controles,
  savetest, gfxtest y audiotest OK; build sin advertencias. RPCS3:
  audiotest OK en el frontend (renderer de audio Null en las pruebas).
- **PS3 real** (paquete «Salsa64 audio», SLSA00072): audiotest suena bien
  (afinador en 440 Hz, cada canal de su lado), 100 % de NTSC, 0 faltas.
  **Zelda OoT suena perfecto** (el autor): 9 min al 99,9 %, HLE en el SPE
  2,4 ms por tarea (mediana; máx. 4,0), PPE esperando 0, 0 bloques tarde.
  Mediciones en docs/perf-log.md.

Notas:
- Una vez, tras la primera corrida de Zelda (120 s), la consola quedó en
  negro después de «salida normal» y hubo que reiniciarla. No se repitió
  en tres salidas más (7 s, 9 min y SM64 100 s); ahora el cierre del audio
  anota cada paso y todos devuelven 0. Además, soltar el audio
  (`salsa64_rsp_set_audio(NULL)`) ya no termina una tarea gráfica en vuelo:
  antes hacía un reset completo del RSP y podía llamar a `wake()` del
  frontend con su semáforo ya destruido. Si vuelve a pasar, mirar esas
  líneas; `salsa64_rsp_set_gfx(NULL)` en main.c tenía el mismo riesgo
  (iba después de `sysSemDestroy(wake_sem)`): corregido al integrar, el
  semáforo se destruye después de soltar la HLE gráfica. Probablemente no
  era la causa del negro: en lv2 un semáforo destruido da error, no se
  cuelga, y el negro llegó después de «salida normal» (fin del proceso:
  ¿algún hilo, SPE o el puerto de audio aún vivo?). Pendiente si se repite.
- `.gitignore`: la regla «core» de los volcados de qemu también ignoraba
  `src/core` para archivos nuevos; se agregó «!src/core/».

Falta:
- [ ] Oír Super Mario 64 (ABI1): sus tareas corren sin errores (0,115 ms)
      pero el juego no pasa del arranque sin F3D.
- [x] naudio (Perfect Dark, Banjo, Conker; Perfect Dark con MP3): ver la entrada
      de naudio. Falta MusyX.
- [ ] Probar la política «silencio» y una escena lenta en la PS3; con el
      recompilador Zelda ya va al 100 % y casi no hace falta estirar.
- [ ] Opcional: SIMD en ENVMIXER, RESAMPLE y ADPCM del SPE (hoy 2,4 ms por
      tarea en Zelda, escalar).
- [ ] Opcional: que el audio marque el ritmo (pacer.c tiene el enganche).
## 2026-10-10 — Bucles de espera en código mapeado por la TLB

Hecho (rama `loving-spence-8951ae`, desde main bd8ac59):
- `src/cpu/idle.c` traduce el código y las cargas de la vuelta también por
  la TLB, sin efectos (`salsa64_vr4300_fetch_probe`: la búsqueda del
  intérprete sin excepciones ni cambios); antes solo KSEG0/KSEG1. El «sí»
  no se guarda (cada vez se vuelve a simular con la TLB y la memoria del
  momento) y la TLB no cambia dentro de una vuelta (COP0 la descarta): un
  remapeo o código reescrito se ve en el análisis siguiente; los descartes
  siguen por pc ^ instrucción. El recompilador no cambia (consulta lo
  mismo que el intérprete).
- Pruebas: `b .` mapeado y sondeo de una variable por dirección mapeada
  (se saltan, igual con y sin detección), y la misma dirección virtual
  remapeada a un contador (deja de saltar y queda descartado).
- Mac, 21,3 s de consola con el intérprete: GoldenEye 0,0 → **97,6 %** en
  bucles de espera (16,2 → 0,8 s); Perfect Dark 0 → 99,3 % (su `b .` del
  arranque). Registros, log e imagen del VI idénticos con y sin el cambio.
- qemu-ppc64: 233/233 con el recompilador en los cuatro modos, nemu64-test
  con la línea base, vitest OK, `--lockstep` sin diferencias en
  nemu64-test, vitest y en GoldenEye y Perfect Dark (21,3 s de consola cada
  uno, con 97,6 % y 99,3 % en bucles de espera).

- **PS3 real** (524d80e, frontend «Salsa64 TLB bucles», SLSA00074):
  GoldenEye a 59,8 campos/s con **0,6 ms de emulación por campo** (antes
  5,6; 0,5 de los 0,6 son la copia del VI) y 99 % en bucles de espera
  (antes 0 %); 1,2 MIPS ejecutados en vez de 93,7.

## 2026-10-10 — Recompilador: conversiones de la FPU, LWL/LWR y MFC0 en nativo

Hecho (rama `funny-lalande-371c7e`; diseño en docs/rec-ppe.md,
cifras en docs/perf-log.md):
- En nativo, con los mismos bits, causas y banderas de FCR31 que el
  intérprete: SQRT, CVT.D.S/CVT.S.D, ROUND/TRUNC/CEIL/FLOOR a W y L,
  CVT.W/CVT.L (redondeo al más cercano), CVT.S.W/CVT.D.W (en enteros, sin
  pasar valores entre registros enteros y de punto flotante), CFC1/CTC1.
  Lo que no da exacto (redondeo dirigido, I habilitada, operandos fuera de
  la franja, NaN, desnormales) sigue en el intérprete.
- LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR en nativo; los pares LWL+LWR y SWL+SWR
  sobre los mismos 4 bytes, byte a byte (sin desplazamientos por registro ni
  load-hit-store en copias).
- MFC0/DMFC0 ya no terminan el bloque; Count desde r29 + k + 1.
- Código nuevo en `src/cpu/rec_emit_ext.h` (lo incluye rec_emit.c); en lo
  común, ganchos de una línea, para no chocar con el trabajo de TLB.
- `rec->stats.exec_calls` (llamadas al intérprete desde el código) en
  salsa64-run y cpu-tests; `SALSA64_REC_OPT_RARE_EXEC` repite lo de antes
  con el mismo binario (cpu-tests la corre como variante).
- Pruebas: `executed_conversions_match_full` (8000 instrucciones contra el
  camino completo, más de 3000 en nativo), LWL/LWR y compañía en cada
  desplazamiento contra un modelo byte a byte, pares en los dos órdenes,
  registros fuera del PPE, caminos lentos (DMEM, fallo de TLB), código que
  se reescribe; MFC0 de cada registro y Count dentro del bloque y en un
  delay slot. 221/221 en el host y con el recompilador (plain, mtctr,
  registros fijos, escritura temprana); nemu64-test con la línea base,
  vitest OK, `--lockstep` sin diferencias en los dos. Prueba de mutación:
  10 de 10 errores puestos a propósito detectados.
- Host: llamadas al intérprete −18 % en nemu64-test y −97 % en vitest.
- Con main traído (TLB y KSEG1, bd8ac59): 240/240 en arm64 y en ppc64 en
  los cuatro modos, nemu64-test con la línea base (623 bloques fuera de
  KSEG0), vitest OK, `--lockstep` sin diferencias en los dos.

Siguiente:
- [x] **PS3 real** (cpu-tests SLSA00077 «Salsa64 CPU FPU», e210aeb):
      nemu64-test 1894 → 1816 ms (−4,1 %, ×4,47 del intérprete) con las
      mismas fallas; bench cvt 4,2 → 40,7 MIPS, no alineadas 20,5 → 218,5,
      mfc0 9,8 → 701, espera por Count 8,3 → 488 (salidas a C 2 M → 8).
      Las 4 «fallas» del RESULTADO eran verificaciones de cobertura en la
      variante `RARE_EXEC` (corregidas).
- [ ] Lo que sigue yendo al intérprete: CVT.S/D.L, CACHE, MTC0/TLB/ERET;
      MFC0 de Random.
- [x] Bench del intérprete (21 → 14,7 MIPS tras la Fase 2): en el binario
      de este trabajo da 21,1 MIPS en `t_n64` y en la máquina alineada; no se
      reproduce y la hipótesis de la página de `sched->next` quedó
      descartada (docs/perf-log.md). Depende de cómo queda el binario.
- [ ] Medir en un juego (Zelda, SM64) cuánto pesan las conversiones y las
      esperas por Count cuando la HLE gráfica deje de ser el cuello.

## 2026-10-10 — Fin de tarea del RSP por sondeo (sin bloquear al SPE)

Hecho (rama `musing-mcclintock-6b5e2c`, commits a2ffd0f + mezcla
16a544f con main 4a4d98c; diseño en docs/hilos-ppe.md «Fin de tarea sin
bloquear»):
- `src/core/rsp.c`: el backend tiene `done(ctx, idle)` (no bloquea); al
  llegar el fin de la tarea, si no terminó, el evento se vuelve a programar
  cada `poll_cycles` (10 000 ciclos) y el CPU sigue emulando. Si el CPU
  saltó ciclos (bucle de espera) desde el sondeo anterior, se le avisa al
  backend y el del SPE duerme 100 µs antes de contestar: el reloj emulado
  no corre más rápido que el real. Con la tarea en curso el RSP no está
  HALT y otro arranque se ignora (`starts_busy`). Genérico para el audio.
- Prueba unitaria `rsp_gfx_task_polls`; estadísticas `sondeos` y
  `dormida` en la línea `gfx:` del frontend.
- ROM gfxtest: la espera por una interrupción vence por tiempo (10 s de
  Count) y no por vueltas (en RPCS3 el primer trabajo del SPE compila
  segundos; antes el CPU estaba congelado mientras tanto). Reconstruida.
- Verificación: 224 pruebas en el host, cpu-tests/vitest/save-tests en
  RPCS3, gfxtest 22/22, test-roms 446, build sin advertencias.

Medido en la PS3 (docs/perf-log.md, «Fin de tarea del RSP por sondeo»):
Zelda, «esperando al SPE» 2,3–8,2 ms por tarea → 0; emulación por campo
igual (3,2–7,5 → 3,2–7,2 ms) porque el CPU está ocioso mientras el RSP
trabaja y el sondeo duerme ese tiempo; 60 campos/s sin regresiones;
gfxtest OK. La ganancia llegará donde el CPU tenga trabajo durante el
RDP.

Pendiente:
- [ ] Repetir con Super Mario 64 (F3D) cuando esté en main: 9,5 ms de CPU
      por campo, el caso donde el sondeo debería rendir.
- [ ] Recompilador: un bucle que lee Count (MFC0) sale a C en cada vuelta
      (gfxtest cayó a 9 campos/s); compilar MFC0 Count en línea (trabajo FPU, MFC0 nativo).

## 2026-10-10 — Recompilador: código en KSEG1 y mapeado por la TLB

Hecho (diseño en docs/rec-ppe.md, «Código fuera de KSEG0»):
- El recompilador compila también código en KSEG1 y mapeado por la TLB
  (kuseg, kseg2/3), con la instrucción en la RDRAM o en la ROM del
  cartucho. Antes eso lo corría el intérprete; GoldenEye y Perfect Dark
  corren casi todo su código mapeado.
- Clave de bloque (vpc, física) en una tabla aparte (`xhash`); KSEG0 sigue
  igual. Todo en `src/cpu/rec_tlb.c` (para que los merges con otros
  cambios del recompilador sean fáciles); rec.c y rec_emit.c solo lo llaman. Los bloques mapeados comparan su traducción al entrar contra una
  TLB del recompilador por página de 4 KB (`vtlb[]`, 4 instrucciones),
  también al llegar por un enlace o por JR. Escribir la TLB no mata ni
  desenlaza bloques: antes de correr código, el despachador vuelve a
  traducir las páginas anotadas si cambiaron la TLB, el ASID o ERL.
- JR/JALR encuentran en el propio código los bloques fuera de KSEG0.
- `cop0.c`: `salsa64_vr4300_fetch_probe` (búsqueda en la TLB sin
  excepciones) y `cpu->tlb_writes`.
- Pruebas nuevas (también en modo estricto): código mapeado con JAL entre
  páginas, remapeo con enlace, cambio de ASID, código mapeado reescrito por
  la dirección mapeada y por KSEG0, KSEG1, ROM por TLB y por KSEG1, dos
  páginas en la misma entrada de `vtlb[]`, bucle mapeado; bucles de espera
  en KSEG1 (se saltan) y mapeados (se descartan, como con el intérprete).
- qemu-ppc64: unitarias 222/222 con el recompilador en los cuatro modos;
  nemu64-test con las fallas de la línea base y 623 bloques fuera de KSEG0;
  vitest OK; `--lockstep` sin diferencias en nemu64-test y vitest.
- Cambiar la ROM o el ISViewer vacía el recompilador (puede haber código
  compilado desde la ROM).
- Memoria: +572 KB del estado del recompilador (6,84 MB).

Siguiente:
- **PS3 real** (b11861c, cpu-tests «Salsa64 CPU TLB», SLSA00076): 232/232
  en el intérprete y en las cuatro variantes; nemu64-test igual al
  intérprete (446 fallas, hash y ciclo), ×4,04–4,29; bench enteros con
  registros fijos 760 MIPS (el préstamo en bucles funciona; antes 128).
- qemu-ppc64 con `--lockstep` (respaldos propios, fuera del repo):
  GoldenEye y Perfect Dark 200 M ciclos cada uno sin diferencias con el
  intérprete. GoldenEye: 3518 de 3524 bloques mapeados y 5050
  traducciones en 2,1 s (reescribe la TLB seguido); Perfect Dark con 8 MB:
  3 traducciones.
- **PS3 real, frontend «Salsa64 TLB»** (SLSA00074, respaldos propios):
  - GoldenEye: 59,8 campos/s a 93,7 MIPS (velocidad completa del VR4300),
    **5,6 ms de emulación por campo**; 3512 de 3518 bloques mapeados, 5050
    traducciones en el arranque y ninguna después, ~0,08 % interpretado;
    estable 100 s. En pantalla, ruido: el juego manda una sola tarea
    gráfica con un microcódigo que la HLE no reconoce (ucode en 0x20e60,
    su F3D) y después solo audio; con el intérprete solo, en la Mac, queda
    igual (mismo bucle en 0x7000071c a los 21 s). Le toca al trabajo de F3D.
  - Perfect Dark: 59,8 campos/s, 5,1 ms por campo, pero se queda en el
    arranque (201 bloques, VI sin configurar, pantalla negra); con el
    intérprete solo también (pc 0x700017f4 a los 21 s, sin tareas del RSP
    ni excepciones). No es del recompilador: falta ver el arranque (CIC
    6105) o algún registro del hardware.
- Diagnóstico (arnés en la Mac, con el intérprete; lockstep de 10,7 s de
  consola sin diferencias en los dos juegos):
  - GoldenEye llega a su hilo ocioso (`b .` en 0x7000071c) y se queda: su
    única tarea gráfica (ucode en 0x20e60) no la reconoce la HLE, y la
    tarea nunca termina para el juego (sin DP), así que no vuelve a
    dibujar; el VI muestra RDRAM sin inicializar (el ruido). Pagina su
    código por fallos de TLB (110 en 10 s). Además el `b .` mapeado no lo
    salta idle.c (solo KSEG0/KSEG1): 0 % en bucles de espera.
  - Perfect Dark (CIC 6105) compara la palabra en 0xA00002E8 con
    0xC86E2000 y, si no coincide, se queda en `b .` (0x700017f0). Ningún
    código del juego la escribe: la deja el arranque del 6105 y nuestro
    arranque HLE no. Escribiéndola a mano (experimento, no en el repo)
    pasa, monta 0x7F000000 en la TLB y llega a su hilo ocioso.
- [x] Arranque HLE del CIC 6105 (`src/core/boot.c`; trabajo «Arranque del CIC
      6105 y censo»): lo que el IPL3 deja
      en la RDRAM (al menos 0x2E8 = 0xC86E2000), desde documentación
      pública, sin código de Nintendo. (Hecho: entrada de arriba.)
- [ ] HLE gráfica: el microcódigo de GoldenEye (F3D; trabajo de F3D) y que
      una tarea con microcódigo desconocido no deje colgado al juego.
- [x] idle.c: bucles de espera en código mapeado por la TLB (entrada de
      arriba).
- [x] GoldenEye ejecuta 0x7F000000 desde la RDRAM: pagina su código por
      fallos de TLB (visto en la emulación, ver abajo).

## 2026-10-10 — Caché de texturas entre tareas (HLE gráfica)

Hecho (rama `unruffled-maxwell-2af921`; diseño en docs/texturas.md):
- Las texturas decodificadas viven en una región de 8 MB mapeada al RSX
  (`src/rdp`) que la HLE del SPE 0 administra entre tareas
  (`src/rsp/gfx/texcache.c`). Identidad: hash de 64 bits (ronda de
  xxHash32 en ocho carriles, SIMD en el SPE) de los bytes de la TMEM que
  lee la decodificación —texels y paleta— y de los parámetros de la tile:
  un cambio en la RDRAM o en la paleta da otra textura sin vigilar
  escrituras (cubre las del CPU, que no avisan). Al arrancar, el frontend
  comprueba que el hash del SPE da lo mismo que el del PPE.
- Anillo con desalojo en orden de llegada; lo que el RSX aún puede leer
  (`job->done_task`, de las barreras de `src/rdp`) se salta. Sin lugar, la
  textura va al arena de la tarea. `salsa64_rdp_dump` copia las texturas
  de la caché al volcado (mismo formato).
- Cargas a la TMEM en bloque (sin `memcpy` de 8 bytes): ×2,4.
- Medición en el frontend: ventanas fijas de 100 campos («gfx campos A–B»,
  con el peor campo), capturas en campos fijos y salida sola
  (`capturas` = «N M»); acomodado sobre los dos hilos de main.
- PS3 real, Zelda (intro y título, con el recompilador; docs/perf-log.md):
  HLE en el SPE 14–31 → 5–9,6 ms por tarea; decodificar 6,7–17,2 → 0–0,09
  ms; las escenas a 41–48 campos/s llegan a 60; capturas idénticas byte a
  byte. Decisión: todo en el SPE 0; los SPE 2–3 quedan para el escalado de
  texturas (docs/texturas.md).
- Pruebas: caché entre tareas, texel y paleta cambiados en la RDRAM, anillo
  con texturas en uso y directorio lleno, hash por tramos; gfxtest con una
  textura y una paleta que el CPU cambia en el cuadro 60 (22 puntos).

Pendiente:
- [ ] LOADTILE pide cada fila con un DMA que espera (~2300 lecturas por
      tarea en Zelda): pedirlas por adelantado. Es lo más caro que queda
      en el SPE junto con el estado de dibujo.
- [ ] Decodificación por formato con SIMD y salidas de 16 bits en
      `lib/rsxkit/`: solo cuando falla la caché; medir antes el peor campo
      en cambios de escena (Kokiri, partida en juego).
- [ ] Achicar el arena de texturas por juego (casi no se usa).

## 2026-10-10 — Dos hilos en el PPE (frontend)

Hecho (rama `musing-mcclintock-6b5e2c`; diseño en docs/hilos-ppe.md):
- `src/frontend/main.c`: hilo de emulación (`sysThreadCreate`, prioridad
  1000, pila de 256 KB) con el CPU, el planificador, el envío de tareas al
  SPE, los controles aplicados al PIF y la copia de la imagen del VI; el
  hilo principal (1001) traduce la salida de la HLE al RSX, presenta con
  flip en vsync, lee los DualShock 3, escribe el guardado, el log de
  estadísticas, capturas y volcados. Duerme en un semáforo (la emulación
  lo despierta por campo y por tarea); nunca espera activa (SMT).
- Colas preasignadas: cola de campos (8 mensajes) con copia del VI en 3
  ranuras (4,4 MB; estados por `lwarx/stwcx.`), cola de tareas gráficas
  de 3 entradas dentro de `src/rdp` (`salsa64_rdp_flush` traduce en el
  hilo de presentación; `acquire` espera un juego libre con
  `rsxkit_fence_done`, la única función de rsxkit documentada como segura
  desde otro hilo), buzón de controles con seqlock, zona de paso del
  guardado (`savefile_stage` / `savefile_write_staged`), lwmutex en el log.
- Si la emulación va más rápida que la tele: se presenta el campo más
  nuevo y los viejos se saltan (`saltados` en el log); la emulación nunca
  espera al vsync. Los campos con verificación del selftest no se saltan.
- Verificación igual que antes: selftest de vitest (RDRAM en emulación,
  pantalla en presentación), gfxtest, capturas, `gfxdump`, repetición
  (`dump:`, de un hilo: llama a `salsa64_rdp_flush`). Pruebas: 212 en el
  host; test-roms (nemu64-test 446 fallas, vitest, controles, savetest,
  gfxtest); fase 1 y fase 2 en RPCS3.
- Lugar para el audio marcado con `audio_out:` en main.c (inicio, cada
  campo presentado, cierre; y `emu_thread` si necesita el hilo de
  emulación).
- Ritmo (`src/frontend/pacer.c`): cada
  campo a la hora de la tele emulada con el reloj real, sin acumular
  deuda; `SALSA64_DATA_DIR/turbo` lo quita para medir el máximo; la
  medición A/B de vitest va sin límite. Enganche para que el audio marque
  el ritmo (`sleep` reemplazable, `pacer_nudge`). 5 pruebas unitarias.

Medido en la PS3 real con el recompilador (main 697922c mezclado; tabla
en docs/perf-log.md, «Dos hilos en el PPE»): Zelda en el título con el
castillo 41–52 → 57–60 campos/s (el flip del vsync ya no frena a la
emulación); vitest sin límite 187 → 363 campos/s con la presentación a
60/s saltando el resto, y con ritmo 60,0 exactos; guardado por hilos y
START + SELECT bien. Las capturas periódicas `screen-N.ppm` pasaron a ser
opcionales (`SALSA64_DATA_DIR/capturas`): cada una frena ~400 ms y era el
«tirón» cada 10 s; en prueba automática siguen siempre.

Pendiente:
- [ ] La espera del hilo de emulación al SPE al terminar cada tarea (10–27
      ms por tarea, 20–53 % del hilo en Zelda) es el cuello siguiente:
      que el evento del RSP se vuelva a programar en vez de bloquear
      (`src/core/rsp.c`), o acortar la HLE (caché de texturas).
- [ ] Copia del VI (0,2–0,3 ms por campo) aunque el VI muestre un destino
      del RSX: evitarla con una pista del hilo de presentación.
- [ ] Repetir la medición con Super Mario 64 cuando F3D esté en main (caso
      de libro: 9,5 ms de emulación + 7–8 ms de presentación en un hilo).
- [ ] Menú y audio en el hilo de presentación (trabajos aparte).

## 2026-10-10 — Fase 3: recompilador MIPS → PPC64 en el PPE (×4,4 con código real)

Hecho (diseño y decisión en docs/rec-ppe.md, números en docs/perf-log.md):
- **Decisión: recompilador propio.** Not64 y Wii64 (GPLv2+, commits en
  docs/THIRD_PARTY.md) se revisaron y descartaron como base: emisor de 32
  bits con pares hi/lo, EABI de la Wii, globales de mupen64, Count solo en
  saltos y FPU sin FCR31. Se usaron solo como referencia de diseño.
- `src/cpu/rec.c`, `rec_emit.c`, `rec.h`, `ppc.h`: bloques desde KSEG0
  hasta un salto con su delay slot; en nativo solo si caben antes del
  próximo evento, así que eventos, Count, interrupciones y excepciones caen
  en el mismo ciclo que con el intérprete. 12 registros MIPS por bloque,
  escritura temprana (contra el load-hit-store entre bloques), enlaces
  parcheados, JR/JALR sin volver a C, invalidación por código escrito
  (también desde el intérprete, DMA del PI/SI/SRAM y cargador), FPU nativa
  con las mismas causas de FCR31 que el intérprete, bucles de espera con la
  misma consulta que `idle.c`. Lo raro (LWL/LWR, COP0, conversiones de la
  FPU, TLB, KSEG1, modo usuario) lo corre el intérprete.
- Caché en la cola NOBITS de 16 MB del segmento de texto
  (`mk/rec-cache.ld`): llamadas a C con `bl` directo. Estado: 6,3 MB.
- Pruebas en la Mac sin PS3: Linux ppc64 + qemu (`tools/test-rec.sh`,
  `docker/ppc64.Dockerfile`): las 197 unitarias también con el recompilador
  en modo estricto; nemu64-test con la línea base; vitest OK; `salsa64-run
  --lockstep` compara contra el intérprete bloque a bloque (sin
  diferencias en nemu64-test).
- **PS3 real** (f7b655d): 163/163 unitarias ejecutando código generado;
  nemu64-test 7,9 s → 1,8 s (**×4,36, 104 MIPS**) con las mismas fallas y
  el mismo ciclo final; bench mixto 21,3 → 156,6 MIPS.
- RPCS3 no ejecuta código generado: ahí se apaga
  (`/dev_hdd0/salsa64/sin-recompilador`, lo crea `tools/rpcs3-run.py`).
- Merges de main (controles, bucles de espera, guardado, Fase 2 gráfica)
  sin regresiones; el DMA del SP, del SI y de la SRAM avisan al
  recompilador.
- Registros fijos (sp, ra, v0, v1, a0–a3) activos en el frontend: +7 % en
  nemu64-test (×4,45 del intérprete); los bucles toman prestados los que no
  usan. La escritura temprana no compensó: opción apagada.
- **Zelda OoT en la PS3 real con el recompilador: 41–60 campos/s** (con el
  intérprete 15–17), 4–14 ms de emulación por campo; se ve igual. Donde no
  llega a 60, el PPE espera a la HLE gráfica del SPE.
- vitest en el frontend: 60 campos/s, 2,9–5,4 ms de emulación por campo.
- Paquetes de prueba con APPID propio: SLSA00063 «Salsa64 recompilador»
  (frontend) y SLSA00065 «Salsa64 CPU recompilador» (cpu-tests con cuatro
  variantes del recompilador y nemu64-test contra el intérprete).

Siguiente:
- [ ] Medir en la PS3 el préstamo de registros fijos en bucles (bench
      enteros con registros fijos: esperado ~750 MIPS, antes 128).
- [x] Más juegos de la tabla de docs/proyecto.md (SM64, MK64, SF64; GoldenEye y
      Perfect Dark usan TLB: ver la entrada de arriba). (Censo en
      docs/compatibilidad.md.)
- [x] Más en nativo: CVT/TRUNC/ROUND de la FPU, LWL/LWR/SWL/SWR, MFC0 sin
      salir del bloque (entrada de arriba). Código por TLB y KSEG1: hecho.
- [ ] CACHE.
- [ ] Fase 2: el DMA del SP hacia la RDRAM tiene que llamar a
      `salsa64_bus_rdram_written`.
## 2026-10-10 — Fase 2 (parte gráfica): criterio cumplido en PS3 real

Hecho (rama `reverent-leavitt-d0f943`, integrada en main después del
guardado y los controles):
- `lib/spukit` (MIT): trabajos por lotes en SPEs, cola en memoria principal
  y timbre por mailbox (1,76 µs ida y vuelta en la PS3).
- HLE de F3DEX2/F3DZEX2 portable (`src/rsp/gfx/`) que corre en el SPE 0
  (`src/spu/gfx/`, E/S por DMA con doble búfer) y en el host; RDP sobre
  `lib/rsxkit` (`src/rdp/`): combiners como combinación lineal, texturas,
  z-buffer, culling, niebla, mezcla según la fórmula del blender.
- Criterio: gfxtest (homebrew propio con el IPL3 libre) 19 de 19 puntos en
  host, RPCS3 y PS3; Zelda OoT (respaldo propio) llega al título y la
  intro se ve sin errores gráficos en la PS3 real (confirmado por el autor).
- Arreglos de la ronda con la PS3: TASKDONE en tareas de libultra sin HLE,
  destinos alineados a 128 (temblor), frente antihorario, mezcla opaca del
  cielo, corrección de perspectiva que `cgcomp` quita en MUL/DP (la luna;
  docs/rsx-notes.md) y FILLRECT inclusivo en 1 ciclo (columna del fundido).
  Todos verificados por el autor en la PS3 real con `salsa64-fase2.pkg`
  (APPID propio SLSA00062, título «Salsa64 Fase 2 (gfx)»).
- Depuración: volcado de la salida de una tarea (`gfxdump`), repetición en
  el RSX sin emular (`embedded` = `dump:<nombre>`), `tools/gfxdump.py` para
  editar volcados y `salsa64-run --raster-dump`.
- Mediciones en docs/perf-log.md: el cuello es el CPU (64 ms por campo, 26 %
  de NTSC); la HLE en el SPE cuesta 1–21 ms por tarea, sobre todo
  decodificando texturas.

Pendiente:
- [ ] Caché de texturas entre tareas y decodificación por formato en el SPE
      (medir antes/después en la PS3).
- [x] F3D (Super Mario 64): ver la entrada «F3D (Super Mario 64) y F3DEX 1.x».
- [ ] Casos de Cell64 (espada Kokiri sin textura, fuente de la pantalla de
      nombre).
- [ ] Al integrar con la Fase 3: `sp_dma` hacia la RDRAM debe llamar
      `salsa64_bus_rdram_written`.
- [ ] «NO CONTROLLER» en Zelda: el control por SI/PIF ya está integrado;
      comprobar en la PS3 que Zelda lo ve.
- [ ] Comprobar en la PS3 Zelda con la HLE en el SPE y la detección de
      bucles de espera juntas: lo que el RSP escriba o avise fuera del
      CPU debe ir en un evento del planificador (nota de los controles).

## 2026-10-10 — Guardado del cartucho: EEPROM y SRAM

Hecho:
- **EEPROM de 4 y 16 Kbit** en el canal 4 del PIF (`src/core/save.c`,
  desde n64brew.dev: Joybus Protocol): información (0x00/0xFF →
  0x0080/0x00C0), lectura (0x04) y escritura (0x05) de bloques de 8 bytes;
  en la de 4 Kbit los bloques 64–255 repiten 0–63. Escritura instantánea
  (en la consola, hasta 6 ms), nunca "ocupada".
- **SRAM de 256 Kbit** en el dominio 2 del PI (0x08000000, espejo cada
  32 KB): DMA en los dos sentidos y accesos directos de 32 bits del CPU.
- **Tipo por juego** (`salsa64_save_detect`): `guardado.cfg` (manual; por
  código de 3 o 4 caracteres, CRC1 o `*`) > cabecera de homebrew ("ED" en
  0x3C, tipo en 0x3F; la de n64tool de libdragon) > tabla por código
  (`src/core/savedb.c`: 178 códigos y 6 excepciones por región, solo donde
  coinciden ares y N64FlashcartMenu) > ninguno, con aviso en el log. Los
  seis juegos objetivo están en la tabla (SM64, MK64, SF64 y GoldenEye
  EEPROM 4K; Perfect Dark 16K; Zelda OoT SRAM). FlashRAM, SRAM de 768 Kbit
  y de 1 Mbit se reconocen pero no se emulan todavía.
- **Persistencia** (`src/frontend/savefile.c`, igual en la PS3 y en el
  host): un archivo por juego y tipo en `/dev_hdd0/salsa64/saves/`
  (`<título>-<CRC1>.eep|sra`, los nombres de Project64). Búfer preasignado
  de 32 KB; se cuentan las escrituras del juego que cambian datos y se
  vuelca tras 2 s sin escrituras (a lo más 30 s si no paran) y al salir.
  Escritura segura: `.tmp`, borrar y renombrar.
- Ganchos mínimos en `pif.c`, `pi.c` y `bus.c` (24 líneas); `bus->save`
  sobrevive al reset.
- Pruebas: suite `save` (14 pruebas; 196 en total) en el host y en RPCS3
  (big-endian). ROM propia `savetest` (`tests/roms/src/savetest`, tres
  variantes por la cabecera, `tools/build-savetest.sh`): escribe un patrón,
  lo relee y en el arranque siguiente verifica que persistió. En
  `tools/test-roms.sh` (dos arranques por tipo con `salsa64-run
  --save-dir`) y en RPCS3 con `salsa64-save-tests` (SLSA00020, las tres
  ROMs sin pantalla; `tools/test-phase1.sh` lo abre dos veces): pasan,
  con el volcado diferido a los 2,0 s. Prueba de mutación: romper la
  repetición de bloques, el identificador, la escritura directa o el DMA
  hacia la SRAM lo atrapan las pruebas y la ROM.
- Sin regresiones: nemu64-test 462 fallas, vitest y controles OK; compilan
  fase 0, fase 1 y el frontend sin advertencias.

- **PS3 real** (Slim CECH-3011A, HFW 4.91 + HEN): `salsa64-save-tests`
  abierto dos veces pasa en los tres tipos ("persistió el arranque 1");
  volcado diferido a los 2,0 s y, en la EEPROM de 16 Kbit, escritura al
  cerrar con cambios pendientes 26 ms después del resultado, que también
  persistió. El `rename` del `.tmp` funciona en el disco de la PS3. El
  frontend real (variante «Salsa64 guardado», SLSA00021, para no pisar la
  app de la Fase 2) con `savetest-sram` como `rom.z64`: azul con un cuadro
  y, al volver a abrir, verde con dos; START + SELECT llega a
  `savefile_close` y a "salida normal".
- `tools/rpcs3-run.py`: en el primer lanzamiento de un ELF nuevo a veces no
  alcanza a leer el TTY.log (RPCS3 compila sus módulos); si el programa
  termina sin RESULTADO leído, lo toma del "Accumulated RPCS3 TTY" del
  final de RPCS3.log.

Notas:
- En la PS3 quedaron `saves/SALSA64_SAVE_*` (de las ROMs de prueba) y los
  paquetes `salsa64-save-tests.pkg` y `salsa64-guardado.pkg`; el
  `rom.z64` de prueba se borró (el `oot.z64` del autor sigue ahí).
- En la primera salida del frontend en la PS3 el log no tuvo "salida
  normal" (no se sabe cómo se salió); en la siguiente, con START + SELECT,
  sí. Si vuelve a faltar, revisar `rsxkit_shutdown`: el guardado se vuelca
  antes, así que no se pierde.

Falta:
- [ ] Probarlo con Zelda (SRAM) y un juego con EEPROM cuando la Fase 2
      permita llegar a guardar (respaldos propios, fuera del repo).
- [x] FlashRAM (2026-10-11, entrada de arriba); faltan la SRAM de 768 Kbit
      y la de 1 Mbit.
- [x] `src/core/rcp.h`: el comentario del SI ya menciona la EEPROM (al
      integrar con la Fase 2) y el del PI, la SRAM.

## 2026-10-10 — Controles (PIF + DualShock 3) y bucles de espera

Hecho (validado en PS3 real, Slim CECH-3011A, HFW 4.91 + HEN):
- **Controles** (ceae30f): comandos joybus del PIF por el SI
  (`src/core/pif.c`, desde n64brew.dev): identificación, estado, accesorio
  sin pak, RST con L+R+Start, errores de canal. Frontend
  (`src/frontend/padmap.c`): stick izquierdo → stick (±80), stick derecho
  → botones C, × → A, □ → B, L2 → Z, L1 → L, R1 → R, START; todo
  configurable en `/dev_hdd0/salsa64/controles.cfg`. El puerto 1 siempre
  tiene control (el DS3 tarda ~0,2 s en aparecer y los juegos buscan
  controles al arrancar). vitest identifica los puertos y lee el control
  1 por el SI: en la PS3 llegaron a la ROM todos los botones, los sticks
  en su sentido (arriba = Y +80), los C del stick derecho y RST.
- **Bucles de espera** (940f6f8, `src/cpu/idle.c`): punto fijo exacto de
  una vuelta y salto en vueltas enteras hasta el próximo evento. Estado
  idéntico con y sin detección (pruebas, vitest, nemu64-test). PS3 real
  con vitest: emulación 65,6 → 35,0 ms por campo (×1,9), 15,2 → 28,3
  campos/s, 45,6 % del tiempo saltado. Costo en el bench: −1 % (21,2 →
  21,0 MIPS). docs/perf-log.md.
- Intérprete optimizado en el frontend: 23,8 MIPS con vitest (antes 13,6).
- 182 pruebas en el host y en RPCS3 (big-endian) y en la PS3; nemu64-test
  sin cambios (462).

Notas:
- ppu-gcc no pone indicaciones estáticas en los saltos (bits a/t en 0): los
  `+`/`-` que muestra objdump no son predicciones del PPE.
- Quien escriba la RDRAM fuera del CPU (RSP en SPE, DMA con latencia) debe
  hacerlo en un evento del planificador, o el salto de bucles de espera se
  lo pasaría de largo (docs/cpu-vr4300.md, «Bucles de espera»).
- `salsa64-cpu-tests-idle.pkg` (título «Salsa64 CPU bucles de espera») se
  instala sobre la misma app SLSA00005 que los demás cpu-tests.

Siguiente:
- [x] EEPROM del cartucho (canal 4 del PIF) y SRAM: entrada de arriba.
- [ ] Controller Pak y Rumble.
- [ ] Bucles de espera: sondeos de E/S que solo cambian en eventos
      (MI_INTR, SP_STATUS) y esperas por Count. (Código mapeado por TLB:
      hecho, 2026-10-10.)
- [ ] Opcional: decidir el chequeo con el inmediato del salto (no depende
      del destino) y medir si recupera el 1 %.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE (ya
      importa: con vitest la emulación baja a 35 ms por campo).

## 2026-10-10 — Resumen: Fase 0 y Fase 1 en PS3 real

Hecho hoy en hardware real (Slim CECH-3011A, HFW 4.91 + HEN; sin CFW posible):
- Fase 0 completa: udplog, spu-compute (6 SPUs), rsx-triangle, cpu-tests y
  jit-ppe pasan. JIT viable solo en `.text`; la caché del recompilador va en
  una cola NOBITS de 16 MB del segmento de texto (docs/jit-ppe.md).
- Fase 1 cumplida: `salsa64.pkg` muestra la ROM `vitest` (entrada de abajo).
- Intérprete en el PPE: 8,7 → 21,2 MIPS; no llegará a ~60 sin recompilador.
- Referencia Cell64 en la misma consola: Zelda en Kokiri 45–50 VI/s, ~15 cps,
  y cuatro fallas gráficas como casos de prueba (docs/referencia-cell64.md).
- Proyecto renombrado a **Salsa64**; repo en github.com/ricowp81/Salsa64.
- Herramientas: log también en disco (`/dev_hdd0/salsa64/<prog>.log`),
  `tools/ps3-live.py` (monitor en vivo), `tools/ps3.conf` con la IP.

Siguiente (una prueba en la PS3 a la vez):
- [ ] Fase 2: RSP gráfico (F3DEX2) en SPE 0 + RDP mínimo en el RSX sobre
      `lib/rsxkit`, con la infraestructura de trabajos SPE.
- [ ] Fase 3: recompilador MIPS → PPC64 en el PPE.
- [x] Controles (SI/PIF + DualShock 3) y detección de bucles de espera
      (entrada de arriba).
- [ ] Después de la Fase 2: audio HLE en SPE 1 sobre la misma infraestructura.

## 2026-10-10 — Fase 1: un homebrew muestra imagen (criterio cumplido)

Hecho:
- VI (`src/core/vi.c`): temporización por campos con dos eventos del
  planificador; duración desde VI_V_SYNC, VI_H_SYNC y el reloj de cada tipo
  de TV (NTSC 1 567 041 ciclos, 59,83 Hz; PAL 49,92 Hz); VI_V_CURRENT
  calculado al leerlo, interrupción en VI_V_INTR, entrelazado con campos de
  262/263 líneas. `salsa64_vi_frame` describe la imagen visible (origen,
  tamaño por VI_H_VIDEO/VI_V_VIDEO y escalas, RGBA5551/RGBA8888);
  `salsa64_n64_run_field` corre hasta el fin del campo.
- `salsa64-run --frame` guarda la imagen del VI en PPM. La pantalla de
  resultados de nemu64-test se ve completa.
- ROM `vitest` (`tests/roms/src/vitest`, propia, GPLv2+): clang 14 para MIPS
  + nust64 (IPL3 libre r7) en contenedor reproducible
  (`tools/build-vitest.sh`). Barras, rampa y cuadro animado alternando
  RGBA5551 y RGBA8888 con doble búfer en la interrupción del VI; se
  verifica sola (Count por campo exacto, V_CURRENT en el manejador).
- `lib/rsxkit` (MIT, `lib/rsxkit/README.md`): init del RSX y video, cuadro
  con doble búfer y flip en vsync, framebuffer 2D escalado (fit/entero/
  estirar, lineal/cercano) con 5 formatos leídos directo por el RSX
  (RGBA5551 = textura R5G5B5A1). Compila aparte, sin -Isrc.
- Frontend (`src/frontend`, `salsa64.pkg`, SLSA00064): ROM de
  `/dev_hdd0/salsa64/rom.z64` o vitest embebida; un campo de emulación y
  una presentación por vuelta; con vitest se verifica solo (RDRAM y pantalla
  en ambos formatos + resultado de la ROM). Sale con START + SELECT.
- Pruebas: 161 en el host y en RPCS3; nemu64-test sin cambios (462);
  `tools/test-roms.sh` corre vitest y `tools/test-phase1.sh` el frontend.
- **PS3 real** (CECH-3011A, HEN): `RESULTADO frontend: OK` en el campo 124;
  8,7 campos/s (14 % de NTSC), 13,6 MIPS, presentación 0,3 ms por campo
  (docs/perf-log.md).

Notas:
- El log UDP no llegó a `tools/udplog.py`: había otro receptor abierto en el
  mismo puerto (`jit-ppe-cache`, desde las 09:27) que se quedó con los
  paquetes unicast. El log en disco (`/dev_hdd0/salsa64/salsa64.log`, por
  FTP) sí estaba completo.
- Los 13,6 MIPS se midieron antes del perfilado. La sospecha del FPSCR no
  aplicaba; el intérprete optimizado da 21,2 MIPS en el bench (entrada
  siguiente).

Siguiente (Fase 1):
- [x] Volver a medir el frontend con vitest con el intérprete optimizado:
      15,2 campos/s, 23,8 MIPS (sin detección de bucles de espera).
- [x] Detección de bucles de espera: ×1,9 con vitest (entrada de arriba).
- [x] SI: comandos del PIF (controles) y mapeo del DualShock 3.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE.
- [ ] Origen del VI: verificar contra ares si libultra deja VI_ORIGIN una
      línea después del inicio del framebuffer (aquí se toma como la
      primera línea visible).
- [ ] Bus PI, cachés, CPUTest de krom y camino plano: igual que abajo.

## 2026-10-10 — Intérprete en el PPE: 8,7 → 21,2 MIPS

Hecho (números y diagnóstico en docs/perf-log.md):
- Benchmark en variantes (mixto, nop, enteros, memoria, mov.s, add.s) con
  MIPS y ns por instrucción.
- La FPU nunca cambiaba el FPSCR: la sospecha no aplicaba. Lo caro era el
  bucle (~250 ciclos por instrucción aun para un NOP: load-hit-store, una
  llamada con marco de pila por instrucción, `lwax` en los `switch`) y, en
  la FPU, pasar valores entre registros enteros y de punto flotante por la
  pila.
- Bucle con pc/next_pc/flag de salto en registros y `cpu->redirect` para
  excepciones/ERET/set_pc; r0 nunca se escribe; NOP sin segundo despacho;
  BC1 en el bucle; `-mno-relative-jumptables` en mk/ps3.mk; camino rápido
  de la FPU (src/cpu/fpu.h) con prueba de equivalencia de 20 000 casos.
- PS3 real: mixto 8,7 → 17,1 MIPS (×2,0); nop ×3,0; add.s ×2,9. 155 pruebas
  en host y RPCS3; nemu64-test sin regresiones.

- Segunda ronda (147d5ac): SPECIAL en la misma tabla de despacho, FPU
  rápida que clasifica con los bits en enteros y una franja de exponentes
  segura (una sola comparación de punto flotante), C.cond en enteros y
  MOV/ABS/NEG/MFC1/MTC1/CFC1/DMFC1/DMTC1 en el bucle. Prueba de
  equivalencia de 30 000 casos. PS3 real: mixto 17,1 → 21,2 MIPS; mov.s
  ×1,8; add.s ×1,8; nop −10 % (cadena de despacho más larga).

Siguiente:
- [ ] Opcional, poco: índice de despacho ya escalado y calculado directo de
      la palabra de la instrucción (recupera parte de los ~9 ciclos que
      perdieron NOP y LW/SW).
- El bucle cuesta ~93 ciclos aun para un NOP: el intérprete no llegará a
  ~60 MIPS en el PPE sin decodificar por adelantado, y la dependencia entre
  instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. La
  meta de velocidad completa es el recompilador (Fase 3).

## 2026-10-10 — Fase 1: arranque HLE, `salsa64-run` y nemu64-test

Hecho (detalle en docs/cpu-vr4300.md):
- Arranque HLE sin PIF ROM ni IPL3 (`src/core/boot.c`, `src/core/rom.c`):
  normaliza .v64/.n64 a .z64, reconoce el IPL3 de Nintendo por CRC-32 y tiene
  dos caminos. ELF (IPL3 libre de libdragon, el de nemu64-test): carga los
  PT_LOAD, deja los boot flags en DMEM (memoria en [0], TV en [9], ELF en
  [12]) y $sp al final de la RDRAM. Plano (IPL3 de Nintendo o compat): copia
  1 MB desde 0x1000 a la dirección del encabezado (ajustada por CIC) y deja
  las variables de libultra en 0x300 y s3–s7. Común: Status 0x34000000,
  tiempos de PI DOM1, RI_SELECT, registros de RDRAM. Fuentes documentadas.
- ISViewer en 0x13FF0000, opcional (tapa los últimos 64 KB de una ROM de
  64 MB).
- `salsa64-run` (`src/host/run.c`, `tools/build.sh host`): corre una ROM N
  ciclos o hasta un texto del ISViewer, detecta bucles sin salida, traza
  excepciones (`--trace-exc`). Sin asignaciones en el bucle.
- nemu64-test compilado en contenedor reproducible
  (`tools/build-nemu64-test.sh`, mismo SHA-256 desde cero), licencias e IPL3
  verificados, en `tests/roms/homebrew/` con sus avisos.
- `tools/test-roms.sh`: corre nemu64-test y compara con la línea base de
  fallas conocidas (`tests/roms/nemu64-test.fallas`); una regresión lo hace
  fallar (probado rompiendo a propósito el enlace de JAL).
- nemu64-test pasó de no arrancar a 4175 de 4637 pruebas (90 %). Las fallas
  del CPU que encontró están arregladas: MULT con rt de 35 bits, LWR parcial,
  enlace de saltos en delay slot, MTC0 de 64 bits, Index.P, TagLo, PageMask y
  EntryLo en la TLB, Random con Wired > 31, funciones de COP0 reservadas
  como NOP, CFC0/CTC0/BC0, CACHE no alineado, latch de COP2, direccionamiento
  de 64 bits (xkuseg/xksseg/xkphys/xkseg), Status.RE; FPU: índices de
  registro con FR = 0, clasificación de NaN (estaba al revés), redondeo
  dirigido e Inexact, desbordamiento, flush con FS, rango de CVT.L,
  codificaciones inexistentes → FPE. Bus: SB/SH/SD fuera de la RDRAM y
  lecturas del bus PI de 16 bits.
- RSP provisional: quitarle HALT lo detiene en el acto con BROKE, para que
  nadie espere para siempre (Fase 2 lo reemplaza).
- Pruebas: 154 unitarias (antes 139) en host (ASan + UBSan) y en RPCS3
  (big-endian): todas pasan. Se actualizaron las 5 que codificaban
  suposiciones que nemu64-test desmintió.
- Rendimiento del intérprete en el host: 237 → 217 MIPS por la FPU exacta
  (docs/perf-log.md). nemu64-test completo: 0,75 s en el host.

Fallas que quedan en nemu64-test (fuera del intérprete): RSP 294, bus PI
del cartucho 81, cachés 46, SP (DMA y registros) 31, RDP y MI repeat 10.

Siguiente (Fase 1):
- [x] Correr `salsa64-cpu-tests` en la PS3 real: 154/154 pasan, **8,8 MIPS**
      (docs/perf-log.md).
- [x] Perfilar el intérprete en el PPE: 8,8 → 21,2 MIPS en dos rondas
      (entrada de arriba; la sospecha del FPSCR no aplicaba).
- [x] VI con contador de líneas e interrupción; framebuffer 2D en el RSX
      (primera pieza de `lib/rsxkit/`): ver la entrada de arriba.
- [ ] Bus PI: latch de escrituras al cartucho y DMA no alineado (81 pruebas
      de nemu64-test); FlashRAM (la SRAM ya está).
- [ ] Cachés del VR4300 (46 pruebas) y ciclos por instrucción (conjuntos
      "timing" y "cycle" de nemu64-test).
- [ ] CPUTest de krom ensamblado desde fuente con bootcode en ceros (traer
      solo los `.asm` por la API: el repo trae el IPL3 de Nintendo). Probaría
      el camino plano del arranque HLE.
- [x] Probar el camino plano con los juegos del usuario (fuera del repo):
      falta reproducir efectos del IPL3 de algunos CIC (6105). (Hecho:
      arranque del 6105 y censo, 2026-10-10.)

## 2026-10-10 — Fase 1 iniciada: intérprete del VR4300

Hecho (diseño en docs/cpu-vr4300.md):
- Intérprete del VR4300 en `src/cpu/`: ALU de 32/64 bits con
  desbordamiento, cargas/almacenamientos (incluidos LWL/LWR/LDL/LDR y sus
  stores), LL/SC, saltos con delay slot y branch-likely, HI/LO, MULT/DIV y
  variantes de 64 bits, traps; COP0 (Count/Compare, Status, Cause, EPC,
  ErrorEPC, excepciones, ERET, TLB de 32 entradas, modos usuario/supervisor);
  COP1 con modo FR, causas/banderas de FCR31, conversiones y comparaciones.
- Bus y mapa físico en `src/core/`: RDRAM de 4/8 MB con camino rápido desde
  KSEG0/KSEG1, SP DMEM/IMEM, ROM, PIF RAM. MI completo (interrupciones →
  IP2); PI y SI con DMA inmediato; VI, AI, SP, DP como registros. Planificador
  de eventos (Compare). Sin asignaciones en el bucle.
- Portable: mismo código para ppu-gcc (big-endian, accesos directos) y gcc
  nativo (arm64, con byte-swap).
- Pruebas: `tools/build.sh test-host` (139 pruebas, ASan + UBSan) y
  `tools/test-phase1.sh` (las mismas en RPCS3, big-endian). Todas pasan en
  ambos. Prueba de mutación: romper SRA, branch-likely o EPC las hace fallar.
- Micro-benchmark del intérprete (bucle mixto con FPU): 237 MIPS en el host
  (Mac Studio, Docker arm64); 35,6 MIPS en RPCS3 (no concluyente).
- Suites elegidas y licencias verificadas (docs/THIRD_PARTY.md):
  nemu64-test (MIT, IPL3 libre) como principal; CPUTest de krom (Unlicense)
  solo ensamblado desde fuente, porque sus ROMs traen el IPL3 de Nintendo.
  Dillonb/n64-tests descartada (sin licencia).

Siguiente (Fase 1): ver la entrada de arriba. Hechos en ella: arranque
HLE, ISViewer, contenedor de nemu64-test, direccionamiento de 64 bits y
bandera Inexact.

## 2026-10-10 — Fase 0: completa en RPCS3, falta la PS3 real

Hecho:
- Imagen `salsa64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
  PPU, 9.5 SPU) + PSL1GHT + cgcomp con Cg Toolkit vía qemu. Los 54 shaders de
  la rama `ps3` del port de SM64 compilan; 18/31 samples de PSL1GHT compilan
  (las fallas son de los samples).
- Programas de la Fase 0, compilados sin advertencias (`tools/build.sh`):
  - `tests/phase0/rsx-triangle`: triángulo + verificación de píxeles.
  - `tests/phase0/spu-compute`: 6 SPUs, DMA de doble búfer, verificación exacta.
  - `tests/phase0/jit-ppe`: 8 mecanismos de memoria, tolera caídas.
  - `tests/phase0/udplog`: 200 líneas numeradas por UDP.
- Logger TTY + UDP (`src/platform/ps3/log.c`) y receptor `tools/udplog.py`.

- RPCS3 0.0.43 + firmware 4.92 en la Mac (docs/rpcs3.md). `tools/test-phase0.sh`
  corre las cuatro pruebas: udplog, spu-compute y rsx-triangle pasan;
  jit-ppe completa su ciclo, pero RPCS3 no ejecuta código generado (no
  concluyente).

Falta para cerrar la Fase 0:
- [x] Correr los 4 programas en RPCS3 y automatizarlo.
- [x] PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; no admite CFW):
      udplog, spu-compute, rsx-triangle y salsa64-cpu-tests pasan; jit-ppe:
      solo `.text` es escribible y ejecutable (docs/jit-ppe.md). Números en
      docs/perf-log.md. El log también se escribe en /dev_hdd0/salsa64/<prog>.log.
- [x] Plan de la Fase 3: recompilador en el PPE con la caché en una región de
      `.text`. Falta medir el tamaño máximo de esa región (docs/jit-ppe.md).
- [ ] Antes: conectar la PS3 (está desconectada; revisar su estado). Datos a
      reunir: IP por cable, versión de firmware, apps de envío instaladas y si
      hay capturadora. Luego correr la Fase 0 y `salsa64-cpu-tests` (primer
      número de MIPS real).
- [ ] Opcional: correr Cell64 en la PS3 y guardar su `perf_trace.csv`
      (docs/referencia-cell64.md).

Nombre: **Salsa64** (decidido el 2026-10-10). Antes: "Retro64" (2026-10-10) y "Cell64" (provisional).
- Ya existe un emulador de N64 para PS3 llamado **Cell64**: v1.52 de
  "ZeBobbel", publicado en Brewology el 2026-09-27
  (store.brewology.com/ahomebrew.php?brewid=344), con una variante "HEN FIX by
  DropSonic DS". Sin código fuente publicado ni descripción técnica; el
  "sitio del desarrollador" es un canal de YouTube con dos videos de prueba.
  Por eso se renombró el proyecto. La carpeta sigue siendo `projects/cell64`.
- Referencia de rendimiento de ese emulador (videos de ZeBobbel, capturadora,
  build "40031911"): el overlay dice "INT" (intérprete; formato del overlay de
  Wii64/Not64). Super Mario 64 en el castillo: 46,9 VI/s (~78 %). Zelda OoT,
  pantalla de título: 26,4 VI/s, 9,1 FPS (~44 %). No se vio modo "DYN".
  En el video de la v1.50 (overlay nuevo, sin indicar INT/DYN): Zelda OoT en
  el menú de archivos a 50,0 VI/s y 50,0 FPS; probablemente versión PAL (50 Hz),
  o sea ~100 % en una escena ligera. En juego (bosque Kokiri): 41,3 VI/s y
  13,8 FPS (relación 3:1, coherente); ~83 % si es PAL, ~69 % si es NTSC.
  Meta para nuestro emulador: superar eso en la misma escena y región.
  Comparar contra esto nuestro intérprete (Fase 1) y el recompilador (Fase 3),
  con los mismos juegos en la misma PS3.

Notas:
- No usar `-Os` en el PPU: ICE de GCC 13.2 (docs/toolchain.md).
- En RPCS3, rsx-triangle necesita "Write Color Buffers" para su verificación.
- Archivos de Salsa64 en la PS3: `/dev_hdd0/salsa64/` (RPCS3 vacía `/dev_hdd0/tmp`).
- Juegos objetivo: tabla en docs/proyecto.md. Comparación con Cell64 en
  docs/referencia-cell64.md (no usa SPEs; todo en un hilo del PPE).
- Ideas sin decidir: versión rápida del intérprete (decodificar una vez,
  como el "FASTINT" de Cell64) y, como investigación lejana, emular la CPU en
  un SPE solo si el PPE no alcanza.
