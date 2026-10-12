# Changelog

All notable changes to Salsa64. Speeds are measured on a real PS3 (HEN,
NTSC backups); `docs/perf-log.md` has the detailed numbers and
`docs/estado.md` the work log (both in Spanish).

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

[0.2.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.2.0
[0.1.0]: https://github.com/ricowp81/Salsa64/releases/tag/v0.1.0
