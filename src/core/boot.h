/*
 * Retro64 — arranque HLE: se salta la PIF ROM (IPL1/IPL2) y el IPL3 del
 * cartucho, y deja la máquina como la dejaría el IPL3 al saltar al programa.
 *
 * Dos caminos, según el IPL3 que trae la ROM:
 *
 *   ELF  IPL3 libre de libdragon (nust64, libdragon): busca un ELF
 *        alineado a 256 bytes desde 0x1000, carga sus segmentos PT_LOAD y
 *        pasa los "boot flags" en DMEM. Es lo que usa nemu64-test.
 *   FLAT IPL3 de Nintendo (reconocido por su CRC), el "compat" de libdragon
 *        o cualquier otro: copia 1 MB de la ROM desde 0x1000 a la dirección
 *        de arranque del encabezado y deja las variables de libultra en
 *        0x80000300.
 *
 * El detalle de qué se deja y de dónde sale está en docs/cpu-vr4300.md
 * ("Arranque HLE").
 */
#ifndef RETRO64_CORE_BOOT_H
#define RETRO64_CORE_BOOT_H

#include "core/n64.h"
#include "core/rom.h"

enum retro64_boot_kind {
	RETRO64_BOOT_FLAT,
	RETRO64_BOOT_ELF,
};

struct retro64_boot_info {
	enum retro64_boot_kind kind;
	enum retro64_cic cic;
	enum retro64_tv_type tv;
	uint32_t entry;       /* PC inicial (dirección virtual de 32 bits) */
	uint32_t elf_offset;  /* ELF: posición del encabezado en la ROM */
	uint32_t loaded;      /* bytes copiados de la ROM a la RDRAM */
	const char *error;    /* motivo si retro64_boot_hle devuelve false */
};

/* Valor de `tv` para usar el que dice el encabezado. */
#define RETRO64_BOOT_TV_AUTO (-1)

/*
 * Arranca la ROM ya puesta con retro64_bus_set_rom (formato .z64). Hace un
 * reset en frío de la máquina antes de cargar. `tv` es un
 * enum retro64_tv_type o RETRO64_BOOT_TV_AUTO. Devuelve false (con
 * info->error) si la ROM no se puede arrancar; info puede ser NULL.
 */
bool retro64_boot_hle(struct retro64_n64 *n64, int tv, struct retro64_boot_info *info);

#endif
