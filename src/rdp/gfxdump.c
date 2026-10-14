/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — armado del volcado de la HLE gráfica en memoria. Ver gfxdump.h.
 */
#include "rdp/gfxdump.h"

#include <string.h>

#include "rdp/targets.h"

#define PAD128(x) (((x) + 127u) & ~127u)

/* Recorre los comandos y junta las texturas que no están en el arena. Con
 * `cmd` NULL solo las cuenta (medir); si no, reescribe sus dibujos para que
 * apunten al arena ampliado. Devuelve el número de texturas distintas, o
 * UINT32_MAX si algo no cuadra. `*tex_total` queda con los bytes del arena
 * ampliado. */
static uint32_t plan(const struct gfx_dump_src *s, const struct gfx_dump_work *w, uint8_t *cmd, uint32_t *tex_total)
{
	uint32_t arena = PAD128(s->arena_bytes), total = arena, n = 0;
	uint8_t *rw = cmd;
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= s->cmd_bytes;) {
		struct gfx_cmd tmp;
		const uint8_t *at = (rw ? rw : s->cmd) + off;
		memcpy(&tmp, at, sizeof tmp);
		if (tmp.op == GFX_OP_END || tmp.size < sizeof tmp)
			break;
		if (tmp.size > s->cmd_bytes - off)
			return UINT32_MAX;
		if (tmp.op == GFX_OP_DRAW) {
			if (tmp.size < sizeof(struct gfx_draw))
				return UINT32_MAX;
			struct gfx_draw *d = rw ? (struct gfx_draw *)(rw + off) : NULL;
			struct gfx_draw dd;
			if (!d) {
				memcpy(&dd, at, sizeof dd);
				d = &dd;
			}
			for (int i = 0; i < d->ntex && i < 2; i++) {
				struct gfx_tex *t = &d->tex[i];
				if (t->where == GFX_TEX_ARENA)
					continue;
				bool target = t->where == GFX_TEX_TARGET;
				uint32_t k = 0, dst = arena;
				while (k < n && (target || w->tgt[k] || w->src[k] != t->offset))
					dst += PAD128(w->len[k++]);
				if (k == n) {
					uint32_t len = (uint32_t)t->pitch * t->height;
					if (n >= w->cap || len > (target ? GFX_TEX_MAX : s->cache_bytes) ||
					    (!target && (!s->cache || t->offset > s->cache_bytes - len)))
						return UINT32_MAX;
					w->src[n] = t->offset;
					w->tgt[n] = target ? t : NULL;
					w->len[n++] = len;
					total += PAD128(len);
				}
				if (rw) {
					t->where = GFX_TEX_ARENA;
					t->offset = dst;
				}
			}
		}
		off += tmp.size;
	}
	*tex_total = total;
	return n;
}

uint32_t gfx_dump_measure(const struct gfx_dump_src *s, const struct gfx_dump_work *w)
{
	uint32_t total;
	/* Medir no reescribe los dibujos, así que las texturas de GFX_TEX_TARGET
	 * se apuntan al propio comando (solo se miran width/height/pitch). */
	if (plan(s, w, NULL, &total) == UINT32_MAX)
		return 0;
	return (uint32_t)sizeof(struct gfx_dump_header) + s->cmd_bytes + s->vtx_bytes + total;
}

uint32_t gfx_dump_build(const struct gfx_dump_src *s, const struct gfx_dump_work *w, uint8_t *out, uint32_t cap)
{
	uint32_t need = gfx_dump_measure(s, w);
	if (!need || need > cap)
		return 0;
	uint8_t *cmd = out + sizeof(struct gfx_dump_header), *vtx = cmd + s->cmd_bytes;
	uint8_t *tex = vtx + s->vtx_bytes;
	memcpy(cmd, s->cmd, s->cmd_bytes);
	uint32_t total;
	uint32_t n = plan(s, w, cmd, &total);
	if (n == UINT32_MAX)
		return 0;
	struct gfx_dump_header h = { GFX_DUMP_MAGIC, s->cmd_bytes, s->vtx_bytes, total };
	memcpy(out, &h, sizeof h);
	memcpy(vtx, s->vtx, s->vtx_bytes);
	uint32_t arena = PAD128(s->arena_bytes);
	memcpy(tex, s->arena, s->arena_bytes);
	memset(tex + s->arena_bytes, 0, arena - s->arena_bytes);
	uint32_t at = arena;
	for (uint32_t k = 0; k < n; k++) {
		uint32_t pad = PAD128(w->len[k]);
		if (w->tgt[k]) {
			const struct gfx_tex *t = w->tgt[k];
			memset(tex + at, 0, pad);
			if (s->rdram)  /* con src_siz 1, las I8 de un destino de 8 bits (las sombras de Conker) */
				(t->src_siz == 1u ? rdp_decode_i8 : rdp_decode_rgba16)(s->rdram, s->rdram_size, t->src_addr,
										      t->src_pitch, t->width, t->height,
										      tex + at, t->pitch);
		} else {
			memcpy(tex + at, s->cache + w->src[k], w->len[k]);
			memset(tex + at + w->len[k], 0, pad - w->len[k]);
		}
		at += pad;
	}
	return need;
}
