/*
 * Salsa64 — HLE gráfica: ejecución de una tarea y lo que comparten los
 * microcódigos (F3D, F3DEX y F3DEX2): recorrido de la display list, G_TEXRECT
 * en tres comandos y el estado que el RSP pasa al RDP (textura, geometry
 * mode y othermode).
 *
 * Cada familia decodifica sus comandos en su archivo (f3dex2.c, f3d.c) y
 * llama a estas funciones y a las de geom.c con los parámetros ya
 * extraídos. Ver hle.h.
 */
#include <string.h>

#include "core/rcp.h"
#include "rsp/gfx/hle.h"

#define G_TEXRECTFLIP 0xE5u

/* El microcódigo se carga con cada tarea: su estado empieza de cero. */
static void reset_rsp(struct gfx_state *g)
{
	memset(g->segments, 0, sizeof g->segments);
	g->geom = 0;
	g->dl_sp = 0;
	g->rdphalf1 = g->rdphalf2 = 0;
	g->raw_n = 0;
	g->tex_s = g->tex_t = 0;
	g->tex_tile = g->tex_level = g->tex_on = 0;
	gfx_geom_reset(g);
}

void gfx_run(struct gfx_state *g, const struct gfx_job *job, struct gfx_result *res)
{
	memset(res, 0, sizeof *res);
	uint32_t t_start = gfx_io_clock();
	g->job = job;
	g->res = res;
	if (job->flags & GFX_JOB_RESET)
		gfx_rdp_reset(g);
	reset_rsp(g);
	g->have_draw = false;
	g->target_sent = false;
	g->dirty = true;
	g->vtx_count = 0;
	g->narena_tex = 0;
	memset(g->tilememo, 0, sizeof g->tilememo);
	gfx_tc_begin(g);
	g->dl_pc = job->dl & 0x00FFFFFF;
	gfx_io_begin(g);

	if (job->gbi == SALSA64_GBI_F3D || job->gbi == SALSA64_GBI_F3DEX)
		gfx_dl_f3d(g);
	else
		gfx_dl_f3dex2(g);

	gfx_flush_draw(g);
	gfx_io_end(g);
	res->t_total = gfx_io_clock() - t_start;
}

bool gfx_dl_next(struct gfx_state *g, uint32_t *w0, uint32_t *w1)
{
	struct gfx_result *res = g->res;
	if (res->commands >= GFX_MAX_COMMANDS) {
		res->flags |= GFX_RES_LOOP;
		return false;
	}
	if (!gfx_io_dl(g, g->dl_pc, w0, w1)) {
		res->dl_errors++;
		return false;
	}
	g->dl_pc += 8;
	res->commands++;
	return true;
}

bool gfx_dl_call(struct gfx_state *g, uint32_t seg, bool branch)
{
	if (!branch) {
		if (g->dl_sp >= GFX_DL_STACK) {
			g->res->dl_errors++;
			return false;
		}
		g->dl_stack[g->dl_sp++] = g->dl_pc;
	}
	g->dl_pc = gfx_addr(g, seg);
	return true;
}

bool gfx_dl_end(struct gfx_state *g)
{
	if (g->dl_sp == 0)
		return false;
	g->dl_pc = g->dl_stack[--g->dl_sp];
	return true;
}

/* Tramo de otro microcódigo tras un G_LOAD_UCODE (Zelda OoT carga S2DEX
 * para el fondo prerrenderizado de una sala y después vuelve a F3DZEX):
 * sin HLE para él, se saltan sus comandos, siguiendo las llamadas y los
 * fines de lista (op_dl, op_enddl: la familia los comparte) y tomando el
 * G_RDPFULLSYNC (del RDP, común a todas), hasta el G_LOAD_UCODE (op_ucode)
 * que vuelve al microcódigo de la tarea. true: la lista sigue; false: se
 * acabó dentro del tramo (GFX_RES_FOREIGN_END, cuenta como cortada) o hubo
 * un error. Antes la tarea terminaba en el G_LOAD_UCODE sin
 * G_RDPFULLSYNC y el juego se quedaba esperando al RDP («RCP is HUNG UP»). */
bool gfx_dl_skip_foreign(struct gfx_state *g, uint32_t op_ucode, uint32_t op_dl, uint32_t op_enddl)
{
	struct gfx_result *res = g->res;
	uint32_t w0, w1;
	res->flags |= GFX_RES_LOAD_UCODE;
	while (gfx_dl_next(g, &w0, &w1)) {
		uint32_t op = w0 >> 24;
		res->skipped++;
		if (op == op_ucode && g->job->ucode && gfx_addr(g, w1) == g->job->ucode)
			return true;
		if (op == op_dl) {
			if (!gfx_dl_call(g, w1, (w0 >> 16) & 1))
				return false;
		} else if (op == op_enddl) {
			if (!gfx_dl_end(g)) {
				res->flags |= GFX_RES_FOREIGN_END;
				return false;
			}
		} else if (op == 0xE9) {  /* G_RDPFULLSYNC */
			res->flags |= GFX_RES_FULLSYNC;
		}
	}
	return false;
}

static bool op_in(uint32_t op, uint32_t set)
{
	for (; set; set >>= 8)
		if ((set & 0xFF) == op)
			return true;
	return false;
}

/* El comando que sigue, si su opcode está en `ops`: lo consume y deja su
 * segunda palabra en *w1. */
static bool take_half(struct gfx_state *g, uint32_t ops, uint32_t *w1)
{
	uint32_t w0;
	if (g->res->commands >= GFX_MAX_COMMANDS || !gfx_io_dl(g, g->dl_pc, &w0, w1) || !op_in(w0 >> 24, ops))
		return false;
	g->dl_pc += 8;
	g->res->commands++;
	return true;
}

void gfx_dl_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t ops_st, uint32_t ops_d)
{
	uint32_t st, d;
	if (!take_half(g, ops_st, &st))
		return;
	g->rdphalf1 = st;
	if (!take_half(g, ops_d, &d))
		return;
	g->rdphalf2 = d;
	gfx_texrect(g, w0, w1, st, d, (w0 >> 24) == G_TEXRECTFLIP);
}

void gfx_dl_rdp_word(struct gfx_state *g, uint32_t hi, uint32_t lo)
{
	if (g->raw_n == 0)
		g->raw_len = (uint8_t)gfx_rdp_raw_words(hi >> 24);
	g->raw[2 * g->raw_n] = hi;
	g->raw[2 * g->raw_n + 1] = lo;
	if (++g->raw_n < g->raw_len)
		return;
	g->raw_n = 0;
	uint32_t op = g->raw[0] >> 24;
	if (op >= 0xC8 && op <= 0xCF)
		gfx_rdp_triangle(g, g->raw);
	else if (op == G_TEXRECTFLIP || op == G_TEXRECTFLIP - 1)
		gfx_texrect(g, g->raw[0], g->raw[1], g->raw[2], g->raw[3], op == G_TEXRECTFLIP);
	else
		gfx_rdp_command(g, g->raw[0], g->raw[1]);
}

void gfx_branch_z(struct gfx_state *g, uint32_t v, uint32_t zval)
{
	if (v < GFX_MAX_VTX && (g->vtx[v].sz > 1023.0f || g->vtx[v].sz <= (float)zval))
		g->dl_pc = gfx_addr(g, g->rdphalf1);
}

void gfx_sp_texture(struct gfx_state *g, uint32_t scale, unsigned level, unsigned tile, unsigned on)
{
	uint16_t s = (uint16_t)(scale >> 16), t = (uint16_t)scale;
	uint8_t en = on != 0;
	if (en != g->tex_on || tile != g->tex_tile || s != g->tex_s || t != g->tex_t)
		g->dirty = true;
	g->tex_s = s;
	g->tex_t = t;
	g->tex_level = (uint8_t)level;
	g->tex_tile = (uint8_t)tile;
	g->tex_on = en;
}

void gfx_sp_geometry(struct gfx_state *g, uint32_t geom)
{
	if ((geom ^ g->geom) & (G_CULL_FRONT | G_CULL_BACK | G_FOG | G_ZBUFFER | G_SHADING_SMOOTH))
		g->dirty = true;
	g->geom = geom;
}

void gfx_sp_othermode(struct gfx_state *g, bool high, int shift, int len, uint32_t data)
{
	if (shift < 0 || shift >= 32 || len <= 0)
		return;
	if (len > 32 - shift)
		len = 32 - shift;
	uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
	uint32_t *mode = high ? &g->oh : &g->ol;
	uint32_t v = (*mode & ~mask) | (data & mask);
	if (v != *mode)
		g->dirty = true;
	*mode = v;
}
