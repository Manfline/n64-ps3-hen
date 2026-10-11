/*
 * Salsa64 — HLE de los microcódigos de la primera generación: Fast3D (F3D;
 * Super Mario 64 y los primeros juegos) y F3DEX 1.x (y F3DLX, F3DLP, que
 * solo cambian el recorte).
 *
 * Comparten la GBI anterior a F3DEX2: comandos inmediatos del 0xBF hacia
 * abajo, G_MTX con sus banderas sin invertir, G_MOVEMEM y G_MOVEWORD con
 * índices propios (luces de 32 bytes en la DMEM, número de luces dado como
 * dirección), geometry mode en dos comandos (G_SETGEOMETRYMODE y
 * G_CLEARGEOMETRYMODE) con otros bits para el sombreado suave y el
 * culling, G_SETOTHERMODE_* con el desplazamiento y el largo tal cual, y
 * G_TEXTURE con el encendido en el byte bajo. Difieren en los vértices: F3D
 * tiene un búfer de 16 y da los índices ×10; F3DEX, uno de 32, índices ×2 y
 * comandos nuevos (G_TRI2, G_QUAD, G_MODIFYVTX, G_BRANCH_Z, G_LOAD_UCODE)
 * en lugar de G_RDPHALF_CONT y G_LINE3D de F3D. Fast3D 2.0G (GoldenEye)
 * usa el 0xB1 para G_TRI4: cuatro triángulos con índices de 4 bits.
 *
 * G_TEXRECT llega seguido de dos G_RDPHALF_*: G_RDPHALF_2 y G_RDPHALF_CONT
 * (SM64, Mario Kart 64) o G_RDPHALF_1 y G_RDPHALF_2 (libultra 2.0).
 *
 * En el SPE 0 son 3,5 KB de código y nada de estado propio (usa la
 * gfx_state de siempre); presupuesto completo en src/spu/gfx/main.c.
 *
 * Fuentes (docs/THIRD_PARTY.md): n64brew.dev y la documentación de la
 * comunidad de SM64 sobre Fast3D para la codificación; GLideN64 como
 * referencia de comportamiento (solo consulta, sin copiar código).
 */
#include "core/rcp.h"
#include "rsp/gfx/hle.h"

enum {
	G_SPNOOP = 0x00, G_MTX = 0x01, G_MOVEMEM = 0x03, G_VTX = 0x04, G_DL = 0x06,
	G_LOAD_UCODE = 0xAF, G_BRANCH_Z = 0xB0,
	G_TRI2 = 0xB1,          /* F3D 2.0G (GoldenEye): G_TRI4 */
	G_MODIFYVTX = 0xB2,     /* F3D: G_RDPHALF_CONT */
	G_RDPHALF_2 = 0xB3, G_RDPHALF_1 = 0xB4,
	G_QUAD = 0xB5,          /* F3D: G_LINE3D */
	G_CLEARGEOMETRYMODE = 0xB6, G_SETGEOMETRYMODE = 0xB7, G_ENDDL = 0xB8,
	G_SETOTHERMODE_L = 0xB9, G_SETOTHERMODE_H = 0xBA, G_TEXTURE = 0xBB, G_MOVEWORD = 0xBC,
	G_POPMTX = 0xBD, G_CULLDL = 0xBE, G_TRI1 = 0xBF,
	G_NOOP = 0xC0,          /* del RDP */
	G_TEXRECT = 0xE4, G_TEXRECTFLIP = 0xE5,
};
#define G_RDPHALF_CONT G_MODIFYVTX

/* G_MTX */
#define F3D_MTX_PROJECTION 0x01u
#define F3D_MTX_LOAD       0x02u
#define F3D_MTX_PUSH       0x04u

/* G_MOVEMEM: el índice va en el byte 2 de la primera palabra. Las luces van
 * de G_MV_L0 a G_MV_L7 de 2 en 2; gSPForceMatrix manda la matriz en cuatro
 * partes de 16 bytes y la primera es G_MV_MATRIX_1. */
enum {
	G_MV_VIEWPORT = 0x80, G_MV_LOOKATY = 0x82, G_MV_LOOKATX = 0x84, G_MV_L0 = 0x86, G_MV_L7 = 0x94,
	G_MV_TXTATT = 0x96, G_MV_MATRIX_2 = 0x98, G_MV_MATRIX_3 = 0x9A, G_MV_MATRIX_4 = 0x9C, G_MV_MATRIX_1 = 0x9E,
};

/* G_MOVEWORD: el índice va en el byte bajo y el offset en los 16 bits de
 * arriba. */
enum {
	G_MW_MATRIX = 0x00, G_MW_NUMLIGHT = 0x02, G_MW_CLIP = 0x04, G_MW_SEGMENT = 0x06, G_MW_FOG = 0x08,
	G_MW_LIGHTCOL = 0x0A, G_MW_POINTS = 0x0C, G_MW_PERSPNORM = 0x0E,
};

/* Geometry mode: estos tres bits están en otro lugar que en F3DEX2. */
#define F3D_SHADING_SMOOTH 0x00000200u
#define F3D_CULL_FRONT     0x00001000u
#define F3D_CULL_BACK      0x00002000u

/* Geometry mode de F3D/F3DEX → el de F3DEX2, que es el que guarda la HLE. */
static uint32_t geom_bits(uint32_t m)
{
	uint32_t r = m & (G_ZBUFFER | G_SHADE | G_FOG | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_LOD |
			  G_CLIPPING);
	if (m & F3D_SHADING_SMOOTH)
		r |= G_SHADING_SMOOTH;
	if (m & F3D_CULL_FRONT)
		r |= G_CULL_FRONT;
	if (m & F3D_CULL_BACK)
		r |= G_CULL_BACK;
	return r;
}

/* Índice de vértice de un byte de comando: ×10 en F3D, ×2 en F3DEX.
 * x·205 >> 11 es x/10 exacto para x < 1029 (sin división en el SPE). */
static uint32_t vtx_index(uint32_t byte, bool ex)
{
	return ex ? byte >> 1 : (byte * 205u) >> 11;
}

/* Triángulo con los índices en los bytes 2, 1 y 0 de `w`. En F3D el byte 3
 * dice qué vértice da el color del sombreado plano; la HLE lo toma del
 * primero, así que se rota (la orientación no cambia). F3DEX ya los manda
 * rotados. */
static void tri(struct gfx_state *g, uint32_t w, bool ex)
{
	uint32_t a = vtx_index((w >> 16) & 0xFF, ex), b = vtx_index((w >> 8) & 0xFF, ex);
	uint32_t c = vtx_index(w & 0xFF, ex);
	switch (ex ? 0 : w >> 24) {
	case 1:
		gfx_triangle(g, b, c, a);
		break;
	case 2:
		gfx_triangle(g, c, a, b);
		break;
	default:
		gfx_triangle(g, a, b, c);
		break;
	}
}

/* G_TRI4 (F3D 2.0G): el triángulo k va en el nibble k de w0 y los nibbles
 * 2k y 2k+1 de w1; los que no se usan repiten un índice (área nula). */
static void tri4(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	for (int k = 0; k < 4; k++) {
		uint32_t a = (w0 >> (4 * k)) & 15, b = (w1 >> (8 * k)) & 15, c = (w1 >> (8 * k + 4)) & 15;
		if (a != b && b != c && a != c)
			gfx_triangle(g, a, b, c);
	}
}

static void movemem(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t index = (w0 >> 16) & 0xFF;
	uint32_t addr = gfx_addr(g, w1);
	const uint8_t *src;
	switch (index) {
	case G_MV_VIEWPORT:
		if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
			gfx_set_viewport(g, src);
		break;
	case G_MV_LOOKATX:
	case G_MV_LOOKATY:
		if ((src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
			gfx_set_lookat(g, index == G_MV_LOOKATX ? 0 : 1, src);
		break;
	case G_MV_MATRIX_1:
		/* Lee las cuatro partes de una vez: las otras tres siguen en la
		 * memoria y sus comandos no hacen falta. */
		gfx_force_matrix(g, addr);
		break;
	case G_MV_MATRIX_2:
	case G_MV_MATRIX_3:
	case G_MV_MATRIX_4:
	case G_MV_TXTATT:
		break;
	default:
		/* Luz i: la ambiental va después de las direccionales. */
		if (index >= G_MV_L0 && index <= G_MV_L7 && !(index & 1) &&
		    (src = gfx_io_read(g, GFX_IO_SMALL, addr, 16)) != NULL)
			gfx_set_light(g, (int)(index - G_MV_L0) / 2, src);
		break;
	}
}

static void moveword(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t offset = (w0 >> 8) & 0xFFFF;
	switch (w0 & 0xFF) {
	case G_MW_MATRIX:
		gfx_insert_matrix(g, offset, w1);
		break;
	case G_MW_NUMLIGHT:
		/* Fin de las luces en la DMEM: 0x80000000 + 32·(n + 1). */
		if (w1 >= 0x80000020u)
			gfx_num_lights(g, (int)((w1 - 0x80000000u) >> 5) - 1);
		break;
	case G_MW_SEGMENT:
		g->segments[(offset >> 2) & 15] = w1 & 0x00FFFFFF;
		break;
	case G_MW_FOG:
		g->fog_mul = (float)(int16_t)(w1 >> 16);
		g->fog_off = (float)(int16_t)(w1 & 0xFFFF);
		break;
	case G_MW_LIGHTCOL:
		/* 32 bytes por luz; +0 el color, +4 su copia (no se usa aparte). */
		if ((offset & 0x1F) == 0)
			gfx_light_color(g, (int)(offset >> 5), w1);
		break;
	case G_MW_POINTS:
		/* 40 bytes por vértice en la DMEM. */
		gfx_modify_vertex(g, offset / 40, offset % 40, w1);
		break;
	case G_MW_CLIP:
	case G_MW_PERSPNORM:
	default:
		break;
	}
}

void gfx_dl_f3d(struct gfx_state *g, gfx_cmd_hook hook)
{
	struct gfx_result *res = g->res;
	const bool ex = g->job->gbi == SALSA64_GBI_F3DEX;
	uint32_t w0, w1;

	while (gfx_dl_next(g, &w0, &w1)) {
		if (hook && hook(g, w0, w1))
			continue;
		uint32_t op = w0 >> 24;
		switch (op) {
		case G_VTX: {
			uint32_t v0, n;
			if (ex) {
				v0 = (w0 >> 17) & 0x7F;
				n = (w0 >> 10) & 0x3F;
			} else {
				v0 = (w0 >> 16) & 0xF;
				n = ((w0 >> 20) & 0xF) + 1;
			}
			uint32_t t0 = gfx_io_clock();
			gfx_load_vertices(g, gfx_addr(g, w1), v0, n);
			res->t_vtx += gfx_io_clock() - t0;
			break;
		}
		case G_TRI1:
			tri(g, w1, ex);
			break;
		case G_MTX: {
			uint32_t p = (w0 >> 16) & 0xFF;
			uint32_t flags = ((p & F3D_MTX_PROJECTION) ? GFX_MTX_PROJECTION : 0) |
					 ((p & F3D_MTX_LOAD) ? GFX_MTX_LOAD : 0) | ((p & F3D_MTX_PUSH) ? GFX_MTX_PUSH : 0);
			gfx_matrix(g, gfx_addr(g, w1), flags);
			break;
		}
		case G_POPMTX:
			/* Solo la modelview tiene pila. */
			if (!(w1 & F3D_MTX_PROJECTION))
				gfx_pop_matrix(g, 1);
			break;
		case G_MOVEMEM:
			movemem(g, w0, w1);
			break;
		case G_MOVEWORD:
			moveword(g, w0, w1);
			break;
		case G_TEXTURE:
			gfx_sp_texture(g, w1, (w0 >> 11) & 7, (w0 >> 8) & 7, w0 & 0xFF);
			break;
		case G_SETGEOMETRYMODE:
			gfx_sp_geometry(g, g->geom | geom_bits(w1));
			break;
		case G_CLEARGEOMETRYMODE:
			gfx_sp_geometry(g, g->geom & ~geom_bits(w1));
			break;
		case G_SETOTHERMODE_L:
		case G_SETOTHERMODE_H:
			gfx_sp_othermode(g, op == G_SETOTHERMODE_H, (int)((w0 >> 8) & 0xFF), (int)(w0 & 0xFF), w1);
			break;
		case G_CULLDL: {
			/* F3D: offsets de 40 bytes y el final exclusivo; F3DEX: ×2. */
			bool out = ex ? gfx_vertices_culled(g, (w0 & 0xFFFF) / 2, (w1 & 0xFFFF) / 2)
				      : w1 >= 40 && gfx_vertices_culled(g, (w0 & 0xFFFFFF) / 40, w1 / 40 - 1);
			if (out && !gfx_dl_end(g))
				return;
			break;
		}
		case G_DL:
			if (!gfx_dl_call(g, w1, (w0 >> 16) & 1))
				return;
			break;
		case G_ENDDL:
			if (!gfx_dl_end(g))
				return;
			break;
		case G_RDPHALF_1:
			g->rdphalf1 = w1;
			break;
		case G_RDPHALF_2:
			g->rdphalf2 = w1;
			/* F3D: G_RDPHALF_1 + G_RDPHALF_2 o G_RDPHALF_CONT pasan al
			 * RDP una doble palabra (GoldenEye arma así triángulos crudos
			 * para el cielo). */
			if (!ex)
				gfx_dl_rdp_word(g, g->rdphalf1, w1);
			break;
		case G_TEXRECT:
		case G_TEXRECTFLIP:
			/* G_RDPHALF_1 + G_RDPHALF_2 (libultra 2.0) o G_RDPHALF_2 +
			 * G_RDPHALF_CONT (la anterior: SM64 y también F3DEX 0.95 en
			 * Mario Kart 64, aunque en F3DEX 1.x el 0xB2 sea G_MODIFYVTX). */
			gfx_dl_texrect(g, w0, w1, G_RDPHALF_1 | G_RDPHALF_2 << 8, G_RDPHALF_2 | G_RDPHALF_CONT << 8);
			break;
		case G_SPNOOP:
		case G_NOOP:
			break;
		/* Lo que cambia entre F3D y F3DEX. */
		case G_MODIFYVTX:
			if (ex)
				gfx_modify_vertex(g, (w0 & 0xFFFF) / 2, (w0 >> 16) & 0xFF, w1);
			else  /* F3D: G_RDPHALF_CONT */
				gfx_dl_rdp_word(g, g->rdphalf1, w1);
			break;
		case G_LOAD_UCODE:
			if (ex) {
				/* Otro microcódigo: se salta su tramo (dl.c). */
				if (!(g->job->ucode && gfx_addr(g, w1) == g->job->ucode) &&
				    !gfx_dl_skip_foreign(g, G_LOAD_UCODE, G_DL, G_ENDDL))
					return;
				break;
			}
			res->unknown_cmds++;  /* F3D no lo tiene */
			res->last_unknown = op;
			break;
		case G_TRI2:
		case G_QUAD:
		case G_BRANCH_Z:
			if (!ex && op == G_TRI2) {
				tri4(g, w0, w1);
			} else if (!ex) {
				/* G_LINE3D y los opcodes que F3D no tiene. */
				res->unknown_cmds++;
				res->last_unknown = op;
			} else if (op == G_TRI2) {
				tri(g, w0, true);
				tri(g, w1, true);
			} else if (op == G_QUAD) {
				/* gSP1Quadrangle: v0 v1 v2 v3 en los cuatro bytes. */
				uint32_t a = vtx_index(w1 >> 24, true), b = vtx_index((w1 >> 16) & 0xFF, true);
				uint32_t c = vtx_index((w1 >> 8) & 0xFF, true), d = vtx_index(w1 & 0xFF, true);
				gfx_triangle(g, a, b, c);
				gfx_triangle(g, a, c, d);
			} else if (op == G_BRANCH_Z) {
				gfx_branch_z(g, (w0 & 0xFFF) >> 1, w1);
			} else {
				res->flags |= GFX_RES_LOAD_UCODE;
				return;
			}
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
