# La jerarquía de memoria del PPE y la emulación del VR4300

Plan para exprimir cachés, ERAT y predicción de saltos del PPE en el código
que genera el recompilador (docs/rec-ppe.md). Es un documento de diseño y
de prioridades, no de implementación: cada palanca lleva su mecanismo, la
ganancia estimada con el razonamiento y los datos que la sostienen, el
costo y el riesgo, cómo medirla y de qué depende. Al final, el orden
recomendado. Escrito el 2026-10-12 sobre lo medido en la PS3 real hasta
esa fecha (docs/perf-log.md) y los perfiles por juego de la misma noche.

## 1. Lo que hay: el PPE en números

Cifras públicas del PPE del Cell (Slim CECH-3011A: el mismo núcleo a
3,2 GHz). **[pub]** viene de la Cell Broadband Engine Architecture v1.01
(IBM/Sony/Toshiba, 2006), del Cell Broadband Engine Programming Handbook
(IBM, v1.1, 2007; «PPE microarchitecture», apéndice A con los costos de
instrucción) y del resumen de 7-cpu.com (medidas en hardware); **[med]** se
midió en este proyecto.

| Dato | Valor | Origen |
|---|---|---|
| Núcleo | 1 núcleo, 2 hilos SMT, **en orden**, 2 instrucciones por ciclo como máximo; enteros 2 ciclos de latencia, FPU 10, `mulld` ~11–15 | [pub] |
| L1 instrucciones | 32 KB, 2 vías, líneas de 128 bytes, 3 ciclos; **no está garantizado que su contenido esté en la L2**; cola de precarga de instrucciones (IPFQ) de 2 entradas compartida por los dos hilos | [pub] |
| L1 datos | 32 KB, 4 vías, líneas de 128 bytes (sectores de 32), 5 ciclos con acierto en el ERAT; **write-through sin asignar al escribir** (todo almacenamiento baja a la L2 por una cola de 16 entradas); 8 fallos en vuelo | [pub] |
| L2 | **512 KB compartida por los dos hilos**, 8 vías, líneas de 128 bytes, write-back, a la mitad del reloj; **~41 ciclos** un acierto (36 más que la L1); 6 fallos en vuelo; 8 flujos de precarga de datos por software (`dcbt` con TH) | [pub] |
| XDR | 25,6 GB/s; un fallo de L2 cuesta **~120 ns más** (unos 380 ciclos); un fallo de TLB con recorrido de la tabla de páginas, ~240 ns más (unos 770) | [pub] |
| ERAT | 64 entradas de instrucciones y 64 de datos, 2 vías, compartidas por los hilos; **cada entrada cubre 4 KB aunque la página sea grande**; alcance 256 KB cada uno; un fallo con acierto en la TLB cuesta **~24 ciclos** más | [pub] |
| TLB | 1024 entradas unificadas, 4 vías; páginas de 4 KB y dos tamaños grandes entre 64 KB, 1 MB y 16 MB | [pub] |
| Saltos | tabla de historia de 4 K × 2 bits con 6 bits de historia global por hilo, pila de enlaces de 4 entradas; **24 ciclos** por salto mal predicho; sin predicción de destino para `bctr` (el destino se conoce al ejecutar) | [pub] |
| Microcódigo | formas con punto, desplazamientos por registro, `lwa`/`lha`, `mtcrf` de varios campos, `lmw`/`stmw`: vacían la tubería, ≥ 11 ciclos; `mfcr` ~34; acceso que cruza 32 bytes, 55 | [pub], docs/rec-ppe.md |
| Load-hit-store | una carga que alcanza un almacenamiento todavía en la cola no se reenvía: decenas de ciclos (30–40) | [pub], [med] |
| Páginas hoy | RDRAM en páginas de 1 MB (heap); `struct salsa64_rec`, `n64` y la **caché de código en 64 KB**; pila en 4 KB (syscall 351, log al arrancar) | [med] |
| Gestión de reemplazo (RMT) | la CBEA define una tabla de reemplazo por caché (L2 y TLB) y, para el PPE, registros de rango (RSR, RMR, CIDR) que asignan una clase a un rango de direcciones; **todo en el entorno privilegiado**: SPR y MMIO «mapeados como páginas privilegiadas» (CBEA §18–19) | [pub] |

Dos consecuencias que mandan sobre todo lo demás:

- **El código es el dato más grande.** Cada instrucción MIPS emulada son hoy
  ~21 palabras de PPC (8–9 ejecutadas, el resto caminos fríos pegados al
  cuerpo): 84 bytes, dos tercios de una línea, por instrucción emulada. La
  N64 ejecuta ~50 M de instrucciones por segundo en GoldenEye: el PPE lee
  del orden de **4 GB/s de código** emulado. Ningún dato del juego se le
  acerca.
- **El ERAT de instrucciones alcanza 256 KB** cualquiera que sea el tamaño
  de página. Una zona caliente de 1 MB (256 páginas de 4 KB para 64
  entradas) falla el ERAT en casi cada entrada a un bloque; una de 256 KB
  cabría justa. Esto explica una parte de por qué juntar los 762 bloques
  calientes de GoldenEye en 1 MB no se notó (docs/perf-log.md, «Perfil
  persistente y zona caliente»).

## 2. Dónde se van los ~54 ciclos por instrucción

GoldenEye en la presa: 17 ns por instrucción MIPS en el código nativo
(54 ciclos), con 8–9 instrucciones de PPC ejecutadas por instrucción MIPS.
En orden y con dependencias cortas eso son **10–14 ciclos de trabajo**;
los **otros ~40 son espera**. Nadie los ha medido por separado (los
contadores del PMU del Cell son privilegiados) y este reparto es una
estimación hecha con las cifras de arriba y los perfiles. Lo que vale es
el orden de magnitud y qué palanca toca cada parte; la sección 5 propone
la medición que los separa.

Datos de los perfiles de la PS3 del 2026-10-12 (`perfiles/*.prf`, puntaje =
entradas a bloques; y el volcado de GoldenEye hasta la presa en el host):

| | GoldenEye | Perfect Dark | Zelda OoT | SM64 | Conker | DK64 | MK64 |
|---|---|---|---|---|---|---|---|
| Instrucciones por entrada a bloque | 12,4 | 15,2 | 15,7 | 19,1 | 16,9 | 19,0 | 16,8 |
| Palabras de PPC por instrucción, pesadas por entradas | 22,3 | 20,6 | 21,1 | 20,6 | 20,0 | 22,4 | 17,7 |
| Entradas a bloques de ≤ 4 instrucciones | 28 % | 36 % | 28 % | 20 % | 24 % | 24 % | 28 % |
| 256 KB de código generado (los bloques de más puntaje) cubren | 66 % de las entradas, 61 % del costo | 61 / 53 | 65 / 59 | 54 / 55 | 65 / 55 | 54 / 53 | 65 / 57 |
| 1 MB | 87 / 86 | 82 / 79 | 88 / 88 | 85 / 86 | 83 / 78 | 81 / 82 | 85 / 83 |
| 4 MB | 98 / 98 | 97 / 97 | 99 / 98 | 99 / 99 | 96 / 96 | 97 / 97 | 99 / 99 |
| La mitad del costo (entradas × palabras) está en | 85 bloques, 252 KB | 82, 331 KB | 44, 256 KB | 65, 285 KB | 73, 352 KB | 66, 312 KB | 75, 276 KB |
| El 90 % de las entradas, en páginas de 4 KB de código MIPS | 110 | 148 | 81 | 94 | 119 | 113 | 114 |

Cómo terminan los bloques (GoldenEye, los 4000 bloques más ejecutados del
volcado del host, pesado por ejecuciones): condicional 38 %, J/JAL 32 %,
**JR/JALR 24 %**, otro 6 %. Por instrucción MIPS: 0,23 cargas, 0,10
almacenamientos, 0,085 de COP1. Los sitios de salida del perfil de la PS3:
el 82 % de las salidas de los bloques calientes van por su sitio dominante,
391 de 594 bloques calientes tienen un sucesor que recibe el 90 % o más de
sus salidas, y de 931 condicionales con cien pasadas o más, 796 se toman
más del 80 % o menos del 20 % de las veces.

Con eso, el reparto estimado de los ~40 ciclos de espera por instrucción
MIPS en GoldenEye:

| Causa | Mecanismo | Estimación | Palancas |
|---|---|---|---|
| **Fetch de código** | 0,3–0,4 líneas de 128 bytes por instrucción (cuerpo más lo frío que arrastra); el conjunto que corre por campo (≥ 1 MB con el 87 % de las entradas) no cabe en la L1 de 32 KB ni, con los datos, en la L2 de 512 KB: casi cada línea es un acierto de L2 (41) o un fallo (380). Con 85 % de aciertos en L2 y la precarga secuencial tapando una parte | **12–25 ciclos** | 3.1, 3.2, 3.5 |
| **ERAT de instrucciones** | un fallo (24 ciclos) por cada entrada a un bloque que no esté en las 64 páginas vigentes: con el código repartido por 20 MB, la mayoría de las entradas; un bloque son ~12 instrucciones | **1–2 ciclos** | 3.1, 3.2 |
| **Datos del juego** | 0,33 accesos a la RDRAM por instrucción; la RDRAM está en páginas de 1 MB pero el ERAT de datos cubre 256 KB: fallos de ERAT frecuentes (24) y fallos de L2 en lo que el juego recorre por campo (380). La N64 real también esperaba a su RDRAM; es el costo más difícil de bajar | **8–15 ciclos** | 3.6 (solo las tablas del emulador) |
| **JR/JALR** | el 24 % de los bloques terminan en un destino calculado: `exit_jr` hace 2–4 cargas dependientes en tablas grandes (`xhash` 32 KB, `xb` 512 KB, `blocks` 1 MB; en KSEG0, `map` de 4 MB) y un `bctr` que el PPE no predice (24) | **3–6 ciclos** | 3.3 |
| **Entrada y salida de cada bloque** | entrada: la prueba del evento va en registros (r27 guarda `sched->next`: 3 palabras), pero un bloque mapeado carga la palabra de la `ktlb` y compara (4–5 palabras, ~10 ciclos de latencia que en orden no se solapan) y uno con COP1 carga `status` (otros ~8); salida: `std` de los registros modificados y, en la entrada siguiente, `ld` de los vivos: load-hit-store cuando el siguiente lee lo que el anterior acaba de escribir (~40). Dentro del bloque, con los registros fijos solo quedan 4 registros por bloque (r14–r17): lo demás va y viene de `cpu->gpr` en cada uso, con el mismo load-hit-store si se escribe y se lee a pocas instrucciones. Por ~12 instrucciones | **2–4 ciclos** | 3.2, 3.4 |
| **Saltos mal predichos** | un salto por bloque más los internos; con 85–90 % de acierto de la tabla de historia, 24 ciclos por fallo | **1–2 ciclos** | 3.4 |
| Microcódigo que queda | `sllv` y compañía, LWL/LWR sueltos, `mulld`; el resto ya se quitó (docs/rec-ppe.md, «Lo que el PPE hace en microcódigo») | < 1 ciclo | 3.9 |

El total cuadra con los ~40 de espera; el reparto entre fetch de código y
datos del juego es lo menos seguro, y es justo lo que la medición de la
sección 5 separa. Lo que sí se sabe de las mediciones en la PS3: (a)
cambiar las instrucciones que se emiten sin cambiar el tamaño no mueve los
47 ciclos (FPU residente, sin microcódigo, COP0 con rutinas); (b) mover
762 bloques calientes a 1 MB contiguo tampoco (los bloques siguen en 256
páginas, el conjunto sigue sin caber); (c) el hilo 1 recorriendo 512 KB
cuesta solo 0,13 ms por ms (la L2 ya está perdida para el conjunto caliente
entero; lo que compite es el margen).

## 3. Palancas

Cada una: mecanismo, ganancia estimada (sobre el tiempo del código nativo
de GoldenEye en la presa, 79 % del hilo 0), costo y riesgo, cómo medirla y
dependencias. Las ganancias no se suman: varias atacan la misma espera.

### 3.1 Código compacto y la sección fría fuera del cuerpo

**Mecanismo.** Menos palabras por instrucción son menos líneas por
instrucción, en la L1, la L2 y el ERAT a la vez. La rama del código
compacto baja de 20,2 a 15,6 palabras en el host (prueba de KSEG0 en 4
palabras, un camino lento por bloque, cadena de escritura, enlaces de 3):
−12 % en lo que se ejecuta y −33 % en lo frío (docs/perf-log.md, «Código
compacto»). Lo frío importa aunque no se ejecute: va pegado al cuerpo, a
`bc` de ±32 KB, y cada línea del cuerpo arrastra la fría vecina a la L1 y
la L2 (la caliente es el 44 % de las palabras del volcado: más de la mitad
de lo que entra a la caché no se ejecuta nunca).

Lo que sigue a esa rama, en orden:

1. **La sección fría en otra zona de la caché.** Hoy el cuerpo y su sección
   fría son contiguos. Ponerlas aparte (todas las secciones frías en un
   tramo propio, o al final de la caché) hace que el cuerpo de un bloque y
   el del siguiente queden en líneas vecinas y que la precarga secuencial
   de la L1 de instrucciones (IPFQ, dos líneas) sirva de algo. Costo: cada
   `bc` al camino frío pasa a ser `bc` a un trampolín `b` lejano (una
   palabra por camino frío, en la zona fría; `bc` invertido sobre un `b` en
   línea cuesta una palabra en el cuerpo y es peor) o se reserva una
   ventana de ±32 KB (cuerpos y frías intercalados por tramo de 32 KB:
   sin trampolines, con la fría a 16 KB de media). Riesgo bajo; es solo
   colocación. Ganancia: con la fría fuera, las líneas por instrucción del
   cuerpo pasan de ~0,65 (21 palabras) a ~0,3 (8 palabras): la mitad del
   tráfico de código. **Estimación: 5–12 % del nativo.**
2. **Frío de verdad para lo frío.** Con el perfil cargado, los bloques que
   no son calientes se compilan sin superbloques, sin FPU en línea y con la
   rutina de `ktlb` en vez del camino rápido de datos mapeados (docs/rec-ppe.md,
   «Siguientes pasos», 2): la mitad de las palabras por bloque frío. Como
   los fríos son el 13 % de las entradas pero el 90 % de los bloques, lo
   que ganan es que, cuando corren, roban la mitad de líneas al conjunto
   caliente. **Estimación: 2–4 %.**
3. **Zona caliente de 256 KB, no de 1 MB.** Los 64 KB más calientes dan
   el 34 % de las entradas en GoldenEye, los 256 KB el 66 %. Una zona de
   256 KB (64 páginas) es la que cabe en el ERAT de instrucciones y la que
   puede quedarse en la L2 junto con los datos. `caliente = 256` en
   `perfil.cfg` ya existe y no se midió. Con la fría fuera del cuerpo, en
   256 KB entran los cuerpos de ~1100 bloques (el 90 % de las entradas).
   **Estimación: 2–5 %**, solo si el ERAT pesa (sección 5 lo dice).

**Medición.** A/B con `jit-ancho` para lo que ya existe; para la fría
fuera del cuerpo, la misma bandera con el mismo paquete. Lo que debe
moverse es el nativo en ns por instrucción (`tools/jit-perfil.py`), no los
campos/s. **Dependencias:** la rama del código compacto (en curso); el
perfil cargado para el punto 2 y 3.

### 3.2 Trazas: el sucesor dominante sin salida ni entrada

**Mecanismo.** El 82 % de las salidas de un bloque caliente van por un
sitio; 391 de 594 tienen un sucesor con el 90 % o más. Hoy cada frontera
cuesta: escritura de los registros modificados (`std` por registro),
enlace (`b`), y en el destino la entrada completa (página de la `ktlb`,
evento, CU1/FR: hasta tres cadenas carga-compara-salta, 7–13 palabras y
~10–20 ciclos de latencia que en orden no se solapan) y la carga de los
vivos (`ld`, con load-hit-store si el anterior acaba de escribirlos). Compilar el sucesor dominante **dentro** del mismo
bloque (como ya se hace con los saltos condicionales hacia adelante y los
destinos internos) quita todo eso en la arista frecuente: los registros
siguen en r14–r25, el evento se revisa una vez con la suma de ciclos de
los dos, la página de la `ktlb` solo si cambia de página. La arista rara
sale como hoy. Es el «JIT por niveles» de docs/rec-ppe.md (1b) reducido a
lo que el perfil ya sabe: J/JAL (32 % de los finales de bloque: el destino
es fijo y el sucesor, seguro), condicionales sesgados (796 de 931) y el
camino no tomado del salto final.

**Ganancia.** Entrada y salida son ~12–18 palabras por bloque de 12
instrucciones (1–1,5 palabras por instrucción de las 8,2 ejecutadas) más
las esperas: 10–20 ciclos de entrada y el load-hit-store ocasional. Fundir
dos bloques de cada tres en la arista dominante (las J/JAL y los sesgados)
quita la mitad de las fronteras: **4–8 % del nativo** por las palabras y
las esperas, y encima lo de 3.1.1 (los cuerpos quedan contiguos de forma
natural). Es también lo que hace que los registros de la N64 vivan en
registros del PPE «entre bloques»: no hace falta un reparto global de
registros, basta con que la traza sea un bloque. Lo que una traza larga sí
pide es **más registros por bloque**: con los fijos (sp, ra, v0, v1,
a0–a3 en r18–r25) quedan cuatro (r14–r17) y lo demás pasa por `cpu->gpr`
en cada uso; s0, at y t6 aparecen en el 26–31 % de los bloques. El préstamo
de fijos que ya existe (guardar al entrar, reponer al salir) cubre los
bucles; en una traza de 30–60 instrucciones conviene repartir por tramos
(un registro del PPE puede servir a dos registros MIPS que no viven a la
vez) antes que ampliar el préstamo.

**Costo y riesgo.** Alto en el emisor: límite de instrucciones por traza
(64 hoy; subirlo a 128–192), la huella del sucesor comprobada al compilar
(el perfil la guarda), invalidación de la traza entera si cambia cualquiera
de sus bloques (ya pasa con los superbloques), y el JAL deja `ra` como
hoy. Riesgo de exactitud: el mismo que los superbloques, cubierto por
`--lockstep`. Riesgo de rendimiento: una traza que se abandona pronto
cuesta la compilación del sucesor que no corre; por eso solo con el perfil
(umbral del 80 %) y nunca a ciegas.

**Medición.** Entradas a bloques por campo (`entries` del perfil JIT: deben
bajar a la mitad en la presa) y ns por instrucción del nativo. **Depende**
de 3.1 (para no fundir código ancho) y del perfil cargado. Hacerlo en el
hilo 1 (traducción por adelantado) es independiente y puede venir después.

### 3.3 JR/JALR: pila de retorno y caché de destinos

**Mecanismo.** El 24 % de los bloques terminan en JR o JALR, casi todos
`jr ra` (retorno de función). `exit_jr` busca el bloque destino en tablas
que no caben en la L2 (fuera de KSEG0, GoldenEye y Perfect Dark: `xhash`
por vpc y la cadena en `xb[]`; en KSEG0, `map[]` de 4 MB por física, 2
bytes por palabra de la RDRAM: una línea distinta por destino) y termina
en un `bctr` que el PPE no predice. Dos piezas:

1. **Pila de retorno sombra.** En cada JAL/JALR el código apila el par
   (vpc de retorno, palabra de entrada del bloque de retorno) en una pila
   chica en `struct salsa64_cpu` (16 entradas, 256 bytes: dos líneas que
   viven en la L1); en `jr ra` desapila, compara el vpc con `ra` (una
   carga, una comparación) y salta; si no coincide, cae a `exit_jr` como
   hoy. El bloque de retorno es el que sigue al JAL en el mismo bloque
   (se conoce al enlazar); si no está compilado, se apila 0 y el JR va a
   `exit_jr`. Los cambios de hilo de libultra (`osYieldThread` y el
   despachador guardan y reponen `ra` por memoria) hacen fallar la
   comparación, nunca aciertan mal: la pila es una pista, el vpc manda.
2. **Caché directa de destinos** para lo que la pila no cubre (`jr` por
   tabla de saltos, `jalr`, retornos tras un cambio de hilo): una tabla
   de 1024 entradas × 8 bytes (vpc, entrada) indexada por bits de la vpc
   (8 KB, cabe en la L1), delante de `xhash`/`map`. Una carga y una
   comparación en el camino de acierto.

Con lo dos, `bctr` sigue: el PPE no predice su destino. Alternativa de
hardware: emitir el JAL como `bl` al bloque destino y el `jr ra` como
`blr` guardado por la comparación, para usar la pila de enlaces del PPE
(4 entradas) y que el retorno llegue predicho. Es más invasivo (LR deja de
estar libre para las llamadas a C: hay que salvarlo en `rec_lr` en cada
camino lento, que hoy ya lo hace) y la pila de 4 entradas se pierde en
cuanto hay una llamada a C en medio; dejarla para después de medir 1 y 2.

**Ganancia.** Por JR: de 2–4 cargas dependientes en líneas frías (~100–200
ciclos cuando `xb`/`blocks` no están en la L2; más con `map[]`) a ~10. Con
0,02 JR por instrucción, **3–6 % del nativo** en GoldenEye y Perfect Dark;
más en los juegos de KSEG0 (SM64, Zelda, MK64), que hoy van sobrados.
**Costo** bajo (una semana de emisor y despachador, tablas en `salsa64_cpu`
y `salsa64_rec`; la pila se vacía en cada vaciado de la caché y en cada
invalidación del bloque de retorno: basta con comparar la palabra de
entrada contra `blocks[id].code`, o vaciarla al invalidar). **Riesgo**
bajo: un acierto falso es imposible si se compara el vpc y se comprueba
que el bloque sigue vivo. **Medición:** contador de aciertos de la pila y
de la caché en las estadísticas; ns por instrucción. **Sin dependencias.**

### 3.4 Pistas de salto y colocación del camino probable

**Mecanismo.** El PPE predice con una tabla de historia de 4 K entradas;
con ~40 000 bloques vivos y un salto por cada 10 instrucciones, la tabla
se pisa. `bc` admite las pistas estáticas de la ISA (`a` y `t` en BO:
predicción fija cuando `a` = 1) y el emisor ya las pone por convención
(camino lento = improbable). Con las cuentas del perfil (796 de 931
condicionales sesgados) se emite cada condicional con la pista que dice su
cuenta y, cuando casi siempre se toma, invertido: el camino tomado en
línea y el no tomado como salida. Lo mismo para el salto final del bloque.
Es el punto 0 de «Siguientes pasos» en docs/rec-ppe.md.

**Ganancia.** 24 ciclos por fallo; si la tabla acierta el 85 % y las
pistas suben al 95 % en los sesgados, son ~2 ciclos por bloque: **1–2 %**.
Barato (un día), sin riesgo de exactitud. **Medición:** solo ns por
instrucción, A/B con una bandera. **Depende** del perfil cargado; va bien
junto con 3.2 (una traza elige el camino probable y con eso ya coloca).

### 3.5 Precarga del sucesor y del retorno

**Mecanismo.** El PPE no tiene `icbt`, pero `dcbt` de una línea de código
la trae a la L2, que es de donde la L1 de instrucciones la va a pedir. Al
enlazar un bloque con su sucesor (el `nop` que se vuelve `b`), el
despachador puede dejar antes de la salida un `dcbt` de la primera línea
del destino; emitirlo en la entrada del bloque (cuando el destino es fijo:
J/JAL, condicional tomado) le da a la línea ~100 ciclos de ventaja en un
bloque de 12 instrucciones, suficiente para un acierto en L2 (41) y parte
de un fallo (380). Para el retorno, la pila de 3.3 sabe el destino en el
JAL: un `dcbt` del bloque de retorno al entrar a la función. Para los
datos del juego no hay precarga posible: la dirección se calcula una o dos
instrucciones antes del acceso.

**Ganancia.** Tapa la latencia del primer fetch de cada bloque (uno por
cada ~12 instrucciones): si hoy cuesta 41–65 ciclos, **3–5 %**. **Costo:**
tres palabras por salida con destino fijo (`addis`, `ori`, `dcbt`: la
dirección es absoluta y conocida al enlazar, se parchea junto con el `b`);
en los bloques de ≤ 4 instrucciones (28 % de las entradas) no hay ventaja
que ganar y conviene no emitirlo. **Riesgo:** gasta huecos de emisión y
ancho de banda de la L2; si el destino cambia (invalidación), el `dcbt`
queda apuntando a código viejo: inofensivo (solo trae una línea). Con 3.2
pierde sentido en las aristas fundidas y queda para las que no lo están.
**Medición:** A/B por bandera. **Depende** de 3.1.1 si se quiere que la
precarga secuencial del hardware haga el resto.

### 3.6 Estado del emulador y tablas: menos líneas, no menos bytes

Lo que el código generado toca fuera de la RDRAM, por línea de 128 bytes:

| Estructura | Tamaño | Qué toca el camino caliente | Hoy |
|---|---|---|---|
| `struct salsa64_cpu` | 2,9 KB | `gpr[32]` (líneas 0 y 1), `hi`/`lo`/`cycles`/`cur_pc`/`delay_slot`, la palabra `irq_pending`…`mode` y `status` (línea 2), `fpr[32]` y `fcr31` (líneas 3–5), `rec_kind`/`rec_site`/`rec_lr`/`rec_tmp` (línea 16, solo salidas y caminos lentos) | 6 líneas calientes, siempre en la L1 |
| `sched->next` | 8 bytes, en `struct salsa64_sched` a mitad de línea | vive en r27; se relee (dos cargas, `cpu->sched` y luego `next`) solo después de cada llamada a C | ya está resuelto |
| `ktlb[]` | 64 KB (16 384 × 4) | una palabra por entrada a un bloque mapeado; GoldenEye corre en ~21 páginas contiguas de 0x7F0xxxxx: una o dos líneas | caliente |
| `code_flags[]` | 64 KB (un byte por 128 bytes de RDRAM) | cada almacenamiento nativo lee el byte de su dirección: tan disperso como los datos del juego | 0,1 cargas extra por instrucción, muchas fuera de la L1 |
| `hits[]` | 256 KB | solo con el perfil: 4 bytes por entrada | 5 % medido: no va encendido |
| `blocks[]`, `xb[]`, `xhash[]`, `map[]` | 1 MB, 512 KB, 32 KB, 4 MB | solo `exit_jr` (3.3) y el despachador | frías |

Palancas chicas, todas baratas:

- **`code_flags` a un bit por 128 bytes** (8 KB en vez de 64: la prueba
  pasa a `lbz` + `rlwinm` + `and`, dos palabras más) o, mejor, **a 1 byte
  por 1 KB con el conteo aparte**: la lectura del almacenamiento solo
  necesita saber si hay código cerca, y 8 KB caben en la L1 de datos
  enteros. Hoy la tabla de 64 KB compite con los datos del juego y cada
  almacenamiento trae una línea que casi nunca sirve. **Estimación 1–2 %**
  (0,1 cargas por instrucción, de las que una parte falla hoy en L1).
  Riesgo: el conteo por bloque (saturado en 255) sigue haciendo falta
  para invalidar; vive en otra tabla que solo toca el compilador.
- **`ktlb` más chica**: no es palanca. Las entradas calientes caben en
  dos líneas; achicarla (por ejemplo, una tabla de 4096 entradas con
  etiqueta, «cambiar datos por trabajo») no ahorra líneas calientes y
  agrega una comparación. Dejarla.
- **Estado apretado**: ya está. Un bloque de KSEG0 enlazado, sin COP1 y
  sin perfil no lee ningún dato del emulador al entrar (el evento se
  prueba en registros); lo que lee el cuerpo (`gpr[]`, `status`,
  `cycles`, la palabra de interrupción, `fpr[]`) ocupa seis líneas que
  nunca salen de la L1. Lo único pendiente es que el despachador escribe
  `redirect` y `rec_abort` como bytes y el código los relee como palabra
  (`lwz` de los cuatro juntos): un reenvío parcial de la cola de
  almacenamiento en cada entrada desde C (520 por campo: dentro del
  ruido). Escribir la palabra entera lo quita.

### 3.7 El hilo 1 y los SPE: no ensuciar la L2 desde fuera

Medido (docs/perf-log.md, «Quitarle trabajo al PPE»): el hilo 1 le cuesta
a la emulación 0,13 ms por ms de trabajo, y con 3 ms por campo la L2 se
nota poco (+3,7 % en ns por ciclo). En la presa el hilo 1 trabaja 2,9 ms
por campo: ~0,4 ms de los 15,5, un 2,5 %. Lo que vale la pena:

- **La copia de la imagen del VI por un SPE.** La hace el hilo 0 al
  terminar el campo: 150–300 KB leídos de la RDRAM y escritos en la ranura
  (0,20–0,30 ms por campo, docs/hilos-ppe.md), que pasan enteros por la L2
  compartida y pisan 2–4 líneas de cada 8 de la mitad de la L2. Un trabajo
  de `lib/spukit` (como la tarea JPEG) copia por DMA sin tocar la L2 del
  PPE; el hilo 0 solo lo arranca y el hilo 1 espera su fin antes de subir
  la ranura. **Ganancia: 1,2–1,8 % del campo** (el tiempo de la copia) más
  lo que la L2 deje de perder, que no se sabe medir aparte. Costo bajo:
  un programa SPE de 100 líneas y una señal más entre hilos. Riesgo: el
  SPE lee la RDRAM mientras el hilo 0 sigue emulando el campo siguiente
  (hoy la copia también la hace mientras el juego dibuja el otro búfer;
  el VI apunta al campo terminado). Dependencia: ninguna.
- **Las copias RSX → RDRAM y conversiones del hilo 1** (0,4–1,5 ms por
  campo en GoldenEye y Perfect Dark) al mismo SPE: devuelven 0,05–0,2 ms
  al hilo 0 por la pendiente medida. Vale solo si el SPE ya está.
- **`dcbz` en los rellenos grandes del PPE** (borrado de la z image, DMA
  del PI de la ROM a la RDRAM, cebado de ranuras): evita leer la línea
  antes de escribirla (la mitad del tráfico a XDR de cada relleno) y no
  deja en la L2 líneas que nadie va a leer. Cambio de una línea por sitio
  con `memset`/`memcpy` propios; ganancia dentro del ruido salvo en las
  cargas de nivel.
- **La traducción al RSX en un SPE: no** (ya decidido: 0,25 ms por campo a
  cambio de rediseñar rsxkit).

### 3.8 Lo tibio: compilar a la segunda

**Mecanismo.** GoldenEye compila 36 000 bloques en 110 s y solo 1100 dan el
90 % de las entradas; una parte grande del resto corre una vez (código de
carga, inicialización de un nivel). Un contador en el despachador (ya pasa
por C la primera vez: `map[]`/`xhash` sin bloque) interpreta el bloque
básico la primera vez y compila a la segunda entrada. No es un intérprete
«compacto» nuevo: es el de siempre, que ya corre lo que no se puede
compilar (docs/rec-ppe.md, «Cuándo corre en nativo»).

**Ganancia.** Compilar cuesta 0,05–0,3 ms por campo en la presa y hasta
1,1 ms al entrar a una zona nueva; lo que se ahorra es eso en los bloques
de una sola pasada y, sobre todo, que esos bloques no ocupen caché ni
desalojen tramos (cada desalojo recompila lo vivo del tramo). **< 2 % en
juego, más en los tirones de carga.** Costo de un día. Riesgo: la
interpretación de un bloque cuesta ~5× su ejecución nativa; con umbral 2
se paga una vez. **Medición:** bloques compilados y desalojos por campo,
peor campo de la tanda al cargar una sala (ya se mide). **Depende** de
nada; combina con la traducción por adelantado en el hilo 1 (que compila
sin umbral lo que el perfil dice).

### 3.9 Lo que queda de microcódigo

Ya se quitó lo frecuente (`andi.`, `andis.`, `mtcrf`). Lo que queda: los
desplazamientos por registro de SLLV/SRLV/SRAV y de sus formas de 64 bits
(no hay otra forma; frecuencia baja fuera de rutinas de bits), LWL/LWR
sueltos (los pares van byte a byte), `mulld` para MULT/MULTU (11–15
ciclos, con pipeline detenido; MULT es frecuente en las rutinas de
matrices de libultra: ~1 % de las instrucciones), `mfcr` una vez por
entrada desde C (520 por campo: nada). No hay palanca aquí mayor del 1 %;
la única con sentido es MULT por `mullw` + `mulhw` cuando los operandos
caben en 32 bits (dos de 6 ciclos en paralelo en vez de un `mulld` de
15), y pide medir primero.

### 3.10 Lo que no es palanca

- **Páginas de 1 MB para la caché de código.** El ERAT cubre 4 KB por
  entrada sea cual sea la página; una página grande solo ahorra fallos de
  TLB, y la caché de 32 MB en páginas de 64 KB son 512 entradas de una
  TLB de 1024 (4 vías): aciertos salvo conflictos. Además la única memoria
  ejecutable con HEN es el segmento de texto que carga lv2 (docs/jit-ppe.md):
  no se elige su tamaño de página. Descartada; el ERAT se ataca con el
  tamaño del conjunto caliente (3.1.3).
- **La tabla de reemplazo de la L2 (RMT) y los registros de rango del
  PPE.** Servirían para reservar vías de la L2 al código caliente o para
  que las copias grandes no lo desalojen (clase con «bypass»). Son
  facilidades del entorno privilegiado: SPR que solo se escriben en modo
  privilegiado y MMIO «que el software privilegiado debe mapear como
  páginas privilegiadas» (CBEA v1.01, §18 y §19). Con HEN el programa
  corre en modo problema; llegar ahí pediría parchear lv2 (un syscall
  propio que ejecute `mtspr`), dependiente del firmware y sin
  documentación. Descartada mientras no haya un camino publicado; lo que
  hace la RMT se aproxima con 3.7 (no traer a la L2 lo que no se vuelve a
  leer) y con `dcbz`.
- **Precarga de datos del juego.** La dirección se conoce una o dos
  instrucciones antes; los flujos de `dcbt` sirven para recorridos
  lineales largos, que en el código del juego no se reconocen al compilar.
  Solo los rellenos y copias del emulador (3.7).
- **Un intérprete compacto para lo tibio** en vez del de siempre: el de
  siempre ya está medido a ~250 ciclos por instrucción y lo tibio pesa
  poco (3.8); un intérprete nuevo no se paga.

## 4. Qué sabe el perfil y qué le falta

El perfil por juego (`perfiles/<juego>.prf`, docs/rec-ppe.md) ya da lo que
3.1–3.5 necesitan: puntaje por bloque, palabras que ocupa, sitios de salida
con tomado/no tomado y los dos sucesores más frecuentes con su huella.
Lo que falta para decidir con datos:

- **Qué bloques terminan en JR** y a cuántos destinos distintos van (3.3):
  una bandera en la entrada (termina en JR/JALR) y, en los calientes, un
  contador de fallos de la pila y de la caché de destinos.
- **Cuántas veces entra cada bloque desde C** (`exit_jr` sin bloque,
  eventos) frente a por enlace: lo da el perfil por causa, no el
  persistente.
- **Puntajes por tiempo, no por entradas**, con el muestreo desde el hilo
  1 (docs/rec-ppe.md, «Siguientes pasos», 0): el contador por entrada
  cuesta 5 % y pesa igual un bloque de 2 instrucciones que uno de 60. Parte
  de ese 5 % es que el contador es leer-modificar-escribir (`lwz`, `addi`,
  `stw` sobre `hits[id]`): un bloque al que se vuelve a entrar a pocas
  instrucciones (una función corta llamada en bucle) relee lo que acaba de
  escribir, load-hit-store incluido. Si se mantiene un contador en el
  camino caliente, mejor `stw` de un valor que ya está en registro (el
  número de campo, para saber «visto en este campo») que un incremento.

## 5. Mediciones en la PS3

Sin PMU, la única forma de separar las esperas es construir casos que
aíslen una. Todas caben en el `bench` de cpu-tests (ya mide MIPS por
variante con `mftb`) y en un solo paquete; media hora de consola.

1. **Huella de código.** Un programa MIPS sintético: una cadena de N
   bloques de 8 instrucciones (ALU y `j` al siguiente, el último vuelve al
   primero), con N tal que el código PPC generado ocupe 16 KB, 64 KB,
   192 KB, 256 KB, 512 KB, 1 MB, 4 MB y 16 MB. Los ns por instrucción
   contra el tamaño dibujan la curva L1 → ERAT (salto en 256 KB) → L2
   (salto en ~400 KB) → XDR. Dice en qué régimen está GoldenEye (su
   conjunto caliente son 1–2 MB) y cuánto vale 3.1.3 y 3.5.
2. **Huella de datos.** Un bucle que lee M KB de la RDRAM con paso de 128
   bytes (M = 16 KB … 8 MB) y con paso de 4 KB (fallos de ERAT puros):
   el costo por acceso en cada régimen, para calibrar la fila «datos del
   juego» de la sección 2.
3. **JR.** Una cadena de K funciones distintas llamadas por `jal` y
   `jr ra` (K = 16, 256, 4096, 16 384) sobre la zona de datos de `xb`/
   `map`: el costo de `exit_jr` contra el tamaño del conjunto de destinos,
   antes y después de 3.3.
4. **Fronteras de bloque.** El mismo bucle de enteros del bench partido en
   bloques de 4, 8, 16 y 64 instrucciones (con `j` entre ellos): el costo
   de entrada y salida por bloque, que calibra 3.2.
5. **Cada palanca, A/B con el mismo paquete** por archivo-bandera en
   `/dev_hdd0/salsa64` (como `jit-ancho`, `jit-viejo`): GoldenEye en la
   presa 2 minutos, Perfect Dark la intro a 5× hasta la segunda escena,
   SM64 y Zelda 30 s; lo que se compara son los ns por instrucción del
   nativo en las ventanas pesadas (`tools/jit-perfil.py`), no los
   campos/s. Protocolo en docs/perf-log.md, «Código compacto».

## 6. Orden recomendado

| # | Palanca | Ganancia estimada (nativo de GoldenEye) | Costo | Por qué en este lugar |
|---|---|---|---|---|
| 1 | 3.1 Código compacto (rama en curso) y medirlo en la PS3 | 5–10 % | ya hecho; medir | Es la base: todo lo demás son líneas por instrucción |
| 2 | 5.1–5.4 Bench de huella, datos, JR y fronteras | — | 2 días + media hora de consola | Decide el reparto de la sección 2 y evita construir sobre una suposición |
| 3 | 3.1.1 La sección fría fuera del cuerpo | 5–12 % | 2–3 días | Colocación pura, sin tocar la exactitud; multiplica lo de 1 |
| 4 | 3.3 Pila de retorno y caché de destinos de JR | 3–6 % | 3–5 días | Independiente, barata, sin riesgo de exactitud; también para los juegos de KSEG0 |
| 5 | 3.1.3 Zona caliente de 256 KB y 3.1.2 frío de verdad para lo frío | 3–8 % | 1 día + 3 días | Con la fría fuera y el bench de huella se sabe cuánto vale el ERAT |
| 6 | 3.2 Trazas por el sucesor dominante | 4–8 % | 2–3 semanas | La de más alcance y más riesgo; después de 1–5 el emisor está compacto y el perfil probado |
| 7 | 3.4 Pistas de salto, 3.5 precarga del sucesor | 1–2 % y 3–5 % | 1 día y 2 días | Chicas y seguras; la precarga vale menos cuanto más fundan las trazas |
| 8 | 3.6 `code_flags` chica | 1–2 % | 1–2 días | Chica; de paso con 6 |
| 9 | 3.7 Copia del VI por SPE, `dcbz` | 1–2 % del campo | 2 días | Fuera del recompilador: puede ir en paralelo con cualquiera |
| 10 | 3.8 Compilar a la segunda | < 2 %, menos tirones | 1 día | Cuando se trabaje la traducción por adelantado |
| — | 3.9 microcódigo, 3.10 páginas grandes, RMT, precarga de datos | — | — | No son palanca, o no son accesibles |

Si todo sale como se estima, el nativo de GoldenEye en la presa baja de
~54 a ~35–40 ciclos por instrucción: de 46 a ~60 campos/s en las escenas
pesadas, que es la velocidad completa. Lo que quede después es de los
datos del juego, y ahí ya no manda el emulador.

## Fuentes

- Cell Broadband Engine Architecture, versión 1.01 (IBM, Sony, Toshiba,
  3 de octubre de 2006): §18 «PPE Address Range Facility», §19 «Cache
  Replacement Management Facility», apéndice E (páginas grandes).
- Cell Broadband Engine Programming Handbook, versión 1.1 (IBM, 2007):
  «PPE microarchitecture» (cachés, ERAT, predicción de saltos, precarga) y
  apéndice A (costos de instrucción y microcódigo).
- 7-cpu.com, «IBM Cell»: latencias medidas del PPE (L1, L2, ERAT, memoria),
  tamaños de ERAT, TLB y tabla de historia, penalización de salto.
- Power ISA 2.0x, Book I (pistas de salto en BO) y Book II (`dcbt`, `dcbz`,
  `icbi`).
- docs/rec-ppe.md, docs/perf-log.md, docs/hilos-ppe.md, docs/uso-cell.md,
  docs/jit-ppe.md y docs/memory-budget.md de este repositorio.
