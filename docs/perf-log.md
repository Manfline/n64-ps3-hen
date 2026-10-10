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
