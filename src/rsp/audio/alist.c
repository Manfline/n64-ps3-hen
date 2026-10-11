/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - alist.c                                         *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Salsa64 — operaciones comunes de las listas de audio. Adaptado de
 * src/alist.c y src/audio.c de mupen64plus-rsp-hle (commit 8a7a472a,
 * 2026-06-23) para Salsa64 (2026-10-10):
 *
 *   - La DMEM y la RDRAM se leen en el orden de bytes de la N64 con índices
 *     de la N64 (el original usa palabras del host con XOR en los índices,
 *     y FILTER e INTERLEAVE dependen de que el host sea little-endian).
 *   - La RDRAM no se toca directo: aud_load/aud_store (E/S por DMA en el
 *     SPE, memcpy en el host) y cada escritura se anota para el
 *     recompilador.
 *   - El formato del estado de ENVMIXER en la RDRAM es propio (solo lo lee
 *     la HLE); el de ADPCM, RESAMPLE, POLEF y FILTER es el de la N64.
 *   - La tabla de RESAMPLE es propia (lut.c), no la del microcódigo.
 *   - Las direcciones de la DMEM se toman módulo 4 KB.
 */
#include <string.h>

#include "rsp/audio/hle.h"

struct ramp {
	int64_t value;
	int64_t step;
	int64_t target;
};

static int16_t ramp_step(struct ramp *r)
{
	r->value += r->step;
	bool reached = r->step <= 0 ? r->value <= r->target : r->value >= r->target;
	if (reached) {
		r->value = r->target;
		r->step = 0;
	}
	return (int16_t)(r->value >> 16);
}

static void sample_mix(uint8_t *dst, int16_t src, int16_t gain)
{
	aud_st16(dst, aud_clamp16(aud_ld16(dst) + ((src * gain) >> 15)));
}

/* ---- RDRAM ------------------------------------------------------------------ */

static void note_range(struct aud_result *r, uint32_t addr, uint32_t len)
{
	uint32_t end = addr + len;
	for (uint32_t i = 0; i < r->nranges; i++) {
		uint32_t a = r->range_addr[i], e = a + r->range_len[i];
		if (addr <= e && end >= a) {  /* se toca o se pisa: juntar */
			a = addr < a ? addr : a;
			e = end > e ? end : e;
			r->range_addr[i] = a;
			r->range_len[i] = e - a;
			return;
		}
	}
	if (r->nranges < AUD_MAX_RANGES) {
		r->range_addr[r->nranges] = addr;
		r->range_len[r->nranges++] = len;
		return;
	}
	/* Sin lugar: el último tramo crece hasta cubrir este. */
	uint32_t i = AUD_MAX_RANGES - 1, a = r->range_addr[i], e = a + r->range_len[i];
	a = addr < a ? addr : a;
	e = end > e ? end : e;
	r->range_addr[i] = a;
	r->range_len[i] = e - a;
}

bool aud_load(struct aud_state *st, void *dst, uint32_t addr, uint32_t size)
{
	addr &= 0xFFFFFF;
	if (aud_io_read(st, dst, addr, size))
		return true;
	memset(dst, 0, size);
	st->res->errors++;
	return false;
}

bool aud_store(struct aud_state *st, uint32_t addr, const void *src, uint32_t size)
{
	addr &= 0xFFFFFF;
	if (!size)
		return true;
	if (!aud_io_write(st, addr, src, size)) {
		st->res->errors++;
		return false;
	}
	note_range(st->res, addr, size);
	return true;
}

bool aud_load_s16(struct aud_state *st, int16_t *dst, uint32_t addr, uint32_t n)
{
	uint8_t buf[64];
	bool ok = true;
	while (n) {
		uint32_t k = n < 32 ? n : 32;
		ok &= aud_load(st, buf, addr, 2 * k);
		for (uint32_t i = 0; i < k; i++)
			*dst++ = aud_ld16(buf + 2 * i);
		addr += 2 * k;
		n -= k;
	}
	return ok;
}

bool aud_store_s16(struct aud_state *st, uint32_t addr, const int16_t *src, uint32_t n)
{
	uint8_t buf[64];
	bool ok = true;
	while (n) {
		uint32_t k = n < 32 ? n : 32;
		for (uint32_t i = 0; i < k; i++)
			aud_st16(buf + 2 * i, *src++);
		ok &= aud_store(st, addr, buf, 2 * k);
		addr += 2 * k;
		n -= k;
	}
	return ok;
}

/* ---- Movimientos ------------------------------------------------------------ */

void alist_clear(struct aud_state *st, uint16_t dmem, uint16_t count)
{
	memset(aud_dm(st, dmem), 0, count);
}

void alist_load(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count)
{
	/* Las restricciones del DMA del RSP. */
	dmem &= ~3;
	address &= ~7u;
	aud_load(st, aud_dm(st, dmem), address, aud_align(count, 8));
}

void alist_save(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count)
{
	dmem &= ~3;
	address &= ~7u;
	aud_store(st, address, aud_dm(st, dmem), aud_align(count, 8));
}

void alist_move(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
	/* Byte a byte y hacia adelante: un tramo que se pisa repite el patrón. */
	while (count--)
		*aud_dm(st, dmemo++) = *aud_dm(st, dmemi++);
}

void alist_copy_every_other_sample(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
	while (count--) {
		aud_st16(aud_dm(st, dmemo), aud_ld16(aud_dm(st, dmemi)));
		dmemo += 2;
		dmemi += 4;
	}
}

void alist_repeat64(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint8_t count)
{
	uint8_t buf[128];
	memcpy(buf, aud_dm(st, dmemi), sizeof buf);
	while (count--) {
		memcpy(aud_dm(st, dmemo), buf, sizeof buf);
		dmemo += 128;
	}
}

void alist_copy_blocks(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t block_size, uint8_t count)
{
	int blocks = count;
	do {
		int left = block_size;
		do {
			memmove(aud_dm(st, dmemo), aud_dm(st, dmemi), 0x20);
			left -= 0x20;
			dmemi += 0x20;
			dmemo += 0x20;
		} while (left > 0);
	} while (--blocks > 0);
}

void alist_interleave(struct aud_state *st, uint16_t dmemo, uint16_t left, uint16_t right, uint16_t count)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *l = aud_dm(st, left), *r = aud_dm(st, right);
	for (count >>= 2; count; count--) {
		int16_t l1 = aud_ld16(l), l2 = aud_ld16(l + 2);
		int16_t r1 = aud_ld16(r), r2 = aud_ld16(r + 2);
		aud_st16(dst, l1);
		aud_st16(dst + 2, r1);
		aud_st16(dst + 4, l2);
		aud_st16(dst + 6, r2);
		dst += 8;
		l += 4;
		r += 4;
	}
}

/* ---- Envolventes ------------------------------------------------------------
 *
 * Estado de ENVMIXER en la RDRAM (80 bytes reservados por el juego; se usan
 * 40), en el orden de la N64:
 *   0 wet  4 dry  8/12 target  16/20 rate o paso  24/28 seq  32/36 valor */

#define ENV_STATE 40

static int32_t env_rd32(const uint8_t *s, unsigned off)
{
	return (int32_t)salsa64_rd32(s + off);
}

static void env_wr32(uint8_t *s, unsigned off, int32_t v)
{
	salsa64_wr32(s + off, (uint32_t)v);
}

void alist_envmix(struct aud_state *st, enum aud_envmix_kind kind, bool init, bool aux,
		  uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi,
		  uint16_t count, int16_t dry, int16_t wet, const int16_t *vol, const int16_t *target,
		  const int32_t *rate, uint32_t address)
{
	/* El lineal de naudio siempre mezcla en los cuatro búferes. */
	unsigned n = kind == AUD_ENVMIX_LIN || aux ? 4 : 2;
	const uint8_t *in = aud_dm(st, dmemi);
	uint8_t *buf[4] = { aud_dm(st, dmem_dl), aud_dm(st, dmem_dr), aud_dm(st, dmem_wl), aud_dm(st, dmem_wr) };
	struct ramp ramps[2];
	int32_t exp_seq[2] = { 0, 0 }, exp_rates[2] = { 0, 0 };
	uint8_t s[ENV_STATE];

	if (init) {
		for (int k = 0; k < 2; k++) {
			ramps[k].value = (int64_t)vol[k] * 65536;
			ramps[k].target = (int64_t)target[k] * 65536;
			if (kind == AUD_ENVMIX_EXP) {
				exp_rates[k] = rate[k];
				exp_seq[k] = (int32_t)((int64_t)vol[k] * rate[k]);
			} else {
				ramps[k].step = rate[k] / 8;
			}
		}
	} else {
		aud_load(st, s, address, sizeof s);
		wet = (int16_t)salsa64_rd16(s + 0);
		dry = (int16_t)salsa64_rd16(s + 4);
		for (int k = 0; k < 2; k++) {
			ramps[k].target = env_rd32(s, 8 + 4 * k);
			if (kind == AUD_ENVMIX_EXP) {
				exp_rates[k] = env_rd32(s, 16 + 4 * k);
				exp_seq[k] = env_rd32(s, 24 + 4 * k);
			} else {
				ramps[k].step = env_rd32(s, 16 + 4 * k);
			}
			ramps[k].value = env_rd32(s, 32 + 4 * k);
		}
	}

	if (kind == AUD_ENVMIX_EXP) {
		/* step != 0 si y solo si value != target */
		ramps[0].step = ramps[0].target - ramps[0].value;
		ramps[1].step = ramps[1].target - ramps[1].value;
	}

	unsigned total = kind == AUD_ENVMIX_EXP ? (count + 15u) / 16u * 8u : count >> 1;
	for (unsigned k = 0; k < total; k++) {
		if (kind == AUD_ENVMIX_EXP && (k & 7) == 0) {
			for (int c = 0; c < 2; c++)
				if (ramps[c].step != 0) {
					exp_seq[c] = (int32_t)(((int64_t)exp_seq[c] * exp_rates[c]) >> 16);
					ramps[c].step = (exp_seq[c] - ramps[c].value) >> 3;
				}
		}
		int16_t l_vol = ramp_step(&ramps[0]);
		int16_t r_vol = ramp_step(&ramps[1]);
		int16_t gains[4] = {
			aud_clamp16((l_vol * dry + 0x4000) >> 15),
			aud_clamp16((r_vol * dry + 0x4000) >> 15),
			aud_clamp16((l_vol * wet + 0x4000) >> 15),
			aud_clamp16((r_vol * wet + 0x4000) >> 15),
		};
		int16_t x = aud_ld16(in + 2 * k);
		for (unsigned b = 0; b < n; b++)
			sample_mix(buf[b] + 2 * k, x, gains[b]);
	}

	memset(s, 0, sizeof s);
	salsa64_wr16(s + 0, (uint16_t)wet);
	salsa64_wr16(s + 4, (uint16_t)dry);
	for (int k = 0; k < 2; k++) {
		env_wr32(s, 8 + 4 * k, (int32_t)ramps[k].target);
		if (kind == AUD_ENVMIX_EXP) {
			env_wr32(s, 16 + 4 * k, exp_rates[k]);
			env_wr32(s, 24 + 4 * k, exp_seq[k]);
		} else {
			env_wr32(s, 16 + 4 * k, (int32_t)ramps[k].step);
		}
		env_wr32(s, 32 + 4 * k, (int32_t)ramps[k].value);
	}
	aud_store(st, address, s, sizeof s);
}

void alist_envmix_nead(struct aud_state *st, bool swap_wet_lr, uint16_t dmem_dl, uint16_t dmem_dr,
		       uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi, unsigned count,
		       uint16_t *env_values, const uint16_t *env_steps, const int16_t *xors)
{
	const uint8_t *in = aud_dm(st, dmemi);
	uint8_t *dl = aud_dm(st, dmem_dl), *dr = aud_dm(st, dmem_dr);
	uint8_t *wl = aud_dm(st, dmem_wl), *wr = aud_dm(st, dmem_wr);
	if (swap_wet_lr) {
		uint8_t *t = wl;
		wl = wr;
		wr = t;
	}
	for (count = aud_align(count, 8); count; count -= 8) {
		for (unsigned i = 0; i < 8; i++) {
			int32_t x = aud_ld16(in + 2 * i);
			int16_t l = (int16_t)((x * (int32_t)env_values[0]) >> 16) ^ xors[0];
			int16_t r = (int16_t)((x * (int32_t)env_values[1]) >> 16) ^ xors[1];
			int16_t l2 = (int16_t)(((int32_t)l * (int32_t)env_values[2]) >> 16) ^ xors[2];
			int16_t r2 = (int16_t)(((int32_t)r * (int32_t)env_values[2]) >> 16) ^ xors[3];
			aud_st16(dl + 2 * i, aud_clamp16(aud_ld16(dl + 2 * i) + l));
			aud_st16(dr + 2 * i, aud_clamp16(aud_ld16(dr + 2 * i) + r));
			aud_st16(wl + 2 * i, aud_clamp16(aud_ld16(wl + 2 * i) + l2));
			aud_st16(wr + 2 * i, aud_clamp16(aud_ld16(wr + 2 * i) + r2));
		}
		env_values[0] += env_steps[0];
		env_values[1] += env_steps[1];
		env_values[2] += env_steps[2];
		dl += 16;
		dr += 16;
		wl += 16;
		wr += 16;
		in += 16;
	}
}

/* ---- Mezclas ---------------------------------------------------------------- */

void alist_mix(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t gain)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *src = aud_dm(st, dmemi);
	for (count >>= 1; count; count--, dst += 2, src += 2)
		sample_mix(dst, aud_ld16(src), gain);
}

void alist_multQ44(struct aud_state *st, uint16_t dmem, uint16_t count, int8_t gain)
{
	uint8_t *dst = aud_dm(st, dmem);
	for (count >>= 1; count; count--, dst += 2)
		aud_st16(dst, aud_clamp16(aud_ld16(dst) * gain >> 4));
}

void alist_add(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const uint8_t *src = aud_dm(st, dmemi);
	for (count >>= 1; count; count--, dst += 2, src += 2)
		aud_st16(dst, aud_clamp16(aud_ld16(dst) + aud_ld16(src)));
}

/* ---- Cambio de tono --------------------------------------------------------- */

/* Estado de RESAMPLE en la RDRAM: las 4 muestras previas y el acumulador. */
void alist_resample(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		    uint32_t pitch, uint32_t address)
{
	uint32_t ipos = (uint32_t)(dmemi >> 1) - 4, opos = dmemo >> 1;
	uint32_t accu;
	int16_t state[5];

	if (init) {
		for (unsigned k = 0; k < 4; k++)
			aud_set_sample(st, ipos + k, 0);
		accu = 0;
	} else {
		aud_load_s16(st, state, address, 5);
		for (unsigned k = 0; k < 4; k++)
			aud_set_sample(st, ipos + k, state[k]);
		accu = (uint16_t)state[4];
	}

	for (count >>= 1; count; count--) {
		const int16_t *lut = aud_resample_lut + ((accu & 0xFC00) >> 8);
		int32_t v = aud_sample(st, ipos) * lut[0] + aud_sample(st, ipos + 1) * lut[1] +
			    aud_sample(st, ipos + 2) * lut[2] + aud_sample(st, ipos + 3) * lut[3];
		aud_set_sample(st, opos++, aud_clamp16(v >> 15));
		accu += pitch;
		ipos += accu >> 16;
		accu &= 0xFFFF;
	}

	for (unsigned k = 0; k < 4; k++)
		state[k] = aud_sample(st, ipos + k);
	state[4] = (int16_t)accu;
	aud_store_s16(st, address, state, 5);
}

void alist_resample_zoh(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count,
			uint32_t pitch, uint32_t pitch_accu)
{
	uint32_t ipos = dmemi >> 1, opos = dmemo >> 1;
	for (count >>= 1; count; count--) {
		aud_set_sample(st, opos++, aud_sample(st, ipos));
		pitch_accu += pitch;
		ipos += pitch_accu >> 16;
		pitch_accu &= 0xFFFF;
	}
}

/* ---- ADPCM -------------------------------------------------------------------
 * Cuadros de 9 bytes (4 bits por muestra) o 5 (2 bits): un byte con la
 * escala y el predictor y 16 residuos. Estado: las 16 últimas muestras. */

static int16_t predict_sample(uint8_t byte, uint8_t mask, unsigned lshift, unsigned rshift)
{
	int16_t sample = (int16_t)((uint16_t)(byte & mask) << lshift);
	return (int16_t)(sample >> rshift);
}

static int32_t rdot(size_t n, const int16_t *x, const int16_t *y)
{
	int32_t accu = 0;
	y += n;
	while (n--)
		accu += *x++ * *--y;
	return accu;
}

static void compute_residuals(int16_t *dst, const int16_t *src, const int16_t *cb_entry,
			      const int16_t *last_samples)
{
	const int16_t *book1 = cb_entry, *book2 = cb_entry + 8;
	int16_t l1 = last_samples[0], l2 = last_samples[1];
	for (size_t i = 0; i < 8; i++) {
		int32_t accu = src[i] * 2048;  /* << 11 sin desplazar negativos */
		accu += book1[i] * l1 + book2[i] * l2 + rdot(i, book2, src);
		dst[i] = aud_clamp16(accu >> 11);
	}
}

void alist_adpcm(struct aud_state *st, bool init, bool loop, bool two_bit_per_sample, uint16_t dmemo,
		 uint16_t dmemi, uint16_t count, const int16_t *codebook, uint32_t loop_address,
		 uint32_t last_frame_address)
{
	int16_t last[16];

	if (init)
		memset(last, 0, sizeof last);
	else
		aud_load_s16(st, last, loop ? loop_address : last_frame_address, 16);

	for (unsigned i = 0; i < 16; i++, dmemo += 2)
		aud_st16(aud_dm(st, dmemo), last[i]);

	for (; count >= 32; count -= 32) {
		int16_t frame[16];
		uint8_t code = *aud_dm(st, dmemi++);
		unsigned scale = code >> 4;
		const int16_t *cb = codebook + ((code & 0xF) << 4);

		if (two_bit_per_sample) {
			unsigned rshift = scale < 14 ? 14 - scale : 0;
			for (unsigned i = 0; i < 4; i++) {
				uint8_t b = *aud_dm(st, dmemi++);
				frame[4 * i + 0] = predict_sample(b, 0xC0, 8, rshift);
				frame[4 * i + 1] = predict_sample(b, 0x30, 10, rshift);
				frame[4 * i + 2] = predict_sample(b, 0x0C, 12, rshift);
				frame[4 * i + 3] = predict_sample(b, 0x03, 14, rshift);
			}
		} else {
			unsigned rshift = scale < 12 ? 12 - scale : 0;
			for (unsigned i = 0; i < 8; i++) {
				uint8_t b = *aud_dm(st, dmemi++);
				frame[2 * i + 0] = predict_sample(b, 0xF0, 8, rshift);
				frame[2 * i + 1] = predict_sample(b, 0x0F, 12, rshift);
			}
		}

		compute_residuals(last, frame, cb, last + 14);
		compute_residuals(last + 8, frame + 8, cb, last + 6);

		for (unsigned i = 0; i < 16; i++, dmemo += 2)
			aud_st16(aud_dm(st, dmemo), last[i]);
	}

	aud_store_s16(st, last_frame_address, last, 16);
}

/* ---- Filtros -----------------------------------------------------------------
 *
 * FILTER (nead): FIR de 8 coeficientes sobre bloques de 8 muestras. En
 * índices de la N64, con X = las 8 muestras guardadas seguidas del bloque:
 *   out[n] = Σ_{t=0..7} L[t] · X[8 + n − t],  redondeado (+0x4000) >> 15
 * Coeficientes: la tabla de la nota (lut_address[0]) se acerca a la mitad
 * de camino a la de la llamada anterior, guardada detrás del estado
 * (lut_address[1]): L = (anterior + nota + 1) >> 1, que queda escrita ahí;
 * la de la nota no se toca. Así lo hace el microcódigo de Zelda OoT
 * ejecutado en el LLE (rama del LLE del RSP, --lle-check-audio): mupen64plus-
 * rsp-hle escribe el promedio redondeado hacia abajo en las dos tablas, y
 * con la anterior en cero el filtro sonaba a la mitad (−6 dB). */
void alist_filter(struct aud_state *st, uint16_t dmem, uint16_t count, uint32_t address,
		  const uint32_t *lut_address)
{
	int16_t lut6[8], lut5[8], x[16];
	uint8_t *io = aud_dm(st, dmem);

	if (count > 0x780)  /* lo que cabe en el búfer del original */
		count = 0x780;
	aud_load_s16(st, lut6, lut_address[0], 8);
	aud_load_s16(st, lut5, lut_address[1], 8);
	for (unsigned i = 0; i < 8; i++)
		lut5[i] = (int16_t)((lut5[i] + lut6[i] + 1) >> 1);
	aud_store_s16(st, lut_address[1], lut5, 8);

	aud_load_s16(st, x, address, 8);
	for (unsigned blk = 0; blk < count; blk += 16, io += 16) {
		for (unsigned i = 0; i < 8; i++)
			x[8 + i] = aud_ld16(io + 2 * i);
		for (unsigned n = 0; n < 8 && blk + 2 * n < count; n++) {
			int32_t v = 0;
			for (unsigned t = 0; t < 8; t++)
				v += lut5[t] * x[8 + n - t];
			aud_st16(io + 2 * n, (int16_t)((v + 0x4000) >> 15));
		}
		memcpy(x, x + 8, 8 * sizeof x[0]);
	}
	aud_store_s16(st, address, x, 8);
}

/* POLEF: filtro de dos polos por bloques de 8 (coeficientes en `table`).
 * Estado en la RDRAM: las 4 últimas muestras; se releen la 3.ª y la 4.ª. */
void alist_polef(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		 uint16_t gain, int16_t *table, uint32_t address)
{
	uint8_t *dst = aud_dm(st, dmemo);
	const int16_t *h1 = table;
	int16_t *h2 = table + 8;
	int16_t l1 = 0, l2 = 0, h2_before[8], frame[8], out[8] = { 0 };

	count = (uint16_t)aud_align(count, 16);
	if (!count)
		return;
	if (!init) {
		int16_t s[4];
		aud_load_s16(st, s, address, 4);
		l1 = s[2];
		l2 = s[3];
	}
	for (unsigned i = 0; i < 8; i++) {
		h2_before[i] = h2[i];
		h2[i] = (int16_t)(((int32_t)h2[i] * gain) >> 14);
	}
	do {
		for (unsigned i = 0; i < 8; i++, dmemi += 2)
			frame[i] = aud_ld16(aud_dm(st, dmemi));
		for (unsigned i = 0; i < 8; i++) {
			int32_t accu = frame[i] * gain;
			accu += h1[i] * l1 + h2_before[i] * l2 + rdot(i, h2, frame);
			out[i] = aud_clamp16(accu >> 14);
			aud_st16(dst + 2 * i, out[i]);
		}
		l1 = out[6];
		l2 = out[7];
		dst += 16;
		count -= 16;
	} while (count != 0);
	aud_store_s16(st, address, out + 4, 4);
}

/* IIR (naudio de Conker y los de MP3): biquad sobre bloques de 8 con
 *   y[n] = c0·x[n] + c1·x[n−1] + c0·x[n−2] + 2·c8·y[n−1] + 2·c9·y[n−2]
 * (c = table[i]; cada producto redondeado a Q15 como VMULF) y la salida
 * truncada a 16 bits sin saturar, como en el original. Estado en la RDRAM:
 * y[n−2], y[n−1] en +4/+6 y x[n−2], x[n−1] en +8/+10. */
static int32_t vmulf(int16_t x, int16_t y)
{
	return ((int32_t)x * y + 0x4000) >> 15;
}

void alist_iirf(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		const int16_t *table, uint32_t address)
{
	uint8_t *dst = aud_dm(st, dmemo);
	int16_t y[8] = { 0 }, x[4] = { 0 };
	unsigned index = 7;

	if (!init) {
		int16_t s[4];
		aud_load_s16(st, s, address + 4, 4);
		y[6] = s[0];
		y[7] = s[1];
		x[1] = s[2];
		x[2] = s[3];
	}
	int32_t prev = vmulf(table[9], y[6]) * 2;
	for (count = (uint16_t)aud_align(count, 16); count; count -= 16, dst += 16) {
		for (unsigned i = 0; i < 8; i++, dmemi += 2) {
			x[index & 3] = aud_ld16(aud_dm(st, dmemi));
			int32_t accu = prev + vmulf(table[0], x[index & 3]) + vmulf(table[1], x[(index - 1) & 3]) +
				       vmulf(table[0], x[(index - 2) & 3]);
			accu += vmulf(table[8], y[index]) * 2;
			prev = vmulf(table[9], y[index]) * 2;
			y[i] = (int16_t)accu;
			aud_st16(dst + 2 * i, y[i]);
			index = (index + 1) & 7;
		}
	}
	int16_t s[4] = { y[6], y[7], x[(index - 2) & 3], x[(index - 1) & 3] };
	aud_store_s16(st, address + 4, s, 4);
}

/* OVERLOAD (distorsión de Conker): ganancia saturada y luego atenuación
 * (Q16 sin signo). `count` en bytes. */
void alist_overload(struct aud_state *st, uint16_t dmem, uint16_t count, int16_t gain, uint16_t attenuation)
{
	uint8_t *p = aud_dm(st, dmem);
	for (count >>= 1; count; count--, p += 2) {
		int32_t accu = aud_clamp16(aud_ld16(p) * gain);
		aud_st16(p, (int16_t)((accu * attenuation) >> 16));
	}
}
