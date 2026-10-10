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
