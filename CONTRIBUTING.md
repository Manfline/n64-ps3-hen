# Contributing to Salsa64

Thanks for your interest. This file covers the legal rules (please read them
first), how to build and test, and how commits and pull requests should look.

## Languages

- Issues and pull requests may be written in **English or Spanish**.
- The code comments, the documentation in `docs/` and the commit history are
  in Spanish. Commit messages in Spanish are preferred; English is accepted
  and may be translated when merging. Don't let the language stop you from
  contributing.

## Legal rules (non-negotiable)

A pull request that breaks any of these will be closed, however good the code.

1. **Toolchain:** only [ps3toolchain](https://github.com/ps3dev/ps3toolchain)
   and [PSL1GHT](https://github.com/ps3dev/PSL1GHT). Never Sony's official
   PS3 SDK (proprietary and incompatible with the GPL), in any form: no code,
   headers, libraries, samples or documentation derived from it.
2. **No Nintendo or Sony material:** no commercial ROMs, BIOS or PIF ROM
   images, Nintendo IPL3 boot code, disassemblies of them, encryption keys,
   PS3 firmware files, or links and instructions to obtain any of these. The
   emulator boots without a PIF ROM by design. Commercial games are tested by
   each contributor with dumps of games they own, outside the repository; do
   not commit captures, saves or logs that contain their data.
3. **Automated tests use only freely licensed homebrew.** A new test ROM must
   be built from source in a reproducible container, carry its license and a
   SHA-256, and be documented in `tests/roms/homebrew/README.md` and
   `docs/THIRD_PARTY.md`. `.gitignore` excludes `*.z64`, `*.n64`, `*.v64` and
   `*.rom` everywhere except `tests/roms/homebrew/`.
4. **Code taken from other projects** (Not64/Wii64, Mupen64Plus, ares or any
   other): check its license first, keep its copyright notices, and record the
   origin (repository, commit, files, license) in `docs/THIRD_PARTY.md`. The
   emulator is GPL-2.0-or-later, so the source license must be compatible
   with that.
5. **`lib/rsxkit/` and `lib/spukit/` are MIT**, so other emulators and
   homebrew can reuse them. No GPL code goes there, and nothing from sources
   with unclear licensing (for example the Super Mario 64 PC port, which is
   read-only reference). Every file there carries
   `SPDX-License-Identifier: MIT`. These libraries must not include anything
   from `src/` or know about the N64: the build compiles them as separate
   static libraries without `-Isrc`, so such an include fails to compile.
   RPCS3 may be read as documentation of the RSX, never copied.

By contributing you agree that your contribution is licensed under
GPL-2.0-or-later, or under MIT for files in `lib/rsxkit/` and `lib/spukit/`.

## Building

Requirements: Docker (or a compatible engine) and Python 3. On arm64 hosts
the engine must also run `linux/amd64` containers, because NVIDIA's Cg
Toolkit (used by `cgcomp`) only exists for x86_64.

```sh
tools/docker-image.sh                  # once: salsa64-toolchain:dev (15–60 min, compiles GCC for PPU and SPU)
tools/build.sh                         # everything for the PS3 (make all inside the image; -j4 etc. go to make)
tools/build.sh frontend                # only the emulator (build/salsa64/)
tools/build.sh -C src/frontend pkg     # build/salsa64/salsa64.pkg
tools/build.sh --sh 'ppu-gcc --version'   # any command inside the image
tools/validate-toolchain.sh            # sanity check: PSL1GHT samples and reference shaders
```

`SALSA64_IMAGE` selects another image tag. The build must stay free of
warnings.

## Testing

Run what applies to your change before opening a pull request:

| Command | What it checks | Needs |
|---|---|---|
| `tools/build.sh test-host` | Core unit tests (CPU, RSP HLE, RDP, saves, menu…) with the native gcc, ASan and UBSan | Docker |
| `tools/build.sh -C tests/fuzz run` | Short fuzzing (ASan/UBSan) of what the frontend reads from third-party files: PNG box art, `.cht` cheats, download indexes. `N=` and `SEED=` change the length and the seed | Docker |
| `tools/test-roms.sh` | Homebrew ROMs on the host with `salsa64-run`: nemu64-test against its baseline of known failures (`tests/roms/nemu64-test.fallas`), plus vitest, savetest, gfxtest (three microcode variants, image checked) and audiotest (sample by sample) | Docker |
| `tools/test-rec.sh` | The recompiler on a big-endian ppc64 host under qemu, in strict mode (`--lockstep` also compares it with the interpreter, ~10 min) | `tools/docker-image-ppc64.sh` once |
| `tools/test-phase0.sh`, `test-phase1.sh`, `test-phase2.sh` | PS3 programs and the emulator in RPCS3 (RSX, SPE jobs, graphics HLE on the SPE) | RPCS3 with PS3 firmware |

A new failure in `tools/test-roms.sh` is a regression. When a change fixes
tests, update the baseline with `tools/test-roms.sh --actualizar` and say so
in the commit.

**RPCS3:** `tools/rpcs3-run.py` drives RPCS3 headless and waits for the
program's `RESULTADO` line. It currently assumes the macOS install
(`/Applications/RPCS3.app`, logs in `~/Library/Caches/rpcs3`); the `RPCS3`
environment variable overrides the binary. You must install the PS3 firmware
in RPCS3 yourself; it never goes into the repository. RPCS3 does not
reproduce the Cell's timing: **never draw conclusions about performance, SPE
synchronization or the JIT from RPCS3.**

**Real PS3 (HEN or CFW):**

```sh
tools/udplog.py                                        # receive the log over UDP (port 18194)
PS3_IP=x.x.x.x tools/deploy-ps3.sh ps3load build/salsa64/salsa64.self
PS3_IP=x.x.x.x tools/deploy-ps3.sh ftp build/salsa64/salsa64.pkg   # via webMAN MOD's FTP
```

`PS3_IP` can also live in `tools/ps3.conf`, which git ignores. The app also
writes its log to `/dev_hdd0/salsa64/salsa64.log`.

**Test packages:** give every test build of the emulator its own title (and,
to keep it from replacing the installed release, its own `PROGRAM` and
`APPID`). The Package Manager and the XMB list apps by the `PARAM.SFO` title,
so two packages called «Salsa64» are easy to mix up:

```sh
tools/build.sh -C src/frontend PROGRAM=salsa64-gfx TITLE="Salsa64 gráficos 3" APPID=SLSA00114 pkg
```

The build stamps the version (`git describe --tags --always --dirty`, worked
out by `tools/build.sh` outside the container), the title and the APPID into
the program: the menu shows them in its top-right corner, the pause menu next
to its title, and the first log line reads «Salsa64 v0.2.5-3-gabc1234
(Salsa64 gráficos 3, SLSA00114)». Check it before trusting a test run, and
include it in bug reports. Without git the version reads «desconocida».

## Code style

- C99/C11 as supported by ps3toolchain's GCC; C++ only if a dependency
  requires it. Match the surrounding code: naming, comment density and
  language (comments are in Spanish).
- New files start with an `SPDX-License-Identifier` line
  (`GPL-2.0-or-later`, or `MIT` in `lib/rsxkit/` and `lib/spukit/`).
- No dynamic allocation in the emulation loop: allocate at startup.
- SPE code: DMA structures aligned to 16 bytes (preferably 128), transfers in
  multiples of 16 bytes, double buffering, and the local store budget (code +
  data + buffers ≤ 256 KB) documented in the file header.
- PPE ↔ SPE communication works in batches (one command list per frame), not
  one request per triangle.
- Anything N64-specific stays out of `lib/rsxkit/`: it goes in `src/rdp/`.
- Every optimization comes with a before/after measurement in
  `docs/perf-log.md`, taken on a real PS3 when it is about speed. New RSX
  findings go in `docs/rsx-notes.md`.

Goals, architecture and roadmap: [docs/proyecto.md](docs/proyecto.md).

## Documentation and working tools

- The profiling app and its session format are described in
  [docs/perfilado.md](docs/perfilado.md) (Spanish); summarize a session with
  `python3 tools/perfilado/perfilado.py resumen <file>`.
- `tools/escenas-host.sh` runs reproducible scenes on the host (from your own
  backups) and compares two builds image by image: use it before and after
  any change to the graphics HLE.
- [docs/bibliotecas.md](docs/bibliotecas.md) (Spanish) explains how `rsxkit`
  and `spukit` are kept in `lib/` with `git subtree` and synced with their own
  repositories.

## Commits and pull requests

- One topic per commit. The first line describes the change, either in the
  imperative or as `Area: description` (for example
  `Control: retener cada pulsación hasta que el PIF la entregue`). The body
  explains why, what was measured or verified (tests run, scenes, numbers)
  and whether validation on a real PS3 is still pending.
- Keep `docs/estado.md` up to date for user-visible changes, and
  `docs/THIRD_PARTY.md` for any outside source.
- In the pull request, list the tests you ran and, if you tried commercial
  games with your own dumps, which games, regions and scenes (never where to
  download them).
