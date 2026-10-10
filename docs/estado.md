# Estado

## 2026-10-10 — Fase 1 iniciada: intérprete del VR4300

Hecho (diseño en docs/cpu-vr4300.md):
- Intérprete del VR4300 en `src/cpu/`: ALU de 32/64 bits con
  desbordamiento, cargas/almacenamientos (incluidos LWL/LWR/LDL/LDR y sus
  stores), LL/SC, saltos con delay slot y branch-likely, HI/LO, MULT/DIV y
  variantes de 64 bits, traps; COP0 (Count/Compare, Status, Cause, EPC,
  ErrorEPC, excepciones, ERET, TLB de 32 entradas, modos usuario/supervisor);
  COP1 con modo FR, causas/banderas de FCR31, conversiones y comparaciones.
- Bus y mapa físico en `src/core/`: RDRAM de 4/8 MB con camino rápido desde
  KSEG0/KSEG1, SP DMEM/IMEM, ROM, PIF RAM. MI completo (interrupciones →
  IP2); PI y SI con DMA inmediato; VI, AI, SP, DP como registros. Planificador
  de eventos (Compare). Sin asignaciones en el bucle.
- Portable: mismo código para ppu-gcc (big-endian, accesos directos) y gcc
  nativo (arm64, con byte-swap).
- Pruebas: `tools/build.sh test-host` (139 pruebas, ASan + UBSan) y
  `tools/test-phase1.sh` (las mismas en RPCS3, big-endian). Todas pasan en
  ambos. Prueba de mutación: romper SRA, branch-likely o EPC las hace fallar.
- Micro-benchmark del intérprete (bucle mixto con FPU): 237 MIPS en el host
  (Mac Studio, Docker arm64); 35,6 MIPS en RPCS3 (no concluyente).
- Suites elegidas y licencias verificadas (docs/THIRD_PARTY.md):
  nemu64-test (MIT, IPL3 libre) como principal; CPUTest de krom (Unlicense)
  solo ensamblado desde fuente, porque sus ROMs traen el IPL3 de Nintendo.
  Dillonb/n64-tests descartada (sin licencia).

Siguiente (Fase 1):
- [ ] Correr `retro64-cpu-tests` en la PS3 real: primer número de MIPS del
      intérprete en el PPE (docs/perf-log.md).
- [ ] Arranque HLE: cargar la ROM, copiar los primeros 1 MB a RDRAM, dejar
      registros y DMEM como el IPL3 (lo que nemu64-test espera: tamaño de
      RAM en DMEM[0], TV en DMEM[9], offset del ELF en DMEM[12]).
- [ ] ISViewer (0x13FF0000) para leer la salida de nemu64-test.
- [ ] Contenedor para compilar nemu64-test (Rust nightly-2026-07-16 +
      nust64 0.4.1) y guardar el ROM en `tests/roms/homebrew/`.
- [ ] VI con contador de líneas e interrupción; framebuffer 2D en el RSX.
- [ ] Pendientes del CPU: direccionamiento de 64 bits, bandera Inexact,
      ciclos por instrucción (docs/cpu-vr4300.md).

## 2026-10-10 — Fase 0: completa en RPCS3, falta la PS3 real

Hecho:
- Imagen `retro64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
  PPU, 9.5 SPU) + PSL1GHT + cgcomp con Cg Toolkit vía qemu. Los 54 shaders de
  la rama `ps3` del port de SM64 compilan; 18/31 samples de PSL1GHT compilan
  (las fallas son de los samples).
- Programas de la Fase 0, compilados sin advertencias (`tools/build.sh`):
  - `tests/phase0/rsx-triangle`: triángulo + verificación de píxeles.
  - `tests/phase0/spu-compute`: 6 SPUs, DMA de doble búfer, verificación exacta.
  - `tests/phase0/jit-ppe`: 8 mecanismos de memoria, tolera caídas.
  - `tests/phase0/udplog`: 200 líneas numeradas por UDP.
- Logger TTY + UDP (`src/platform/ps3/log.c`) y receptor `tools/udplog.py`.

- RPCS3 0.0.43 + firmware 4.92 en la Mac (docs/rpcs3.md). `tools/test-phase0.sh`
  corre las cuatro pruebas: udplog, spu-compute y rsx-triangle pasan;
  jit-ppe completa su ciclo, pero RPCS3 no ejecuta código generado (no
  concluyente).

Falta para cerrar la Fase 0:
- [x] Correr los 4 programas en RPCS3 y automatizarlo.
- [ ] PS3 real: correr los 4 programas (rsx-triangle, spu-compute, udplog,
      jit-ppe con HEN y, si se puede, CFW). Llenar docs/jit-ppe.md.
- [ ] Decidir el plan de la Fase 3 según el resultado del JIT.
- [ ] Antes: conectar la PS3 (está desconectada; revisar su estado). Datos a
      reunir: IP por cable, versión de firmware, apps de envío instaladas y si
      hay capturadora. Luego correr la Fase 0 y `retro64-cpu-tests` (primer
      número de MIPS real).
- [ ] Opcional: correr Cell64 en la PS3 y guardar su `perf_trace.csv`
      (docs/referencia-cell64.md).

Nombre: **Retro64** (decidido el 2026-10-10; antes "Cell64", provisional).
- Ya existe un emulador de N64 para PS3 llamado **Cell64**: v1.52 de
  "ZeBobbel", publicado en Brewology el 2026-09-27
  (store.brewology.com/ahomebrew.php?brewid=344), con una variante "HEN FIX by
  DropSonic DS". Sin código fuente publicado ni descripción técnica; el
  "sitio del desarrollador" es un canal de YouTube con dos videos de prueba.
  Por eso se renombró el proyecto. La carpeta sigue siendo `projects/cell64`.
- Referencia de rendimiento de ese emulador (videos de ZeBobbel, capturadora,
  build "40031911"): el overlay dice "INT" (intérprete; formato del overlay de
  Wii64/Not64). Super Mario 64 en el castillo: 46,9 VI/s (~78 %). Zelda OoT,
  pantalla de título: 26,4 VI/s, 9,1 FPS (~44 %). No se vio modo "DYN".
  En el video de la v1.50 (overlay nuevo, sin indicar INT/DYN): Zelda OoT en
  el menú de archivos a 50,0 VI/s y 50,0 FPS; probablemente versión PAL (50 Hz),
  o sea ~100 % en una escena ligera. En juego (bosque Kokiri): 41,3 VI/s y
  13,8 FPS (relación 3:1, coherente); ~83 % si es PAL, ~69 % si es NTSC.
  Meta para nuestro emulador: superar eso en la misma escena y región.
  Comparar contra esto nuestro intérprete (Fase 1) y el recompilador (Fase 3),
  con los mismos juegos en la misma PS3.

Notas:
- No usar `-Os` en el PPU: ICE de GCC 13.2 (docs/toolchain.md).
- En RPCS3, rsx-triangle necesita "Write Color Buffers" para su verificación.
- Archivos de Retro64 en la PS3: `/dev_hdd0/retro64/` (RPCS3 vacía `/dev_hdd0/tmp`).
- Juegos objetivo: tabla en docs/proyecto.md. Comparación con Cell64 en
  docs/referencia-cell64.md (no usa SPEs; todo en un hilo del PPE).
- Ideas sin decidir: versión rápida del intérprete (decodificar una vez,
  como el "FASTINT" de Cell64) y, como investigación lejana, emular la CPU en
  un SPE solo si el PPE no alcanza.
