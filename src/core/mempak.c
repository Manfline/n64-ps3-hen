/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — Controller Pak. Ver mempak.h.
 */
#include "core/mempak.h"

#include <string.h>

#define ID_COPIES     4
#define ID_LEN        32
#define ID_SERIAL_LEN 24
#define ID_BLANK      0x18  /* byte sin uso, antes del ID de dispositivo */
#define ID_DEVICE     0x19
#define ID_BANKS      0x1A
#define ID_SUM1       0x1C
#define ID_SUM2       0x1E
#define ID_SUM_BASE   0xFFF2u
#define FAT_SUM       1     /* byte con la suma de la tabla */
#define FAT_FIRST     5     /* primera página de datos */
#define FAT_PAGES     128
#define FAT_FREE      3
#define MPK_ADDR_MASK 0xFFE0u
#define MPK_LIMIT     0x8000u

static const uint16_t id_at[ID_COPIES] = { 0x20, 0x60, 0x80, 0xC0 };

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

/* Valor que aporta a la verificación cada bit de la dirección, del 15 al 5. */
static const uint8_t addr_bit_crc[11] = { 0x01, 0x1A, 0x0D, 0x1C, 0x0E, 0x07, 0x19, 0x16, 0x0B, 0x1F, 0x15 };

uint8_t salsa64_mempak_addr_crc(uint16_t addr)
{
	uint8_t crc = 0;
	for (unsigned i = 0; i < 11; i++)
		if (addr & (0x8000u >> i))
			crc ^= addr_bit_crc[i];
	return crc;
}

bool salsa64_mempak_addr_ok(uint16_t raw)
{
	return (raw & 0x1F) == salsa64_mempak_addr_crc(raw);
}

static uint8_t table_sum(const uint8_t *page)
{
	uint8_t sum = 0;
	for (unsigned i = 2; i < SALSA64_MEMPAK_PAGE; i++)
		sum = (uint8_t)(sum + page[i]);
	return sum;
}

static void id_sums(uint8_t *id)
{
	uint16_t sum = 0;
	for (unsigned i = 0; i < ID_SUM1; i += 2)
		sum = (uint16_t)(sum + be16(id + i));
	put16(id + ID_SUM1, sum);
	put16(id + ID_SUM2, (uint16_t)(ID_SUM_BASE - sum));
}

void salsa64_mempak_format(uint8_t *pak, uint32_t seed)
{
	memset(pak, 0, SALSA64_MEMPAK_SIZE);
	/* Serie: bytes de un xorshift de 32 bits; solo tiene que diferir de un
	 * pak a otro y ser siempre el mismo para el mismo `seed`. */
	uint32_t x = seed * 2654435761u + 0x9E3779B9u;
	uint8_t id[ID_LEN] = { 0 };
	for (unsigned i = 0; i < ID_SERIAL_LEN; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		id[i] = (uint8_t)(x >> 11);
	}
	id[ID_DEVICE] = 0x01;
	id[ID_BANKS] = 0x01;
	id_sums(id);
	for (unsigned k = 0; k < ID_COPIES; k++)
		memcpy(pak + id_at[k], id, ID_LEN);
	uint8_t *fat = pak + SALSA64_MEMPAK_PAGE;
	for (unsigned p = FAT_FIRST; p < FAT_PAGES; p++)
		put16(fat + 2 * p, FAT_FREE);
	fat[FAT_SUM] = table_sum(fat);
	memcpy(pak + 2 * SALSA64_MEMPAK_PAGE, fat, SALSA64_MEMPAK_PAGE);
}

bool salsa64_mempak_valid(const uint8_t *pak)
{
	for (unsigned k = 0; k < ID_COPIES; k++) {
		uint8_t id[ID_LEN];
		memcpy(id, pak + id_at[k], ID_LEN);
		id_sums(id);
		if (memcmp(id, pak + id_at[k], ID_LEN) != 0)
			return false;
	}
	for (unsigned t = 1; t <= 2; t++) {
		const uint8_t *fat = pak + t * SALSA64_MEMPAK_PAGE;
		if (fat[FAT_SUM] != table_sum(fat))
			return false;
	}
	return true;
}

void salsa64_mempak_init(struct salsa64_mempak *mp)
{
	memset(mp->present, 0, sizeof mp->present);
	mp->writes = mp->flushed = mp->seen = 0;
	mp->seen_us = mp->dirty_us = 0;
	for (unsigned p = 0; p < SALSA64_MEMPAK_PORTS; p++)
		salsa64_mempak_format(mp->data[p], p + 1);
}

static bool uniform(const uint8_t *pak)
{
	for (unsigned i = 1; i < SALSA64_MEMPAK_SIZE; i++)
		if (pak[i] != pak[0])
			return false;
	return pak[0] == 0x00 || pak[0] == 0xFF;
}

bool salsa64_mempak_load(struct salsa64_mempak *mp, const uint8_t *data, uint32_t len)
{
	memcpy(mp->data, data, len < SALSA64_MEMPAK_TOTAL ? len : SALSA64_MEMPAK_TOTAL);
	for (unsigned p = 0; p < SALSA64_MEMPAK_PORTS; p++)
		if (uniform(mp->data[p]))
			salsa64_mempak_format(mp->data[p], p + 1);
	return len == SALSA64_MEMPAK_TOTAL;
}

bool salsa64_mempak_read(const struct salsa64_mempak *mp, unsigned port, uint16_t raw, uint8_t *out)
{
	memset(out, 0, SALSA64_MEMPAK_BLOCK);
	if (port >= SALSA64_MEMPAK_PORTS || !salsa64_mempak_addr_ok(raw))
		return false;
	uint16_t addr = raw & MPK_ADDR_MASK;
	if (addr < MPK_LIMIT)
		memcpy(out, mp->data[port] + addr, SALSA64_MEMPAK_BLOCK);
	return true;
}

bool salsa64_mempak_write(struct salsa64_mempak *mp, unsigned port, uint16_t raw, const uint8_t *in)
{
	if (port >= SALSA64_MEMPAK_PORTS || !salsa64_mempak_addr_ok(raw))
		return false;
	uint16_t addr = raw & MPK_ADDR_MASK;
	if (addr < MPK_LIMIT && memcmp(mp->data[port] + addr, in, SALSA64_MEMPAK_BLOCK) != 0) {
		memcpy(mp->data[port] + addr, in, SALSA64_MEMPAK_BLOCK);
		mp->writes++;
	}
	return true;
}

bool salsa64_mempak_dirty(const struct salsa64_mempak *mp)
{
	return mp->writes != mp->flushed;
}

bool salsa64_mempak_due(struct salsa64_mempak *mp, uint64_t now_us)
{
	if (mp->writes != mp->seen) {
		if (mp->seen == mp->flushed)
			mp->dirty_us = now_us;
		mp->seen = mp->writes;
		mp->seen_us = now_us;
	}
	if (!salsa64_mempak_dirty(mp))
		return false;
	return now_us - mp->seen_us >= SALSA64_MEMPAK_QUIET_US ||
	       now_us - mp->dirty_us >= SALSA64_MEMPAK_MAX_DELAY_US;
}

void salsa64_mempak_flushed(struct salsa64_mempak *mp)
{
	mp->flushed = mp->seen = mp->writes;
}
