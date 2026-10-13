/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE del microcódigo F3DEX2 (y su familia: F3DZEX2, F3DLX2…).
 *
 * Decodifica la display list de una tarea gráfica: los comandos del RSP
 * (vértices, matrices, luces, control de flujo) se resuelven con dl.c y
 * geom.c; los del RDP pasan a rdp.c. Codificación de los comandos: la GBI
 * pública de F3DEX2 (n64brew.dev, "F3DEX2") y el comportamiento de
 * mupen64plus/GLideN64 como referencia (docs/THIRD_PARTY.md).
 */
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

static void do_moveword(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t index = (w0 >> 16) & 0xFF, offset = w0 & 0xFFFF;
	switch (index) {
	case G_MW_SEGMENT:
		g->segments[(offset >> 2) & 15] = w1 & 0x00FFFFFF;
		break;
	case G_MW_NUMLIGHT:
		gfx_num_lights(g, (int)(w1 / 24));
		break;
	case G_MW_FOG:
		g->fog_mul = (float)(int16_t)(w1 >> 16);
		g->fog_off = (float)(int16_t)(w1 & 0xFFFF);
		break;
	case G_MW_LIGHTCOL:
		if ((offset % 24) == 0)
			gfx_light_color(g, (int)(offset / 24), w1);
		break;
	case G_MW_CLIP:
		gfx_clip_ratio(g, offset, w1);
		break;
	case G_MW_MATRIX:
		/* gSPInsertMatrix: Kirby 64 orienta así sus enemigos hacia la
		 * cámara (sin esto quedaban aplastados: sin cuerpo). */
		gfx_insert_matrix(g, offset, w1);
		break;
	case G_MW_FORCEMTX:
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
	case G_MV_MATRIX:
		gfx_force_matrix(g, addr);  /* gSPForceMatrix */
		break;
	default:
		break;
	}
}

static void tri(struct gfx_state *g, uint32_t w)
{
	gfx_triangle(g, ((w >> 16) & 0xFF) / 2, ((w >> 8) & 0xFF) / 2, (w & 0xFF) / 2);
}


void gfx_dl_f3dex2(struct gfx_state *g, gfx_cmd_hook hook)
{
	struct gfx_result *res = g->res;
	const bool zex = g->job->gbi == SALSA64_GBI_F3DZEX2;
	uint32_t w0, w1;

	while (gfx_dl_next(g, &w0, &w1)) {
		if (hook && hook(g, w0, w1))
			continue;
		uint32_t op = w0 >> 24;
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
			gfx_modify_vertex(g, (w0 & 0xFFFF) / 2, (w0 >> 16) & 0xFF, w1);
			break;
		case G_CULLDL:
			/* Todos fuera del mismo plano: la display list en curso termina. */
			if (gfx_vertices_culled(g, (w0 & 0xFFFF) / 2, (w1 & 0xFFFF) / 2) && !gfx_dl_end(g))
				return;
			break;
		case G_BRANCH_Z:
			if (zex) {
				/* F3DZEX: G_BRANCH_W, salta si w del vértice < w1. */
				uint32_t v = (w0 >> 1) & 0x7F;
				if (v < GFX_MAX_VTX && g->vtx[v].w < (float)(int32_t)w1)
					g->dl_pc = gfx_addr(g, g->rdphalf1);
			} else {
				gfx_branch_z(g, (w0 & 0xFFF) >> 1, w1);
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
		case G_TEXTURE:
			gfx_sp_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, (w0 >> 1) & 0x7F);
			break;
		case G_POPMTX:
			gfx_pop_matrix(g, w1 / 64);
			break;
		case G_GEOMETRYMODE:
			gfx_sp_geometry(g, (g->geom & (w0 | 0xFF000000u)) | w1);
			break;
		case G_MTX:
			/* El bit de PUSH va invertido en la palabra. */
			gfx_matrix(g, gfx_addr(g, w1), (w0 & 0xFF) ^ GFX_MTX_PUSH);
			break;
		case G_MOVEWORD:
			do_moveword(g, w0, w1);
			break;
		case G_MOVEMEM:
			do_movemem(g, w0, w1);
			break;
		case G_LOAD_UCODE:
			/* Otro microcódigo: S2DEX2 (el fondo prerrenderizado de Zelda
			 * OoT) se interpreta (s2dex.c); los demás se saltan (dl.c). El
			 * mismo, nada. */
			if (g->job->ucode && gfx_addr(g, w1) == g->job->ucode)
				break;
			if (gfx_ucode_is_s2dex2(g, gfx_addr(g, g->rdphalf1), (w0 & 0xFFFF) + 1) ? !gfx_dl_s2dex2(g)
												   : !gfx_dl_skip_foreign(g, G_LOAD_UCODE, G_DL, G_ENDDL))
				return;
			break;
		case G_DL:
			if (!gfx_dl_call(g, w1, (w0 >> 16) & 1))
				return;
			break;
		case G_ENDDL:
			if (!gfx_dl_end(g))
				return;
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
			int len = (int)(w0 & 0xFF) + 1;
			gfx_sp_othermode(g, op == G_SETOTHERMODE_H, 32 - (int)((w0 >> 8) & 0xFF) - len, len, w1);
			break;
		}
		case G_TEXRECT:
		case G_TEXRECTFLIP:
			gfx_dl_texrect(g, w0, w1, G_RDPHALF_1, G_RDPHALF_2);
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
}
