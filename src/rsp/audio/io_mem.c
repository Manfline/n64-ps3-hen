/*
 * Salsa64 — E/S de la HLE de audio con memoria directa (host y PPE).
 *
 * La RDRAM se lee y escribe en su lugar (job->rdram_ea es un puntero del
 * mismo proceso). El SPE tiene su propia versión con DMA
 * (src/spu/audio/io_spu.c).
 */
#include <string.h>

#include "rsp/audio/hle.h"

#define PTR(ea) ((uint8_t *)(uintptr_t)(ea))

uint32_t aud_io_clock(void)
{
	return 0;
}

void aud_io_begin(struct aud_state *st)
{
	(void)st;
}

void aud_io_end(struct aud_state *st)
{
	(void)st;
}

static bool in_rdram(const struct aud_state *st, uint32_t addr, uint32_t size)
{
	return addr < st->job->rdram_size && size <= st->job->rdram_size - addr;
}

bool aud_io_read(struct aud_state *st, void *dst, uint32_t addr, uint32_t size)
{
	if (!in_rdram(st, addr, size))
		return false;
	memcpy(dst, PTR(st->job->rdram_ea) + addr, size);
	return true;
}

bool aud_io_write(struct aud_state *st, uint32_t addr, const void *src, uint32_t size)
{
	if (!in_rdram(st, addr, size))
		return false;
	memcpy(PTR(st->job->rdram_ea) + addr, src, size);
	return true;
}

bool aud_io_alist(struct aud_state *st, uint32_t index, uint32_t *w1, uint32_t *w2)
{
	uint32_t addr = (st->job->alist & 0xFFFFF8) + 8 * index;
	if (!in_rdram(st, addr, 8))
		return false;
	const uint8_t *p = PTR(st->job->rdram_ea) + addr;
	*w1 = salsa64_rd32(p);
	*w2 = salsa64_rd32(p + 4);
	return true;
}
