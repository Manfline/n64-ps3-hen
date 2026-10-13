# Salsa64

A Nintendo 64 emulator for the PlayStation 3 (homebrew, runs on HEN or CFW),
designed around the Cell processor: the PPE runs the VR4300 CPU with a
dynamic recompiler, the SPEs do the vector work (RSP graphics and audio
HLE), and the RSX draws.

Why another one: RetroArch on the PS3 has no working N64 core (the existing
cores need OpenGL, which the PS3 does not offer), and no N64 emulator for the
PS3 uses the SPEs. Both machines are big-endian, so emulated RAM needs no
byte swapping, and the N64's RSP (MIPS + vector unit + local memory with DMA)
maps naturally onto an SPE.

> Most project documentation (`docs/`) is written in Spanish. This README and
> [CONTRIBUTING.md](CONTRIBUTING.md) are in English.

## Status

Work in progress, but already playable on a real PS3:

- **Playable on real hardware (v0.2.2):** Zelda: Ocarina of Time and
  Majora's Mask, Super Mario 64, Mario Kart 64, Donkey Kong 64, Paper Mario
  and Mario Party at full speed; Perfect Dark, GoldenEye 007, Conker, Star
  Fox 64 and Banjo-Kazooie playable (GoldenEye 45–60 fps in the Dam). Note:
  Conker shows a black screen for 25–30 s at boot before the logos (just
  wait); Super Smash Bros. freezes on the first KO. Full list (Spanish):
  [docs/compatibilidad.md](docs/compatibilidad.md); release notes:
  [CHANGELOG.md](CHANGELOG.md).
- **CPU:** VR4300 interpreter and a MIPS → PPC64 dynamic recompiler on the
  PPE (with TLB-mapped code, FPU and idle-loop detection).
- **RSP:** high-level emulation of graphics microcodes (Fast3D, F3DEX 0.95/1.x,
  F3DEX2 and F3DZEX2, Rare's variants for Perfect Dark and Conker, S2DEX
  backgrounds, the JPEG task of Zelda) on SPE 0, and of audio microcodes (ABI1,
  nead, naudio with MP3) on SPE 1. An experimental low-level RSP interpreter
  (real microcode on an SPE) exists for microcodes without HLE.
- **RDP on the RSX:** combiners translated to RSX fragment programs generated
  at run time (there is no Cg compiler on the console).
- **Frontend:** XMB-style ROM menu, DualShock 3 → N64 controller mapping,
  EEPROM, SRAM and FlashRAM saves.
- **Video:** 720p or 1080p output with internal render scale 1× to 5×;
  framebuffer effects (render to texture, copies back to N64 memory, motion
  blur read from the RSX); save states, FPS counter, PS button pause.
- **Not yet:** large SRAM saves (768 Kbit, 1 Mbit), Controller/Rumble/Transfer
  Pak and post-processing filters (CRT, scaling).

Performance numbers are logged in [docs/perf-log.md](docs/perf-log.md). Only
measurements on a real PS3 count; RPCS3 is used for functional tests.

## Using it on a PS3

Full guide (in Spanish): [docs/uso-ps3.md](docs/uso-ps3.md).

1. Install `salsa64.pkg` with the XMB Package Manager (HEN or CFW). It shows
   up as "Salsa64" under Games.
2. Copy your own ROM dumps (`.z64`, `.v64` or `.n64`, any byte order, up to
   64 MB) to `/dev_hdd0/salsa64` (for example over webMAN MOD's FTP) or to a
   `salsa64` folder at the root of a FAT32 USB stick.
3. Start Salsa64 and pick a game from the menu.

| N64 | DualShock 3 |
|---|---|
| Analog stick | Left stick |
| C buttons | Right stick |
| D-pad | D-pad |
| A / B | × / □ |
| Z | L2 |
| L / R | L1 / R1 |
| Start | START |

- **START + SELECT** pauses and asks before closing the game: × closes it
  (saving first) and returns to the menu, ○ keeps playing. The question can
  be turned off in Options («Confirmar al salir»).
- The **PS button** pauses emulation and sound while the system menu is open;
  the game resumes where it was when the menu closes.
- Saves go to `/dev_hdd0/salsa64/saves`, one file per game.
- Optional text files in `/dev_hdd0/salsa64` change the button mapping
  (`controles.cfg`), the save type per game (`guardado.cfg`), audio
  (`audio.cfg`) and overscan cropping (`overscan.cfg`); see the guide.

## Building

Everything builds inside a Docker image with
[ps3toolchain](https://github.com/ps3dev/ps3toolchain) and
[PSL1GHT](https://github.com/ps3dev/PSL1GHT); nothing from Sony's official SDK
is used. Requirements:

- Docker (or a compatible engine) with several GB free. On arm64 hosts
  (Apple Silicon) the engine must also be able to run `linux/amd64`
  containers (Docker Desktop does; with colima, enable Rosetta or binfmt),
  because NVIDIA's Cg Toolkit, used by `cgcomp`, only exists for x86_64.
- Python 3 for the helper scripts on the host.

```sh
tools/docker-image.sh                  # once: builds salsa64-toolchain:dev (GCC for PPU/SPU; 15–60 min)
tools/build.sh                         # all PS3 programs: test programs and the emulator
tools/build.sh -C src/frontend pkg     # build/salsa64/salsa64.pkg
tools/build.sh test-host               # core unit tests with the native gcc (ASan/UBSan)
tools/build.sh host                    # build/host/salsa64-run: run a ROM on the host
tools/test-roms.sh                     # homebrew ROM suites on the host, against the baseline
```

Outputs go to `build/<program>/`: `.elf` (RPCS3, ps3load), `.self`,
`.fake.self` and, with `pkg`, `.pkg`. Tests in RPCS3, deploying to a PS3 and
the other test scripts are described in [CONTRIBUTING.md](CONTRIBUTING.md).

## Repository layout

```
src/core/      N64 core: memory, interfaces (PI, SI, VI, AI), PIF, boot HLE
src/cpu/       VR4300 interpreter and MIPS → PPC64 recompiler
src/rsp/       RSP graphics, audio and JPEG HLE; low-level RSP interpreter
src/spu/       SPE programs (graphics HLE on SPE 0, audio on SPE 1, LLE)
src/rdp/       N64 RDP and combiners on top of lib/rsxkit
src/frontend/  PS3 application: menu, input, audio output, saves
src/host/      salsa64-run, the host-side runner used for tests
lib/rsxkit/    generic RSX graphics layer, no N64 code (MIT)
lib/spukit/    batched SPE jobs: queue + mailbox doorbell (MIT)
tests/         unit tests, PS3 test programs, homebrew test ROMs
tools/         build, test and deploy scripts
docs/          design notes, measurements, third-party sources (Spanish)
```

Goals, architecture and roadmap: [docs/proyecto.md](docs/proyecto.md).

## License

Copyright (C) 2026 ricowp81.

Salsa64 is free software under the **GNU General Public License, version 2 or
(at your option) any later version** ([LICENSE](LICENSE)). Files that adapt
code from other projects keep their original copyright notices (see
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md)).

`lib/rsxkit/` and `lib/spukit/` are under the **MIT license** (their own
`LICENSE` files) so that other emulators and homebrew, including projects
whose licenses cannot link GPL code, can reuse them. Third-party sources and
references are listed in [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md).

## Legal notice

Salsa64 is not affiliated with, endorsed or sponsored by Nintendo or Sony.
"Nintendo 64" and "PlayStation" are trademarks of their respective owners.

This repository does not contain, and will not accept, commercial ROMs,
BIOS or PIF ROM images, IPL3 boot code from Nintendo, encryption keys, or any
other material copyrighted by Nintendo or Sony, nor any part of Sony's
official PS3 SDK. The emulator boots games without a PIF ROM (high-level
emulation of the boot process). Use it only with dumps of games you own.
The test ROMs in `tests/roms/homebrew/` are freely licensed homebrew built
from source (see their README).
