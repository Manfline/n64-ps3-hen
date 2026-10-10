# ROMs homebrew de prueba

Solo ROMs de libre distribución, compiladas desde fuente en un contenedor
reproducible y sin código de Nintendo. Procedencia y licencias en
`docs/THIRD_PARTY.md`.

| Archivo | Qué es |
|---|---|
| `nemu64-test.z64` | nemu64-test (github.com/thelemmy/nemu64-test, commit `9a8b9f7d`), conjunto "base". Lo genera `tools/build-nemu64-test.sh`. |
| `nemu64-test.z64.sha256` | SHA-256 del ROM; recompilarlo desde cero da el mismo valor. |
| `nemu64-test.LICENSE` | Licencia MIT de nemu64-test. |
| `nemu64-test.NOTICES` | Avisos de licencia de lo enlazado en el ROM (crates de Rust, compiler-builtins, core/alloc, IPL3 de libdragon). |
| `nemu64-test.crates` | Crates de los que depende, con su licencia (`cargo tree`). |
| `salsa64-vitest.z64` | ROM propia de prueba del VI (fuente en `tests/roms/src/vitest`, GPLv2+): barras de color, rampa y un cuadro animado en RGBA5551 y RGBA8888, con autoverificación de la temporización. La genera `tools/build-vitest.sh`. |
| `salsa64-vitest.z64.sha256` | SHA-256 del ROM; recompilarlo desde cero da el mismo valor. |
| `salsa64-vitest.buildinfo` | Versiones de clang, lld y nust64 con que se compiló. |
| `salsa64-gfxtest.z64` | ROM propia de prueba de la HLE gráfica (fuente en `tests/roms/src/gfxtest`, GPLv2+): z-buffer, Gouraud, texturas RGBA16/I4/CI4, TEXRECT en 1 ciclo y en copia, mezcla, culling, 2 ciclos con dos texturas, luces, mapeo de entorno y w grande, con comandos F3DEX2. La verifican `salsa64-run --gfxtest-check` y el frontend (puntos de `expect.h`). |
| `salsa64-gfxtest-f3d.z64`, `salsa64-gfxtest-f3dex.z64` | Las mismas escenas con los comandos de F3D (Fast3D) y de F3DEX 1.x. Las tres las genera `tools/build-gfxtest.sh`. |
| `salsa64-gfxtest*.z64.sha256`, `salsa64-gfxtest*.buildinfo` | SHA-256 y versiones de compilación de cada variante. |
| `salsa64-savetest-eeprom4k.z64`, `-eeprom16k.z64`, `-sram.z64` | ROM propia de prueba del guardado del cartucho (fuente en `tests/roms/src/savetest`, GPLv2+), en tres variantes que solo difieren en el título y en el tipo de guardado de la cabecera de homebrew. Escribe un patrón, lo relee y en el arranque siguiente comprueba que persistió. Las genera `tools/build-savetest.sh`. |
| `salsa64-savetest-*.z64.sha256` | SHA-256 de cada variante; recompilarlas desde cero da los mismos valores. |
| `salsa64-savetest.buildinfo` | Versiones de clang, lld y nust64 con que se compiló. |

Todos los ROMs llevan el IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1;
el contenedor comprueba su MD5 (`7f014ed41cc53bb1a5e44f211f8c3dec`, el que
publica libdragon para r7) antes de usarlo.

Para correrlos: `tools/test-roms.sh` (compara con la línea base
`tests/roms/nemu64-test.fallas`) o, a mano,
`tools/build.sh --sh 'build/host/salsa64-run -u "success rate)" tests/roms/homebrew/nemu64-test.z64'`.
