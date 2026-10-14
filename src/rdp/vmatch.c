/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — emparejamiento de vértices entre cuadros para la interpolación
 * por geometría. Ver vmatch.h y docs/interpolacion.md.
 */
#include "rdp/vmatch.h"

#include <string.h>

#define KIND_ADDR 1u
#define KIND_HASH 2u
#define WINDOW    8       /* instancias de una clave, a cada lado de la suya en el orden, que se miran */
#define SMALL     64.0f   /* un grupo de menos píxeles que esto es «chico» (partículas, destellos, estelas) */
#define SAMPLES   32      /* instancias de una clave con que se saca su desplazamiento típico */
#define FAR_AWAY  1e30f   /* coordenada de pantalla de un vértice que no se ve */

static uint32_t mix(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return h;
}

static uint32_t kcap_for(uint32_t gcap)
{
	uint32_t k = 16;
	while (k < 4 * gcap)
		k <<= 1;
	return k;
}

static size_t up16(size_t b)
{
	return (b + 15) & ~(size_t)15;
}

/* Lo que va en la memoria de una tabla, en este orden: grupos, coordenadas
 * de recorte, coordenadas de pantalla, claves, orden y marcas. */
size_t vmatch_table_bytes(uint32_t gcap, uint32_t ccap)
{
	size_t b = up16((size_t)gcap * sizeof(struct vmatch_group));
	b += up16((size_t)ccap * 4 * sizeof(float));
	b += up16((size_t)ccap * 2 * sizeof(float));
	b += up16((size_t)kcap_for(gcap) * sizeof(struct vmatch_key));
	b += up16((size_t)2 * gcap * sizeof(uint32_t));
	b += up16((size_t)2 * gcap * sizeof(uint32_t));
	b += up16(gcap);
	return b;
}

/* Coordenadas de pantalla de los vértices (vmatch_build): junto a las de
 * recorte, en la misma memoria; no van en la cabecera pública. */
static float (*screen_of(const struct vmatch_table *t))[2]
{
	return (float(*)[2])((uint8_t *)t->clip + up16((size_t)t->ccap * 4 * sizeof(float)));
}

void vmatch_table_init(struct vmatch_table *t, void *mem, uint32_t gcap, uint32_t ccap)
{
	uint8_t *p = mem;
	memset(t, 0, sizeof *t);
	memset(mem, 0, vmatch_table_bytes(gcap, ccap));
	t->gcap = gcap, t->ccap = ccap, t->kcap = kcap_for(gcap);
	t->g = (struct vmatch_group *)p;
	p += up16((size_t)gcap * sizeof(struct vmatch_group));
	t->clip = (float(*)[4])p;
	p += up16((size_t)ccap * 4 * sizeof(float));
	p += up16((size_t)ccap * 2 * sizeof(float));  /* screen_of */
	t->keys = (struct vmatch_key *)p;
	p += up16((size_t)t->kcap * sizeof(struct vmatch_key));
	t->order = (uint32_t *)p;
	p += up16((size_t)2 * gcap * sizeof(uint32_t));
	t->live = (uint32_t *)p;
	p += up16((size_t)2 * gcap * sizeof(uint32_t));
	t->taken = p;
}

/* Una entrada de otra generación está libre (vmatch_build no borra la tabla). */
static bool key_live(const struct vmatch_table *t, const struct vmatch_key *k)
{
	return k->kind >> 2 == t->gen;
}

/* Lugar de la clave en la tabla, o ~0u si no está. */
static uint32_t key_find(const struct vmatch_table *t, uint32_t kind, uint32_t v)
{
	uint32_t mask = t->kcap - 1, tag = t->gen << 2 | kind;
	for (uint32_t i = mix(v, kind) & mask, n = 0; n < t->kcap; i = (i + 1) & mask, n++) {
		const struct vmatch_key *k = &t->keys[i];
		if (!key_live(t, k))
			return ~0u;
		if (k->kind == tag && k->v == v)
			return i;
	}
	return ~0u;
}

/* La clave, agregada si no estaba; ~0u si la tabla está llena. */
static uint32_t key_get(struct vmatch_table *t, uint32_t kind, uint32_t v)
{
	uint32_t mask = t->kcap - 1, tag = t->gen << 2 | kind;
	for (uint32_t i = mix(v, kind) & mask, n = 0; n < t->kcap; i = (i + 1) & mask, n++) {
		struct vmatch_key *k = &t->keys[i];
		if (!key_live(t, k)) {
			if (t->nlive >= 2 * t->gcap)
				return ~0u;
			k->kind = tag, k->v = v, k->count = 0, k->has_m = 0;
			t->live[t->nlive++] = i;
			return i;
		}
		if (k->kind == tag && k->v == v)
			return i;
	}
	return ~0u;
}

static float fabs_(float v)
{
	return v < 0.0f ? -v : v;
}

/* Cerca de la pantalla: un vértice pegado a la cámara o muy fuera se mueve
 * cientos de píxeles sin que sea un corte. */
static bool near_screen(const float c[4])
{
	return c[3] > 0.0f && fabs_(c[0]) <= 1.5f * c[3] && fabs_(c[1]) <= 1.5f * c[3];
}

void vmatch_build(struct vmatch_table *t, const uint8_t *cmd, uint32_t cmd_bytes)
{
	float(*scr)[2] = screen_of(t);
	t->ngroups = t->nclip = t->nlive = 0;
	t->full = false;
	if (++t->gen >= 1u << 29) {  /* da la vuelta: una sola vez borrar */
		memset(t->keys, 0, t->kcap * sizeof *t->keys);
		t->gen = 1;
	}
	uint32_t off = 0;
	while (off + sizeof(struct gfx_cmd) <= cmd_bytes) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)(cmd + off);
		if (c->op == GFX_OP_END || c->size < sizeof *c || c->size > cmd_bytes - off)
			break;
		off += c->size;
		if (c->op != GFX_OP_VGROUP || c->size < sizeof(struct gfx_vgroup))
			continue;
		const struct gfx_vgroup *r = (const struct gfx_vgroup *)c;
		uint32_t n = c->b & 0xFF;
		if (c->size < sizeof *r + 16u * n || t->ngroups >= t->gcap || t->nclip + n > t->ccap) {
			t->full = true;
			break;
		}
		struct vmatch_group *g = &t->g[t->ngroups];
		g->addr = c->a;
		g->n = n;
		g->v0 = (c->b >> 8) & 0xFF;
		g->stride = c->b >> 16;
		g->hash = c->c;
		g->akey = key_get(t, KIND_ADDR, g->addr);
		g->hkey = g->hash ? key_get(t, KIND_HASH, g->hash) : ~0u;
		if (g->akey == ~0u || (g->hash && g->hkey == ~0u)) {
			t->full = true;
			break;
		}
		g->occ = t->keys[g->akey].count++;
		g->hocc = g->hash ? t->keys[g->hkey].count++ : 0;
		g->clip = t->nclip;
		memcpy(g->vp, r->vp, sizeof g->vp);
		g->pair = -1;
		const float(*src)[4] = (const float(*)[4])((const uint8_t *)r + sizeof *r);
		__builtin_prefetch((const uint8_t *)r + c->size);  /* el registro que sigue */
		memcpy(t->clip[t->nclip], src, 16u * n);
		/* En pantalla: cada vértice visible y el centro y el tamaño del grupo. */
		float x0 = FAR_AWAY, y0 = FAR_AWAY, x1 = -FAR_AWAY, y1 = -FAR_AWAY, sx = 0.0f, sy = 0.0f;
		uint32_t vis = 0;
		g->flat = n > 0;
		for (uint32_t k = 0; k < n; k++) {
			float *s = scr[t->nclip + k];
			if (fabs_(src[k][3] - 1.0f) > 1e-3f)
				g->flat = 0;
			if (!near_screen(src[k])) {
				s[0] = s[1] = FAR_AWAY;
				continue;
			}
			float iw = 1.0f / src[k][3];
			s[0] = src[k][0] * iw * g->vp[0];
			s[1] = src[k][1] * iw * g->vp[1];
			x0 = s[0] < x0 ? s[0] : x0, x1 = s[0] > x1 ? s[0] : x1;
			y0 = s[1] < y0 ? s[1] : y0, y1 = s[1] > y1 ? s[1] : y1;
			sx += s[0], sy += s[1];
			vis++;
		}
		if (vis) {
			g->cx = sx / (float)vis, g->cy = sy / (float)vis;
			g->ext = x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0;
		} else {
			g->cx = g->cy = 0.0f;
			g->ext = -1.0f;
		}
		t->nclip += n;
		t->ngroups++;
	}
	/* Los grupos de cada clave, juntos y en su orden. */
	uint32_t run[2] = { 0, 0 };
	for (uint32_t i = 0; i < t->nlive; i++) {
		struct vmatch_key *k = &t->keys[t->live[i]];
		k->start = run[(k->kind & 3) - 1];
		run[(k->kind & 3) - 1] += k->count;
	}
	for (uint32_t i = 0; i < t->ngroups; i++) {
		const struct vmatch_group *g = &t->g[i];
		t->order[t->keys[g->akey].start + g->occ] = i;
		if (g->hash)
			t->order[t->gcap + t->keys[g->hkey].start + g->hocc] = i;
	}
}

/* Una instancia de un modelo repetido (ckey: su clave en el cuadro de
 * ahora) que se movió mucho más que sus hermanas: otra partícula. Los
 * modelos de una sola instancia no saltan (la cámara mueve igual todo). */
static bool jumped(const struct vmatch_group *g, const struct vmatch_group *h, const struct vmatch_key *ckey)
{
	if (!ckey || !ckey->has_m || g->ext < 0.0f || h->ext < 0.0f)
		return false;
	float ext = g->ext > h->ext ? g->ext : h->ext;
	float dx = g->cx - h->cx - ckey->mdx, dy = g->cy - h->cy - ckey->mdy, lim = 2.0f * ext + 24.0f;
	return dx * dx + dy * dy > lim * lim;
}

static void sort_small(float *v, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++)
		for (uint32_t j = i; j > 0 && v[j] < v[j - 1]; j--) {
			float x = v[j];
			v[j] = v[j - 1], v[j - 1] = x;
		}
}

/* El desplazamiento típico (mediana) de las instancias de cada clave
 * repetida del cuadro de ahora, emparejando cada una con la de su mismo
 * lugar en el orden. */
static void key_medians(const struct vmatch_table *prev, struct vmatch_table *cur)
{
	for (uint32_t i = 0; i < cur->nlive; i++) {
		struct vmatch_key *k = &cur->keys[cur->live[i]];
		if (k->count < 3)
			continue;
		uint32_t kind = k->kind & 3, ps = key_find(prev, kind, k->v);
		if (ps == ~0u)
			continue;
		const struct vmatch_key *pk = &prev->keys[ps];
		const uint32_t *corder = cur->order + (kind == KIND_HASH ? cur->gcap : 0) + k->start;
		const uint32_t *porder = prev->order + (kind == KIND_HASH ? prev->gcap : 0) + pk->start;
		float dx[SAMPLES], dy[SAMPLES];
		uint32_t n = 0;
		for (uint32_t j = 0; j < k->count && j < pk->count && n < SAMPLES; j++) {
			const struct vmatch_group *g = &cur->g[corder[j]], *h = &prev->g[porder[j]];
			if (g->ext < 0.0f || h->ext < 0.0f || g->n != h->n)
				continue;
			dx[n] = g->cx - h->cx, dy[n] = g->cy - h->cy;
			n++;
		}
		if (n < 3)
			continue;
		sort_small(dx, n);
		sort_small(dy, n);
		k->mdx = dx[n / 2], k->mdy = dy[n / 2];
		k->has_m = 1;
	}
}

/* La pareja de g entre los grupos del anterior con la clave `key` (order:
 * los de esa clase de clave; ckey, la de g en el cuadro de ahora): la de su
 * mismo lugar en el orden si está libre y no saltó; si no, la más cercana en
 * pantalla (con el desplazamiento típico de la clave) de las que están a
 * WINDOW lugares que no saltó. -1 si no hay. */
static int32_t choose(const struct vmatch_table *prev, const struct vmatch_table *cur, const struct vmatch_group *g,
		      const struct vmatch_key *key, const struct vmatch_key *ckey, const uint32_t *order, uint32_t occ,
		      bool *reordered)
{
	*reordered = false;
	if (!key->count)
		return -1;
	const uint32_t *base = order + key->start;
	uint32_t center = occ < key->count ? occ : key->count - 1;
	uint32_t q0 = base[center];
	const struct vmatch_group *h0 = &prev->g[q0];
	bool ok0 = !cur->taken[q0] && h0->n == g->n && h0->stride == g->stride;
	if (ok0 && !jumped(g, h0, ckey))
		return (int32_t)q0;
	if (key->count == 1 || g->ext < 0.0f)
		return ok0 ? (int32_t)q0 : -1;
	float mx = ckey && ckey->has_m ? ckey->mdx : 0.0f, my = ckey && ckey->has_m ? ckey->mdy : 0.0f;
	uint32_t lo = center > WINDOW ? center - WINDOW : 0, hi = center + WINDOW < key->count ? center + WINDOW : key->count - 1;
	int32_t best = -1;
	float bestd = FAR_AWAY;
	for (uint32_t i = lo; i <= hi; i++) {
		uint32_t q = base[i];
		const struct vmatch_group *h = &prev->g[q];
		if (i == center || cur->taken[q] || h->n != g->n || h->stride != g->stride || h->ext < 0.0f ||
		    jumped(g, h, ckey))
			continue;
		float dx = g->cx - h->cx - mx, dy = g->cy - h->cy - my, d = dx * dx + dy * dy;
		if (d < bestd)
			bestd = d, best = (int32_t)q;
	}
	if (best >= 0) {
		*reordered = true;
		return best;
	}
	return ok0 ? (int32_t)q0 : -1;
}

void vmatch_pair(const struct vmatch_table *prev, struct vmatch_table *cur, float max_disp, struct vmatch_stats *st)
{
	memset(st, 0, sizeof *st);
	st->groups = cur->ngroups;
	bool can = prev && !prev->full && !cur->full;
	if (can) {
		memset(cur->taken, 0, prev->ngroups);
		key_medians(prev, cur);
	}
	const float(*ps)[2] = can ? screen_of(prev) : NULL;
	const float(*cs)[2] = screen_of(cur);
	for (uint32_t i = 0; i < cur->ngroups; i++) {
		struct vmatch_group *g = &cur->g[i];
		g->pair = -1;
		if (g->flat) {  /* ni cuenta para decidir un corte */
			st->flat++;
			continue;
		}
		st->vertices += g->n;
		if (!can)
			continue;
		int32_t p = -1;
		bool content = false, reord = false;
		const struct vmatch_key *ckey = &cur->keys[g->akey];
		uint32_t ks = key_find(prev, KIND_ADDR, g->addr);
		if (ks != ~0u)
			p = choose(prev, cur, g, &prev->keys[ks], ckey, prev->order, g->occ, &reord);
		if (p < 0 && g->hash && (ks = key_find(prev, KIND_HASH, g->hash)) != ~0u) {
			ckey = &cur->keys[g->hkey];
			p = choose(prev, cur, g, &prev->keys[ks], ckey, prev->order + prev->gcap, g->hocc, &reord);
			content = p >= 0;
		}
		if (p < 0)
			continue;
		st->found++;
		st->found_vertices += g->n;
		st->by_content += content;
		const struct vmatch_group *h = &prev->g[p];
		if (jumped(g, h, ckey)) {
			st->jumped++;
			continue;
		}
		/* Desplazamiento de cada vértice visible en los dos cuadros: ninguno
		 * más que max_disp, y todos parecidos (sin deformarse de golpe). */
		float dx0 = FAR_AWAY, dy0 = FAR_AWAY, dx1 = -FAR_AWAY, dy1 = -FAR_AWAY;
		bool moved = false;
		for (uint32_t k = 0; k < g->n && !moved; k++) {
			const float *a = ps[h->clip + k], *b = cs[g->clip + k];
			if (a[0] == FAR_AWAY || b[0] == FAR_AWAY)
				continue;
			float dx = b[0] - a[0], dy = b[1] - a[1];
			moved = fabs_(dx) > max_disp || fabs_(dy) > max_disp;
			dx0 = dx < dx0 ? dx : dx0, dx1 = dx > dx1 ? dx : dx1;
			dy0 = dy < dy0 ? dy : dy0, dy1 = dy > dy1 ? dy : dy1;
		}
		if (moved) {
			st->moved++;
			continue;
		}
		float ext = g->ext > h->ext ? g->ext : h->ext;
		if (dx1 >= dx0 && ext < SMALL) {
			float spread = dx1 - dx0 > dy1 - dy0 ? dx1 - dx0 : dy1 - dy0;
			float lim = 0.75f * ext > 24.0f ? 0.75f * ext : 24.0f;
			if (spread > lim) {
				st->deformed++;
				continue;
			}
		}
		g->pair = p;
		cur->taken[p] = 1;
		st->paired++;
		st->paired_vertices += g->n;
		st->reordered += reord;
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
	uint32_t paired = 0, last_serial = 0;
	const float(*last_clip)[4] = NULL;  /* coordenadas de allá del último grupo (los vértices vienen seguidos) */
	const float *last_vp = NULL;
	uint32_t last_n = 0;
	for (uint32_t i = 0; i < nvtx; i++) {
		const struct gfx_vertex *v = &vtx[i];
		__builtin_prefetch(v + 8);  /* el PPE espera la memoria: pedir antes */
		__builtin_prefetch(out + i + 16, 1);
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
		} else {
			uint32_t serial = GFX_VGID_SERIAL(v->pad), k = GFX_VGID_INDEX(v->pad);
			if (serial != last_serial) {
				last_serial = serial;
				last_clip = NULL;
				if (serial && serial <= cur->ngroups && cur->g[serial - 1].pair >= 0) {
					const struct vmatch_group *g = &cur->g[serial - 1];
					last_clip = prev->clip + prev->g[g->pair].clip;
					last_vp = g->vp;
					last_n = g->n;
				}
			}
			if (!last_clip || k >= last_n)
				continue;
			memcpy(c, last_clip[k], sizeof c);
			vp = last_vp;
		}
		/* Como gfx_vtx_out (geom.c): x = vtrans·w + vscale·x, y = vtrans·w −
		 * vscale·y; z queda la de ahora (la profundidad la pone quien dibuja,
		 * con la z/w de ahora: también la del Z de primitiva). */
		out[i][0] = vp[2] * c[3] + vp[0] * c[0];
		out[i][1] = vp[3] * c[3] - vp[1] * c[1];
		out[i][3] = c[3];
		paired++;
	}
	return paired;
}
