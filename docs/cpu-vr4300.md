# Intérprete del VR4300 (Fase 1)

Código: `src/cpu/` (CPU) y `src/core/` (bus, RCP, planificador, arranque).
Escrito desde documentación pública (manual del VR4300 de NEC, MIPS IV ISA,
n64brew.dev, documentación del IPL3 de libdragon) y validado con nemu64-test;
no se copió código de Not64, Mupen64Plus ni ares.

## Estructura

| Archivo | Contenido |
|---|---|
| `src/core/types.h`, `endian.h` | Tipos, extensión de signo, accesores big-endian |
| `src/core/bus.{h,c}` | Mapa físico: RDRAM, SP DMEM/IMEM, registros, ROM, PIF RAM |
| `src/core/rcp.h`, `mi.c`, `pi.c`, `si.c`, `vi.c`, `ai.c`, `sp.c` | Registros del RCP (ver estado abajo) |
| `src/core/pif.{h,c}` | PIF: comandos joybus de los controles en la PIF RAM (ver abajo) |
| `src/core/sched.{h,c}` | Planificador de eventos por ciclos, ranuras fijas |
| `src/core/n64.{h,c}` | La máquina: CPU + bus + planificador |
| `src/core/rom.{h,c}` | Orden de bytes del volcado, encabezado, CIC y tipo de TV |
| `src/core/boot.{h,c}` | Arranque HLE (sin PIF ROM ni IPL3) |
| `src/host/run.c` | `salsa64-run`: ejecutor de línea de comandos para el host |
| `src/cpu/vr4300.h` | Estado del CPU y API pública |
| `src/cpu/vr4300_internal.h` | Decodificación, traducción, accesos a memoria, FPRs |
| `src/cpu/interp.c` | Bucle, enteros, saltos, cargas/almacenamientos |
| `src/cpu/cop0.c` | COP0, excepciones, TLB, Count/Compare, interrupciones |
| `src/cpu/cop1.c` | FPU |
| `src/cpu/idle.c` | Detección de bucles de espera (ver abajo) |

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
  dentro de `struct salsa64_n64` (10,4 KB).
- **TLB al reset.** Entradas inválidas con VPN distintas en KSEG0 (nunca
  coinciden), como hace libultra.

## Arranque HLE

`salsa64_boot_hle` (`src/core/boot.c`) sustituye la PIF ROM (IPL1 e IPL2) y
el IPL3 del cartucho: carga el programa y deja la máquina como la deja el
IPL3 al saltar a él. No se incluye ni se ejecuta código de Nintendo; la
variante de IPL3 de la ROM del usuario se reconoce por el CRC-32 de sus bytes
0x40–0xFFF.

Antes de arrancar, `salsa64_rom_normalize` lleva el volcado a .z64
(big-endian): .v64 (bytes intercambiados de dos en dos, firma `37 80 40 12`)
y .n64 (palabras invertidas, `40 12 37 80`) se convierten una sola vez al
cargar. Cualquier otra firma se toma como .z64, como pide n64brew.dev (ROM
Header), porque las ROMs homebrew pueden traer otros valores en esos bytes.

Hay dos caminos, según el IPL3 que trae la ROM:

| Camino | Cuándo | Qué hace |
|---|---|---|
| ELF | IPL3 no reconocido y hay un ELF alineado a 256 bytes desde 0x1000 (busca en 64 MB) | IPL3 libre de libdragon (lo usan nust64 y libdragon; nemu64-test lo trae). Carga los PT_LOAD (dirección virtual de 32 bits, BSS en cero). ELF de 32 o 64 bits, solo big-endian. Los ELF comprimidos con n64elfcompress todavía dan error. |
| Plano | IPL3 de Nintendo reconocido, o ningún ELF | Copia 1 MB desde 0x1000 a la dirección de arranque del encabezado (u32 en 0x08; −0x100000 con CIC 6103, −0x200000 con 6106, fija en 0x80000480 con 7102). Lo hacen el IPL3 de Nintendo y el "compat" de libdragon. |

Estado común a los dos:

| Qué | Valor | Fuente |
|---|---|---|
| Status | 0x34000000 (CU1, CU0, FR) | IPL1 (n64brew.dev: Initial Program Load); nemu64-test lo verifica (StartupTest) |
| Config | 0x7006E463 (valor de reset) | IPL1 escribe 0x0006E463; nemu64-test lee 0x7006E463 |
| Cause, Compare | 0 | libdragon `boot/ipl3.c` (stage1) |
| Count | 23 437 500 (0,5 s) con un IPL3 de Nintendo reconocido; 0 en el camino ELF y sin IPL3 reconocido | El IPL3 de Nintendo lo pone en 0 al empezar (visto corriendo el del cartucho en el host) y tarda ~500 ms en frío (n64brew.dev: IPL3). `osContInit` espera hasta 0,5 s de `osGetTime()`: con 0, Conker dormía ahí ~29 s (docs/estado.md, 2026-10-13). libdragon `boot/ipl3.c` (stage1) lo pone en 0 |
| FCR31 | 0 (tras encender) | nemu64-test, StartupTest |
| PI_BSD_DOM1 LAT/PWD/PGS/RLS | bytes 3, 2, 1 (bits 0–3) y 1 (bits 4–5) del encabezado | n64brew.dev: ROM Header |
| RI_SELECT | 0x14 (TSEL = 1, RSEL = 4) | n64brew.dev: RDRAM Interface |
| Registros de RDRAM | DeviceType 0xB4190010, reg. 2 = 0x2B3B1A0B, reg. 6 = 0x101C0A04, repetidos cada 0x40 bytes | nemu64-test (rdram-regs, valores medidos en la consola) |
| VI_V_INTR | 0x3FF | IPL1 |
| SP | detenido (SP_STATUS = HALT), PC = 0; DMEM e IMEM en 0 salvo los boot flags | IPL1/IPL3; nemu64-test (StartupTest) |
| MI | sin interrupciones pendientes ni máscara | libdragon `minidragon.h` (`rcp_reset`) |
| RDRAM | en 0 antes de cargar | libdragon (`boot/README.md`) |

Solo camino ELF (libdragon `boot/README.md`, `loader.c`, `cleanup.c`):

- Boot flags en DMEM: [0] tamaño de la RDRAM; [4] entropía (0, para que cada
  corrida sea igual); [8] tipo de ROM (0 = cartucho), [9] tipo de TV (0 PAL,
  1 NTSC, 2 MPAL), [10] reset (0 = en frío), [11] consola (0 = N64);
  [12–13] posición del ELF en la ROM en páginas de 256 bytes (escrito como
  `(0x10000000 + offset) << 8`). nemu64-test lee justo DMEM[0], DMEM[9] y
  DMEM[12].
- $sp = 0x80000000 + tamaño de la RDRAM − 0x10.
- PC = e_entry.

Solo camino plano:

- Variables de libultra en la RDRAM: 0x300 osTvType, 0x304 osRomType (0),
  0x308 osRomBase (0xB0000000), 0x30C osResetType (0), 0x314 osVersion (1),
  0x318 osMemSize. Son las que escribe el IPL3 "compat" de libdragon, que
  reproduce el comportamiento histórico (libdragon `boot/ipl3.c` y README).
- s3 = tipo de ROM (0), s4 = TV, s5 = reset (0), s6 = semilla del IPL3 del
  CIC (0x3F en 6101/6102/7101/7102, 0x78 en 6103, 0x91 en 6105, 0x85 en
  6106; n64brew.dev: PIF-NUS), s7 = versión (1, medido en ares con 6102,
  6103, 6105 y 6106). Son los valores que el IPL2 pasa al IPL3 (libdragon
  `boot/ipl3.c` los nombra así). Con el 6105, ares deja s6 = 0xA0000200 al
  entrar al juego (no reproducido).
- $sp = 0xA4001FF0 (lo deja el IPL1).

Solo CIC 6103 (lo que su IPL3 deja al saltar al juego y Banjo-Kazooie
comprueba; fuentes en docs/THIRD_PARTY.md):

| Qué | Efecto | Quién lo comprueba |
|---|---|---|
| Copia de su propia imagen | DMEM 0x4C0–0x764 → RDRAM 0x000–0x2A4 (de la ROM del usuario, como con el 6105). Los vectores de excepción que pone después el juego pisan parte | Banjo-Kazooie, antes de cada proyección: si 0x1D8 no tiene la palabra que viene de la ROM 0x698, cambia los planos cercano y lejano de 30 y 4000 por 750 y 1250 (faltan personajes, el cielo y el fondo; la profundidad tiembla) |
| DMEM e IMEM | Todo en 0xFF salvo 6103 en IMEM 0x000 (con el 6106: 0x17D8, y `osCicId` = 0x17D8) | Banjo-Kazooie al iniciar: solo con DMEM[0] = 0xFFFFFFFF e IMEM[0] = 6103 trae de la ROM (0xB70) el arranque del RSP; sin él, el microcódigo real no arranca |

Solo CIC 6105/7105 (lo que su IPL3 deja además y los juegos comprueban;
fuentes en docs/THIRD_PARTY.md):

| Qué | Efecto | Quién lo comprueba |
|---|---|---|
| Copia de su propia imagen | DMEM 0x554–0x888 → RDRAM 0x004–0x338. En DMEM están los bytes 0x40–0xFFF de la ROM en las mismas posiciones, así que se copian de la ROM del usuario | Perfect Dark (`b .` si 0x2E8 ≠ 0xC86E2000, ROM 0x838) y Zelda OoT (un actor revisa la misma palabra) |
| Microcódigo del RSP | Lee 0x1F0 bytes de RDRAM 0x1E8 a IMEM 0x120 y los escribe con un DMA con paso: 24 filas de 8 bytes desde RDRAM 0x2FB1F0, una cada 0xFF0 (SP_WR_LEN = 0xFE817000). Se reproduce el efecto, no el microcódigo | Donkey Kong 64 (fila 3, 0x2FE1C0) |
| Tamaño de la RDRAM | En 0x3F0, además de osMemSize en 0x318 | homebrew que arranca con IPL3 6105 (n64bootloader) |
| `osCicId` (0x310) | 6105 (también con el 6103: 6103) | Conker (sin él no arranca su hilo principal), Perfect Dark, DK64; con 6103, Banjo-Kazooie y Diddy Kong Racing |

Las variables de libultra (0x300–0x318, con `osCicId`) se escriben después
y pisan la copia; de 0x31C a 0x337 queda lo copiado. `osCicId` se escribe
solo con el 6103 y el 6105, las dos variantes de las que hay juegos que lo
comprueban; con las demás queda en 0. Con el 6105 no se reproduce lo que queda en
IMEM 0x000–0x11F (el microcódigo, descifrado del residuo del IPL2 que deja la
PIF ROM): es código de Nintendo. Tampoco el resto de los registros que deja
cada IPL3 (los cálculos del checksum): no hay documentación pública ni juego
conocido que los lea. En las demás variantes no hay efectos documentados en
la RDRAM más allá de la dirección de arranque. `salsa64_boot_hle_as` fuerza
la variante (pruebas: las ROMs sintéticas no traen un IPL3 de Nintendo).

El tipo de TV sale del código de destino del encabezado (byte 0x3E: D, F, H,
I, L, P, S, U, W, X, Y, Z → PAL; B → MPAL; el resto NTSC) y se puede forzar
(`salsa64-run --tv`).

## ISViewer

Consola de depuración de los kits de desarrollo, en 0x13FF0000–0x13FFFFFF
(los últimos 64 KB del espacio de una ROM de 64 MB, así que está apagado por
omisión: `salsa64_bus_set_isviewer`). Las ROMs escriben el texto desde +0x20
y su longitud en +0x14; esa escritura llama al callback. El búfer se puede
leer (nemu64-test lo detecta escribiendo y leyendo 0x12345678) y respeta las
escrituras de 8/16 bits, a diferencia del bus PI real. Fuente: README de
nemu64-test y `src/isviewer.rs`.

## Bucles de espera

`src/cpu/idle.c`. Los programas esperan al VI en bucles cortos: el `b .` del
hilo ocioso de libultra o un sondeo de una variable que cambia el manejador
de interrupciones (vitest: `lw; beq; nop`). Interpretarlos es casi todo el
campo en escenas ligeras.

- **Cuándo se mira.** Solo los saltos tomados hacia atrás con el destino a
  menos de 8 instrucciones (`idle_hint` en `interp.c`: una resta y una
  comparación). Antes de analizar se consulta una tabla de 64 entradas con
  `pc ^ instrucción` de los saltos ya descartados.
- **Criterio exacto.** Se simula una vuelta (delay slot, cuerpo, salto) en
  una copia de los registros. Es bucle de espera si solo hay enteros sin
  efectos y cargas de la RDRAM por KSEG0/KSEG1, el salto se vuelve a tomar y
  los 32 registros terminan iguales: la vuelta es un punto fijo y sin un
  evento del planificador nada cambia. No mira el código como patrón: un
  contador, un almacenamiento, un sondeo de E/S (VI_V_CURRENT, SI_STATUS) o
  COP0 (Count) no pasan.
- **Salto exacto.** Se suman vueltas enteras hasta el próximo evento: el
  salto vuelve a ejecutarse, a lo sumo, en el ciclo del evento. La
  interrupción cae en el mismo ciclo y en la misma instrucción; Count, EPC y
  Random quedan idénticos. `tests/unit/test_idle.c` corre cada escenario
  con y sin detección y compara el estado; `salsa64-run --no-idle` permite
  comparar ROMs completas (vitest y nemu64-test dan registros e imagen
  idénticos).
- **Descartes.** Lo que el código no permite (almacena, E/S, salto en el
  cuerpo) se descarta al primer intento; lo que falla el punto fijo, al
  segundo (el primero puede fallar porque el delay slot todavía no escribió
  su valor). Apagar y prender la detección (`salsa64_cpu_set_idle`) vacía
  las tablas.
- **Condición para el resto.** Quien escriba la RDRAM fuera del CPU (DMA con
  latencia, el RSP en un SPE en la Fase 2) debe hacerlo en un evento del
  planificador; si no, el salto se pasaría de largo esa escritura. La HLE
  de audio en el SPE 1 escribe la RDRAM mientras corre la tarea, pero el
  juego solo la mira después de la interrupción del SP, que llega en el
  evento de fin de tarea: ahí se espera al SPE y se avisa lo escrito con
  `salsa64_bus_rdram_written` (como en la consola, donde el RSP también
  escribe durante la tarea). El AI solo lee la RDRAM, al empezar cada DMA,
  que es otro evento.
- **Medición.** `cpu->idle_cycles` cuenta los ciclos saltados; el frontend y
  `salsa64-run` reportan MIPS sin ellos y el porcentaje saltado.
- **Falta.** Bucles en código mapeado por TLB (GoldenEye), sondeos de E/S
  cuyo valor solo cambia en eventos (MI_INTR, SP_STATUS) y esperas por Count
  (`wait_ms` de libdragon): calcular cuándo se cumple la condición.

## PIF y controles

`src/core/pif.c`, desde n64brew.dev (PIF-NUS, Joybus Protocol):

- Una escritura en la PIF RAM (DMA del SI o directa) con el bit 0x01 del
  byte 63 prepara los intercambios: recorre la RAM (0x00 salta canal, 0xFD
  reset de la línea, 0xFE fin, 0xFF nada; si no, `TX RX datos respuesta`)
  y apaga el bit. Cada lectura de la PIF RAM por DMA los ejecuta antes de
  copiar; repetir la lectura los vuelve a ejecutar.
- Canales 0–3: control estándar. Identificación (0x00/0xFF) `05 00 02` (sin
  accesorio); estado (0x01) botones, X, Y; L + R + Start da RST con Start
  apagado y el stick en 0. Lectura del accesorio (0x02): 32 ceros y CRC
  0xFF; escritura (0x03): CRC-8 (0x85) de los datos complementado. Un
  puerto sin control o un comando desconocido: bit 0x80 en RX; una
  respuesta más corta que RX: bit 0x40.
- Canal 4 (EEPROM del cartucho): todavía sin dispositivo.
- Por omisión hay un control en el puerto 1 (como una consola encendida);
  el frontend los conecta según los DualShock 3 (`src/frontend/padmap.c`).
- Falta: EEPROM, Controller Pak, Rumble Pak, el reto del CIC (bit 0x02) y
  la latencia del DMA del SI.

## Particularidades del VR4300 que se modelan

Lo marcado con (n64t) lo confirmó nemu64-test; varias reglas de la primera
versión resultaron al revés y se corrigieron con la suite.

CPU:
- Operaciones de 32 bits: usan los 32 bits bajos y extienden el signo.
- SRA/SRAV desplazan el registro de 64 bits y extienden los 32 bajos.
- MULT toma rs con sus 64 bits y rt como entero de 35 bits con signo; el
  producto (64 bits) se reparte en LO/HI extendiendo el signo de cada mitad
  (n64t). Con entradas que son extensión de signo de 32 bits es lo usual.
- DIV/DDIV entre cero: LO = -1 (dividendo ≥ 0) o 1, HI = dividendo; sin
  excepción. INT_MIN / -1 da INT_MIN y resto 0.
- LWL, y LWR con la palabra completa, extienden el signo del resultado de 32
  bits; LWR parcial deja los 32 bits altos de rt como estaban (n64t).
- El enlace de JAL, JALR, BLTZAL/BGEZAL(L) es la instrucción que sigue al
  delay slot (`next_pc`): si el salto está en el delay slot de otro, es el
  destino de ese + 4 (n64t).
- Operaciones de 64 bits en modo usuario/supervisor sin UX/SX → RI.
- Opcodes, funciones SPECIAL/REGIMM y valores de rs de COP0 reservados → RI;
  COP3 → RI sin índice de coprocesador (n64t).
- CACHE no hace nada (no se emulan las cachés), pero una dirección alineada a
  2 y no a 4 da AdEL (n64t).
- Direccionamiento de 64 bits con KX/SX/UX: xkuseg/xksseg/xkseg de 40 bits,
  xkphys con los bits 58–32 en 0 (físico = bits 31–0), ckseg*; el PC es de 64
  bits (n64t, tlb64). Sin KX/SX/UX, una dirección que no es extensión de signo
  de 32 da error de dirección.
- Status.RE en modo usuario (little-endian): la dirección física se invierte
  con XOR 7, 6, 4 o 0 según el tamaño, también al buscar instrucciones; los
  accesos parciales (LWL…SDR) toman el desplazamiento de la dirección ya
  invertida y BadVAddr conserva la original (n64t, endian_re).

COP0:
- MTC0 transfiere los 64 bits del GPR, igual que DMTC0 (Context, XContext,
  EPC y el latch; n64t).
- Registros 7, 21–25 y 31 leen el último valor escrito a COP0 (64 bits).
- Index: el bit P (31) también se escribe con MTC0 (n64t). TagLo guarda los
  32 bits tal cual; TagHi siempre lee 0 (n64t).
- Con CO = 1, las funciones que no son TLBR/TLBWI/TLBWR/TLBP/ERET no hacen
  nada, salvo 0x10 (el RFE del R3000), que da RI. CFC0, CTC0 y BC0 existen
  pero no hacen nada (n64t). nemu64-test usa esto para sus extensiones de
  emulador ("emux"), que en la consola son NOP.
- TLB: compara región (bits 63–62) y VPN2 (39–13) además del ASID. Guarda la
  máscara de página por pares de bits según el bit alto de cada par (01 → 00,
  10 → 11) y de EntryLo solo PFN de 20 bits, C, D y V (n64t).
- Random (6 bits) baja de 31 hasta Wired y vuelve a 31; con Wired > 31 pasa
  por 0 y sigue desde 63 hasta Wired (n64t).
- COP2 no existe, pero con CU2 = 1 MFC2/DMFC2/CFC2/MTC2/DMTC2/CTC2 usan un
  latch de 64 bits; DCFC2, DCTC2 y el resto dan RI con Cause.CE = 2 (n64t).

FPU:
- Registros en las instrucciones de cálculo: fs ignora el bit bajo con FR = 0,
  ft no; el resultado va al registro físico fd completo y los resultados S y
  W dejan los 32 bits altos en 0. MOV.S copia los 64 bits. MFC1, MTC1, LWC1 y
  SWC1 con FR = 0 e índice impar usan la mitad alta del par (n64t).
- NaN de entrada con el bit alto de la mantisa en 1 (qNaN de IEEE 754-2008) →
  Invalid (V) y NaN por omisión 0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF; con ese bit
  en 0 (sNaN de IEEE, incluido el NaN por omisión) → Unimplemented (E).
  Entrada desnormal → E. Si una entrada da E, la causa es solo E (n64t). En
  las comparaciones, un NaN con el bit alto en 1 siempre da V; uno con el bit
  en 0, solo en las comparaciones con señal.
- Redondeo: los cuatro modos de FCR31 en todas las operaciones, con la
  bandera Inexact (n64t). El host calcula al más cercano en double y el signo
  del error exacto sale de TwoSum o de un fma (ver la cabecera de cop1.c).
- Resultado diminuto: E, salvo con FS = 1 y U e I deshabilitadas; entonces se
  aplana con U e I a ±0, o al ±mínimo normal si el modo redondea hacia
  afuera (n64t). Desbordamiento: O e I, con ±inf o ±máximo según el modo.
- Conversión a entero de NaN, infinito o fuera de rango → E; a 64 bits el
  rango es |x| < 2^53 (n64t). CVT.S/D.L con |x| ≥ 2^55 → E.
- Instrucciones de COP1 que no existen (DCFC1, DCTC1, formatos reservados,
  BC1 con rt ≥ 4) → excepción de punto flotante con E, no RI (n64t).

Bus (n64brew.dev: Memory map; n64t):
- Fuera de la RDRAM el tamaño del acceso se ignora. SB/SH a registros del
  RCP, SP DMEM/IMEM, PIF RAM o el bus PI escriben la palabra completa con el
  registro desplazado a su carril; SD escribe solo la palabra alta.
- El bus PI es de 16 bits: una lectura de 8/16 bits en una dirección que no
  es múltiplo de 4 trae la palabra que empieza en la dirección par
  (LB de ROM+2 devuelve el byte 4).

RCP (provisional):
- El RSP no ejecuta (Fase 2): quitarle HALT lo detiene en el acto con BROKE
  y, si está habilitada, la interrupción de BREAK. Así nadie espera para
  siempre a que termine una tarea.

## Pendiente

Del CPU, nemu64-test ya no marca fallas (con el conjunto "base"). Falta:

- Cachés de instrucciones y datos (46 pruebas de nemu64-test): hoy no hay
  caché y CACHE solo verifica la alineación. Hace falta para el código que
  se modifica a sí mismo sin invalidar la caché.
- Ciclos reales por instrucción (MULT, DIV, fallas de caché): conjuntos
  "timing" y "cycle" de nemu64-test, no compilados todavía.
- Watchpoints y errores de bus.
- ELF comprimidos (n64elfcompress) en el arranque HLE.

Fuera del CPU, lo que nemu64-test marca (línea base en
`tests/roms/nemu64-test.fallas`):

| Área | Pruebas con fallas | Cuándo |
|---|---|---|
| RSP (instrucciones, vectores, registros desde el RSP) | 294 | Fase 2 (SPE 0) |
| Bus PI: latch de escrituras al cartucho, DMA no alineado | 81 | Fase 1, junto con SRAM/FlashRAM |
| Cachés del VR4300 | 46 | ver arriba |
| DMA del SP y registros del SP | 31 | Fase 2 |
| RDP y modo "repeat" del MI | 10 | Fase 2 |

## Pruebas

- `tools/build.sh test-host`: 182 pruebas (`tests/unit/`) con gcc nativo,
  ASan y UBSan. `build/host/salsa64-unit bench` agrega el micro-benchmark.
- `tools/test-roms.sh`: compila `salsa64-run` y corre nemu64-test hasta su
  resumen (≈190 M ciclos, unos 2 s en el host con ASan). Compara la lista de
  pruebas que fallan con `tests/roms/nemu64-test.fallas`: una falla nueva es
  regresión; una que desaparece pide `tools/test-roms.sh --actualizar`. Se
  comprobó que detecta regresiones (romper el enlace de JAL hace fallar dos
  pruebas).
- `salsa64-run` también sirve a mano: `--trace-exc N` muestra las primeras N
  excepciones del CPU, `--regs` vuelca los registros al terminar.
- `tools/rpcs3-run.py cpu-tests`: las mismas pruebas compiladas con ppu-gcc
  (big-endian) en RPCS3, más el micro-benchmark. En la PS3 real se corre
  igual (`tests/phase1/cpu-tests`).
- Se verificó que las pruebas detectan errores: romper SRA, la anulación de
  branch-likely o el EPC en delay slot hace fallar 7 pruebas.
