# Turbo3D: candidato para Dark Rift

Estado: implementación experimental, apagada por omisión. El autor confirma
en PS3 ambos personajes, texturas y animaciones correctos en la pelea
probada. No se declara validado el juego completo.

Candidato `5c51bb2`, `Salsa64 Turbo3D` (`SLSA00184`): host 676 pruebas
sin fallos; catorce imágenes dentro del presupuesto; RPCS3 normal y con
afín + recorte, 42/42 puntos y listas SPE/PPE iguales en las seis imágenes.
Identidad y SHA del paquete en `estado.md`.

Primera corrida PS3 conservada (2026-10-21): la imagen carga y supera sus
autopruebas, produce geometría y cierra limpia tras 7016 campos. Los
resúmenes no muestran tareas cortadas. Posteriormente el autor confirma
explícitamente que los dos personajes, texturas y animaciones se ven bien:
la ausencia de modelos queda corregida en la escena probada.
Detalle y límites de la medición en `estado.md` y `perf-log.md`.

## Evidencia y alcance

En la consola, Dark Rift arranca pero el autor no ve los modelos. Su log
identifica código de 4096 bytes, CRC de bytes `42C29CD9` y de palabras
`2BDCFC8A`. El contador de tareas sin HLE llega al menos a 2048: son
tareas omitidas, no tareas Turbo3D dibujadas. Los avisos son muestreados;
doce líneas no significan doce tareas ni 2048 es el total exacto.
Los dibujos de otras tareas F3D no demuestran soporte de Turbo3D.

Se adapta una fuente GPLv2 o posterior de Glide64mk2; revisión y avisos
en `THIRD_PARTY.md`. No incluye microcódigo, datos de juegos ni ROMs.

## Implementación

- Registros de 16 bytes con punteros a estado global, objeto, vértices y
  triángulos. Un puntero de objeto nulo termina la lista.
- Estado global de 100 bytes: modos RDP, segmentos, viewport y lista RDP.
  Se resuelve con los segmentos anteriores; sus punteros posteriores usan
  los nuevos. Objeto de 24 bytes con matriz combinada de 64 bytes detrás,
  salvo bandera 1 de reutilización.
- Vértices estándar de 16 bytes, coordenadas de textura s10.5 directas,
  sin iluminación; sombreado suave. Triángulos de cuatro bytes, índices
  de un byte. La caché propia cubre 256 lugares y se invalida por tarea.
  No se amplía ni se cambia el estado RAW compartido entre imágenes.
- Listas auxiliares RDP terminadas por dos palabras cero; admiten
  TEXRECT de 16 bytes desde el primer comando y triángulos RDP crudos.
- Lecturas acotadas a RDRAM, validación de índices cargados y presupuesto
  compartido de comandos. Una entrada inválida corta la tarea y se informa;
  nunca se redondea una lista auxiliar mal alineada ni se reutilizan
  vértices sin inicializar.

Imagen SPE 5: Turbo3D y su F3D acompañante (`77195A68`), sin recarga por
alternancia de esas dos identidades. Caché de texturas propia de 512
entradas; no se reducen las cachés ni el mínimo de pila de otras imágenes.
Los 11264 bytes de vértices y 256 de validez solo existen en esa imagen.

## Activación y prueba

PS3: archivo `/dev_hdd0/salsa64/gfx-turbo3d`; para apagar, renombrarlo a
`gfx-turbo3d.off` y reiniciar la aplicación. El log confirma «Turbo3D
experimental activo». Host: `SALSA64_GFX_TURBO3D=1`.

Sin la opción se conserva la identificación anterior y el modo LLE auto
puede seguir atendiendo la familia desconocida. Con LLE all se sigue
ejecutando el microcódigo real, independientemente de la opción.

Pruebas propias del host: formato big-endian, segmentos nuevos, carga y
reutilización de matriz/vértices, lugares 253–255, salida esperada de
posición y color, límites de RDRAM e índices, TEXRECT inicial, comandos
crudos truncados y par no nulo cuya suma da cero. La autoprueba al cargar
la imagen compara dos objetos Turbo3D entre SPE y PPE y también su F3D.
Esto valida consistencia interna, **no equivalencia con el microcódigo
real**. Falta una entrada reproducible y la comparación gráfica de referencia.

En PS3, tras aprobar el envío: abrir Dark Rift, entrar en combate y comprobar
ambos personajes, animación, escenario, texturas y menús; regresar al XMB y
conservar el log antes de abrir otro juego. Revisar autopruebas de imagen 5,
triángulos, tareas cortadas y estabilidad. Comparar después con la opción
apagada sin cambiar otros ajustes. No usar FPS sin modelos como referencia
de rendimiento equivalente.

Pendientes: confirmar en hardware modelos y recorte, contrastar con RSP
real, y revisar semánticas no cubiertas por la fuente (bandera de sombreado
del triángulo, flags globales y normalización de perspectiva). No se
presentan esas partes como verificadas ni se inventa su comportamiento.
