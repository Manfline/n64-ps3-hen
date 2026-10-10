# Código y herramientas de terceros

## Código incluido o adaptado en el repositorio

| Archivo | Origen | Licencia | Notas |
|---|---|---|---|
| `tests/phase0/rsx-triangle/main.c` | Sample `samples/graphics/rsxtest` de PSL1GHT (`f649a08f`) | MIT | Inicialización del RSX, superficie, viewport y flip reescritos en C a partir de `rsxutil.cpp`/`main.cpp` |
| `tests/phase0/spu-compute/spu/mix.c` | Patrón DMA del sample `samples/spu/spudma` de PSL1GHT | MIT | Solo el patrón `mfc_get`/espera por etiqueta; el cálculo y el doble búfer son propios |

Aviso de PSL1GHT: Copyright (c) 2011 PSL1GHT Development Team, licencia MIT
(compatible con GPLv2 o posterior).

## Herramientas usadas para compilar (no se redistribuyen en el repo)

| Componente | Origen | Licencia | Uso |
|---|---|---|---|
| ps3toolchain | github.com/ps3dev/ps3toolchain | BSD-2-Clause (scripts); GCC/binutils GPL; newlib varias | Compiladores PPU/SPU dentro de la imagen Docker |
| PSL1GHT | github.com/ps3dev/PSL1GHT | MIT | SDK homebrew, enlazado con los programas |
| NVIDIA Cg Toolkit 3.1 | developer.nvidia.com/cg-toolkit | Propietaria (NVIDIA), gratuita | `libCg.so` para `cgcomp`, solo en la imagen local; no se commitea ni se publica |
| Port de SM64, rama `ps3` | github.com/fgsfdsfgs/sm64-port | — | Solo para validar `cgcomp`: los shaders se descargan a `build/validate/` y no se copian al repo |
