# Estado

## 2026-10-10 — Fase 0 en curso

Hecho:
- Imagen `cell64-toolchain:dev` (ver docs/toolchain.md): ps3toolchain (GCC 13.2
  PPU, 9.5 SPU) + PSL1GHT + cgcomp con Cg Toolkit vía qemu. Los 54 shaders de
  la rama `ps3` del port de SM64 compilan; 18/31 samples de PSL1GHT compilan
  (las fallas son de los samples).
- Programas de la Fase 0, compilados sin advertencias (`tools/build.sh`):
  - `tests/phase0/rsx-triangle`: triángulo + verificación de píxeles.
  - `tests/phase0/spu-compute`: 6 SPUs, DMA de doble búfer, verificación exacta.
  - `tests/phase0/jit-ppe`: 8 mecanismos de memoria, tolera caídas.
  - `tests/phase0/udplog`: 200 líneas numeradas por UDP.
- Logger TTY + UDP (`src/platform/ps3/log.c`) y receptor `tools/udplog.py`.

Falta para cerrar la Fase 0:
- [ ] RPCS3 en la Mac + firmware de PS3 instalado por el autor; correr los 4
      programas y automatizarlo (`tools/rpcs3-run.sh`, por escribir).
- [ ] PS3 real: correr los 4 programas (rsx-triangle, spu-compute, udplog,
      jit-ppe con HEN y, si se puede, CFW). Llenar docs/jit-ppe.md.
- [ ] Decidir el plan de la Fase 3 según el resultado del JIT.

Notas:
- No usar `-Os` en el PPU: ICE de GCC 13.2 (docs/toolchain.md).
- En RPCS3, rsx-triangle necesita "Write Color Buffers" para su verificación.
