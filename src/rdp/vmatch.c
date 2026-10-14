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
#define COLLAPSE  0.7f    /* radio a media interpolación / el menor de los dos cuadros: menos, se aplasta (giro > ~90°) */
#define OBJ_MAX   128.0f  /* tamaño máximo en pantalla (píxeles) de un objeto de vmatch_atomic */
#define OBJ_GAP   4.0f    /* píxeles entre grupos seguidos del mismo objeto */

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
	b += up16((size_t)gcap * sizeof(uint32_t));
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
	p += up16(gcap);
	t->parent = (uint32_t *)p;
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
 * cientos de píxeles sin que sea un corte. El umbral de w deja fuera además
 * los desnormales (en la FPU escalar del PPE uno cuesta más de 100 ciclos:
 * Cell Handbook, A.6.3) y el 1/w de un w diminuto. */
static bool near_screen(const float c[4])
{
	return c[3] > 1e-6f && fabs_(c[0]) <= 1.5f * c[3] && fabs_(c[1]) <= 1.5f * c[3];
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
		g->vz[0] = r->vz[0], g->vz[1] = r->vz[1];
		g->pair = -1;
		const float(*src)[4] = (const float(*)[4])((const uint8_t *)r + sizeof *r);
		__builtin_prefetch((const uint8_t *)r + c->size);  /* el registro que sigue */
		memcpy(t->clip[t->nclip], src, 16u * n);
		/* En pantalla: cada vértice visible y el centro y el tamaño del grupo. */
		float x0 = FAR_AWAY, y0 = FAR_AWAY, x1 = -FAR_AWAY, y1 = -FAR_AWAY, sx = 0.0f, sy = 0.0f;
		uint32_t vis = 0;
		const float vx = g->vp[0], vy = g->vp[1];
		uint32_t flat = n > 0, front = 0;
		for (uint32_t k = 0; k < n; k++) {
			float *s = scr[t->nclip + k];
			flat &= fabs_(src[k][3] - 1.0f) <= 1e-3f;
			front |= src[k][3] > 1e-6f;
			if (!near_screen(src[k])) {
				s[0] = s[1] = FAR_AWAY;
				continue;
			}
			float iw = 1.0f / src[k][3];
			s[0] = src[k][0] * iw * vx;
			s[1] = src[k][1] * iw * vy;
			x0 = s[0] < x0 ? s[0] : x0, x1 = s[0] > x1 ? s[0] : x1;
			y0 = s[1] < y0 ? s[1] : y0, y1 = s[1] > y1 ? s[1] : y1;
			sx += s[0], sy += s[1];
			vis++;
		}
		g->flat = flat, g->front = front;
		g->why = VMATCH_NEW;
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

/* Un giro grande en un cuadro (Link que se da vuelta) aplasta el grupo a
 * media interpolación: su radio (desde el centro, con los vértices que se
 * ven en los dos cuadros, a y b en pantalla) en el punto medio es menos de
 * COLLAPSE veces el menor de los dos cuadros. Solo con radio de más de 2 px. */
static bool collapses(const float (*a)[2], const struct vmatch_group *h, const float (*b)[2],
		      const struct vmatch_group *g)
{
	float sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0, q0 = 0, q1 = 0, q01 = 0;
	uint32_t nv = 0;
	for (uint32_t k = 0; k < g->n; k++) {
		if (a[k][0] == FAR_AWAY || b[k][0] == FAR_AWAY)
			continue;
		float ax = a[k][0] - h->cx, ay = a[k][1] - h->cy, bx = b[k][0] - g->cx, by = b[k][1] - g->cy;
		sx0 += ax, sy0 += ay, sx1 += bx, sy1 += by;
		q0 += ax * ax + ay * ay, q1 += bx * bx + by * by, q01 += ax * bx + ay * by;
		nv++;
	}
	if (nv < 3)
		return false;
	/* n · radio² = suma de |u|² − |suma de u|² / n; el punto medio es
	 * (u0 + u1) / 2. */
	float in = 1.0f / (float)nv, smx = sx0 + sx1, smy = sy0 + sy1;
	float r0 = q0 - (sx0 * sx0 + sy0 * sy0) * in, r1 = q1 - (sx1 * sx1 + sy1 * sy1) * in;
	float rm = 0.25f * (q0 + q1 + 2.0f * q01 - (smx * smx + smy * smy) * in);
	float lo = r0 < r1 ? r0 : r1;
	return lo > 4.0f * (float)nv && rm < COLLAPSE * COLLAPSE * lo;
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
		g->why = VMATCH_NEW;
		if (g->flat) {  /* ni cuenta para decidir un corte */
			g->why = VMATCH_FLAT;
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
			g->why = VMATCH_JUMPED;
			st->jumped++;
			continue;
		}
		/* Por el contenido (otra dirección: un búfer dinámico), además el
		 * mismo tamaño en pantalla, a menos del doble: un grupo del HUD que
		 * se empareje con otro crecería de golpe en los intermedios. */
		if (content && g->ext >= 0.0f && h->ext >= 0.0f) {
			float big = g->ext > h->ext ? g->ext : h->ext, small = g->ext > h->ext ? h->ext : g->ext;
			if (big > 2.0f * small + 8.0f) {
				g->why = VMATCH_SCALE;
				st->deformed++;
				continue;
			}
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
			g->why = VMATCH_MOVED;
			st->moved++;
			continue;
		}
		float ext = g->ext > h->ext ? g->ext : h->ext;
		float spread = dx1 - dx0 > dy1 - dy0 ? dx1 - dx0 : dy1 - dy0;
		if (dx1 >= dx0 && ext < SMALL) {
			float lim = 0.75f * ext > 24.0f ? 0.75f * ext : 24.0f;
			if (spread > lim) {
				g->why = VMATCH_DEFORMED;
				st->deformed++;
				continue;
			}
		}
		if (dx1 >= dx0 && spread > 0.25f * ext && collapses(ps + h->clip, h, cs + g->clip, g)) {
			g->why = VMATCH_COLLAPSE;
			st->collapsed++;
			continue;
		}
		g->pair = p;
		g->why = VMATCH_OK;
		cur->taken[p] = 1;
		st->paired++;
		st->paired_vertices += g->n;
		st->reordered += reord;
	}
	st->cut = st->found && (st->moved * 3 > st->found || st->found_vertices * 2 < st->vertices);
}

static uint32_t uf_find(uint32_t *parent, uint32_t i)
{
	while (parent[i] != i)
		i = parent[i] = parent[parent[i]];
	return i;
}

/* El grupo (desde 1) de un vértice de salida, o 0. */
static uint32_t group_of(uint32_t pad)
{
	if (!pad)
		return 0;
	return (pad & GFX_VCLIP_FLAG) ? GFX_VCLIP_SERIAL(pad) : GFX_VGID_SERIAL(pad);
}

static bool rejected(uint32_t why)
{
	return why == VMATCH_MOVED || why == VMATCH_JUMPED || why == VMATCH_DEFORMED || why == VMATCH_SCALE ||
	       why == VMATCH_COLLAPSE;
}

static void unpair(struct vmatch_group *g, uint32_t why, struct vmatch_stats *st)
{
	g->pair = -1;
	g->why = why;
	st->atomic++;
	st->paired--;
	st->paired_vertices -= g->n;
}

void vmatch_atomic(struct vmatch_table *cur, const struct gfx_vertex *vtx, uint32_t nvtx, struct vmatch_stats *st)
{
	uint32_t ng = cur->ngroups, *parent = cur->parent;
	if (st->cut || !ng)
		return;
	/* Casi siempre no hay nada que hacer: sin rechazados ni grupos sin
	 * pareja delante de la cámara. */
	bool any_rejected = false, any_lone = false;
	for (uint32_t i = 0; i < ng; i++) {
		const struct vmatch_group *g = &cur->g[i];
		any_rejected |= g->ext >= 0.0f && rejected(g->why);
		any_lone |= g->pair < 0 && !g->flat && g->front;
	}
	/* Objetos: grupos seguidos que se tocan en pantalla (los del HUD y los
	 * que no se ven no cortan uno). */
	const float(*scr)[2] = screen_of(cur);
	for (uint32_t i = 0; any_rejected && i < ng;) {
		float b[4] = { FAR_AWAY, FAR_AWAY, -FAR_AWAY, -FAR_AWAY };
		uint32_t total = 0, bad = 0, j = i;
		for (; j < ng; j++) {
			const struct vmatch_group *g = &cur->g[j];
			if (g->flat || g->ext < 0.0f)
				continue;
			float gb[4] = { FAR_AWAY, FAR_AWAY, -FAR_AWAY, -FAR_AWAY };
			for (uint32_t k = 0; k < g->n; k++) {
				const float *p = scr[g->clip + k];
				if (p[0] == FAR_AWAY)
					continue;
				gb[0] = p[0] < gb[0] ? p[0] : gb[0], gb[2] = p[0] > gb[2] ? p[0] : gb[2];
				gb[1] = p[1] < gb[1] ? p[1] : gb[1], gb[3] = p[1] > gb[3] ? p[1] : gb[3];
			}
			if (b[0] != FAR_AWAY) {
				float x0 = gb[0] < b[0] ? gb[0] : b[0], y0 = gb[1] < b[1] ? gb[1] : b[1];
				float x1 = gb[2] > b[2] ? gb[2] : b[2], y1 = gb[3] > b[3] ? gb[3] : b[3];
				if (gb[0] > b[2] + OBJ_GAP || gb[2] < b[0] - OBJ_GAP || gb[1] > b[3] + OBJ_GAP ||
				    gb[3] < b[1] - OBJ_GAP || x1 - x0 > OBJ_MAX || y1 - y0 > OBJ_MAX)
					break;
				b[0] = x0, b[1] = y0, b[2] = x1, b[3] = y1;
			} else {
				memcpy(b, gb, sizeof b);
			}
			if (g->why == VMATCH_NEW)
				continue;
			total += g->n;
			bad += rejected(g->why) ? g->n : 0;
		}
		if (j == i)
			j++;
		if (bad && 2 * bad >= total)
			for (uint32_t k = i; k < j; k++)
				if (cur->g[k].pair >= 0 && !cur->g[k].flat && cur->g[k].ext >= 0.0f)
					unpair(&cur->g[k], VMATCH_OBJECT, st);
		i = j;
	}
	if (!any_rejected && !any_lone)
		return;
	/* Mallas: los grupos de cada triángulo. */
	for (uint32_t i = 0; i < ng; i++)
		parent[i] = i;
	for (uint32_t i = 0; i + 3 <= nvtx; i += 3) {
		__builtin_prefetch(&vtx[i + 12].pad);
		uint32_t a = group_of(vtx[i].pad) - 1, b = group_of(vtx[i + 1].pad) - 1, c = group_of(vtx[i + 2].pad) - 1;
		if (a == b && b == c)
			continue;
		uint32_t q[3] = { a, b, c }, root = ~0u;
		for (int k = 0; k < 3; k++) {
			if (q[k] >= ng)  /* sin grupo: ~0u */
				continue;
			uint32_t r = uf_find(parent, q[k]);
			if (root == ~0u)
				root = r;
			else if (r != root)
				parent[r] = root;
		}
	}
	/* Una malla con un grupo delante de la cámara sin pareja (que no sea
	 * del HUD) sale entera como en el cuadro de ahora; taken marca aquí
	 * las mallas así. */
	uint8_t *bad = cur->taken;
	for (uint32_t i = 0; i < ng; i++)
		bad[i] = 0;
	for (uint32_t i = 0; i < ng; i++) {
		const struct vmatch_group *g = &cur->g[i];
		if (g->pair < 0 && !g->flat && g->front)
			bad[uf_find(parent, i)] = 1;
	}
	for (uint32_t i = 0; i < ng; i++)
		if (cur->g[i].pair >= 0 && bad[uf_find(parent, i)])
			unpair(&cur->g[i], VMATCH_COMPONENT, st);
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
	const float(*last_cclip)[4] = NULL; /* y las de ahora */
	const float *last_vp = NULL, *last_vz = NULL;
	uint32_t last_n = 0;
	for (uint32_t i = 0; i < nvtx; i++) {
		const struct gfx_vertex *v = &vtx[i];
		__builtin_prefetch(v + 8);  /* el PPE espera la memoria: pedir antes */
		__builtin_prefetch(out + i + 16, 1);
		out[i][0] = v->x, out[i][1] = v->y, out[i][2] = v->z, out[i][3] = v->w;
		if (!prev || st->cut || !v->pad)
			continue;
		float c[4], cc[4];  /* coordenadas de recorte de allá y de ahora */
		const float *vp, *vz;
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
			if (fabs_(da - db) < 1e-6f || !prev_clip(prev, cur, GFX_VGID(serial, ia), a, &vp) ||
			    !prev_clip(prev, cur, GFX_VGID(serial, ib), b, &vpb))
				continue;
			float t = da / (da - db);
			for (int q = 0; q < 4; q++) {
				c[q] = a[q] + (b[q] - a[q]) * t;
				cc[q] = ca[q] + (cb[q] - ca[q]) * t;
			}
			vz = cur->g[serial - 1].vz;
		} else {
			uint32_t serial = GFX_VGID_SERIAL(v->pad), k = GFX_VGID_INDEX(v->pad);
			if (serial != last_serial) {
				last_serial = serial;
				last_clip = NULL;
				if (serial && serial <= cur->ngroups && cur->g[serial - 1].pair >= 0) {
					const struct vmatch_group *g = &cur->g[serial - 1];
					last_clip = prev->clip + prev->g[g->pair].clip;
					last_cclip = cur->clip + g->clip;
					last_vp = g->vp;
					last_vz = g->vz;
					last_n = g->n;
				}
			}
			if (!last_clip || k >= last_n)
				continue;
			memcpy(c, last_clip[k], sizeof c);
			memcpy(cc, last_cclip[k], sizeof cc);
			vp = last_vp;
			vz = last_vz;
		}
		/* Un vértice que cambia de lado de la cámara (w pasa por 0 entre los
		 * dos cuadros) se proyectaría al infinito en algún intermedio: sus
		 * triángulos taparían media pantalla o dejarían un hueco (el agua que
		 * asoma en Zelda al caminar junto al estanque). Queda donde está;
		 * la regla es del vértice, así sus copias en otros grupos hacen lo
		 * mismo y no se abren grietas. */
		if ((c[3] > 1e-6f) != (v->w > 1e-6f))
			continue;
		/* Como gfx_vtx_out (geom.c): x = vtrans·w + vscale·x, y = vtrans·w −
		 * vscale·y, z = vz[1]·w + vz[0]·z. Si la z de ahora no sale así (la
		 * z de primitiva cambió entre el G_VTX y el triángulo), la de allá
		 * con la misma z/w que ahora. */
		out[i][0] = vp[2] * c[3] + vp[0] * c[0];
		out[i][1] = vp[3] * c[3] - vp[1] * c[1];
		out[i][2] = vz[1] * c[3] + vz[0] * c[2];
		out[i][3] = c[3];
		float zc = vz[1] * cc[3] + vz[0] * cc[2];
		if (fabs_(zc - v->z) > 1e-4f * (fabs_(v->z) + fabs_(v->w)) + 1e-6f)
			out[i][2] = fabs_(v->w) > 1e-6f ? v->z / v->w * c[3] : v->z;
		paired++;
	}
	return paired;
}
