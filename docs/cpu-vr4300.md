# Intérprete del VR4300 (Fase 1)

Código: `src/cpu/` (CPU) y `src/core/` (bus, RCP, planificador). Escrito
desde documentación pública (manual del VR4300 de NEC, MIPS IV ISA,
n64brew.dev); no se copió código de Not64, Mupen64Plus ni ares.

## Estructura

| Archivo | Contenido |
|---|---|
| `src/core/types.h`, `endian.h` | Tipos, extensión de signo, accesores big-endian |
| `src/core/bus.{h,c}` | Mapa físico: RDRAM, SP DMEM/IMEM, registros, ROM, PIF RAM |
| `src/core/rcp.h`, `mi.c`, `pi.c`, `si.c`, `vi.c`, `ai.c`, `sp.c` | Registros del RCP (ver estado abajo) |
| `src/core/sched.{h,c}` | Planificador de eventos por ciclos, ranuras fijas |
| `src/core/n64.{h,c}` | La máquina: CPU + bus + planificador |
| `src/cpu/vr4300.h` | Estado del CPU y API pública |
| `src/cpu/vr4300_internal.h` | Decodificación, traducción, accesos a memoria, FPRs |
| `src/cpu/interp.c` | Bucle, enteros, saltos, cargas/almacenamientos |
| `src/cpu/cop0.c` | COP0, excepciones, TLB, Count/Compare, interrupciones |
| `src/cpu/cop1.c` | FPU |

## Decisiones

- **Orden de bytes.** Toda memoria de la N64 se guarda en big-endian. Los
  accesores de `core/endian.h` son `memcpy` + `__builtin_bswap*` en el host
  little-endian y cargas directas en la PS3. Nunca se byte-swapea un bloque.
- **Camino rápido.** KSEG0/KSEG1 en modo kernel se traducen restando la
  base; si la dirección física cae en la RDRAM se accede al búfer directo.
  Todo lo demás (TLB, modos, MMIO) va por funciones aparte.
- **Delay slots.** `pc`/`next_pc` más dos banderas: `branch` (la
  instrucción actual saltó) y `delay_slot` (la actual está en un delay
  slot). Las excepciones usan `cur_pc` y `delay_slot` para EPC y Cause.BD.
  Un branch-likely no tomado avanza `pc` dos instrucciones.
- **Excepciones a mitad de instrucción.** Cada acceso devuelve `false` si
  lanzó una excepción; la instrucción no escribe nada en ese caso.
- **Tiempo.** `cycles` en PClock (93,75 MHz); 1 ciclo por instrucción por
  ahora. Count = `cycles/2 + count_offset`, sin contarlo instrucción por
  instrucción: Compare es un evento del planificador. Random también se
  calcula al leerlo.
- **Interrupciones.** `irq_pending` se recalcula solo cuando cambian Status,
  Cause o el MI; el bucle mira una bandera por instrucción.
- **Sin asignaciones.** La RDRAM la pone quien llama; todo lo demás va
  dentro de `struct retro64_n64` (10,4 KB).
- **TLB al reset.** Entradas inválidas con VPN distintas en KSEG0 (nunca
  coinciden), como hace libultra.

## Particularidades del VR4300 que se modelan

- Operaciones de 32 bits: usan los 32 bits bajos y extienden el signo.
- SRA/SRAV desplazan el registro de 64 bits y extienden los 32 bajos.
- DIV/DDIV entre cero: LO = -1 (dividendo ≥ 0) o 1, HI = dividendo; sin
  excepción. INT_MIN / -1 da INT_MIN y resto 0.
- LWL/LWR extienden el signo del resultado de 32 bits.
- Operaciones de 64 bits en modo usuario/supervisor sin UX/SX → RI.
- Registros COP0 7, 21–25 y 31 leen el último valor escrito a COP0.
- FPU: entradas desnormales o sNaN → Unimplemented (E); qNaN → Invalid (V);
  resultado diminuto → E, o se aplana a ±0 con FCR31.FS; NaN por omisión
  0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF; conversión a entero de NaN, infinito o
  fuera de rango → E; CVT.S/D.L con |x| ≥ 2^55 → E.
- Modo FR = 0: 32 bits sobre registro impar = mitad alta del par; 64 bits
  ignoran el bit bajo.

## Pendiente / sin validar

Las reglas de la FPU (NaN de entrada, rango de .L, subdesbordamiento) salen
de la documentación y deben confirmarse con nemu64-test. Faltan:

- Direccionamiento de 64 bits (KX/SX/UX: xkuseg, xkphys, XTLB completo).
- Bandera Inexact en aritmética y modos de redondeo distintos del más
  cercano en ADD/SUB/MUL/DIV/SQRT (las conversiones sí los respetan).
- Ciclos reales por instrucción (MULT, DIV, fallas de caché) y caché.
- Watchpoints, errores de bus, la instrucción CACHE (hoy es NOP).
- Escrituras de 8/16 bits a registros del RCP: hoy llegan con los otros
  bytes en cero; en la consola el comportamiento es distinto.

## Pruebas

- `tools/build.sh test-host`: 139 pruebas (`tests/unit/`) con gcc nativo,
  ASan y UBSan. `build/host/retro64-unit bench` agrega el micro-benchmark.
- `tools/rpcs3-run.py cpu-tests`: las mismas pruebas compiladas con ppu-gcc
  (big-endian) en RPCS3, más el micro-benchmark. En la PS3 real se corre
  igual (`tests/phase1/cpu-tests`).
- Se verificó que las pruebas detectan errores: romper SRA, la anulación de
  branch-likely o el EPC en delay slot hace fallar 7 pruebas.
