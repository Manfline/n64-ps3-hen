# Censo gráfico: el microcódigo real contra la HLE (2026-10-18)

`salsa64-run --lle=all --lle-check-gfx` es el equivalente gráfico del censo de
audio ([audio-censo.md](audio-censo.md)): cada tarea gráfica corre en el
microcódigo del propio juego (el LLE de [rsp-lle.md](rsp-lle.md)) y en la HLE
desde el mismo estado, y se compara lo que dejan los dos. Es una **herramienta
de medición**: este documento no corrige ningún microcódigo; lista lo que
mide y por dónde conviene seguir.

**Resultado en una línea.** En 47 juegos (108 260 tareas gráficas, una escena
de ~50 s de consola por juego) 104 924 tienen HLE; en 15 993 (15 %) el
microcódigo real todavía entrega al RDP casi nada y la comparación no dice nada
de la HLE (eran 28 032, el 27 %, antes de arreglar el camino LLE → RDP: ver
«0. El oráculo»; en 12 de los juegos, 12 083 de ellas eran pantallas fijas y no tareas
vacías, punto 4: queda Yoshi's Story); de las 88 931 restantes, **47,5 % son idénticas píxel a píxel,
35,1 % tienen una diferencia menor y 17,4 % son distintas** (7947 de ellas, el
8,9 %, grandes: más del 3 % de la pantalla). Antes: 74 005 válidas, 48,2 % /
23,1 % / 28,7 % (20 % grandes). Con F3DEX2 y la rama Zelda, donde el LLE ya era
fiable, la HLE es idéntica o casi en el 90–100 % de las tareas de Smash, Kirby,
Paper Mario, Resident Evil 2, Pokémon Stadium 2, Mario Party y Majora; los
números malos que quedan son de microcódigos viejos (F3DEX 1.x), de los propios
de Rare y de Conker, y las tareas que el oráculo aún no dibuja.

## Método

`--lle-check-gfx` pone `--lle=all` (todas las tareas al microcódigo real; el RDP
consume sus comandos por DPC_START/DPC_END y `src/rsp/gfx/dpc.c` los interpreta
sobre el mismo estado del RDP y el mismo rasterizador de referencia del host,
`src/host/raster.c`, que usa la HLE) y enciende el rasterizador. Al arrancar
cada tarea gráfica (`src/host/gfxcheck.c`):

1. **Mismo estado.** Los registros del RDP, las tiles y la TMEM de quien
   consume al LLE (`gfx_state_save`) se copian a otro `gfx_state`, y con ellos
   las color images dibujadas hace poco (`fbimg`): la HLE corre la misma lista
   desde ahí, con sus propios búferes de salida y su propia caché de texturas.
   Cada tarea se mide sola: una diferencia no se arrastra a las siguientes.
2. **Mismo rasterizador, mismo punto de partida.** Se guarda el estado del
   rasterizador (`raster_state_save`: tabla de destinos, imágenes, profundidades).
3. **El microcódigo corre la tarea entera en el acto** (ver «Carrera con el
   CPU»); cada tramo del RDP que entrega se dibuja en el rasterizador de siempre.
4. **Al terminar** (el primer acceso del CPU al SP con HALT y TASKDONE; antes se
   entrega lo que el RDP tuviera congelado, `salsa64_dp_drain`) se guarda
   el estado del rasterizador (el del LLE), se vuelve al de antes de la tarea, se
   dibuja la salida de la HLE y se comparan, destino por destino y píxel por
   píxel (0xRRGGBB, 5 bits por canal), las imágenes. El rasterizador vuelve al
   estado del LLE y la RDRAM, que con `raster_fbfx` 2 el dibujo de la HLE habría
   tocado, se restaura.

### Por qué en píxeles

La HLE no emite los comandos del RDP: emite su propia representación (vértices
en píxeles de la N64 y `gfx_draw` con el estado completo). El LLE entrega
comandos crudos que `dpc.c` convierte a la misma representación, pero **no
coinciden uno a uno**: la HLE agrupa triángulos en dibujos (Super Mario 64: 61
dibujos por tarea contra 768 del camino LLE), recorta de otra manera
(triángulos 3 veces más en el LLE), reconoce rectángulos y no emite los
estados que no usa. Comparar listas exigiría alinearlas; pasar las dos por el
mismo rasterizador y mirar los píxeles es robusto a todo eso y es lo que se ve.
La prueba del propio método es `SALSA64_GCHK_SELF=1` (sin `--lle`): la HLE
contra sí misma da 274 de 274 tareas idénticas en Super Mario 64, con la RDRAM
restaurada y las color images en su sitio.

Se compara además, sin entrar en la clasificación:

- el **estado del RDP al final de la tarea** (othermode, combiner, colores,
  tijeras, imágenes y tiles; `gchk_state_diff`): los bits 0 a 7 de othermode H
  (máscara de mezcla y dither, que la HLE no lleva) se cuentan aparte como
  `oh.dither`;
- los **comandos que la HLE no implementa** (`unknown_cmds`, con su opcode: el
  gancho `GFX_UNKNOWN`, solo con `-DGFX_CENSUS`) y los del RDP que el LLE
  entrega;
- los triángulos y rectángulos de cada lado y el histograma de opcodes del RDP
  que entrega el LLE.

### Clasificación

Por tarea, sobre los píxeles que difieren entre la imagen del LLE y la de la HLE
(`src/host/gfxcheck.h` tiene las constantes):

- **idéntica**: ningún píxel distinto;
- **menor**: la mayor diferencia de un canal es ≤ 8 (un paso de 5 bits:
  redondeo del combiner o del sombreado) o los píxeles distintos sin explicar
  son ≤ 0,2 % del área de los destinos comparados;
- **distinta**: el resto; **grande** si los sin explicar pasan del 3 % del área.

Un píxel distinto **se explica** si no es una diferencia de verdad: si su color
(±8 por canal) está en la vecindad de 3 × 3 del otro lado (un borde con la
cobertura corrida un píxel, una textura muestreada medio texel corrida), o si la
media de 5 × 5 de los dos lados difiere en menos de 16 (ruido de dither y
redondeo en zonas con textura). De las distintas, el **tipo** dominante: la HLE
no dibuja píxeles que el LLE sí; la HLE dibuja de más; los dos dibujan lo mismo
con otro color.

### Carrera con el CPU

La HLE lee la RDRAM al arrancar la tarea; el LLE, por rebanadas, ~1536 ciclos
después y con el CPU ya escribiendo (Super Mario 64 mueve la cabeza del título
en su sitio; con la velocidad nominal media tarea dibujaba otra pose). En este
modo el microcódigo corre cada tarea entera en el acto, antes de que el CPU
avance (`gchk_lle_start`, tope de 16 M de instrucciones: una tarea que espera al
CPU sigue por rebanadas). Con `SALSA64_GCHK_ASYNC=1` y `--lle-speed 200` vuelve el
ritmo de rebanadas: NBA Showtime e Hydro Thunder se atoran si el RSP termina en
el acto, y el censo los corre así.

### Qué dice y qué no dice

- **No dice cuál de los dos lados tiene razón.** Una diferencia puede estar en
  la HLE, en el intérprete del RSP, o en `dpc.c` (que reinterpreta los
  triángulos con sus atributos en las esquinas). Donde se miró a ojo, el LLE
  falló más veces que la HLE; se separa con la columna «LLE vacía» y el tipo
  («la HLE no dibuja» apunta a la HLE; «la HLE dibuja de más», al LLE).
- **LLE casi vacía.** Una tarea donde la HLE dibuja ≥ 2 % de la pantalla y el LLE
  menos de la décima parte de eso se cuenta aparte y no entra en los
  porcentajes: el camino LLE → RDP falló en ella. Son el 15 % de las tareas tras los
  arreglos del punto 0 (eran el 27 %).
- Las imágenes son de 320 × 240 (`RASTER_W` × `RASTER_H`); lo más grande se
  recorta. El rasterizador va con los efectos de framebuffer en el nivel 2, como
  la PS3 (render a textura y copias a la RDRAM); no mide z, solo lo que cambia
  el color.
- Una escena por juego, ~50 s de consola con START y A alternados cada 60 campos
  (o un guion de `docs/pads/`): son menús, títulos y demos, no una partida.

## Cómo se corre

    SALSA64_ROMS=<carpeta con los respaldos> tools/censo-gfx.sh [-o dir] [-j n] [-t s] [-f campos] [juego…]
    python3 tools/censo-gfx-tabla.py build/censo-gfx      # las tablas de este documento

`tools/censo-gfx.sh` compila `salsa64-run` con el compilador del Mac (sin
sanitizadores), corre cada escena con un tope de tiempo real (`SALSA64_GCHK_SECONDS`)
y deja `NOMBRE.log` (el informe) y `NOMBRE.tsv` (una línea por tarea: campo,
microcódigo, clase, píxeles distintos, mayor diferencia, píxeles dibujados por el
LLE y por la HLE, triángulos y rectángulos de cada lado, tipo, caja, registros del
RDP distintos y píxeles sin explicar). Los respaldos son del usuario y no entran
al repositorio. Variables de `salsa64-run`:

| Variable | Qué hace |
|---|---|
| `SALSA64_GCHK_LOG=ruta` | el TSV por tarea |
| `SALSA64_GCHK_SECONDS=s` | tope de tiempo real; al pasarlo, informe y salida |
| `SALSA64_GCHK_DUMP=dir` (`_N`, `_FROM`) | de las distintas, un PPM de LLE \| HLE \| diferencias (rojo: solo el LLE cambió el píxel; verde: solo la HLE; amarillo: los dos, con otro color) |
| `SALSA64_GCHK_VERBOSE=1` | por cada distinta, los destinos con sus píxeles y los comandos de cada lado |
| `SALSA64_GCHK_STATE=1` | othermode y combiner de cada tarea donde el estado final difiere |
| `SALSA64_GCHK_ASYNC=1` | el LLE por rebanadas en vez de la tarea en el acto |
| `SALSA64_GCHK_SELF=1` | la HLE contra sí misma (prueba del método; sin LLE) |

`tests/unit/test_gfxcheck.c` prueba la clasificación (idéntica, redondeo,
borde corrido, falta, sobra, otro color), el estado del RDP y el cauce entero
con el rasterizador (incluida la RDRAM restaurada).

## Resultados

Escenas: START y A alternados cada 60 campos desde el 200 en la mayoría (2400 a
3600 campos); guiones de `docs/pads/` en Super Mario 64 (castillo), Mario Kart 64
(Luigi), GoldenEye (Dam), Pokémon Stadium (versus), Diddy Kong Racing, Jet Force
Gemini, Mickey's Speedway y Killer Instinct. Las filas de «Tareas» cuentan las
tareas gráficas de la escena; el resto de columnas, las que tienen HLE.

### Por juego

| Juego | Tareas | Con HLE | LLE vacía (excluidas) | Idénticas | Menores | Distintas (grandes) | Sin HLE | Microcódigos |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| 1080 | 2687 | 2687 | 16 | 1414 (52.9 %) | 447 (16.7 %) | 810 (200) (30.3 %) | 0 | F3DEX |
| banjo | 1150 | 1150 | 0 | 67 (5.8 %) | 659 (57.3 %) | 424 (408) (36.9 %) | 0 | F3DEX |
| banjotooie | 951 | 951 | 2 | 5 (0.5 %) | 811 (85.5 %) | 133 (115) (14.0 %) | 0 | F3DEX2 |
| castlevania | 1632 | 1632 | 372 | 193 (15.3 %) | 863 (68.5 %) | 204 (127) (16.2 %) | 0 | F3DEX2 |
| conker | 1716 | 1716 | 5 | 658 (38.5 %) | 203 (11.9 %) | 850 (767) (49.7 %) | 0 | F3DEXBG |
| cruisnusa | 1 | 1 | 0 | 0 (0.0 %) | 1 (100.0 %) | 0 (0) (0.0 %) | 0 | F3D |
| dk64 | 1375 | 1375 | 92 | 269 (21.0 %) | 960 (74.8 %) | 54 (54) (4.2 %) | 0 | F3DEX2 |
| dkr | 2729 | 2729 | 0 | 153 (5.6 %) | 1232 (45.1 %) | 1344 (1004) (49.2 %) | 0 | F3DDKR (sin texto) |
| doom64 | 1437 | 1437 | 154 | 287 (22.4 %) | 996 (77.6 %) | 0 (0) (0.0 %) | 0 | F3DEX |
| ewj3d | 2943 | 2943 | 791 | 851 (39.5 %) | 140 (6.5 %) | 1161 (1057) (53.9 %) | 0 | F3DEX |
| fifa99 | 2253 | 2253 | 321 | 994 (51.4 %) | 390 (20.2 %) | 548 (396) (28.4 %) | 0 | F3DEX |
| fzerox | 2753 | 2753 | 835 | 397 (20.7 %) | 268 (14.0 %) | 1253 (453) (65.3 %) | 0 | F3DEX2 |
| goldeneye | 6526 | 6526 | 1857 | 1602 (34.3 %) | 2798 (59.9 %) | 269 (238) (5.8 %) | 0 | F3D |
| hydrothunder | 1336 | 1336 | 239 | 622 (56.7 %) | 318 (29.0 %) | 157 (0) (14.3 %) | 0 | F3DEX2 |
| indy | 1801 | 0 | 0 | 0 (—) | 0 (—) | 0 (0) (—) | 1801 | desconocido (sin HLE) |
| jfg | 1289 | 1289 | 0 | 490 (38.0 %) | 8 (0.6 %) | 791 (406) (61.4 %) | 0 | F3DJFG (sin texto) |
| killerinstinct | 3599 | 3599 | 0 | 83 (2.3 %) | 2668 (74.1 %) | 848 (100) (23.6 %) | 0 | F3D |
| kirby64 | 1411 | 1411 | 0 | 197 (14.0 %) | 1214 (86.0 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| mace | 1445 | 1445 | 504 | 113 (12.0 %) | 56 (6.0 %) | 772 (685) (82.0 %) | 0 | F3DEX |
| marioparty | 4430 | 4430 | 361 | 2587 (63.6 %) | 1477 (36.3 %) | 5 (1) (0.1 %) | 0 | F3DEX2 |
| marioparty3 | 4728 | 4728 | 7 | 2811 (59.5 %) | 1405 (29.8 %) | 505 (0) (10.7 %) | 0 | F3DEX2 |
| mariotennis | 1380 | 1380 | 4 | 525 (38.2 %) | 821 (59.7 %) | 30 (0) (2.2 %) | 0 | F3DEX2 |
| mickey | 1332 | 1332 | 34 | 246 (19.0 %) | 740 (57.0 %) | 312 (4) (24.0 %) | 0 | F3DJFG (sin texto) |
| mk4 | 2829 | 2829 | 598 | 2167 (97.1 %) | 58 (2.6 %) | 6 (0) (0.3 %) | 0 | F3DEX |
| mk64 | 2714 | 2714 | 0 | 605 (22.3 %) | 1395 (51.4 %) | 714 (237) (26.3 %) | 0 | F3DEX |
| mkmythologies | 1472 | 1472 | 820 | 478 (73.3 %) | 1 (0.2 %) | 173 (28) (26.5 %) | 0 | F3DEX |
| mkt | 2953 | 2953 | 0 | 1833 (62.1 %) | 746 (25.3 %) | 374 (0) (12.7 %) | 0 | F3D |
| mm | 1563 | 1563 | 0 | 854 (54.6 %) | 639 (40.9 %) | 70 (69) (4.5 %) | 0 | F3DZEX2 |
| mspacman | 2923 | 2923 | 2063 | 577 (67.1 %) | 283 (32.9 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| nbashowtime | 2838 | 2838 | 305 | 2381 (94.0 %) | 141 (5.6 %) | 11 (0) (0.4 %) | 0 | F3DEX2 |
| nflblitz | 1478 | 1478 | 10 | 1175 (80.0 %) | 56 (3.8 %) | 237 (0) (16.1 %) | 0 | F3DEX2 |
| oot | 1627 | 1627 | 169 | 329 (22.6 %) | 1039 (71.3 %) | 90 (58) (6.2 %) | 0 | F3DZEX2 |
| papermario | 2954 | 2954 | 0 | 1768 (59.9 %) | 1186 (40.1 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| perfectdark | 1761 | 1761 | 85 | 156 (9.3 %) | 886 (52.9 %) | 634 (326) (37.8 %) | 0 | F3DPD (sin texto) |
| pksnap | 1883 | 1883 | 0 | 1750 (92.9 %) | 9 (0.5 %) | 124 (115) (6.6 %) | 0 | F3DEX2 |
| pkstadium | 827 | 827 | 331 | 232 (46.8 %) | 178 (35.9 %) | 86 (74) (17.3 %) | 0 | F3DEX2 |
| pkstadium2 | 1378 | 1378 | 44 | 1092 (81.9 %) | 235 (17.6 %) | 7 (0) (0.5 %) | 0 | F3DEX2 |
| re2 | 6995 | 6966 | 0 | 6812 (97.8 %) | 150 (2.2 %) | 4 (0) (0.1 %) | 29 | F3DEX2; S2DEX; desconocido (sin HLE) |
| roadrash64 | 2990 | 2990 | 1608 | 1065 (77.1 %) | 110 (8.0 %) | 207 (196) (15.0 %) | 0 | F3DEX2 |
| rogue | 1506 | 0 | 0 | 0 (—) | 0 (—) | 0 (0) (—) | 1506 | desconocido (sin HLE) |
| rush2049 | 1472 | 1472 | 522 | 167 (17.6 %) | 91 (9.6 %) | 692 (275) (72.8 %) | 0 | F3DEX2 |
| sf64 | 1997 | 1997 | 15 | 298 (15.0 %) | 1649 (83.2 %) | 35 (0) (1.8 %) | 0 | F3DEX |
| sm64 | 2037 | 2037 | 232 | 12 (0.7 %) | 1788 (99.1 %) | 5 (0) (0.3 %) | 0 | F3D |
| smash | 2946 | 2946 | 7 | 2822 (96.0 %) | 113 (3.8 %) | 4 (0) (0.1 %) | 0 | F3DEX2 |
| swracer | 5003 | 5003 | 2145 | 1051 (36.8 %) | 880 (30.8 %) | 927 (206) (32.4 %) | 0 | F3DEX2 |
| waverace | 913 | 913 | 154 | 28 (3.7 %) | 211 (27.8 %) | 520 (343) (68.5 %) | 0 | F3DWR |
| yoshi | 2297 | 2297 | 1291 | 17 (1.7 %) | 929 (92.3 %) | 60 (5) (6.0 %) | 0 | S2DEX1 |
| **Total** | 108260 | 104924 | 15993 | 42233 (47.5 %) | 31248 (35.1 %) | 15450 (7947) (17.4 %) | 3336 | |

Los porcentajes son sobre las tareas válidas (con HLE y con el LLE dibujando).

### Por microcódigo

| Microcódigo | Juegos | Tareas | Con HLE | LLE vacía (excluidas) | Idénticas | Menores | Distintas (grandes) | Sin HLE |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| F3D: RSP SW Version: 2.0D, 04-01-96 | cruisnusa, killerinstinct, mkt, sm64 | 8590 | 8590 | 232 | 1928 (23.1 %) | 5203 (62.3 %) | 1227 (100) (14.7 %) | 0 |
| F3D: RSP SW Version: 2.0G, 09-30-96 | goldeneye | 6526 | 6526 | 1857 | 1602 (34.3 %) | 2798 (59.9 %) | 269 (238) (5.8 %) | 0 |
| F3DDKR (sin texto) | dkr | 2729 | 2729 | 0 | 153 (5.6 %) | 1232 (45.1 %) | 1344 (1004) (49.2 %) | 0 |
| F3DEX2: F3DEX fifo 2.03 1998 | fzerox | 2048 | 2048 | 782 | 2 (0.2 %) | 15 (1.2 %) | 1249 (453) (98.7 %) | 0 |
| F3DEX2: F3DEX fifo 2.04H 1998 | kirby64, smash | 4357 | 4357 | 7 | 3019 (69.4 %) | 1327 (30.5 %) | 4 (0) (0.1 %) | 0 |
| F3DEX2: F3DEX fifo 2.05 1998 | nflblitz | 1478 | 1478 | 10 | 1175 (80.0 %) | 56 (3.8 %) | 237 (0) (16.1 %) | 0 |
| F3DEX2: F3DEX fifo 2.06 1998 | castlevania, marioparty, pkstadium | 5821 | 5821 | 1064 | 2760 (58.0 %) | 1702 (35.8 %) | 295 (202) (6.2 %) | 0 |
| F3DEX2: F3DEX fifo 2.07 1998 | dk64 | 1375 | 1375 | 92 | 269 (21.0 %) | 960 (74.8 %) | 54 (54) (4.2 %) | 0 |
| F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999 | papermario, pkstadium2 | 4332 | 4332 | 44 | 2860 (66.7 %) | 1421 (33.1 %) | 7 (0) (0.2 %) | 0 |
| F3DEX2: F3DEX fifo 2.08 1999 | hydrothunder, marioparty3, mariotennis | 5999 | 5999 | 243 | 3946 (68.6 %) | 1620 (28.1 %) | 190 (0) (3.3 %) | 0 |
| F3DEX2: F3DEX xbus 2.08 1999 | mspacman | 2923 | 2923 | 2063 | 577 (67.1 %) | 283 (32.9 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.05 1998 | roadrash64 | 2990 | 2990 | 1608 | 1065 (77.1 %) | 110 (8.0 %) | 207 (196) (15.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.06 1998 | marioparty | 534 | 534 | 0 | 252 (47.2 %) | 282 (52.8 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.08 1999 | banjotooie, marioparty3, rush2049, swracer | 8400 | 8400 | 2676 | 1235 (21.6 %) | 2705 (47.3 %) | 1784 (596) (31.2 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.08H 1999 | pksnap | 1883 | 1883 | 0 | 1750 (92.9 %) | 9 (0.5 %) | 124 (115) (6.6 %) | 0 |
| F3DEX2: F3DEX.Rej fifo 2.06 1998 | re2 | 1742 | 1742 | 0 | 1588 (91.2 %) | 150 (8.6 %) | 4 (0) (0.2 %) | 0 |
| F3DEX2: F3DFLX.Rej fifo 2.03F 1998 | fzerox | 565 | 565 | 53 | 390 (76.2 %) | 122 (23.8 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.03 1998 | fzerox | 140 | 140 | 0 | 5 (3.6 %) | 131 (93.6 %) | 4 (0) (2.9 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.06 1998 | marioparty | 534 | 534 | 0 | 0 (0.0 %) | 534 (100.0 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.08 1999 | marioparty3, nbashowtime | 3309 | 3309 | 305 | 2381 (79.3 %) | 142 (4.7 %) | 481 (0) (16.0 %) | 0 |
| F3DEX: F3DEX 0.95 | mk64 | 2714 | 2714 | 0 | 605 (22.3 %) | 1395 (51.4 %) | 714 (237) (26.3 %) | 0 |
| F3DEX: F3DEX 1.21 | banjo, mace | 2595 | 2595 | 504 | 180 (8.6 %) | 715 (34.2 %) | 1196 (1093) (57.2 %) | 0 |
| F3DEX: F3DEX 1.23 | 1080, ewj3d | 5630 | 5630 | 807 | 2265 (47.0 %) | 587 (12.2 %) | 1971 (1257) (40.9 %) | 0 |
| F3DEX: F3DEX.NoN 1.00 | doom64 | 1437 | 1437 | 154 | 287 (22.4 %) | 996 (77.6 %) | 0 (0) (0.0 %) | 0 |
| F3DEX: F3DEX.NoN 1.22 | sf64 | 1997 | 1997 | 15 | 298 (15.0 %) | 1649 (83.2 %) | 35 (0) (1.8 %) | 0 |
| F3DEX: F3DEX.NoN 1.23 | fifa99 | 2253 | 2253 | 321 | 994 (51.4 %) | 390 (20.2 %) | 548 (396) (28.4 %) | 0 |
| F3DEX: F3DLX.Rej 1.21 | mkmythologies | 1472 | 1472 | 820 | 478 (73.3 %) | 1 (0.2 %) | 173 (28) (26.5 %) | 0 |
| F3DEX: F3DLX.Rej 1.23 | mk4 | 2829 | 2829 | 598 | 2167 (97.1 %) | 58 (2.6 %) | 6 (0) (0.3 %) | 0 |
| F3DEXBG: F3DEXBG.NoN fifo 2.08 1999 | conker | 1716 | 1716 | 5 | 658 (38.5 %) | 203 (11.9 %) | 850 (767) (49.7 %) | 0 |
| F3DJFG (sin texto) | jfg, mickey | 2621 | 2621 | 34 | 736 (28.4 %) | 748 (28.9 %) | 1103 (410) (42.6 %) | 0 |
| F3DPD (sin texto) | perfectdark | 1761 | 1761 | 85 | 156 (9.3 %) | 886 (52.9 %) | 634 (326) (37.8 %) | 0 |
| F3DWR: RSP SW Version: 2.0D, 04-01-96 | waverace | 913 | 913 | 154 | 28 (3.7 %) | 211 (27.8 %) | 520 (343) (68.5 %) | 0 |
| F3DZEX2: F3DZEX.NoN fifo 2.06H 1998 | oot | 1627 | 1627 | 169 | 329 (22.6 %) | 1039 (71.3 %) | 90 (58) (6.2 %) | 0 |
| F3DZEX2: F3DZEX.NoN fifo 2.08I /Kawasedo 1999 | mm | 1563 | 1563 | 0 | 854 (54.6 %) | 639 (40.9 %) | 70 (69) (4.5 %) | 0 |
| S2DEX1: S2DEX 1.06 | yoshi | 2297 | 2297 | 1291 | 17 (1.7 %) | 929 (92.3 %) | 60 (5) (6.0 %) | 0 |
| S2DEX: S2DEX fifo 2.06 1998 | re2 | 5224 | 5224 | 0 | 5224 (100.0 %) | 0 (0.0 %) | 0 (0) (0.0 %) | 0 |
| desconocido (sin HLE) | indy, re2, rogue | 3336 | 0 | 0 | 0 (—) | 0 (—) | 0 (0) (—) | 3336 |

### Microcódigos gráficos sin HLE (CRC de ucode_data)

| CRC | Juego | Tareas |
|---|---|---:|
| 9dc92266 | indy | 1801 |
| 444d50b0 | rogue | 576 |
| cc9d06d7 | rogue | 575 |
| 61b95a1c | rogue | 120 |
| 1cad21a1 | rogue | 120 |
| 214c9620 | rogue | 36 |
| 92ac5480 | rogue | 35 |
| 3be4649f | rogue | 20 |
| a4eb5eb4 | rogue | 19 |
| 70c5f7c2 | re2 | 1 |
| dd5e2e96 | re2 | 1 |
| eb215526 | re2 | 1 |
| dfd9fb02 | re2 | 1 |
| 5430511c | re2 | 1 |
| b4985bc9 | re2 | 1 |
| d6e1e78d | re2 | 1 |
| 7b5f6dd8 | re2 | 1 |
| c9858342 | re2 | 1 |
| 9a7ba062 | re2 | 1 |
| df7d779a | re2 | 1 |
| 71f719ab | re2 | 1 |
| 5328a983 | re2 | 1 |
| b315fa2b | re2 | 1 |
| 3b1e5d52 | re2 | 1 |
| 22061fef | re2 | 1 |
| a6e1e63b | re2 | 1 |
| 6ea8823b | re2 | 1 |
| 3ef9cd5e | re2 | 1 |
| 39d6ec0e | re2 | 1 |
| 5526bfd0 | re2 | 1 |
| b166ee30 | re2 | 1 |
| e05b2a4f | re2 | 1 |
| 83a50454 | re2 | 1 |
| 7f983b61 | re2 | 1 |
| 3b5088da | re2 | 1 |
| 9111ec8d | re2 | 1 |
| 980c2fc6 | re2 | 1 |
| b40e3c6f | re2 | 1 |
| 36f0777f | rogue | 1 |
| 100a054d | rogue | 1 |
| 0b3ddb01 | rogue | 1 |
| 3b183961 | rogue | 1 |
| c7a107ca | rogue | 1 |

### Tipo de las tareas distintas (válidas)

| Microcódigo | La HLE no dibuja | La HLE dibuja de más | Otro color |
|---|---:|---:|---:|
| F3D: RSP SW Version: 2.0D, 04-01-96 | 18 | 355 | 854 |
| F3D: RSP SW Version: 2.0G, 09-30-96 | 1 | 119 | 149 |
| F3DDKR (sin texto) | 0 | 33 | 1311 |
| F3DEX2: F3DEX fifo 2.03 1998 | 0 | 425 | 824 |
| F3DEX2: F3DEX fifo 2.04H 1998 | 0 | 0 | 4 |
| F3DEX2: F3DEX fifo 2.05 1998 | 0 | 48 | 189 |
| F3DEX2: F3DEX fifo 2.06 1998 | 0 | 94 | 201 |
| F3DEX2: F3DEX fifo 2.07 1998 | 0 | 38 | 16 |
| F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999 | 0 | 0 | 7 |
| F3DEX2: F3DEX fifo 2.08 1999 | 0 | 10 | 180 |
| F3DEX2: F3DEX.NoN fifo 2.05 1998 | 0 | 187 | 20 |
| F3DEX2: F3DEX.NoN fifo 2.08 1999 | 21 | 1122 | 641 |
| F3DEX2: F3DEX.NoN fifo 2.08H 1999 | 1 | 45 | 78 |
| F3DEX2: F3DEX.Rej fifo 2.06 1998 | 0 | 0 | 4 |
| F3DEX2: F3DLX.Rej fifo 2.03 1998 | 0 | 0 | 4 |
| F3DEX2: F3DLX.Rej fifo 2.08 1999 | 0 | 0 | 481 |
| F3DEX: F3DEX 0.95 | 0 | 200 | 514 |
| F3DEX: F3DEX 1.21 | 155 | 429 | 612 |
| F3DEX: F3DEX 1.23 | 0 | 692 | 1279 |
| F3DEX: F3DEX.NoN 1.22 | 0 | 0 | 35 |
| F3DEX: F3DEX.NoN 1.23 | 0 | 110 | 438 |
| F3DEX: F3DLX.Rej 1.21 | 0 | 80 | 93 |
| F3DEX: F3DLX.Rej 1.23 | 0 | 1 | 5 |
| F3DEXBG: F3DEXBG.NoN fifo 2.08 1999 | 0 | 286 | 564 |
| F3DJFG (sin texto) | 2 | 38 | 1063 |
| F3DPD (sin texto) | 1 | 376 | 257 |
| F3DWR: RSP SW Version: 2.0D, 04-01-96 | 0 | 48 | 472 |
| F3DZEX2: F3DZEX.NoN fifo 2.06H 1998 | 1 | 73 | 16 |
| F3DZEX2: F3DZEX.NoN fifo 2.08I /Kawasedo 1999 | 1 | 0 | 69 |
| S2DEX1: S2DEX 1.06 | 3 | 7 | 50 |

### Comandos sin implementar, por microcódigo

Ninguno: la HLE contó 0 comandos sin implementar (`unknown_cmds`) en todas las escenas.

### Registros del RDP que difieren al final de la tarea

- F3D: RSP SW Version: 2.0D, 04-01-96: oh.dither ×3602
- F3D: RSP SW Version: 2.0G, 09-30-96: oh.dither ×1
- F3DDKR (sin texto): oh.dither ×2581
- F3DEX2: F3DEX fifo 2.03 1998: oh.dither ×1
- F3DEX2: F3DEX fifo 2.04H 1998: tile.lrs ×1411, tile.cms ×1352, tile.cmt ×1352, tile.masks ×1352, tile.maskt ×1352, tile.line ×1352, tile.siz ×933, timg_width ×932
- F3DEX2: F3DEX fifo 2.05 1998: oh.dither ×1
- F3DEX2: F3DEX fifo 2.06 1998: oh ×1948, ol ×1422, oh.dither ×277
- F3DEX2: F3DEX fifo 2.07 1998: ol ×8, cc_hi ×8, cc_lo ×8, prim ×8, timg_addr ×8, timg_width ×8, timg_fmt ×8, tile.fmt ×8
- F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999: oh ×319, oh.dither ×298
- F3DEX2: F3DEX fifo 2.08 1999: oh ×1406, ol ×1406, oh.dither ×234
- F3DEX2: F3DEX xbus 2.08 1999: oh.dither ×3
- F3DEX2: F3DEX.NoN fifo 2.05 1998: oh.dither ×1
- F3DEX2: F3DEX.NoN fifo 2.08 1999: oh ×76, timg_addr ×76, ol ×74, timg_fmt ×74, tile.fmt ×74, tile.masks ×64, tile.maskt ×63, tile.siz ×57
- F3DEX2: F3DEX.NoN fifo 2.08H 1999: oh.dither ×1, ol ×1
- F3DEX2: F3DEX.Rej fifo 2.06 1998: oh ×227, ol ×227, oh.dither ×1
- F3DEX2: F3DFLX.Rej fifo 2.03F 1998: oh.dither ×3, oh ×1
- F3DEX2: F3DLX.Rej fifo 2.03 1998: oh.dither ×2, oh ×1
- F3DEX2: F3DLX.Rej fifo 2.08 1999: oh.dither ×1
- F3DEX: F3DEX 0.95: oh ×1, oh.dither ×1
- F3DEX: F3DEX 1.21: oh ×438, timg_addr ×429, ol ×338, cc_hi ×330, cc_lo ×330, tile.masks ×298, timg_fmt ×294, tile.fmt ×294
- F3DEX: F3DEX 1.23: ol ×6, oh.dither ×2
- F3DEX: F3DEX.NoN 1.00: oh ×1, oh.dither ×1
- F3DEX: F3DEX.NoN 1.23: oh.dither ×40
- F3DEX: F3DLX.Rej 1.21: oh.dither ×1
- F3DEX: F3DLX.Rej 1.23: oh.dither ×1
- F3DJFG (sin texto): oh.dither ×2093
- F3DPD (sin texto): ol ×44, timg_addr ×37, oh.dither ×31, oh ×30, prim ×30, scissor ×30, cc_hi ×5, cc_lo ×5
- F3DWR: RSP SW Version: 2.0D, 04-01-96: oh ×6, oh.dither ×2
- S2DEX1: S2DEX 1.06: tile.siz ×1542, timg_width ×1541, timg_siz ×1541, tile.lrs ×1158, tile.lrt ×1135, tile.fmt ×65, tile.line ×63, tile.cms ×5
- S2DEX: S2DEX fifo 2.06 1998: timg_addr ×2854, tile.cms ×2854, tile.cmt ×2854, tile.masks ×2854, tile.maskt ×2854, tile.line ×2854, tile.ult ×2854, tile.lrs ×2854

### Peor tarea válida distinta de cada juego y microcódigo

| Juego | Microcódigo | Tipo | Tarea (campo) | Píxeles distintos | Dibujados LLE / HLE | Caja |
|---|---|---|---|---:|---|---|
| 1080 | F3DEX | otro color | 2480 (2793) | 43909 | 62466 / 61800 | (0,0-320,240) |
| banjo | F3DEX | otro color | 298 (651) | 63072 | 63065 / 32360 | (0,0-292,216) |
| banjotooie | F3DEX2 | la HLE no dibuja | 426 (1368) | 68461 | 68911 / 1829 | (0,0-304,228) |
| castlevania | F3DEX2 | la HLE dibuja de más | 727 (1184) | 44109 | 8426 / 51278 | (0,0-320,240) |
| conker | F3DEXBG | otro color | 1225 (2570) | 57135 | 60883 / 60728 | (2,0-290,216) |
| dk64 | F3DEX2 | la HLE dibuja de más | 622 (1472) | 35015 | 15664 / 42000 | (10,10-309,229) |
| dkr | F3DDKR (sin texto) | otro color | 1845 (3753) | 64381 | 70009 / 74658 | (0,0-320,239) |
| ewj3d | F3DEX | otro color | 2316 (2372) | 76717 | 72996 / 61772 | (0,0-320,240) |
| fifa99 | F3DEX | otro color | 2250 (2995) | 46362 | 51074 / 60609 | (0,0-320,240) |
| fzerox | F3DEX2 | otro color | 735 (981) | 25231 | 40768 / 41495 | (12,16-308,224) |
| fzerox | F3DEX2 | otro color | 681 (920) | 2970 | 2751 / 3606 | (161,150-270,232) |
| goldeneye | F3D | la HLE dibuja de más | 3153 (3593) | 39898 | 7869 / 41036 | (1,10-319,230) |
| hydrothunder | F3DEX2 | otro color | 877 (2081) | 29852 | 45513 / 47306 | (12,12-308,228) |
| jfg | F3DJFG (sin texto) | otro color | 638 (1297) | 30873 | 41718 / 46099 | (1,0-320,240) |
| killerinstinct | F3D | otro color | 745 (1145) | 45125 | 38391 / 50664 | (60,0-320,240) |
| mace | F3DEX | otro color | 268 (644) | 42152 | 57512 / 60279 | (12,10-308,230) |
| marioparty | F3DEX2 | otro color | 2039 (1851) | 51990 | 73713 / 74302 | (0,0-320,240) |
| marioparty3 | F3DEX2 | otro color | 2379 (2061) | 20654 | 76156 / 76800 | (0,0-320,240) |
| marioparty3 | F3DEX2 | otro color | 2550 (2129) | 8218 | 18498 / 18019 | (61,13-256,163) |
| marioparty3 | F3DEX2 | otro color | 2591 (2145) | 6354 | 10004 / 8592 | (0,83-258,240) |
| mariotennis | F3DEX2 | otro color | 1220 (2679) | 43240 | 54524 / 56152 | (0,0-320,224) |
| mickey | F3DJFG (sin texto) | otro color | 1153 (2437) | 34839 | 59022 / 58774 | (0,0-320,240) |
| mk4 | F3DEX | la HLE dibuja de más | 631 (744) | 39865 | 4324 / 41477 | (18,12-320,228) |
| mk64 | F3DEX | otro color | 194 (610) | 25407 | 76734 / 76724 | (10,70-309,170) |
| mkmythologies | F3DEX | otro color | 543 (1140) | 36332 | 63234 / 64135 | (0,0-320,240) |
| mkt | F3D | otro color | 2157 (2201) | 792 | 19447 / 19548 | (116,214-205,238) |
| mm | F3DZEX2 | la HLE no dibuja | 1498 (3403) | 56320 | 76800 / 40799 | (0,32-320,208) |
| nbashowtime | F3DEX2 | otro color | 2788 (2947) | 20177 | 34425 / 33997 | (16,12-320,228) |
| nflblitz | F3DEX2 | otro color | 1034 (2106) | 55642 | 65222 / 64041 | (0,0-320,240) |
| oot | F3DZEX2 | otro color | 1599 (3514) | 40594 | 55651 / 56010 | (0,32-320,208) |
| perfectdark | F3DPD (sin texto) | otro color | 640 (1363) | 36999 | 51784 / 62706 | (0,0-320,220) |
| pksnap | F3DEX2 | la HLE dibuja de más | 152 (167) | 63185 | 29604 / 65517 | (0,0-320,240) |
| pkstadium | F3DEX2 | otro color | 257 (599) | 48880 | 52874 / 52288 | (0,0-320,240) |
| pkstadium2 | F3DEX2 | otro color | 92 (199) | 4233 | 15941 / 15934 | (248,104-320,240) |
| re2 | F3DEX2 | otro color | 491 (175) | 17478 | 18497 / 18497 | (31,13-320,240) |
| roadrash64 | F3DEX2 | la HLE dibuja de más | 1203 (1212) | 15046 | 17394 / 21157 | (0,0-320,240) |
| rush2049 | F3DEX2 | otro color | 321 (696) | 48687 | 60338 / 65368 | (0,0-320,220) |
| sf64 | F3DEX | otro color | 1929 (3862) | 18130 | 64212 / 64150 | (8,8-312,232) |
| sm64 | F3D | otro color | 1188 (2551) | 30306 | 57652 / 57660 | (0,8-320,232) |
| smash | F3DEX2 | otro color | 147 (168) | 8878 | 20807 / 20464 | (10,10-310,230) |
| swracer | F3DEX2 | otro color | 3378 (2125) | 42158 | 68019 / 67978 | (8,16-312,240) |
| waverace | F3DWR | otro color | 570 (1969) | 36691 | 46523 / 49441 | (8,20-311,219) |
| yoshi | S2DEX1 | otro color | 298 (575) | 36384 | 73499 / 73487 | (0,0-319,240) |

## Qué mirar primero (lista priorizada)

### 0. El oráculo mismo: el camino LLE → RDP (arreglado en parte)

La primera versión de este censo dejaba 28 032 tareas (27 %) sin comparar porque
el microcódigo real «no dibujaba». Se midió con volcados crudos (cada
DPC_START/END/CURRENT/STATUS y los comandos que leía `dpc.c`) y no era un solo
fallo sino tres, ninguno del intérprete del RSP:

1. **Comandos partidos entre dos DPC_END.** F3DEX 1.x (y otros) avanzan DPC_END de
   a 8 bytes dentro de un comando: un TEXRECT (16 bytes) llegaba como dos tramos
   de 8, y `dpc.c` tiraba el primero («el comando sigue en el próximo tramo» sin
   guardarlo) y arrancaba el siguiente en mitad del comando, que salía como
   ceros (opcode 0x00, los «NOP» del hallazgo inicial). En Mario Kart 64 Map
   Select, 116 443 de 2,35 millones de tramos eran un TEXRECT partido. Arreglo
   en `src/core/sp.c` (`dp_run`): solo se entregan comandos completos y la cola
   incompleta se guarda en el bus y se completa con el tramo contiguo siguiente,
   como el RDP de verdad. **Es código compartido con la PS3** (la PS3 repite por
   `salsa64_dp_submit` los tramos del SPE por el mismo `dp_run`): requiere
   validación en la consola, pero solo cambia lo que antes se perdía.
2. **RDP congelado hasta que el CPU lo descongela.** F3DEX2 en FIFO pone DPC_STATUS
   en FREEZE al arrancar la tarea y es el CPU quien lo quita después; los comandos
   se acumulan y se entregan al descongelar, tras el fin de la tarea. El censo
   comparaba antes de eso y el LLE parecía vacío (Donkey Kong 64, Banjo-Kazooie,
   Banjo-Tooie, 1080°, Killer Instinct…). Arreglo solo del oráculo
   (`salsa64_dp_drain`, host): al terminar la tarea se dibuja lo pendiente aunque
   esté congelado. No toca la PS3.
3. **Cada tramo envejecía los destinos.** El rasterizador cuenta «tareas» para
   decidir si una color image sigue fresca (`RDP_TARGETS_FRESH` = 3) y la vuelve a
   cebar desde la RDRAM si no; el LLE entrega cada comando como un trabajo
   propio y bastaban 3 tramos sin dibujar en el destino (los de estado) para
   borrar lo dibujado (Mortal Kombat Mythologies: 3204 de 10 864 píxeles). Ahora
   los tramos de una misma lista (hasta su SYNC_FULL, `GFX_JOB_DPC_CONT`) cuentan
   como una tarea (`sync.c`, `raster.c`, `run.c`). **Trasladado a la PS3** (2026-10-18,
   por validar en la consola): `src/rdp/rdp.c` (`translate`) ya no llama a
   `rdp_targets_task` en los trabajos con `GFX_JOB_DPC_CONT` (la bandera llega por
   `done`, que la guarda en la ranura) y `spu_dpc_run` (`src/platform/ps3/gfx_spu.c`) la
   pone mientras la lista no haya visto SYNC_FULL (o un corte), con el mismo criterio de
   `sync.c`. El log de la PS3 trae al final «gfx: listas del RDP: N trabajos continuaban
   una lista abierta…, M destinos que el envejecimiento por tramo habría vuelto a cebar»:
   N > 0 con LLE/JIT o con una lista del CPU (Killer Instinct) confirma que el camino se
   usa; M es cuántas veces un destino dibujado habría pasado de fresco a cebarse de nuevo.

Resultado: el LLE vacío bajó de 28 032 a 15 993 tareas. Mario Kart 64 (0.95),
Killer Instinct, Mortal Kombat Trilogy, Banjo-Kazooie y Donkey Kong 64 quedaron
sin casi ninguna; Mario Kart 64 pasó de 0,7 % a 22 % idénticas y 51 % menores.

4. **«Vacía» era «no cambió nada»: pantallas fijas** (2026-10-18, GoldenEye campo 216
   y Mace campo 530, con volcado crudo de DPC_START/END/STATUS y de cada comando:
   `SALSA64_GCHK_DPC_TRACE=campo_ini,campo_fin`, solo con `-DGFX_CENSUS`). Una tarea se
   marcaba «LLE casi vacía» si la HLE cambiaba el 2 % de la pantalla respecto a lo que
   había en el búfer y el LLE menos de una décima de eso. Pero el búfer es de un
   triple búfer: cuando el juego repite el mismo cuadro (menú, título, logo), el LLE
   lo dibuja entero y **deja exactamente lo que ya había** (0 píxeles cambiados) y la HLE,
   que difiere unos píxeles del LLE, «cambia» 2 595 (GoldenEye, clase «menor», dither
   y redondeo) o 7 643 (Mace, «distinta»). En GoldenEye 216 el LLE entrega 24
   triángulos y 254 rectángulos (la HLE, 12 y 254) con el RDP configurado igual; en
   Mace 530, 648 triángulos y 48 rectángulos (la HLE, 324 y 48), el mismo cociente 2 de
   siempre por el recorte. Cuando la escena cambia el LLE también cambia (Ms. Pac-Man:
   0 → 112 píxeles cuando los de la HLE pasan de 27 192 a 27 284). No es una causa del
   camino LLE → RDP sino de la medición: **ahora una tarea solo es «vacía» si además
   el LLE entregó menos triángulos y rectángulos que la HLE** (`gfxcheck.c`, el informe
   añade «de ellas N con triángulos y rectángulos del LLE ≥ HLE» y
   `tools/censo-gfx-tabla.py` aplica lo mismo). Es una inferencia por el recuento y
   por esa coincidencia de cambios; no se probó con el búfer envenenado.

Resultado: el LLE vacío bajó de 28 032 a 15 993 tareas con los puntos 1 a 3, y a
**1 069 con el 4** (las 12 escenas de abajo; las otras 805 de las 15 993 son de juegos
que no se volvieron a correr aquí). Las que dejaban de contar entran ahora en la
clasificación: la HLE de esos juegos queda peor parada, no mejor (Mace pasa de
«504 vacías» a 1 276 distintas de 1 445).

| Juego | Tareas | Vacías antes | Vacías ahora | Idénticas | Menores | Distintas (grandes) |
|---|---:|---:|---:|---:|---:|---:|
| GoldenEye | 6526 | 1857 | 0 | 1602 | 4624 | 300 (269) |
| Star Wars Racer | 5003 | 2145 | 0 | 1051 | 2366 | 1586 (220) |
| Ms. Pac-Man | 2923 | 2063 | 0 | 577 | 2346 | 0 |
| Road Rash 64 | 2990 | 1608 | 0 | 1065 | 110 | 1815 (1804) |
| Yoshi's Story | 2297 | 1291 | 1069 | 17 | 1151 | 60 (5) |
| F-Zero X | 2753 | 835 | 0 | 397 | 321 | 2035 (529) |
| Mortal Kombat Mythologies | 1472 | 820 | 0 | 478 | 61 | 933 (28) |
| Earthworm Jim 3D | 2943 | 791 | 0 | 851 | 152 | 1940 (1836) |
| Mortal Kombat 4 | 2829 | 598 | 0 | 2167 | 498 | 164 (0) |
| Rush 2049 | 1472 | 522 | 0 | 167 | 388 | 917 (331) |
| Mace | 1445 | 504 | 0 | 113 | 56 | 1276 (1189) |
| Doom 64 | 1437 | 154 | 0 | 287 | 1150 | 0 |

(GoldenEye antes: 1 857 en la primera corrida; 1 821 en la segunda con la misma
definición, la escena tiene un tope de tiempo.) **Lo que sigue vacío de verdad:
Yoshi's Story** (S2DEX 1.06, 1 069 tareas desde el campo ~1900, campo 1904 volcado): la
lista dibuja el fondo en 48 tiras de `TEXRECT` en modo copia (320 × 5, tile 0 tras un
`LOAD_TILE` a la tile 7); el LLE entrega 150 rectángulos contra los 298 de la HLE y
cambia 700–2 000 píxeles de los 43 000 de la HLE. Es el siguiente caso: volcar los
pixeles de una tira (`SALSA64_GFX_PIX`) y mirar la carga a la TMEM y el `TEXRECT` en
copia de `gfx_texrect`.

### 1. Diferencias de la HLE (el LLE dibuja y la HLE no, o el color difiere donde el LLE es fiable)

Top 5 por tareas distintas grandes (tabla por juego), ya sobre un LLE que dibuja:

| # | Juego | Microcódigo | Qué se vio |
|---|---|---|---|
| 1 | Earthworm Jim 3D | F3DEX 1.23 | 1161 distintas (1057 grandes, 54 % de las válidas): 1080 «la HLE dibuja de más», 860 «otro color»; el LLE sigue vacío en 791 tareas más |
| 2 | Diddy Kong Racing | F3DDKR | 1344 distintas (1004 grandes, 49 %), 1311 «otro color»; a las HLE de Rare solo las puede juzgar el LLE, y aquí dibuja (0 vacías) |
| 3 | Conker's Bad Fur Day | F3DEXBG | 850 distintas (767 grandes, 50 %): 564 «otro color», 291 «de más» |
| 4 | Mace + Banjo-Kazooie | F3DEX 1.21 | 1196 distintas (1093 grandes, 57 %): Mace 901 «de más» (con 504 vacías aún) y Banjo 155 «la HLE no dibuja» (la intro de Rareware sale con otro brillo y sin el resplandor), 237 «otro color» |
| 5 | F-Zero X, Jet Force Gemini/Mickey's, FIFA 99, Perfect Dark | F3DEX2 2.03, F3DJFG, F3DEX.NoN 1.23, F3DPD | 1249 (453 grandes), 1103 (410), 548 (396) y 634 (326) distintas |

Aparte: Pokémon Snap (F3DEX2 NoN 2.08H) sigue siendo el caso mejor medido, 124 de
1883 distintas (115 grandes), todas escenas 3D con el cielo de otro color (gris
violáceo en la HLE, azul en el LLE; el «orden de dibujo» ya conocido), con 0 vacías.
Mickey's Speedway tiene 2 «la HLE no dibuja», Majora's Mask 1 (la pantalla gris de
transición) y Banjo-Tooie ahora es comparable (132 distintas de 951, 115 grandes).
Killer Instinct pasó de 3345 a 848 distintas (100 grandes), todas «otro color».

### 2. Los problemas conocidos contra el censo

| Problema | Microcódigo | Qué dice el censo |
|---|---|---|
| Mortal Kombat 4, barra de vida | F3DLX.Rej 1.23 | 2829 tareas: 2167 idénticas, 58 menores, 6 distintas y 598 vacías. No concluyente: las tareas de pelea (color image de 400 × 300) están entre las vacías por el recorte a 320 × 240 |
| Castlevania, orden de dibujo | F3DEX2 fifo 2.06 | 204 distintas (127 grandes) de 1260 válidas; en la selección de jugador el pergamino sale con otro tono y la HLE dibuja de más (51 278 píxeles contra 8 426 del LLE) |
| Earthworm Jim 3D, orden de dibujo | F3DEX 1.23 | punto 1 de la tabla: 1161 distintas, el caso a mirar primero |
| Pokémon Snap, orden de dibujo | F3DEX2 NoN 2.08H | el punto 1; es el caso mejor medido |
| Mario Tennis, alfa | F3DEX2 fifo 2.08 | 65 distintas (11 grandes) de 1380; en la pista (tarea 1061) el LLE saca ruido en franjas horizontales y la HLE franjas verticales de colores: las dos mal, es una escena de render a textura |
| Mace, textos | F3DEX 1.21 | 504 vacías y 772 distintas (685 grandes): el LLE de Mace aún no es fiable |
| FIFA 99, pantalla negra | F3DEX NoN 1.23 | 548 distintas (396 grandes) y 321 vacías en 2253; 438 «otro color», 223 «la HLE dibuja de más» |
| El sol sobre las montañas | — | no se identificó la escena; con el TSV (`SALSA64_GCHK_LOG`) se busca por juego y campo |
| Resident Evil 2, partes invisibles | S2DEX 2.06 + F3DEX.Rej 2.06 | 6966 tareas con HLE: 6812 idénticas (97,8 %), 150 menores, 4 distintas; las 5224 S2DEX (fondos de la intro) son idénticas. La escena (intro) no llega a los personajes; hay que repetirla con un guion que los alcance |
| Yoshi's Story, cielo | S2DEX 1.06 | 1291 de 2297 con el LLE vacío; de las 1006 válidas, 60 distintas (5 grandes), 929 menores |

### 3. Otros hallazgos

- **Microcódigos gráficos sin HLE**: Indiana Jones y la Máquina Infernal (un
  microcódigo, 1801 tareas) y Star Wars: Rogue Squadron (nueve, 1506 tareas) no
  tienen HLE; 29 tareas de Resident Evil 2 (video: otro camino). El LLE las
  corre; para compararlas hace falta una HLE (la de Factor 5, en curso).
- **La HLE no deja nada sin implementar en lo que corrió**: 0 comandos en
  `unknown_cmds` en las 47 escenas. Lo que le falta no son opcodes desconocidos
  sino semántica: el dither de othermode H (`oh.dither`: 3602 tareas de F3D, 2581
  de DKR, 2093 de JFG), y estado final distinto en Smash/Kirby (tiles: S2DEX
  dentro de F3DEX2), Yoshi (tiles y imagen de textura) y GoldenEye (color image).
- **Estado que difiere entre tareas**: en Mario Party (F3DEX2 2.06/2.08) el
  LLE termina con othermode 0x00380CFF / 0 y la HLE con 0x00B92CFF / 1 (1948 y
  1406 tareas): el microcódigo reinicia el modo al final y la HLE no. Es
  inofensivo mientras la lista siguiente lo fije.

## Pendiente

- Las tareas vacías que quedan de verdad (punto 4): Yoshi's Story, 1 069 (tiras de `TEXRECT`
  en modo copia); volver a correr el censo de los otros 35 juegos con la medición nueva.
  Validar en la PS3 los cambios del punto 0 (el 1 está en código compartido; el 3, ya en
  `rdp.c` y `gfx_spu.c`, falta probarlo en la consola).
- Escenas de juego (no menús) para los títulos donde la barra de vida, el HUD y
  los personajes importan: Mortal Kombat 4, Resident Evil 2, Mace, FIFA 99.
- Comparar el LLE con una referencia externa (ares) en las escenas donde LLE y
  HLE discrepan, para decidir de qué lado está la razón.
- Cobertura de z (no solo color) y 640 × 480 (también los 400 × 300 de Mortal
  Kombat 4) para Perfect Dark.
