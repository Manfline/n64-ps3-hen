# Estado

## 2026-10-10 — Cierre de la jornada (2): Fase 0 y Fase 1 en PS3 real

Hecho hoy en hardware real (Slim CECH-3011A, HFW 4.91 + HEN; sin CFW posible):
- Fase 0 completa: udplog, spu-compute (6 SPUs), rsx-triangle, cpu-tests y
  jit-ppe pasan. JIT viable solo en `.text`; la caché del recompilador va en
  una cola NOBITS de 16 MB del segmento de texto (docs/jit-ppe.md).
- Fase 1 cumplida: `salsa64.pkg` muestra la ROM `vitest` (entrada de abajo).
- Intérprete en el PPE: 8,7 → 21,2 MIPS; no llegará a ~60 sin recompilador.
- Referencia Cell64 en la misma consola: Zelda en Kokiri 45–50 VI/s, ~15 cps,
  y cuatro fallas gráficas como casos de prueba (docs/referencia-cell64.md).
- Proyecto renombrado a **Salsa64**; repo privado github.com/ricowp81/Salsa64.
- Herramientas: log también en disco (`/dev_hdd0/salsa64/<prog>.log`),
  `tools/ps3-live.py` (monitor en vivo), `tools/ps3.conf` con la IP.

Siguiente (trabajos lanzados al cerrar; una prueba en la PS3 a la vez):
- [ ] Fase 2: RSP gráfico (F3DEX2) en SPE 0 + RDP mínimo en el RSX sobre
      `lib/rsxkit`, con la infraestructura de trabajos SPE.
- [ ] Fase 3: recompilador MIPS → PPC64 en el PPE.
- [ ] Controles (SI/PIF + DualShock 3) y detección de bucles de espera.
- [ ] Después de la Fase 2: audio HLE en SPE 1 sobre la misma infraestructura.

## 2026-10-10 — Fase 1: un homebrew muestra imagen (criterio cumplido)

Hecho:
- VI (`src/core/vi.c`): temporización por campos con dos eventos del
  planificador; duración desde VI_V_SYNC, VI_H_SYNC y el reloj de cada tipo
  de TV (NTSC 1 567 041 ciclos, 59,83 Hz; PAL 49,92 Hz); VI_V_CURRENT
  calculado al leerlo, interrupción en VI_V_INTR, entrelazado con campos de
  262/263 líneas. `salsa64_vi_frame` describe la imagen visible (origen,
  tamaño por VI_H_VIDEO/VI_V_VIDEO y escalas, RGBA5551/RGBA8888);
  `salsa64_n64_run_field` corre hasta el fin del campo.
- `salsa64-run --frame` guarda la imagen del VI en PPM. La pantalla de
  resultados de nemu64-test se ve completa.
- ROM `vitest` (`tests/roms/src/vitest`, propia, GPLv2+): clang 14 para MIPS
  + nust64 (IPL3 libre r7) en contenedor reproducible
  (`tools/build-vitest.sh`). Barras, rampa y cuadro animado alternando
  RGBA5551 y RGBA8888 con doble búfer en la interrupción del VI; se
  verifica sola (Count por campo exacto, V_CURRENT en el manejador).
- `lib/rsxkit` (MIT, `lib/rsxkit/README.md`): init del RSX y video, cuadro
  con doble búfer y flip en vsync, framebuffer 2D escalado (fit/entero/
  estirar, lineal/cercano) con 5 formatos leídos directo por el RSX
  (RGBA5551 = textura R5G5B5A1). Compila aparte, sin -Isrc.
- Frontend (`src/frontend`, `salsa64.pkg`, SLSA00064): ROM de
  `/dev_hdd0/salsa64/rom.z64` o vitest embebida; un campo de emulación y
  una presentación por vuelta; con vitest se verifica solo (RDRAM y pantalla
  en ambos formatos + resultado de la ROM). Sale con START + SELECT.
- Pruebas: 161 en el host y en RPCS3; nemu64-test sin cambios (462);
  `tools/test-roms.sh` corre vitest y `tools/test-phase1.sh` el frontend.
- **PS3 real** (CECH-3011A, HEN): `RESULTADO frontend: OK` en el campo 124;
  8,7 campos/s (14 % de NTSC), 13,6 MIPS, presentación 0,3 ms por campo
  (docs/perf-log.md).

Notas:
- El log UDP no llegó a `tools/udplog.py`: había otro receptor abierto en el
  mismo puerto (`jit-ppe-cache`, desde las 09:27) que se quedó con los
  paquetes unicast. El log en disco (`/dev_hdd0/salsa64/salsa64.log`, por
  FTP) sí estaba completo.
- Los 13,6 MIPS se midieron antes del perfilado. La sospecha del FPSCR no
  aplicaba; el intérprete optimizado da 21,2 MIPS en el bench (entrada
  siguiente).

Siguiente (Fase 1):
- [ ] Volver a medir el frontend con vitest con el intérprete optimizado.
- [ ] Detección de bucles de espera (vitest y los juegos esperan al VI en
      un bucle): saltar hasta el siguiente evento ahorraría casi todo el
      campo en escenas ligeras.
- [ ] SI: comandos del PIF (controles) para mapear el DualShock 3.
- [ ] Repartir emulación y presentación entre los dos hilos del PPE.
- [ ] Origen del VI: verificar contra ares si libultra deja VI_ORIGIN una
      línea después del inicio del framebuffer (aquí se toma como la
      primera línea visible).
- [ ] Bus PI, cachés, CPUTest de krom y camino plano: igual que abajo.

## 2026-10-10 — Intérprete en el PPE: 8,7 → 21,2 MIPS

Hecho (números y diagnóstico en docs/perf-log.md):
- Benchmark en variantes (mixto, nop, enteros, memoria, mov.s, add.s) con
  MIPS y ns por instrucción.
- La FPU nunca cambiaba el FPSCR: la sospecha no aplicaba. Lo caro era el
  bucle (~250 ciclos por instrucción aun para un NOP: load-hit-store, una
  llamada con marco de pila por instrucción, `lwax` en los `switch`) y, en
  la FPU, pasar valores entre registros enteros y de punto flotante por la
  pila.
- Bucle con pc/next_pc/flag de salto en registros y `cpu->redirect` para
  excepciones/ERET/set_pc; r0 nunca se escribe; NOP sin segundo despacho;
  BC1 en el bucle; `-mno-relative-jumptables` en mk/ps3.mk; camino rápido
  de la FPU (src/cpu/fpu.h) con prueba de equivalencia de 20 000 casos.
- PS3 real: mixto 8,7 → 17,1 MIPS (×2,0); nop ×3,0; add.s ×2,9. 155 pruebas
  en host y RPCS3; nemu64-test sin regresiones.

- Segunda ronda (147d5ac): SPECIAL en la misma tabla de despacho, FPU
  rápida que clasifica con los bits en enteros y una franja de exponentes
  segura (una sola comparación de punto flotante), C.cond en enteros y
  MOV/ABS/NEG/MFC1/MTC1/CFC1/DMFC1/DMTC1 en el bucle. Prueba de
  equivalencia de 30 000 casos. PS3 real: mixto 17,1 → 21,2 MIPS; mov.s
  ×1,8; add.s ×1,8; nop −10 % (cadena de despacho más larga).

Siguiente:
- [ ] Opcional, poco: índice de despacho ya escalado y calculado directo de
      la palabra de la instrucción (recupera parte de los ~9 ciclos que
      perdieron NOP y LW/SW).
- El bucle cuesta ~93 ciclos aun para un NOP: el intérprete no llegará a
  ~60 MIPS en el PPE sin decodificar por adelantado, y la dependencia entre
  instrucciones por `cpu->gpr` en memoria no se quita en un intérprete. La
  meta de velocidad completa es el recompilador (Fase 3).

## 2026-10-10 — Fase 1: arranque HLE, `salsa64-run` y nemu64-test

Hecho (detalle en docs/cpu-vr4300.md):
- Arranque HLE sin PIF ROM ni IPL3 (`src/core/boot.c`, `src/core/rom.c`):
  normaliza .v64/.n64 a .z64, reconoce el IPL3 de Nintendo por CRC-32 y tiene
  dos caminos. ELF (IPL3 libre de libdragon, el de nemu64-test): carga los
  PT_LOAD, deja los boot flags en DMEM (memoria en [0], TV en [9], ELF en
  [12]) y $sp al final de la RDRAM. Plano (IPL3 de Nintendo o compat): copia
  1 MB desde 0x1000 a la dirección del encabezado (ajustada por CIC) y deja
  las variables de libultra en 0x300 y s3–s7. Común: Status 0x34000000,
  tiempos de PI DOM1, RI_SELECT, registros de RDRAM. Fuentes documentadas.
- ISViewer en 0x13FF0000, opcional (tapa los últimos 64 KB de una ROM de
  64 MB).
- `salsa64-run` (`src/host/run.c`, `tools/build.sh host`): corre una ROM N
  ciclos o hasta un texto del ISViewer, detecta bucles sin salida, traza
  excepciones (`--trace-exc`). Sin asignaciones en el bucle.
- nemu64-test compilado en contenedor reproducible
  (`tools/build-nemu64-test.sh`, mismo SHA-256 desde cero), licencias e IPL3
  verificados, en `tests/roms/homebrew/` con sus avisos.
- `tools/test-roms.sh`: corre nemu64-test y compara con la línea base de
  fallas conocidas (`tests/roms/nemu64-test.fallas`); una regresión lo hace
  fallar (probado rompiendo a propósito el enlace de JAL).
- nemu64-test pasó de no arrancar a 4175 de 4637 pruebas (90 %). Las fallas
  del CPU que encontró están arregladas: MULT con rt de 35 bits, LWR parcial,
  enlace de saltos en delay slot, MTC0 de 64 bits, Index.P, TagLo, PageMask y
  EntryLo en la TLB, Random con Wired > 31, funciones de COP0 reservadas
  como NOP, CFC0/CTC0/BC0, CACHE no alineado, latch de COP2, direccionamiento
  de 64 bits (xkuseg/xksseg/xkphys/xkseg), Status.RE; FPU: índices de
  registro con FR = 0, clasificación de NaN (estaba al revés), redondeo
  dirigido e Inexact, desbordamiento, flush con FS, rango de CVT.L,
  codificaciones inexistentes → FPE. Bus: SB/SH/SD fuera de la RDRAM y
  lecturas del bus PI de 16 bits.
- RSP provisional: quitarle HALT lo detiene en el acto con BROKE, para que
  nadie espere para siempre (Fase 2 lo reemplaza).
- Pruebas: 154 unitarias (antes 139) en host (ASan + UBSan) y en RPCS3
  (big-endian): todas pasan. Se actualizaron las 5 que codificaban
  suposiciones que nemu64-test desmintió.
- Rendimiento del intérprete en el host: 237 → 217 MIPS por la FPU exacta
  (docs/perf-log.md). nemu64-test completo: 0,75 s en el host.

Fallas que quedan en nemu64-test (fuera del intérprete): RSP 294, bus PI
del cartucho 81, cachés 46, SP (DMA y registros) 31, RDP y MI repeat 10.

Siguiente (Fase 1):
- [x] Correr `salsa64-cpu-tests` en la PS3 real: 154/154 pasan, **8,8 MIPS**
      (docs/perf-log.md).
- [x] Perfilar el intérprete en el PPE: 8,8 → 21,2 MIPS en dos rondas
      (entrada de arriba; la sospecha del FPSCR no aplicaba).
- [x] VI con contador de líneas e interrupción; framebuffer 2D en el RSX
      (primera pieza de `lib/rsxkit/`): ver la entrada de arriba.
- [ ] Bus PI: latch de escrituras al cartucho y DMA no alineado (81 pruebas
      de nemu64-test); SRAM/FlashRAM.
- [ ] Cachés del VR4300 (46 pruebas) y ciclos por instrucción (conjuntos
      "timing" y "cycle" de nemu64-test).
- [ ] CPUTest de krom ensamblado desde fuente con bootcode en ceros (traer
      solo los `.asm` por la API: el repo trae el IPL3 de Nintendo). Probaría
      el camino plano del arranque HLE.
- [ ] Probar el camino plano con los juegos del usuario (fuera del repo):
      falta reproducir efectos del IPL3 de algunos CIC (6105).

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

Siguiente (Fase 1): ver la entrada de arriba. Hechos en ella: arranque
HLE, ISViewer, contenedor de nemu64-test, direccionamiento de 64 bits y
bandera Inexact.

## 2026-10-10 — Fase 0: completa en RPCS3, falta la PS3 real

Hecho:
- Imagen `salsa64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
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
- [x] PS3 real (2026-10-10, Slim CECH-3011A, HFW 4.91 + HEN; no admite CFW):
      udplog, spu-compute, rsx-triangle y salsa64-cpu-tests pasan; jit-ppe:
      solo `.text` es escribible y ejecutable (docs/jit-ppe.md). Números en
      docs/perf-log.md. El log también se escribe en /dev_hdd0/salsa64/<prog>.log.
- [x] Plan de la Fase 3: recompilador en el PPE con la caché en una región de
      `.text`. Falta medir el tamaño máximo de esa región (docs/jit-ppe.md).
- [ ] Antes: conectar la PS3 (está desconectada; revisar su estado). Datos a
      reunir: IP por cable, versión de firmware, apps de envío instaladas y si
      hay capturadora. Luego correr la Fase 0 y `salsa64-cpu-tests` (primer
      número de MIPS real).
- [ ] Opcional: correr Cell64 en la PS3 y guardar su `perf_trace.csv`
      (docs/referencia-cell64.md).

Nombre: **Salsa64** (decidido el 2026-10-10). Antes: "Retro64" (2026-10-10) y "Cell64" (provisional).
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
- Archivos de Salsa64 en la PS3: `/dev_hdd0/salsa64/` (RPCS3 vacía `/dev_hdd0/tmp`).
- Juegos objetivo: tabla en docs/proyecto.md. Comparación con Cell64 en
  docs/referencia-cell64.md (no usa SPEs; todo en un hilo del PPE).
- Ideas sin decidir: versión rápida del intérprete (decodificar una vez,
  como el "FASTINT" de Cell64) y, como investigación lejana, emular la CPU en
  un SPE solo si el PPE no alcanza.
