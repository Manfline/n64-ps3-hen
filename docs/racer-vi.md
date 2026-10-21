<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Racer: salto vertical durante un video

## Evidencia anterior (2026-10-20)

Revisión sobre `59cb166` (v0.2.9), sin ejecutar el juego ni modificar la
presentación. En `build/ps3-limpieza-20261019/` hay dos cajas cuyo título es
`STAR WARS EP1 RACER` y cuyo CRC es `72F70398-6556A98B`:

| Caja | Campos conservados | Tareas conservadas | Tareas descartadas del anillo |
|---|---|---|---|
| `cajanegra-628/caja.s4bb` | 621–628 | 951–957 | 951 |
| `cajanegra-762/caja.s4bb` | 761–762 | 1074–1075 | 1052 |

SHA-256 de las cajas, respectivamente:

```
961eddf72fd8ebebb39c41e831d9a18a878f4415a1c3a9a3c78886a2598ce534
be715a8f9b1f722586ca616d49cc6578c6b820a569d3c86a60e67a6fc7323359
```

La repetición de las tareas 951 y 1074 con `salsa64-run --raster-dump`
muestra el paisaje de la demo de carrera y los vehículos. **Ninguna de
estas dos cajas conserva el video con el salto.** El anillo ya había
descartado las tareas anteriores: no permite probar la causa del salto del
video. La repetición de una tarea dibuja su salida HLE; tampoco reproduce
por sí sola la selección y presentación de destinos de la PS3.

En los diez campos conservados, la imagen es siempre RSX, sin campos
saltados, repetidos o intermedios; la emulación lleva cero campos de
ventaja. El VI conserva `CONTROL=00013052`, `WIDTH=640`,
`H_VIDEO=006c02ec`, `X_SCALE=00000200`, `Y_SCALE=00000400` y
`V_SYNC=524`. Produce 320 × 237 píxeles de 16 bits con paso de 1280 bytes.
`V_VIDEO` alterna `002501ff` / `002301fd`; los orígenes recorren tres
búferes, con un desplazamiento de dos o una filas de 640 bytes sobre sus
bases `63b800`, `6d2c00` y `76a000`. Las tareas guardadas usan color images
de 320 de ancho y 16 bits. Esta alternancia es compatible con el modo
entrelazado ya conocido; no demuestra el salto de media pantalla reportado
en otra escena.

## Captura del video (2026-10-20)

La traza de 1800 campos y las cajas pedidas durante el salto conservan los
campos 539–558, 635–656 y 750–757. Los SHA-256 de las tres cajas son:

```
a26649fe09d330196079f0668e7661686a32ef49dc563615614063dd111177c9
1f6b057765f6449fc8e37fcec3a199825a2dae95fc6421dc1fe4fef9ed9fe9cb
d010c29d1d6dd3c02c47b44e9e0220685b3242f914eaa584e6250c582312a4a0
```

El VI permanece en el mismo modo entrelazado del video: `CONTROL=00013052`,
imagen 320 × 237, paso 1280, `V_VIDEO` alternado y tres buffers con las
bases `63b800`, `6d2c00` y `76a000`. La decisión es RSX en todos los campos
que llegan a presentarse; no hay alternancia con RDRAM, interpolación ni
cambio de los registros que explique el salto.

El problema está en el alto reservado por el destino. Una ventana VI ocupa
`237 × (1280 / (320 × 2)) = 474` filas lógicas. Sin embargo, los buffers de
`63b800` se crean de 320 × 384 y presentan 383 filas; el de `76a000` es
320 × 464 y presenta 463. Las escalas resultantes son 3×, 2× y 3×. La tijera
de los dibujos termina en 384, pero el VI sigue leyendo el video de la RDRAM
por debajo de esa tijera. La traza confirma el recorte dependiente del
destino aunque la ventana VI sea estable; falta verificar en la PS3 que la
reserva completa elimina el salto visible.

## Qué hace realmente `trace-vi.off`

El archivo respaldado contiene `600\n`. El frontend abre únicamente
`/dev_hdd0/salsa64/trace-vi` al iniciar cada juego: `.off` no se lee y no
es un interruptor con semántica propia. Renombrarlo a `trace-vi` activa
hasta 600 registros; vacío usa 300; un cero desactiva la traza. Para apagarla
se vuelve a renombrar a `.off` antes de abrir el juego.

La implementación actual consume el presupuesto solo al presentar una
imagen válida. Omite los retornos por campo saltado y los campos sin imagen.
Por tanto, «600» no significa necesariamente los primeros 600 campos
emulados. Registra el origen, tamaño, fuente RSX/RDRAM/nada, destino,
ranura, edad y ventaja de emulación; faltan los registros y la ventana que
se eligió del destino. Con RDRAM o nada, `last_present` puede ser la elección
RSX de un campo anterior y debe tratarse como tal.

La caja sí guarda los catorce registros, pero `V_CURRENT` del array crudo
vale cero: `salsa64_vi_read` calcula ese registro al leerlo y no lo escribe
en el array. La caja tampoco guarda `salsa64_vi_frame.vpos`. No se debe
deducir la paridad de `campo % 2`: SERRATE puede haberse activado después de
arrancar el contador de campos.

## Instrumentación de la captura, sin cambiar la imagen

La bandera existente sigue apagada por omisión. `trace-vi` acepta `N` o
`N INICIO`: una ventana de `N` campos empezando en `INICIO` (si se omite,
empieza en el 1). Al abrir el juego se informa la ventana elegida. El
presupuesto ya no se consume solo al presentar una imagen válida.

En `post_field` se guardan paridad y `V_CURRENT` calculado junto con los
datos ya encolados; `present_field` usa esa instantánea y `m.f.vpos`, nunca
registros vivos del otro hilo. Hay una sola línea por campo de la ventana,
incluso con cola llena, descartado, saltado o sin imagen. Lleva campo,
tareas, origen, dimensiones, paso, bpp, vpos, paridad, atraso y los catorce
registros del VI.

Solo si la decisión actual es RSX se añade la selección que ya produjo
`salsa64_rdp_present`: dirección, tamaño y escala del destino; ranura,
edad; ventana lógica; región final de la superficie; overscan; rectángulo de
salida; y tipo verdadero, intermedio o repetido. El getter se consulta
después de esa llamada y no vuelve a recorrer `rdp_targets_present`, que
modifica su historial entrelazado. Con RDRAM, nada o un descarte no se
reutiliza una selección anterior.

No se introducen esperas, copias extra de framebuffer, retención de campos
ni llamadas adicionales al selector. Sin bandera no se formatean líneas ni se
consulta el getter. El frontend de PS3 compiló desde un build limpio. La
suite host pasó con 660 pruebas y 870 603 verificaciones; la invocación normal
mantiene una advertencia preexistente de `test_menu.c` como error. Se conserva
la prueba `targets_present_interlaced_vpos` para la ventana entrelazada de
Racer.

## Corrección propuesta

El paquete de la corrección es «Salsa64 Racer video» (SLSA00177),
`build/salsa64-racer-video/salsa64-racer-video.pkg`, SHA-256
`caf4eeaec9c7f9caf623571153e53e2d80504de2a2863bffedc746411c2b99d7`.

El anillo de VI conserva ahora altura, paso y tamaño de píxel de cada origen.
Al crear una color image compatible, se exige el máximo entre la tijera y la
ventana VI que la cubre. Para el buffer que comienza dos filas antes del
origen, son 476 filas: dos ocultas más las 474 visibles. Así se crea y se
ceba desde RDRAM una superficie que contiene toda la ventana que el selector
ya elegía; no cambian registros VI, origen, paridad ni la regla que escoge la
ranura.

La prueba unitaria modela el caso exacto (320 × 237, paso 1280, origen dos
filas dentro): exige 476 líneas de destino y sigue presentando las 474 de la
ventana entrelazada. Con la traza apagada, la PS3 real reprodujo el mismo
video sin el salto. No hay medición de rendimiento asociada.
