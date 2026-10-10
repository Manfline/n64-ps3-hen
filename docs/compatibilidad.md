# Compatibilidad

Censo de los juegos objetivo (tabla de docs/proyecto.md) y algunos extra con
`salsa64-run` en la Mac: intérprete, 3 minutos de consola (1,69e10 ciclos)
sin tocar el control, RDRAM de 8 MB. Los ROMs son respaldos propios, fuera
del repositorio; aquí solo hay datos de compatibilidad (códigos,
microcódigos, direcciones).

Se repite con `tools/censo.sh rom...`: compila `salsa64-run` con el
compilador de la Mac (el Docker de desarrollo no ve las carpetas de fuera del
proyecto) y deja log e imágenes en `build/censo/`. Los textos, CRC y palabras
de identificación de los microcódigos salen de `--trace-rsp`.

## 2026-10-10 (2) — main 3270538 (F3D integrado) + osCicId

Código medido: esta rama (arranque del 6105 y `osCicId`) con main 3270538
(audio en el SPE 1, bucles de espera mapeados, F3D 2.0D/2.0G y F3DEX
0.95/1.x, el DP se cierra tras un microcódigo sin HLE). Antes de integrar
F3D se midió con su rama fusionada aparte: los mismos números.

| Juego | Región / versión | CIC | Guardado | Gráficos (HLE) | Audio (HLE) | A los 3 minutos |
|---|---|---|---|---|---|---|
| Super Mario 64 | NTSC-U, NSME v0 | 6102 | EEPROM 4K | F3D 2.0D: 5252 tareas, 0 comandos sin implementar | ABI 1: 10757 tareas, 0 errores | **Título** («PRESS START» con la cara de Mario) |
| Zelda: Ocarina of Time | NTSC-U 1.2, CZLE v2 | 6105 | SRAM 256K | F3DZEX 2.06H + S2DEX 2.05: 3724 tareas | nead (OoT): 10494 tareas, 0 errores | Demos del título (escena 3D de bosque) |
| Mario Kart 64 | NTSC-U, NKTE v0 | 6102 | EEPROM 4K | F3DEX 0.95: 5101 tareas | nead (MK64): 10707 tareas, 0 errores | Intro (bandera de cuadros) |
| Star Fox 64 | NTSC-U, NFXE v0 | 6101 | EEPROM 4K | F3DEX 1.22 NoN: 5375 tareas | nead (SF64): 10654 tareas, 0 errores | Intro («We need your help, Star Fox!») |
| GoldenEye 007 | NTSC-U, NGEE v0 | 6102 | EEPROM 4K | F3D 2.0G (Rare): 10127 tareas | ABI 1 (GoldenEye): 5362 tareas, 0 errores | Créditos de la intro; VI de 440×326 |
| Perfect Dark | NTSC-U, NPDE v0 | 6105 | EEPROM 16K | **Sin HLE**: 6761 tareas con microcódigo desconocido (sin texto) | naudio con MP3: 5208 tareas, **todas con error** | Corre (VI 320×218 en el Expansion Pak), no dibuja ni suena |
| Banjo-Kazooie (extra) | NTSC-U, NBKE v0 | 6103 | EEPROM 4K | F3DEX 1.21: 4604 tareas | naudio (BK): 5365 tareas, **todas con error** | Demo de juego (Banjo en un nivel) |
| Conker's Bad Fur Day (extra) | NTSC-U, NFUE v0 | 6105 | EEPROM 16K | F3DEXBG.NoN fifo 2.08 (variante de F3DEX2): **1 tarea** a los 29,5 s, 2 comandos sin implementar (último 0x14), y no manda más | naudio (Conker): 5381 tareas, **todas con error** | VI 292×213; espera tras la primera tarea gráfica |

Todos corren 3 minutos sin excepciones raras (interrupciones, coprocesador
no usable por el cambio perezoso de la FPU y, en GoldenEye y Conker, fallos
de TLB porque paginan código desde la ROM). Bucles de espera: 62 % (Perfect
Dark) a 98 % (Conker); GoldenEye pasó de 0 % a 86 % con los bucles mapeados de
main.

Código fuera de KSEG0 (PC muestreado al final de cada bloque): 100 % en
GoldenEye, Perfect Dark y Conker (código en 0x7000xxxx, 0x1000xxxx y
0x1500xxxx por la TLB); 0 % en los demás. Con el recompilador: Perfect Dark
6289 de 6294 bloques en 10,7 s de consola (qemu, `--lockstep` sin
diferencias) y 6078 de 6083 en la PS3 real.

### Conker: `osCicId`

Sin este commit, Conker se quedaba a los 88 mil ciclos en su hilo ocioso
sin configurar el VI ni mandar tareas: su hilo ocioso crea el hilo principal
pero **solo lo arranca si `osCicId` (0x80000310) vale 6105**, y el arranque
HLE no escribía esa palabra. El IPL3 del 6105 deja ahí su número y el del
6103, el suyo (Banjo-Kazooie y Diddy Kong Racing comprueban 6103; Perfect
Dark lo comprueba al cerrar puertas; DK64 también lo espera). Ahora el
arranque escribe 6103 o 6105 según la variante (docs/cpu-vr4300.md).

Después corre su lógica y su audio, configura el VI y a los 29,5 s manda su
primera tarea gráfica (microcódigo «F3DEXBG.NoN fifo 2.08», ucode en
0xBEBB0 con ucode_size 0, data en 0xC0050). La HLE la toma como F3DEX2, deja
2 comandos sin implementar (el último 0x14) y el juego no manda otra en los
3 minutos: le toca al trabajo de F3D. Su audio usa naudio de Conker
(`ucode_data` 00000000 1ab0140c 1fa41340 f0000f00).

### Perfect Dark

- Antes del arranque del 6105: `b .` en 0x700017F0 a los 15 M ciclos,
  porque 0xA00002E8 ≠ 0xC86E2000 (la palabra la deja el IPL3 del 6105). 201
  bloques en la PS3.
- Ahora: monta 0x7F000000 en la TLB, configura el VI, manda su primera
  tarea gráfica en el ciclo 186 M (ucode en 0x5A0B0, data en 0x89AD0, flags
  2 = DP_WAIT, sin texto: variante de Rare que GLideN64 llama F3DPD) y audio
  desde el ciclo 585 M. Con el DP cerrado tras el microcódigo desconocido
  (rama de F3D) sigue corriendo y alterna búferes de 320×218 en el Expansion
  Pak.
- PS3 real (frontend «Salsa64 arranque», SLSA00078, con el recompilador,
  antes de la rama de F3D): pasa la comprobación; 59,8 campos/s, 93,7 MIPS,
  5,8 ms de emulación por campo, pantalla negra.
- Sus otras comprobaciones del «bootloader» (ROM 0xA5C, 0x454, 0xDC0, según
  `docs/piracychecks.md` del decomp) leen la ROM del cartucho (0xB0000A5C):
  no dependen del arranque. La de `osCicId` (al cerrar puertas) queda
  cubierta por este commit.
- El desafío del CIC 6105 por la PIF durante el juego no apareció en 3
  minutos.

### Identificación de los microcódigos

| Juego | Gráfico: texto en `ucode_data`; CRC-32 de ucode / data | Audio: `ucode_data` en 0x00, 0x10, 0x28, 0x30 |
|---|---|---|
| SM64 | «RSP SW Version: 2.0D, 04-01-96»; 11d4da6c / b0de6837 | 00000001 11181470 1e24138c f0000f00 (ABI 1) |
| OoT | «RSP Gfx ucode F3DZEX.NoN fifo 2.06H» y «RSP Gfx[Safe] S2DEX fifo 2.05»; 72fea817 / 56159df6 | 00000001 1f681230 1b341190 17841830 (nead de OoT/MM) |
| MK64 | «RSP Gfx ucode F3DEX 0.95»; cd02d1ad / 270ed8eb | 00000001 11181350 1a18126c 1a8c1adc (nead de Mario Kart) |
| SF64 | «RSP Gfx ucode F3DEX.NoN 1.22»; cc888e6c / 5b69be26 | 00000001 110412cc 1b2c120c 180418b0 (nead de Star Fox) |
| GoldenEye | «RSP SW Version: 2.0G, 09-30-96»; 9cba9d04 / 2a9b4b97 | 00000001 11181470 1dc8138c f0000f00 (ABI 1 de GoldenEye; ucode_size = 0) |
| Perfect Dark | sin texto; c543d0a8 / 49fd74ec | 00000000 1ae8143c 1fb01358 f0000f00 (naudio de Banjo-Tooie/JFG/PD) |
| Banjo-Kazooie | «RSP Gfx ucode F3DEX 1.21»; 9fc2ac6e / 85d611d3 | 10f013a0 00001280 00010010 00020004 (naudio de Banjo-Kazooie) |
| Conker | «RSP Gfx ucode F3DEXBG.NoN fifo 2.08»; ucode_size 0 / 1c5c25dc | 00000000 1ab0140c 1fa41340 f0000f00 (naudio de Conker) |

Los nombres de las variantes de audio siguen la identificación de
mupen64plus-rsp-hle (por esas mismas palabras; solo consulta). Los CRC de
`ucode_data` del audio cambian entre tareas en algunos juegos: los datos
tienen partes que el microcódigo escribe; las cuatro palabras son estables.

### Lo que falta, por prioridad

| # | Qué | Desbloquea | Quién |
|---|---|---|---|
| 1 | HLE de naudio: la de Banjo-Kazooie, la de Conker y la de Perfect Dark (con MP3); hoy toda tarea da error | Sonido en los tres juegos de Rare (y Banjo-Tooie, JFG) | trabajo de audio (`src/rsp/audio`, SPE 1) |
| 2 | HLE del microcódigo de Perfect Dark (variante de Rare de F3DEX sin texto: identificar por CRC, vértices propios) | Perfect Dark | trabajo de F3D |
| 3 | F3DEXBG 2.08 de Conker: el comando 0x14 y lo que le impide mandar la segunda tarea | Conker | trabajo de F3D |
| 4 | Probar con el control (pasar los títulos; `--pad` de la rama de F3D) y medir en la PS3 las escenas de docs/proyecto.md | Mediciones de la tabla de juegos | tras 1–3 |
| 5 | Desafío del CIC 6105 por la PIF durante el juego (no apareció en 3 minutos) | Perfect Dark, Conker, Banjo-Tooie a largo plazo | PIF (`src/core/pif.c`), cuando aparezca |
| 6 | Zelda: en una captura de la demo el cielo aparece en la parte de abajo; revisar si es el encuadre o geometría que falta | Zelda | trabajo de F3D |

## 2026-10-10 (1) — primera pasada, sin F3D ni audio

Con el arranque del 6105 y antes de main f698afe: solo Zelda dibujaba. Los
demás mandaban una tarea gráfica con un microcódigo sin HLE y se quedaban
esperando la interrupción del DP, porque esa tarea terminaba con TASKDONE
pero sin G_RDPFULLSYNC. Un experimento local (disparar el DP tras esa tarea)
mostró que los seis seguían 3 minutos sin otro bloqueo; la rama de F3D hace
ahora eso mismo. Tareas del RSP en esos 3 minutos: SM64 10765 de audio,
OoT 3724 gráficas y 10494 de audio, MK64 10763, SF64 5384, GoldenEye 5362,
Perfect Dark 5203, Banjo-Kazooie 5365 (de audio, más la gráfica sin HLE).
