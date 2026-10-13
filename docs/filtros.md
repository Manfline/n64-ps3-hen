# Filtros de la presentación

Los filtros CRT de Salsa64 son **filtros originales del autor, portados de
sus versiones de RetroMac** (sus apps para Mac y Apple TV): el CRT de
máscara de sombra (v1.3) y el CRT con cable (v1.1: euroconector RGB,
S-Video o compuesto NTSC hacia una Trinitron chica). Van en la parte GPL
(`src/frontend/filters/`), sobre la maquinaria genérica de posprocesado de
`lib/rsxkit` (MIT, `rsxkit/post.h`), que no sabe nada de estos filtros.

Se eligen en la fila «Filtro» de Opciones (y en los ajustes por juego),
después de los filtros sencillos de VIDEOGAMES SCZ, con el crédito
«Filtro original del autor» (`MENU_FILTER_CRT_*` en menu.h; claves
`crt-mascara|crt-rgb|crt-svideo|crt-compuesto`). `load_view` de main.c
pasa el elegido a `filter_setup` al abrir el juego, y `filter_select`
lo cambia en vivo (las tablas se rehacen solas en el cuadro siguiente).
El archivo `filtro` de `/dev_hdd0/salsa64` manda sobre el menú, para
pruebas. Las capturas de pantalla, las miniaturas de los estados y las
pruebas automáticas siguen sin filtro.

## Cómo se dibujan en el RSX

Los shaders originales calculan todo en cada píxel de pantalla. En RetroMac
ya van repartidos en pasadas: lo que no cambia de un píxel al vecino se
calcula una vez en una tabla. En el RSX (familia NV40) el costo lo manda el
número de instrucciones por píxel, así que se llevó más lejos:

| Pasada | Tamaño | Qué hace | Instrucciones |
|---|---|---|---|
| reducir (rsxkit) | imagen del juego | la imagen a escala interna, de vuelta a la resolución del juego con un filtro de caja (3 × 3 lecturas bilineales) | 27 (9 lecturas) |
| lineal | imagen del juego | gamma 2,4 → luz lineal, una vez por píxel del juego | 12 |
| resplandor | imagen × 1 o × 2 | las 12 lecturas en espiral, en una rejilla que la pasada final interpola | 31 (12) |

Máscara de sombra:

| Pasada | Tamaño | Qué hace | Instrucciones |
|---|---|---|---|
| tesela (×3) | 3D × 2H (12 × 6) | para cada píxel de la tesela que se repite en la red delta: el punto más cercano de cada color (su celda en la tabla de puntos) y cuánto lo cubre | 56 |
| puntos | un texel por punto de fósforo (~250 × 245 a 720p) | la energía del haz en su canal (dos scanlines con su desenfoque, haz integrado sobre la franja con erf) y su apertura, guardadas como `color = A · cobertura + B` | 146 (10) |
| final | la imagen en pantalla | 3 teselas, 3 puntos, el resplandor, el velo y la gamma 2,2 | 33 (7) |

CRT con cable:

| Pasada | Tamaño | Qué hace | Instrucciones |
|---|---|---|---|
| señal | muestras de cada línea (4 por ciclo de subportadora, ~710) × líneas | la señal sostenida y promediada sobre el tramo (una lectura bilineal), en luma y color sobre la subportadora | 31 |
| decodifica | igual | los filtros de la tele (9 muestras con RGB, 27 con S-Video y compuesto), a RGB, a luz lineal | 49 / 103 |
| líneas A y B | columnas de la imagen × líneas | la scanline en esa columna, interpolada, ya como la usa el haz (amplitud con la apertura de la máscara de franjas; k = 1/2σ²) | 51 + 26 (58 + 31 con dos tablas) |
| final | la imagen en pantalla | el haz de las dos scanlines más cercanas, el resplandor y la gamma | 42 (5) |

Lo que queda distinto del original:

- **La fuente es la imagen reducida a la resolución del juego** (filtro de
  caja sobre la escala interna: con la escala 5× cada píxel promedia 25),
  que es lo que muestra un CRT; el original toma la imagen de la consola.
- **El resplandor sale de una rejilla** de 1 o 2 puntos por píxel del juego
  (como la rejilla del resplandor de RetroMac) y promedia la luz ya lineal
  con el filtro bilineal del RSX; el original interpola los valores
  codificados en cada píxel. Fuera de la imagen lee 0 con el modo BORDER
  (el original, 0 fuera y el borde repetido en el último medio texel).
- **El cable se decodifica en muestras fijas** de cada línea y la pasada de
  líneas interpola; el original decodifica en cada columna de pantalla. El
  compuesto usa dos tablas corridas medio paso (8 muestras por ciclo) para
  que el dot crawl no se aplane al interpolar.
- Tablas intermedias en fp16.
- El ruido del cuadro (`FRAME_PHASE`) va en 0, como en los presets: el
  patrón del compuesto no se mueve.

## Verificación

- La tesela de la máscara se verificó en el host contra el cálculo original
  del punto más cercano en cada píxel de pantallas enteras
  (`tests/unit/test_crt.c`).
- `tools/crt-ref.c` es el filtro original píxel por píxel en doble
  precisión; con el sufijo `-rsx` (`mascara-rsx`…) hace el resplandor como
  el RSX (rejilla con la luz lineal), para separar esa aproximación del
  resto. Se compara contra la pantalla con filtro (`capturas-pantalla`, o
  los `filtro-<clave>-<alto>-pantalla.ppm` que la app guarda la primera vez
  que dibuja cada filtro en cada resolución, con la imagen que le entró en
  `-fuente.ppm`):

      cc -O2 -o build/crt-ref tools/crt-ref.c -lm
      build/crt-ref mascara-rsx fuente.ppm 1280 720 ref.ppm
      build/crt-ref compara ref.ppm pantalla.ppm diferencia.ppm

En RPCS3 (gfxtest como juego, escala 1×, 720p; 2026-10-13), diferencia por
canal en el rectángulo de la imagen:

| Filtro | contra el original | contra el original con el resplandor del RSX |
|---|---|---|
| Máscara de sombra | promedio 0,54/255, máximo 18 | 0,03, máximo 1 (100 % a 1 o menos) |
| Cable RGB | 0,65, máximo 43 | 0,13, máximo 36 (98,5 % a 1 o menos) |
| Cable S-Video | 0,74, máximo 65 | 0,26, máximo 64 (96,5 %) |
| Cable compuesto | 0,72, máximo 45 | 0,25, máximo 45 (96,6 %) |

Lo que queda en los cables viene de interpolar las muestras de la señal
entre columnas (bordes nítidos); en la máscara, nada más que el
resplandor. RPCS3 traduce el microcódigo del RSX a GLSL: la PS3 real puede
diferir en el filtrado de fp16 y el modo BORDER; la app lo comprueba sola la
primera vez que dibuja cada filtro (el log dice «comprobación en el RSX»).

## Errores de cgcomp que se cruzaron

Los filtros sacaron a la luz tres errores del ensamblador de cgcomp (y uno
de la API de PSL1GHT), anotados en `docs/rsx-notes.md`: los códigos de
condición (`?:`) no se ensamblan bien; el color de salida se escribe en R0
aunque Cg lo use de temporal; y `|R0.z|` sale como `|R0.zxxx|`. Además Cg
corre en silencio y `lerp(1.0, vector, k)` trunca el vector. El build de los
filtros (`FPO_CHECK` en mk/ps3.mk) corrige en el ensamblador el choque del
color de salida y el valor absoluto antes de reensamblarlo
(`tools/cg-salida.py`) y falla si quedan códigos de condición
(`tools/fpo-info.py --sin-condiciones`).

## Costo en el RSX

Medido con marcas de tiempo del RSX entre pasadas (`rsxkit_stamp`); el log
de la PS3 lo escribe cada 5 s («rsx: filtro …»). El archivo `filtros-ciclo`
pasa por todos los filtros cada 20 s para medirlos en una sola corrida.

Pendiente de la primera medición en la PS3 real (ver docs/perf-log.md).
