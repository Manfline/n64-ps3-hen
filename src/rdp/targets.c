/*
 * Salsa64 — destinos de render del RDP. Ver targets.h.
 */
#include "rdp/targets.h"

#include <string.h>

/* G_IM_SIZ_* de G_SETCIMG (gbi): 4, 8, 16 y 32 bits. */
#define IM_SIZ_8b  1u
#define IM_SIZ_32b 3u

void rdp_targets_init(struct rdp_targets *s)
{
	memset(s, 0, sizeof *s);
	s->shown = -1;
	s->vi_width = 320;
}

uint32_t rdp_surface_bytes(uint32_t w, uint32_t h)
{
	return ((w * 4 + 127) & ~127u) * h;
}

uint32_t rdp_vram_worst(uint32_t scale)
{
	uint32_t a = rdp_surface_bytes(320 * scale, 240 * scale), b = rdp_surface_bytes(640, 480);
	return 10 * (a > b ? a : b);  /* a 1× una de 640 × 480 queda nativa */
}

uint32_t rdp_target_scale(uint32_t global, uint32_t lw, uint32_t lh)
{
	uint32_t s = lh ? 240 * global / lh : 1;
	if (s > RDP_SCALE_MAX)
		s = RDP_SCALE_MAX;
	while (s > 1 && (lw * s > 4096 || lh * s > 4096))
		s--;
	return s ? s : 1;
}

uint32_t rdp_scale_pick(uint32_t requested, uint32_t screen_h, bool *reduced)
{
	return rdp_scale_pick_budget(requested, screen_h, RDP_VRAM_BUDGET, reduced);
}

uint32_t rdp_scale_pick_budget(uint32_t requested, uint32_t screen_h, uint32_t budget, bool *reduced)
{
	uint32_t s = requested;
	if (!s)
		s = screen_h >= 240 ? screen_h / 240 : 1;
	if (s > RDP_SCALE_MAX)
		s = RDP_SCALE_MAX;
	bool cut = false;
	while (s > 1 && rdp_vram_worst(s) > budget) {
		s--;
		cut = true;
	}
	if (reduced)
		*reduced = cut;
	return s;
}

static bool stale(const struct rdp_targets *s, const struct rdp_target *t)
{
	return s->field - t->last_field > RDP_TARGETS_STALE;
}

/* Orden de reciclado: menor es más reciclable. */
static int evict_class(const struct rdp_targets *s, const struct rdp_target *t)
{
	if (stale(s, t))
		return 0;
	if (t->width != s->vi_width)
		return 1;
	return 2;
}

int rdp_targets_get(struct rdp_targets *s, uint32_t addr, uint32_t width, uint32_t siz, uint32_t min_lh,
		    bool *fresh)
{
	int free_i = -1, victim = -1;
	uint32_t lw = width ? width : 320;
	uint32_t lh = lw * 3 / 4;
	if (min_lh > lh)
		lh = min_lh;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		struct rdp_target *t = &s->t[i];
		if (!t->used) {
			if (free_i < 0)
				free_i = i;
			continue;
		}
		if (t->addr == addr && t->width == width) {
			if (t->lh >= lh) {
				*fresh = false;
				return i;
			}
			/* Crece: la superficie se vuelve a crear (lo dibujado hasta
			 * ahora se pierde; pasa una vez). */
			t->lh = lh;
			t->drawn = false;
			s->created++;
			*fresh = true;
			return i;
		}
		if (i == s->shown)
			continue;
		if (victim < 0) {
			victim = i;
			continue;
		}
		const struct rdp_target *v = &s->t[victim];
		int ct = evict_class(s, t), cv = evict_class(s, v);
		if (ct < cv || (ct == cv && (int32_t)(t->last_field - v->last_field) < 0))
			victim = i;
	}
	int i = free_i >= 0 ? free_i : victim;
	if (i < 0)
		i = s->shown >= 0 ? s->shown : 0;  /* no pasa: hay más de una ranura */
	struct rdp_target *t = &s->t[i];
	t->used = true;
	t->drawn = false;
	t->presented = false;
	t->cpu_owned = false;
	t->addr = addr;
	t->width = width;
	t->bpp = siz == IM_SIZ_32b ? 4 : siz == IM_SIZ_8b ? 1 : 2;
	t->fmt = 0;
	t->lw = lw;
	t->lh = lh;
	t->last_field = s->field;
	t->last_task = s->task;
	s->created++;
	*fresh = true;
	return i;
}

void rdp_targets_drawn(struct rdp_targets *s, int slot)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX)
		return;
	s->t[slot].last_field = s->field;
	s->t[slot].last_task = s->task;
	s->t[slot].drawn = true;
	s->t[slot].cpu_owned = false;  /* el RDP volvió a escribir: la verdad está en el RSX */
}

void rdp_targets_task(struct rdp_targets *s)
{
	s->task++;
}

enum rdp_texsrc rdp_targets_texture(struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, int cur,
				    int *slot, uint32_t *x0, uint32_t *y0)
{
	int i = rdp_targets_locate(s, addr, pitch, bpp, x0, y0);
	*slot = i;
	if (i < 0)
		return RDP_TEXSRC_NONE;
	const struct rdp_target *t = &s->t[i];
	if (t->cpu_owned || s->task - t->last_task > RDP_TARGETS_FRESH) {
		s->tex_rdram++;
		return RDP_TEXSRC_RDRAM;
	}
	if (i == cur) {
		s->tex_self++;
		return RDP_TEXSRC_SELF;
	}
	s->tex_rsx++;
	return RDP_TEXSRC_RSX;
}

bool rdp_targets_readback(struct rdp_targets *s, int slot, bool force)
{
	if (slot < 0 || slot >= RDP_TARGETS_MAX || !s->t[slot].used)
		return false;
	struct rdp_target *t = &s->t[slot];
	if (!force) {
		if (t->presented || t->bpp != 2 || t->width != s->vi_width)
			return false;
	}
	t->cpu_owned = true;
	s->readbacks++;
	return true;
}

void rdp_vi_ring_push(struct rdp_vi_ring *r, uint32_t origin, uint32_t width)
{
	if (!width)
		return;
	for (uint32_t k = 0; k < RDP_VI_RING && k < r->n; k++)
		if (r->origin[(r->n - 1 - k) % RDP_VI_RING] == origin)
			return;
	r->origin[r->n % RDP_VI_RING] = origin;
	r->width[r->n % RDP_VI_RING] = width;
	r->n++;
}

bool rdp_vi_ring_covers(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width)
{
	(void)width;
	for (uint32_t k = 0; k < RDP_VI_RING && k < r->n; k++) {
		uint32_t o = r->origin[(r->n - 1 - k) % RDP_VI_RING], w = r->width[(r->n - 1 - k) % RDP_VI_RING];
		/* Un framebuffer empieza un poco antes del origen (líneas que el VI
		 * no muestra) y mide unas 3/4 del ancho de alto. */
		uint32_t line = w * 2, lo = o > 8 * line ? o - 8 * line : 0, hi = o + line * (w * 3 / 4);
		if (addr >= lo && addr < hi)
			return true;
	}
	return false;
}

bool rdp_readback_wanted(const struct rdp_vi_ring *r, uint32_t addr, uint32_t width, uint32_t siz)
{
	if (siz != 2u || !r->n || width != r->width[(r->n - 1) % RDP_VI_RING])
		return false;
	return !rdp_vi_ring_covers(r, addr, width);
}

int rdp_targets_locate(const struct rdp_targets *s, uint32_t addr, uint32_t pitch, uint32_t bpp, uint32_t *x0,
		       uint32_t *y0)
{
	int best = -1;
	uint32_t best_off = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		uint32_t line = t->width * t->bpp;
		if (!t->used || t->width != pitch || t->bpp != bpp || !line || addr < t->addr ||
		    addr >= t->addr + line * t->lh)
			continue;
		uint32_t off = addr - t->addr;
		if (best < 0 || off < best_off) {
			best = i;
			best_off = off;
		}
	}
	if (best < 0)
		return -1;
	uint32_t line = s->t[best].width * bpp;
	*x0 = (best_off % line) / bpp;
	*y0 = best_off / line;
	return best;
}

/* ¿El destino cubre el origen del VI? Da la línea lógica donde cae. */
static bool covers(const struct rdp_target *t, uint32_t origin, uint32_t *y0)
{
	uint32_t line = t->width * t->bpp;
	if (!line || origin < t->addr || origin >= t->addr + line * t->lh)
		return false;
	*y0 = (origin - t->addr) / line;
	return true;
}

void rdp_targets_present(struct rdp_targets *s, const struct salsa64_vi_frame *f, uint32_t field,
			 struct rdp_present *out)
{
	s->field = field;
	/* El campo solo avanza cuando el VI muestra algo: lo dibujado mientras
	 * estuvo en negro (SM64 lo apaga 26 campos al cambiar de escena) queda
	 * con el campo de ahora, no con el de antes del apagado. La antigüedad
	 * de cada destino (para medir el retraso de lo que se ve) se toma antes. */
	uint32_t age[RDP_TARGETS_MAX];
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		age[i] = s->t[i].drawn ? 0 : field - s->t[i].last_field;
		if (s->t[i].drawn) {
			s->t[i].last_field = field;
			s->t[i].drawn = false;
		}
	}
	if (f->width)
		s->vi_width = f->width;

	int best = -1;
	uint32_t best_y0 = 0;
	for (int i = 0; i < RDP_TARGETS_MAX; i++) {
		const struct rdp_target *t = &s->t[i];
		uint32_t y0;
		if (!t->used || stale(s, t) || !covers(t, f->origin, &y0))
			continue;
		if (best >= 0) {
			const struct rdp_target *b = &s->t[best];
			bool tw = t->width == f->width, bw = b->width == f->width;
			if (bw != tw) {
				if (bw)
					continue;
			} else if (y0 != best_y0) {
				if (y0 > best_y0)
					continue;
			} else if ((int32_t)(t->last_field - b->last_field) <= 0) {
				continue;
			}
		}
		best = i;
		best_y0 = y0;
	}
	if (best < 0 && s->shown >= 0 && s->t[s->shown].used && f->origin == s->shown_origin &&
	    field - s->shown_field <= 2 && covers(&s->t[s->shown], f->origin, &best_y0))
		best = s->shown;

	s->shown = best;
	s->shown_origin = f->origin;
	s->shown_field = field;
	if (best >= 0)
		s->t[best].presented = true;
	out->slot = best;
	out->y0 = out->w = out->h = out->age = 0;
	if (best < 0)
		return;
	const struct rdp_target *t = &s->t[best];
	out->age = age[best];
	out->y0 = best_y0;
	out->w = f->width < t->lw ? f->width : t->lw;
	out->h = f->height < t->lh - best_y0 ? f->height : t->lh - best_y0;
}
