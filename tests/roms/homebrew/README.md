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

El ROM lleva el IPL3 libre de libdragon r7 (Unlicense) que trae nust64 0.4.1;
el contenedor comprueba su MD5 (`7f014ed41cc53bb1a5e44f211f8c3dec`, el que
publica libdragon para r7) antes de usarlo.

Para correrlo: `tools/test-roms.sh` (compara con la línea base
`tests/roms/nemu64-test.fallas`) o, a mano,
`tools/build.sh --sh 'build/host/salsa64-run -u "success rate)" tests/roms/homebrew/nemu64-test.z64'`.
