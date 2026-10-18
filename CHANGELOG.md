# Changelog

All notable changes to Salsa64. Speeds are measured on a real PS3 (HEN,
NTSC backups); `docs/perf-log.md` has the detailed numbers and
`docs/estado.md` the work log (both in Spanish).

## [0.2.8] — unreleased

### Added
- **Restart from the pause menu:** two new rows, both with an "Are you sure?"
  box. "Restart (Reset button)" does what the console's Reset button does: the
  game first gets the pre-NMI interrupt (the one libultra uses to save and
  mute the audio), half a second later the NMI resets the CPU and the RCP
  with the RDRAM kept (the program is copied again from the ROM and the game
  sees a soft reset). "Restart from scratch" closes the game completely
  (saves are written first) and opens it again without going back to the list.
  They work with the HLE, the RSP in low-level mode, interpolation and the
  recompiler. On a computer, `salsa64-run --reset-at N` and `--nmi-at N` do
  the same at a given field; Super Mario 64, Zelda: Ocarina of Time and
  GoldenEye 007 go back to their logos after either.
- **MusyX audio** (issue #11): the games that use Factor 5's MusyX engine
  (v1 and v2) used to run mute because their audio task is not a command list.
  They now have sound: Hydro Thunder, Rush 2049, NBA Showtime, Resident
  Evil 2 and Star Wars: Rogue Squadron (the last two still have no graphics).
  Gauntlet Legends and Tarzan use the same engine but have not been tried.
  Checked on a computer against the real microcode, the mix matches byte for
  byte (Indiana Jones differs by 1 in one channel in 49 of its tasks).
- **Diddy Kong Racing is playable:** HLE for Rare's own graphics microcode
  (F3DDKR): logos, intro, title, menus, track selection and the races
  (Ancient Lake, the Timber's Island adventure) draw correctly in the test
  build on a computer; the console run is still to be confirmed. Jet Force
  Gemini and Mickey's Speedway USA use a variant of the same microcode that is
  recognised too and now run with every command implemented (the title
  screens, menus, cut-scenes and the first race of Mickey's Speedway draw in
  the test build; the start lights of that race change from red to green).
  Neither has been tried on a console yet.
- **Killer Instinct Gold draws:** the game writes part of every frame (the
  stage background and the frames around the screen) as RDP lists from the CPU
  and does not start its graphics until it gets the RDP interrupt of those
  lists. They are now drawn by the graphics SPE, in the same state and with
  the same texture cache as the rest of the frame, and the interrupts of the
  RDP are queued so that two close ones are not lost.
- **Audio identical to the real microcode in 99.87 % of the tasks** (51 136
  tasks in 21 games, compared byte for byte; it was 13.1 %): pitch changes,
  mixing, envelopes, the filters and the MP3 decoder follow the rounding of
  the real RSP. The JPEG task of Ocarina of Time and Pokémon Stadium 1 and 2
  (pre-rendered backgrounds and images) is now exact too: 0 pixels different
  out of 1 075 200 (it was about 1 %). Listen for it in Conker, Perfect
  Dark, Banjo-Tooie, Majora's Mask, Yoshi's Story and 1080° Snowboarding.
- **Frozen-game detector:** if a game that was reading the controllers and
  drawing stops doing both for 6 seconds while the video keeps running, the
  log says what the hardware and the CPU were waiting for and
  `/dev_hdd0/salsa64/` gets a RAM dump (`cuelgue-<field>.rdram`, up to three
  per game) to attach to the bug report.

### Changed
- **The graphics SPE program comes in three images**, one per family of
  graphics microcodes (first generation: Fast3D, F3DEX 1.x and Rare's
  variants; F3DEX2 with S2DEX2 and F-Zero X's variant; Conker's). The one each
  game needs is loaded with its first graphics task and checked against the
  PPE before use. Each image has the texture cache back at 1024 textures and
  room in the SPE's local store for the microcodes still missing (S2DEX,
  Factor 5). The log says which image each game loaded, its size and its
  stack headroom.

### Known issues
- Killer Instinct Gold: in the first version of the fix, the second fight
  froze with the music still playing (the controllers stopped being read and
  no more graphics tasks arrived). This build includes the detector above,
  and it has not been shown on a console whether it is solved.
- NBA Showtime runs sped up.
- Perfect Dark slows down in scenes of more than about 2300 triangles.
- Rogue Squadron, Resident Evil 2 and Indiana Jones still draw nothing (no
  HLE for their graphics microcode); only their audio works.

## [0.2.7] — 2026-10-16

### Added
- **Controller Pak:** emulated, with one `.mpk` file per game (four paks, one
  per controller) and a per-game «Accesorio» option; controller 1 carries a
  Controller Pak by default. Games that save only to the Pak (Mario Kart 64
  ghosts, Rush 2049, Hydro Thunder, Donald Duck…) can now save, and games
  that kept polling for a missing Pak no longer slow down (1080 Snowboarding).
- **Button remapping from the menu** (issue #6): Options → «Controles» edits
  the mapping for every game, and each ROM's settings edit the mapping for
  that game. Press the DualShock 3 button (or two at once, e.g. Z on L2 + R2)
  for each N64 button, and choose which stick moves the N64 stick; the C
  buttons come from the other one.
- **ROM list with folders** (issue #10): subfolders, thousands of entries
  instead of 256, sorting by header title or by file name, file names for
  titles that are not Latin-1 (Japanese ROMs), L3 to refresh the list without
  leaving, and going up returns to the folder you came from. Salsa64's own
  data folders (saves, states, covers…) are no longer listed.
- **The left stick moves the menu** like the d-pad, for controllers with a
  worn-out d-pad.

### Fixed
- **Wave Race 64:** it uses its own variant of the graphics microcode, which
  was read as the standard one (textures «like rays from the centre»).
- **Pokémon Stadium:** the «Game Pak Check» screen (player 1 panel, «OK?» and
  the WARNING notice, which turned into noise after going back to the title),
  the ghost of «VS Event Battle» on «Please Select», and a crash in VS.
- **Donald Duck: Quack Attack / Goin' Quackers** was given an EEPROM it does
  not have (it saves only to the Controller Pak).
- **FPU:** CVT.W / CVT.L follow the rounding mode and MUL.S stays in
  recompiled code instead of falling back to the interpreter.

### Faster
- **CPU recompiler:** libultra's idle loop (waiting through the scheduler) is
  now skipped like any other wait loop, and the COP0 writes most games repeat
  stay in recompiled code. On a real PS3: **Doom 64 21 → 60 fields/s,
  NFL Blitz 26 → 60, 1080 Snowboarding 31 → 60, Star Wars Episode I: Racer
  32 → 46**.

### Known issues
- Frame interpolation (experimental) makes 2D sprites and UI layers flicker or
  leave a trail (Mario Kart 64 items, Smash menus; issue #17). It can be turned
  off per game.
- Games with **MusyX** audio have no sound yet (Hydro Thunder, Rush 2049, NBA
  Showtime, Gauntlet Legends…; issue #11). It is the next item.
- Diddy Kong Racing, Indiana Jones, Rogue Squadron and Resident Evil 2 do not
  run yet (no HLE for their microcodes). The Rumble Pak is not emulated.

## [0.2.6] — 2026-10-15

### Added
- **Banjo-Tooie is playable:** the PIF now answers the game's CIC-6105
  challenge, which it uses to unlock its data.
- **F-Zero X races:** the game probes the 64DD (open bus on the cartridge
  port) and switches graphics microcode in the middle of every frame; both
  are handled, so the cars, the HUD and the cars' highlights (F-Zero's own
  F3DFLX microcode, alpha from a lookup table) are drawn.
- **Frame interpolation (experimental, off by default):** games that run at
  20 or 30 fps can show in-between frames rebuilt from the geometry, for
  smoother motion. «Interpolación: Por geometría (exp.)» in Options, per
  game, or live from «Imagen» in the pause menu. It looks best at 3× / 720p.
- **○ in the game list exits to the XMB**, with a confirmation, so you no
  longer need to scroll to the end of a long list.
- **Version, title and APPID** shown in the menu, the pause menu and the
  first line of the log (handy for bug reports).
- **Conker's shadows** (render to an 8-bit texture), **Pokémon Snap** (exact
  primitive depth, leaves with alpha threshold), **Kirby 64** (enemies were
  missing their bodies; S2DEX palettes) and **Pokémon Stadium 2** (its JPEG
  images).
- **Black box** for bug reports (SELECT + L2 with the `cajanegra` file): the
  last graphics tasks and frames, compressed in the background.
- **CFW fallback:** if the CPU recompiler cannot run generated code on a
  console (issue #2), Salsa64 falls back to the interpreter instead of
  hanging.

### Fixed
- **Console freeze in F-Zero X** at 5× (course selection): an off-screen
  scissor went past the RSX limit. Also, no wait on the GPU can be endless
  any more: after 2 s the log says what is stuck, the PS button keeps
  working, and after 12 s the game saves and closes on its own.
- **Large ROMs (40–64 MB) failed to open** («No se pudo leer») after playing
  other games in the same session: Conker, Paper Mario, Pokémon Stadium 2,
  Smash Remix, Resident Evil 2. The ROM now goes outside the fragmented heap.
- **Star Fox 64:** coloured noise at the top during the «Nintendo presents»
  logo.
- **Super Mario Odyssey 64 (hack)** ran far too fast (an unusual VI_V_SYNC).
- **Interpolation turned on mid-game** no longer slows the graphics SPE down
  (stretched audio in heavy scenes).

### Faster
- CPU recompiler: compact FPU code, a cached base register for memory
  accesses and a JR/JALR target cache (GoldenEye and Perfect Dark 2–8 %
  faster in the heaviest scenes).

### Known issues
- F-Zero X: some background objects are drawn over the track (shading
  interpolation after clipping; to be fixed in a later version).
- Diddy Kong Racing, Indiana Jones, Rogue Squadron and Resident Evil 2 do
  not run yet (their graphics microcode has no HLE); low-level RSP emulation
  is in progress.
- Super Mario Odyssey 64 may crash after a while (under investigation; the
  log now records what overwrote the code).
- GoldenEye: thin blue lines at some seams. An experimental fix (drawing the
  anti-aliased edges) is included but off by default, because on real
  hardware it made some model polygons overlap; the `aristas-aa` file turns
  it on.

## [0.2.5] — 2026-10-13

### Fixed
- **Console freeze in Options:** with «CRT Trinitron» chosen as the filter,
  moving to the Filter row in Options (main menu) froze the PS3 (its help
  text was missing an end marker and the menu read past it). The pause menu
  was not affected.
- **Fewer copies back to N64 memory:** after a change in the screen width
  (Perfect Dark's intro, GoldenEye after the mission screens) every frame
  was taken for an auxiliary image and copied back, with the emulation
  waiting each time; Perfect Dark's intro went from about one copy per
  frame to almost none.

### Added
- **Mask density** for the «CRT máscara» filter: Normal or Fine (smaller,
  closer dots: 9 px triads instead of 12), in Options, per game and live in
  «Imagen». The choice made in Options is now saved like the rest.

## [0.2.4] — 2026-10-13

### Fixed
- **Super Smash Bros.** (and Smash Remix) no longer freeze on the first KO.
  The game keeps code and pointers in the margins of its z-buffer and, in the
  KO, draws a mask there; copies back to N64 memory now write only what the
  RDP could have drawn (the box of each drawing inside its scissor). The
  `fbfx.cfg` workaround is no longer needed.
- Stretched textures when a tile with clamp and mask did not fit the decode
  area (Smash: Hyrule Castle stage, Kongo Jungle leaves): one period of the
  mask is decoded and repeated instead of cutting rows.

### Added
- **Five CRT filters** made for Salsa64, still experimental: shadow mask,
  a small Trinitron TV over RGB, S-Video or composite, and a big Trinitron
  (Sony KV-29). In Options, per game and live from «Imagen» in the pause
  menu. While a CRT filter is on, the internal scale is capped at 3× (no
  visible difference above it). Measured on a real PS3 at 1080p (GoldenEye,
  Dam): 7.4–13.2 ms of RSX per frame, every field presented. A short
  «Aplicando filtro…» notice shows while a filter is prepared.
- Experimental: the signal stage of the S-Video and composite filters on
  two SPEs (`filtros-spe` file, off by default; not faster yet).

### Changed
- `lib/rsxkit`: post-processing reduces the image in two passes (width, then
  height). The CRT filters redo only their final pass when the game repeats
  a frame.

## [0.2.3] — 2026-10-13

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
