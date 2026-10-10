# Salsa64

Emulador de Nintendo 64 para PlayStation 3 (homebrew) diseñado alrededor del
Cell: el PPE ejecuta el VR4300, los SPEs hacen el trabajo vectorial (RSP,
audio, posprocesado) y el RSX dibuja. Licencia GPLv2 o posterior, salvo la
capa gráfica genérica `lib/rsxkit/`, que es MIT (ver `lib/rsxkit/LICENSE`).

Estado: Fase 1 (intérprete del VR4300). Ver `docs/estado.md`.

## Compilar

Requiere Docker (en las Macs: contexto `colima-dev`).

```sh
tools/docker-image.sh        # una vez: imagen salsa64-toolchain:dev (~10 min)
tools/build.sh               # compila los programas de tests/phase0 y phase1
tools/build.sh test-host     # pruebas unitarias del núcleo con gcc nativo
tools/test-phase1.sh         # las mismas pruebas en RPCS3 (big-endian)
tools/build.sh host          # salsa64-run: corre una ROM en el host
tools/test-roms.sh           # nemu64-test con salsa64-run, contra la línea base
tools/build-nemu64-test.sh   # recompila tests/roms/homebrew/nemu64-test.z64
tools/validate-toolchain.sh  # samples de PSL1GHT + shaders de referencia
```

Salidas en `build/<programa>/`: `.elf` (RPCS3, ps3load), `.self`,
`.fake.self` y, con `tools/build.sh -C <dir> pkg`, `.pkg`.

## Probar en la PS3

```sh
tools/udplog.py                                   # log UDP en la Mac
PS3_IP=x.x.x.x tools/deploy-ps3.sh ps3load build/salsa64-jit-ppe/salsa64-jit-ppe.self
```

No se incluyen ni se aceptan ROMs comerciales, BIOS, PIF ROMs ni material de
Nintendo o Sony. Las pruebas automatizadas usan solo homebrew de libre
distribución.
