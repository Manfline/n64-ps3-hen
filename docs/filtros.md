# Filtros de la presentación

Los filtros CRT de Salsa64 son **filtros originales del autor, portados de
sus versiones de RetroMac** (sus apps para Mac y Apple TV): el CRT de
máscara de sombra (v1.3), el CRT con cable (v1.1: euroconector RGB,
S-Video o compuesto NTSC hacia una Trinitron chica) y el CRT Trinitron (un
Sony KV-29 de la sala, sin la señal de un cable: el preset `kv29` de
RetroMac, la v6 de su Trinitron de Shadertoy sin los alambres
amortiguadores); la máscara tiene además una densidad «Fina», con los
puntos más chicos y más juntos (pedido del autor). Van en la parte GPL
(`src/frontend/filters/`), sobre la maquinaria genérica de posprocesado de
`lib/rsxkit` (MIT, `rsxkit/post.h`), que no sabe nada de estos filtros.

Se eligen en la fila «Filtro» de Opciones (y en los ajustes por juego),
después de los filtros sencillos de VIDEOGAMES SCZ, con el crédito
«Filtro original del autor» (`MENU_FILTER_CRT_*` en menu.h; claves
`crt-mascara|crt-rgb|crt-svideo|crt-compuesto|crt-trinitron`; la densidad de la
máscara, en su fila «Densidad», que solo aparece con ella), y en vivo en
«Imagen» del menú de pausa. Son experimentales: la fila dice «(exp.)» y la
ayuda «(experimental)». Con un CRT la escala interna no pasa de 3× (lo
elegido menor se respeta; ver «Costo en el RSX»). `load_view` de main.c
pasa el elegido a `filter_setup` al abrir el juego, y `filter_select`
lo cambia en vivo (las tablas se rehacen solas en el cuadro siguiente; si
eso va a tardar más de un par de cuadros, ese cuadro sale sin filtro con el
aviso «Aplicando filtro…» y la preparación va en el siguiente).
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
| reducir (rsxkit) | imagen del juego | la imagen a escala interna, de vuelta a la resolución del juego con un filtro de caja: en dos pasadas, a lo ancho (3 lecturas bilineales, sobre una tabla fp16 de una fila por fila de la imagen grande) y a lo alto; con `filtros-reducir` en 1, en una (3 × 3 lecturas) | 27 (9 lecturas) en una pasada |
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

Densidad «Fina» de la máscara (`SALSA64_CRT_MASK_FINE`, preset
`crt_mask_monitor_fine`, `pitch` 0,75; clave `crt-mascara-fina` para el
archivo `filtro` y `filtros-ciclo`): la misma cuenta con la separación de los
puntos por 0,75 en píxeles enteros, desde 2 px (a 720p y 1080p con 320 de
ancho: D = 3, filas de 3, radio 1,2, en lugar de 4, 3 y 1,6; tríadas de 9 px
en vez de 12). Se compararon 4, 3, 2,5, 2 y 1 px con `tools/crt-ref.c`
(`CRT_REF_D`): a 2,5 px la red no cae en píxeles enteros y se ven franjas (y
la tesela necesita 3D entero); a 2 px los puntos son bloques de ~2 × 2 px y
se pierden las tríadas redondas; a 1 px, con las filas corridas medio
píxel, ni siquiera se ven puntos. El autor eligió 3 px. La tesela da el
punto más cercano exacto también con D = 2 y 3 (prueba en el host). Con D
impar las filas impares van corridas 1,5 px y sus puntos quedan con el
centro en un borde de píxel, no en una esquina: el promedio de la máscara
(lo que conserva el brillo) es el de las dos clases de punto y el pico, el
mayor; sin eso la fina salía un 2 % más oscura.

CRT Trinitron: el mismo tubo, sin señal ni decodificación. Las líneas A y B
leen la luz lineal del juego: en cada columna, 5 texeles de la scanline con
peso gaussiano (el desenfoque del haz a lo ancho, `h_blur` 0,40) y la
saturación (1,05), sin bucle en el programa (Cg dejaba el de 5 vueltas con
un salto condicional, que cgcomp no ensambla). La pasada final es la del
cable, con las constantes del KV-29 (haz 0,22–0,62, resplandor 0,15, brillo
1,10).

Con el archivo `filtros-spe`, **la señal y la decodificación de S-Video y
compuesto van en dos SPEs** (src/spu/crt/main.c, protocolo en
`src/frontend/filters/crt_spe.h`). El RSX copia la imagen reducida a la
memoria principal y escribe una etiqueta; cada SPE la espera, hace la mitad
de las líneas y escribe la suya; mientras, el RSX hace lineal y resplandor,
y antes de las líneas espera las dos etiquetas y pasa la tabla del SPE (R,
G, B antes de la gamma, fp16) a luz lineal con la saturación, a su memoria
local («linealizar», 12 instrucciones). En el SPE van 4 líneas en los 4
carriles de cada vector: las 27 muestras del filtro de la tele son una FMA
por toma y canal, con los pesos en registros (~27 ciclos por muestra).
Resplandor y líneas se quedan en el RSX (en el SPE costarían más de lo que
ahorran: docs/perf-log.md). Si un SPE devuelve error o no termina en 300 ms
después de recibir la imagen, el PPE escribe su etiqueta para soltar al RSX
y la señal vuelve al RSX por el resto de la corrida. El log dice cuánto
esperó el RSX, la ida y vuelta (de la copia a pasar la espera) y el trabajo
del SPE más lento. En RPCS3, contra el original con el resplandor del RSX:
S-Video 0,23/255 de promedio y compuesto 0,24 (como en el RSX).

Cuando la imagen no cambió (el juego dibuja 20 o 30 cuadros por segundo y
el mismo se presenta en dos o tres campos), solo se hace la pasada final:
las tablas siguen valiendo. Lo sabe por un número de cada destino del RDP
que cambia con cada dibujo, carga desde la RDRAM o creación
(`salsa64_rdp_present_source`); el framebuffer 2D (la CPU) se filtra
siempre entero. Los cuatro filtros son fijos para una imagen fija (el
compuesto va con `frame_phase` 0).
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
primera vez que dibuja cada filtro en cada corrida (el log dice
«comprobación en el RSX»; unas decenas de ms, bajo el aviso «Aplicando
filtro…»).

Con el archivo `filtros-capturas` en `/dev_hdd0/salsa64`, la primera vez
de cada filtro y alto de pantalla la app guarda la imagen que entró
(`filtro-<clave>-<alto>-fuente.ppm`) y la que dibujó, con la pantalla
entera en negro alrededor (`-pantalla.ppm`). El RSX vuelve a dibujar la
pasada final en una superficie de la memoria principal y copia ahí la
fuente; un hilo aparte espera su barrera y escribe los archivos. Antes se
leían desde el PPE la pantalla y la fuente en la memoria del RSX, y la
imagen se quedaba quieta ~2 s. En RPCS3 la pantalla de la primera captura
de cada corrida sale negra (la fuente no, y las siguientes salen bien,
también dibujando antes una vez en esa memoria): para verificar ahí un
filtro, ponerlo segundo en la lista de `filtros-ciclo`
(`crt-rgb crt-trinitron`).

En RPCS3 (2026-10-13, la ROM de prueba como juego, 720p, escala 5× con el
tope de 3×, reducción en dos pasadas), contra el original con el
resplandor del RSX: máscara y Trinitron a 1/255 o menos en todo píxel
(promedio 0,02); RGB 0,12 (máximo 36), S-Video 0,27 (64) y compuesto 0,24
(45), como antes.

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

Medido en la PS3 real con GoldenEye (docs/perf-log.md, «Filtros CRT en el
RSX»). Primero a escala 5×: máscara 6,7 / 9,6 ms por cuadro a 720p /
1080p, RGB 8,4 / 11,9, S-Video 9,6 / 13,0 y compuesto 11,9 / 15,4; a
1080p se saltaban cuadros. Con el tope de 3× y la reducción en dos pasadas,
a 1080p: máscara 7,4, RGB 9,4, S-Video 10,6, compuesto 13,2 y Trinitron
10,1, sin saltar ningún campo; la señal del cable en el SPE, por ahora, no
ahorra (S-Video −0,45 ms, compuesto +0,5). Las pasadas siguen
las instrucciones por píxel de sus programas, salvo la reducción: 3,1 ms en
todas por leer la superficie de 5× (7,6 MB). Lo que va por píxel de salida
(el final, los puntos, las líneas) crece 2,25 veces de 720p a 1080p; lo que
va sobre la imagen chica (lineal, resplandor, señal y decodificación) no.
Las propuestas para bajarlo (escala 2× con un CRT, no rehacer el filtro en
los campos repetidos, la señal del cable en un SPE) y sus estimados están
en el mismo lugar.
