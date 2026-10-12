# Notas del RSX

Hallazgos sobre el RSX en homebrew (documentación escasa: anotar todo aquí).

## Shaders y cgcomp

- `cgcomp` (PSL1GHT) compila Cg con `libCg` a ensamblador NV40 (vp40/fp40) y
  luego lo ensambla a microcódigo del RSX con su propio parser
  (`tools/cgcomp/source/compilerfp.cpp`, `compilervp.cpp`, `fpparser.cpp`).
- `cgcomp -a` compila **desde ensamblador**, sin `libCg`. Ese ensamblador es
  código C++ autocontenido: candidato a portarse a la consola para generar
  fragment programs de combiners en tiempo de ejecución (Fase 4). Revisar
  antes su licencia (PSL1GHT es MIT) y dependencias.
- Los fragment programs se ejecutan desde memoria del RSX: copiar el
  microcódigo a un búfer de `rsxMemalign` y pasar su offset a
  `rsxLoadFragmentProgramLocation`. Los vertex programs se suben por el búfer
  de comandos (`rsxLoadVertexProgram`).
- Entradas del vertex program: `POSITION` → atributo 0, `COLOR` → atributo 3
  (`GCM_VERTEX_ATTRIB_COLOR0`). Si se enlazan 3 componentes a un `float4`,
  `w` vale 1.
- **`cgcomp` quita la corrección de perspectiva en MUL, DP2/3/4, DIV y NRM3**
  (`fpparser.cpp: IsPCDisablingInstruction`): pone el bit 31 de SRC2 de la
  instrucción, y la entrada (varying) que lee llega dividida entre w. Con
  w = 1 no se nota; con los w de un juego de N64 (≈ 2000 para la luna del
  intro de Zelda) el término vale casi cero. RPCS3 lo emula así
  (`tcN * gl_FragCoord.w` en el GLSL que registra con *Log shader
  programs*). rsxkit limpia el bit al subir cada fragment program
  (`lib/rsxkit/src/fprog.c`); gfxtest lo cubre con un cuadro de w = 1000.
  El generador de fragment programs de la Fase 4 debe dejar el bit en 0.
  Formato de cada instrucción: 4 palabras de 32 bits con las mitades de 16
  bits intercambiadas; si alguna fuente es constante (tipo 2), los 16 bytes
  siguientes son su valor.
- RPCS3 compila los shaders en segundo plano: la primera corrida después de
  cambiar un fragment program puede saltarse dibujos (pantalla negra en la
  captura). Repetir antes de concluir.

## Microcódigo del fragment program (generador de lib/rsxkit, 2026-10-10)

Lo que hace falta para emitir un fragment program sin cgcomp (fuentes:
`nvfx_shader.h` y `compilerfp.cpp` de cgcomp, MIT, y los `.fpo` que
compila; `lib/rsxkit/src/fpgen.c`):

- Cuatro palabras de 32 bits por instrucción. En memoria, cada palabra va
  con sus mitades de 16 bits intercambiadas (`endian_fp` de cgcomp:
  escribe `SWAP32(palabra)` y luego intercambia mitades, así que un PPU
  big-endian guarda `(w << 16) | (w >> 16)`). Si una fuente es constante
  (tipo 2), los 16 bytes siguientes son sus cuatro floats, con el mismo
  intercambio.
- Palabra 0: bit 0 fin del programa; bits 1–6 registro destino; bit 7
  destino H (media precisión; H2k y H2k+1 son las mitades de Rk); bits
  9–12 máscara xyzw; bits 13–16 **el único registro de entrada que puede
  leer la instrucción** (COL0 = 1, TEXCOORDn = 4 + n; las fuentes de tipo
  1 lo toman de aquí: dos entradas distintas en una instrucción no
  caben, hay que pasar una por MOV a un temporal); bits 17–20 unidad de
  textura (TEX); bits 22–23 precisión (0 fp32, 1 fp16); bits 24–29 opcode
  (MOV 1, MUL 2, ADD 3, MAD 4, TEX 0x17); bit 31 saturar.
- Palabras 1–3 (fuentes 0–2): bits 0–1 tipo (0 temporal, 1 entrada, 2
  constante); 2–7 índice del temporal; 8 temporal H; 9–16 swizzle (dos
  bits por componente: identidad 0xE4, `.wwww` 0xFF, `.zwzw` 0xEE); 17
  negar. cgcomp pone en toda instrucción, en la palabra 1, la condición
  «siempre» (7 en los bits 18–20) con swizzle identidad (bits 21–28), y
  en la palabra 3, cuando lee un TEXCOORD, 0x7FC en los bits 19–29. El
  bit 31 de la palabra 3 quita la corrección de perspectiva (arriba).
  Una fuente que no se usa va como temporal 0 con swizzle identidad.
- `result.color` es R0 (precisión completa, bit 7 en 0). cgcomp hace las
  operaciones en fp16 (precisión 1, temporales H) y en la última escribe
  R0 con precisión 1 y saturación; TEX va con precisión 0.
- Lo que `rsxLoadFragmentProgramLocation` necesita además del offset:
  `num_regs` (registros R; mínimo 2), `fp_control` (0x40 = escribe el
  color; se le suma `num_regs << 24` y el bit 10), y las máscaras
  `texcoords`/`texcoord2D`/`texcoord3D` (un `NV40TCL_TEX_COORD_CONTROL(i)`
  por TEXCOORD leído: bit 4 si es de 3 o 4 componentes, bit 0 si de 2).
  rsxkit arma un `rsxFragmentProgram` con solo esos campos.
- Verificación sin consola: `tests/unit/test_fpgen.c` interpreta el
  microcódigo (TEX, MOV, MUL, ADD, MAD, swizzle, H/R, saturación) y
  comprueba primero que los `.fpo` de cgcomp (`tests/unit/fpgen_ref.h`,
  generado por `tools/fpgen-ref.sh`) dan la fórmula de su shader y luego
  que los programas generados dan la misma. Los de referencia leen las
  mismas entradas que los generados equivalentes (`texcoords` igual).
- Orientación: con la y hacia arriba en el espacio de recorte, el frente de
  la N64 es antihorario (`rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW)`). Con CW
  el culling sale al revés (faltaban el cielo y la luna en Zelda).
- Profundidad: `rsxSetZMinMaxControl(ctx, FALSE, TRUE, FALSE)` (sin recorte
  en z cercano/lejano por hardware; la HLE ya recortó) y z/w en [0, 1].

## Framebuffer

- Superficie X8R8G8B8 en memoria local del RSX; el PPE puede leerla por su
  dirección mapeada (lento; solo para pruebas).
- **RPCS3: para leer el framebuffer desde el PPE hay que activar "Write Color
  Buffers"** (configuración de GPU); si no, la memoria emulada no recibe el
  render y la verificación de `rsx-triangle` falla aunque se vea bien.

## Flags de segmentos del ELF (lv2.ld)

Además de R/W/X, `lv2.ld` usa bits altos en `p_flags` para acceso del SPU y
del RSX: 0x00200000/0x00400000 (SPU escritura/lectura) y
0x02000000/0x04000000 (RSX escritura/lectura).

## Texturas (lib/rsxkit, 2026-10-10)

- `rsxLoadTexture` de PSL1GHT arma el registro de formato con
  `format << 8 | 0x8000`: el bit 0x80 de los formatos de Sony
  (`CELL_GCM_TEXTURE_*`) lo pone la biblioteca, así que los valores
  `GCM_TEXTURE_FORMAT_*` de PSL1GHT van sin él (A8R8G8B8 = 5 → 0x85).
- El formato 23 (0x97 con el bit) es **R5G5B5A1**, idéntico al RGBA5551 de
  la N64 (R en los bits altos, alfa en el bit 0). PSL1GHT lo llama, por
  error, `GCM_TEXTURE_FORMAT_R5G6B5A1`. Con él, el framebuffer de 16 bits
  de la N64 se muestra sin convertir en el PPE.
- Las texturas de 16 y 32 bits se leen en el orden de bytes de la PS3
  (big-endian): un framebuffer RGBA8888 de la N64 (bytes R, G, B, A) leído
  como A8R8G8B8 deja R en el canal "A", G en "R" y B en "G"; el remap lo
  corrige: `GCM_TEXTURE_REMAP_MODE(XYXY, B, A, R, G, ONE, REMAP, REMAP,
  REMAP)` (el primer grupo dice de qué canal sale cada salida A, R, G, B;
  el segundo, si se copia, vale 0 o vale 1).
- Texturas lineales (`GCM_TEXTURE_FORMAT_LIN`) de tamaño que no es potencia
  de dos, con coordenadas normalizadas y `CLAMP_TO_EDGE`: funcionan (320×237).
  Pitch usado: múltiplo de 64 bytes; offset alineado a 128.
- Tras reescribir una textura hay que invalidar la caché de texturas
  (`rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE)`) antes de dibujar.
- Modo inmediato: `rsxDrawVertexBegin` + `rsxDrawVertex2f(TEX0)` +
  `rsxDrawVertex4f(POS)` + `rsxDrawVertexEnd`; escribir el atributo 0
  (posición) emite el vértice, así que va al final. Los arreglos de vértices
  se desactivan con `rsxBindVertexArrayAttrib(..., elems = 0, ...)`.
- Etiquetas: `rsxSetWriteBackendLabel` después de un dibujo se escribe cuando
  el RSX termina ese dibujo; sirve para saber cuándo se puede reescribir
  una textura. Antes de esperar una etiqueta hay que vaciar el búfer de
  comandos (`rsxFlushBuffer`), o la espera no termina.

Verificado en RPCS3 0.0.43 y en la **PS3 real** (2026-10-10, CECH-3011A,
HEN): el frontend con vitest lee la pantalla del RSX y RGBA5551 y RGBA8888
salen con el color esperado a 1280×720. Subir la textura y dibujar cuesta
0,2–0,4 ms por cuadro en el PPE.

## Trampas de PSL1GHT (f649a08, 2026-10-10)

- **`rsxSetVertexProgramConstants` está roto:** avanza `context->current` a
  mano mientras escribe y después otra vez con `RSX_CONTEXT_CURRENT_END`, así
  que deja palabras sin escribir en el FIFO (RPCS3: "FIFO error: possible
  desync event" y "Dead FIFO commands queue"); además su `count` son floats,
  no vec4, y en el bucle de 32 floats copia dos veces el mismo bloque. Usar
  `rsxLoadVertexProgramParameterBlock(ctx, base, n_vec4, datos)`, que parte en
  bloques de 8 vec4 bien.
- `rsxLoadVertexProgram` sube también las constantes internas que pone
  `cgcomp` (literales como 2.0): no hace falta subirlas a mano.
- `cgcomp` asigna las constantes del vertex program en orden de declaración
  y consecutivas (un `uniform float4 k[15]` queda como `k[0]`…`k[14]`), así
  que se pueden subir de una vez.

## Mezcla alfa dentro de una sola llamada de dibujo (RPCS3, 2026-10-10)

En RPCS3 (0.0.43, renderizador de la Mac), los triángulos translúcidos de
**una misma** `rsxDrawVertexArray` no se mezclan con lo que dibujaron los
triángulos anteriores de esa llamada: el destino que ven es el de antes de
la llamada. Con el menú en un solo lote (fondo opaco y encima barras,
paneles, ondas y sombras con alfa) todo lo translúcido salía como fuente ×
alfa sobre negro (panel (0, 0, 0), sombras como cajas negras, ondas
oscuras); con el fondo en una llamada aparte, la mezcla sobre él salía bien.
Entre llamadas distintas el orden se respeta (gfxtest comprueba una mezcla
al 50 % y pasa).

El RSX real mezcla en el orden de las primitivas, como cualquier GPU; esto
es de RPCS3. Para que se vea igual en los dos, `lib/rsxkit/ui.h` manda una
llamada de dibujo por elemento (rectángulo, texto, láminas y líneas de cada
onda: `rsxkit_ui_groups`); con el estado en caché, cada llamada son unas
pocas palabras del búfer de comandos. Quien junte en un solo lote
geometría translúcida que se superpone debería hacer lo mismo si quiere
verla bien en RPCS3.

## Cobertura con AA_EN: POLYGON_SMOOTH no sirve (2026-10-12, PS3 real)

Con AA_EN el RDP escribe un píxel si cualquiera de sus 8 muestras de
cobertura cae dentro del triángulo; el RSX solo mira el centro. Se probó
`POLYGON_SMOOTH_ENABLE` (método 0x1838 del NV40,
`rsxSetPolygonSmoothEnable`) para que el RSX generara fragmentos también en
los píxeles cubiertos en parte: en la PS3 real no cambia nada (el volcado de
SELECT + R2 en el túnel de Dam traía la marca en 100 de 109 dibujos y la
captura del RSX seguía con las grietas). RPCS3 también lo ignora. La
cobertura se emula en la HLE, ensanchando los triángulos (`aa_grow` en
src/rsp/gfx/rdp.c), que además sirve a cualquier escala interna.
