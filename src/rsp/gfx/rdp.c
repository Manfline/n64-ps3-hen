/*
 * Salsa64 — HLE del RDP: estado (othermode, combiner, colores, tiles,
 * imágenes), rectángulos y salida de triángulos agrupados por estado.
 *
 * Cada primitiva se dibuja con el estado vigente; ese estado se resume en un
 * gfx_draw (modo de mezcla, profundidad, texturas decodificadas, combiner
 * ya compilado) y las primitivas seguidas con el mismo resumen comparten
 * registro. Así un cuadro típico produce unos cientos de dibujos para el
 * RSX y no uno por triángulo.
 *
 * Codificación de los comandos y significado de los bits: n64brew.dev
 * (Reality Display Processor/Commands).
 */
#include <string.h>

#include "rsp/gfx/hle.h"

enum {
	RDP_TRI_FIRST = 0xC8, RDP_TRI_LAST = 0xCF,
	G_RDPLOADSYNC = 0xE6, G_RDPPIPESYNC = 0xE7, G_RDPTILESYNC = 0xE8, G_RDPFULLSYNC = 0xE9,
	G_SETKEYGB = 0xEA, G_SETKEYR = 0xEB, G_SETCONVERT = 0xEC, G_SETSCISSOR = 0xED,
	G_SETPRIMDEPTH = 0xEE, G_RDPSETOTHERMODE = 0xEF, G_LOADTLUT = 0xF0,
	G_SETTILESIZE = 0xF2, G_LOADBLOCK = 0xF3, G_LOADTILE = 0xF4, G_SETTILE = 0xF5,
	G_FILLRECT = 0xF6, G_SETFILLCOLOR = 0xF7, G_SETFOGCOLOR = 0xF8, G_SETBLENDCOLOR = 0xF9,
	G_SETPRIMCOLOR = 0xFA, G_SETENVCOLOR = 0xFB, G_SETCOMBINE = 0xFC, G_SETTIMG = 0xFD,
	G_SETZIMG = 0xFE, G_SETCIMG = 0xFF,
};

_Static_assert(sizeof(struct gfx_vertex) == 32, "gfx_vertex: 32 bytes");
_Static_assert(sizeof(struct gfx_draw) % 16 == 0, "gfx_draw: múltiplo de 16");
_Static_assert(sizeof(struct gfx_zclear) % 16 == 0, "gfx_zclear: múltiplo de 16");
_Static_assert(sizeof(struct gfx_job) <= 112, "gfx_job cabe en un trabajo de spukit");
_Static_assert(sizeof(struct gfx_result) <= 112, "gfx_result cabe en un resultado de spukit");

/* Clases de primitiva: cambian partes del estado (los rectángulos no se
 * descartan por orientación ni usan la profundidad de los vértices). */
enum prim_kind { PRIM_TRI, PRIM_RECT, PRIM_FILL };

/* Mezcla del blender (othermode L, bits 16–31). */
enum { BL_IN = 0, BL_MEM = 1, BL_BL = 2, BL_FOG = 3 };
enum { BA_IN = 0, BA_FOG = 1, BA_SHADE = 2, BA_ZERO = 3 };
enum { BB_1MA = 0, BB_MEM = 1, BB_ONE = 2, BB_ZERO = 3 };

void gfx_rdp_reset(struct gfx_state *g)
{
	g->oh = 0;
	g->ol = 0;
	g->cc_hi = g->cc_lo = 0;
	g->prim = g->env = g->fog = g->blend = g->fill = 0;
	g->prim_lod = g->prim_min = 0;
	g->prim_z = g->prim_dz = 0;
	g->scissor[0] = g->scissor[1] = 0;
	g->scissor[2] = 320 << 2;
	g->scissor[3] = 240 << 2;
	g->cimg_addr = 0;
	g->cimg_width = 320;
	g->cimg_fmt = G_IM_FMT_RGBA;
	g->cimg_siz = G_IM_SIZ_16b;
	g->zimg_addr = 0;
	g->timg_addr = 0;
	g->timg_width = 1;
	g->timg_fmt = g->timg_siz = 0;
	memset(g->tiles, 0, sizeof g->tiles);
	memset(g->tmem, 0, sizeof g->tmem);
	memset(g->tmem_key, 0, sizeof g->tmem_key);
	g->load_seq = 0;
}

void gfx_init(struct gfx_state *g)
{
	memset(g, 0, sizeof *g);
	gfx_rdp_reset(g);
	gfx_geom_reset(g);
}

static void rgba8(uint32_t c, uint8_t out[4])
{
	out[0] = (uint8_t)(c >> 24);
	out[1] = (uint8_t)(c >> 16);
	out[2] = (uint8_t)(c >> 8);
	out[3] = (uint8_t)c;
}

/* Color de relleno del RDP en RGBA8888 según el tamaño de la color image. */
static uint32_t fill_rgba(const struct gfx_state *g)
{
	if (g->cimg_siz == G_IM_SIZ_32b)
		return g->fill;
	uint32_t p = g->fill & 0xFFFF;
	uint32_t r = (p >> 11) & 31, gr = (p >> 6) & 31, b = (p >> 1) & 31;
	r = r << 3 | r >> 2;
	gr = gr << 3 | gr >> 2;
	b = b << 3 | b >> 2;
	return r << 24 | gr << 16 | b << 8 | ((p & 1) ? 0xFF : 0);
}

/* Factor del blender como k + ka·α (α = alfa del color que entra). */
struct bl_factor {
	float k, ka;
};

static bool bl_eq(struct bl_factor f, float k, float ka)
{
	return f.k > k - 1e-3f && f.k < k + 1e-3f && f.ka > ka - 1e-3f && f.ka < ka + 1e-3f;
}

/* P·A + M·B (n64brew.dev: Set Other Modes) llevado a lo que hace el RSX:
 * se suman los factores del color que entra (IN) y del de la memoria (MEM).
 * A_SHADE se toma como el alfa que entra; MEM_A como 1; los colores
 * constantes (blend, fog) en P o M se ignoran. */
static uint8_t classify_blend(unsigned p, unsigned a, unsigned m, unsigned b, uint32_t fog)
{
	struct bl_factor fa = { 0.0f, 0.0f }, fb = { 0.0f, 0.0f };
	switch (a) {
	case BA_IN:
	case BA_SHADE: fa.ka = 1.0f; break;
	case BA_FOG:   fa.k = (fog & 0xFF) / 255.0f; break;
	default:       break;
	}
	switch (b) {
	case BB_1MA: fb.k = 1.0f - fa.k, fb.ka = -fa.ka; break;
	case BB_MEM:
	case BB_ONE: fb.k = 1.0f; break;
	default:     break;
	}
	struct bl_factor in = { 0.0f, 0.0f }, mem = { 0.0f, 0.0f };
	if (p == BL_IN) in.k += fa.k, in.ka += fa.ka;
	if (p == BL_MEM) mem.k += fa.k, mem.ka += fa.ka;
	if (m == BL_IN) in.k += fb.k, in.ka += fb.ka;
	if (m == BL_MEM) mem.k += fb.k, mem.ka += fb.ka;

	if (bl_eq(mem, 0.0f, 0.0f))
		return GFX_BLEND_OFF;                 /* p. ej. IN·0 + IN·1 */
	if (bl_eq(in, 0.0f, 0.0f) && bl_eq(mem, 1.0f, 0.0f))
		return GFX_BLEND_KEEP;                /* solo profundidad */
	if (bl_eq(in, 0.0f, 1.0f) && bl_eq(mem, 1.0f, 0.0f))
		return GFX_BLEND_ADD;
	return GFX_BLEND_ALPHA;                       /* IN·α + MEM·(1 − α) y parecidos */
}

static void blend_state(const struct gfx_state *g, struct gfx_draw *d)
{
	uint32_t ol = g->ol;
	unsigned c0p = (ol >> 30) & 3, c0a = (ol >> 26) & 3, c0m = (ol >> 22) & 3, c0b = (ol >> 18) & 3;
	unsigned c1p = (ol >> 28) & 3, c1a = (ol >> 24) & 3, c1m = (ol >> 20) & 3, c1b = (ol >> 16) & 3;
	unsigned p = c0p, a = c0a, m = c0m, b = c0b;

	if (d->cycle == G_CYC_2CYCLE) {
		/* Primer ciclo de niebla (G_RM_FOG_SHADE_A): fog·a_shade + in·(1−a). */
		if (c0p == BL_FOG && c0a == BA_SHADE && c0m == BL_IN && c0b == BB_1MA) {
			d->fog = 1;
			d->fog_color = g->fog;
		}
		p = c1p, a = c1a, m = c1m, b = c1b;
	}

	d->blend = !(ol & FORCE_BL) ? GFX_BLEND_OFF : classify_blend(p, a, m, b, g->fog);

	if ((ol & G_AC_MASK) == G_AC_THRESHOLD) {
		d->atest = GFX_ATEST_GEQUAL;
		d->aref = (uint8_t)(g->blend & 0xFF) ? (uint8_t)(g->blend & 0xFF) : 1;
	} else if ((ol & CVG_X_ALPHA) && !(ol & FORCE_BL)) {
		/* Bordes recortados por el alfa de la textura (G_RM_*_TEX_EDGE). */
		d->atest = GFX_ATEST_GEQUAL;
		d->aref = 128;
	}
}

static float shift_scale(unsigned shift)
{
	if (shift == 0)
		return 1.0f;
	if (shift <= 10)
		return 1.0f / (float)(1u << shift);
	return (float)(1u << (16 - shift));
}

static bool texture_state(struct gfx_state *g, struct gfx_draw *d, int tile, int slot, bool copy)
{
	struct gfx_tex *t = &d->tex[slot];
	if (!gfx_tile_texture(g, tile & 7, t))
		return false;
	const struct gfx_tile *ti = &g->tiles[tile & 7];
	bool bilinear = !copy && ((g->oh >> G_MDSFT_TEXTFILT) & 3) != G_TF_POINT;
	float half = bilinear ? 0.5f : 0.0f;
	t->flags = (uint8_t)((ti->masks && !(ti->cms & 2) ? ((ti->cms & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT)
							 : GFX_WRAP_CLAMP) |
			     (ti->maskt && !(ti->cmt & 2) ? ((ti->cmt & 1) ? GFX_WRAP_MIRROR : GFX_WRAP_REPEAT)
							   : GFX_WRAP_CLAMP) << 2 |
			     (bilinear ? GFX_TEX_BILINEAR : 0));
	float w = (float)t->width, h = (float)t->height;
	t->xf[0] = shift_scale(ti->shifts) / w;
	t->xf[1] = shift_scale(ti->shiftt) / h;
	t->xf[2] = (half - ti->uls / 4.0f) / w;
	t->xf[3] = (half - ti->ult / 4.0f) / h;
	return true;
}

/* Resume el estado vigente en `d` (sin el tramo de vértices). */
static void build_state(struct gfx_state *g, struct gfx_draw *d, enum prim_kind kind, int tile)
{
	memset(d, 0, sizeof *d);
	d->h.op = GFX_OP_DRAW;
	d->h.size = sizeof *d;
	d->zimg = g->zimg_addr;
	memcpy(d->scissor, g->scissor, sizeof d->scissor);
	d->cycle = (uint8_t)((g->oh >> G_MDSFT_CYCLETYPE) & 3);

	if (kind == PRIM_FILL || d->cycle == G_CYC_FILL) {
		uint8_t c[4];
		rgba8(fill_rgba(g), c);
		for (int lane = 0; lane < 4; lane++)
			d->k[GFX_K_ONE][0][lane] = c[lane] / 255.0f;
		d->k[GFX_K_ONE][0][3] = 1.0f;
		d->cycle = G_CYC_FILL;
		return;
	}
	if (d->cycle == G_CYC_COPY) {
		for (int lane = 0; lane < 4; lane++)
			d->k[GFX_K_T0][0][lane] = 1.0f;
		d->ntex = 1;
		if (!texture_state(g, d, tile, 0, true))
			d->ntex = 0;
		if ((g->ol & G_AC_MASK) == G_AC_THRESHOLD) {
			d->atest = GFX_ATEST_GEQUAL;
			d->aref = 1;
		}
		return;
	}

	bool zbuf = kind == PRIM_RECT || (g->geom & G_ZBUFFER);
	if (zbuf && (g->ol & Z_CMP))
		d->depth |= GFX_DEPTH_TEST;
	if (zbuf && (g->ol & Z_UPD))
		d->depth |= GFX_DEPTH_WRITE;
	if ((g->ol & ZMODE_MASK) == ZMODE_DEC)
		d->depth |= GFX_DEPTH_DECAL;
	if (kind == PRIM_TRI) {
		if ((g->geom & (G_CULL_FRONT | G_CULL_BACK)) == G_CULL_BACK)
			d->cull = GFX_CULL_BACK;
		else if ((g->geom & (G_CULL_FRONT | G_CULL_BACK)) == G_CULL_FRONT)
			d->cull = GFX_CULL_FRONT;
	}
	blend_state(g, d);
	if (kind != PRIM_TRI || !(g->geom & G_FOG))
		d->fog = 0;

	if (gfx_combine(g, d))
		g->res->approx_cc++;
	bool textured = kind == PRIM_RECT || g->tex_on;
	if (!textured)
		d->ntex = 0;
	unsigned n = d->ntex;
	for (unsigned i = 0; i < n; i++)
		if (!texture_state(g, d, tile + (int)i, (int)i, false)) {
			d->ntex = (uint8_t)i;
			break;
		}
	if (d->ntex < n) {
		/* Sin la textura (arena lleno): sus términos se anulan. */
		for (int t = d->ntex == 0 ? GFX_K_T0 : GFX_K_T1; t <= GFX_K_T1A; t++)
			memset(d->k[t], 0, sizeof d->k[t]);
	}
}

void gfx_flush_draw(struct gfx_state *g)
{
	if (!g->have_draw)
		return;
	g->have_draw = false;
	if (g->draw.h.b == 0)
		return;
	if (gfx_io_cmd(g, &g->draw, sizeof g->draw))
		g->res->draws++;
}

static void send_target(struct gfx_state *g)
{
	if (g->target_sent)
		return;
	struct gfx_cmd c = { GFX_OP_TARGET, sizeof c, g->cimg_addr, g->cimg_width,
			     (uint32_t)g->cimg_fmt << 8 | g->cimg_siz };
	gfx_flush_draw(g);
	if (gfx_io_cmd(g, &c, sizeof c))
		g->target_sent = true;
}

/* Deja abierto un dibujo con el estado vigente para `nvtx` vértices más y
 * devuelve dónde escribirlos (NULL si no hay lugar). */
static struct gfx_vertex *begin_prim(struct gfx_state *g, enum prim_kind kind, int tile, uint32_t nvtx)
{
	send_target(g);
	if (g->dirty || !g->have_draw || g->draw.prim_kind != (uint32_t)kind ||
	    (kind == PRIM_RECT && g->draw.prim_tile != (uint32_t)tile)) {
		struct gfx_draw d;
		uint32_t t0 = gfx_io_clock();
		build_state(g, &d, kind, tile);
		g->res->t_state += gfx_io_clock() - t0;
		d.prim_kind = kind;
		d.prim_tile = (uint32_t)tile;
		g->dirty = false;
		/* Mismo estado que el dibujo abierto: se continúa. */
		bool same = g->have_draw &&
			    memcmp((const uint8_t *)&d + sizeof d.h, (const uint8_t *)&g->draw + sizeof d.h,
				   sizeof d - sizeof d.h) == 0;
		if (!same) {
			gfx_flush_draw(g);
			g->draw = d;
			g->draw.h.a = g->vtx_count;
			g->draw.h.b = 0;
			g->have_draw = true;
		}
	}
	struct gfx_vertex *v = gfx_io_vtx(g, nvtx);
	if (!v)
		return NULL;
	g->vtx_count += nvtx;
	g->draw.h.b += nvtx;
	return v;
}

void gfx_triangle(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c)
{
	if (a >= GFX_MAX_VTX || b >= GFX_MAX_VTX || c >= GFX_MAX_VTX)
		return;
	const struct gfx_vtx *v[3] = { &g->vtx[a], &g->vtx[b], &g->vtx[c] };
	/* Fuera del mismo plano los tres: no se ve. */
	if (v[0]->clip & v[1]->clip & v[2]->clip)
		return;
	struct gfx_vertex *o = begin_prim(g, PRIM_TRI, g->tex_tile, 3);
	if (!o)
		return;
	g->res->triangles++;
	for (int i = 0; i < 3; i++)
		gfx_vtx_out(g, v[i], &o[i]);
	if (!(g->geom & G_SHADING_SMOOTH)) {
		/* Sombreado plano: el color del primer vértice. */
		for (int i = 1; i < 3; i++) {
			o[i].r = o[0].r;
			o[i].g = o[0].g;
			o[i].b = o[0].b;
			o[i].a = o[0].a;
		}
	}
}

/* Rectángulo de pantalla en dos triángulos. x, y en píxeles; s, t en texels
 * en cada esquina (0: arriba a la izquierda, 1: arriba a la derecha,
 * 2: abajo a la izquierda, 3: abajo a la derecha). */
static void rect(struct gfx_state *g, enum prim_kind kind, int tile, float x0, float y0, float x1, float y1,
		 const float s[4], const float t[4])
{
	if (x1 <= x0 || y1 <= y0)
		return;
	struct gfx_vertex *o = begin_prim(g, kind, tile, 6);
	if (!o)
		return;
	g->res->rects++;
	float z = (g->ol & G_ZS_PRIM) ? (float)g->prim_z / 32767.0f : 0.0f;
	const float xs[4] = { x0, x1, x0, x1 }, ys[4] = { y0, y0, y1, y1 };
	static const int order[6] = { 0, 1, 2, 2, 1, 3 };
	for (int i = 0; i < 6; i++) {
		int k = order[i];
		o[i].x = xs[k];
		o[i].y = ys[k];
		o[i].z = z;
		o[i].w = 1.0f;
		o[i].s = s ? s[k] : 0.0f;
		o[i].t = t ? t[k] : 0.0f;
		o[i].r = o[i].g = o[i].b = o[i].a = 0;
		o[i].pad = 0;
	}
}

void gfx_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip)
{
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
	float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
	int tile = (w1 >> 24) & 7;
	float s0 = (float)(int16_t)(w2 >> 16) / 32.0f, t0 = (float)(int16_t)(w2 & 0xFFFF) / 32.0f;
	float dsdx = (float)(int16_t)(w3 >> 16) / 1024.0f, dtdy = (float)(int16_t)(w3 & 0xFFFF) / 1024.0f;
	if (cycle == G_CYC_COPY || cycle == G_CYC_FILL) {
		/* Píxeles enteros y bordes inferiores inclusivos; en copia, 4
		 * píxeles por ciclo (dsdx = 4.0 es un texel por píxel). */
		if (cycle == G_CYC_COPY)
			dsdx /= 4.0f;
		ulx = (float)(int)ulx, uly = (float)(int)uly;
		lrx = (float)(int)lrx + 1.0f, lry = (float)(int)lry + 1.0f;
	} else if (lry - uly < 1.0f) {
		lry = uly + 1.0f;
	}
	float w = lrx - ulx, h = lry - uly;
	float s[4], t[4];
	if (!flip) {
		s[0] = s0, s[1] = s0 + w * dsdx, s[2] = s0, s[3] = s0 + w * dsdx;
		t[0] = t0, t[1] = t0, t[2] = t0 + h * dtdy, t[3] = t0 + h * dtdy;
	} else {
		s[0] = s0, s[1] = s0, s[2] = s0 + h * dsdx, s[3] = s0 + h * dsdx;
		t[0] = t0, t[1] = t0 + w * dtdy, t[2] = t0, t[3] = t0 + w * dtdy;
	}
	rect(g, PRIM_RECT, tile, ulx, uly, lrx, lry, s, t);
}

static void fillrect(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	unsigned cycle = (g->oh >> G_MDSFT_CYCLETYPE) & 3;
	float lrx = (float)((w0 >> 14) & 0x3FF), lry = (float)((w0 >> 2) & 0x3FF);
	float ulx = (float)((w1 >> 14) & 0x3FF), uly = (float)((w1 >> 2) & 0x3FF);
	if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
		lrx += 1.0f;
		lry += 1.0f;
	} else if (lry == uly) {
		lry += 1.0f;
	}
	if (cycle == G_CYC_FILL && g->cimg_addr == g->zimg_addr) {
		/* Borrar el z-buffer: se dibuja en la color image que apunta a él. */
		struct gfx_zclear z;
		memset(&z, 0, sizeof z);
		z.h.op = GFX_OP_ZCLEAR;
		z.h.size = sizeof z;
		z.h.a = g->zimg_addr;
		z.h.b = g->fill & 0xFFFF;
		z.rect[0] = (uint16_t)ulx;
		z.rect[1] = (uint16_t)uly;
		z.rect[2] = (uint16_t)lrx;
		z.rect[3] = (uint16_t)lry;
		gfx_flush_draw(g);
		gfx_io_cmd(g, &z, sizeof z);
		g->res->rects++;
		return;
	}
	rect(g, cycle == G_CYC_FILL ? PRIM_FILL : PRIM_RECT, 0, ulx, uly, lrx, lry, NULL, NULL);
}

static void set_image(uint32_t w0, uint8_t *fmt, uint8_t *siz, uint16_t *width)
{
	*fmt = (w0 >> 21) & 7;
	*siz = (w0 >> 19) & 3;
	*width = (uint16_t)((w0 & 0xFFF) + 1);
}

void gfx_rdp_command(struct gfx_state *g, uint32_t w0, uint32_t w1)
{
	uint32_t op = w0 >> 24;
	switch (op) {
	case G_RDPFULLSYNC:
		g->res->flags |= GFX_RES_FULLSYNC;
		return;
	case G_RDPLOADSYNC:
	case G_RDPPIPESYNC:
	case G_RDPTILESYNC:
	case G_SETKEYGB:
	case G_SETKEYR:
	case G_SETCONVERT:
		return;
	case G_SETSCISSOR:
		g->scissor[0] = (w0 >> 12) & 0xFFF;
		g->scissor[1] = w0 & 0xFFF;
		g->scissor[2] = (w1 >> 12) & 0xFFF;
		g->scissor[3] = w1 & 0xFFF;
		break;
	case G_SETPRIMDEPTH:
		g->prim_z = (uint16_t)(w1 >> 16);
		g->prim_dz = (uint16_t)w1;
		break;
	case G_RDPSETOTHERMODE:
		g->oh = w0 & 0x00FFFFFF;
		g->ol = w1;
		break;
	case G_LOADTLUT: {
		uint32_t t0 = gfx_io_clock();
		gfx_load_tlut(g, w0, w1);
		g->res->t_load += gfx_io_clock() - t0;
		break;
	}
	case G_SETTILESIZE: {
		struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
		t->uls = (w0 >> 12) & 0xFFF;
		t->ult = w0 & 0xFFF;
		t->lrs = (w1 >> 12) & 0xFFF;
		t->lrt = w1 & 0xFFF;
		break;
	}
	case G_LOADBLOCK:
	case G_LOADTILE: {
		uint32_t t0 = gfx_io_clock();
		if (op == G_LOADBLOCK)
			gfx_load_block(g, w0, w1);
		else
			gfx_load_tile(g, w0, w1);
		g->res->t_load += gfx_io_clock() - t0;
		break;
	}
	case G_SETTILE: {
		struct gfx_tile *t = &g->tiles[(w1 >> 24) & 7];
		t->fmt = (w0 >> 21) & 7;
		t->siz = (w0 >> 19) & 3;
		t->line = (uint16_t)(((w0 >> 9) & 0x1FF) * 8);
		t->tmem = (uint16_t)((w0 & 0x1FF) * 8);
		t->pal = (w1 >> 20) & 0xF;
		t->cmt = (w1 >> 18) & 3;
		t->maskt = (w1 >> 14) & 0xF;
		t->shiftt = (w1 >> 10) & 0xF;
		t->cms = (w1 >> 8) & 3;
		t->masks = (w1 >> 4) & 0xF;
		t->shifts = w1 & 0xF;
		break;
	}
	case G_FILLRECT:
		fillrect(g, w0, w1);
		return;
	case G_SETFILLCOLOR:
		g->fill = w1;
		break;
	case G_SETFOGCOLOR:
		g->fog = w1;
		break;
	case G_SETBLENDCOLOR:
		g->blend = w1;
		break;
	case G_SETPRIMCOLOR:
		g->prim = w1;
		g->prim_min = (uint8_t)(w0 >> 8);
		g->prim_lod = (uint8_t)w0;
		break;
	case G_SETENVCOLOR:
		g->env = w1;
		break;
	case G_SETCOMBINE:
		g->cc_hi = w0 & 0x00FFFFFF;
		g->cc_lo = w1;
		break;
	case G_SETTIMG:
		set_image(w0, &g->timg_fmt, &g->timg_siz, &g->timg_width);
		g->timg_addr = gfx_addr(g, w1);
		return;
	case G_SETZIMG:
		g->zimg_addr = gfx_addr(g, w1);
		break;
	case G_SETCIMG: {
		uint32_t addr = gfx_addr(g, w1);
		uint8_t fmt, siz;
		uint16_t width;
		set_image(w0, &fmt, &siz, &width);
		if (addr != g->cimg_addr || width != g->cimg_width || siz != g->cimg_siz) {
			gfx_flush_draw(g);
			g->target_sent = false;
		}
		g->cimg_addr = addr;
		g->cimg_fmt = fmt;
		g->cimg_siz = siz;
		g->cimg_width = width;
		break;
	}
	default:
		if (op >= RDP_TRI_FIRST && op <= RDP_TRI_LAST) {
			/* Triángulos crudos del RDP: no los genera F3DEX2. */
			g->res->unknown_cmds++;
			g->res->last_unknown = op;
		}
		return;
	}
	g->dirty = true;
}
