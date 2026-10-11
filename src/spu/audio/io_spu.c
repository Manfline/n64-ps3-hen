/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — E/S de la HLE de audio en el SPE: la RDRAM se lee y escribe
 * por DMA.
 *
 *   lista     bloques de 1 KB alineados, uno a la vez (una tarea de audio
 *             tiene unos pocos KB de comandos)
 *   lecturas  el tramo alineado a 16 que contiene lo pedido, a un búfer de
 *             4 KB + 32 (spukit_get_unaligned), y de ahí a su destino
 *   escrituras se copian a un búfer cuyo desplazamiento dentro de 16 bytes
 *             es el mismo que el de la dirección de destino y salen en
 *             trozos de alineación natural (1, 2, 4, 8 o múltiplos de 16):
 *             nunca se lee y reescribe un tramo mayor, porque el CPU
 *             emulado puede estar escribiendo la RDRAM vecina al mismo
 *             tiempo. Se espera cada escritura antes de seguir: una lectura
 *             posterior de la misma dirección ve el dato nuevo.
 *
 * Etiquetas DMA: 0 lecturas, 1 escrituras, 2 lista. La 31 es de spukit.
 */
#include <spukit/spu.h>
#include <string.h>

#include "core/endian.h"
#include "rsp/audio/hle.h"

#define TAG_READ  0
#define TAG_WRITE 1
#define TAG_LIST  2

#define IO_MAX     4096u  /* bytes por transferencia (LOADBUFF/SAVEBUFF ≤ 4 KB) */
#define LIST_BLOCK 1024u

static uint8_t rd_buf[IO_MAX + 32] __attribute__((aligned(128)));
static uint8_t wr_buf[IO_MAX + 32] __attribute__((aligned(128)));
static uint8_t list_buf[LIST_BLOCK] __attribute__((aligned(128)));
static uint32_t list_addr;  /* bloque cargado en list_buf (~0: ninguno) */

uint32_t aud_io_clock(void)
{
	return ~spu_read_decrementer();
}

void aud_io_begin(struct aud_state *st)
{
	(void)st;
	list_addr = ~0u;
}

void aud_io_end(struct aud_state *st)
{
	(void)st;
	spukit_dma_wait(1u << TAG_WRITE);
}

static bool in_rdram(const struct aud_state *st, uint32_t addr, uint32_t size)
{
	return addr < st->job->rdram_size && size <= st->job->rdram_size - addr;
}

bool aud_io_read(struct aud_state *st, void *dst, uint32_t addr, uint32_t size)
{
	if (!in_rdram(st, addr, size))
		return false;
	uint8_t *d = dst;
	while (size) {
		uint32_t n = size < IO_MAX ? size : IO_MAX;
		const uint8_t *p = spukit_get_unaligned(rd_buf, st->job->rdram_ea + addr, n, TAG_READ);
		memcpy(d, p, n);
		d += n;
		addr += n;
		size -= n;
	}
	return true;
}

bool aud_io_write(struct aud_state *st, uint32_t addr, const void *src, uint32_t size)
{
	if (!in_rdram(st, addr, size))
		return false;
	const uint8_t *s = src;
	while (size) {
		uint32_t n = size < IO_MAX ? size : IO_MAX;
		uint64_t ea = st->job->rdram_ea + addr;
		uint8_t *ls = wr_buf + (ea & 15);
		memcpy(ls, s, n);
		for (uint32_t left = n; left;) {
			uint32_t k;
			if ((ea & 15) == 0 && left >= 16)
				k = left & ~15u;
			else if ((ea & 7) == 0 && left >= 8)
				k = 8;
			else if ((ea & 3) == 0 && left >= 4)
				k = 4;
			else if ((ea & 1) == 0 && left >= 2)
				k = 2;
			else
				k = 1;
			mfc_put((volatile void *)ls, ea, k, TAG_WRITE, 0, 0);
			ls += k;
			ea += k;
			left -= k;
		}
		spukit_dma_wait(1u << TAG_WRITE);
		s += n;
		addr += n;
		size -= n;
	}
	return true;
}

bool aud_io_alist(struct aud_state *st, uint32_t index, uint32_t *w1, uint32_t *w2)
{
	uint32_t addr = (st->job->alist & 0xFFFFF8) + 8 * index;
	if (!in_rdram(st, addr, 8))
		return false;
	uint32_t block = addr & ~(LIST_BLOCK - 1);
	if (block != list_addr) {
		uint32_t n = st->job->rdram_size - block < LIST_BLOCK ? st->job->rdram_size - block : LIST_BLOCK;
		spukit_dma_get(list_buf, st->job->rdram_ea + block, n, TAG_LIST);
		spukit_dma_wait(1u << TAG_LIST);
		list_addr = block;
	}
	const uint8_t *p = list_buf + (addr - block);
	*w1 = salsa64_rd32(p);
	*w2 = salsa64_rd32(p + 4);
	return true;
}
