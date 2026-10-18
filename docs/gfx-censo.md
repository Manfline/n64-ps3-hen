# Censo gráfico: el microcódigo real contra la HLE (2026-10-18)

`salsa64-run --lle=all --lle-check-gfx` es el equivalente gráfico del censo de
audio ([audio-censo.md](audio-censo.md)): cada tarea gráfica corre en el
microcódigo del propio juego (el LLE de [rsp-lle.md](rsp-lle.md)) y en la HLE
desde el mismo estado, y se compara lo que dejan los dos. Es una **herramienta
de medición**: este documento no corrige ningún microcódigo; lista lo que
mide y por dónde conviene seguir.

**Resultado en una línea.** En 47 juegos (105 373 tareas gráficas, una escena
de ~50 s de consola por juego) 102 037 tienen HLE; en 28 032 (27 %) el
microcódigo real entrega al RDP casi nada y la comparación no dice nada de la
HLE (el camino LLE → RDP falla, sobre todo con F3DEX 1.x); de las 74 005
restantes, **48,2 % son idénticas píxel a píxel, 23,1 % tienen una diferencia
menor y 28,7 % son distintas** (14 950 de ellas, el 20 %, grandes: más del 3 %
de la pantalla). Con F3DEX2 y la rama Zelda, donde el LLE es fiable, la HLE es
idéntica o casi en el 90–100 % de las tareas de Smash, Kirby, Paper Mario,
Resident Evil 2, Pokémon Stadium 2, Mario Party y Majora; los números malos
son de microcódigos viejos (F3D, F3DEX 1.x), de los propios de Rare y de
S2DEX 1.x, donde el oráculo mismo es poco fiable (ver «Qué dice y qué no dice»).

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
4. **Al terminar** (el primer acceso del CPU al SP con HALT y TASKDONE) se guarda
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
  porcentajes: el camino LLE → RDP falló en ella. Son el 27 % de las tareas.
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
| 1080 | 2687 | 2687 | 1913 | 16 (2.1 %) | 265 (34.2 %) | 493 (403) (63.7 %) | 0 | F3DEX |
| banjo | 1150 | 1150 | 540 | 56 (9.2 %) | 122 (20.0 %) | 432 (409) (70.8 %) | 0 | F3DEX |
| banjotooie | 951 | 951 | 767 | 3 (1.6 %) | 49 (26.6 %) | 132 (115) (71.7 %) | 0 | F3DEX2 |
| castlevania | 1632 | 1632 | 372 | 193 (15.3 %) | 863 (68.5 %) | 204 (127) (16.2 %) | 0 | F3DEX2 |
| conker | 1716 | 1716 | 3 | 554 (32.3 %) | 307 (17.9 %) | 852 (769) (49.7 %) | 0 | F3DEXBG |
| cruisnusa | 1 | 1 | 1 | 0 (—) | 0 (—) | 0 (0) (—) | 0 | F3D |
| dk64 | 1375 | 1375 | 1069 | 235 (76.8 %) | 56 (18.3 %) | 15 (8) (4.9 %) | 0 | F3DEX2 |
| dkr | 2729 | 2729 | 61 | 92 (3.4 %) | 681 (25.5 %) | 1895 (1421) (71.0 %) | 0 | F3DDKR (sin texto) |
| doom64 | 1437 | 1437 | 1388 | 32 (65.3 %) | 11 (22.4 %) | 6 (3) (12.2 %) | 0 | F3DEX |
| ewj3d | 2943 | 2943 | 1634 | 14 (1.1 %) | 34 (2.6 %) | 1261 (1257) (96.3 %) | 0 | F3DEX |
| fifa99 | 2253 | 2253 | 323 | 993 (51.5 %) | 372 (19.3 %) | 565 (416) (29.3 %) | 0 | F3DEX |
| fzerox | 2753 | 2753 | 835 | 377 (19.7 %) | 270 (14.1 %) | 1271 (456) (66.3 %) | 0 | F3DEX2 |
| goldeneye | 3639 | 3639 | 1628 | 1595 (79.3 %) | 212 (10.5 %) | 204 (179) (10.1 %) | 0 | F3D |
| hydrothunder | 1336 | 1336 | 239 | 622 (56.7 %) | 318 (29.0 %) | 157 (0) (14.3 %) | 0 | F3DEX2 |
| indy | 1801 | 0 | 0 | 0 (—) | 0 (—) | 0 (0) (—) | 1801 | desconocido (sin HLE) |
| jfg | 1289 | 1289 | 366 | 129 (14.0 %) | 3 (0.3 %) | 791 (674) (85.7 %) | 0 | F3DJFG (sin texto) |
| killerinstinct | 3599 | 3599 | 217 | 49 (1.4 %) | 205 (6.1 %) | 3128 (3091) (92.5 %) | 0 | F3D |
| kirby64 | 1411 | 1411 | 0 | 197 (14.0 %) | 1214 (86.0 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| mace | 1445 | 1445 | 756 | 1 (0.1 %) | 42 (6.1 %) | 646 (646) (93.8 %) | 0 | F3DEX |
| marioparty | 4430 | 4430 | 361 | 2587 (63.6 %) | 1477 (36.3 %) | 5 (1) (0.1 %) | 0 | F3DEX2 |
| marioparty3 | 4728 | 4728 | 7 | 2811 (59.5 %) | 1405 (29.8 %) | 505 (0) (10.7 %) | 0 | F3DEX2 |
| mariotennis | 1380 | 1380 | 3 | 519 (37.7 %) | 793 (57.6 %) | 65 (11) (4.7 %) | 0 | F3DEX2 |
| mickey | 1332 | 1332 | 224 | 64 (5.8 %) | 174 (15.7 %) | 870 (404) (78.5 %) | 0 | F3DJFG (sin texto) |
| mk4 | 2829 | 2829 | 598 | 2167 (97.1 %) | 58 (2.6 %) | 6 (0) (0.3 %) | 0 | F3DEX |
| mk64 | 2714 | 2714 | 666 | 18 (0.9 %) | 146 (7.1 %) | 1884 (1019) (92.0 %) | 0 | F3DEX |
| mkmythologies | 1472 | 1472 | 1434 | 38 (100.0 %) | 0 (0.0 %) | 0 (0) (0.0 %) | 0 | F3DEX |
| mkt | 2953 | 2953 | 2931 | 6 (27.3 %) | 4 (18.2 %) | 12 (12) (54.5 %) | 0 | F3D |
| mm | 1563 | 1563 | 0 | 856 (54.8 %) | 641 (41.0 %) | 66 (66) (4.2 %) | 0 | F3DZEX2 |
| mspacman | 2923 | 2923 | 2064 | 577 (67.2 %) | 282 (32.8 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| nbashowtime | 2838 | 2838 | 305 | 2381 (94.0 %) | 141 (5.6 %) | 11 (0) (0.4 %) | 0 | F3DEX2 |
| nflblitz | 1478 | 1478 | 10 | 1175 (80.0 %) | 56 (3.8 %) | 237 (0) (16.1 %) | 0 | F3DEX2 |
| oot | 1627 | 1627 | 169 | 329 (22.6 %) | 1039 (71.3 %) | 90 (58) (6.2 %) | 0 | F3DZEX2 |
| papermario | 2954 | 2954 | 0 | 1768 (59.9 %) | 1186 (40.1 %) | 0 (0) (0.0 %) | 0 | F3DEX2 |
| perfectdark | 1761 | 1761 | 466 | 61 (4.7 %) | 302 (23.3 %) | 932 (825) (72.0 %) | 0 | F3DPD (sin texto) |
| pksnap | 1883 | 1883 | 0 | 1750 (92.9 %) | 9 (0.5 %) | 124 (115) (6.6 %) | 0 | F3DEX2 |
| pkstadium | 827 | 827 | 331 | 232 (46.8 %) | 177 (35.7 %) | 87 (75) (17.5 %) | 0 | F3DEX2 |
| pkstadium2 | 1378 | 1378 | 44 | 1073 (80.4 %) | 239 (17.9 %) | 22 (13) (1.6 %) | 0 | F3DEX2 |
| re2 | 6995 | 6966 | 0 | 6812 (97.8 %) | 150 (2.2 %) | 4 (0) (0.1 %) | 29 | F3DEX2; S2DEX; desconocido (sin HLE) |
| roadrash64 | 2990 | 2990 | 1608 | 1065 (77.1 %) | 110 (8.0 %) | 207 (196) (15.0 %) | 0 | F3DEX2 |
| rogue | 1506 | 0 | 0 | 0 (—) | 0 (—) | 0 (0) (—) | 1506 | desconocido (sin HLE) |
| rush2049 | 1472 | 1472 | 503 | 167 (17.2 %) | 102 (10.5 %) | 700 (277) (72.2 %) | 0 | F3DEX2 |
| sf64 | 1997 | 1997 | 199 | 149 (8.3 %) | 905 (50.3 %) | 744 (637) (41.4 %) | 0 | F3DEX |
| sm64 | 2037 | 2037 | 244 | 12 (0.7 %) | 1201 (67.0 %) | 580 (8) (32.3 %) | 0 | F3D |
| smash | 2946 | 2946 | 7 | 2822 (96.0 %) | 113 (3.8 %) | 4 (0) (0.1 %) | 0 | F3DEX2 |
| swracer | 5003 | 5003 | 2145 | 1051 (36.8 %) | 880 (30.8 %) | 927 (206) (32.4 %) | 0 | F3DEX2 |
| waverace | 913 | 913 | 156 | 28 (3.7 %) | 29 (3.8 %) | 700 (666) (92.5 %) | 0 | F3DWR |
| yoshi | 2297 | 2297 | 1445 | 13 (1.5 %) | 428 (50.2 %) | 411 (387) (48.2 %) | 0 | S2DEX1 |
| **Total** | 105373 | 102037 | 28032 | 35689 (48.2 %) | 17071 (23.1 %) | 21245 (14950) (28.7 %) | 3336 | |

Los porcentajes son sobre las tareas válidas (con HLE y con el LLE dibujando).

### Por microcódigo

| Microcódigo | Juegos | Tareas | Con HLE | LLE vacía (excluidas) | Idénticas | Menores | Distintas (grandes) | Sin HLE |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| F3D: RSP SW Version: 2.0D, 04-01-96 | cruisnusa, killerinstinct, mkt, sm64 | 8590 | 8590 | 3393 | 67 (1.3 %) | 1410 (27.1 %) | 3720 (3111) (71.6 %) | 0 |
| F3D: RSP SW Version: 2.0G, 09-30-96 | goldeneye | 3639 | 3639 | 1628 | 1595 (79.3 %) | 212 (10.5 %) | 204 (179) (10.1 %) | 0 |
| F3DDKR (sin texto) | dkr | 2729 | 2729 | 61 | 92 (3.4 %) | 681 (25.5 %) | 1895 (1421) (71.0 %) | 0 |
| F3DEX2: F3DEX fifo 2.03 1998 | fzerox | 2048 | 2048 | 782 | 2 (0.2 %) | 15 (1.2 %) | 1249 (453) (98.7 %) | 0 |
| F3DEX2: F3DEX fifo 2.04H 1998 | kirby64, smash | 4357 | 4357 | 7 | 3019 (69.4 %) | 1327 (30.5 %) | 4 (0) (0.1 %) | 0 |
| F3DEX2: F3DEX fifo 2.05 1998 | nflblitz | 1478 | 1478 | 10 | 1175 (80.0 %) | 56 (3.8 %) | 237 (0) (16.1 %) | 0 |
| F3DEX2: F3DEX fifo 2.06 1998 | castlevania, marioparty, pkstadium | 5821 | 5821 | 1064 | 2760 (58.0 %) | 1701 (35.8 %) | 296 (203) (6.2 %) | 0 |
| F3DEX2: F3DEX fifo 2.07 1998 | dk64 | 1375 | 1375 | 1069 | 235 (76.8 %) | 56 (18.3 %) | 15 (8) (4.9 %) | 0 |
| F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999 | papermario, pkstadium2 | 4332 | 4332 | 44 | 2841 (66.3 %) | 1425 (33.2 %) | 22 (13) (0.5 %) | 0 |
| F3DEX2: F3DEX fifo 2.08 1999 | hydrothunder, marioparty3, mariotennis | 5999 | 5999 | 242 | 3940 (68.4 %) | 1592 (27.7 %) | 225 (11) (3.9 %) | 0 |
| F3DEX2: F3DEX xbus 2.08 1999 | mspacman | 2923 | 2923 | 2064 | 577 (67.2 %) | 282 (32.8 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.05 1998 | roadrash64 | 2990 | 2990 | 1608 | 1065 (77.1 %) | 110 (8.0 %) | 207 (196) (15.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.06 1998 | marioparty | 534 | 534 | 0 | 252 (47.2 %) | 282 (52.8 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.08 1999 | banjotooie, marioparty3, rush2049, swracer | 8400 | 8400 | 3422 | 1233 (24.8 %) | 1954 (39.3 %) | 1791 (598) (36.0 %) | 0 |
| F3DEX2: F3DEX.NoN fifo 2.08H 1999 | pksnap | 1883 | 1883 | 0 | 1750 (92.9 %) | 9 (0.5 %) | 124 (115) (6.6 %) | 0 |
| F3DEX2: F3DEX.Rej fifo 2.06 1998 | re2 | 1742 | 1742 | 0 | 1588 (91.2 %) | 150 (8.6 %) | 4 (0) (0.2 %) | 0 |
| F3DEX2: F3DFLX.Rej fifo 2.03F 1998 | fzerox | 565 | 565 | 53 | 370 (72.3 %) | 124 (24.2 %) | 18 (3) (3.5 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.03 1998 | fzerox | 140 | 140 | 0 | 5 (3.6 %) | 131 (93.6 %) | 4 (0) (2.9 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.06 1998 | marioparty | 534 | 534 | 0 | 0 (0.0 %) | 534 (100.0 %) | 0 (0) (0.0 %) | 0 |
| F3DEX2: F3DLX.Rej fifo 2.08 1999 | marioparty3, nbashowtime | 3309 | 3309 | 305 | 2381 (79.3 %) | 142 (4.7 %) | 481 (0) (16.0 %) | 0 |
| F3DEX: F3DEX 0.95 | mk64 | 2714 | 2714 | 666 | 18 (0.9 %) | 146 (7.1 %) | 1884 (1019) (92.0 %) | 0 |
| F3DEX: F3DEX 1.21 | banjo, mace | 2595 | 2595 | 1296 | 57 (4.4 %) | 164 (12.6 %) | 1078 (1055) (83.0 %) | 0 |
| F3DEX: F3DEX 1.23 | 1080, ewj3d | 5630 | 5630 | 3547 | 30 (1.4 %) | 299 (14.4 %) | 1754 (1660) (84.2 %) | 0 |
| F3DEX: F3DEX.NoN 1.00 | doom64 | 1437 | 1437 | 1388 | 32 (65.3 %) | 11 (22.4 %) | 6 (3) (12.2 %) | 0 |
| F3DEX: F3DEX.NoN 1.22 | sf64 | 1997 | 1997 | 199 | 149 (8.3 %) | 905 (50.3 %) | 744 (637) (41.4 %) | 0 |
| F3DEX: F3DEX.NoN 1.23 | fifa99 | 2253 | 2253 | 323 | 993 (51.5 %) | 372 (19.3 %) | 565 (416) (29.3 %) | 0 |
| F3DEX: F3DLX.Rej 1.21 | mkmythologies | 1472 | 1472 | 1434 | 38 (100.0 %) | 0 (0.0 %) | 0 (0) (0.0 %) | 0 |
| F3DEX: F3DLX.Rej 1.23 | mk4 | 2829 | 2829 | 598 | 2167 (97.1 %) | 58 (2.6 %) | 6 (0) (0.3 %) | 0 |
| F3DEXBG: F3DEXBG.NoN fifo 2.08 1999 | conker | 1716 | 1716 | 3 | 554 (32.3 %) | 307 (17.9 %) | 852 (769) (49.7 %) | 0 |
| F3DJFG (sin texto) | jfg, mickey | 2621 | 2621 | 590 | 193 (9.5 %) | 177 (8.7 %) | 1661 (1078) (81.8 %) | 0 |
| F3DPD (sin texto) | perfectdark | 1761 | 1761 | 466 | 61 (4.7 %) | 302 (23.3 %) | 932 (825) (72.0 %) | 0 |
| F3DWR: RSP SW Version: 2.0D, 04-01-96 | waverace | 913 | 913 | 156 | 28 (3.7 %) | 29 (3.8 %) | 700 (666) (92.5 %) | 0 |
| F3DZEX2: F3DZEX.NoN fifo 2.06H 1998 | oot | 1627 | 1627 | 169 | 329 (22.6 %) | 1039 (71.3 %) | 90 (58) (6.2 %) | 0 |
| F3DZEX2: F3DZEX.NoN fifo 2.08I /Kawasedo 1999 | mm | 1563 | 1563 | 0 | 856 (54.8 %) | 641 (41.0 %) | 66 (66) (4.2 %) | 0 |
| S2DEX1: S2DEX 1.06 | yoshi | 2297 | 2297 | 1445 | 13 (1.5 %) | 428 (50.2 %) | 411 (387) (48.2 %) | 0 |
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
| F3D: RSP SW Version: 2.0D, 04-01-96 | 2 | 158 | 3560 |
| F3D: RSP SW Version: 2.0G, 09-30-96 | 3 | 59 | 142 |
| F3DDKR (sin texto) | 25 | 172 | 1698 |
| F3DEX2: F3DEX fifo 2.03 1998 | 0 | 425 | 824 |
| F3DEX2: F3DEX fifo 2.04H 1998 | 0 | 0 | 4 |
| F3DEX2: F3DEX fifo 2.05 1998 | 0 | 48 | 189 |
| F3DEX2: F3DEX fifo 2.06 1998 | 0 | 94 | 202 |
| F3DEX2: F3DEX fifo 2.07 1998 | 0 | 7 | 8 |
| F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999 | 0 | 0 | 22 |
| F3DEX2: F3DEX fifo 2.08 1999 | 4 | 23 | 198 |
| F3DEX2: F3DEX.NoN fifo 2.05 1998 | 0 | 187 | 20 |
| F3DEX2: F3DEX.NoN fifo 2.08 1999 | 21 | 1132 | 638 |
| F3DEX2: F3DEX.NoN fifo 2.08H 1999 | 1 | 45 | 78 |
| F3DEX2: F3DEX.Rej fifo 2.06 1998 | 0 | 0 | 4 |
| F3DEX2: F3DFLX.Rej fifo 2.03F 1998 | 0 | 0 | 18 |
| F3DEX2: F3DLX.Rej fifo 2.03 1998 | 0 | 0 | 4 |
| F3DEX2: F3DLX.Rej fifo 2.08 1999 | 0 | 0 | 481 |
| F3DEX: F3DEX 0.95 | 0 | 679 | 1205 |
| F3DEX: F3DEX 1.21 | 156 | 451 | 471 |
| F3DEX: F3DEX 1.23 | 0 | 344 | 1410 |
| F3DEX: F3DEX.NoN 1.00 | 2 | 2 | 2 |
| F3DEX: F3DEX.NoN 1.22 | 0 | 405 | 339 |
| F3DEX: F3DEX.NoN 1.23 | 0 | 113 | 452 |
| F3DEX: F3DLX.Rej 1.23 | 0 | 1 | 5 |
| F3DEXBG: F3DEXBG.NoN fifo 2.08 1999 | 0 | 288 | 564 |
| F3DJFG (sin texto) | 10 | 256 | 1395 |
| F3DPD (sin texto) | 2 | 849 | 81 |
| F3DWR: RSP SW Version: 2.0D, 04-01-96 | 0 | 107 | 593 |
| F3DZEX2: F3DZEX.NoN fifo 2.06H 1998 | 1 | 73 | 16 |
| F3DZEX2: F3DZEX.NoN fifo 2.08I /Kawasedo 1999 | 1 | 0 | 65 |
| S2DEX1: S2DEX 1.06 | 0 | 358 | 53 |

### Comandos sin implementar, por microcódigo

Ninguno: la HLE contó 0 comandos sin implementar (`unknown_cmds`) en todas las escenas.

### Registros del RDP que difieren al final de la tarea

- F3D: RSP SW Version: 2.0D, 04-01-96: oh.dither ×3602, cimg_addr ×215, cimg_width ×215, cimg_fmt ×215
- F3D: RSP SW Version: 2.0G, 09-30-96: cimg_addr ×527, cimg_width ×527, cimg_fmt ×526, zimg_addr ×110, fill ×51, blend ×25, prim_z ×22, k4 ×22
- F3DDKR (sin texto): oh.dither ×2581
- F3DEX2: F3DEX fifo 2.03 1998: oh.dither ×1
- F3DEX2: F3DEX fifo 2.04H 1998: tile.lrs ×1411, tile.cms ×1352, tile.cmt ×1352, tile.masks ×1352, tile.maskt ×1352, tile.line ×1352, tile.siz ×933, timg_width ×932
- F3DEX2: F3DEX fifo 2.05 1998: oh.dither ×1
- F3DEX2: F3DEX fifo 2.06 1998: oh ×1948, ol ×1422, oh.dither ×277
- F3DEX2: F3DEX fifo 2.07 1998: cimg_addr ×1349, timg_addr ×299, prim ×206, fog ×42, tile.siz ×29, tile.fmt ×26, cc_hi ×24, timg_fmt ×23
- F3DEX2: F3DEX fifo 2.08 /Kawasedo 1999: oh ×319, oh.dither ×298
- F3DEX2: F3DEX fifo 2.08 1999: oh ×1406, ol ×1406, oh.dither ×234
- F3DEX2: F3DEX xbus 2.08 1999: oh.dither ×3
- F3DEX2: F3DEX.NoN fifo 2.05 1998: oh.dither ×1
- F3DEX2: F3DEX.NoN fifo 2.08 1999: cimg_addr ×769, prim ×223, env ×212, timg_addr ×93, ol ×88, tile.fmt ×83, timg_fmt ×81, oh ×79
- F3DEX2: F3DEX.NoN fifo 2.08H 1999: oh.dither ×1, ol ×1
- F3DEX2: F3DEX.Rej fifo 2.06 1998: oh ×227, ol ×227, oh.dither ×1
- F3DEX2: F3DFLX.Rej fifo 2.03F 1998: oh.dither ×3, oh ×1
- F3DEX2: F3DLX.Rej fifo 2.03 1998: oh.dither ×2, oh ×1
- F3DEX2: F3DLX.Rej fifo 2.08 1999: oh.dither ×1
- F3DEX: F3DEX 0.95: oh ×1, oh.dither ×1
- F3DEX: F3DEX 1.21: cimg_addr ×604, oh ×442, timg_addr ×434, ol ×346, cc_hi ×338, cc_lo ×338, timg_fmt ×296, tile.fmt ×296
- F3DEX: F3DEX 1.23: cimg_addr ×241, cimg_width ×241, cimg_fmt ×241, ol ×6, oh.dither ×2
- F3DEX: F3DEX.NoN 1.00: oh ×1, oh.dither ×1
- F3DEX: F3DEX.NoN 1.23: oh.dither ×40
- F3DEX: F3DLX.Rej 1.21: oh.dither ×1
- F3DEX: F3DLX.Rej 1.23: oh.dither ×1
- F3DJFG (sin texto): oh.dither ×2093
- F3DPD (sin texto): cimg_addr ×217, env ×115, ol ×50, timg_addr ×45, prim ×38, oh ×36, oh.dither ×34, scissor ×30
- F3DWR: RSP SW Version: 2.0D, 04-01-96: oh ×6, oh.dither ×2
- S2DEX1: S2DEX 1.06: tile.siz ×1542, timg_width ×1541, timg_siz ×1541, tile.lrs ×1158, tile.lrt ×1135, tile.fmt ×65, tile.line ×63, tile.cms ×5
- S2DEX: S2DEX fifo 2.06 1998: timg_addr ×2854, tile.cms ×2854, tile.cmt ×2854, tile.masks ×2854, tile.maskt ×2854, tile.line ×2854, tile.ult ×2854, tile.lrs ×2854

### Peor tarea válida distinta de cada juego y microcódigo

| Juego | Microcódigo | Tipo | Tarea (campo) | Píxeles distintos | Dibujados LLE / HLE | Caja |
|---|---|---|---|---:|---|---|
| 1080 | F3DEX | otro color | 2480 (2793) | 46071 | 60911 / 60811 | (0,0-320,240) |
| banjo | F3DEX | otro color | 298 (651) | 63072 | 63065 / 32360 | (0,0-292,216) |
| banjotooie | F3DEX2 | la HLE no dibuja | 426 (1368) | 68461 | 68911 / 1829 | (0,0-304,228) |
| castlevania | F3DEX2 | la HLE dibuja de más | 727 (1184) | 44109 | 8426 / 51278 | (0,0-320,240) |
| conker | F3DEXBG | otro color | 1225 (2570) | 57135 | 61635 / 61480 | (2,0-290,216) |
| dk64 | F3DEX2 | otro color | 459 (1143) | 33120 | 49543 / 50875 | (10,10-309,229) |
| dkr | F3DDKR (sin texto) | otro color | 1845 (3753) | 64385 | 70009 / 74658 | (0,0-320,239) |
| doom64 | F3DEX | la HLE no dibuja | 302 (729) | 21565 | 17324 / 5869 | (0,20-320,240) |
| ewj3d | F3DEX | otro color | 982 (999) | 76800 | 76800 / 76800 | (0,0-320,240) |
| fifa99 | F3DEX | otro color | 505 (830) | 76798 | 75108 / 76355 | (0,0-320,240) |
| fzerox | F3DEX2 | otro color | 735 (981) | 25231 | 40768 / 41495 | (12,16-308,224) |
| fzerox | F3DEX2 | otro color | 352 (588) | 10888 | 24655 / 24655 | (60,33-260,232) |
| fzerox | F3DEX2 | otro color | 681 (920) | 2970 | 2751 / 3606 | (161,150-270,232) |
| goldeneye | F3D | la HLE dibuja de más | 3639 (4088) | 138686 | 69136 / 69550 | (0,0-320,240) |
| hydrothunder | F3DEX2 | otro color | 877 (2081) | 29852 | 45513 / 47306 | (12,12-308,228) |
| jfg | F3DJFG (sin texto) | otro color | 1268 (2557) | 31765 | 40888 / 45359 | (4,1-320,240) |
| killerinstinct | F3D | la HLE no dibuja | 4 (32) | 71236 | 76089 / 14756 | (0,0-320,240) |
| mace | F3DEX | otro color | 157 (333) | 63821 | 47193 / 65017 | (12,10-307,229) |
| marioparty | F3DEX2 | otro color | 2039 (1851) | 51990 | 73713 / 74302 | (0,0-320,240) |
| marioparty3 | F3DEX2 | otro color | 2379 (2061) | 20654 | 76156 / 76800 | (0,0-320,240) |
| marioparty3 | F3DEX2 | otro color | 2550 (2129) | 8218 | 18498 / 18019 | (61,13-256,163) |
| marioparty3 | F3DEX2 | otro color | 2591 (2145) | 6354 | 10004 / 8592 | (0,83-258,240) |
| mariotennis | F3DEX2 | la HLE dibuja de más | 1061 (2361) | 99878 | 50459 / 98039 | (0,0-320,224) |
| mickey | F3DJFG (sin texto) | la HLE dibuja de más | 837 (1805) | 151643 | 37269 / 151643 | (0,0-320,240) |
| mk4 | F3DEX | la HLE dibuja de más | 631 (744) | 39865 | 4324 / 41477 | (18,12-320,228) |
| mk64 | F3DEX | otro color | 243 (748) | 75273 | 76416 / 75936 | (0,0-320,240) |
| mkt | F3D | otro color | 2405 (2451) | 62835 | 76241 / 76201 | (0,23-319,239) |
| mm | F3DZEX2 | la HLE no dibuja | 1498 (3403) | 56320 | 76800 / 40799 | (0,32-320,208) |
| nbashowtime | F3DEX2 | otro color | 2788 (2947) | 20177 | 34425 / 33997 | (16,12-320,228) |
| nflblitz | F3DEX2 | otro color | 1034 (2106) | 55642 | 65222 / 64041 | (0,0-320,240) |
| oot | F3DZEX2 | otro color | 1599 (3514) | 40594 | 55651 / 56010 | (0,32-320,208) |
| perfectdark | F3DPD (sin texto) | la HLE dibuja de más | 394 (1049) | 86307 | 63861 / 118755 | (0,0-320,240) |
| pksnap | F3DEX2 | la HLE dibuja de más | 152 (167) | 63185 | 29604 / 65517 | (0,0-320,240) |
| pkstadium | F3DEX2 | otro color | 415 (957) | 76552 | 76800 / 76800 | (0,0-320,240) |
| pkstadium2 | F3DEX2 | otro color | 575 (1383) | 49731 | 61685 / 61685 | (0,0-320,240) |
| re2 | F3DEX2 | otro color | 491 (175) | 17478 | 18497 / 18497 | (31,13-320,240) |
| roadrash64 | F3DEX2 | la HLE dibuja de más | 1203 (1212) | 15046 | 17394 / 21157 | (0,0-320,240) |
| rush2049 | F3DEX2 | otro color | 321 (696) | 48687 | 60338 / 65368 | (0,0-320,220) |
| sf64 | F3DEX | otro color | 1630 (3264) | 28115 | 68094 / 68092 | (8,8-312,232) |
| sm64 | F3D | otro color | 1970 (4115) | 38362 | 66318 / 66012 | (0,8-320,232) |
| smash | F3DEX2 | otro color | 147 (168) | 8878 | 20807 / 20464 | (10,10-310,230) |
| swracer | F3DEX2 | otro color | 3378 (2125) | 42158 | 68019 / 67978 | (8,16-312,240) |
| waverace | F3DWR | otro color | 230 (868) | 53757 | 36624 / 55210 | (8,21-311,219) |
| yoshi | S2DEX1 | otro color | 252 (483) | 76547 | 76547 / 66532 | (0,0-320,240) |

## Qué mirar primero (lista priorizada)

### 0. El oráculo mismo: el camino LLE → RDP en F3DEX 1.x y otros

Sin esto, 28 032 tareas (27 %) no se pueden comparar. La columna «LLE vacía» por
juego lo muestra: **F3DEX 1.x** (Mario Kart 64 0.95: 666 de 2714; Banjo-Kazooie
1.21: 540 de 1150; 1080° y Earthworm Jim 3D 1.23: 1913 de 2687 y 1634 de 2943;
Mace 1.21: 756 de 1445; Doom 64 1.00: 1388 de 1437; Mortal Kombat Mythologies
1.21: 1434 de 1472), F3D 2.0D de Mortal Kombat Trilogy (2931 de 2953) y GoldenEye
(F3D 2.0G: 1628 de 3639), **F3DEX2** de Ms. Pac-Man (xbus, 2064 de 2923), Road
Rash 64 (1608 de 2990), Star Wars Racer Episode I (2145 de 5003), Donkey Kong 64
(1069 de 1375) y S2DEX 1.x de Yoshi's Story (1445 de 2297).

Dos síntomas medidos: en Mario Kart 64 (menú «Map Select») el microcódigo
entrega a `dpc.c` ~410 comandos por tarea (cargas de textura: SETTIMG,
LOADSYNC, SETTILE, SETTILESIZE) pero ~10 rectángulos contra los 207 que la HLE
dibuja; y entre ellos aparecen palabras de ceros (opcode 0x00, NOP) en lugar de
los G_TEXRECT que faltan (p. ej. los primeros 12 NOP aparecen cada 0x70 bytes
al leer el FIFO de la RDRAM; pasa con la tarea en el acto y con las rebanadas
del LLE, así que no es la carrera con el CPU). Hipótesis (sin comprobar): el RDP
instantáneo (`dp_flush` entrega [CURRENT, END) al escribir DPC_END) lee de la
RDRAM antes de que llegue lo último que el microcódigo manda por DMA, o el
intérprete del RSP escribe mal esos comandos en F3DEX 1.x. Los fondos de
Killer Instinct (F3D, 3128 distintas) muestran lo mismo desde otro lado: el LLE
dibuja el fondo y deja sin dibujar a los luchadores y el HUD que la HLE sí dibuja.
Arreglar esto vale más que cualquier cambio de HLE: es lo que permite fiarse del
censo en más de una docena de juegos.

### 1. Diferencias de la HLE (el LLE dibuja y la HLE no, o el color difiere donde el LLE es fiable)

Las más claras son las 228 tareas válidas «la HLE no dibuja»: Banjo-Kazooie
(F3DEX 1.21, 156 tareas; la intro de Rareware sale con otro brillo y sin el
resplandor), Diddy Kong Racing (25), Banjo-Tooie (21; parece una cortina de
transición: la HLE dibuja una región que crece, el LLE la pantalla entera),
Mickey's Speedway (F3DJFG, 10 tareas con 76 000 píxeles del LLE contra 11 000
de la HLE) y Majora's Mask (una pantalla gris de transición o pausa: el LLE
dibuja la imagen de fondo y la HLE la mitad).
Después, por microcódigo, las distintas grandes donde el LLE casi siempre
dibuja (columna «Distintas (grandes)» de la tabla por microcódigo):

| # | Microcódigo | Juego | Qué se vio |
|---|---|---|---|
| 1 | F3DEX2 NoN 2.08H | Pokémon Snap | 124 de 1883 tareas distintas (115 grandes), todas escenas 3D: el cielo sale con otro color (gris violáceo en la HLE, azul en el LLE) y el terreno con otra composición; coincide con el «orden de dibujo» ya conocido. Con el LLE fiable (0 vacías) |
| 2 | F3DEX2 fifo 2.03 + F3DFLX/F3DLX | F-Zero X | 1249 distintas (98,7 % de las válidas de 2.03; 453 grandes): 425 «la HLE dibuja de más», 824 «otro color»; a ojo la pista y el HUD son iguales (la diferencia son bordes y texto con más de un píxel de corrimiento). Los F3DFLX son idénticos o casi (18 distintas de 565) |
| 3 | F3DEXBG | Conker's Bad Fur Day | 852 distintas (769 grandes, 50 %): 564 «otro color», 288 «de más» |
| 4 | F3DEX2 NoN 2.08 | Banjo-Tooie, Rush 2049, Star Wars Racer, Mario Party 3 | 1791 distintas (598 grandes, 36 %): 1132 «la HLE dibuja de más» (el LLE vacío a medias) y 638 «otro color» |
| 5 | F3DDKR, F3DJFG, F3DPD, F3DWR | Diddy Kong Racing, Jet Force Gemini y Mickey's, Perfect Dark, Wave Race | 1895 (71 %), 1661 (82 %), 932 (72 %) y 700 (92 %) distintas, casi todas «otro color»: a las HLE de Rare solo las puede juzgar el LLE, y aquí el LLE dibuja (61, 590, 466 y 156 tareas vacías) |

Lo demás de «otro color» en F3D 2.0D (3560, casi todo Killer Instinct y Super
Mario 64) y en F3DEX 1.x es, por lo dicho en el punto 0, más del LLE que de la HLE.

### 2. Los problemas conocidos contra el censo

| Problema | Microcódigo | Qué dice el censo |
|---|---|---|
| Mortal Kombat 4, barra de vida | F3DLX.Rej 1.23 | 2829 tareas: 2167 idénticas, 58 menores, 6 distintas (0 grandes) y 598 con el LLE vacío (21 %). No concluyente: las tareas de pelea, donde estaría la barra, están entre las vacías |
| Castlevania, orden de dibujo | F3DEX2 fifo 2.06 | 204 distintas (127 grandes) de 1260 válidas; en la selección de jugador el pergamino sale con otro tono y la HLE dibuja de más (51 278 píxeles contra 8 426 del LLE) |
| Earthworm Jim 3D, orden de dibujo | F3DEX 1.23 | 1634 de 2943 con el LLE vacío; las 1261 distintas (1257 grandes) no se pueden atribuir a la HLE (punto 0) |
| Pokémon Snap, orden de dibujo | F3DEX2 NoN 2.08H | el punto 1 de la tabla anterior; es el caso mejor medido |
| Mario Tennis, alfa | F3DEX2 fifo 2.08 | 65 distintas (11 grandes) de 1380; en la pista (tarea 1061) el LLE saca ruido en franjas horizontales y la HLE franjas verticales de colores: las dos mal, es una escena de render a textura |
| Mace, textos | F3DEX 1.21 | 756 de 1445 con el LLE vacío y 646 distintas grandes: sin LLE no se puede medir |
| FIFA 99, pantalla negra | F3DEX NoN 1.23 | 565 distintas (416 grandes) y 323 vacías en 2253; 452 «otro color», 113 «la HLE dibuja de más» |
| El sol sobre las montañas | — | no se identificó la escena; con el TSV (`SALSA64_GCHK_LOG`) se busca por juego y campo |
| Resident Evil 2, partes invisibles | S2DEX 2.06 + F3DEX.Rej 2.06 | 6966 tareas con HLE: 6812 idénticas (97,8 %), 150 menores, 4 distintas; las 5224 S2DEX (fondos de la intro) son idénticas. La escena (intro) no llega a los personajes; hay que repetirla con un guion que los alcance |
| Yoshi's Story, cielo | S2DEX 1.06 | 1445 de 2297 con el LLE vacío; de las 852 válidas, 411 distintas (387 grandes): en la pantalla de título la HLE dibuja la imagen a ~15 % de brillo y el LLE a pleno (358 «la HLE dibuja de más») |

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

- Arreglar el camino LLE → RDP (punto 0) y repetir el censo: sin él, solo F3DEX2
  y Zelda dan números fiables.
- Escenas de juego (no menús) para los títulos donde la barra de vida, el HUD y
  los personajes importan: Mortal Kombat 4, Resident Evil 2, Mace, FIFA 99.
- Comparar el LLE con una referencia externa (ares) en las escenas donde LLE y
  HLE discrepan, para decidir de qué lado está la razón.
- Cobertura de z (no solo color) y 640 × 480 para Perfect Dark.
