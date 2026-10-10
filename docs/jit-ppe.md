# JIT en el PPE (prueba bloqueante para la Fase 3)

Pregunta: ¿puede un programa homebrew escribir instrucciones PowerPC en
memoria y ejecutarlas? Sin eso no hay recompilador MIPS → PPC64 en el PPE y
la Fase 3 se replantea: intérprete optimizado en el PPE y recompilación hacia
los SPEs (su local store ejecuta lo que llegue por DMA).

## Programa

`tests/phase0/jit-ppe` (`build/salsa64-jit-ppe/`). Tiene dos conjuntos de
mecanismos, que se eligen al compilar:

- `PRUEBA=mecanismos`: la primera corrida, dónde se puede ejecutar código
  generado (tabla de abajo).
- `PRUEBA=cache` (por defecto): cuánto se puede reservar para la caché del
  recompilador (ver «Tamaño de la caché»). `CAVE_MB` y `NOBITS_MB` fijan los
  tamaños (32 y 32 por defecto).

```
tools/build.sh -C tests/phase0/jit-ppe pkg                       # cache, 32 + 32 MB
tools/build.sh -C tests/phase0/jit-ppe pkg CAVE_MB=16 NOBITS_MB=16
tools/build.sh -C tests/phase0/jit-ppe pkg PRUEBA=mecanismos
```

Para cada mecanismo:

1. reserva memoria;
2. escribe `mulli r3,r3,3 ; addi r3,r3,7 ; blr`;
3. limpia cachés: `dcbst` por línea de 128 bytes, `sync`, `icbi` por línea,
   `isync`;
4. llama al código con un descriptor ELFv1 propio (entrada, TOC actual) y
   espera `f(5) = 22`;
5. cambia la constante a 9, limpia y vuelve a llamar: espera 24 (si sale 22,
   el PPE ejecutó instrucciones viejas de la caché);
6. mide el costo de limpiar 64 KB (promedio de 100).

| Mecanismo | Memoria |
|---|---|
| `data` | arreglo estático en `.data` (0x1001xxxx) |
| `bss` | arreglo estático en `.bss` |
| `seg0` | arreglo RW en `.data.rel.ro`, segmento de 0x50000: mismos 256 MB que `.text` |
| `heap` | `memalign()` de newlib |
| `sysmem64k` | `sysMemoryAllocate`, páginas de 64 KB |
| `sysmem1m` | `sysMemoryAllocate`, páginas de 1 MB |
| `mmapper` | `sysMMapperAllocateAddress` + `AllocateMemory` + `MapMemory` (RW) |
| `text` | zona de nops dentro de `.text`, escritura directa (se espera que falle) |

`seg0` contra `data` distingue si lv2 aplica el no-ejecutable por segmento
(bit N del SLB) o por página (bit N de la PTE).

Mecanismos de `PRUEBA=cache`:

| Mecanismo | Memoria |
|---|---|
| `text` | inicio de una zona de nops de `CAVE_MB` dentro de `.text` (ocupa archivo) |
| `text_fin` | últimos 64 KB de esa zona |
| `nobits` | inicio de una cola NOBITS de `NOBITS_MB` al final del segmento de texto; antes de escribir comprueba que toda la cola esté en ceros |
| `nobits_fin` | últimos 64 KB de la cola |

La cola NOBITS la pone `tests/phase0/jit-ppe/nobits.ld`, un script con
`INSERT AFTER .sys_proc_prx_param` que se pasa con `-T` **antes** de
`-T lv2.ld`: al recibir un `-T`, gcc deja de agregar `lv2.ld`, e `INSERT` solo
encuentra su destino en un script que venga después. Queda un único segmento
de texto `R E` con memsz > filesz, como el de `.data`/`.bss`.

En `text` y `nobits` también se mide limpiar la zona entera (promedio de 10).
Al arrancar, el programa registra la memoria de usuario (syscall 352,
`sys_memory_get_user_memory_size`, que PSL1GHT no envuelve).

### Caídas

Ejecutar una página no ejecutable mata el proceso. Antes de cada fase
arriesgada el programa agrega `<mecanismo> PROBANDO <fase>` a
`/dev_hdd0/salsa64/jit_state.txt`; al relanzarlo, lo que quedó en PROBANDO
se registra como `CRASH <fase>` y sigue con el siguiente mecanismo. **Hay que
relanzarlo hasta que imprima `RESULTADO jit-ppe`.** Al terminar, el archivo se
renombra a `/dev_hdd0/salsa64/jit_result.txt` y la siguiente ejecución
empieza de cero.

## Cómo correrla en la PS3

1. En la Mac: `tools/udplog.py --out build/logs/jit-ppe.log`. El log queda
   también en `/dev_hdd0/salsa64/jit-ppe.log` (se lee por FTP si no llega por
   UDP).
2. Enviar `build/salsa64-jit-ppe/salsa64-jit-ppe.self` con ps3load o instalar el
   `.pkg` (`tools/build.sh -C tests/phase0/jit-ppe pkg`), ver
   `tools/deploy-ps3.sh`.
3. Relanzar tras cada caída hasta ver `RESULTADO`.
4. Repetir con HEN y, si se puede, con CFW. Anotar firmware y versión de
   HEN/CFW.

RPCS3 no es concluyente (su recompilador no reproduce los permisos de página
de lv2); sirve solo para validar que el programa corre.

## Resultados

| Fecha | Entorno | data | bss | seg0 | heap | sysmem64k | sysmem1m | mmapper | text | flush 64 KB |
|---|---|---|---|---|---|---|---|---|---|---|
| 2026-10-10 | RPCS3 0.0.43, LLVM e intérprete dinámico (**no concluyente**) | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH escritura | — |
| 2026-10-10 | **PS3 Slim CECH-3011A, HFW 4.91 + HEN** | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | CRASH ejec. | **OK**: f(5)=22, reescrita=24 | 12,2 µs |

CFW no se puede probar con la consola disponible: la CECH-3011A tiene firmware
mínimo de fábrica ≥ 3.60 y solo admite HEN.

En RPCS3 todos los mecanismos reservan memoria bien y todos caen al ejecutar,
porque el emulador solo ejecuta código de los módulos cargados (ver
docs/rpcs3.md); eso no dice nada del hardware. Sí valida el programa: la
máquina de estados completa el ciclo tras 8 caídas, y `.text` es de solo
lectura.

Conclusión (PS3 real, 2026-10-10): **el JIT en el PPE es viable con HEN**,
pero solo dentro de `.text`. La zona de nops de `.text` (64 KB, paquete
`.gnpdrm.pkg` hecho con `make_self_npdrm`) se puede escribir, ejecutar,
reescribir y volver a ejecutar; limpiar 64 KB de caché cuesta 12,2 µs. Toda
memoria de datos (estática, heap, `sysMemoryAllocate`, `sys_mmapper`) mata el
proceso al ejecutar, incluso `seg0` en el mismo segmento de 256 MB que
`.text`: el no-ejecutable se aplica por página, no por segmento.

RPCS3 se equivoca en sentido contrario (ahí `.text` es de solo lectura), así
que el JIT solo se puede probar en la PS3.

Consecuencias para la Fase 3:
- La caché del recompilador va en el segmento de texto (ver «Tamaño de la
  caché»).
- CFW no hace falta para el JIT; la consola de pruebas no lo admite de todos
  modos.

## Tamaño de la caché

Pregunta: ¿cuánto se puede reservar en el segmento de texto (se quieren
16–32 MB) sin inflar el EBOOT?

### Tamaño del paquete (medido al compilar)

| Variante | ELF sin firmar | EBOOT.BIN | `.pkg` / `.gnpdrm.pkg` |
|---|---|---|---|
| Zona de nops de 64 KB (`PRUEBA=mecanismos`) | 0,5 MB | 82 352 B | 97 968 B |
| Zona de nops de 16 MB | 16,3 MB | 97 232 B | 112 880 B |
| Zona de nops de 32 MB | 32,3 MB | 113 456 B | 129 072 B |
| Zona de 1 MB + cola NOBITS de 32 MB | 1,3 MB | 83 184 B | 98 800 B |
| Zona de 32 MB + cola NOBITS de 32 MB | 32,3 MB | 113 568 B | 129 200 B |

`make_self_npdrm` comprime cada segmento con zlib: 32 MB de nops ocupan
~31 KB en el EBOOT. La cola NOBITS no ocupa nada (filesz no cambia). Ninguna
de las dos opciones infla el paquete.

### En la PS3

| Fecha | Entorno | text | text_fin | nobits | nobits_fin | flush 64 KB | flush 32 MB |
|---|---|---|---|---|---|---|---|
| 2026-10-10 | **PS3 Slim CECH-3011A, HFW 4.91 + HEN**, `.gnpdrm.pkg`, zona 32 MB + cola 32 MB | **OK** | **OK** | **OK**, ceros | **OK** | 10,5–12,2 µs | 5,4 ms |
| 2026-10-10 | ídem, segundo arranque | **OK** | **OK** | **OK**, ceros | **OK** | 10,4–13,4 µs | 5,4–5,5 ms |
| 2026-10-10 | ídem, `.pkg` normal (sin `package_finalize`) instalado encima | **OK** | **OK** | **OK**, ceros | **OK** | 10,5–12,0 µs | 5,4–5,5 ms |

Las cuatro pasan en cada arranque, sin caídas: f(5)=22 y, tras reescribir,
24. El segmento de texto mide 64 MB en memoria (0x10000–0x4040000; la cola
empieza en 0x2040000) y lv2 lo carga entero con el mismo permiso: la cola
NOBITS llega en ceros y se puede escribir, ejecutar y reescribir hasta su
último byte, igual que la zona de nops.

Memoria de usuario con ese EBOOT: total 214 016 KB (209 MB), disponible
143 680 KB. Faltan 68,7 MB, de los que 64,2 son el segmento de texto: **los
segmentos del ELF se descuentan de la memoria de usuario del juego**, también
la cola NOBITS. Reservar la caché ahí cuesta lo mismo que pedirla con
`sysMemoryAllocate`.

HEN instala también el `.pkg` normal y el resultado es idéntico. Era lo
esperable: los dos paquetes llevan el mismo EBOOT.BIN (NPDRM, de
`make_self_npdrm`) y `package_finalize` solo cambia el contenedor, así que el
tipo de paquete no influye en los permisos. No se probó un SELF sin NPDRM
(`make_self`, `fself`): no hace falta, el `.gnpdrm.pkg` es el formato de
distribución.

Limpiar la caché de 32 MB entera (dcbst + icbi por línea) cuesta 5,4 ms, un
tercio de un cuadro a 60 Hz: aceptable para un reinicio o un cambio de ROM;
en juego hay que invalidar por bloques.

### Decisión

La caché del recompilador va en una **cola NOBITS al final del segmento de
texto**, con el mismo `INSERT` de `nobits.ld`, no en una zona de nops dentro
de `.text`:

- No ocupa espacio en el EBOOT ni hay que descomprimirla al cargar; la zona
  de nops tampoco infla el paquete, pero viaja comprimida y lv2 la
  descomprime en cada arranque.
- No parte `.text`: con 32 MB de nops en medio, el enlazador tuvo que meter
  stubs de salto largo (`.branch_lt`) para las llamadas que cruzan la zona.
- El tamaño se fija al enlazar y no cambia el código.

Tamaño: **16 MB para empezar**. `bl` alcanza ±32 MB; con el código del
emulador (1–2 MB) justo antes de la cola, todo bloque de una caché de 16 MB
puede llamar a los helpers en C con `bl` directo. Con 32 MB, la parte lejana
de la caché necesita llamadas absolutas (`mtctr`/`bctrl`) o trampolines. Si
hace falta más, se sube a 32 MB sabiendo que se descuenta de los 209 MB de
usuario (docs/memory-budget.md).

