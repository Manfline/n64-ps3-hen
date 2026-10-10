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
| `salsa64-audiotest.z64` | ROM propia de prueba del AI y de la HLE de audio (fuente en `tests/roms/src/audiotest`, GPLv2+): un tono conocido (La 4, Do 5 a la izquierda, Mi 5 a la derecha, Sol 5, Do 6, silencio) primero desde el CPU y después intercalado por una tarea de audio ("Salsa64 ABI NEAD"); se verifica sola y `salsa64-run --audiotest-check` compara lo que sale por el AI muestra por muestra. La genera `tools/build-audiotest.sh`. |
| `salsa64-audiotest.z64.sha256`, `salsa64-audiotest.buildinfo` | SHA-256 (reproducible) y versiones de clang, lld y nust64. |
| `salsa64-savetest-eeprom4k.z64`, `-eeprom16k.z64`, `-sram.z64` | ROM propia de prueba del guardado del cartucho (fuente en `tests/roms/src/savetest`, GPLv2+), en tres variantes que solo difieren en el título y en el tipo de guardado de la cabecera de homebrew. Escribe un patrón, lo relee y en el arranque siguiente comprueba que persistió. Las genera `tools/build-savetest.sh`. |
| `salsa64-savetest-*.z64.sha256` | SHA-256 de cada variante; recompilarlas desde cero da los mismos valores. |
| `salsa64-savetest.buildinfo` | Versiones de clang, lld y nust64 con que se compiló. |

Todos los ROMs llevan el IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1;
el contenedor comprueba su MD5 (`7f014ed41cc53bb1a5e44f211f8c3dec`, el que
publica libdragon para r7) antes de usarlo.

Para correrlos: `tools/test-roms.sh` (compara con la línea base
`tests/roms/nemu64-test.fallas`) o, a mano,
`tools/build.sh --sh 'build/host/salsa64-run -u "success rate)" tests/roms/homebrew/nemu64-test.z64'`.
