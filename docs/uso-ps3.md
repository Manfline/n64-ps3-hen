# Usar Salsa64 en la PS3

Guía para quien instala el `.pkg` en una PS3 con HEN o CFW. Salsa64 no
incluye ROMs ni las acepta en el repositorio: se usan respaldos propios.

## Instalación

1. Copiar el `.pkg` a la PS3 (por FTP o por USB) e instalarlo desde el
   Package Manager del XMB. Aparece como «Salsa64» en la sección de juegos.
2. Poner las ROMs donde las busca el menú (siguiente sección).
3. Abrir Salsa64 desde el XMB.

## Dónde poner las ROMs

El menú busca ROMs en estas carpetas, al arrancar la aplicación:

| Carpeta | Qué es |
|---|---|
| `/dev_hdd0/salsa64` | Disco duro interno de la PS3 |
| `/dev_usb000/salsa64` … `/dev_usb007/salsa64` | Una memoria USB: carpeta `salsa64` en la raíz |

Dos formas de llevarlas allí:

- **FTP** (webMAN MOD): conectarse a la PS3 desde la computadora y copiar los
  archivos a `/dev_hdd0/salsa64` (crear la carpeta si no existe).
- **USB**: formatear la memoria en FAT32, crear una carpeta `salsa64` en la
  raíz, copiar allí las ROMs y conectarla a la PS3 antes de abrir Salsa64.

Reglas de la búsqueda:

- Extensiones: `.z64`, `.v64` y `.n64` (mayúsculas o minúsculas). Se aceptan
  los tres órdenes de bytes (big-endian, byte-swapped y little-endian); el
  formato se reconoce por la cabecera, no por la extensión.
- No entra en subcarpetas: las ROMs van directamente en `salsa64`.
- Se ignoran los archivos que empiezan con `.`, los que no tienen una de
  esas extensiones, los que miden menos de 4 KB y los ilegibles. También
  se ignoran las ROMs que no caben en la lista (máximo 256).
- La ROM más grande que se puede abrir es de 64 MB.
- La lista se ordena por el título de la cabecera de la ROM (o por el nombre
  del archivo si la cabecera no trae título).
- Si la lista está vacía, el menú muestra las carpetas donde buscó.

## Menú

Se dibuja con el RSX; el panel de la derecha muestra el archivo, su
ubicación, el código del juego, el tamaño, la región y el tipo de guardado
detectado. La última ROM abierta queda elegida al volver al menú.

| Botón (DualShock 3) | Acción |
|---|---|
| Cruceta arriba/abajo | Elegir (mantenida, repite y acelera) |
| Cruceta izquierda/derecha | Saltar una página |
| × | Abrir la ROM elegida (o la opción) |
| △ | Ir a Opciones |
| ○ | Volver de Opciones |

Al final de la lista están «Opciones» y «Salir al XMB».

Opciones:

- **Fondo animado**: enciende o apaga las ondas del fondo.
- **Recorte del borde (overscan)**: sin recorte, normal (por omisión) o
  amplio. Recorta los bordes de la imagen, donde algunos juegos dejan
  basura.
- **Resolución**: 720p o 1080p (se aplica al volver a abrir Salsa64).
- **Mostrar FPS**: el contador de velocidad dentro del juego (abajo, en
  «Contador de velocidad»). También se cambia en el juego con SELECT + △.
- **Resolución**: 720p (por omisión, render a 3×) o 1080p (render a 4×).
  Solo se aplica al volver a abrir Salsa64; el panel muestra la salida
  actual y, si pediste 1080p y no se logró, recuerda activarlo en Ajustes de
  pantalla del XMB.

## Controles en el juego

Los DualShock 3 de los puertos 1 a 4 son los controles 1 a 4 de la N64.

| N64 | DualShock 3 |
|---|---|
| Stick | Stick izquierdo |
| Botones C | Stick derecho |
| Cruceta | Cruceta |
| A | × |
| B | □ |
| Z | L2 |
| L | L1 |
| R | R1 |
| Start | START |

○, △, R2, L3, R3 y SELECT no tienen función en la N64. SELECT sirve para
los atajos de Salsa64:

| Atajo | Qué hace |
|---|---|
| START + SELECT | Cierra el juego y vuelve al menú (guarda la partida, apaga el audio y los SPEs) |
| SELECT + R1 | Guarda el estado en la ranura actual |
| SELECT + L1 | Carga el estado de la ranura actual |
| SELECT + derecha / izquierda (cruceta) | Cambia de ranura (0 a 9, en ronda) |
| SELECT + △ | Muestra u oculta el contador de velocidad |

Mientras SELECT está apretado, R1, L1, △ y la cruceta izquierda/derecha no
llegan al juego (no se mueve el personaje al cambiar de ranura). Si
`controles.cfg` asigna SELECT a un botón de la N64, ese botón se pulsa
también con los atajos.

«Salir del juego» del menú del sistema (botón PS) sale en cualquier
momento.

Para cambiar el mapeo, ver `controles.cfg` más abajo.

## Partidas guardadas

Salsa64 guarda lo que el cartucho guardaría (EEPROM y SRAM de 256 Kbit) en
`/dev_hdd0/salsa64/saves`, un archivo por juego, con el nombre
`<título>-<CRC1>.<extensión>` (`.eep` para EEPROM, `.sra` para SRAM). Se
escribe unos segundos después de la última escritura del juego y al volver
al menú o salir. Para respaldarlas, copiar esa carpeta por FTP.

El tipo de guardado se detecta solo: primero lo que diga `guardado.cfg`, luego
la cabecera de los homebrew y por último una tabla interna por código de
juego. Si un juego no guarda, el panel del menú mostrará «—» como tipo de
guardado: se fija a mano en `guardado.cfg`. FlashRAM, SRAM de 768 Kbit y
SRAM de 1 Mbit se reconocen pero todavía no se emulan.

## Estados guardados

Un estado guarda el juego entero en el instante (como pausar la consola y
sacarle una foto a la memoria): se vuelve exactamente a ese punto, en
cualquier parte del juego, no solo donde el juego deja guardar. Hay diez
ranuras por juego (0 a 9); al abrir un juego la ranura actual es la 0.

- **SELECT + R1** guarda en la ranura actual (si había algo, lo reemplaza).
- **SELECT + L1** carga la ranura actual.
- **SELECT + derecha / izquierda** cambia de ranura.

Un aviso abajo en la pantalla dice qué pasó durante unos 2 segundos:
«Estado guardado en ranura 3», «Ranura 4 (vacía)», «Estado cargado», «No
hay estado en la ranura 2». Guardar o cargar detiene la imagen un instante
(lo que tarda la PS3 en escribir o leer unos 8 MB; queda en el log).

Los archivos van en `/dev_hdd0/salsa64/states`, con el mismo nombre que la
partida del juego en `saves/` y la ranura en la extensión:
`<título>-<CRC1>.st0` … `.st9` (unos 8 MB cada uno). Para respaldarlos,
copiar esa carpeta por FTP. Sirven también en `salsa64-run` (la PC) y al
revés.

A tener en cuenta:

- Cargar un estado también vuelve la partida del cartucho (EEPROM o SRAM) a
  como estaba en ese momento, y así se escribe en `saves/`: un estado viejo
  deshace lo guardado después dentro del juego.
- Un estado es de una ROM: con otra (u otra versión del mismo juego) se
  rechaza. También se rechazan los de una versión de Salsa64 que ya no los
  entiende; el aviso lo dice.
- Con `lle` (RSP de bajo nivel, experimental) no hay estados.
- El primer cuadro después de cargar puede salir negro.

## Contador de velocidad

Con **SELECT + △** (o «Mostrar FPS» en Opciones) aparece arriba a la
izquierda una línea como:

```
VI 59,9 (100 %) · juego 30 · pantalla 60
```

- **VI**: campos de video que emula Salsa64 por segundo y, entre paréntesis,
  la velocidad respecto de la consola (100 % es velocidad completa, NTSC o
  PAL).
- **juego**: cuadros nuevos que dibuja el juego por segundo (muchos juegos
  van a 20 o 30 aunque la tele vaya a 60).
- **pantalla**: cuadros que presenta la PS3 por segundo.

Es un promedio de medio segundo. Se recuerda en `menu.cfg` (`fps = si`).

## Abrir una ROM sin menú

Si existe `/dev_hdd0/salsa64/embedded`, el menú no aparece y se arranca
directo lo que diga el archivo. Para abrir una ROM propia:

```
file:mijuego.z64
```

(una sola línea, el nombre de un archivo que esté en `/dev_hdd0/salsa64`).
Con este archivo presente START + SELECT sale al XMB en lugar de volver al
menú. Para recuperar el menú, borrar `embedded`.

## Archivos de configuración

Todos están en `/dev_hdd0/salsa64` y son opcionales. Son archivos de texto;
se crean por FTP.

| Archivo | Formato | Qué hace |
|---|---|---|
| `menu.cfg` | Líneas `fondo = si\|no` y `fps = si\|no` | Fondo animado del menú y contador de velocidad en el juego. Lo escriben el menú (Opciones) y SELECT + △ |
| `overscan.cfg` | Uno, dos o cuatro números: `8`, `8 6` o `izq der arr abajo` | Recorte de la imagen en píxeles de la N64 (a 320 × 240) por lado. Por omisión `8 6`; `0` lo apaga; máximo 64. Lo escribe el menú (`0`, `8 6` o `16 12`) |
| `video.cfg` | `1080` | Pide salida 1080p con el render de la N64 a 4× (1280 × 960). Sin archivo, 720p a 3× (960 × 720). Si la tele o el XMB no aceptan 1080, usa el siguiente modo. Lo escribe el menú (`1080` o `720`) |
| `controles.cfg` | Líneas `BOTON_N64 = BOTON_DS3`; `#` comenta | Mapeo del control (abajo) |
| `guardado.cfg` | Líneas `CLAVE = tipo`; `#` comenta | Tipo de guardado por juego (abajo) |
| `audio.cfg` | Líneas `clave = valor` | Audio (abajo) |
| `ultima-rom` | Ruta de la última ROM abierta | Lo escribe el menú; no hace falta tocarlo |
| `log.cfg` | `ip` o `ip:puerto` | Destino del log por UDP, para quien depura (por omisión, difusión al puerto 18194) |
| `turbo` | Basta con que exista | Sin límite de velocidad (para medir; no es un modo de juego) |
| `capturas` | Basta con que exista | Guarda capturas de pantalla (`screen-N.ppm`) cada pocos segundos; frena la imagen un instante en cada una |
| `lle` | Vacío o `all` | Prueba el RSP de bajo nivel (experimental, lento). Vacío: solo microcódigo sin HLE; `all`: todo |
| `gfx_ppe` | Basta con que exista | Corre la HLE gráfica en el PPE en lugar del SPE (para comparar tiempos) |
| `embedded` | `file:nombre` | Abre una ROM sin menú (sección anterior) |

La carpeta también contiene `states/` (estados guardados), el log (`salsa64.log`) y archivos de diagnóstico
que escribe la aplicación (`gfxdump.bin`, `screen-N.ppm`, `menu.ppm`);
no hace falta tocarlos.

### `controles.cfg`

```
# comentario
Z = R2
C_ABAJO = CIRCULO
zona_muerta = 12
```

- N64: `A B Z START L R ARRIBA ABAJO IZQUIERDA DERECHA C_ARRIBA C_ABAJO
  C_IZQUIERDA C_DERECHA`.
- DualShock 3: `CRUZ CIRCULO CUADRADO TRIANGULO L1 R1 L2 R2 L3 R3 START SELECT
  ARRIBA ABAJO IZQUIERDA DERECHA NADA`.
- Ajustes del stick: `zona_muerta` (0 a 100), `rango` (1 a 127), `umbral_c`
  (1 a 127).
- Mayúsculas o minúsculas da igual. Las líneas que no se entienden se
  ignoran (y se anotan en el log).

### `guardado.cfg`

Una línea por juego; gana la más específica.

```
NZL = sram256k        # código de 3 caracteres: todas las regiones
NK4E = eeprom16k      # código de 4: una región
EC7011B7 = ninguno    # CRC1 de la cabecera (8 dígitos hex): una ROM
* = eeprom4k          # cualquier ROM
```

Tipos: `ninguno`, `eeprom4k`, `eeprom16k`, `sram256k` (o `sram`). Los demás
(`sram768k`, `flashram`, `sram1m`) se reconocen pero no se emulan.

### `audio.cfg`

| Clave | Valores | Efecto |
|---|---|---|
| `politica` | `estirar` (por omisión) o `silencio` | Qué hacer con el audio si la emulación va lenta |
| `nivel_ms` | número (por omisión 60) | Colchón mínimo de audio |
| `hle` | `spe` (por omisión), `ppe` o `no` | Dónde corre la emulación del audio |
| `salida` | `si` (por omisión) o `no` | Abrir o no el puerto de audio |

Hay otras dos claves de diagnóstico (`tarea_us`, `volcado_s`) en
`src/frontend/audio_out.h`.
