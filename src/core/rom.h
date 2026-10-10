/*
 * Retro64 — imagen del cartucho: orden de bytes, encabezado y variante de CIC.
 *
 * El núcleo trabaja siempre con la ROM en el orden de la consola (.z64,
 * big-endian). Los volcados .v64 (pares de bytes intercambiados) y .n64
 * (palabras de 32 bits invertidas) se normalizan una sola vez al cargarlos;
 * se detectan por los 4 primeros bytes. n64brew.dev (ROM Header) pide
 * aceptar cualquier valor en esos bytes, así que lo que no es la firma de
 * .v64 o .n64 se toma como .z64.
 *
 * Nada de esto guarda código de Nintendo: la variante de CIC se reconoce por
 * el CRC-32 del IPL3 que trae la ROM del usuario.
 */
#ifndef RETRO64_CORE_ROM_H
#define RETRO64_CORE_ROM_H

#include "core/types.h"

#define RETRO64_ROM_HEADER_SIZE 0x40u
#define RETRO64_ROM_IPL3_END    0x1000u  /* el IPL3 ocupa 0x40–0xFFF */

enum retro64_rom_format {
	RETRO64_ROM_Z64,  /* big-endian, el de la consola */
	RETRO64_ROM_V64,  /* bytes intercambiados de dos en dos */
	RETRO64_ROM_N64,  /* palabras de 32 bits invertidas */
};

/* Valores del IPL2 y de osTvType. */
enum retro64_tv_type {
	RETRO64_TV_PAL = 0,
	RETRO64_TV_NTSC = 1,
	RETRO64_TV_MPAL = 2,
};

/* Variantes del IPL3 de Nintendo (n64brew.dev: CIC-NUS). */
enum retro64_cic {
	RETRO64_CIC_UNKNOWN,  /* IPL3 libre (libdragon), ceros u otro */
	RETRO64_CIC_6101,
	RETRO64_CIC_6102,     /* también 7101 */
	RETRO64_CIC_7102,
	RETRO64_CIC_6103,     /* también 7103 */
	RETRO64_CIC_6105,     /* también 7105 */
	RETRO64_CIC_6106,     /* también 7106 */
};

enum retro64_rom_format retro64_rom_format(const uint8_t *rom, size_t size);
/* Convierte a .z64 en el mismo búfer y devuelve el formato original. */
enum retro64_rom_format retro64_rom_normalize(uint8_t *rom, size_t size);

/* CRC-32 (ISO-HDLC, el de zlib). */
uint32_t retro64_crc32(const uint8_t *data, size_t len);

/* Requieren una ROM .z64 de al menos RETRO64_ROM_IPL3_END bytes. */
enum retro64_cic retro64_rom_cic(const uint8_t *rom);
/* Según el código de destino del encabezado (byte 0x3E); NTSC si no se sabe. */
enum retro64_tv_type retro64_rom_tv_type(const uint8_t *rom);
/* Semilla del IPL3 que el IPL2 deja en s6 (n64brew.dev: PIF-NUS). */
uint8_t retro64_cic_seed(enum retro64_cic cic);
const char *retro64_cic_name(enum retro64_cic cic);

#endif
