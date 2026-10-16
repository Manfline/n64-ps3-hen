# Perfilado: datos de cada sesión de juego

La app **«Salsa64 perfil»** (APPID `SLSA00120`) es Salsa64 con todo el
registro de rendimiento encendido: cada partida en la PS3 real deja en el
disco lo que hace falta para optimizar después (dónde se va el tiempo del
recompilador, qué tirones hubo y por qué, qué hizo raro la HLE gráfica, cómo
anduvo el audio), sin banderas ni protocolos de medición. La app normal no
cambia: el código está en ella, pero apagado (el recompilador no lleva
`rec->trace` y su código generado es el de siempre).

Nada sale de la consola salvo el log UDP de siempre: los datos se bajan por
FTP. Los archivos de sesión **no llevan ni un byte del juego**: direcciones,
cuentas, tiempos, huellas de un solo sentido e ids. Los volcados automáticos
de la HLE sí (son la salida de la HLE con sus texturas): quedan entre la
consola y la Mac y no se comparten.

## Armar la app

```sh
rm -rf build/salsa64-perfil
tools/build.sh -C src/frontend PERFILADO=1 pkg
```

`PERFILADO=1` define `SALSA64_PERFILADO` y cambia el programa, el título y
el APPID (`build/salsa64-perfil/`, «Salsa64 perfil», `SLSA00120`); el menú
lo muestra arriba a la derecha («Salsa64 perfil · SLSA00120») y la primera
línea del log también. Comparte `/dev_hdd0/salsa64` con la app normal
(ROMs, partidas, estados, ajustes).

## Dónde queda

```
/dev_hdd0/salsa64/perfilado/<juego>-<CRC>/<n>.sesion     una por partida (n sube)
/dev_hdd0/salsa64/perfilado/<juego>-<CRC>/juego.prf      el perfil persistente del recompilador
/dev_hdd0/salsa64/perfilado/<juego>-<CRC>/volcados/      volcados automáticos de la HLE (datos del juego)
```

`<juego>-<CRC>` es el mismo nombre que el de las partidas guardadas (título
interno y CRC1 de la cabecera). Después de jugar se baja la carpeta
`perfilado/` entera. El total tiene un tope (`tope_mb`, 200 MB por omisión):
al abrir un juego se borran las sesiones más viejas de cualquier juego (y
sus volcados) hasta dejar lugar; `juego.prf` no se borra nunca. Si una
sesión larga pasa del tope, sigue escribiendo solo lo liviano (ventanas,
tirones, contexto, cierre) y deja de lado la traza, las compilaciones y las
tareas.

## perfilado.cfg

`/dev_hdd0/salsa64/perfilado.cfg` (opcional; lo lee la app de perfilado al
abrir cada juego), una clave por línea, `#` comenta:

| Clave | Por omisión | Qué hace |
|---|---|---|
| `perfilado` | `si` | `no` apaga todo (la app se comporta como la normal) |
| `traza` | `si` | ventanas de la secuencia de bloques (para el modelo de la caché) |
| `jr` | `si` | JR por sitio, con aciertos del destino repetido |
| `bloques` | `si` | cada compilación con sus palabras por clase |
| `muestreo` | `si` | muestreo por tiempo desde un hilo de baja prioridad (`rec_sample.h`) |
| `perfil` | `si` | el perfil persistente (cuenta entradas a bloques, ~5 %; sin zona caliente salvo con `perfil-activo`) |
| `tareas` | `si` | un registro por tarea gráfica |
| `volcados` | `8` | volcados automáticos de la HLE por sesión (`0`: ninguno) |
| `volcados_cada_s` | `30` | separación mínima entre volcados |
| `tope_mb` | `200` | tope de toda la carpeta `perfilado/` |
| `tiron_ms` | `25` | un campo de emulación más largo es un tirón |
| `traza_cada_s` | `10` | una ventana de la traza cada tanto |
| `traza_largo` | `16384` | entradas por ventana (hasta 32768) |

Lo que cambia el código generado (`traza`, `jr`, `perfil`) se aplica al
abrir el juego.

## Qué registra

**Contexto** (al arrancar): versión y commit, título y APPID de la app,
nombre interno del juego, código, CRC1/CRC2, región, TV, tamaño de la ROM,
recompilador y sus opciones, HLE gráfica y JPEG (SPE o PPE), LLE, escala,
filtro, recorte, aspecto, resolución, ritmo, perfil persistente, muestreo,
fecha y la configuración del perfilado. **Fin** (al cerrar): duración,
campos, ciclos, bloques, vaciados, desalojos, JR, bytes perdidos, volcados,
cómo terminó (menú, XMB).

**Ventana** cada 5 s (lo mismo que las estadísticas del log): campos/s y %
de la tele, MIPS, % en bucles de espera, ms de emulación, copia del VI y
ritmo por campo y el peor campo; presentados, saltados, descartados, sin
imagen, del RSX; retraso (cola y antigüedad del destino); hilo 1 ocupado;
RSX por campo y por cuadro; tareas, dibujos, triángulos, SPE, espera al SPE,
traducción al RSX; esperas por un juego de búferes; copias a la RDRAM
(hechas, esperadas por la emulación, tarde, perdidas); tareas cortadas,
comandos de otro microcódigo saltados, cambios y programas generados de
fragment program, destinos creados y reciclados; audio (ms de salida, ‰ en
silencio y estirado, faltas, silencios, saltos, muestras descartadas);
recompilador (bloques, retraducciones, palabras, desalojos, vaciados,
bloques muertos, instrucciones interpretadas, JR, µs compilando); muestreo
por categoría; el hueco más largo entre lecturas de los mandos; bytes
perdidos.

**Camino lento y compilaciones**: cada ventana de 5 s añade contadores del
camino lento por clase (llamadas y ticks del timebase PPE) y compilaciones
por motivo (bloques, ticks y palabras generadas). Las clases distinguen
KSEG0/KSEG1, RDRAM y E/S por traducción, excepciones de traducción,
desalineación, grupos MMIO, escrituras sobre código, instrucciones que pasan
al intérprete y excepciones por tipo. Se guarda también el conjunto de PCs de más
tiempo en la sesión. Los motivos de compilación son primera compilación,
escritura de CPU, DMA, desalojo/vaciado y retraducción del mismo contenido
MIPS. El lector ignora tipos nuevos que no reconoce, por lo que los archivos
anteriores siguen siendo legibles; la cabecera permanece en versión 1.

La medición toma el timebase solo dentro de las rutinas C del camino lento y
durante compilaciones, con `rec->trace` activo. No cambia palabras emitidas.
Por esa restricción no cuenta aciertos de `ktlb[]` que resuelve el código
generado: registra los accesos que llegan a C, que son fallos del camino
rápido. Tampoco atribuye cada excepción de traducción a un PC MIPS exacto.
KSEG y MMIO se clasifican con la dirección recibida y el resultado de la
traducción.

**Tirón**: cada campo de emulación de más de `tiron_ms`, con lo que lo
explica: bloques compilados y µs compilando (y cuántos eran
retraducciones), desalojos, vaciados y bloques muertos, instrucciones
interpretadas, espera al SPE (y lo dormido en bucles de espera), espera por
un juego de búferes del RSX, espera por copias a la RDRAM, copia del VI,
JPEG, tareas gráficas y de audio, cuánto se solapó el hilo de presentación,
el muestreo del campo por categoría (nativo, camino lento, despachador,
intérprete, eventos, compilar, espera, ritmo, otro), los bloques invalidados
(código reescrito por el juego o por DMA) y los KB que llegaron por DMA del
cartucho (la E/S de cargar un nivel).

**Recompilador** (`src/cpu/rec_trace.h`):

- *Compilaciones*: dirección virtual y física, palabra de entrada en la caché
  (la ubicación real del código), huella MIPS, µs de timebase, id,
  instrucciones, palabras calientes y frías, cómo termina el bloque (`jr ra`,
  `jr`, `jalr`, `jal`, `j`, condicional, likely, a C, tope; y si es un
  bucle), tramo, banderas (retraducción, zona caliente, depende de
  Status.FR, sin código) e instrucciones y palabras calientes y frías por
  clase: entrada (¿cabe?, COP1, recarga), ALU, memoria, FPU, saltos (sin el
  delay slot, con su enlace), salida (escritura de registros, enlaces,
  trampolines) y otras (COP0, CACHE, al intérprete). Con la FPU compacta,
  una operación de la FPU es una llamada a una rutina del preámbulo: cuenta
  la llamada, no la rutina.
- *Desalojos y vaciados*: cuándo, de qué tramo o por qué causa, y cuántos
  bloques murieron.
- *Traza*: ventanas de `traza_largo` entradas, cada `traza_cada_s`, desde un
  punto al azar de un campo: el id de cada bloque al que entra la ejecución
  (desde C, por enlace o por `exit_jr`) en orden y las vueltas a C (salida
  con su tipo, compilación, instrucciones interpretadas, eventos, bucle de
  espera), con una tabla de los bloques que aparecen (vpc, palabra de
  entrada, instrucciones, cómo termina, palabras). Es la entrada del
  laboratorio que modela la caché del PPE en la Mac (docs/cache-ppe.md):
  L1I de 32 KB, L2 de 512 KB, con las direcciones reales del código. Las
  vueltas de un bucle dentro de un bloque no pasan por su entrada y no se
  registran.
- *JR*: por bloque que termina en JR/JALR, cuántos se ejecutaron y cuántos
  fueron al mismo destino que la vez anterior (lo que acertaría una caché de
  destinos de una entrada por sitio). La pila de regresos se simula con las
  ventanas de la traza (el lector: `jal`/`jalr` apilan y `jr ra` compara;
como un bloque con condicionales internos puede salir antes de su salto
final, un `jal` cuenta solo si lo que sigue es su destino, y `jalr` y `jr
ra` solo en bloques sin otros saltos; los demás `jr ra` se informan aparte).
- *Muestreo por tiempo* (al cerrar): la tabla por dirección de
  `rec_sample.h` (muestras, instrucciones, palabras) y el reparto por
  categoría. Pesada con las compilaciones da las palabras por instrucción
  por clase **pesadas por tiempo**.
- *Perfil persistente*: `juego.prf` en la carpeta del juego, como
  `perfiles/<juego>.prf` con `perfil-activo` (docs/rec-ppe.md), pero sin
  zona caliente: el código queda donde lo pone la app normal.

**Tareas gráficas** (una por tarea, al traducirla al RSX): campo, número,
comandos, triángulos, dibujos, texturas decodificadas, comandos sin
implementar y el último, banderas de la HLE (`GFX_RES_*`: cortada, lista
llena…), combiners aproximados (cuenta de la HLE y dibujos marcados con
`gfx_draw.approx`), comandos de otro microcódigo saltados, errores de
display list, µs en el SPE, µs traduciendo, µs que la emulación esperó su
juego de búferes, cambios de fragment program, copias a la RDRAM y qué tuvo
de nuevo. Un opcode desconocido o un combiner aproximado que no se había
visto en la sesión deja además un evento (`desconocido=0x..`,
`aproximado=<huella>`): la huella resume lo que quedó del combiner (ciclo,
texturas, niebla, términos y factores) y sirve para comparar sesiones.

**Volcados automáticos**: el primer opcode desconocido de cada tipo, cada
combiner aproximado nuevo y la primera tarea cortada vuelcan la salida de
esa tarea (como SELECT + R2) en `volcados/<n>-<campo>-<motivo>.bin`, hasta
`volcados` por sesión y separados por `volcados_cada_s`. Se ven en la Mac
con `salsa64-run --raster-dump`. El volcado espera al RSX: es un tirón de la
presentación, por eso hay pocos.

## Costo

El hilo que emula no espera nunca al disco: los registros van a dos anillos
en memoria (2 MB el del hilo que emula, 512 KB el de presentación) y un hilo
de prioridad 1500 los vacía cada medio segundo. Si un anillo se llena, el
registro se pierde y se cuenta (`perdidos` en las ventanas y el fin).

En el código generado (lo que pesa):

| Parte | Qué agrega | Cuándo corre |
|---|---|---|
| `traza` | una palabra caliente por bloque (`bne cr3`) y tres frías | la palabra, en cada entrada a un bloque; el resto solo con una ventana abierta |
| `jr` | una palabra en cada sitio JR/JALR y ~16 instrucciones en una rutina compartida | en cada JR |
| `muestreo` | dos palabras por bloque (`li`, `sth`) y un hilo que lee cada 200 µs | en cada entrada; ~5 % medido en la PS3 (docs/rec-ppe.md) |
| `perfil` | cuatro palabras por bloque (`hits[id]++`) | en cada entrada; ~5 % medido en la PS3 |
| `bloques` | nada en el código; ~90 bytes por compilación | al compilar |

Medido en el host (qemu, GoldenEye 1,5 G ciclos con el pad de la presa; el
tiempo de qemu no es el del Cell, sirve para comparar entre sí):
ver «Mediciones» al final. **En la PS3 hay que medirlo con el paquete**: si
algo pasa del 10 %, se apaga en `perfilado.cfg`.

## El lector

```sh
tools/perfilado/perfilado.py resumen perfilado/GoldenEye_007-DCBC50D1        # todas las sesiones del juego
tools/perfilado/perfilado.py resumen perfilado/GoldenEye_007-DCBC50D1/3.sesion
tools/perfilado/perfilado.py comparar ANTES DESPUÉS     # dos sesiones, o la última de dos carpetas
tools/perfilado/perfilado.py juegos perfilado            # por juego: la última sesión contra la anterior
tools/perfilado/perfilado.py traza perfilado/…/3.sesion -o traza.txt   # para el laboratorio de la caché
tools/perfilado/perfilado.py json perfilado/…/3.sesion   # todo lo leído
```

El **resumen** de una sesión: contexto; velocidad (campos/s mediana, p10 y
mínimo, MIPS, % en espera, ms de emulación, peor campo, presentados y
saltados, retraso, hilo 1); audio; tirones por causa (la que más µs
explica; si ninguna llega al 30 % del campo, «CPU» con la categoría del
muestreo que más pesó) y los peores con su detalle; recompilador
(compilaciones, retraducciones, MB generados, desalojos, vaciados, µs por
compilación, palabras por instrucción en total y por clase, sin pesar y
pesadas por tiempo); los bloques que más tiempo se llevan; JR por segundo,
aciertos del destino repetido y los sitios con más JR; la traza (entradas,
vueltas a C por mil entradas, huella de código caliente) y la pila de
regresos simulada con 4, 8, 16 entradas y sin tope; gráficos (por tarea:
dibujos, triángulos, SPE, traducción; esperas por juego de búferes, copias,
tareas cortadas, comandos saltados, errores, opcodes desconocidos, combiners
aproximados distintos); los volcados.

**comparar** pone dos sesiones lado a lado (pensado para la prueba de
regresión: la versión anterior contra la nueva, mismo juego): campos/s
(mediana, p10, mínimo), MIPS, emulación por campo, tirones por minuto, sus
ms y cuántos por cada causa, esperas por juego de búferes, copias a la
RDRAM y las que esperó la emulación, tareas cortadas, comandos saltados,
tareas con desconocidos y con combiners aproximados, combiners aproximados
distintos, audio, palabras por instrucción, retraducciones, desalojos y
bytes perdidos, con una flecha en lo que empeoró más de 10 %; y los opcodes
desconocidos y los combiners aproximados que aparecen en la segunda y no en
la primera. **juegos** hace eso por cada juego de la carpeta (la última
sesión contra la anterior).

La **traza** exportada tiene una línea por entrada: `V ventana campo
ciclos`, `B id vpc palabra n fin calientes frías` (una entrada a un bloque)
o `M tipo valor` (una marca de C: 81 salida, 82 compilación, 83 intérprete,
84 eventos, 85 bucle de espera).

## Formato del archivo

Todo big-endian. Cabecera de 16 bytes: `S64P`, versión (u16, hoy 1), largo
de la cabecera (u16, 16) y 8 bytes en 0. Después, registros seguidos: una
palabra u32 con el tipo en el byte alto y el largo del contenido en los 24
bajos, y el contenido. Un archivo cortado (la consola se apagó) se lee hasta
el último registro completo. Los registros de los dos anillos se intercalan
en el orden en que se vacían: cada uno lleva sus milisegundos desde que se
abrió la sesión (`ms`).

| Tipo | Nombre | Contenido |
|---|---|---|
| 1 | CONTEXTO | `ms` u32, texto `clave=valor` por línea |
| 2 | NOMBRES | `ms` u32, texto `ventana=a,b,…` y `tiron=a,b,…`: los campos de VENTANA y TIRÓN en orden |
| 3 | VENTANA | `ms` u32, cuántos u16, 0 u16, los valores u32 |
| 4 | TIRÓN | igual que VENTANA |
| 5 | BLOQUES | tamaño de la entrada u16 (88), cuántas u16, entradas |
| 6 | DESALOJO | `ms`, campo, tipo u8 (0 tramo frío, 1 mitad caliente, 2 vaciado), tramo o causa u8, 0 u16, bloques muertos u32 |
| 7 | TRAZA | `ms`, campo, ciclo u64 al abrirla, entradas u32, bloques en la tabla u32, sin lugar en la tabla u32, las entradas u32, la tabla (24 bytes c/u) |
| 8 | JR | `ms`, sitios u32, JR sin lugar u32, sitios (16 bytes c/u) |
| 9 | MUESTRAS | `ms`, muestras u64, categorías u32 y su cuenta (u32 c/u), sin id u32, sin lugar u32, bloques u32, bloques (16 bytes c/u) |
| 10 | TAREAS | tamaño de la entrada u16 (48), cuántas u16, entradas |
| 11 | VOLCADO | `ms`, campo, tarea, motivo u32 (1 desconocido, 2 aproximado, 3 cortada), qué u32 (opcode, huella o banderas), nombre del archivo |
| 12 | FIN | `ms` u32, texto `clave=valor` |
| 13 | PERDIDO | anillo u32, bytes u32 |
| 14 | EVENTO | `ms` u32, texto |
| 15 | CAMINO LENTO | `ms:u32`, `n:u16`, `pcs:u16`; `n` pares `llamadas:u32,ticks:u64`; `pcs` ternas `vpc:u32,ticks:u64,llamadas:u32` |
| 16 | COMPILACIÓN | `ms:u32`, `n:u16`, reservado `u16`, máximo de compilaciones por campo `u32`, campo del máximo `u32`; `n` ternas `bloques:u32,ticks:u64,palabras:u32` |

Los campos de VENTANA y TIRÓN están en `src/frontend/perfilado.h`
(`PERF_WINDOW_FIELDS`, `PERF_STALL_FIELDS`); el registro NOMBRES los nombra
en cada archivo y el lector los toma de ahí, así que agregar uno al final no
rompe nada. Las unidades van en el nombre (`_us`, `_ms`, `_pm` por mil,
`_x100`).

**Entrada de BLOQUES** (88 bytes): `ms` u32, campo u32, vpc u32, física u32,
palabra de entrada en la caché u32, ticks del timebase compilándolo u32,
huella MIPS u64, id u16, palabras calientes u16, frías u16, instrucciones
u8, fin u8 (0 tope, 1 `jr ra`, 2 `jr`, 3 `jalr`, 4 `jal`, 5 `j`, 6
condicional, 7 likely, 8 a C; +0x80 si es bucle), tramo u8 (0 KSEG0, 1
KSEG1, 2 mapeado), banderas u8 (1 retraducción, 2 zona caliente, 4 depende
de FR, 8 sin código), instrucciones por clase 7 × u8, 0 u8, palabras
calientes por clase 7 × u16, frías por clase 7 × u16, 0 u16, destino fijo
del salto final u32 (J, JAL o condicional; 0 en JR/JALR o sin salto), saltos
del bloque u8 (el final y los condicionales internos: con más de uno, el
bloque pudo salir antes de llegar al final), 3 bytes en 0. Clases: entrada,
ALU, memoria, FPU, saltos, salida, otras.

**Tabla de TRAZA** (24 bytes): id u16, instrucciones u8, fin u8, vpc u32,
palabra de entrada u32, palabras calientes u16, frías u16, destino fijo u32,
saltos u8, 3 bytes en 0. **Entradas**: un
id de bloque (1–65535) es una entrada a ese bloque; con el byte alto en 0x81
y más, una marca de C con su valor en los 24 bits bajos (0x81 salida al
despachador: 1 enlace, 2 a mitad, 3 salto calculado, 4 bucle de espera, 5 en
la entrada, 6 fin en C, 7 Status.FR, 8 excepción o ERET; 0x82 compilación
del id; 0x83 instrucciones interpretadas; 0x84 eventos; 0x85 bucle de
espera). Un id que se reutiliza dentro de la ventana (raro: hace falta un
desalojo en medio) queda con los datos del último.

**Sitio de JR** (16 bytes): vpc u32, JR u32, al mismo destino que la vez
anterior u32, fin u8, instrucciones u8, 0 u16.

**Bloque de MUESTRAS** (16 bytes): vpc u32, muestras u32, instrucciones u16,
palabras u16, calientes u16, 0 u16.

**Entrada de TAREAS** (48 bytes): `ms` u32, campo u32, tarea u32, comandos
u32, triángulos u32, dibujos u16, texturas u16, desconocidos u16, último
desconocido u8, banderas de la HLE u8, combiners aproximados u16, dibujos
aproximados u16, comandos saltados u16, errores de display list u16, µs en
el SPE u16, µs traduciendo u16, µs de espera por el juego de búferes u16,
cambios de fragment program u16, copias a la RDRAM u8, nuevos u8 (1 opcode
desconocido nuevo, 2 combiner aproximado nuevo, 4 volcado), 0 u16. Los u16
saturan en 65535.

## En el host

`salsa64-run --rec --perfilado DIR` escribe la misma sesión (sin hilos, un
anillo que se vacía cada 60 campos) en `DIR/<juego>-<CRC>/<n>.sesion`;
`SALSA64_PERFILADO_CFG` lleva las claves de `perfilado.cfg` separadas por
comas (también por `tools/qemu-ppc64.sh`). Los tiempos son los del host: en
qemu no dicen nada del Cell, pero prueban el registro y el lector de punta a
punta. Las pruebas: `tests/unit/test_rec_trace.c` (con el recompilador en
qemu, la secuencia de entradas, los JR por bloque y las palabras por clase)
y `tests/unit/test_perfilado.c` (configuración, registros por el anillo,
sesiones numeradas, rotación).

## Mediciones

**Host, qemu (2026-10-13).** GoldenEye con `docs/pads/goldeneye-dam.pad`
hasta la presa (7,5 G ciclos, 80 s de consola), `salsa64-run --rec`, con
cada parte sola y todo junto (`traza_cada_s=1` o `2`):

- La RDRAM al final es **idéntica byte a byte** en todas las variantes (sin
  perfilado, todo, solo compilaciones, solo JR, solo traza, muestreo): el
  registro no cambia la emulación. `tools/test-rec.sh` lo comprueba en cada
  corrida con gfxtest (el estado del campo 300 con y sin `--perfilado`).
- Tiempo de qemu: sin perfilado 46,8–58,0 s en cinco corridas; con todo
  50,3–54,2 s en cuatro; el mínimo de cada uno difiere menos de 1 %. El
  ruido de qemu es de ±10 % y qemu no mide el Cell: el costo real se mide en
  la PS3 (los ns por instrucción del log, con y sin `perfilado = no`).

Lo que dejó esa sesión (con el código de qemu; la estructura vale para la
PS3, los tiempos no):

- 14 826 compilaciones (3,7 % retraducciones), 11 MB generados, 4
  desalojos de tramos; 13,7 palabras de PPC por instrucción en los bloques
  vivos (5,8 calientes): memoria 7,6 calientes y 9,5 frías por instrucción,
  saltos 7,1 y 2,7, FPU (compacta) 4,1 y 5,1, ALU 1,8.
- 44,6 M JR: 557 000 por segundo de consola, 22 por mil instrucciones
  ejecutadas (la estimación de docs/cache-ppe.md, §3.3, era 0,02 por
  instrucción); el 64 % va al mismo destino que la vez anterior en su
  sitio. Sobre las ventanas de la traza, una pila de regresos acierta el 89
  % de los `jr ra` (de 4 entradas el 90 %: más profundidad no suma) y la
  caché de destino de una entrada el 53 %.
- 40 ventanas de 16 384 entradas: 4 vueltas a C por mil entradas a
  bloques; 6 194 bloques distintos con 1,8 MB de código caliente (4,5 MB
  con lo frío), contra 512 KB de L2.

**PS3 real:** pendiente (la primera tanda es la prueba de regresión de la
v0.2.6 con la app de perfilado).
