/*
 * Salsa64 — HLE gráfica: el estado que persiste entre tareas (el del RDP y
 * la TMEM) para los estados guardados. Ver GFX_STATE_* en gfx.h.
 *
 * Formato portable (big-endian, el mismo en el host, el PPE y el SPE):
 * GFX_STATE_REGS bytes con una versión y los registros, campo por campo,
 * y después la TMEM tal cual (sus bytes ya están en el orden de la N64).
 * Lo del RSP (matrices, luces, segmentos) no va: se reinicia en cada tarea.
 */
#include <string.h>

#include "core/endian.h"
#include "rsp/gfx/hle.h"

#define FORMAT 1u

struct pk {
	uint8_t *p;
	const uint8_t *q;
};

static void w8(struct pk *k, uint8_t v) { *k->p++ = v; }
static void w16(struct pk *k, uint16_t v) { salsa64_wr16(k->p, v); k->p += 2; }
static void w32(struct pk *k, uint32_t v) { salsa64_wr32(k->p, v); k->p += 4; }
static uint8_t r8(struct pk *k) { return *k->q++; }
static uint16_t r16(struct pk *k) { uint16_t v = salsa64_rd16(k->q); k->q += 2; return v; }
static uint32_t r32(struct pk *k) { uint32_t v = salsa64_rd32(k->q); k->q += 4; return v; }

void gfx_state_pack_regs(const struct gfx_state *g, uint8_t out[GFX_STATE_REGS])
{
	struct pk k = { out, NULL };
	memset(out, 0, GFX_STATE_REGS);
	w32(&k, FORMAT);
	w32(&k, g->oh);
	w32(&k, g->ol);
	w32(&k, g->cc_hi);
	w32(&k, g->cc_lo);
	w32(&k, g->prim);
	w32(&k, g->env);
	w32(&k, g->fog);
	w32(&k, g->blend);
	w32(&k, g->fill);
	w8(&k, g->prim_lod);
	w8(&k, g->prim_min);
	w16(&k, g->prim_z);
	w16(&k, g->prim_dz);
	for (unsigned i = 0; i < 4; i++)
		w16(&k, g->scissor[i]);
	w32(&k, g->cimg_addr);
	w16(&k, g->cimg_width);
	w8(&k, g->cimg_fmt);
	w8(&k, g->cimg_siz);
	w32(&k, g->zimg_addr);
	w32(&k, g->timg_addr);
	w16(&k, g->timg_width);
	w8(&k, g->timg_fmt);
	w8(&k, g->timg_siz);
	for (unsigned i = 0; i < 8; i++) {
		const struct gfx_tile *t = &g->tiles[i];
		w8(&k, t->fmt);
		w8(&k, t->siz);
		w8(&k, t->pal);
		w8(&k, t->cms);
		w8(&k, t->cmt);
		w8(&k, t->masks);
		w8(&k, t->maskt);
		w8(&k, t->shifts);
		w8(&k, t->shiftt);
		w16(&k, t->line);
		w16(&k, t->tmem);
		w16(&k, t->uls);
		w16(&k, t->ult);
		w16(&k, t->lrs);
		w16(&k, t->lrt);
	}
}

bool gfx_state_unpack_regs(struct gfx_state *g, const uint8_t in[GFX_STATE_REGS])
{
	struct pk k = { NULL, in };
	if (r32(&k) != FORMAT)
		return false;
	g->oh = r32(&k);
	g->ol = r32(&k);
	g->cc_hi = r32(&k);
	g->cc_lo = r32(&k);
	g->prim = r32(&k);
	g->env = r32(&k);
	g->fog = r32(&k);
	g->blend = r32(&k);
	g->fill = r32(&k);
	g->prim_lod = r8(&k);
	g->prim_min = r8(&k);
	g->prim_z = r16(&k);
	g->prim_dz = r16(&k);
	for (unsigned i = 0; i < 4; i++)
		g->scissor[i] = r16(&k);
	g->cimg_addr = r32(&k);
	g->cimg_width = r16(&k);
	g->cimg_fmt = r8(&k);
	g->cimg_siz = r8(&k);
	g->zimg_addr = r32(&k);
	g->timg_addr = r32(&k);
	g->timg_width = r16(&k);
	g->timg_fmt = r8(&k);
	g->timg_siz = r8(&k);
	for (unsigned i = 0; i < 8; i++) {
		struct gfx_tile *t = &g->tiles[i];
		t->fmt = r8(&k);
		t->siz = r8(&k);
		t->pal = r8(&k);
		t->cms = r8(&k);
		t->cmt = r8(&k);
		t->masks = r8(&k);
		t->maskt = r8(&k);
		t->shifts = r8(&k);
		t->shiftt = r8(&k);
		t->line = r16(&k);
		t->tmem = r16(&k);
		t->uls = r16(&k);
		t->ult = r16(&k);
		t->lrs = r16(&k);
		t->lrt = r16(&k);
	}
	g->tex_gen++;  /* las texturas resueltas por tile ya no valen */
	return true;
}

_Static_assert(4 + 9 * 4 + 2 + 2 * 2 + 4 * 2 + 4 + 2 + 2 + 4 + 4 + 2 + 2 + 8 * (9 + 6 * 2) <= GFX_STATE_REGS,
	       "los registros del RDP no caben en GFX_STATE_REGS");
_Static_assert(sizeof(((struct gfx_state *)0)->tmem) == GFX_STATE_BYTES - GFX_STATE_REGS,
	       "la TMEM no es lo que resta de GFX_STATE_BYTES");

void gfx_state_save(const struct gfx_state *g, uint8_t *out)
{
	gfx_state_pack_regs(g, out);
	memcpy(out + GFX_STATE_REGS, g->tmem, sizeof g->tmem);
}

bool gfx_state_load(struct gfx_state *g, const uint8_t *in)
{
	if (!gfx_state_unpack_regs(g, in))
		return false;
	memcpy(g->tmem, in + GFX_STATE_REGS, sizeof g->tmem);
	return true;
}
