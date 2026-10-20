<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Detalle opcional de la HLE gráfica del SPE

La medición de Road Rash 64 dejó unos 3,80 ms/tarea (43,23 %) fuera de
vértices, estado y cargas de TMEM. Este diagnóstico permite separar ese
tramo; no cambia la geometría ni aplica una optimización.

## Activación y A/B

Crear el archivo vacío `/dev_hdd0/salsa64/gfx-spe-detalle` antes de abrir el
juego. Solo instrumenta la imagen 1 (F3DEX2/F3DZEX2/S2DEX2/F3DFLX). Para
desactivar, renombrarlo a `gfx-spe-detalle.off` y volver a abrir el juego.
El archivo se consulta al inicializar el backend, no por primitivo.

Sin archivo no se reserva el bloque ni el acumulador en XDR, no se ocupa
el marco de detalle en la pila del SPE y no hay DMA diagnóstico, lecturas
extra del reloj ni incrementos de los contadores nuevos. El binario conserva
las comprobaciones de activación; su coste debe medirse en la consola.
Las otras imágenes eliminan los contadores y las llamadas de inicio/fin
mediante el preprocesador. La caché de texturas conserva su capacidad.

Al cargar la imagen 1 con detalle activo se ejecutan 17 pares de la misma
lista propia de la autoprueba, alternando el orden sin/con detalle. Cada
muestra reinicia el RDP y desactiva la caché para hacer equivalentes las
entradas. El log indica mediana, mínimo, máximo y diferencias de salida;
los vértices y comandos deben coincidir exactamente entre ambas corridas
del SPE. Además la autoprueba habitual compara el SPE instrumentado contra
el PPE sin detalle con su tolerancia previa. Estas listas no representan
la carga de una carrera y sus tiempos solo son válidos en PS3 real.

## Protocolo y memoria

`gfx_job` usa los 16 bytes antes libres: dirección de 64 bits, capacidad y
versión de detalle. Ocupa exactamente 112 bytes. `gfx_result` conserva sus
112 bytes y la cola de `lib/spukit` no cambia. Los tamaños se comprueban al
compilar. Es necesario reconstruir juntas las imágenes SPE y el PPE.

El bloque versión 2 tiene 272 bytes, alineado a 128; contiene solo palabras
de 32 bits y el número de tarea. El PPE reserva un bloque y un acumulador
de 64 bits al abrir el juego solo si está presente el archivo, y los libera
al cerrar. Hay una sola tarea en vuelo por trabajador: se recoge antes de
reutilizar el bloque. La imagen 1 valida versión, capacidad, alineación y
dirección menor de 4 GB antes de activar el detalle. Las demás lo ignoran.
El bloque del SPE vive en un marco separado, solo mientras se instrumenta.

El SPE termina las salidas normales, envía el detalle por la etiqueta 9 y
espera su terminación antes de devolver el resultado de la cola. El PPE lo
lee únicamente después de recoger ese resultado y comprueba versión,
tamaño y tarea; un bloque viejo no se incorpora al promedio. No hay
comunicación con el PPE por primitivo. El DMA del diagnóstico queda fuera
de `t_total`, pero dentro del tiempo del manejador: esa diferencia incluye
también la envoltura normal, no solo el DMA diagnóstico.

## Lectura del log

Los resúmenes aparecen cada 100 tareas instrumentadas y al cerrar para la
ventana parcial. Se identifican por número de tarea, no de campo; cruzar
los intervalos con `gfx campos` antes de comparar aperturas o escenas.

La partición exclusiva es `total = vtx + estado + TMEM + tri sin estado +
matrices + listas + cierre + resto`. El tiempo de triángulos incluye
recorte, preparación, emisión y vaciados intermedios; descuenta el incremento
de estado anidado. Matrices mide `G_MTX` (las otras operaciones de matriz
quedan en resto); listas mide la lectura de comandos y su precarga; cierre
mide el último dibujo, el END y la espera final. Resto incluye despacho,
inicialización, luces cargadas aparte, rectángulos y lo no cubierto. No
interpretarlo como una etapa concreta. Los relojes y contadores también
cuestan tiempo; la partición mide la ejecución instrumentada.

Por ranura (0 vértices, 1 matrices, 2 pequeña, 3 TMEM) se publican
peticiones, bytes pedidos, bytes alineados y tiempo de envío/espera. Estos
tiempos están anidados en las etapas anteriores: no se suman otra vez.
El tiempo de vértices menos su DMA aproxima el resto de transformación y
registro de grupos; incluye iluminación, niebla y generación de ST. Se
cuentan vértices con cada función activada y cargas completadas. El host
usa memoria directa: no simula bytes ni latencia DMA y su reloj vale cero.

La línea `lote vtx` de la imagen 1 separa los vértices transformados por el
lote y sus grupos de cuatro de los que siguieron escalares: modo
`gfx-escalar`, iluminación (con el subconjunto que además tiene TEXGEN), falta
de niebla y la cola de uno a tres de una carga por lo demás apta. Iluminación
y TEXGEN no son dos rechazos acumulables: TEXGEN solo cambia el vértice dentro
del camino iluminado. El contador no atribuye rechazos a recorte, alineación,
destino ni tipo de DMA porque esos no son condiciones de este lote.

Los triángulos de entrada son llamadas a `gfx_triangle`; aceptados significa
que obtuvieron salida, plano común y cercano son rechazos, y recortados
cuenta cruces del plano cercano antes de intentar reservar salida. Un
triángulo recortado puede emitir dos; el total del consumidor no equivale
a las llamadas de entrada. Los bytes de vértices/comandos son salida útil;
los de textura son uso del arena, que puede incluir alineación. Los bytes
DMA de texturas incluyen además escrituras a la caché.

Las salidas separan número de operaciones MFC, bytes y espera de búfer por
vértices/comandos/texturas. La espera final común permanece en cierre.
Las listas cuentan bloques pedidos (incluida precarga), bytes, aciertos en
el bloque actual y cambios de bloque; un cambio puede aprovechar un bloque
precargado. No se contabilizan bytes de precarga desperdiciados ni un
histograma por número de luces en esta versión.

## Validación y siguiente medición

La prueba host `gfx_detail_parity` compara detalle apagado/encendido sobre
la lista propia F3DEX2: resultados, vértices, comandos, texturas y caché
idénticos; comprueba contadores y que una capacidad insuficiente o versión
desconocida no escriben el bloque. El host usa ASan y UBSan.

En PS3 comprobar primero diferencias cero en las autopruebas y el coste
del par sintético. Repetir el mismo recorrido, resolución y ajustes en dos
aperturas, sin/con archivo, conservando los logs separados. Comparar los
campos 1400–1699 con ventanas de tareas equivalentes: ms/tarea, campos/s,
tiempos exclusivos, DMA, sondeos y audio. Si el detalle perturba demasiado
la carga, el siguiente cambio debe muestrear tareas enteras. No usar estos
datos instrumentados como prueba de una ganancia de rendimiento.

No se ha ejecutado una carrera ni `--lle-check-gfx`: la tarea se valida con
listas propias, sin ROM ni microcódigo propietario. La comparación contra
el microcódigo real requiere una captura autorizada y queda pendiente, al
igual que sincronización y coste en la consola. El techo de ganancia de
esta entrega es cero: solo aporta medidas para elegir un cambio posterior.
