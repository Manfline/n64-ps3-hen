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

## Qué significa para Salsa64

- Parten de la misma familia de código que podríamos usar nosotros (Wii64,
  Mupen64Plus), pero con todo el trabajo en un solo hilo del PPE.
- Nuestra diferencia es la arquitectura: el RSP gráfico y el de audio y las
  texturas van en los SPEs, y el PPE se dedica a la CPU de la N64.
- Meta: superar sus números en los juegos objetivo de `docs/proyecto.md`, en las
  mismas escenas y región.
