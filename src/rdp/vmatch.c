/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — emparejamiento de vértices entre cuadros para la interpolación
 * por geometría. Ver vmatch.h y docs/interpolacion.md.
 */
#include "rdp/vmatch.h"

#include <string.h>

static uint32_t mix(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return h;
}

/* Claves: por dirección (tipo 0) y por contenido (tipo 1). */
static uint32_t key_hash(uint32_t kind, uint32_t v, uint32_t occ)
{
	return mix(v ^ (kind ? 0xA5A5A5A5u : 0u), occ);
}

static bool same_key(const struct vmatch_group *g, uint32_t kind, uint32_t v, uint32_t occ)
{
	return kind ? g->hash == v && g->hocc == occ : g->addr == v && g->occ == occ;
}

static void index_put(struct vmatch_table *t, uint32_t kind, uint32_t v, uint32_t occ, uint32_t gi)
{
	uint32_t mask = t->icap - 1;
	for (uint32_t i = key_hash(kind, v, occ) & mask, n = 0; n < t->icap; i = (i + 1) & mask, n++)
		if (!t->index[i]) {
			t->index[i] = gi + 1;
			return;
		}
}

static int32_t index_get(const struct vmatch_table *t, uint32_t kind, uint32_t v, uint32_t occ)
{
	uint32_t mask = t->icap - 1;
	for (uint32_t i = key_hash(kind, v, occ) & mask, n = 0; n < t->icap; i = (i + 1) & mask, n++) {
		uint32_t e = t->index[i];
		if (!e)
			return -1;
		if (same_key(&t->g[e - 1], kind, v, occ))
			return (int32_t)(e - 1);
	}
	return -1;
}

/* Cuántos grupos anteriores del cuadro tienen esa clave (repeticiones). */
static uint32_t count_key(const struct vmatch_table *t, uint32_t kind, uint32_t v)
{
	uint32_t occ = 0;
	while (index_get(t, kind, v, occ) >= 0)
		occ++;
	return occ;
}

void vmatch_table_init(struct vmatch_table *t, struct vmatch_group *g, uint32_t gcap, float (*clip)[4], uint32_t ccap,
		       uint32_t *index, uint32_t icap)
{
	memset(t, 0, sizeof *t);
	t->g = g, t->gcap = gcap;
	t->clip = clip, t->ccap = ccap;
	t->index = index, t->icap = icap;
}

/* FNV-1a por palabras (big-endian, como la RDRAM) de [addr, addr + len). */
static uint32_t content_hash(const uint8_t *rdram, uint32_t size, uint32_t addr, uint32_t len)
{
	uint32_t h = 0x811C9DC5u;
	if (!rdram || addr >= size || len > size - addr)
		return 0;
	for (const uint8_t *p = rdram + addr, *e = p + (len & ~3u); p < e; p += 4)
		h = (h ^ ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3])) * 0x01000193u;
	return h;
}

void vmatch_build(struct vmatch_table *t, const uint8_t *cmd, uint32_t cmd_bytes, const uint8_t *rdram,
		  uint32_t rdram_size)
{
	t->ngroups = t->nclip = 0;
	t->full = false;
	memset(t->index, 0, t->icap * sizeof *t->index);
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c)
			break;
		if (c->op == GFX_OP_VGROUP && c->size >= sizeof(struct gfx_vgroup)) {
			const struct gfx_vgroup *r = (const struct gfx_vgroup *)c;
			uint32_t n = c->b & 0xFF;
			if (c->size < sizeof *r + 16u * n || t->ngroups >= t->gcap || t->nclip + n > t->ccap ||
			    2 * (t->ngroups + 1) > t->icap) {
				t->full = true;
				break;
			}
			struct vmatch_group *g = &t->g[t->ngroups];
			g->addr = c->a;
			g->n = n;
			g->v0 = (c->b >> 8) & 0xFF;
			g->stride = c->b >> 16;
			g->hash = content_hash(rdram, rdram_size, g->addr, n * g->stride);
			g->occ = count_key(t, 0, g->addr);
			g->hocc = count_key(t, 1, g->hash);
			g->clip = t->nclip;
			memcpy(g->vp, r->vp, sizeof g->vp);
			g->pair = -1;
			memcpy(t->clip[t->nclip], (const uint8_t *)r + sizeof *r, 16u * n);
			t->nclip += n;
			index_put(t, 0, g->addr, g->occ, t->ngroups);
			index_put(t, 1, g->hash, g->hocc, t->ngroups);
			t->ngroups++;
		}
		off += c->size;
	}
}

static float fabs_(float v)
{
	return v < 0.0f ? -v : v;
}

/* Cerca de la pantalla en los dos cuadros: un vértice pegado a la cámara (w
 * chica) se mueve cientos de píxeles sin que sea un corte. */
static bool near_screen(const float c[4])
{
	return c[3] > 0.0f && fabs_(c[0]) <= 1.5f * c[3] && fabs_(c[1]) <= 1.5f * c[3];
}

void vmatch_pair(const struct vmatch_table *prev, struct vmatch_table *cur, float max_disp, struct vmatch_stats *st)
{
	memset(st, 0, sizeof *st);
	st->groups = cur->ngroups;
	for (uint32_t i = 0; i < cur->ngroups; i++) {
		struct vmatch_group *g = &cur->g[i];
		g->pair = -1;
		st->vertices += g->n;
		if (!prev || prev->full || cur->full)
			continue;
		int32_t p = index_get(prev, 0, g->addr, g->occ);
		bool content = false;
		if ((p < 0 || prev->g[p].n != g->n || prev->g[p].stride != g->stride) && g->hash) {
			p = index_get(prev, 1, g->hash, g->hocc);
			content = true;
		}
		if (p < 0 || prev->g[p].n != g->n)
			continue;
		st->found++;
		st->found_vertices += g->n;
		st->by_content += content;
		const struct vmatch_group *q = &prev->g[p];
		bool moved = false;
		for (uint32_t k = 0; k < g->n && !moved; k++) {
			const float *a = prev->clip[q->clip + k], *b = cur->clip[g->clip + k];
			if (!near_screen(a) || !near_screen(b))
				continue;
			float dx = (a[0] / a[3] - b[0] / b[3]) * g->vp[0];
			float dy = (a[1] / a[3] - b[1] / b[3]) * g->vp[1];
			moved = fabs_(dx) > max_disp || fabs_(dy) > max_disp;
		}
		if (moved) {
			st->moved++;
			continue;
		}
		g->pair = p;
		st->paired++;
		st->paired_vertices += g->n;
	}
	st->cut = st->found && (st->moved * 3 > st->found || st->found_vertices * 2 < st->vertices);
}

/* Las coordenadas de recorte en el cuadro anterior del vértice con
 * identidad `id` (GFX_VGID) y el viewport de su grupo; false sin pareja. */
static bool prev_clip(const struct vmatch_table *prev, const struct vmatch_table *cur, uint32_t id, float c[4],
		      const float **vp)
{
	uint32_t serial = GFX_VGID_SERIAL(id), k = GFX_VGID_INDEX(id);
	if ((id & GFX_VCLIP_FLAG) || !serial || serial > cur->ngroups)
		return false;
	const struct vmatch_group *g = &cur->g[serial - 1];
	if (g->pair < 0 || k >= g->n)
		return false;
	memcpy(c, prev->clip[prev->g[g->pair].clip + k], 4 * sizeof(float));
	*vp = g->vp;
	return true;
}

uint32_t vmatch_prev(const struct vmatch_table *prev, const struct vmatch_table *cur, const struct vmatch_stats *st,
		     const struct gfx_vertex *vtx, uint32_t nvtx, float (*out)[4])
{
	uint32_t paired = 0;
	for (uint32_t i = 0; i < nvtx; i++) {
		const struct gfx_vertex *v = &vtx[i];
		out[i][0] = v->x, out[i][1] = v->y, out[i][2] = v->z, out[i][3] = v->w;
		if (!prev || st->cut || !v->pad)
			continue;
		float c[4];
		const float *vp;
		if (v->pad & GFX_VCLIP_FLAG) {
			/* Del recorte cercano entre los lugares a y b de un grupo: la
			 * misma t que usó la HLE (con las coordenadas de ahora) entre
			 * las posiciones de allá de a y b; la salida es lineal en las
			 * coordenadas de recorte. */
			uint32_t serial = GFX_VCLIP_SERIAL(v->pad), ia = GFX_VCLIP_A(v->pad), ib = GFX_VCLIP_B(v->pad);
			float a[4], b[4];
			const float *vpb;
			if (!serial || serial > cur->ngroups || ia >= cur->g[serial - 1].n || ib >= cur->g[serial - 1].n)
				continue;
			const float *ca = cur->clip[cur->g[serial - 1].clip + ia], *cb = cur->clip[cur->g[serial - 1].clip + ib];
			float da = ca[2] + ca[3], db = cb[2] + cb[3];
			if (da == db || !prev_clip(prev, cur, GFX_VGID(serial, ia), a, &vp) ||
			    !prev_clip(prev, cur, GFX_VGID(serial, ib), b, &vpb))
				continue;
			float t = da / (da - db);
			for (int q = 0; q < 4; q++)
				c[q] = a[q] + (b[q] - a[q]) * t;
		} else if (!prev_clip(prev, cur, v->pad, c, &vp)) {
			continue;
		}
		/* Como gfx_vtx_out (geom.c): x = vtrans·w + vscale·x, y = vtrans·w −
		 * vscale·y; la profundidad z/w se queda la de ahora (también la del Z
		 * de primitiva). */
		float zw = v->w != 0.0f ? v->z / v->w : 0.0f;
		out[i][0] = vp[2] * c[3] + vp[0] * c[0];
		out[i][1] = vp[3] * c[3] - vp[1] * c[1];
		out[i][2] = zw * c[3];
		out[i][3] = c[3];
		paired++;
	}
	return paired;
}
