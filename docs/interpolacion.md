# Interpolación de cuadros (diseño y prototipo)

Estado: **diseño con prototipo en el host** (2026-10-13). Nada de esto
corre todavía en la PS3; el plan para la consola está al final y espera la
decisión del autor.

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

## Plan para la PS3 (después de que se decida)

1. Tope de cuadros por juego con `dp_cycles` y el interruptor (sin
   interpolación todavía): medir en la consola el ahorro de CPU en la presa
   y en Perfect Dark en juego.
2. Geometría en el host completo: identidad por grupo (no por vértice),
   registros por grupo y el flujo de posiciones anteriores, igual que en la
   PS3; pruebas unitarias del emparejamiento.
3. Vertex program interpolado en rsxkit (genérico: «dos posiciones y t») y
   la superficie de presentación; redibujar la última tarea en los campos
   repetidos, primero en RPCS3 (que no sirve para medir tiempos).
4. SPE de interpolación (emparejamiento después de la tarea) y medición en
   la PS3 real: Zelda OoT en el bosque, SM64, Mario Kart, GoldenEye y
   Perfect Dark con tope; ms del RSX y del hilo 1, campos presentados.
5. Por imagen: búsqueda en el SPE con SIMD y deformación en el RSX, con el
   HUD aparte.

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
