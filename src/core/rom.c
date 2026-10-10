/*
 * Salsa64 — imagen del cartucho. Ver rom.h.
 *
 * Fuentes:
 *   - n64brew.dev, "ROM Header": firmas y código de destino.
 *   - n64brew.dev, "CIC-NUS" y "PIF-NUS": variantes y semillas del IPL3.
 *   - nust64 0.4.1 (MIT, src/rom.rs): CRC-32 del IPL3 (bytes 0x40–0xFFF) de
 *     cada variante. Son solo los valores del CRC, no el código.
 */
#include "core/rom.h"

#include "core/endian.h"

enum salsa64_rom_format salsa64_rom_format(const uint8_t *rom, size_t size)
{
	if (size < 4)
		return SALSA64_ROM_Z64;
	uint32_t magic = salsa64_rd32(rom);
	if (magic == 0x37804012u)
		return SALSA64_ROM_V64;
	if (magic == 0x40123780u)
		return SALSA64_ROM_N64;
	return SALSA64_ROM_Z64;
}

enum salsa64_rom_format salsa64_rom_normalize(uint8_t *rom, size_t size)
{
	enum salsa64_rom_format fmt = salsa64_rom_format(rom, size);
	if (fmt == SALSA64_ROM_V64) {
		for (size_t i = 0; i + 1 < size; i += 2) {
			uint8_t t = rom[i];
			rom[i] = rom[i + 1];
			rom[i + 1] = t;
		}
	} else if (fmt == SALSA64_ROM_N64) {
		for (size_t i = 0; i + 3 < size; i += 4) {
			uint8_t a = rom[i], b = rom[i + 1];
			rom[i] = rom[i + 3];
			rom[i + 1] = rom[i + 2];
			rom[i + 2] = b;
			rom[i + 3] = a;
		}
	}
	return fmt;
}

uint32_t salsa64_crc32(const uint8_t *data, size_t len)
{
	/* Bit a bit: solo se usa al arrancar, sobre 4 KB. */
	uint32_t crc = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

enum salsa64_cic salsa64_rom_cic(const uint8_t *rom)
{
	switch (salsa64_crc32(rom + SALSA64_ROM_HEADER_SIZE,
			      SALSA64_ROM_IPL3_END - SALSA64_ROM_HEADER_SIZE)) {
	case 0x6170A4A1u: return SALSA64_CIC_6101;
	case 0x90BB6CB5u: return SALSA64_CIC_6102;
	case 0x009E9EA3u: return SALSA64_CIC_7102;
	case 0x0B050EE0u: return SALSA64_CIC_6103;
	case 0x98BC2C86u: return SALSA64_CIC_6105;
	case 0xACC8580Au: return SALSA64_CIC_6106;
	default:          return SALSA64_CIC_UNKNOWN;
	}
}

enum salsa64_tv_type salsa64_rom_tv_type(const uint8_t *rom)
{
	switch (rom[0x3E]) {
	case 'D': case 'F': case 'H': case 'I': case 'L': case 'P':
	case 'S': case 'U': case 'W': case 'X': case 'Y': case 'Z':
		return SALSA64_TV_PAL;
	case 'B':
		return SALSA64_TV_MPAL;  /* Brasil: PAL-M */
	default:
		return SALSA64_TV_NTSC;
	}
}

uint8_t salsa64_cic_seed(enum salsa64_cic cic)
{
	switch (cic) {
	case SALSA64_CIC_6103: return 0x78;
	case SALSA64_CIC_6105: return 0x91;
	case SALSA64_CIC_6106: return 0x85;
	default:               return 0x3F;  /* 6101, 6102/7101, 7102 */
	}
}

const char *salsa64_cic_name(enum salsa64_cic cic)
{
	switch (cic) {
	case SALSA64_CIC_6101: return "6101";
	case SALSA64_CIC_6102: return "6102/7101";
	case SALSA64_CIC_7102: return "7102";
	case SALSA64_CIC_6103: return "6103/7103";
	case SALSA64_CIC_6105: return "6105/7105";
	case SALSA64_CIC_6106: return "6106/7106";
	default:               return "desconocido";
	}
}
