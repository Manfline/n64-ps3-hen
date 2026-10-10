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
| 2026-10-10 | f416bee | **Zelda OoT (NTSC) en el frontend**, recompilador con registros fijos, HLE gráfica en el SPE | PS3 real (mismo equipo), respaldo del usuario | intérprete: 15,4–17,5 campos/s, emulación 56–64 ms por campo | **41–60 campos/s (69–100 %), emulación 4–14 ms por campo** | Logo e intro a 58–60; título y escenas con muchos triángulos 41–55, ahora limitadas por la espera a la HLE gráfica del SPE (10–30 ms por tarea), no por el CPU. 86–88 % del tiempo del CPU en bucles de espera. ~10 500 bloques, 7,9 MB de caché, 1,7 % de instrucciones interpretadas. Se ve igual que con el intérprete (el autor) |

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

