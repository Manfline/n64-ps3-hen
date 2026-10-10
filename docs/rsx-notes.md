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
