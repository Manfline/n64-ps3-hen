# Cell64 (nombre provisional)

Emulador de Nintendo 64 para PlayStation 3 (homebrew) diseñado alrededor del
Cell: el PPE ejecuta el VR4300, los SPEs hacen el trabajo vectorial (RSP,
audio, posprocesado) y el RSX dibuja. Licencia GPLv2 o posterior.

Estado: Fase 0 (entorno). Ver `docs/estado.md`.

## Compilar

Requiere Docker (en las Macs: contexto `colima-dev`).

```sh
tools/docker-image.sh        # una vez: imagen cell64-toolchain:dev (~10 min)
tools/build.sh               # compila los programas de tests/phase0
tools/validate-toolchain.sh  # samples de PSL1GHT + shaders de referencia
```

Salidas en `build/<programa>/`: `.elf` (RPCS3, ps3load), `.self`,
`.fake.self` y, con `tools/build.sh -C <dir> pkg`, `.pkg`.

## Probar en la PS3

```sh
tools/udplog.py                                   # log UDP en la Mac
PS3_IP=x.x.x.x tools/deploy-ps3.sh ps3load build/cell64-jit-ppe/cell64-jit-ppe.self
```

No se incluyen ni se aceptan ROMs comerciales, BIOS, PIF ROMs ni material de
Nintendo o Sony. Las pruebas automatizadas usan solo homebrew de libre
distribución.
