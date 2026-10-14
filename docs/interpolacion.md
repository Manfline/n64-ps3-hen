# Interpolación de cuadros (diseño y prototipo)

Estado: **diseño con prototipo en el host** (2026-10-13). El **tope de
cuadros** (paso 1) se midió en la PS3 y quedó en pausa (abajo). Siguiente
paso: Zelda OoT por geometría, sin tope (plan al final).

Objetivo: que los juegos cuya lógica va a 20–30 cuadros por segundo se vean
a 60, sin cargar más el CPU emulado (el cuello de botella), con el trabajo
en los SPE y el RSX. Un interruptor por juego, apagado por omisión, que
apagado no cueste nada ni cambie un píxel.

Se diseñaron y probaron dos caminos con las mismas escenas:

- **Por geometría**: la HLE ve cada vértice; se empareja con el mismo
  vértice del cuadro anterior del juego y la lista del cuadro nuevo se
  vuelve a dibujar con las posiciones interpoladas.
- **Por imagen**: un SPE estima el movimiento entre las dos imágenes
  (búsqueda por bloques) y el RSX deforma la imagen con esos vectores.

## Resumen y recomendación

1. **Por geometría es mucho mejor** en todas las escenas medidas. En
   GoldenEye, con la verdad a la vista (el cuadro que el juego dibujó de
   verdad en ese campo): 30,0 dB contra 24,1 dB por imagen y 19,9 dB de
   repetir el cuadro (hoy) a 20 cuadros; 33,6 contra 24,1 y 20,7 a 30
   cuadros. Sin parches por juego empareja el 97–99 % de los vértices en
   GoldenEye, Zelda, SM64 y Mario Kart (Perfect Dark, con la identidad por
   contenido, 89–99 % en el logo de la intro).
2. **Hallazgo previo que cambia el planteamiento**: en nuestro emulador
   GoldenEye y Perfect Dark no van a 20–30 cuadros sino a ~60 (el RSP y el
   RDP de la HLE terminan casi al instante; en la consola el RDP los
   frena). Por eso GoldenEye en la presa está limitado por el CPU en la PS3
   (52–57 campos/s). Retrasando la interrupción del DP lo que tarda un RDP
   real, GoldenEye baja a 30 cuadros y **el CPU emulado trabaja 48 % menos**
   (Perfect Dark, en el logo de la intro, 28 % menos). Un **tope de
   cuadros** más la interpolación libera el PPE en vez de cargarlo: esa es
   la combinación que más gana en los juegos pesados.
3. Modos del interruptor propuestos: **Apagado**, **Por geometría**
   (interpola, hasta k−1 campos de retraso), **Por geometría, sin retraso**
   (extrapola: 27,0 dB a 20 cuadros, 29,7 a 30; sigue muy por encima de
   repetir) y **Por imagen** (para lo que no pasa por la HLE: juegos con el
   RSP a bajo nivel, imágenes del CPU). Aparte, por juego, **Tope de
   cuadros**: libre, 30 o 20.
4. Orden: Zelda OoT (20 → 60, la mayor ganancia visible, ritmo fijo),
   luego SM64 y Mario Kart (30 → 60), luego GoldenEye y Perfect Dark con el
   tope de 30. Por imagen, en una segunda etapa.

## Cuántos cuadros dibujan los juegos en Salsa64

Medido en el host (`salsa64-run`, respaldos propios, escenas de
`docs/pads/`): cuadros nuevos (cambios de origen del VI o tareas gráficas)
por campos.

| Juego, escena | Cuadros del juego | Notas |
|---|---|---|
| GoldenEye, presa (girando y caminando) | 276 de 279 campos (≈ 60) | con 2 ciclos por instrucción en vez de 1 (`-DSALSA64_CPI=2`): 268 de 279; no es el CPU |
| GoldenEye, DP 1,5 M ciclos después | 276 de 279 | |
| GoldenEye, DP 3 M ciclos después (~32 ms, un RDP lento) | 140 de 279 (30) | CPU en espera 60,5 % → 79,5 %: **trabajo del CPU −48 %**; 7,2 s → 3,9 s de host |
| Perfect Dark, logo de la intro | 268 de 267 (≈ 60) | |
| Perfect Dark, DP 3 M ciclos después | 150 de 298 (30) | CPU en espera 84,0 % → 88,5 %: −28 %; 15,9 s → 10,2 s de host |
| Zelda OoT, juego y escenas | 20 (100 tareas en 300 campos) | ritmo fijo del juego; los menús van a 60 |
| Super Mario 64, camino al castillo | 30 (148 en 294) | |
| Mario Kart 64, Luigi Raceway | 30 (148 en 294) | |

El CPU emulado cobra 1 ciclo por instrucción, pero cobrar 2 casi no cambia
el ritmo de GoldenEye: lo que lo frena en la consola es esperar al RDP. Un
juego de ritmo libre como GoldenEye calcula el movimiento con el tiempo
transcurrido, así que a 30 cuadros se juega igual de rápido, con la mitad
de trabajo de CPU por segundo (el que espera, lo saltan los bucles de espera
de `cpu/idle.c`).

## Cómo se midió (prototipo del host)

- La HLE (con `-DSALSA64_GFX_INTERP`, solo en el build del host) avisa de
  cada grupo de vértices cargado y de cada triángulo que sale; el prototipo
  (`src/host/interp.c`) empareja y guarda, por cada vértice de salida, su
  posición en el cuadro anterior. Para inventar un intermedio se vuelve a
  rasterizar la salida del cuadro nuevo con las posiciones mezcladas, desde
  el estado del rasterizador de antes de esa tarea y sin tocar los destinos
  del juego ni la RDRAM.
- Por imagen: búsqueda por bloques en luma a 320 × 240 y deformación en el
  host, con el mismo algoritmo que iría en el SPE y el RSX.
- **Con la verdad** (`--interp-decimate`): como GoldenEye y Perfect Dark
  dan un cuadro por campo, se toma 1 de cada k tareas como «cuadro del
  juego» y las de en medio son lo que el juego dibujó de verdad en ese
  momento. Cada intermedio se compara con ellas.
- Sin verdad (Zelda, SM64, Mario Kart: ya van a 20–30): cada tarea es un
  cuadro y los intermedios se inventan entre una y la siguiente; se miran a
  simple vista.
- Métricas: PSNR y porcentaje de píxeles con algún canal a más de 24/255,
  y las mismas con las imágenes reducidas a bloques de 4 × 4 (no castigan
  una textura fina corrida un subpíxel, que en movimiento no se ve). Las
  referencias son «repetir» (lo de hoy: el cuadro viejo hasta que llega el
  nuevo) y «fundido» (mezcla de los dos).

### Resultados con la verdad (GoldenEye, presa, 144 y 108 intermedios)

| Método | Como a 20 cuadros (k = 3) | Como a 30 cuadros (k = 2) |
|---|---|---|
| Repetir (hoy) | 19,9 dB, 27,3 % malos | 20,7 dB, 23,6 % |
| Fundido | 21,9 dB, 20,4 % | 22,7 dB, 17,9 % |
| **Geometría, interpolada** | **30,0 dB, 3,7 %** (4×4: 34,9 dB, 5,9 %) | **33,6 dB, 1,7 %** (4×4: 39,3 dB, 4,1 %) |
| Geometría, extrapolada | 27,0 dB, 7,2 % (4×4: 32,4 dB, 9,3 %) | 29,7 dB, 3,3 % (4×4: 35,3 dB, 4,8 %) |
| Imagen, interpolada | 24,1 dB, 9,2 % (4×4: 28,7 dB, 10,5 %) | 24,1 dB, 8,9 % (4×4: 28,4 dB, 10,4 %) |
| Imagen, extrapolada | 23,5 dB, 11,3 % | 24,8 dB, 8,2 % |

Lo que queda mal por geometría son sobre todo cosas del cuadro nuevo que no
estaban en el viejo (chispas de un impacto, el trazo de una bala): se ven
uno o dos campos antes. Por imagen, el 83–87 % de los bloques que fallan
está en un borde del campo de vectores: el arma contra el fondo, el borde
de la pantalla por donde entra lo nuevo al girar, el HUD translúcido.

### Emparejamiento por juego (sin parches)

| Juego, escena | Vértices cargados con pareja | Vértices de salida interpolados | Lo que no |
|---|---|---|---|
| GoldenEye, presa | 98,1 % | 94,0 % | rectángulos del HUD, chispas |
| Zelda OoT, vuelo de Navi | 99,3 % | 95,8 % | franjas negras y texto (rectángulos) |
| Zelda OoT, toda la intro con cambios de escena | 93,8 % | 76,5 % | escenas nuevas (bien: no se interpolan) |
| Super Mario 64, castillo | 96,8 % | 93,7 % | HUD |
| Mario Kart 64, Luigi Raceway | 99,2 % | 90,0 % | HUD (más rectángulos) |
| Perfect Dark, logo de la intro | 89–99 % | 84–98 % | (escena casi quieta: solo vale el emparejamiento) |

Perfect Dark copia sus vértices a uno de dos búferes que alterna en cada
cuadro: por dirección empareja el 0 %; la identidad por contenido (abajo)
lo arregla.

Las animaciones lado a lado quedan en `build/interp/` (fuera de git: son
imágenes de juegos comerciales) con `tools/interp-video.sh` y la página
`tools/interp-index.html`; ver «Reproducir en la Mac».

## Camino por geometría

### Qué se empareja: vértices, no matrices

La salida de la HLE ya está transformada (coordenadas de recorte por la
escala del viewport, `gfx_vertex`). Interpolar las matrices elemento a
elemento y transformar es **exactamente** lo mismo que interpolar las
coordenadas de recorte de cada vértice (la transformación es lineal en la
matriz), así que basta con saber dónde estaba cada vértice en el cuadro
anterior: no hace falta identificar matrices, pilas ni objetos. Interpolar
la matriz elemento a elemento encoge un poco lo que gira mucho entre dos
cuadros (cos(θ/2) a la mitad): a 20 cuadros, girando 180°/s, 9° por cuadro,
0,3 %; descomponer en rotación y traslación (como hace RT64 con sus grupos
de matrices) no hace falta para estos juegos.

Identidad de un vértice cargado (G_VTX):

1. **Dirección** en la RDRAM de su entrada en la lista del juego, más
   **cuántas veces se cargó antes esa misma dirección en el cuadro**: el
   mismo modelo dibujado varias veces (árboles, enemigos iguales, las
   letras) se distingue por orden de aparición. Los modelos viven en la
   memoria del juego (objetos de Zelda, salas, el modelo de Mario): la
   dirección no cambia de un cuadro a otro.
2. De respaldo, si la dirección no estaba: **hash del contenido** de la
   entrada (x, y, z, s, t, color o normal) más sus repeticiones. Lo
   necesita Perfect Dark (doble búfer de vértices) y apenas se usa en los
   demás (1–2 %).

Los vértices que el juego anima escribiendo la lista (agua, banderas)
conservan la dirección: se interpola de dónde estaban a dónde están, que es
lo correcto. Los que el juego arma en un búfer nuevo cada cuadro y además
cambian (pieles de algunos personajes) no encuentran pareja y quedan en su
lugar del cuadro nuevo.

Un grupo (los n vértices de un G_VTX) se interpola **todo o nada**: si a
uno le falta pareja, ninguno se mueve, y una malla no se rasga. Las grietas
que aparecían entre grupos interpolados y vecinos sin interpolar (puntos
del fondo entre las baldosas del piso de la presa) desaparecieron con esta
regla y con el criterio de cortes de abajo.

### Cortes, objetos que aparecen y desaparecen

- **Por vértice**: un grupo cuyo desplazamiento en pantalla pasa de 120
  píxeles de la N64 por cuadro del juego no se interpola (corte de cámara,
  teletransporte). Solo cuentan los vértices cerca de la pantalla
  (|x|, |y| ≤ 1,5 w) en los dos cuadros: uno pegado a la cámara (w chica)
  se mueve cientos de píxeles sin que sea un corte; con el criterio en
  pantalla sin esa banda, la pared que Bond tenía enfrente quedaba sin
  interpolar y la geometría salía peor que la imagen.
- **Por cuadro**: si más de un tercio de los grupos con pareja se movió
  más que eso, o si menos de la mitad de los vértices tiene pareja (otra
  escena con algún modelo en común, como Link), no se interpola nada de ese
  cuadro.
- Un cambio de escena se resuelve solo: los vértices de la escena nueva no
  tienen pareja.
- **Aparecer y desaparecer**: el intermedio dibuja la lista del cuadro
  nuevo. Lo que aparece entra en su lugar (uno o dos campos antes que
  hoy); lo que desaparece se va uno o dos campos antes. No se mezclan
  objetos de dos cuadros.
- **Cruzar el plano cercano**: se interpola en coordenadas de recorte y el
  intermedio se recorta en w > 0 (lo hace el RSX). Con los microcódigos sin
  recorte cercano (Zelda: «NoN») un triángulo que el cuadro original no
  dibujaba (algún vértice detrás de la cámara) tampoco se dibuja en el
  intermedio; si se recortaba, aparecían triángulos enormes que el original
  nunca mostró.

### HUD, rectángulos 2D, texturas que se desplazan, partículas

- **Rectángulos** (TEXRECT, FILLRECT, S2DEX, fondos de Zelda): no tienen
  vértices del juego ni identidad; quedan donde está el cuadro nuevo. El
  HUD casi nunca se mueve, así que es lo correcto; los números (tiempo de
  Mario Kart, balas de GoldenEye) cambian al ritmo del juego.
- **Carteles** (los árboles de SM64, partículas con su propia matriz): su
  cuadrado comparte dirección; se emparejan por orden de aparición y se
  interpolan bien. Si el número de partículas cambia, el orden se corre:
  la mayoría queda fuera por el desplazamiento y se queda quieta un campo.
- **Texturas que se desplazan** (agua, cascadas: la tile corre cada
  cuadro): el prototipo no las interpola (avanzan al ritmo del juego, como
  hoy). Se puede emparejar la tile por formato, máscara y orden del dibujo
  y mezclar su desplazamiento (lo hace RT64), con cuidado al dar la vuelta
  la máscara. Queda para después.
- **Mapeo de entorno** (G_TEXTURE_GEN, el reflejo del arma): s y t salen de
  la normal y la cámara del cuadro nuevo; no se interpolan.
- **Lo que sale de la pantalla**: la HLE descarta los triángulos fuera de
  un mismo plano (y G_CULLDL, listas enteras); lo que en el cuadro nuevo ya
  salió no está en su lista y falta en el borde de los intermedios al girar
  rápido (una franja de pocos píxeles; en la presa no se notó en los mapas
  de error). Arreglo barato: con la interpolación, no descartar un
  triángulo cuyos vértices en el cuadro anterior se veían.

### Cuadro nuevo o repetido

El VI repite el framebuffer mientras el juego no cambia el origen. El hilo
de presentación ya lo distingue (`present_field`: el origen cambia → cuadro
nuevo) y `src/rdp/targets.c` sabe qué destino se presenta y qué tarea lo
dibujó: esa es la lista que se redibuja. Por cada campo repetido se dibuja
un intermedio en t = j/k, con k (campos por cuadro) del último intervalo
entre cuadros nuevos. Si el cuadro siguiente llega antes, se salta a él;
si llega tarde, se queda en t = 1 (como hoy).

### Interpolar o extrapolar

| | Interpolar | Extrapolar |
|---|---|---|
| Qué se muestra | entre el cuadro anterior y el nuevo | más allá del nuevo, con la misma velocidad |
| Retraso | hasta k − 1 campos (33 ms a 20 cuadros, 17 ms a 30) | ninguno |
| Errores | lo nuevo aparece un poco antes | se pasa al frenar o cambiar de dirección; lo que se detiene sigue un poco |
| GoldenEye (k = 3 / k = 2) | 30,0 / 33,6 dB | 27,0 / 29,7 dB |

Las dos valen la pena (las dos muy por encima de repetir, 19,9 / 20,7 dB):
interpolar por omisión, extrapolar para quien prefiera la respuesta del
control (apuntar en GoldenEye).

### En la PS3

Dónde va cada cosa:

- **Emparejamiento**: necesita la entrada de cada G_VTX y las coordenadas
  de recorte que calcula la HLE. Dos opciones:
  1. En el **SPE 0**, al cargar los vértices, con la tabla del cuadro
     anterior en **memoria principal** (en el local store no cabe: quedan
     ~3,3 KB de pila sin usar). Por grupo: una consulta del índice y una
     lectura de n × 16 bytes por DMA, y la escritura de las posiciones de
     ahora: ~250 grupos por cuadro en GoldenEye y ~360 en Zelda, unos
     1000–1500 DMA, **~0,3–0,6 ms por tarea** (la HLE de una tarea de
     GoldenEye tarda 5–15 ms). Necesita ~3–4 KB de código: hay que liberar
     local store (candidatos: el directorio de la caché de texturas, 28,5 KB,
     o el área de decodificación).
  2. En **otro SPE, después de la tarea** (recomendado por el local store):
     el SPE 0 escribe por grupo un registro con su identidad, el viewport y
     las n coordenadas de recorte que ya calculó, y en el `pad` libre de
     cada vértice de salida de qué grupo y lugar sale (los pocos vértices
     que inventa el recorte cercano, además los dos padres y t). Un SPE
     libre empareja y escribe las posiciones anteriores. Cuesta ~16 bytes
     más por vértice cargado de salida del SPE 0 y casi nada de su local
     store; el SPE de interpolación sirve también para el camino por imagen.
- **Mezcla en el RSX**: un segundo atributo de vértice con la posición
  anterior (16 bytes por vértice, en otro flujo o con paso de 48 bytes) y
  una variante del vertex program de rsxkit (`rsxkit_lincomb.vcg` con
  `pos = lerp(prev, pos, t)` y t constante; extrapolar es t > 1). El cuadro
  verdadero usa el programa de siempre.
- **Volver a dibujar**: el hilo de presentación traduce otra vez la salida
  de la tarea con el programa interpolado y la constante t (la traducción
  de una tarea de GoldenEye cuesta 1,81 ms del hilo 1, y cada ms del hilo 1
  quita ~0,13 ms a la emulación: docs/perf-log.md). Más adelante se puede
  grabar el tramo de comandos del RSX de la tarea y repetirlo con otra t.
  La salida de la tarea (vértices, comandos, texturas) tiene que seguir
  viva uno o dos campos más: un juego de búferes más en la rotación.
- **Dónde se dibuja**: en una superficie de presentación propia, no en el
  destino del juego. Así **en los cuadros inventados no se copia nada a la
  RDRAM**, no cambia la tabla de destinos y lo que el CPU lee del
  framebuffer es siempre lo del juego. Los dibujos a destinos auxiliares
  (render a textura) no se repiten: el intermedio lee esas texturas como
  quedaron. Un juego que dependa mucho de efectos de framebuffer se marca
  «Apagado» en la lista.
- **Filtros**: el CRT y la reducción se aplican al cuadro presentado, sea
  verdadero o inventado (los filtros ya rehacen el trabajo en cada campo
  con imagen nueva).

### Costo estimado en la PS3

| | Zelda OoT (20 → 60) | SM64, Mario Kart (30 → 60) | GoldenEye, Perfect Dark con tope 30 |
|---|---|---|---|
| CPU emulado (hilo 0) | igual | igual | **−48 % / −28 %** (por el tope) |
| Hilo 1 (traducción otra vez) | ~2/3 de traducción más por campo: ~0,07 ms de emulación perdida | ~1/2: ~0,05 ms | ~1/2 de 1,81 ms: ~0,12 ms |
| SPE (emparejamiento) | ~0,4 ms por cuadro | ~0,4 ms | ~0,3 ms (a la mitad de tareas que hoy) |
| RSX | la escena ×3 | la escena ×2 | igual que hoy (hoy ya dibuja 60 por segundo) |
| Memoria | un juego de búferes de la HLE más (~3 MB), tablas ~0,2 MB, una superficie de presentación (2,7 MB a 3×) | igual | igual |

GoldenEye usa hoy 2,05–2,25 ms del RSX por cuadro a 3×; Zelda, SM64 y Mario
Kart dibujan menos triángulos. Con el tope de 3× y los filtros el RSX va al
50–82 %: Zelda a 60 con un CRT pesado a 1080p puede no caber; la escala
automática o una escala menor lo resuelve (se mide en la consola).

## Camino por imagen

### Búsqueda por bloques

- Luma de 8 bits a resolución nativa (320 × 240), pirámide de tres niveles
  (320, 160, 80).
- **Bloques de 8 × 8** en cada nivel (1200 + 300 + 80 = 1580 bloques).
- Nivel más grueso: búsqueda completa en **±8 píxeles** (±32 en el nivel
  0). Los otros: candidatos (el vector del padre × 2 y los de sus vecinos,
  la mediana de los vecinos ya resueltos, cero) y un diamante de un píxel
  hasta que no mejora. Costo: SAD + 4 × |v − mediana| (campo más suave).
- Medido: **~41 000–45 000 SAD de 8 × 8 por búsqueda** (26–28 por bloque).

Deformación: para el intermedio en t, cada píxel toma el vector bilineal
entre centros de bloque y mezcla la imagen vieja en p + d·t con la nueva en
p − d·(1 − t); si las dos muestras no concuerdan (oclusión) toma la del
cuadro más cercano en el tiempo (una imagen nítida en vez de un fantasma
doble). Un píxel igual en los dos cuadros se queda (HUD opaco, cielo).

### Costo del SPE

Un SAD de 8 × 8 en el SPE: cuatro vectores de 16 bytes (dos filas cada
uno), lecturas desalineadas con `shufb`, `absd` y `sumb`: ~25 instrucciones,
**~30 ciclos** con el doble despacho y el manejo del candidato. 43 000 ×
30 ≈ 1,3 M ciclos ≈ **0,4 ms**, más la luma y la pirámide (~0,1 ms) y el
DMA de las imágenes: **~0,6–0,8 ms por cuadro del juego** en un SPE libre
(a 20 cuadros hay 50 ms). Local store: por franjas de bloques con el margen
de búsqueda (~90 filas de la imagen vieja, ~30 KB doble búfer); ~100 KB en
total. Sobra para hacer la búsqueda en el tiempo de cada intermedio (más
calidad, ×(k − 1)).

### Costo del RSX y memoria

- Reducir el destino a 1× para el SPE (si no hay un CRT que ya lo haga):
  1,2–1,8 ms a 3× por cuadro del juego (docs/perf-log.md, «Reducir»).
- Copiar la imagen anterior antes de que el juego dibuje encima: ~0,3 ms y
  2,7 MB a 3×.
- La deformación, un pase a la resolución de salida con tres lecturas por
  píxel (vector, imagen vieja, imagen nueva): estimado 1–2 ms a 1080p por
  campo inventado.

### Oclusiones, HUD y latencia

- Las oclusiones son donde falla: el 83–87 % de los bloques mal
  interpolados está en un borde del campo de vectores. Lo que entra por el
  borde de la pantalla al girar no está en ninguna de las dos imágenes.
- El HUD opaco se queda (píxel igual); el translúcido o con sombra sobre un
  fondo que se mueve se deforma (las balas de GoldenEye, el «8th» de Mario
  Kart con el humo detrás). Mejora posible: el HUD son los rectángulos
  después del último triángulo; el RSX puede guardar la imagen antes de
  ellos para la búsqueda y redibujarlos encima del intermedio.
- Cortes: si el SAD medio por píxel pasa de 10/255, no se interpola (en la
  presa da 5 falsos de 72, que solo repiten el cuadro).
- Latencia: la misma que por geometría al interpolar. Extrapolar por imagen
  casi no mejora al fundido; no vale la pena ofrecerlo.

Su lugar: los juegos que no pasan por la HLE gráfica (el RSP a bajo nivel,
microcódigos sin HLE), imágenes del CPU y los juegos donde la geometría no
empareje.

## Arquitectura común

- **El interruptor**, como el filtro y la escala (mismo patrón de
  `src/frontend`):
  - global en Opciones (`menu.cfg`, clave `interpolacion`), por juego en
    `juegos/<ROM>.cfg` (con «global» por omisión; manda el juego) y en vivo
    desde «Imagen» de la pausa (una fila más en `pausectl`/`osd`);
  - valores: `apagado`, `geometria`, `geometria-sin-retraso`, `imagen`; y
    por juego `tope = libre | 30 | 20`;
  - una lista por código de juego (como `core/savedb.c`) recomienda el
    modo y el tope cuando el usuario dejó «global»: Zelda OoT, SM64, Mario
    Kart → geometría; GoldenEye, Perfect Dark → geometría y tope 30;
    juegos con el RSP a bajo nivel → imagen;
  - textos en los 6 idiomas de `src/frontend/i18n.c`: «Interpolación»,
    «Apagada», «Por geometría», «Por geometría, sin retraso», «Por
    imagen», «Tope de cuadros», «Libre», con su ayuda.
- **Costo cero apagado**: sin la bandera del trabajo la HLE no busca nada
  (una comparación por G_VTX), el vertex program es el de siempre, no hay
  pases extra del RSX ni trabajo del SPE de interpolación, y el tope no
  toca la interrupción del DP. La memoria se reserva al arrancar (sin
  asignaciones en el bucle). El prototipo del host lo comprueba: con
  `-DSALSA64_GFX_INTERP` y sin `--interp`, 37 de 37 volcados de la presa y
  61 de 61 del vuelo de Navi idénticos byte por byte a los de `main`, y
  gfxtest pasa en sus tres variantes; con `--interp`, la imagen del juego
  sigue igual.
- **En vivo**: el modo se lee en cada campo (presentación) y en cada tarea
  (bandera del trabajo de la HLE); cambiarlo en la pausa vale desde el
  cuadro siguiente. El tope cambia `dp_cycles` del RSP emulado en la
  barrera de la pausa.
- **El tope de cuadros**: retrasar la interrupción del DP (como un RDP
  lento) es lo más simple y ya existe en el host (`SALSA64_DP_CYCLES`).
  Un retraso fijo por tarea castiga a los juegos con varias tareas por
  cuadro; mejor «el DP termina no antes de k campos desde la tarea que
  dibujó el framebuffer anterior». Solo por juego y medido: un juego que
  no mida el tiempo (ritmo fijo) iría más lento.

## Tope de cuadros (paso 1, hecho)

> En pausa y fuera de la rama de la interpolación: su código es el commit
> dfc788a (rama `interp-respaldo-2eaef15`, sobre v0.2.4); el paquete de la
> interpolación no lo trae. Lo que sigue describe ese commit y lo medido con
> él.

Opción «Tope de cuadros: Libre / 30 / 20» en Opciones (`menu.cfg`, `tope =
libre|30|20`), por juego (`juegos/<ROM>.cfg`, con «global») y en «Imagen»
del menú de pausa (vale al seguir jugando, con la emulación quieta en el
límite de un campo). En el núcleo, `salsa64_rsp_set_frame_cap`: la
interrupción del DP de cada tarea gráfica no llega antes de k − 0,1 campos
del VI (1,9 o 2,9; el campo en curso, NTSC o PAL) desde la anterior. Un
espaciado y no un retraso fijo por tarea: si el juego llega tarde a un
cuadro no paga el retraso encima, y el ritmo queda parejo. Las
interrupciones del RDP a bajo nivel (DPC) no se tocan; la retenida por
FREEZE (Banjo) respeta el tope al descongelarse. Si el tiempo retrocede
(estado cargado), la anterior no cuenta. Con «Libre» el código no hace
nada distinto.

Medido en el host (respaldos propios), 300 campos de la presa girando y
caminando:

| | Cuadros del juego | CPU en espera | CPU activo | Host |
|---|---|---|---|---|
| GoldenEye libre | 297 (59,4/s) | 60,5 % | 39,5 % | 4,5 s |
| GoldenEye tope 30 | 159 (31,8/s) | 77,2 % | 22,8 % (−42 %) | 2,8 s |
| GoldenEye tope 20 | 105 (21,0/s) | 83,3 % | 16,7 % (−58 %) | 2,2 s |
| Perfect Dark libre (30 s de la intro) | 1290 (43/s) | 84,0 % | 16,0 % | 6,1 s |
| Perfect Dark tope 30 | 749 (25/s) | 88,2 % | 11,8 % (−26 %) | 4,5 s |
| Perfect Dark tope 20 | 493 (16/s) | 89,7 % | 10,3 % (−36 %) | 4,0 s |

Con el tope de 30 el DP espera de media 22,5 ms desde el fin de la tarea
(un RDP de la consola tarda del orden de eso en la presa). Zelda (20), SM64
y Mario Kart (30) con tope 30: las mismas tareas, la misma espera y el
mismo audio que libre. Banjo y Conker con 30: imágenes idénticas a libre
(sus DP ya llegan espaciados); con 20, Banjo pasa por el FREEZE con el tope
activo (388 esperas) y llega al título, Conker a la taberna: ninguno se
cuelga. Audio: las mismas muestras del AI con y sin tope, sin huecos (los
de Conker son previos y no cambian). Regresión con «Libre»: 248 volcados de
GoldenEye, Perfect Dark, Zelda, SM64, Mario Kart, Banjo y Conker idénticos
byte por byte a main.

**¿Un modelo de tiempo del RDP en vez de un tope?** Contar ciclos por
comando o por píxel (lo que tardaría el RDP de verdad) sería más fiel y
serviría para todos los juegos a la vez, pero el ritmo dependería de la
escena como en la consola (GoldenEye bajaría a 15–30 en la presa), la
estimación de la HLE (área de cada triángulo a 1×, modo de ciclo, lecturas
de la z) costaría en el SPE 0 y cambiaría el comportamiento de juegos que
hoy van bien. Para liberar el PPE en juegos concretos y con un ritmo parejo
(lo que la interpolación necesita), el tope por juego es más simple y
predecible; el modelo puede volver como opción si algún juego necesita los
tiempos del RDP.

### En la PS3 real (2026-10-13): en pausa

Medido por el autor con «Salsa64 tope» (SLSA00115), GoldenEye en la presa
y Perfect Dark, Libre contra 30:

- **GoldenEye**: el tope se aplica (30–31,6 cuadros del juego estables,
  MIPS −34 %, espera 45 → 64 %), pero la emulación por campo no bajó
  (14,2 → 15,2 ms). Causa: con el tope, cada cuadro hacía una **copia del
  framebuffer a la RDRAM esperada** («158 copias… la emulación esperó 158
  tareas, 15,35 ms c/u»: ~8 ms por campo). Era un fallo previo de
  `readbacks_wanted` (src/rdp/rdp.c): tomaba como ancho del VI el del
  último origen *agregado* al anillo, y un origen ya visto no se vuelve a
  agregar; tras las pantallas de la misión (440 de ancho), la presa (320)
  quedaba «de otro ancho» y cada cuadro parecía una imagen auxiliar. Con
  Libre el orden de los orígenes lo tapaba por casualidad. Arreglado
  (`rdp_vi_ring.cur_width`, el ancho del último campo); en el host, la
  regla de la PS3 (`SALSA64_PS3_RB=1`) pasa de 1083 copias a 4 con el tope
  y **Perfect Dark sin tope, en la intro, de 1167 a 3**: ya pagaba ese costo.
  Falta volver a medir con «Salsa64 tope 2».
- **Perfect Dark**: los DP van a 31,6 por segundo y las tareas gráficas
  también (31,4–31,6/s); el «55–60 cuadros del juego» de algunas ventanas
  es del contador, que cuenta cambios de origen del VI: en las pantallas de
  576 × 480 (entrelazadas) el origen cambia en cada campo (líneas pares e
  impares) aunque no haya cuadro nuevo. En juego (320 de ancho) cuadros y
  tareas coinciden (20,8–31,5). El contador de «cuadros del juego» del
  contador de FPS tiene el mismo problema con los modos entrelazados: para
  contar cuadros habría que contar destinos nuevos presentados (targets.c
  sabe qué tarea dibujó el que se muestra), no cambios de origen.
- **Errores gráficos con 30** («los polígonos de los bordes desaparecían o
  se pintaban más cerca del centro» uno o dos cuadros; sin decir en qué
  juego): sin causa todavía. En el host la geometría de la HLE sale igual
  con y sin tope (grietas en el plano cercano, `SALSA64_GFX_CUTCHECK`:
  separación máxima 0,36 px libre y 0,30 con 30, en la presa). GoldenEye y
  Perfect Dark mandan una tarea gráfica por cuadro (tareas = cuadros en el
  log), así que no hay cuadros armados a medias por una segunda tarea
  retrasada. Candidatos: las copias a la RDRAM de cada cuadro del primer
  paquete (escribían la imagen del RSX reducida en la RDRAM cada cuadro;
  con el arreglo desaparecen) y que a 30 un cuadro con un defecto que ya
  existía se ve dos campos en vez de uno. Para separarlos: «tope 2», y el
  juego y el lugar si se repite.

Por eso el tope queda en pausa; el siguiente paso es la interpolación por
geometría en Zelda OoT, que va a 20 fijos y no necesita tope.

## Plan para la PS3 (después de que se decida)

1. Tope de cuadros por juego (en la rama, en pausa: arriba).
2. **Zelda OoT por geometría, sin tope** (abajo, «Paso 2»), y después SM64
   y Mario Kart (30 fijos).
3. GoldenEye y Perfect Dark, con el tope si se retoma.
4. Por imagen: búsqueda en el SPE con SIMD y deformación en el RSX, con el
   HUD aparte.

### Paso 2: Zelda OoT por geometría (plan)

Opción «Interpolación: Apagada / Por geometría» (y «sin retraso» después),
como el filtro: Opciones, juegos/<ROM>.cfg e «Imagen». Zelda va a 20
cuadros fijos (3 campos por cuadro), una tarea gráfica por cuadro, sin
recorte cercano (NoN: no hay vértices inventados por el recorte) y con
99 % de los vértices de triángulos emparejados en el host.

1. **La HLE da la identidad, sin local store nuevo** (SPE 0). Con la
   bandera del trabajo `GFX_JOB_INTERP`:
   - por cada G_VTX, un registro en el flujo de comandos que ya existe
     (`GFX_OP_VGROUP`: dirección y paso de la lista del juego, v0, n, el
     viewport y las n coordenadas de recorte que ya calculó; ≤ 1 KB), así
     no hace falta otro búfer de DMA; el hash del contenido lo saca el PPE
     de la RDRAM;
   - en el `pad` libre de cada vértice de salida, de qué grupo y lugar
     sale (el RSX no lo lee); los que agrega el recorte cercano, su grupo
     y sus dos extremos (en GoldenEye, SM64 y Mario Kart los dos extremos
     son siempre del mismo G_VTX), y la fracción la recalcula el PPE con
     las coordenadas de recorte del registro.
   Sin la bandera no se emite nada: el SPE 0 hace lo de hoy (las
   identidades quedan en 0). En `src/rsp/gfx/vgroup.c`, con -Os.
2. **Emparejamiento en el PPE, al traducir** (hilo 1; código portable,
   probado en el host): índice de los grupos del cuadro anterior (dirección
   + repeticiones, contenido de respaldo), reglas de corte por grupo y por
   cuadro, y por cada vértice de salida su posición en el cuadro anterior,
   en un flujo paralelo dentro del mismo juego de búferes (el área de
   vértices de 2 MB tiene lugar de sobra: Zelda usa ~100 KB). Zelda: ~360
   grupos y ~2900 vértices por cuadro, ~0,1–0,2 ms del hilo 1 por cuadro
   (≈ 0,03 ms de emulación por campo). Si pesara, pasa a un SPE libre.
3. **rsxkit, genérico**: una variante del vertex program con un segundo
   atributo de posición y `pos = lerp(prev, pos, t)`; el cuadro verdadero
   usa el programa de siempre.
4. **Presentación**: en cada campo en que el VI repite el origen, se vuelve
   a traducir la tarea que dibujó el destino presentado con t = j/3, en
   una superficie de presentación propia (sin copias a la RDRAM ni cambios
   en la tabla de destinos), y se presenta con el filtro de siempre. Con 3
   juegos de búferes y una tarea cada 3 campos, el de la tarea sigue vivo
   (no hace falta memoria nueva); si ya se reusó, ese campo se presenta
   como hoy. Interpolar: el cuadro N se completa 2 campos después (33 ms).
5. **Pruebas**: en el host, el camino del PPE contra el prototipo
   (mismas posiciones) y apagado idéntico a main; en RPCS3, que arranque y
   se vea; en la PS3, Zelda en el bosque Kokiri y en el campo de Hyrule:
   ms del RSX (la escena ×3), hilo 1, campos/s de la emulación (no deben
   bajar) y a simple vista.

### Pasos 1–3: hechos en el host y en RPCS3 (2026-10-13)

- **HLE (SPE 0).** `src/rsp/gfx/vgroup.c` (con -Os) arma el registro
  `GFX_OP_VGROUP` de cada G_VTX cuando el trabajo trae `GFX_JOB_INTERP`;
  la identidad viaja en los bits altos de `gfx_vtx.clip` hasta
  `gfx_vertex.pad`. Los vértices que agrega el recorte cercano llevan su
  grupo y sus dos extremos: medidos en el host, GoldenEye (2604), SM64
  (760) y Mario Kart (822) los tienen siempre en el mismo G_VTX; la
  fracción la recalcula el PPE con las coordenadas del registro. El
  contador de grupos va en un hueco de `gfx_state` (el estado no crece).
  Local store: +952 bytes de código, el mismo bss. Sobre main 0571a0b
  (con los gráficos de Snap y Kirby y gSPInsertMatrix) el SPE 0 de main
  tiene 107 552 bytes de código y holgura de la pila 1984 (mínimo 1536):
  sin más, la interpolación no cabría. Con `--gc-sections` en el enlace de
  los SPU (el primer commit de la rama) queda en 105 752 con la
  interpolación incluida, y holgura 3776. Apagada: una prueba de la
  bandera por G_VTX y las identidades en 0.
- **Emparejamiento (PPE).** `src/rdp/vmatch.c`, portable: el mismo en el
  host (`salsa64-run --interp`) y en la PS3. En el host da lo mismo que el
  prototipo (GoldenEye contra la verdad, k = 3: 30,05 dB y 3,30 % de
  píxeles malos). En la PS3 corre al traducir cada tarea (hilo de
  presentación), con dos tablas de 2048 grupos y 32 768 vértices (~1,3 MB,
  reservadas la primera vez que se enciende); las posiciones anteriores
  van detrás de los vértices, en el mismo juego de búferes (con la bandera
  la HLE escribe vértices hasta 4/3 MB de los 2).
- **rsxkit.** `rsxkit_vertices_prev(offset, location, t)` liga un arreglo
  más de posiciones (TEXCOORD1) y dibuja con `rsxkit_lincomb_lerp.vcg`, el
  mismo vertex program con `prev + (pos − prev)·t`. Sin llamarla, el
  programa y el estado de siempre.
- **Prueba sin presentación nueva:** `/dev_hdd0/salsa64/interp-prueba`
  (con «t»): cada tarea se empareja con la anterior y se dibuja en t. En
  RPCS3, gfxtest con t = 1 sale idéntico (los mismos bytes) a sin el
  archivo, y con t = 0 el cubo que gira sale un cuadro atrás (0,71 % de
  los píxeles) y lo demás igual. Zelda (la intro, 1800 campos, con
  t = 1): 72–83 % de los vértices de salida con pareja por ventana de
  5 s (~1000–4000 por tarea), un corte, nada sin lugar en las tablas, y
  0,18–0,26 ms del PPE por tarea (el tiempo de RPCS3 no es concluyente);
  en el host la misma intro da 75,8 %: el PPE empareja como el prototipo
  (en el vuelo de Navi, 95,8 %). GoldenEye no llega a jugar en RPCS3 con
  ningún build (el RSX muere a los ~16 s; docs/estado.md).
- **Apagada = main:** en el host, 105 imágenes de 7 escenas (Perfect
  Dark, la pausa de Zelda, SM64, Mario Kart, GoldenEye, Banjo y Conker)
  idénticas a las de antes; pruebas unitarias bien (494; solo falla
  test_cover en macOS, como en main). En RPCS3, la imagen de gfxtest de
  la rama sin el archivo es la misma (byte a byte) que la de main.

### Paso 4: campos intermedios (hecho; por medir en la PS3)

Opción «Interpolación: Apagado / Por geometría» en Opciones (`menu.cfg`,
`interpolacion = no|geometria`), por juego (`juegos/<ROM>.cfg`, con
«global») y en «Imagen» del menú de pausa (vale al seguir), en los 6
idiomas. Con «Por geometría» (`SALSA64_INTERP_FIELDS`, src/rdp/rdp.c):

- **Cuándo.** En cada campo, el destino que presenta el VI y la tarea que
  lo dibujó (`last_task` de la tabla de destinos). Cuando cambia, empieza un
  cuadro del juego; k es lo que duró el anterior (2 a 4 campos: 30 a 15
  cuadros; si no, nada). En el campo j desde que apareció, con j + 1 < k, se
  presenta un intermedio con t = (j + 1)/k; en el último, el cuadro de
  verdad. A 20 cuadros: 1/3, 2/3 y el cuadro: llega dos campos después
  (33 ms). Un juego de 60 nunca entra.
- **Qué se dibuja.** La tarea de ese cuadro otra vez, en una superficie
  aparte del tamaño del destino (y su profundidad; cuentan en el tope de
  memoria del RSX): primero una copia del destino (lo que la tarea no
  redibuja queda igual) y después solo sus dibujos a ese destino, con el
  vertex program interpolado; los `GFX_OP_ZCLEAR` borran la profundidad
  aparte. Nada de la tabla de destinos, copias a la RDRAM ni espejos: lo
  presentado es lo único que cambia, y el filtro y la pausa lo leen de ahí
  (`salsa64_rdp_present_source`, con su propio contador de cambios).
- **Solo si se puede.** Que el juego de búferes de esa tarea siga con ella
  y sus posiciones de antes valgan (hubo anterior, sin corte). Mientras se
  redibuja, `acquire` no reusa ese juego, y hasta que el RSX pasa la
  barrera del redibujo la HLE no suelta de la caché de texturas lo que la
  tarea usa (un apretón de manos con `sync`: `interp_hold` contra
  `acquire`); si ya es tarde, ese campo va como siempre y cuenta como
  «saltado».
- **Apagada** no cambia nada: ni banderas a la HLE, ni tablas, ni
  superficie, ni comandos de más; `acquire` ni mira.
- **Log** cada 5 s: «gfx: interpolación: … vértices de salida con pareja X %
  …, emparejar Y ms del PPE por tarea; N campos intermedios (n/s, m
  saltados), redibujar Z ms del PPE por campo intermedio».

Sobre main 0571a0b, en el host las 105 imágenes de las 7 escenas siguen
idénticas a main con la opción apagada, y en RPCS3 gfxtest sale igual byte
a byte que el de main y el menú pasa.

En RPCS3 (Zelda, la intro, 1800 campos, «Por geometría» en `menu.cfg`): 40
campos intermedios por segundo en las ventanas del juego a 20 (dos por
cuadro), 0–3 saltados por ventana, 72–85 % de los vértices de salida con
pareja, emparejar 0,17–0,26 ms y redibujar 0,07–0,22 ms del PPE por campo
(tiempos de RPCS3, no concluyentes); la prueba termina bien. gfxtest (las
pruebas apagan la interpolación) idéntico byte a byte a main.

**Para medir en la PS3** (paquete «Salsa64 interp 1», SLSA00116): Zelda en
el bosque Kokiri y en el campo de Hyrule, con la interpolación encendida y
apagada desde «Imagen». A la vista: más fluido y sin objetos que tiemblen.
En el log: el % de pareja, los ms del PPE (emparejar y redibujar) y los
campos/s de la emulación, que no deben bajar.

### Primera prueba en la PS3 («Salsa64 interp 1») y arreglos («interp 2»)

El autor, con Zelda OoT y Majora a escala 5×: «se ve fantástica», pero hay
texturas que parpadean, y en Majora el hada deja una estela como de
píxeles fantasma (no siempre). El log:

- La emulación se mantuvo en 60 campos/s. Emparejar costó 0,8–3,5 ms del
  PPE por tarea (en RPCS3, 0,2): 3,5 ms en el campo de Hyrule con 7195
  vértices; en Majora, 1,5–3,4. En esas ventanas se saltaron hasta 96 de
  200 intermedios.
- La presentación pasó de ~2 a 7–8 ms de RSX por campo (la escena hasta
  tres veces a 5×) y se saltaron 10–23 campos presentados cada 5 s.

Lo que se cambió:

- **Parpadeo.** Un intermedio que no llegaba a tiempo mostraba el cuadro
  final y el campo siguiente volvía a 2/3: lo que se mueve iba adelante y
  atrás. Ahora ese campo repite lo del campo anterior (el movimiento nunca
  retrocede), y desde que un cuadro aparece la HLE no suelta de la caché de
  texturas lo que usa su tarea (antes, solo mientras se redibujaba), así
  fallan muchos menos. El log separa los campos «enteros» (el cuadro no se
  interpola: corte, texturas de un destino) de los «repetidos» (no llegaron
  a tiempo).
- **Estela del hada.** Las instancias de un modelo repetido (partículas,
  destellos) se emparejaban por orden: una partícula que renace en otro
  lugar se dibujaba a medio camino entre los dos, y en una estela en anillo
  un vértice salta del final al principio. Ahora, entre instancias, la del
  mismo orden si se movió como sus hermanas (la mediana de las demás) y si
  no, la más cercana que sí; sin pareja (quieta en su lugar de ahora) la que
  se movió mucho más que sus hermanas o el grupo chico (menos de 64 px) que
  se deforma de golpe. Los modelos de una sola instancia no pasan por esto
  (la cámara mueve igual todo): GoldenEye contra la verdad sigue en 30,05
  dB; en el bosque Kokiri caen 3–4 grupos por tarea, en Majora 0–1.
- **Efectos de framebuffer.** Una tarea que lee texturas de un destino no
  se interpola (redibujada más tarde leería lo que el destino tenga
  entonces).
- **Costo.** vmatch arma las tablas sin pasadas cuadráticas, solo lee la
  RDRAM (hash del contenido) de los grupos que cambiaron de dirección, pide
  la memoria antes (prefetch), no divide por vértice (la profundidad z/w la
  toma el vertex program del vértice de ahora) y no empareja nada si el
  juego va a 60 cuadros. En el host (tools/vmatch-bench.c con dos tareas de
  Majora, 6921 vértices de salida): armar 31 → 21 µs, posiciones 23 → 17
  µs; el costo real en el PPE no se puede medir aquí: el log ahora lo
  separa en armar, emparejar y posiciones.

No encontré la escena exacta de la estela en los estados de la consola
(el hada no está a la vista en ninguno); conviene que el autor diga el
lugar, o guarde un estado ahí, si sigue con «interp 2».

Después el autor agregó: el polvo de Epona va «con retraso» y el HUD
tiembla (al guardar la espada en Zelda, el botón A gira). En el host, con el
estado del árbol Deku de la consola y A al guardar la espada, el giro se ve
bien. El botón es 3D con perspectiva, en un búfer dinámico (cambia de
dirección en cada cuadro), así que se empareja por el contenido: el hash lo
leía el PPE de la RDRAM al traducir, y en la consola eso llega más tarde que
en el host; si el juego ya reescribió ese búfer, la pareja sale mal. Lo
mismo vale para partículas en búferes dinámicos. «interp 3»:

- **El hash del contenido lo calcula el SPE** en `gfx_vgroup`, con los 16
  bytes de cada vértice que acaba de leer, y va en el registro: el PPE ya no
  lee la RDRAM (menos memoria: armar 21 → 15 µs en el banco del host). En
  el SPE 0, 184 bytes de código (holgura 3648).
- **Lo que va con proyección ortográfica** (w = 1 en todos sus vértices: el
  HUD 3D) no se interpola ni cuenta para decidir un corte; sale como en el
  cuadro verdadero, igual que los rectángulos y TEXRECT, que ya salían así
  (sin pareja: su posición de ahora).
- Lo que queda sin pareja (partículas que renacen, etc.) sale como en el
  cuadro verdadero que se va a presentar, no de la copia: el redibujo pinta
  todos los dibujos de la tarea sobre ella.

Con «interp 2», en el bosque Kokiri moviendo a Link, el corazón que late
del HUD salía en algunos intermedios enorme o fuera de lugar (hasta ~40 %
de la pantalla). Con el estado que guardó el autor no se reproduce en el
host con ninguna de las dos versiones: el corazón va en un búfer dinámico
(su dirección alterna entre dos) y se emparejaba por el hash que el PPE leía
de la RDRAM al traducir; en la consola, si el juego ya había escrito ahí otra
cosa, la pareja era otro grupo y el corazón se estiraba hacia él. En
«interp 3» el hash viene de la HLE, el corazón es ortográfico (w = 1: no se
interpola) y, por si acaso, una pareja por el contenido que cambia de tamaño
en pantalla más del doble se descarta.

Desnormales (Cell Handbook A.6.3: en la FPU escalar del PPE un operando o
resultado desnormal vacía el pipeline, ≥ 100 ciclos; el SPU y el vertex
program los aplanan): en vmatch las coordenadas de pantalla salen de
vértices con w > 1e-6 (nada de 1/w de un w diminuto) y la fracción del
recorte cercano exige una diferencia de al menos 1e-6; las diferencias de
pantalla son 0 o mayores que ~1e-5, así que sus cuadrados no llegan a
desnormales. Si hubiera tirones, se verían como picos en «armar» o
«emparejar» del log.

### «interp 3» en la PS3 y el agua («interp 4»)

El autor: el HUD ya quedó bien; al caminar junto al estanque del bosque
Kokiri «el agua rebasa un lugar donde no debería estar por unos instantes».
Con su estado (ranura 1, campo 16772) y caminando, el emparejamiento en el
host es casi perfecto (4 grupos rechazados en toda la corrida). La causa es
otra: un vértice que en un cuadro está detrás de la cámara (w < 0) y en el
otro delante pasa por w = 0 en algún intermedio, se proyecta al infinito y
sus triángulos tapan media pantalla o dejan un hueco por el que asoma el
agua. El rasterizador del host descartaba esos triángulos y no se veía:
con `SALSA64_INTERP_COMO_RSX=1` el host recorta como el RSX (también en los
cuadros de verdad) y aparece (en el intermedio 75-1 desaparece casi todo el
piso cercano). Ahora un vértice que cambia de lado de la cámara entre los
dos cuadros no se interpola: queda donde está. La regla es del vértice, así
sus copias en otros grupos hacen lo mismo y no se abren grietas; con ella
ese intermedio sale bien. Cuesta un poco en GoldenEye, que recorta en el
plano cercano y antes los interpolaba bien: 30,05 → 29,73 dB contra la
verdad (3,30 → 3,64 % de píxeles malos).

Los «repetidos» que suben a 60–100 por ventana en esa zona (con 21–26
intermedios por segundo) son intermedios que no llegaron a tiempo; a 5× el
RSX va justo y el estanque translúcido pesa: a probar a 3×.

### Mario 64: ningún intermedio a tiempo («interp 5»)

Con «interp 3», Mario 64 (30 cuadros, k = 2) daba en la PS3, incluso a 1×,
0 campos intermedios y ~150 repetidos cada 5 s, y Zelda a 5× la mitad. Se
reproduce en RPCS3 con la intro de Mario. Con los motivos separados en el
log, todos eran «la caché»: el redibujo desistía porque la HLE ya podía
haber soltado texturas de esa tarea. Dos errores en `acquire`:

- `done_given` guardaba el done_task sin recortar por la protección: con
  la traducción un cuadro por delante de lo que se muestra parecía que la
  HLE ya había soltado la tarea en pantalla, aunque había recibido una
  anterior. Ahora guarda lo que la HLE recibe de verdad (y, mientras
  acquire recorta, el valor provisional queda a la vista del redibujo).
- La protección de la tarea en pantalla se apagaba entera si la emulación
  iba más adelante que un juego de búferes (Mario dibuja cuadros antes de
  mostrarlos), y con ella las tareas siguientes, que sí se van a mostrar.
  Ahora protege desde la tarea en pantalla o, si su juego ya se reusó,
  desde la más vieja cuyo juego sigue vivo.

En RPCS3, Mario: 0 → 28 intermedios por segundo (de 30), 0–1 repetidos;
Zelda igual (40/s); gfxtest igual byte a byte a main. El log separa los
repetidos en «su juego de búferes ya en uso», «la caché» y «sin
superficie».

### Nunca un objeto a medias: giros, mallas y objetos

Con el estado del estanque (ranura 1, campo 16772) caminando y girando,
casi nada se rechaza (0,4 grupos nuevos y 0,2 rechazados por tarea, de
~235 delante de la cámara), pero cuando Link se da vuelta en un cuadro
(tarea 107: de correr a la derecha a correr a la izquierda) los
intermedios lo mostraban despedazado: 9 de sus ~60 grupos se rechazaban
por deformarse y salían en la pose nueva, y los demás se interpolaban
aplastados (media vuelta interpolada en línea recta pasa por el centro).
Cada miembro de Link es una malla aparte (comparte vértices solo con uno o
dos vecinos) y va en su propio dibujo, así que ni la malla ni el dibujo
juntan al personaje. Ahora, en `vmatch_atomic` (entre emparejar y las
posiciones):

- Aplastamiento (`VMATCH_COLLAPSE`): el radio del grupo (desde su centro,
  en pantalla) en el punto medio de la interpolación, contra el menor de
  los dos cuadros; menos de 0,7 (un giro de más de un cuarto de vuelta) y
  no se interpola. Solo se mide en los grupos cuyos vértices se movieron
  distinto (dispersión de más de un cuarto de su tamaño), así no cuesta.
  En la tarea 107 se aplastarían 43 de los 59 grupos de Link; en las otras
  133 tareas, de 0 a 2.
- Objeto (`VMATCH_OBJECT`): grupos seguidos en el orden de dibujo que se
  tocan en pantalla (a 4 px o menos, hasta 128 px en total); si la mitad o
  más de sus vértices quedó rechazada (movida, saltó, deformada, de otro
  tamaño o aplastada), el objeto entero sale en el cuadro de ahora. Los
  grupos nuevos no votan ni arrastran a su objeto.
- Malla (`VMATCH_COMPONENT`): los grupos que comparten triángulos; si uno
  delante de la cámara quedó sin pareja (también si es nuevo), la malla
  entera sin interpolar, para que sus triángulos con los demás no se
  estiren.

Con esto Link sale entero en la pose nueva en los dos intermedios del giro
(en el resto de la corrida, 0,3 grupos por tarea por su objeto y 0,1
aplastados). Se probó también juntar por dibujo (el mismo estado de
render): no alcanza para Link y arrastra partículas que comparten
textura. GoldenEye contra la verdad igual (29,73 dB: no se rechaza nada).
En el banco (tarea de Majora) emparejar pasa de 13 a 14 µs; sin rechazos
ni grupos sin pareja `vmatch_atomic` no hace nada. El volcado
`SALSA64_INTERP_GRUPOS` da el motivo de cada grupo y su aplastamiento
(`col`), y `SALSA64_INTERP_SIN_MALLA=1` apaga `vmatch_atomic` en el host.

### El agua que tapa la orilla («interp 5» en la PS3)

Con «interp 5», en el estanque del bosque Kokiri (ranura 1, campo 16772,
caminando), el autor: «el agua se sigue saliendo de su cauce», a cualquier
escala interna. Con sus entradas exactas sacadas del log (las líneas
«control 1: …» como guion de `--pad`) se ve en el host: en un intermedio
el agua tapa un borde de tierra y en otro la tierra tapa el agua junto a
Link. La causa: el vertex program interpolaba x, y y w pero tomaba la
profundidad z/w del vértice de ahora. Los triángulos grandes del agua y
los chicos de la orilla se desplazan distinto, y con la z/w de ahora en
otra posición dos superficies casi a la misma profundidad se cruzan. Ahora
el registro GFX_OP_VGROUP lleva la vista en z del grupo (z de salida =
vz[1]·w + vz[0]·z, como gfx_vtx_out; con la z de primitiva, vz[0] = 0),
vmatch_prev da también la z de allá (si la z de ahora no sale con esa
vista, la de allá con la misma z/w que ahora) y el vertex program
interpola las cuatro coordenadas: el mundo quieto se dibuja como con una
sola matriz intermedia y las profundidades no se cruzan. En el host, con
las entradas del autor, los dos intermedios salen como los cuadros de
verdad (píxeles que cambian solo en el intermedio: 358 → 57 y 1114 →
225); lo que queda es el tramado del agua translúcida del rasterizador del
host, que cambia en cada cuadro. GoldenEye contra la verdad 29,73 →
29,76 dB. 16 bytes más por registro; el vertex program, una división
menos. Con «interp 6» en la PS3, el autor: «el agua ya está quieta».

### Piezas de Link al darse vuelta («interp 6» en la PS3)

Con «interp 6» (el agua ya quieta), en la media vuelta de Link «salen
pedazos de gráficos por unos instantes». Con las entradas del autor en el
host (otra vez del log, ranura 1) salen dos casos de objeto a medias:

- Un tajo con la espada (tarea 211): se aplastan el brazo y la espada
  (9 de unos 60 grupos), menos de la mitad del objeto, así que esas piezas
  salían en el cuadro de ahora y el cuerpo interpolado: el brazo se
  despega. Ahora el objeto es todo o nada: si la parte rechazada es menor,
  las piezas que se deformaban o aplastaban recuperan su pareja y se
  interpolan con las demás (interpoladas, dos piezas unidas por una
  articulación siguen unidas: el punto que comparten va por la misma
  recta). Un grupo solo (una partícula, una estela) sigue rechazado.
- Media vuelta con Link partido en dos objetos (3 de 15 medias vueltas):
  las piernas y el torso quedaban a más de 4 px en el orden de dibujo, y
  uno salía en el cuadro de ahora y el otro interpolado. Ahora dos grupos
  seguidos cuyos vértices vienen del mismo modelo (direcciones a menos de
  128 KB) se juntan hasta a 24 px; con eso, 0 de 15.

Los cortes de cámara del recorrido (Z para centrar la cámara) se detectan
como corte y los intermedios salen como el cuadro final. En el resto, por
tarea: 0,4 grupos por su objeto y 0,17 piezas deformadas o aplastadas que
se interpolan con su objeto; GoldenEye contra la verdad igual (29,76 dB).
El volcado de grupos da el objeto de cada uno (`obj`).

### El entorno que se despedaza al centrar la cámara con Z

Con «interp 6», «si volteo y presiono Z, el entorno se despedaza»: Z pone
la cámara detrás de Link en dos o tres cuadros. Con las entradas del autor
en el host, esos cuadros tienen de 40 a 140 grupos que se movieron más de
120 px; solo algunos llegaban al corte de antes (un tercio de los
encontrados) y en los demás lo movido salía en el cuadro de ahora y el
resto interpolado: el escenario en pedazos. Ahora el cuadro entero no se
interpola (corte «por un giro de la cámara», aparte en el log) si más del
5 % de los grupos que se ven se movió más que max_disp, o si la mitad de
los emparejados se desplazó más de max_disp / 2. En el recorrido del autor
(1141 tareas) son 33 cortes, los 11 recentrados de cámara con sus dos o
tres cuadros; caminando, los grupos movidos no pasan del 3 % y la mediana
de los desplazamientos de 28 px (percentil 99). GoldenEye contra la verdad
igual (29,76 dB).

### Destellos que no van en la dirección correcta

En la escena del Gran Árbol Deku que cuenta el origen (estado del autor en
la ranura 3, campo 66299; es una escena de historia y se repite igual en el
host) caen unas 150 chispas alargadas que salen en todas direcciones. En
algunos intermedios una chispa deja una raya larga que no está en ningún
cuadro: en el cuadro anterior estaba casi pegada a la cámara (más cerca
que el plano cercano, z + w < 0 con w > 0), y la interpolación desde ahí
dispara su proyección. Ahora una instancia chica de un modelo repetido (las
partículas) con un vértice que cruza el plano cercano entre los dos
cuadros no se interpola (`VMATCH_NEAR`, «cruzan el plano cercano»): 7,6
por tarea en esa escena, 1,3 caminando por el bosque. Para paredes, piso y
grupos únicos no: se interpolan bien (GoldenEye contra la verdad 29,76 →
29,07 dB si también se rechazan los grupos chicos únicos; así, igual).
Se probó también elegir la instancia más cercana (con la velocidad de cada
una del cuadro anterior) en vez de la del mismo lugar en el orden: en esta
escena no mejora (las chispas que se mueven fuera de su eje, 3,6 % con las
dos reglas y 4,5 % con «la más cercana» sin velocidad), así que quedó la de
antes.

### El piso sin fluidez en el árbol Deku

«A veces cuando camino, el suelo no tiene la fluidez» (dentro del árbol
Deku). Con las entradas del autor en el host la geometría se interpola
igual de bien que en el resto (la regla de malla no se dispara: 0,0 grupos
por tarea), pero en el log de la PS3 de esa caminata hay ventanas con
13–81 campos saltados de 300 (la presentación, 8–12 ms por campo a 5×,
más el redibujo de los intermedios no alcanza): el piso va a tirones
cuando la consola salta campos, no por el emparejamiento. Con «interp 7»
a 720p y 3×, el autor: «la interpolación en Zelda mejora mucho»: a 5× el
RSX no alcanza. Idea para después: con la interpolación encendida, avisar
en «Imagen» que a 5× el RSX no alcanza, o sugerir una escala menor.

### Estimar la cámara entre los dos cuadros (plan, sin hacer)

Para el mundo quieto, las coordenadas de recorte de un vértice en los dos
cuadros cumplen antes = T · ahora con una sola 4 × 4 T = P V_antes V_ahora⁻¹
P⁻¹ (P la proyección, V la vista). T se puede estimar en el PPE por mínimos
cuadrados con una muestra de vértices emparejados (C = Σ ahora·ahoraᵀ,
D = Σ antes·ahoraᵀ, T = D C⁻¹: dos 4 × 4 acumuladas y una inversa; con 256
vértices, unos 16 000 flops), y una o dos pasadas sacando los que se
apartan (Link, enemigos, partículas). Con T:

- Grupos nuevos (el piso que entra por el borde, lo que aparece): su
  posición de antes es T · ahora, así se mueven con el mundo y la regla de
  malla ya no necesita tumbar a sus vecinos.
- Giros de golpe de la cámara (Z en Zelda): en vez del corte, girar el
  mundo quieto con una potencia fraccionaria de T (T es semejante a una
  rotación más traslación: T^t = P R^t P⁻¹, R^t por slerp) en cada campo
  intermedio. Necesita separar P (en Zelda la vista va dentro de la
  proyección con lookAt) o sacar T^t por descomposición propia de la 4 × 4
  (autovalores e^{±iθ}, 1, 1). El corte queda de respaldo si la estimación
  no cuadra (residuo grande, pocos vértices quietos).
- Costo: la estimación por tarea cabe en unos 0,05 ms del PPE con la
  muestra; T^t por campo intermedio es una 4 × 4 por campo, pero aplicarla
  a todos los vértices del mundo quieto en vez de la recta del vertex
  program pide otro vertex program (matriz por dibujo, no por vértice) o
  posiciones por campo desde el PPE.
- Riesgos: escenas casi sin mundo quieto (todo se mueve), cámaras con FOV
  que cambia (P no constante: T deja de ser semejante a una rotación),
  vértices del mundo que el juego escribe ya transformados.

### Modo híbrido con matrices (estimación, sin hacer)

Lo que hoy se rechaza por aplastarse (giros de más de un cuarto de vuelta
en un cuadro: Link que se da vuelta, un enemigo que gira, una rueda) sale
en el cuadro de ahora, o sea a los cuadros del juego en ese momento. Un
modo híbrido lo interpolaría bien: solo para esos grupos, en vez de la
recta entre las dos posiciones, la matriz del grupo interpolada (giro por
slerp del cuaternión, traslación y escala en línea recta) aplicada a las
coordenadas de recorte de ahora: C_t = (P M)_t · (P M)_ahora⁻¹, una 4 × 4
por grupo y por campo intermedio, lineal en el vértice (los del recorte
cercano siguen en su recta). Los demás grupos, como ahora.

- SPE 0: la HLE ya tiene la matriz del modelo y la proyección en cada G_VTX
  (`mv`, `proj` de gfx_state); el registro GFX_OP_VGROUP llevaría la
  modelview (64 bytes más por grupo: ~19 KB por tarea en Zelda) y la
  proyección cuando cambia. Unos 300–500 bytes de código (holgura hoy 3648)
  y casi nada de tiempo.
- PPE: descomponer las dos matrices de cada grupo con giro grande (polar
  3 × 3 → cuaternión, unas pocas iteraciones: ~2–4 µs por grupo en el PPE)
  solo en los grupos que hoy se aplastan (0–60 por tarea, casi siempre 0):
  0–0,2 ms por tarea cuando pasa. Por campo intermedio, las posiciones de
  esos vértices en la t del campo (0,01–0,05 ms): como la t sale de la
  duración del cuadro, se escribirían por campo en el búfer de «antes»
  (prev' = (p_t − t · ahora) / (1 − t)), con un búfer por campo para no
  pisar lo que el RSX lee. El vertex program no cambia.
- Riesgos: cada juego pone la cámara en otro lado (Zelda la mete en la
  proyección con lookAt; Mario 64 en la modelview), así que hay que
  interpolar P · M entero o separar la proyección en perspectiva y giro;
  slerp toma el camino corto (una media vuelta exacta puede girar por el
  otro lado, y más de media vuelta por cuadro sale al revés); matrices con
  cizalla o reflexión (escala negativa) no se descomponen bien y quedarían
  en el cuadro de ahora; los microcódigos de Rare (gSPInsertMatrix, la
  modelview de Perfect Dark) y los vértices que el CPU escribe ya
  transformados (sin matriz útil) siguen como hoy. No arregla la
  identidad: un grupo mal emparejado gira igual de mal.
- Lo que arregla: los personajes que se dan vuelta, lo que gira rápido y
  la perspectiva en giros de cámara cerca de la pantalla, sin aplastarse
  ni saltar al cuadro de ahora. Antes de la PS3, en el host: el giro de
  Link de la tarea 107 del estanque como prueba. Unos 3–5 días.

## Reproducir en la Mac

```
make -C src/host OUT=$PWD/build/host-mac CC=clang SAN= -j8
cc -O2 -o build/ppmtool tools/ppmtool.c -lz
# GoldenEye: estado en la presa y la comparación con la verdad (k = 3)
build/host-mac/salsa64-run -q --pad "$(cat docs/pads/goldeneye-dam.pad)" \
    --state-save 5300:build/interp/ge/f5300.st --fields 5300 goldeneye.z64
build/host-mac/salsa64-run -q --state-load build/interp/ge/f5300.st \
    --pad "5300:-@0,70;5360:-@50,50;5450:-@-60,30;5540:-@0,0" \
    --trace-from 5340 --interp build/interp/ge/final3 --interp-decimate \
    --interp-k 3 --fields 5560 goldeneye.z64
tools/interp-video.sh build/interp/ge/final3 3 42 216 decimate
# Zelda: el vuelo de Navi (partida nueva) sin verdad, a 20 cuadros
build/host-mac/salsa64-run -q --pad "$(cat docs/pads/zelda-casa-nueva.pad)" \
    --save-dir build/interp/oot/partida --state-save 9250:build/interp/oot/f9250.st \
    --fields 9250 oot.z64
build/host-mac/salsa64-run -q --state-load build/interp/oot/f9250.st \
    --save-dir build/interp/oot/partida --pad "$(cat docs/pads/zelda-casa-nueva.pad)" \
    --trace-from 9420 --interp build/interp/oot/final --interp-k 3 --fields 9900 oot.z64
cp tools/interp-index.html build/interp/index.html
```

- `--interp DIR`: desde `--trace-from`, por cada cuadro del juego escribe
  `o-T.ppm` (lo que dibujó la tarea T), `gi`/`ge` (geometría interpolada y
  extrapolada), `ii`/`ie` (imagen), los mapas de error `eg`/`ei` contra la
  verdad, `metricas.csv`, `tareas.csv` y `resumen.txt`.
  `SALSA64_INTERP_SIN_IMAGENES=1` solo cuenta.
- `--interp-k K` (campos por cuadro), `--interp-decimate` (1 de cada K
  tareas es el cuadro del juego), `--interp-disp PX` (umbral de corte, 120).
- `SALSA64_INTERP_BASE_FINAL=1` dibuja los intermedios del host sobre el
  cuadro final (como la PS3), `SALSA64_INTERP_T0=1` agrega `g0-T.ppm` (todo
  en las posiciones de antes: debería dar el cuadro anterior),
  `SALSA64_INTERP_COMO_RSX=1` recorta todo en w > 0 como el RSX,
  `SALSA64_INTERP_GRUPOS=1` vuelca cada grupo, su pareja y el motivo
  (`grupos-T.txt`), `SALSA64_INTERP_SIN_MALLA=1` apaga `vmatch_atomic` y
  `SALSA64_INTERP_VOLCAR=1` las tareas 20 y 21 para
  `tools/vmatch-bench.c`; `ppmtool flicker A M B` cuenta lo que cambia solo
  en un intermedio.
- En RPCS3 (pasos 1–3, sin presentación nueva): `tools/rpcs3-run.py
  build/salsa64/salsa64.elf --embedded gfxtest --bandera interp-prueba=0`
  dibuja cada tarea un cuadro atrás (el cubo); con `=1`, igual que sin el
  archivo, y el log de cada 5 s trae la línea «gfx: interpolación».
- `SALSA64_DP_CYCLES=3000000` hace de tope de 30 en GoldenEye y Perfect
  Dark; `-DSALSA64_CPI=2` (en `EXTRA_CFLAGS`) cobra dos ciclos por
  instrucción.
- Limitaciones del rasterizador del host (no de la interpolación): el
  cielo de Zelda sale plano y el piso parpadea en algunos cuadros; la intro
  de Perfect Dark en alta resolución sale recortada a 320 × 240.

## Referencias y licencias

- **RT64** (renderizador de N64 de los ports de PC, github.com/rt64/rt64,
  licencia MIT verificada en su `LICENSE`): se leyó su diseño como
  referencia, sin copiar código. Empareja llamadas de dibujo y matrices
  entre cuadros por identificadores que los parches de cada juego ponen en
  un GBI extendido (grupos de matrices con interpolación por componente:
  posición, rotación, escala), con un modo automático por hash y
  puntuación; descompone las matrices (cuaterniones) salvo en las cámaras;
  descarta la traslación si la velocidad cambia demasiado; interpola el
  desplazamiento de las tiles; interpola con un cuadro de retraso, sin
  extrapolar. Lo de aquí es más simple porque la HLE ya da posiciones de
  recorte: se empareja por vértice, sin parches.
- **Ship of Harkinian** (port de PC de Zelda OoT): el juego graba un árbol
  de llamadas con claves puestas a mano en su código; no aplica a un
  emulador (no se verificó su licencia; solo se leyó la idea).
- No se encontró ningún emulador de N64 con interpolación automática sin
  parches.
