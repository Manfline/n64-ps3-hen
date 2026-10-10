/*
 * Salsa64 — HLE del microcódigo F3DEX2 (y su familia: F3DZEX2, F3DLX2…).
 *
 * Interpreta la display list de una tarea gráfica: los comandos del RSP
 * (vértices, matrices, luces, control de flujo) se resuelven aquí y en
 * geom.c; los del RDP pasan a rdp.c. Codificación de los comandos: la GBI
 * pública de F3DEX2 (n64brew.dev, "F3DEX2") y el comportamiento de
 * mupen64plus/GLideN64 como referencia (docs/THIRD_PARTY.md).
 */
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/gfx/hle.h"

enum {
	G_VTX = 0x01, G_MODIFYVTX = 0x02, G_CULLDL = 0x03, G_BRANCH_Z = 0x04,
	G_TRI1 = 0x05, G_TRI2 = 0x06, G_QUAD = 0x07,
	G_SPECIAL_3 = 0xD3, G_SPECIAL_2 = 0xD4, G_SPECIAL_1 = 0xD5, G_DMA_IO = 0xD6,
	G_TEXTURE = 0xD7, G_POPMTX = 0xD8, G_GEOMETRYMODE = 0xD9, G_MTX = 0xDA,
	G_MOVEWORD = 0xDB, G_MOVEMEM = 0xDC, G_LOAD_UCODE = 0xDD, G_DL = 0xDE,
	G_ENDDL = 0xDF, G_SPNOOP = 0xE0, G_RDPHALF_1 = 0xE1, G_SETOTHERMODE_L = 0xE2,
	G_SETOTHERMODE_H = 0xE3, G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5,
	G_RDPHALF_2 = 0xF1,
};

/* G_MOVEWORD */
enum {
	G_MW_MATRIX = 0x00, G_MW_NUMLIGHT = 0x02, G_MW_CLIP = 0x04, G_MW_SEGMENT = 0x06,
	G_MW_FOG = 0x08, G_MW_LIGHTCOL = 0x0A, G_MW_FORCEMTX = 0x0C, G_MW_PERSPNORM = 0x0E,
};

/* G_MOVEMEM */
enum { G_MV_MMTX = 2, G_MV_PMTX = 6, G_MV_VIEWPORT = 8, G_MV_LIGHT = 10, G_MV_POINT = 12, G_MV_MATRIX = 14 };

/* G_MTX (el bit de push va invertido en la palabra) */
#define G_MTX_PUSH       0x01u
#define G_MTX_LOAD       0x02u
#define G_MTX_PROJECTION 0x04u

/* G_MODIFYVTX */
enum { G_MWO_POINT_RGBA = 0x10, G_MWO_POINT_ST = 0x14, G_MWO_POINT_XYSCREEN = 0x18, G_MWO_POINT_ZSCREEN = 0x1C };

static void reset_rsp(struct gfx_state *g)
{
	memset(g->segments, 0, sizeof g->segments);
	g->geom = 0;
	g->dl_sp = 0;
	g->rdphalf1 = g->rdphalf2 = 0;
	g->tex_s = g->tex_t = 0;
	g->tex_tile = g->tex_level = g->tex_on = 0;
	gfx_geom_reset(g);
}

static void do_mtx(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	const uint8_t *src = gfx_io_read(g, GFX_IO_MTX, gfx_addr(g, w1), 64);
	if (!src) {
		g->res->dl_errors++;
		return;
	}
	uint32_t p = (w0 & 0xFF) ^ G_MTX_PUSH;
	float m[16];
	gfx_load_matrix(m, src);
	if (p & G_MTX_PROJECTION) {
		if (p & G_MTX_LOAD)
			memcpy(g->proj, m, sizeof m);
		else
			gfx_mtx_mul(g->proj, m, g->proj);
	} else {
		if ((p & G_MTX_PUSH) && g->mv_top + 1 < GFX_MTX_STACK) {
			memcpy(g->mv[g->mv_top + 1], g->mv[g->mv_top], sizeof m);
			g->mv_top++;
		}
		if (p & G_MTX_LOAD)
			memcpy(g->mv[g->mv_top], m, sizeof m);
		else
			gfx_mtx_mul(g->mv[g->mv_top], m, g->mv[g->mv_top]);
		g->lights_dirty = true;
	}
	g->mvp_dirty = true;
}

static void do_moveword(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t index = (w0 >> 16) & 0xFF, offset = w0 & 0xFFFF;
	switch (index) {
	case G_MW_SEGMENT:
		g->segments[(offset >> 2) & 15] = w1 & 0x00FFFFFF;
		break;
	case G_MW_NUMLIGHT:
		g->num_lights = (int)(w1 / 24);
		if (g->num_lights > GFX_MAX_LIGHTS)
			g->num_lights = GFX_MAX_LIGHTS;
		g->lights_dirty = true;
		break;
	case G_MW_FOG:
		g->fog_mul = (float)(int16_t)(w1 >> 16);
		g->fog_off = (float)(int16_t)(w1 & 0xFFFF);
		break;
	case G_MW_LIGHTCOL: {
		int light = (int)(offset / 24);
		if (light <= GFX_MAX_LIGHTS && (offset % 24) == 0) {
			g->lights[light].col[0] = (float)(w1 >> 24);
			g->lights[light].col[1] = (float)((w1 >> 16) & 0xFF);
			g->lights[light].col[2] = (float)((w1 >> 8) & 0xFF);
		}
		break;
	}
	case G_MW_MATRIX:
	case G_MW_FORCEMTX:
	case G_MW_CLIP:
	case G_MW_PERSPNORM:
	default:
		break;
	}
}

static void do_movemem(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t index = w0 & 0xFF;
	uint32_t offset = ((w0 >> 8) & 0xFF) * 8;
	uint32_t len = ((w0 >> 19) & 0x1F) * 8 + 8;
	uint32_t addr = gfx_addr(g, w1);
	switch (index) {
	case G_MV_VIEWPORT: {
		const uint8_t *vp = gfx_io_read(g, GFX_IO_SMALL, addr, 16);
		if (vp)
			gfx_set_viewport(g, vp);
		break;
	}
	case G_MV_LIGHT: {
		/* offset/24: 0 lookat X, 1 lookat Y, 2.. luces 0.. */
		int n = (int)(len / 24) ? (int)(len / 24) : 1;
		const uint8_t *src = gfx_io_read(g, GFX_IO_SMALL, addr, (uint32_t)n * 24);
		if (!src)
			break;
		for (int i = 0; i < n; i++) {
			int slot = (int)(offset / 24) + i;
			if (slot < 2)
				gfx_set_lookat(g, slot, src + 24 * i);
			else if (slot - 2 <= GFX_MAX_LIGHTS)
				gfx_set_light(g, slot - 2, src + 24 * i);
		}
		break;
	}
	case G_MV_MATRIX: {
		/* Fuerza la matriz combinada (gSPForceMatrix). */
		const uint8_t *src = gfx_io_read(g, GFX_IO_MTX, addr, 64);
		if (src) {
			gfx_load_matrix(g->mvp, src);
			g->mvp_dirty = false;
		}
		break;
	}
	default:
		break;
	}
}

static void do_modifyvtx(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t where = (w0 >> 16) & 0xFF, v = (w0 & 0xFFFF) / 2;
	if (v >= GFX_MAX_VTX)
		return;
	struct gfx_vtx *p = &g->vtx[v];
	switch (where) {
	case G_MWO_POINT_RGBA:
		p->c[0] = (uint8_t)(w1 >> 24);
		p->c[1] = (uint8_t)(w1 >> 16);
		p->c[2] = (uint8_t)(w1 >> 8);
		p->c[3] = (uint8_t)w1;
		break;
	case G_MWO_POINT_ST:
		p->s = (float)(int16_t)(w1 >> 16) / 32.0f;
		p->t = (float)(int16_t)(w1 & 0xFFFF) / 32.0f;
		break;
	case G_MWO_POINT_XYSCREEN: {
		/* Posición de pantalla nueva (10.2): se vuelve a llevar a recorte. */
		float sx = (float)(int16_t)(w1 >> 16) / 4.0f, sy = (float)(int16_t)(w1 & 0xFFFF) / 4.0f;
		if (p->w != 0.0f && g->vscale[0] != 0.0f && g->vscale[1] != 0.0f) {
			p->x = (sx - g->vtrans[0]) / g->vscale[0] * p->w;
			p->y = (g->vtrans[1] - sy) / g->vscale[1] * p->w;
		}
		p->sx = sx;
		p->sy = sy;
		break;
	}
	case G_MWO_POINT_ZSCREEN: {
		float sz = (float)(w1 >> 16);
		if (p->w != 0.0f && g->vscale[2] != 0.0f)
			p->z = (sz - g->vtrans[2]) / g->vscale[2] * p->w;
		p->sz = sz;
		break;
	}
	default:
		break;
	}
}

/* G_CULLDL: si todos los vértices del rango quedan fuera del mismo plano,
 * la display list en curso termina. */
static bool culled(const struct gfx_state *g, uint32_t v0, uint32_t v1)
{
	uint32_t all = 0x3F;
	if (v0 >= GFX_MAX_VTX)
		return false;
	if (v1 >= GFX_MAX_VTX)
		v1 = GFX_MAX_VTX - 1;
	for (uint32_t i = v0; i <= v1; i++)
		all &= g->vtx[i].clip;
	return all != 0;
}

static void tri(struct gfx_state *g, uint32_t w)
{
	gfx_triangle(g, ((w >> 16) & 0xFF) / 2, ((w >> 8) & 0xFF) / 2, (w & 0xFF) / 2);
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
	g->ntexcache = 0;
	gfx_io_begin(g);

	uint32_t pc = job->dl & 0x00FFFFFF;
	uint32_t texrect_w0 = 0, texrect_w1 = 0;
	int texrect = 0;  /* palabras de G_TEXRECT pendientes */
	bool flip = false;

	for (uint32_t n = 0;; n++) {
		uint32_t w0, w1;
		if (n >= GFX_MAX_COMMANDS) {
			res->flags |= GFX_RES_LOOP;
			break;
		}
		if (!gfx_io_dl(g, pc, &w0, &w1)) {
			res->dl_errors++;
			break;
		}
		pc += 8;
		res->commands++;
		uint32_t op = w0 >> 24;

		/* G_TEXRECT llega en tres comandos: el rectángulo y dos mitades
		 * (G_RDPHALF_1: s, t; G_RDPHALF_2: dsdx, dtdy). */
		if (texrect) {
			if (op == G_RDPHALF_1 && texrect == 2) {
				g->rdphalf1 = w1;
				texrect = 1;
				continue;
			}
			if (op == G_RDPHALF_2 && texrect == 1) {
				g->rdphalf2 = w1;
				gfx_texrect(g, texrect_w0, texrect_w1, g->rdphalf1, w1, flip);
				texrect = 0;
				continue;
			}
			texrect = 0;  /* incompleto: se descarta */
		}

		switch (op) {
		case G_VTX: {
			uint32_t count = (w0 >> 12) & 0xFF;
			uint32_t end = (w0 >> 1) & 0x7F;
			if (count <= end) {
				uint32_t t0 = gfx_io_clock();
				gfx_load_vertices(g, gfx_addr(g, w1), end - count, count);
				res->t_vtx += gfx_io_clock() - t0;
			}
			break;
		}
		case G_MODIFYVTX:
			do_modifyvtx(g, w0, w1);
			break;
		case G_CULLDL:
			if (culled(g, (w0 & 0xFFFF) / 2, (w1 & 0xFFFF) / 2))
				goto enddl;
			break;
		case G_BRANCH_Z:
			if (job->gbi == SALSA64_GBI_F3DZEX2) {
				/* F3DZEX: G_BRANCH_W, salta si w del vértice < w1. */
				uint32_t v = (w0 >> 1) & 0x7F;
				if (v < GFX_MAX_VTX && g->vtx[v].w < (float)(int32_t)w1)
					pc = gfx_addr(g, g->rdphalf1);
			} else {
				/* Salta si la z de pantalla del vértice (0..0x3FF) <= w1. */
				uint32_t v = (w0 & 0xFFF) >> 1;
				if (v < GFX_MAX_VTX && (g->vtx[v].sz > 1023.0f || g->vtx[v].sz <= (float)w1))
					pc = gfx_addr(g, g->rdphalf1);
			}
			break;
		case G_TRI1:
			tri(g, w0);
			break;
		case G_TRI2:
			tri(g, w0);
			tri(g, w1);
			break;
		case G_QUAD: {
			uint32_t a = ((w0 >> 16) & 0xFF) / 2, b = ((w0 >> 8) & 0xFF) / 2, c = (w0 & 0xFF) / 2;
			uint32_t d = ((w1 >> 16) & 0xFF) / 2, e = ((w1 >> 8) & 0xFF) / 2, f = (w1 & 0xFF) / 2;
			gfx_triangle(g, a, b, c);
			gfx_triangle(g, d, e, f);
			break;
		}
		case G_TEXTURE: {
			uint8_t on = (w0 >> 1) & 0x7F;
			uint8_t tile = (w0 >> 8) & 7;
			if (on != g->tex_on || tile != g->tex_tile || (w1 >> 16) != g->tex_s ||
			    (w1 & 0xFFFF) != g->tex_t)
				g->dirty = true;
			g->tex_s = (uint16_t)(w1 >> 16);
			g->tex_t = (uint16_t)w1;
			g->tex_level = (w0 >> 11) & 7;
			g->tex_tile = tile;
			g->tex_on = on;
			break;
		}
		case G_POPMTX: {
			uint32_t k = w1 / 64;
			g->mv_top = k > (uint32_t)g->mv_top ? 0 : g->mv_top - (int)k;
			g->mvp_dirty = true;
			g->lights_dirty = true;
			break;
		}
		case G_GEOMETRYMODE: {
			uint32_t geom = (g->geom & (w0 | 0xFF000000u)) | w1;
			if ((geom ^ g->geom) & (G_CULL_FRONT | G_CULL_BACK | G_FOG | G_ZBUFFER | G_SHADING_SMOOTH))
				g->dirty = true;
			g->geom = geom;
			break;
		}
		case G_MTX:
			do_mtx(g, w0, w1);
			break;
		case G_MOVEWORD:
			do_moveword(g, w0, w1);
			break;
		case G_MOVEMEM:
			do_movemem(g, w0, w1);
			break;
		case G_LOAD_UCODE:
			res->flags |= GFX_RES_LOAD_UCODE;
			goto done;
		case G_DL:
			if (!((w0 >> 16) & 1)) {  /* llamada: se guarda el regreso */
				if (g->dl_sp >= GFX_DL_STACK) {
					res->dl_errors++;
					goto done;
				}
				g->dl_stack[g->dl_sp++] = pc;
			}
			pc = gfx_addr(g, w1);
			break;
		case G_ENDDL:
		enddl:
			if (g->dl_sp == 0)
				goto done;
			pc = g->dl_stack[--g->dl_sp];
			break;
		case G_SPNOOP:
		case G_SPECIAL_1:
		case G_SPECIAL_2:
		case G_SPECIAL_3:
		case G_DMA_IO:
			break;
		case G_RDPHALF_1:
			g->rdphalf1 = w1;
			break;
		case G_RDPHALF_2:
			g->rdphalf2 = w1;
			break;
		case G_SETOTHERMODE_L:
		case G_SETOTHERMODE_H: {
			uint32_t len = (w0 & 0xFF) + 1;
			uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
			uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
			uint32_t *mode = op == G_SETOTHERMODE_L ? &g->ol : &g->oh;
			uint32_t v = (*mode & ~mask) | (w1 & mask);
			if (v != *mode)
				g->dirty = true;
			*mode = v;
			break;
		}
		case G_TEXRECT:
		case G_TEXRECTFLIP:
			texrect_w0 = w0;
			texrect_w1 = w1;
			flip = op == G_TEXRECTFLIP;
			texrect = 2;
			break;
		default:
			if (op >= 0xE4) {
				gfx_rdp_command(g, w0, w1);
			} else {
				res->unknown_cmds++;
				res->last_unknown = op;
			}
			break;
		}
	}
done:
	gfx_flush_draw(g);
	gfx_io_end(g);
	res->t_total = gfx_io_clock() - t_start;
}
