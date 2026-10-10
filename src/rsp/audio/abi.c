/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - alist_audio.c, alist_nead.c                     *
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
 * Salsa64 — comandos de las listas de audio por ABI y ejecución de una
 * tarea. Adaptado de src/alist_audio.c, src/alist_nead.c y src/hle.c de
 * mupen64plus-rsp-hle (commit 8a7a472a, 2026-06-23) para Salsa64
 * (2026-10-10): mismas tablas de comandos y decodificación de cada uno,
 * sobre el estado de aud_state y las operaciones de alist.c.
 *
 * Comando: w1 (opcode en los bits 24–30) y w2. ABI1 numera los búferes de
 * la DMEM desde 0x5C0 y usa direcciones segmentadas; nead usa direcciones
 * de la DMEM y de la RDRAM directas.
 *
 * Falta: naudio (Perfect Dark y los de Rare) y MusyX; esas tareas terminan
 * con un error en el resultado y sin tocar la RDRAM.
 */
#include <string.h>

#include "core/rcp.h"
#include "rsp/audio/hle.h"

typedef void (*acmd_fn)(struct aud_state *st, uint32_t w1, uint32_t w2);

static void UNKNOWN(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w2;
	st->res->unknown++;
	st->res->last_unknown = w1;
}

static void SPNOOP(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)st, (void)w1, (void)w2;
}

/* ---- ABI1 ------------------------------------------------------------------- */

enum { DMEM_BASE = 0x5C0 };

static uint32_t a1_address(struct aud_state *st, uint32_t so)
{
	uint32_t seg = (so >> 24) & 0x3F, off = so & 0xFFFFFF;
	if (seg >= 16) {
		st->res->errors++;
		return off;
	}
	return st->abi1.segments[seg] + off;
}

static void A1_SEGMENT(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	uint32_t seg = (w2 >> 24) & 0x3F;
	if (seg < 16)
		st->abi1.segments[seg] = w2 & 0xFFFFFF;
	else
		st->res->errors++;
}

static void A1_CLEARBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint16_t dmem = (uint16_t)(w1 + DMEM_BASE), count = w2 & 0xFFF;
	if (count)
		alist_clear(st, dmem, (uint16_t)aud_align(count, 16));
}

static void A1_ENVMIXER(struct aud_state *st, uint32_t w1, uint32_t w2, enum aud_envmix_kind kind)
{
	const struct aud_abi1 *a = &st->abi1;
	uint8_t flags = (uint8_t)(w1 >> 16);
	alist_envmix(st, kind, flags & A_INIT, flags & A_AUX, a->out, a->dry_right, a->wet_left, a->wet_right,
		     a->in, a->count, a->dry, a->wet, a->vol, a->target, a->rate, a1_address(st, w2));
}

static void A1_ENVMIXER_EXP(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	A1_ENVMIXER(st, w1, w2, AUD_ENVMIX_EXP);
}

static void A1_ENVMIXER_GE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	A1_ENVMIXER(st, w1, w2, AUD_ENVMIX_GE);
}

static void A1_RESAMPLE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint8_t flags = (uint8_t)(w1 >> 16);
	uint16_t pitch = (uint16_t)w1;
	alist_resample(st, flags & A_INIT, st->abi1.out, st->abi1.in, (uint16_t)aud_align(st->abi1.count, 16),
		       (uint32_t)pitch << 1, a1_address(st, w2));
}

static void A1_SETVOL(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_abi1 *a = &st->abi1;
	uint8_t flags = (uint8_t)(w1 >> 16);
	if (flags & A_AUX) {
		a->dry = (int16_t)w1;
		a->wet = (int16_t)w2;
	} else {
		unsigned lr = (flags & A_LEFT) ? 0 : 1;
		if (flags & A_VOL) {
			a->vol[lr] = (int16_t)w1;
		} else {
			a->target[lr] = (int16_t)w1;
			a->rate[lr] = (int32_t)w2;
		}
	}
}

static void A1_SETLOOP(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	st->abi1.loop = a1_address(st, w2);
}

static void A1_ADPCM(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	const struct aud_abi1 *a = &st->abi1;
	uint8_t flags = (uint8_t)(w1 >> 16);
	alist_adpcm(st, flags & A_INIT, flags & A_LOOP, false, a->out, a->in, (uint16_t)aud_align(a->count, 32),
		    a->table, a->loop, a1_address(st, w2));
}

static void A1_LOADBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	if (st->abi1.count)
		alist_load(st, st->abi1.in, a1_address(st, w2), st->abi1.count);
}

static void A1_SAVEBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	if (st->abi1.count)
		alist_save(st, st->abi1.out, a1_address(st, w2), st->abi1.count);
}

static void A1_SETBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_abi1 *a = &st->abi1;
	uint8_t flags = (uint8_t)(w1 >> 16);
	if (flags & A_AUX) {
		a->dry_right = (uint16_t)(w1 + DMEM_BASE);
		a->wet_left = (uint16_t)((w2 >> 16) + DMEM_BASE);
		a->wet_right = (uint16_t)(w2 + DMEM_BASE);
	} else {
		a->in = (uint16_t)(w1 + DMEM_BASE);
		a->out = (uint16_t)((w2 >> 16) + DMEM_BASE);
		a->count = (uint16_t)w2;
	}
}

static void A1_DMEMMOVE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint16_t dmemi = (uint16_t)(w1 + DMEM_BASE), dmemo = (uint16_t)((w2 >> 16) + DMEM_BASE);
	uint16_t count = (uint16_t)w2;
	if (count)
		alist_move(st, dmemo, dmemi, (uint16_t)aud_align(count, 16));
}

static void A1_LOADADPCM(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint16_t count = (uint16_t)w1;
	uint32_t n = aud_align(count, 8) >> 1;
	if (n > 16 * 16)
		n = 16 * 16;
	aud_load_s16(st, st->abi1.table, a1_address(st, w2), n);
}

static void A1_INTERLEAVE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	uint16_t left = (uint16_t)((w2 >> 16) + DMEM_BASE), right = (uint16_t)(w2 + DMEM_BASE);
	if (st->abi1.count)
		alist_interleave(st, st->abi1.out, left, right, (uint16_t)aud_align(st->abi1.count, 16));
}

static void A1_MIXER(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	int16_t gain = (int16_t)w1;
	uint16_t dmemi = (uint16_t)((w2 >> 16) + DMEM_BASE), dmemo = (uint16_t)(w2 + DMEM_BASE);
	if (st->abi1.count)
		alist_mix(st, dmemo, dmemi, (uint16_t)aud_align(st->abi1.count, 32), gain);
}

static void A1_POLEF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_abi1 *a = &st->abi1;
	uint8_t flags = (uint8_t)(w1 >> 16);
	if (a->count)
		alist_polef(st, flags & A_INIT, a->out, a->in, (uint16_t)aud_align(a->count, 16), (uint16_t)w1,
			    a->table, a1_address(st, w2));
}

static const acmd_fn abi1[16] = {
	SPNOOP,       A1_ADPCM,     A1_CLEARBUFF, A1_ENVMIXER_EXP,
	A1_LOADBUFF,  A1_RESAMPLE,  A1_SAVEBUFF,  A1_SEGMENT,
	A1_SETBUFF,   A1_SETVOL,    A1_DMEMMOVE,  A1_LOADADPCM,
	A1_MIXER,     A1_INTERLEAVE, A1_POLEF,    A1_SETLOOP,
};

static const acmd_fn abi1_ge[16] = {
	SPNOOP,       A1_ADPCM,     A1_CLEARBUFF, A1_ENVMIXER_GE,
	A1_LOADBUFF,  A1_RESAMPLE,  A1_SAVEBUFF,  A1_SEGMENT,
	A1_SETBUFF,   A1_SETVOL,    A1_DMEMMOVE,  A1_LOADADPCM,
	A1_MIXER,     A1_INTERLEAVE, A1_POLEF,    A1_SETLOOP,
};

/* ---- nead ------------------------------------------------------------------- */

static void N_LOADADPCM(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint32_t n = (uint16_t)w1 >> 1;
	if (n > 16 * 16)
		n = 16 * 16;
	aud_load_s16(st, st->nead.table, w2 & 0xFFFFFF, n);
}

static void N_SETLOOP(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	st->nead.loop = w2 & 0xFFFFFF;
}

static void N_SETBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	st->nead.in = (uint16_t)w1;
	st->nead.out = (uint16_t)(w2 >> 16);
	st->nead.count = (uint16_t)w2;
}

static void N_ADPCM(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	const struct aud_nead *n = &st->nead;
	uint8_t flags = (uint8_t)(w1 >> 16);
	alist_adpcm(st, flags & 0x1, flags & 0x2, flags & 0x4, n->out, n->in,
		    (uint16_t)aud_align(n->count, 32), n->table, n->loop, w2 & 0xFFFFFF);
}

static void N_CLEARBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint16_t count = w2 & 0xFFF;
	if (count)
		alist_clear(st, (uint16_t)w1, count);
}

static void N_LOADBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_load(st, w1 & 0xFFF, w2 & 0xFFFFFF, (w1 >> 12) & 0xFFF);
}

static void N_SAVEBUFF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_save(st, w1 & 0xFFF, w2 & 0xFFFFFF, (w1 >> 12) & 0xFFF);
}

static void N_MIXER(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_mix(st, (uint16_t)w2, (uint16_t)(w2 >> 16), (w1 >> 12) & 0xFF0, (int16_t)w1);
}

static void N_RESAMPLE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	const struct aud_nead *n = &st->nead;
	uint8_t flags = (uint8_t)(w1 >> 16);
	alist_resample(st, flags & 0x1, n->out, n->in, (uint16_t)aud_align(n->count, 16),
		       (uint32_t)(uint16_t)w1 << 1, w2 & 0xFFFFFF);
}

static void N_RESAMPLE_ZOH(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	const struct aud_nead *n = &st->nead;
	alist_resample_zoh(st, n->out, n->in, n->count, (uint32_t)(uint16_t)w1 << 1, (uint16_t)w2);
}

static void N_DMEMMOVE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	uint16_t count = (uint16_t)w2;
	if (count)
		alist_move(st, (uint16_t)(w2 >> 16), (uint16_t)w1, (uint16_t)aud_align(count, 4));
}

static void N_ENVSETUP1_MK(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_nead *n = &st->nead;
	n->env_values[2] = (w1 >> 8) & 0xFF00;
	n->env_steps[2] = 0;
	n->env_steps[0] = (uint16_t)(w2 >> 16);
	n->env_steps[1] = (uint16_t)w2;
}

static void N_ENVSETUP1(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_nead *n = &st->nead;
	n->env_values[2] = (w1 >> 8) & 0xFF00;
	n->env_steps[2] = (uint16_t)w1;
	n->env_steps[0] = (uint16_t)(w2 >> 16);
	n->env_steps[1] = (uint16_t)w2;
}

static void N_ENVSETUP2(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	st->nead.env_values[0] = (uint16_t)(w2 >> 16);
	st->nead.env_values[1] = (uint16_t)w2;
}

static void n_envmixer(struct aud_state *st, uint32_t w1, uint32_t w2, bool mk)
{
	struct aud_nead *n = &st->nead;
	int16_t xors[4];
	xors[0] = (int16_t)(0 - (int16_t)((w1 & 0x2) >> 1));
	xors[1] = (int16_t)(0 - (int16_t)(w1 & 0x1));
	xors[2] = mk ? 0 : (int16_t)(0 - (int16_t)((w1 & 0x8) >> 1));
	xors[3] = mk ? 0 : (int16_t)(0 - (int16_t)((w1 & 0x4) >> 1));
	alist_envmix_nead(st, mk ? false : (w1 >> 4) & 1, (w2 >> 20) & 0xFF0, (w2 >> 12) & 0xFF0,
			  (w2 >> 4) & 0xFF0, (w2 << 4) & 0xFF0, (w1 >> 12) & 0xFF0, (w1 >> 8) & 0xFF,
			  n->env_values, n->env_steps, xors);
}

static void N_ENVMIXER_MK(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	n_envmixer(st, w1, w2, true);
}

static void N_ENVMIXER(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	n_envmixer(st, w1, w2, false);
}

static void N_DUPLICATE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_repeat64(st, (uint16_t)(w2 >> 16), (uint16_t)w1, (uint8_t)(w1 >> 16));
}

static void N_INTERL(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_copy_every_other_sample(st, (uint16_t)w2, (uint16_t)(w2 >> 16), (uint16_t)w1);
}

static void N_INTERLEAVE_MK(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	(void)w1;
	if (st->nead.count)
		alist_interleave(st, st->nead.out, (uint16_t)(w2 >> 16), (uint16_t)w2, st->nead.count);
}

static void N_INTERLEAVE(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_interleave(st, (uint16_t)w1, (uint16_t)(w2 >> 16), (uint16_t)w2, (w1 >> 12) & 0xFF0);
}

static void N_ADDMIXER(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_add(st, (uint16_t)w2, (uint16_t)(w2 >> 16), (w1 >> 12) & 0xFF0);
}

static void N_HILOGAIN(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_multQ44(st, (uint16_t)(w2 >> 16), w1 & 0xFFF, (int8_t)(w1 >> 16));
}

static void N_FILTER(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_nead *n = &st->nead;
	uint8_t flags = (uint8_t)(w1 >> 16);
	uint32_t address = w2 & 0xFFFFFF;
	if (flags > 1) {
		n->filter_count = (uint16_t)w1;
		n->filter_lut[0] = address;
	} else {
		n->filter_lut[1] = address + 0x10;
		alist_filter(st, (uint16_t)w1, n->filter_count, address, n->filter_lut);
	}
}

static void N_16(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	alist_copy_blocks(st, (uint16_t)(w2 >> 16), (uint16_t)w1, (uint16_t)w2, (uint8_t)(w1 >> 16));
}

static void N_POLEF(struct aud_state *st, uint32_t w1, uint32_t w2)
{
	struct aud_nead *n = &st->nead;
	uint8_t flags = (uint8_t)(w1 >> 16);
	if (n->count)
		alist_polef(st, flags & A_INIT, n->out, n->in, n->count, (uint16_t)w1, n->table, w2 & 0xFFFFFF);
}

static const acmd_fn nead_mk[0x20] = {
	SPNOOP,     N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	SPNOOP,     N_RESAMPLE, SPNOOP,       SPNOOP,
	N_SETBUFF,  SPNOOP,     N_DMEMMOVE,   N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE_MK, N_POLEF, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1_MK, N_ENVMIXER_MK,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  SPNOOP,
	SPNOOP, SPNOOP, SPNOOP, SPNOOP, SPNOOP, SPNOOP, SPNOOP, SPNOOP,
};

static const acmd_fn nead_sf[0x20] = {
	SPNOOP,     N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, SPNOOP,
	N_SETBUFF,  SPNOOP,     N_DMEMMOVE,   N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE_MK, N_POLEF, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  SPNOOP,
	N_HILOGAIN, UNKNOWN,    N_DUPLICATE,  SPNOOP,
	SPNOOP, SPNOOP, SPNOOP, SPNOOP,
};

static const acmd_fn nead_sfj[0x20] = {
	SPNOOP,     N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, SPNOOP,
	N_SETBUFF,  SPNOOP,     N_DMEMMOVE,   N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE_MK, N_POLEF, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
	N_HILOGAIN, UNKNOWN,    N_DUPLICATE,  SPNOOP,
	SPNOOP, SPNOOP, SPNOOP, SPNOOP,
};

static const acmd_fn nead_fz[0x20] = {
	UNKNOWN,    N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	N_ADDMIXER, N_RESAMPLE, SPNOOP,       SPNOOP,
	N_SETBUFF,  SPNOOP,     N_DMEMMOVE,   N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE, SPNOOP,     N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
	SPNOOP,     UNKNOWN,    N_DUPLICATE,  SPNOOP,
	SPNOOP, SPNOOP, SPNOOP, SPNOOP,
};

static const acmd_fn nead_wrjb[0x20] = {
	SPNOOP,     N_ADPCM,    N_CLEARBUFF,  UNKNOWN,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, SPNOOP,
	N_SETBUFF,  SPNOOP,     N_DMEMMOVE,   N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE, SPNOOP,     N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
	N_HILOGAIN, UNKNOWN,    N_DUPLICATE,  N_FILTER,
	SPNOOP, SPNOOP, SPNOOP, SPNOOP,
};

/* Yoshi's Story, 1080°, Zelda OoT y Zelda MM comparten la tabla; MM y los
 * siguientes cambian solo el comando 3 o el 0. */
static const acmd_fn nead_oot[0x18] = {
	UNKNOWN,    N_ADPCM,    N_CLEARBUFF,  UNKNOWN,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, N_FILTER,
	N_SETBUFF,  N_DUPLICATE, N_DMEMMOVE,  N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE, N_HILOGAIN, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
};

static const acmd_fn nead_mm[0x18] = {
	UNKNOWN,    N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, N_FILTER,
	N_SETBUFF,  N_DUPLICATE, N_DMEMMOVE,  N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE, N_HILOGAIN, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
};

static const acmd_fn nead_mmb[0x18] = {
	SPNOOP,     N_ADPCM,    N_CLEARBUFF,  SPNOOP,
	N_ADDMIXER, N_RESAMPLE, N_RESAMPLE_ZOH, N_FILTER,
	N_SETBUFF,  N_DUPLICATE, N_DMEMMOVE,  N_LOADADPCM,
	N_MIXER,    N_INTERLEAVE, N_HILOGAIN, N_SETLOOP,
	N_16,       N_INTERL,   N_ENVSETUP1,  N_ENVMIXER,
	N_LOADBUFF, N_SAVEBUFF, N_ENVSETUP2,  UNKNOWN,
};

/* ---- Ejecución -------------------------------------------------------------- */

struct abi_table {
	const acmd_fn *fn;
	uint32_t size;
	bool abi1;  /* segmentos que se borran en cada tarea */
};

static bool abi_table(uint32_t abi, struct abi_table *t)
{
	t->abi1 = false;
	switch (abi) {
	case SALSA64_ABI_AUDIO:     t->fn = abi1; t->size = 16; t->abi1 = true; return true;
	case SALSA64_ABI_AUDIO_GE:
	case SALSA64_ABI_AUDIO_BC:  t->fn = abi1_ge; t->size = 16; t->abi1 = true; return true;
	case SALSA64_ABI_NEAD_MK:   t->fn = nead_mk; t->size = 0x20; return true;
	case SALSA64_ABI_NEAD_SF:   t->fn = nead_sf; t->size = 0x20; return true;
	case SALSA64_ABI_NEAD_SFJ:  t->fn = nead_sfj; t->size = 0x20; return true;
	case SALSA64_ABI_NEAD_WRJB: t->fn = nead_wrjb; t->size = 0x20; return true;
	case SALSA64_ABI_NEAD_FZ:
	case SALSA64_ABI_NEAD_EFZ:  t->fn = nead_fz; t->size = 0x20; return true;
	case SALSA64_ABI_NEAD_YS:
	case SALSA64_ABI_NEAD_1080:
	case SALSA64_ABI_NEAD_OOT:  t->fn = nead_oot; t->size = 0x18; return true;
	case SALSA64_ABI_NEAD_MM:
	case SALSA64_ABI_NEAD_AC:   t->fn = nead_mm; t->size = 0x18; return true;
	case SALSA64_ABI_NEAD_MMB:  t->fn = nead_mmb; t->size = 0x18; return true;
	default:                    return false;
	}
}

void aud_init(struct aud_state *st)
{
	memset(st, 0, sizeof *st);
}

void aud_run(struct aud_state *st, const struct aud_job *job, struct aud_result *res)
{
	struct abi_table t;
	memset(res, 0, sizeof *res);
	st->job = job;
	st->res = res;
	if (!abi_table(job->abi, &t)) {
		res->errors++;
		return;
	}
	if (t.abi1)
		memset(st->abi1.segments, 0, sizeof st->abi1.segments);

	aud_io_begin(st);
	uint32_t n = job->alist_size / 8;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t w1, w2;
		if (!aud_io_alist(st, i, &w1, &w2)) {
			res->errors++;
			break;
		}
		uint32_t op = (w1 >> 24) & 0x7F;
		res->commands++;
		if (op < t.size)
			t.fn[op](st, w1, w2);
		else
			UNKNOWN(st, w1, w2);
	}
	aud_io_end(st);
}
