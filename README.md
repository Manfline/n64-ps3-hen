# Salsa64

Emulador de Nintendo 64 para PlayStation 3 (homebrew) diseñado alrededor del
Cell: el PPE ejecuta el VR4300, los SPEs hacen el trabajo vectorial (RSP,
audio, posprocesado) y el RSX dibuja. Licencia GPLv2 o posterior, salvo la
capa gráfica genérica `lib/rsxkit/`, que es MIT (ver `lib/rsxkit/LICENSE`).

Estado: ya juega en una PS3 real Zelda: Ocarina of Time, Super Mario 64,
Mario Kart 64, GoldenEye 007 y Perfect Dark, con ajustes pendientes por juego.
Ver `docs/estado.md` para el detalle y las mediciones.

## Usar en la PS3

Se instala el `.pkg` desde el Package Manager y se ponen las ROMs propias
(`.z64`, `.v64` o `.n64`) en `/dev_hdd0/salsa64` (por FTP de webMAN MOD) o en
una carpeta `salsa64` en la raíz de una memoria USB en FAT32. El menú las
lista, × abre la elegida y START + SELECT vuelve al menú. Detalles, controles,
partidas guardadas y archivos de configuración en `docs/uso-ps3.md`.

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

Aviso legal: no se incluyen ni se aceptan ROMs comerciales, BIOS, PIF ROMs ni material de
Nintendo o Sony. Cada usuario usa sus propios respaldos. Las pruebas
automatizadas usan solo homebrew de libre distribución.
