# Changelog

All notable changes to Salsa64. Speeds are measured on a real PS3 (HEN,
NTSC backups); `docs/perf-log.md` has the detailed numbers and
`docs/estado.md` the work log (both in Spanish).

## [Unreleased]

### Added — contributed by VIDEOGAMES SCZ
- **Pause menu** (START + SELECT): resume, save/load state, state slot,
  cheats, FPS counter and quit (with the «Confirmar al salir» question when it
  is on). It replaces the old «¿Volver al menú?» box; the SELECT shortcuts
  still work.
- **Cheats** per game: Project64 (`Config/Cheats`, section chosen by the ROM
  CRC, options with `??`) and RetroArch `.cht` files in
  `/dev_hdd0/salsa64/cheats`, found by ROM name or by a similar name of the
  same region. GameShark codes 80/81/A0/A1/F0/F1, D0–D3 and 50. Page in the
  menu (□ on a ROM) and list inside the game (SELECT + □), with the active
  ones remembered next to the `.cht`.
- **Box art** in the menu: PNG files in `/dev_hdd0/salsa64/covers` (any color
  type, interlaced too), by ROM name, game code or a RetroArch-style name.
  Built-in PNG decoder (no zlib needed).
- **Aspect** option: 4:3 (original) or 16:9 (stretched).
- **Simple image filters**: Smooth (as before), Sharp (nearest-neighbour) and
  soft or strong scanlines.
- **Per-game settings** (SELECT on a ROM): aspect, filter and overscan,
  saved in `juegos/<ROM>.cfg`.
- **Six menu languages**: Spanish, English, French, German, Italian and
  Portuguese.
- **Downloads of box art and cheats** from the menu — experimental and
  **off by default** (hidden unless `descargas.cfg` has `activar = si`): the
  PS3 only speaks old TLS and HTTPS certificates cannot be verified. Not yet
  tried on a real PS3.

### Added
- **«Imagen» in the pause menu**: filter, aspect, overscan and internal scale
  of the running game. Filter, aspect and overscan change live on the paused
  frame; the scale applies when the game resumes. «Guardar para este juego»
  stores them as per-game settings.
- **Per-game internal scale** (`escala` in `juegos/<ROM>.cfg`, also on the
  SELECT page): set when the game opens, back to the Options one when it
  closes.

### Changed
- `lib/rsxkit`: the 2D layer can draw an RGBA image slot
  (`rsxkit_ui_image_set` / `rsxkit_ui_image`); it maps 3 MB instead of 2.
- Games with Nintendo's boot code start with the CPU timer where the real
  boot code leaves it (about half a second), so those that waited in the
  controller setup start up to half a second sooner, as on the console.

### Fixed
- **Conker's Bad Fur Day** stayed black after the «For Mature» screen since
  0.2.1: the z-buffer clear written to RDRAM ignored the scissor and
  overwrote game data just past the z-buffer. It is now clipped like the
  RDP does.
- Conker no longer shows 25–30 s of black at start-up.

## [0.2.2] — 2026-10-12

### Performance
- **Smaller recompiled code.** The dynamic recompiler now emits about 23 %
  less PowerPC code per N64 instruction (one shared slow path per block
  instead of one per memory access, a 4-instruction KSEG0 check, 3-word block
  links). On a real PS3 the GoldenEye Dam runs 6–12 % more fields per second
  in its heaviest parts; Perfect Dark 3–7 % faster per instruction.

### Controls
- An N64 button can take one or two DualShock 3 buttons in `controles.cfg`:
  `Z = L2 R2` puts Z on both triggers.

### Diagnostics
- Time sampling of the recompiled code from the second PPE thread
  (`muestreo` file): logs where the emulation time goes, per category and per
  block.
- Optional per-game recompiler profile (`perfil-activo` file; off by
  default): block counts, successors and branch statistics saved in
  `perfiles/`, with no game code in the file.

## [0.2.1] — 2026-10-12

### Fixed
- **Donkey Kong 64:** the camera no longer zooms in and out every ~42 frames.
  The game reads the depth buffer from N64 memory to check whether DK is
  hidden; the depth clear is now written back to memory.
- **Zelda: Ocarina of Time:** Navi's glow and the torch halos are drawn again
  (the game also decides them by reading the depth buffer).
- **Zelda: Ocarina of Time:** no more hitch when entering pre-rendered rooms.
  The JPEG task now runs on a free SPE, vectorized (2.9 ms per room on the SPE
  and 0.3 ms on the emulation thread, down from 68 ms on the PPE).

### Diagnostics
- The presentation thread's time per field is always measured and logged;
  a synthetic load (`carga-hilo1` file) measures its cost to emulation
  (~2.5 % in the GoldenEye Dam).

## [0.2.0] — 2026-10-12

### Games
- **Donkey Kong 64 runs at full speed** (was ~20 %). The recompiler no longer
  flushes its code cache when the game switches the FPU register mode
  (`Status.FR`), which it does ~260 times per second; only blocks that use
  odd FPU registers depend on it and get a twin per mode.
- Donkey Kong 64: interlaced high-resolution logos shown whole, no more
  "zoom" every other frame (both framebuffers at the same scale), zipper
  scene no longer black.
- **Majora's Mask is playable:** 128 KB FlashRAM saves (also stored in save
  states), the end-of-day bell zoom, the Scarecrow dance and Zelda's memory
  blur, and the telescope lens (`MIRROR_ONCE` on the RSX, `G_TP_NONE` at half
  a texel).
- **Perfect Dark:** intro at 60 fps (texture decoding on the SPEs, trimmed to
  what is sampled); rooftop motion blur at full resolution, read straight
  from the RSX instead of decoded on the CPU.
- **Star Fox 64:** no freeze after 5 seconds (framebuffer copies no longer
  overwrite data a DMA wrote later), no static at the top of the screen
  (fills taller than 480 lines split among framebuffers), no flicker in game.
- **Banjo-Kazooie** boots like the real CIC 6103 (its copy protection used to
  scramble the camera planes); intro fade fixed (fog as the blender color).
- Conker's face no longer black (`G_SETCONVERT`, K4/K5). Zelda: lines on the
  N64 logo (TEXRECT clamped per tile) and Link's tunic (mask with mirror
  inside clamp). Mario Kart 64: black area above the Luigi Raceway wall.

### Features
- PS button: the game pauses while the XMB menu is open and resumes cleanly.
- START + SELECT asks before leaving the game («Confirmar al salir» in the
  menu options).
- Per-field presentation trace (`trace-vi` file) and graphics dumps
  (SELECT + R2) for bug reports.

### Performance and stability
- Combiner results are reused on the graphics SPE while their inputs don't
  change.
- Framebuffer copies waited on by the emulation are always translated, so
  they no longer hit the 200 ms timeout (Perfect Dark intro, Star Fox logo).
- No deadlock when a game sends more graphics tasks in one field than there
  are buffer sets (Paper Mario, Mario Party).
- The build checks the graphics SPE stack headroom (at least 1.5 KB).

### Known issues
- Super Smash Bros. (and Smash Remix) freeze on the first KO. Workaround:
  `/dev_hdd0/salsa64/fbfx.cfg` containing `1`.
- Banjo-Tooie, F-Zero X, Diddy Kong Racing and Indiana Jones show a black
  screen; Resident Evil 2 flickers; Doom 64 runs at ~40 %.
- GoldenEye blue seams, Banjo-Kazooie jigsaw screens, Conker square shadows,
  noisy textures in Kirby 64 and Pokémon Stadium 2, stretched textures in
  Pokémon Snap.

## [0.1.0] — 2026-10-11

First public preview.

- Dynamic recompiler MIPS → PowerPC64 on the PPE; graphics HLE (F3D, F3DEX,
  F3DEX2, S2DEX and Rare's microcodes) and audio HLE on the SPEs; runtime
  generated RSX fragment programs for the RDP combiners.
- Output at 720p or 1080p with internal render scale 1× to 5×, changeable
  from the menu without restarting.
- XMB-style ROM menu (`.z64`, `.v64`, `.n64`, internal drive or USB).
- Save states (10 slots per game), FPS counter, EEPROM and SRAM saves.
- Framebuffer effects: render to texture and copies back to N64 memory.
- Full speed: Zelda: Ocarina of Time, Super Mario 64, Mario Kart 64.
  Playable: GoldenEye, Perfect Dark, Banjo-Kazooie, Conker.

[0.2.2]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.2
[0.2.1]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.1
[0.2.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.0
[0.1.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.1.0
