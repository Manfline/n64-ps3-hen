# JIT en el PPE (prueba bloqueante para la Fase 3)

Pregunta: ¿puede un programa homebrew escribir instrucciones PowerPC en
memoria y ejecutarlas? Sin eso no hay recompilador MIPS → PPC64 en el PPE y
la Fase 3 se replantea: intérprete optimizado en el PPE y recompilación hacia
los SPEs (su local store ejecuta lo que llegue por DMA).

## Programa

`tests/phase0/jit-ppe` (`build/retro64-jit-ppe/`). Para cada mecanismo:

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

### Caídas

Ejecutar una página no ejecutable mata el proceso. Antes de cada fase
arriesgada el programa agrega `<mecanismo> PROBANDO <fase>` a
`/dev_hdd0/retro64/jit_state.txt`; al relanzarlo, lo que quedó en PROBANDO
se registra como `CRASH <fase>` y sigue con el siguiente mecanismo. **Hay que
relanzarlo hasta que imprima `RESULTADO jit-ppe`.** Al terminar, el archivo se
renombra a `/dev_hdd0/retro64/jit_result.txt` y la siguiente ejecución
empieza de cero.

## Cómo correrla en la PS3

1. En la Mac: `tools/udplog.py --out build/logs/jit-ppe.log`.
2. Enviar `build/retro64-jit-ppe/retro64-jit-ppe.self` con ps3load o instalar el
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
- La caché del recompilador va en una región reservada dentro de `.text`.
- Pendiente medir: tamaño máximo de esa región (16–32 MB de nops inflan el
  EBOOT; probar si un segmento con memsz > filesz, rellenado con ceros por el
  cargador, también es escribible y ejecutable) y si el permiso depende de
  cómo se firma el SELF (`.pkg` contra `.gnpdrm.pkg`, `fself`).
- CFW no hace falta para el JIT; la consola de pruebas no lo admite de todos
  modos.
