# Compatibilidad

Censo de los juegos objetivo (tabla de docs/proyecto.md) con `salsa64-run` en la
Mac: intérprete, 3 minutos de consola (1,69e10 ciclos) sin tocar el control,
RDRAM de 8 MB. Los ROMs son respaldos propios, fuera del repositorio; aquí
solo hay datos de compatibilidad (códigos, microcódigos, direcciones).

Se repite con `tools/censo.sh rom...` (compila `salsa64-run` con el
compilador de la Mac, porque el Docker de desarrollo no ve las carpetas de
fuera del proyecto, y deja log e imágenes en `build/censo/`). Los textos y
CRC de los microcódigos salen de `--trace-rsp`.

## 2026-10-10 — commit del arranque del 6105 (rama `amazing-noether-39a548`)

### Resumen

| Juego | Región / versión | CIC | Guardado | ¿Dibuja? | Dónde se queda |
|---|---|---|---|---|---|
| Super Mario 64 | NTSC-U, NSME v0 | 6102 | EEPROM 4K (tabla) | No | Primera tarea gráfica (F3D) sin HLE; espera al DP |
| Zelda: Ocarina of Time | NTSC-U 1.2, CZLE v2 | 6105 | SRAM 256K (tabla) | **Sí** | Sigue sin trabarse; a los 3 minutos dibuja una escena 3D de bosque (demos del título) |
| Mario Kart 64 | NTSC-U, NKTE v0 | 6102 | EEPROM 4K (tabla) | No | Primera tarea gráfica (F3DEX 0.95, el detector no la clasifica); espera al DP |
| Star Fox 64 | NTSC-U, NFXE v0 | 6101 | EEPROM 4K (tabla) | No | Primera tarea gráfica (F3DEX 1.22) sin HLE; espera al DP |
| GoldenEye 007 | NTSC-U, NGEE v0 | 6102 | EEPROM 4K (tabla) | No | Primera tarea gráfica (F3D de Rare) sin HLE; espera al DP |
| Perfect Dark | NTSC-U, NPDE v0 | 6105 | EEPROM 16K (tabla) | No | Pasa la comprobación del 0x2E8 (arranque del 6105, este commit); primera tarea gráfica (variante de Rare, sin texto) sin HLE; espera al DP |
| Banjo-Kazooie (extra) | NTSC-U, NBKE v0 | 6103 | EEPROM 4K (tabla) | No | Primera tarea gráfica (F3DEX 1.21) sin HLE; espera al DP |

«Espera al DP»: la tarea con un microcódigo que la HLE no reconoce termina
con TASKDONE pero sin la interrupción del DP (no hay G_RDPFULLSYNC que la
dispare), y el planificador de libultra no manda la siguiente tarea gráfica
nunca: el juego sigue mandando audio y queda en su hilo ocioso. Experimento
(local, no está en el repo): disparando también la interrupción del DP tras
esa tarea, **los seis siguen** durante los 3 minutos —28 a 56 tareas
gráficas por segundo, el VI alterna búferes— sin excepciones raras. En este
tramo no hay otro bloqueo que la HLE gráfica.

### Por juego

Tareas del RSP en 3 minutos (gfx = con HLE; «desc.» = gráficas con
microcódigo desconocido). «pc» es el PC al final de cada bloque de 2^20
ciclos (muestreo: cuánto del tiempo corre código mapeado por la TLB).

| Juego | Microcódigo gráfico (texto en `ucode_data`; CRC-32 de ucode / data) | Audio (`ucode_data` en 0x00, 0x10, 0x28, 0x30) | Tareas RSP (gfx / desc. / audio / otras) | Campos del VI | Bucles de espera | pc fuera de KSEG0 |
|---|---|---|---|---|---|---|
| SM64 | «RSP SW Version: 2.0D, 04-01-96» → F3D (gbi 3, pendiente); ucode 11d4da6c, data b0de6837 | 00000001 11181470 1e24138c f0000f00 (ABI 1) | 0 / 1 / 10765 / 0 | 10769 (VI sin imagen, origen 0x27F) | 99 % | 0 % |
| OoT | «RSP Gfx ucode F3DZEX.NoN fifo 2.06H» y «RSP Gfx[Safe] S2DEX fifo 2.05» → F3DZEX2 (gbi 2); ucode 72fea817, data 56159df6 | 00000001 1f681230 1b341190 17841830 (nead de OoT/MM) | 3724 / 0 / 10494 / 1 (tipo 4, ucode de 0x20 bytes) | 10769 | 87 % | 0 % |
| MK64 | «RSP Gfx ucode F3DEX 0.95» → desconocido (gbi 0: el detector solo acepta 1.x y 2.x); ucode cd02d1ad, data 270ed8eb | 00000001 11181350 1a18126c 1a8c1adc (nead de Mario Kart) | 0 / 1 / 10763 / 0 | 10769 (sin imagen) | 99 % | 0 % |
| SF64 | «RSP Gfx ucode F3DEX.NoN 1.22» → F3DEX (gbi 4, pendiente); ucode cc888e6c, data 5b69be26 | 00000001 110412cc 1b2c120c 180418b0 (nead de Star Fox) | 0 / 1 / 5384 / 0 | 10769 (sin imagen) | 99 % | 0 % |
| GoldenEye | «RSP SW Version: 2.0G, 09-30-96» → F3D (gbi 3, pendiente; Rare lo modificó); ucode 9cba9d04, data 2a9b4b97 | 00000001 11181470 1dc8138c f0000f00 (ABI 1 de GoldenEye; la tarea trae ucode_size = 0) | 0 / 1 / 5362 / 0 | 10769 (RDRAM sin inicializar: ruido) | 0 % (`b .` mapeado en 0x7000071C) | 100 % |
| Perfect Dark | **Sin texto** en `ucode_data` (variante de Rare de F3DEX; GLideN64 la llama F3DPD) → desconocido; ucode c543d0a8, data 49fd74ec | 00000000 1ae8143c 1fb01358 f0000f00 (naudio de Banjo-Tooie/JFG/PD) | 0 / 1 / 5203 / 0 | 10769 (sin imagen) | 0 % (hilo ocioso mapeado en 0x70001938) | 100 % |
| Banjo-Kazooie | «RSP Gfx ucode F3DEX 1.21» → F3DEX (gbi 4, pendiente); ucode 9fc2ac6e, data 85d611d3 | 10f013a0 00001280 00010010 00020004 (naudio de Banjo-Kazooie) | 0 / 1 / 5365 / 0 | 10769 (RDRAM sin dibujar) | 98 % | 0 % |

Las variantes de audio se nombran según la identificación de
mupen64plus-rsp-hle (`src/main.c`, por esas mismas palabras; solo consulta).
Los CRC de `ucode_data` del audio cambian entre tareas en algunos juegos
(GoldenEye, MK64): los datos tienen partes que el microcódigo escribe; las
cuatro palabras de arriba son estables.

Zelda: la imagen de la HLE a los 3 minutos muestra el bosque con el cielo
en la parte de abajo; puede ser el encuadre de la demo o geometría que
falta (revisar con el trabajo de F3D).

Comandos gráficos desconocidos: 0 en OoT (el único que pasa por la HLE); en
los demás no se llega a interpretar ninguna lista.

Excepciones (las primeras 16 de cada juego): solo interrupciones (0),
coprocesador no usable (11, el cambio perezoso de la FPU de libultra) y, en
GoldenEye, fallos de TLB (2, pagina su código desde la ROM). Nada raro.

Código fuera de KSEG0 con el recompilador (`--rec --lockstep` en qemu):
Perfect Dark 6289 de 6294 bloques en 10,7 s de consola, sin diferencias con
el intérprete; en la PS3 real 6078 de 6083 (693 traducciones de la TLB).
GoldenEye: 3518 de 3524 (docs/estado.md, trabajo de la TLB).

### Perfect Dark en detalle

- Antes de este commit: `b .` en 0x700017F0 a los 15 M ciclos, porque
  0xA00002E8 ≠ 0xC86E2000 (la palabra la deja el IPL3 del 6105; ver
  docs/cpu-vr4300.md, «Arranque HLE»). 201 bloques en la PS3.
- Ahora: monta 0x7F000000 en la TLB, configura el VI (control 0x13016,
  sin búfer todavía), manda su primera tarea gráfica en el ciclo 186 M
  (ucode en 0x5A0B0, data en 0x89AD0, flags 2 = DP_WAIT) y audio desde el
  ciclo 585 M, y queda en su hilo ocioso (0x7000193C) esperando al DP.
- PS3 real (frontend «Salsa64 arranque», SLSA00078, con el recompilador):
  igual que en la Mac. 59,8 campos/s, 93,7 MIPS, 5,8 ms de emulación por
  campo, pantalla negra (nada dibuja).
- El siguiente bloqueo es solo gráfico: con la interrupción del DP
  (experimento) manda 800 tareas gráficas en 21 s y alterna búferes de
  320×218 en el Expansion Pak (0x622F00 a los 3 minutos). El
  desafío del CIC 6105 durante el juego (PIF) no apareció en 3 minutos.

### Lo que falta, por prioridad

| # | Qué | Desbloquea | Quién |
|---|---|---|---|
| 1 | HLE de F3D (Fast3D: SM64 «2.0D», GoldenEye «2.0G» con los cambios de Rare) y de F3DEX 1.x (Banjo-Kazooie 1.21, Star Fox 64 1.22 NoN, Mario Kart 64 0.95) | 4 de los 6 juegos objetivo y Banjo-Kazooie | trabajo de F3D (`src/rsp/gfx`) |
| 2 | Que una tarea gráfica con microcódigo desconocido no deje colgado al juego: disparar también la interrupción del DP (el experimento lo confirma en los seis) | Que el juego siga (audio, lógica, guardado) aunque no dibuje; ver más allá del primer cuadro | trabajo del fin de tarea del RSP (`src/core/rsp.c`) |
| 3 | Detector: «F3DEX 0.95» (Mario Kart 64) como F3DEX 1.x; identificar por CRC el microcódigo de Perfect Dark, que no trae texto | Mario Kart 64; Perfect Dark | trabajo de F3D (`src/core/rsp.c`, `src/rsp/gfx`) |
| 4 | HLE del microcódigo de Perfect Dark (variante de Rare de F3DEX, vértices propios) | Perfect Dark (y probablemente Banjo-Tooie, Jet Force Gemini) | trabajo de F3D, después de F3DEX 1.x |
| 5 | HLE de audio: ABI 1 (SM64; variante de GoldenEye), nead (MK64, SF64, OoT), naudio (Banjo-Kazooie; la de Perfect Dark) | Sonido en los seis | trabajo de audio (`src/rsp/audio`, SPE 1) |
| 6 | Bucles de espera en código mapeado por la TLB (`b .` en 0x7000071C de GoldenEye, hilo ocioso de Perfect Dark en 0x70001938): hoy 0 % saltado | Tiempo de PPE en los dos juegos de Rare | trabajo de bucles de espera (`src/cpu/idle.c`) |
| 7 | Desafío del CIC 6105 por la PIF durante el juego (no apareció en 3 minutos; algunos juegos de Rare lo usan más adelante) | Perfect Dark, Banjo-Tooie a largo plazo | PIF (`src/core/pif.c`), cuando aparezca |
| 8 | Probar con el control (pasar el título) y medir en la PS3 los juegos que ya dibujen | Escenas de medición de docs/proyecto.md | tras 1–5 |
