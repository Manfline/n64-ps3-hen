/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de audio: el estado que persiste entre tareas, para los
 * estados guardados. Ver AUD_STATE_* en audio.h.
 *
 * Formato portable (big-endian, el mismo en el host, el PPE y el SPE): una
 * versión, la DMEM con su margen y la memoria de MP3 tal cual (ya están en
 * el orden de bytes de la N64) y el estado de cada familia de ABI campo por
 * campo.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/audio/audio.h"

#define FORMAT 1u

struct pk {
	uint8_t *p;
	const uint8_t *q;
	bool load;
};

static void f16(struct pk *k, uint16_t *v)
{
	if (k->load) {
		*v = salsa64_rd16(k->q);
		k->q += 2;
	} else {
		salsa64_wr16(k->p, *v);
		k->p += 2;
	}
}

static void f32(struct pk *k, uint32_t *v)
{
	if (k->load) {
		*v = salsa64_rd32(k->q);
		k->q += 4;
	} else {
		salsa64_wr32(k->p, *v);
		k->p += 4;
	}
}

static void fs16(struct pk *k, int16_t *v, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		f16(k, (uint16_t *)&v[i]);
}

static void fu16(struct pk *k, uint16_t *v, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		f16(k, &v[i]);
}

static void fbytes(struct pk *k, uint8_t *v, uint32_t n)
{
	if (k->load) {
		memcpy(v, k->q, n);
		k->q += n;
	} else {
		memcpy(k->p, v, n);
		k->p += n;
	}
}

static void fields(struct pk *k, struct aud_state *st)
{
	fbytes(k, st->dmem, sizeof st->dmem);
	fbytes(k, st->mp3, sizeof st->mp3);

	struct aud_abi1 *a = &st->abi1;
	for (unsigned i = 0; i < 16; i++)
		f32(k, &a->segments[i]);
	f16(k, &a->in);
	f16(k, &a->out);
	f16(k, &a->count);
	f16(k, &a->dry_right);
	f16(k, &a->wet_left);
	f16(k, &a->wet_right);
	fs16(k, &a->dry, 1);
	fs16(k, &a->wet, 1);
	fs16(k, a->vol, 2);
	fs16(k, a->target, 2);
	f32(k, (uint32_t *)&a->rate[0]);
	f32(k, (uint32_t *)&a->rate[1]);
	f32(k, &a->loop);
	fs16(k, a->table, 16 * 16);

	struct aud_nead *n = &st->nead;
	f16(k, &n->in);
	f16(k, &n->out);
	f16(k, &n->count);
	fu16(k, n->env_values, 3);
	fu16(k, n->env_steps, 3);
	f32(k, &n->loop);
	f16(k, &n->filter_count);
	f32(k, &n->filter_lut[0]);
	f32(k, &n->filter_lut[1]);
	fs16(k, n->table, 16 * 16);

	struct aud_naudio *na = &st->naudio;
	fs16(k, &na->dry, 1);
	fs16(k, &na->wet, 1);
	fs16(k, na->vol, 2);
	fs16(k, na->target, 2);
	f32(k, (uint32_t *)&na->rate[0]);
	f32(k, (uint32_t *)&na->rate[1]);
	f32(k, &na->loop);
	fs16(k, na->table, 16 * 16);
}

#define PACKED (4 + AUD_DMEM_SIZE + AUD_DMEM_SLACK + AUD_MP3_SIZE + \
		(16 * 4 + 6 * 2 + 2 * 2 + 4 * 2 + 2 * 4 + 4 + 256 * 2) + \
		(3 * 2 + 6 * 2 + 4 + 2 + 2 * 4 + 256 * 2) + \
		(2 * 2 + 4 * 2 + 2 * 4 + 4 + 256 * 2))
_Static_assert(PACKED <= AUD_STATE_BYTES, "el estado de la HLE de audio no cabe en AUD_STATE_BYTES");

void aud_state_save(const struct aud_state *st, uint8_t *out)
{
	struct pk k = { out, NULL, false };
	memset(out, 0, AUD_STATE_BYTES);
	uint32_t format = FORMAT;
	f32(&k, &format);
	fields(&k, (struct aud_state *)st);
}

bool aud_state_load(struct aud_state *st, const uint8_t *in)
{
	if (salsa64_rd32(in) != FORMAT)
		return false;
	struct pk k = { NULL, in + 4, true };
	fields(&k, st);
	return true;
}
