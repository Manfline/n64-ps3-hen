# El SPE 0 por familias de microcódigos

El programa del SPE 0 (la HLE gráfica: `src/spu/gfx` + `src/rsp/gfx`) ya no
cabía en los 256 KB del local store: el 2026-10-17 quedaban 1840 bytes de
holgura de pila (mínimo 1536) y la caché de texturas había bajado de 1024 a
768 entradas para que entrara Diddy Kong Racing, con S2DEX suelto, Factor 5,
Acclaim, ZSortBOSS, Turbo3D y otros todavía por escribir. Desde entonces el
programa se enlaza **varias veces desde las mismas fuentes**, una imagen por
grupo de familias, y el PPE carga la de la familia de cada tarea.

- Las fuentes no se duplican: cada imagen compila todo `src/rsp/gfx` con
  `-DGFX_SPE0_IMG=<n>` y lo que no es de su grupo queda fuera por
  `GFX_HAS(parte)` (`src/rsp/gfx/spe0.h`) y `--gc-sections`.
- El host y el PPE compilan todo, sin límite (`GFX_PARTS = GFX_PART_ALL`),
  y su caché de texturas tiene 1024 entradas.
- `src/platform/ps3/gfx_spu.c` elige, carga y comprueba la imagen.

## Inventario (antes de partir)

Del `.map` del programa único de main e22e10c (enlazado aparte con
`-Wl,-Map`, las mismas banderas y `--gc-sections`) y `tools/spu-stack.py`:
texto 111 152, datos 800, bss 138 640 bytes (250 592 de 262 144); la cadena
más honda usa 9520 bytes y la holgura es 1840.

| Parte | Código (bytes) | Familia |
|---|---:|---|
| `rdp.c` (estado y primitivas del RDP) | 25 552 | común |
| `tmem.c` (cargas y decodificación de texturas) | 15 664 | común |
| `combine.c` (combiner, `-Os`) | 11 072 | común |
| `geom.c` (matrices, vértices, luces) | 10 208 | común |
| `texcache.c` (hash y caché, `-Os`) | 4 752 | común |
| `dl.c` (recorrido de la lista, `-Os`) | 4 344 | común |
| `io_spu.c` (E/S por DMA) | 3 904 (+ 98 352 de bss: búferes) | común |
| `main.c` | 1 784 (+ 40 032 de bss: `gfx_state`) | común |
| `vgroup.c` (interpolación, `-Os`) | 1 520 | común |
| `dpc.c` (listas del RDP del CPU) | 608 | común |
| biblioteca de C, arranque | ~3 100 | común |
| `s2dex.c` (S2DEX2 en la tarea, `-Os`) | 5 440 | F3DEX2 |
| libgcc (`__muldi3`, `__moddi3`, `__udivdi3`, `__umoddi3`) | 5 328 | F3DEX2 (solo por `s2dex.c`) |
| `f3dexbg.c` (Conker) | 4 456 | F3DEX2 |
| `f3dex2.c` (F3DEX2, F3DZEX2) | 3 088 | F3DEX2 |
| `flx.c` (F3DFLX de F-Zero X, `-Os`) | 848 | F3DEX2 |
| `f3d.c` (F3D, F3DEX 1.x, F3DWR) | 4 112 | F3D |
| `f3ddkr.c` (DKR, JFG, Mickey, `-Os`) | 3 256 | F3D |
| `f3dpd.c` (Perfect Dark, `-Os`) | 1 344 | F3D |

El bss: `gfx_state` 40 032 bytes con la caché de 768 entradas (el directorio
son 28 bytes por entrada más el índice de 4 KB: 768 → 25,6 KB, 1024 → 32,7
KB) y los búferes de DMA de `io_spu.c` 98 352 (texturas 2 × 32 KB, vértices
2 × 8 KB, comandos 2 × 4 KB, lista 2 × 1 KB, lecturas 5,8 KB). Las
familias propias suman 13 832 bytes de la segunda generación (con libgcc,
19 160) y 8 712 de la primera; todo lo demás es común.

## Qué mezcla cada juego

`salsa64-run` cuenta al final las tareas gráficas por microcódigo y los
cambios de imagen que habría en la PS3 (línea «gfx por microcódigo»). Con
los 68 respaldos del autor (unos 50 juegos distintos; cada uno 4·10⁹ ciclos,
~43 s de consola, y las escenas largas de la regresión):

- **Ningún juego mezcla generaciones** (primera: F3D, F3DEX 1.x, F3DWR,
  F3DPD, F3DDKR, F3DJFG; segunda: F3DEX2, F3DZEX2, F3DEXBG) entre tareas: 0
  cambios de imagen en todos.
- **Dentro de una tarea**, lo que se mezcla es siempre de la segunda
  generación y de la misma familia: S2DEX2 en F3DEX2 (Zelda OoT: 957 fondos
  en la casa de Link; Kirby 64: 308) y F3DFLX en F3DEX2 (F-Zero X). Un
  G_LOAD_UCODE de la primera generación solo salta el tramo ajeno (`f3d.c`);
  el de Conker no cambia de microcódigo (`f3dexbg.c` lo consume: enciende la
  iluminación avanzada), así que S2DEX2 y F3DFLX nunca corren en Conker.
- **Entre tareas**: Resident Evil 2 manda F3DEX2 y S2DEX sueltos (1217 y 3649
  tareas en 43 s) y Yoshi's Story solo S2DEX: el S2DEX suelto va en la imagen
  de F3DEX2. Rogue Squadron e Indiana Jones (Factor 5) mandan solo su
  microcódigo (hoy «desconocido»): pueden ir en una imagen propia.

## Partición

| Imagen | Programa | Familias | Juegos medidos |
|---|---|---|---|
| 0 «F3D» | `spu_gfx0` | F3D, F3DEX 1.x, F3DWR, F3DPD, F3DDKR, F3DJFG | SM64, GoldenEye, Perfect Dark, MK64, Wave Race, Star Fox 64, Banjo-Kazooie, Diddy Kong Racing, Jet Force Gemini, Mickey's Speedway, Killer Instinct Gold, Doom 64, 1080, FIFA 99… |
| 1 «F3DEX2» | `spu_gfx1` | F3DEX2, F3DZEX2, S2DEX2 y F3DFLX en la tarea | Zelda OoT y MM, Kirby 64, F-Zero X, Smash, Banjo-Tooie, DK64, Paper Mario, Pokémon Snap y Stadium, Mario Party, RE2 (sus tareas F3DEX2)… |
| 2 «Conker» | `spu_gfx2` | F3DEXBG (sobre F3DEX2) | Conker's Bad Fur Day |

Conker va aparte porque su variante (4,5 KB) le quitaba a la imagen de
F3DEX2 justo el lugar que necesita el S2DEX suelto; no comparte tareas con
nadie y su imagen no trae S2DEX2 ni F3DFLX (que nunca corren con él).

**Números de cada imagen** (`spu-size` y `tools/spu-stack.py` en el build,
2026-10-17; el texto de `s2dex.c` ya en 32 bits, ver abajo):

| Imagen | Texto | Datos + bss | Hasta `_end` | Caché | Cadena más honda | Holgura | Sobre el mínimo |
|---|---:|---:|---:|---:|---:|---:|---:|
| antes (todo) | 111 152 | 800 + 138 640 | 250 592 | 768 | 9 520 | 1 840 | 304 |
| 0 F3D | 92 256 | 800 + 145 808 | 238 992 | 1024 | 9 056 | 14 080 | 12 544 |
| 1 F3DEX2 | 92 840 | 800 + 145 808 | 239 632 | 1024 | 9 472 | 13 024 | 11 488 |
| 2 Conker | 91 520 | 800 + 145 808 | 238 352 | 1024 | 8 992 | 14 784 | 13 248 |

Las tres recuperan las 1024 entradas de la caché de texturas. La cadena
más honda sigue siendo la de siempre en la imagen de F3DEX2 (fondo S2DEX →
TEXRECT → estado → combiner); en la de F3D es un triángulo crudo del RDP por
G_RDPHALF (DKR) y en la de Conker un G_TRI4. En RPCS3, al cargar cada
imagen después de sus tareas de prueba, la pila que el SPE nunca tocó es algo
mayor que la holgura del build (14 736, 14 256 y 15 536 bytes, contando
palabras en cero): el grafo de llamadas del build es una cota.

**Hueco para lo que falta** (lo que queda sobre el mínimo de 1536 bytes):

- **S2DEX suelto** (RE2, Yoshi's Story) en la imagen 1: 11 488 bytes con
  la caché en 1024; con 896 entradas, 3 584 más, y con 768, 7 168 más
  (`GFX_SPE0_TC_1` en `spe0.h`). Lo que comparte con S2DEX2 (`s2dex.c`: los
  fondos, `G_OBJ_LOADTXTR`) ya está en la imagen.
- **Factor 5** (Rogue Squadron, Indiana Jones, Naboo) en una imagen nueva
  con solo lo común: la imagen 0 sin sus 8 712 bytes de la primera
  generación deja ~21 KB sobre el mínimo con la caché en 1024 (~28 KB con
  768).
- **Acclaim, ZSortBOSS, Turbo3D, F3DAM**: en la imagen 0 si son de la
  primera generación (12,5 KB libres), o en imágenes propias.

### Los 64 bits de `s2dex.c`

El cálculo de las tiras de los fondos (`bg_draw`) iba en `uint64_t`: con
eso el SPE traía 5 328 bytes de libgcc (multiplicación, división y módulo
de 64 bits) que solo usaba S2DEX2. Los valores caben en 32 bits (imageX <
2048 texels, frameX ≥ −8192 y frameW < 16 384 píxeles, la escala < 65 536 y
la imagen ≤ 1024 texels: nada pasa de 2³¹), así que ahora va en 32 bits:
las imágenes de la regresión salen iguales y la imagen 1 recupera esos
bytes.

## Cuándo se decide y cuánto cuesta cambiar

**Se decide por tarea, con recarga** (`need_image` en `gfx_spu.c`): cada
tarea pide la imagen de su familia (`gfx_spe0_image`) y, si no es la
cargada, se cambia antes de mandarla. Al abrir el juego se carga la imagen 0
(así se sabe enseguida si el SPE arranca; si no, la HLE va en el PPE como
antes) y la primera tarea de un juego de la segunda generación la cambia.
Las listas del RDP del CPU y los estados guardados usan la imagen que haya
(el RDP es común a todas). Como ningún juego medido mezcla imágenes entre
tareas, en la práctica hay **un cambio por juego**, al empezar, y ninguno
después; la recarga por tarea queda para lo que no se midió.

El otro camino, decidir al abrir el juego por la ROM, no sirve solo: el
microcódigo se conoce con la primera tarea, no por la ROM (hay versiones del
mismo juego con microcódigos distintos), y un juego que mezclara familias
quedaría sin HLE para unas tareas.

**Un cambio de imagen** (`load_image`):

1. Con la tarea en vuelo ya recogida, el SPE copia a la memoria principal el
   tramo del estado del RDP (registros, tiles y TMEM: el mismo de los
   estados guardados, < 16 KB) y las color images dibujadas (128 bytes).
2. Se para el trabajador de spukit y se arranca el de la otra imagen
   (`sysSpuImageImport` y un grupo de un hilo: el lv2 la carga al local
   store).
3. La imagen nueva se comprueba contra el PPE (hash de las texturas y sus
   tareas de prueba, abajo) y una tarea vacía con `GFX_JOB_RESET` borra lo
   que dejó la prueba.
4. El SPE trae el estado del RDP y las color images, y el directorio de la
   caché de texturas queda vacío con una **barrera**: mientras el consumidor
   no termine la última tarea de la imagen anterior (`job->done_task`), las
   texturas nuevas van al arena de la tarea y no a la región, donde el RSX
   puede estar leyendo las de antes (`gfx_tc_fence`, `texcache.c`).

Lo que se pierde: el directorio de la caché (las texturas se decodifican de
nuevo en las primeras tareas) y la memoria del último combiner; lo demás es
del RSP, que empieza de cero en cada tarea. En RPCS3 (no concluyente) un
cambio con el SPE ya compilado tarda **3,2 ms** (guardar y parar 1,8,
arrancar 0,35, comprobar 1,0); la primera vez que RPCS3 ve una imagen,
segundos (compila el programa del SPE). En la PS3 real el log de cada
juego da el tiempo de la carga y de cada cambio. Con un cambio por juego, el
costo es de una vez; si un juego alternara imágenes cada cuadro serían
~3 ms + texturas de nuevo por cambio, y entonces hay que juntar sus familias
en una imagen.

El ELF crece con las tres imágenes embebidas: 2 045 000 → 2 241 624 bytes
(+197 KB; los `.bin` de las imágenes son 100 329, 101 048 y 99 675 bytes
contra 140 826 del programa único). La comprobación del arranque usa 0,5 MB
estáticos del PPE (RDRAM de mentira de 128 KB, dos juegos de salidas y una
`gfx_state`).

## Autoprueba al cargar cada imagen

Como el audio y el JPEG: `src/rsp/gfx/selftest.c` arma una display list de
mentira por microcódigo (viewport, matrices, vértices, triángulos, LOADBLOCK
y LOADTLUT, RGBA16 y CI4, un combiner de un ciclo y uno de dos con dos
texturas, TEXRECT en un ciclo y en copia, borrado de z y relleno; en la
segunda generación, un tramo S2DEX2 con un G_BG_COPY) y cada microcódigo de
la imagen se ejecuta en el SPE y en el PPE. La salida debe ser la misma: los
contadores, las texturas y la caché exactos; los flotantes de los vértices y
de los dibujos (`xf`, términos `k`) a lo más 64 pasos, o los dos casi cero (la
división del SPE deja a veces `0x00000001` donde el PPE da 0). El log dice
«tareas de prueba iguales al PPE» o las palabras que difieren.

`tests/unit/test_gfx_selftest.c` comprueba en el host que esas listas pasan
por lo que dicen (5 o 7 triángulos, 4 o 5 rectángulos, 2 o 3 texturas, el
fondo) sin errores, y la comparación con tolerancia.

Con el archivo `/dev_hdd0/salsa64/spe0-todas`, al arrancar el juego se
cargan y comprueban las tres imágenes una tras otra (con el cambio entre
ellas y su costo) antes de seguir con la imagen 0: así se prueban en RPCS3
con las ROMs de prueba, que solo usan una familia
(`tools/rpcs3-run.py build/salsa64/salsa64.elf --embedded gfxtest --bandera spe0-todas`).

## Lo que dice el log

- Al cargar: `SPE 0: imagen F3DEX2 (spu_gfx1, … bytes del ELF) … para gbi 1:
  local store … de 262144 bytes hasta _end, caché de texturas 1024 entradas,
  pila: la cadena más honda … y holgura … (build), … sin tocar al cargar;
  hash igual al del PPE, tareas de prueba iguales al PPE`.
- Al cambiar: `SPE 0: cambio de imagen F3D → F3DEX2 en la tarea N … ms
  (guardar y parar, arrancar, comprobar, estado); estado del RDP llevado,
  caché de texturas vacía con barrera hasta la tarea M`.
- Antes de cambiar y al cerrar: los bytes de pila que el SPE no tocó nunca
  con esa imagen (`GFX_JOB_INFO`, medidos en el SPE): las palabras que siguen
  en cero desde `_end` (aproximado) o, con el archivo
  `/dev_hdd0/salsa64/spe0-pila`, las que siguen con el patrón con que el SPE
  llena su pila libre al arrancar (exacto).

## Regla para agregar un microcódigo

1. **Medir qué mezcla el juego.** `salsa64-run` con el respaldo y una escena
   larga: la línea «gfx por microcódigo» dice qué familias manda entre
   tareas y cuántos cambios de imagen habría. Dentro de una tarea, lo que
   carga con G_LOAD_UCODE (`--trace-rsp`, el log de la HLE).
2. **Elegir la imagen.** Lo que el juego mezcla dentro de una tarea va en la
   misma imagen que su familia (es código de esa familia: un G_LOAD_UCODE
   no puede cambiar de imagen). Lo que mezcla entre tareas, también, salvo
   que el cambio sea raro (al empezar o al cambiar de modo de juego). Si no
   mezcla nada, la imagen con más hueco o una nueva.
3. **Código.** Un `GFX_PART_*` nuevo en `spe0.h`, en el `GFX_SPE0_PARTS_<n>`
   de su imagen; su despacho en `dl.c` (o el G_LOAD_UCODE que lo carga)
   detrás de `GFX_HAS(…)`; el `gbi` en `gfx_spe0_image`. Lo que el código común
   llame solo para esa familia (como `gfx_flx_*` desde `geom.c` y `rdp.c`),
   también detrás de `GFX_HAS`, para que las demás imágenes no lo enlacen.
   Sin `uint64_t` en lo que corre en el SPE si se puede: la aritmética de 64
   bits trae libgcc (5,3 KB).
4. **Una imagen nueva**: `GFX_SPE0_IMAGES`, `GFX_SPE0_PARTS_<n>`,
   `GFX_SPE0_TC_<n>` y su nombre en `spe0.h`; `GFX_IMAGES` y
   `gfx<n>_STACK_ARGS` en `src/frontend/Makefile`; su entrada en `images[]`
   de `gfx_spu.c` (con `spu_gfx<n>_bin.h` y `spu_gfx<n>_stack.h`).
5. **Autoprueba.** El `gbi` en la lista de `check_lists` (`gfx_spu.c`) y, si
   su codificación no es la de F3D ni la de F3DEX2, en
   `gfx_selftest_build`.
6. **Probar que la imagen basta.** Compilar `salsa64-run` con solo esa
   imagen (`make -C src/host OUT=$PWD/build/host-img<n> CC=cc SAN=
   EXTRA_CFLAGS=-DGFX_SPE0_IMG=<n>`) y comparar las imágenes de `--gfx-frame`
   (y `--gfx-frame-every`) de sus juegos con las del `salsa64-run` normal: deben
   ser iguales byte a byte. En RPCS3, `spe0-todas`: la imagen arranca y sus
   tareas de prueba dan lo mismo que el PPE. El build falla si la holgura de
   pila baja de 1536 bytes; si no cabe, bajar `GFX_SPE0_TC_<n>` (128
   entradas son 3 584 bytes) o pasar código a otra imagen.

## Regresión del 2026-10-17

**Host** (respaldos del autor, fuera del repositorio): cada escena con
`--gfx-frame` al final y `--gfx-frame-every` (el md5 de cada imagen),
`salsa64-run` de main e22e10c contra el de la rama (caché de texturas de 1024
en el host, el conteo de microcódigos y `s2dex.c` en 32 bits) y contra uno
compilado **con solo la imagen de cada juego** (`-DGFX_SPE0_IMG=<n>`):

| Juego | Escena | Imágenes | Imagen del SPE 0 | main = rama = sola su imagen |
|---|---|---:|---|---|
| GoldenEye 007 | `goldeneye-dam.pad`, 6000 campos | 12 | F3D | sí |
| Perfect Dark | 3000 campos | 12 | F3D | sí |
| Diddy Kong Racing | `dkr-pista.pad`, 10 000 campos (6500, 8500 y 9500 entre ellas) | 21 | F3D | sí |
| Jet Force Gemini | `jfg-3d.pad`, 14 000 campos | 28 | F3D | sí |
| Mickey's Speedway USA | `mickey-semaforo.pad`, 10 000 campos | 17 | F3D | sí |
| Wave Race 64 | 3000 campos | 12 | F3D | sí |
| Mario Kart 64 | `mk64-luigi.pad`, 6000 campos | 12 | F3D | sí |
| Super Mario 64 | `sm64-castillo.pad`, 4300 campos | 9 | F3D | sí |
| Banjo-Kazooie | 3000 campos | 12 | F3D | sí |
| Killer Instinct Gold | `killerinstinct-pelea.pad`, 3000 campos (listas del CPU) | 22 | F3D | sí |
| Zelda OoT | `zelda-casa-nueva.pad`, 15 500 campos (957 fondos de S2DEX2) | 31 | F3DEX2 | sí |
| Kirby 64 | 3000 campos (308 fondos) | 12 | F3DEX2 | sí |
| F-Zero X | 3000 campos | 12 | F3DEX2 | sí |
| Conker's Bad Fur Day | 3000 campos | 12 | Conker | sí |

212 imágenes iguales entre main y la rama, y las 145 + 55 + 12 de cada juego
con solo su imagen, iguales a las de main: cada imagen basta para sus juegos.
En todas, 0 cambios de imagen.

**Pruebas:** `tools/build.sh test-host` 613/613 (las 3 nuevas de
`test_gfx_selftest.c`); `tools/test-roms.sh` rc 0; `tools/build.sh` (todos
los programas) rc 0; `tools/test-rec.sh` rc 1 solo por `menu_scan_dir` bajo qemu (como en main): 613 pruebas en ppc64 en los cinco modos, nemu64-test en 433 fallas (pin+eager también), fastmem en 440, vitest, estados y perfilado en verde.

**RPCS3** (no concluyente para tiempos): con el archivo `spe0-todas` las tres imágenes
arrancan y sus tareas de prueba dan lo mismo que el PPE, y el cambio entre
ellas lleva el estado; gfxtest (F3DEX2: cambio F3D → F3DEX2 en la primera
tarea, 3,4 ms), sus variantes F3D y F3DEX (imagen 0), vitest, audiotest y el
menú (gfxtest, audiotest y gfxtest F3D abiertos y cerrados en orden, cada uno
con su imagen) en verde. `--estados` con gfxtest falla igual en main («la ROM
terminó antes de cargar el estado»): no es de esta rama.

### Un fallo del recompilador de SPU de RPCS3

Con la pila libre llena de un patrón al arrancar (para medirla), en algunos builds la imagen de F3D
corría mal en RPCS3: las tareas de prueba daban el doble de comandos que el
PPE (126 contra 62) y las de gfxtest F3D se cortaban por bucle (un millón de
comandos, 265 ms por tarea). Cambiaba con el build aunque los bytes de las
imágenes del SPE fueran los mismos (solo se movía la memoria del PPE). El
mismo ELF que fallaba **pasaba con el intérprete de SPU de RPCS3**, y en la
misma disposición pasaba con la pila en ceros: no es del código. Con
`0x00001A5A` (un `stop` del SPU) fallaba igual. Por eso la pila **no se
llena** salvo con el archivo `spe0-pila` (para medir en la PS3 real); sin él
se cuentan las palabras en cero, como estaba el local store en main. Si
alguna vez se ve algo parecido en RPCS3, probar con `SPU Decoder:
Interpreter (dynamic)` antes de buscar en la HLE.
