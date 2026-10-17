# RPCS3

Emulador para las pruebas rápidas en la Mac. **Nunca concluir sobre
rendimiento, sincronización entre SPEs ni JIT con RPCS3.**

## Instalación (2026-10-10)

- RPCS3 v0.0.43-20262-8f2fe1a9, build oficial macOS arm64
  (github.com/RPCS3/rpcs3-binaries-mac-arm64), en `/Applications/RPCS3.app`.
  Firma ad hoc; usa MoltenVK sobre la GPU de la Mac.
- Firmware 4.92 (`PS3UPDAT.PUP`, descargado por el autor; nunca va al repo),
  instalado con `rpcs3 --installfw`.
- Configuración global: `~/Library/Application Support/rpcs3/`
  (`dev_hdd0/` es el disco duro virtual). Logs: `~/Library/Caches/rpcs3/`
  (`RPCS3.log`, `TTY.log`).

## Automatización

- `tools/test-phase0.sh`: recompila y corre las cuatro pruebas de la Fase 0.
- `tools/rpcs3-run.py <programa>`: lanza `rpcs3 --no-gui --config <yml> <elf>`,
  sigue `TTY.log` hasta la línea `RESULTADO` y copia `RPCS3.log` a
  `build/logs/rpcs3-<programa>.rpcs3.log`.
- El `dev_hdd0/salsa64` es uno solo para todas las corridas y guarda los
  archivos de bandera de las anteriores (`lle`, `capturas`, `sin-recompilador`…),
  que cambian lo que se prueba: una corrida «sin RSP a bajo nivel» con un `lle`
  olvidado no lo es. `--quitar NOMBRE` (repetible) los quita durante la corrida
  y los devuelve al terminar; `--bandera NOMBRE[=CONTENIDO]` pone uno.
- `tools/rpcs3/salsa64.yml`: Write Color Buffers, red conectada, salir al
  terminar el proceso. `--config` aplica solo esas claves sobre la config
  global.
- `tools/rpcs3/salsa64-interp.yml`: lo mismo con `PPU Decoder: Interpreter
  (dynamic)`.

## Peculiaridades encontradas

- **Vacía `/dev_hdd0/tmp` en cada arranque.** Los archivos de Salsa64 van en
  `/dev_hdd0/salsa64/` (`src/platform/ps3/paths.h`).
- **No mandar SIGTERM mientras se cierra:** aborta con "Emulation object is
  unavailable (process teardown)". Los programas salen solos en modo
  autotest (`/dev_hdd0/salsa64/autotest`, lo crea el lanzador) y RPCS3 se
  cierra con "Exit RPCS3 when process finishes"; solo se usa SIGKILL si se
  cuelga.
- **Errores fatales en `--no-gui`:** RPCS3 queda detenido y muestra la
  ventana desde un proceso aparte (`rpcs3 --error …`) que sobrevive al
  principal. El lanzador detecta `·F ` en `RPCS3.log`, mata ambos y registra
  el mensaje.
- **Leer el framebuffer desde el PPE** requiere "Write Color Buffers".
- **UDP:** con "Internet enabled: Connected" los sockets son reales y el
  broadcast llega a `tools/udplog.py` en la misma Mac. Con el número de
  secuencia del logger: 0 perdidas y duplicados frecuentes (udplog: 40 de 203;
  rsx-triangle y spu-compute: todas las líneas dos veces). En jit-ppe el
  conteo mezcla los relanzamientos, porque cada uno reinicia la secuencia.
- **Código generado en tiempo de ejecución:** ni el recompilador LLVM ni el
  intérprete dinámico lo ejecutan: "Segfault executing location 0" al saltar
  a memoria de datos (su tabla de funciones solo tiene el código cargado de
  los módulos). Escribir en `.text` da "Access violation writing … (read-only
  memory)". Ver docs/jit-ppe.md.
