# Referencia: el emulador Cell64 (ZeBobbel)

Otro emulador de N64 para PS3, publicado antes que Salsa64 (por él se renombró
este proyecto). Lo usamos como línea base de rendimiento y para comparar
arquitecturas. Datos del 2026-10-10.

## Qué es

- "Cell64", publicado por ZeBobbel en Brewology
  (store.brewology.com/ahomebrew.php?brewid=344). v1.52 del 2026-09-27, con
  una variante "HEN FIX" de DropSM. Según los videos, requiere HEN o CFW.
- Paquete probado: "Cell64 Compatibility Test 01.52", título `CLFI00001`,
  content ID `UP0001-CLFI00001_00-CELL64FASTINT052`, build
  `01.52-compat-hw-001`, compilado el 2026-09-24.

## Base de código y licencia

Según el `THIRD_PARTY_NOTICES.txt` que trae el paquete:

- Es una continuación de `emukidid/wii64-ps3`, que viene de Wii64 y Mupen64.
  La versión se distribuye bajo **GPLv2**; los archivos heredados permiten
  GPLv2 o posterior.
- Gráficos: `glN64_GX` (glN64 de Orkin, port de sepp256), con un backport de
  F3DZEX2 tomado de GLideN64.
- RSP HLE: `mupen64plus-rsp-hle`, importado del commit `1441a5f` de Wii64 y
  comparado con mupen64plus-rsp-hle `8a7a472` (release 2026.6).
- Además: CIC-NUS-6105 de X-Scale (BSD), adler32 de zlib, y Project64 como
  referencia para el latch de escritura del bus del cartucho.
- Menciona una "corresponding source distribution", pero no encontramos el
  código publicado. Por la GPL debería poder obtenerse.

**Si algún día tomamos código de Cell64:** sus cambios son GPLv2 a secas, así
que la obra combinada quedaría en GPLv2 sin "o posterior". Preferir las
fuentes originales (Wii64, Mupen64Plus: GPLv2 o posterior) y registrar el
origen en `docs/THIRD_PARTY.md`.

## Cómo reparte el trabajo (observado en RPCS3)

Método: RPCS3 0.0.43 con firmware 4.92, el paquete anterior instalado y Zelda
OoT (respaldo propio, NTSC) durante unos 6 minutos. Solo se observó su
comportamiento: llamadas al sistema en `RPCS3.log` y su propia traza
`perf_trace.csv`. No se desensambló el binario.

- **No usa los SPEs.** No crea ningún hilo de SPU: ni grupos de hilos, ni
  SPUs crudos, ni SPURS. `sys_spu_image_import` está enlazado, pero nunca se
  llama.
- **Hilos del PPE.** El principal hace la CPU de la N64, el RSP gráfico, el RSP
  de audio y la presentación. `cell64_audio` alimenta la salida de audio. Hay
  además dos hilos del sistema (eventos de configuración e interrupciones del
  RSX).
- **CPU:** intérprete. El content ID dice "FASTINT" y los overlays de builds
  anteriores muestran "INT".
- **Audio:** `cellAudio`; el HLE de audio usa VMX, el AltiVec del PPE
  (`envmix_vmx=1`).
- **Video:** `cellGcm` estándar a 1280×720.
- **Archivos:** ROMs en `/dev_usb000/cell64/roms/`; configuración, partidas y
  trazas en `/dev_usb000/cell64/`.

## Su traza de rendimiento

Escribe `/dev_usb000/cell64/perf_trace.csv`: una línea de encabezado
`# cell64_perf_v1,...` con la ROM y sus opciones de optimización, y luego una
fila `window` por segundo con 36 columnas. Las más útiles:
`speed_permille`, `measured_vis`, `measured_fps`, `cpu_other_us`,
`rsp_gfx_us`, `rsp_audio_us`, `texture_update_us`, `rsx_wait_us`,
`present_us`, `limit_sleep_us`, `draw_calls` y `triangles`.

Promedios en RPCS3 sobre 360 ventanas. **Son tiempos de la Mac, no del Cell:**
sirven solo para ver la estructura.

| Medida | Promedio por segundo |
|---|---|
| Velocidad | 99,3 % (59,6 VI/s) |
| `cpu_other_us` | 169 ms |
| `rsp_gfx_us` | 55 ms |
| `rsp_audio_us` | 14 ms |
| `texture_update_us` | 11 ms |
| Llamadas de dibujo / triángulos | ~3200 / ~12 100 |

**Lo valioso:** esa misma traza en la PS3 real mediría cuánto le cuestan al
PPE el RSP gráfico, el de audio y las texturas. Es justo el tiempo que Salsa64
libera al mover esas tareas a los SPEs. Si el autor corre Cell64 en la
consola, guardar el `perf_trace.csv` (con la región y la escena) en
`build/logs/`.

## Rendimiento en PS3 según sus videos

Ver `docs/estado.md`: Mario 64 ~78 % y Zelda en Kokiri ~83 % (PAL, v1.50).
GoldenEye y Perfect Dark, injugables.

## En PS3 real (2026-10-10)

Consola: Slim CECH-3011A, HFW 4.91 + HEN. Zelda: Ocarina of Time (USA, Rev B),
respaldo del usuario.

- **Paquete:** el de 30 MB (`01.52`) da error al lanzarlo con HEN; funciona
  la variante de 570 KB («HDD0», la de HEN), mismo título `CLFI00001`.
- **ROMs:** esa variante redirige `/dev_hdd0/cell64` hacia la ruta de la USB
  y lee de **`/dev_hdd0/cell64/roms/`** (carpeta real). Redirigir además
  `/dev_usb000` con webMAN (`/remap.ps3`) colgó la consola en el menú: no
  encadenar redirecciones.
- **Trazas:** el arranque en `/dev_hdd0/tmp/cell64_boot_stage.txt` (solo
  hasta el segundo cuadro del menú y luego la carga de la ROM); el rendimiento
  en `/dev_hdd0/cell64/perf_trace.csv`. Salida a 1920×1080.
- **Velocidad** (traza de ~130 s, `perf_trace.csv` por ventanas de 60 VI):
  - Introducción y menú: 100 % (~20 cps, lo normal del juego).
  - Gameplay en el **bosque Kokiri** (NTSC): **45–50 VI/s (75–83 %), ~15 cps**
    según el overlay de Cell64; en la traza, 57–82 % según la ventana. Limita la CPU: el
    intérprete gasta 0,9–1,3 s del PPE por cada segundo de juego (correlación
    −0,85 entre velocidad y `cpu_other_us`). El RSP gráfico (0,25–0,4 s/s) y el
    de audio (~0,1 s/s) también corren en el PPE. El audio se queda sin datos
    (30–118 underruns por ventana): eso son los tirones.
  - Escena más ligera (VI 4441–6721, ~38 s): 75–78 % **durmiendo ~0,5 s por
    ventana** en el limitador, con 50 underruns por ventana. El limitador duerme
    aunque vaya atrasado: falla de sincronización, no de potencia.
- **Fallas gráficas:** en la pantalla de nombre salen **cuadros blancos en
  lugar de letras**; los diálogos y el gameplay se ven bien. Como la fuente de
  los diálogos sí sale, la falla está en cómo se carga la del teclado de la
  pantalla de nombre (formato de 4 bits o forma de carga a TMEM), y el
  combiner solo pinta el color primitivo. Caso de prueba para la Fase 4 de
  Salsa64.
- **Espada Kokiri:** en la animación de obtenerla (Link la levanta y se ve
  el modelo) la hoja sale **sin textura, en blanco y negro** (solo sombreado);
  empuñada en el gameplay se ve bien. Así que no es la textura de la espada:
  falla el modelo de «objeto obtenido», que el juego carga aparte en el momento
  y dibuja con su propia configuración (algunos con mapeo de entorno,
  `G_TEXTURE_GEN` + `LookAt`). Caso de prueba para el HLE de F3DEX2 de Salsa64
  (Fase 2) y la Fase 4: abrir el cofre de la espada.
- **Menú de pausa:** tarda en abrir y la pantalla de equipo se ve mal. En la
  traza, al abrirlo hay ~5 s sin presentar cuadros (`measured_fps` = 0) con la
  CPU al tope y luego ~1 MB de texturas. `framebuffer_copies` vale 0 en toda la
  sesión: Cell64 no copia el framebuffer del RSX a la RDRAM, y el juego lo
  necesita para el fondo de la pausa (lo filtra la CPU) y para dibujar a Link
  en la pantalla de equipo (render a textura). Para Salsa64: emular esas
  copias, al menos por juego (ver «Riesgos conocidos» en docs/proyecto.md).
- **Para Salsa64:** el cuello es el mismo que el nuestro (intérprete en el
  PPE). Mover el RSP gráfico y el de audio a SPEs libera ~30–40 % del PPE en
  gameplay; el limitador debe dejar de dormir cuando va atrasado.

## Qué significa para Salsa64

- Parten de la misma familia de código que podríamos usar nosotros (Wii64,
  Mupen64Plus), pero con todo el trabajo en un solo hilo del PPE.
- Nuestra diferencia es la arquitectura: el RSP gráfico y el de audio y las
  texturas van en los SPEs, y el PPE se dedica a la CPU de la N64.
- Meta: superar sus números en los juegos objetivo de `docs/proyecto.md`, en las
  mismas escenas y región.
