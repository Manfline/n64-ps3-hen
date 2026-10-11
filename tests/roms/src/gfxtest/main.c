/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — gfxtest: ROM 3D de prueba para la HLE gráfica (F3DEX2, F3D o
 * F3DEX 1.x según GBI en el Makefile) y el RDP. Código propio, sin libultra
 * ni libdragon; se empaqueta con el IPL3 libre de libdragon (nust64).
 *
 * Cada cuadro arma una display list (gbi.h) y la manda al RSP como una
 * tarea gráfica de libultra (OSTask en DMEM 0xFC0). El microcódigo es un
 * BREAK y su segmento de datos lleva el texto GBI_MARKER ("Salsa64 GBI
 * F3DEX2", "Salsa64 GBI Fast3D" o "Salsa64 GBI F3DEX1"): en un emulador con
 * HLE que lo reconozca se dibuja la escena; en una N64 real el RSP solo se
 * detiene (no hay microcódigo de Nintendo aquí).
 *
 * Escena (expect.h): borrado del z-buffer y fondo con FILLRECT, dos cuadros
 * con z-buffer, un triángulo Gouraud, un cuadro con textura RGBA16 (texels
 * puntuales), un TEXRECT I4 con alpha compare, un TEXRECT en modo COPY con
 * textura CI4 y paleta, un cuadro translúcido, un cubo iluminado en
 * perspectiva que gira, un cuadro con mapeo de entorno (G_TEXTURE_GEN) y
 * cuatro cuadros de culling (antihorario y horario con G_CULL_BACK y con
 * G_CULL_FRONT: se ven dos), un cuadro de 2 ciclos con dos texturas
 * mezcladas por el alfa primitivo (TEXEL0 en la tile 0, TEXEL1 en la 1),
 * un cuadro con w = 1000 coloreado por PRIM·TEXEL0_ALPHA, dos TEXRECT cuya
 * textura (L) y paleta (M) cambia el CPU en la RDRAM en el cuadro
 * GFXTEST_CHANGE_FRAME (prueba de la caché de texturas entre tareas), un
 * cuadro con textura RGBA16 de alfa de 1 bit en G_RM_TEX_EDGE (N), dos
 * cuadros de 2 ciclos que no caben en una combinación lineal (piden un
 * fragment program generado): TEXEL0·TEXEL1·SHADE (P) y lerp(ENV, TEXEL0,
 * SHADE_ALPHA)·SHADE, la niebla por luz de GoldenEye (Q), un cuadro el
 * doble de grande que un viewport chico con clip ratio 1 (O: el RSP lo
 * recorta a la ventana) y uno que cruza el plano cercano (R: la mitad de
 * detrás no se dibuja). Al
 * final de cada lista, un tramo de otro microcódigo (G_LOAD_UCODE) que la
 * HLE debe saltar; en GFXTEST_CUT_FRAME la lista termina dentro de él (no
 * en la variante F3D, que no tiene G_LOAD_UCODE).
 *
 * Se verifica sola en lo que el CPU puede ver: que cada tarea termine con
 * SP_STATUS en HALT|BROKE|TASKDONE, que llegue la interrupción del SP y
 * después la del DP. Tras GFXTEST_FRAMES cuadros imprime por el ISViewer
 * "gfxtest: OK …" o "gfxtest: FALLO …" y sigue dibujando. La imagen la
 * verifica el frontend en la PS3 (expect.h).
 */
#include <stdint.h>

#include "expect.h"
#include "gbi.h"

#define REG(pa) (*(volatile uint32_t *)(0xA0000000u | (pa)))

#define SP_MEM_ADDR  REG(0x04040000)
#define SP_DRAM_ADDR REG(0x04040004)
#define SP_RD_LEN    REG(0x04040008)
#define SP_STATUS    REG(0x04040010)
#define SP_PC        REG(0x04080000)
#define MI_MODE      REG(0x04300000)
#define MI_INTR      REG(0x04300008)
#define VI_CONTROL   REG(0x04400000)
#define VI_ORIGIN    REG(0x04400004)
#define VI_WIDTH     REG(0x04400008)
#define VI_V_INTR    REG(0x0440000C)
#define VI_V_CURRENT REG(0x04400010)
#define VI_BURST     REG(0x04400014)
#define VI_V_SYNC    REG(0x04400018)
#define VI_H_SYNC    REG(0x0440001C)
#define VI_H_SYNC_LEAP REG(0x04400020)
#define VI_H_VIDEO   REG(0x04400024)
#define VI_V_VIDEO   REG(0x04400028)
#define VI_V_BURST   REG(0x0440002C)
#define VI_X_SCALE   REG(0x04400030)
#define VI_Y_SCALE   REG(0x04400034)
#define BOOT_FLAGS   REG(0x04000008)

#define ISV_MAGIC    REG(0x13FF0000)
#define ISV_LEN      REG(0x13FF0014)
#define ISV_BUF      0x13FF0020u

#define MI_SP 0x01u
#define MI_VI 0x08u
#define MI_DP 0x20u
#define SP_HALT     0x001u
#define SP_BROKE    0x002u
#define SP_TASKDONE 0x200u

#define W GFXTEST_W
#define H GFXTEST_H
/* Espera máxima por una interrupción, en ticks de Count (46,875 MHz): 10 s.
 * Es un guardia contra cuelgues, no una medida: el emulador puede tardar
 * lo que tarde su RSP (en RPCS3 el primer trabajo del SPE compila segundos
 * mientras el CPU sigue contando). */
#define TIMEOUT_TICKS (10u * 46875000u)

/* ---- Datos que ve el RSP ---------------------------------------------------
 * Todo en una sección sin cargar ni limpiar (.rsp), escrita siempre por
 * KSEG1: así ninguna línea sucia de la caché pisa lo que lee el RSP. */

typedef struct {
	int16_t x, y, z;
	uint16_t flag;
	int16_t s, t;
	uint8_t r, g, b, a;
} vtx;

typedef struct {
	int16_t i[16];
	uint16_t f[16];
} mtx;

typedef struct {
	uint8_t col[3], pad0, colc[3], pad1;
	int8_t dir[3], pad2;
	uint32_t pad3;
} light;

struct task {
	uint32_t type, flags, ucode_boot, ucode_boot_size, ucode, ucode_size, ucode_data, ucode_data_size;
	uint32_t dram_stack, dram_stack_size, output_buff, output_buff_size, data_ptr, data_size;
	uint32_t yield_data_ptr, yield_data_size;
};

struct rsp_data {
	gfx dl[1024];
	struct task task;
	uint32_t ucode[4];
	char ucode_data[32];
	uint64_t dram_stack[128];
	int16_t vp[8], vp_o[8];
	mtx ident, ortho, persp, trans, rot_y, rot_x, far;
	light sun, ambient, look_x, look_y;
	vtx quad_a[4], quad_b[4], tri_c[3], quad_d[4], quad_g[4], quad_i[4], cube[24];
	vtx cull[16];  /* 4 cuadros: antihorario/horario × cull back/front */
	vtx quad_j[4], quad_k[4], quad_n[4], quad_p[4], quad_q[4], quad_o[4], quad_r[4];
	uint16_t red[4 * 4], blue[4 * 4];
	uint16_t yellow[4 * 4], magenta[4 * 4];
	uint16_t checker[8 * 8];
	uint8_t glyph[16 * 8 / 2];
	uint8_t ci[16 * 4 / 2];
	uint16_t palette[16];
	uint16_t chg[4 * 4] __attribute__((aligned(8)));
	uint16_t pal2[16] __attribute__((aligned(8)));
	uint16_t edge[8 * 8] __attribute__((aligned(8)));
} __attribute__((aligned(64)));

static struct rsp_data rsp_mem __attribute__((section(".rsp")));
static uint16_t fb[2][W * H] __attribute__((section(".fb"), aligned(64)));
static uint16_t zbuf[W * H] __attribute__((section(".fb"), aligned(64)));

#define R ((volatile struct rsp_data *)((uint32_t)&rsp_mem | 0x20000000u))

void *memset(void *d, int c, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	const uint8_t *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

/* sin(2π·i/64) en s15.16. */
static const int32_t sin64[64] = {
	0, 6424, 12785, 19024, 25080, 30893, 36410, 41576, 46341, 50660, 54491, 57798, 60547, 62714, 64277, 65220,
	65536, 65220, 64277, 62714, 60547, 57798, 54491, 50660, 46341, 41576, 36410, 30893, 25080, 19024, 12785, 6424,
	0, -6424, -12785, -19024, -25080, -30893, -36410, -41576, -46341, -50660, -54491, -57798, -60547, -62714,
	-64277, -65220, -65536, -65220, -64277, -62714, -60547, -57798, -54491, -50660, -46341, -41576, -36410,
	-30893, -25080, -19024, -12785, -6424,
};

static int32_t isin(unsigned a) { return sin64[a & 63]; }
static int32_t icos(unsigned a) { return sin64[(a + 16) & 63]; }

/* Matriz desde 16 valores s15.16, fila por fila. */
static void set_mtx(volatile mtx *m, const int32_t v[16])
{
	for (int i = 0; i < 16; i++) {
		m->i[i] = (int16_t)(v[i] >> 16);
		m->f[i] = (uint16_t)v[i];
	}
}

#define ONE 65536

static void set_vtx(volatile vtx *v, int x, int y, int z, int s, int t, uint32_t rgba)
{
	v->x = (int16_t)x, v->y = (int16_t)y, v->z = (int16_t)z, v->flag = 0;
	v->s = (int16_t)s, v->t = (int16_t)t;
	v->r = (uint8_t)(rgba >> 24), v->g = (uint8_t)(rgba >> 16), v->b = (uint8_t)(rgba >> 8), v->a = (uint8_t)rgba;
}

/* Cuadro ortográfico de (x0, y0) a (x1, y1) en unidades de 1/32 de recorte. */
static void set_quad(volatile vtx *q, int x0, int y0, int x1, int y1, int z, uint32_t rgba)
{
	set_vtx(&q[0], x0, y0, z, 0, 0, rgba);
	set_vtx(&q[1], x1, y0, z, 8 * 32, 0, rgba);
	set_vtx(&q[2], x1, y1, z, 8 * 32, 8 * 32, rgba);
	set_vtx(&q[3], x0, y1, z, 0, 8 * 32, rgba);
}

static void set_light(volatile light *l, unsigned c, int dx, int dy, int dz)
{
	for (int i = 0; i < 3; i++)
		l->col[i] = l->colc[i] = (uint8_t)c;
	l->dir[0] = (int8_t)dx, l->dir[1] = (int8_t)dy, l->dir[2] = (int8_t)dz;
}

static uint16_t rgba16(unsigned r5, unsigned g5, unsigned b5)
{
	return (uint16_t)(r5 << 11 | g5 << 6 | b5 << 1 | 1);
}

static void init_data(void)
{
	volatile struct rsp_data *r = R;
	static const char marker[] = GBI_MARKER;
	for (unsigned i = 0; i < sizeof r->ucode_data; i++)
		r->ucode_data[i] = i < sizeof marker ? marker[i] : 0;
	r->ucode[0] = 0x0000000D;  /* break */
	r->ucode[1] = r->ucode[2] = r->ucode[3] = 0;

	/* Viewport de 320×240 (cuartos de píxel) y z 0..0x3FE. */
	const int16_t vp[8] = { W * 2, H * 2, 511, 0, W * 2, H * 2, 511, 0 };
	for (int i = 0; i < 8; i++)
		r->vp[i] = vp[i];
	/* O: 20×16 píxeles con centro en (20, 90) (expect.h). */
	const int16_t vp_o[8] = { 10 * 4, 8 * 4, 511, 0, 20 * 4, 90 * 4, 511, 0 };
	for (int i = 0; i < 8; i++)
		r->vp_o[i] = vp_o[i];

	const int32_t ident[16] = { ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE };
	const int32_t ortho[16] = { ONE / 32, 0, 0, 0, 0, ONE / 32, 0, 0, 0, 0, ONE / 32, 0, 0, 0, 0, ONE };
	/* Perspectiva de 45°, 4:3, planos 50 y 1000. */
	const int32_t persp[16] = { 118663, 0, 0, 0, 0, 158218, 0, 0, 0, 0, -72435, -ONE, 0, 0, -6898526, 0 };
	const int32_t trans[16] = { ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE, 0, 20 * ONE, -57 * ONE, -300 * ONE, ONE };
	set_mtx(&r->ident, ident);
	set_mtx(&r->ortho, ortho);
	set_mtx(&r->persp, persp);
	set_mtx(&r->trans, trans);
	/* Proyección ×1000: mismo lugar en pantalla que la ortográfica, con
	 * w = 1000 (como los objetos lejanos de un juego). */
	const int32_t far[16] = { 1000 * ONE, 0, 0, 0, 0, 1000 * ONE, 0, 0, 0, 0, 1000 * ONE, 0, 0, 0, 0, 1000 * ONE };
	set_mtx(&r->far, far);

	set_light(&r->sun, 200, 40, 60, 100);
	set_light(&r->ambient, 50, 0, 0, 0);
	set_light(&r->look_x, 0, 127, 0, 0);
	set_light(&r->look_y, 0, 0, 127, 0);

	/* Objetos ortográficos (expect.h). z más chica = más cerca. */
	set_quad(r->quad_a, -24, 16, -12, 0, -8, 0xFF0000FF);
	set_quad(r->quad_b, -20, 12, -8, -4, 8, 0x00FF00FF);
	set_vtx(&r->tri_c[0], -28, 28, 0, 0, 0, 0xFF0000FF);
	set_vtx(&r->tri_c[1], -12, 28, 0, 0, 0, 0x00FF00FF);
	set_vtx(&r->tri_c[2], -20, 20, 0, 0, 0, 0x0000FFFF);
	set_quad(r->quad_d, -4, 24, 12, 4, 0, 0xFFFFFFFF);
	set_quad(r->quad_g, 16, -8, 28, -24, 0, 0xFFFFFFFF);
	/* Culling (expect.h): set_quad da los vértices en sentido horario con
	 * la y hacia arriba; los antihorarios se arman al revés. */
	set_quad(&r->cull[0], 16, 16, 22, 8, 0, 0x00FFFFFF);   /* back, antihorario: se ve */
	set_quad(&r->cull[4], 23, 16, 29, 8, 0, 0xFF0000FF);   /* back, horario: no */
	set_quad(&r->cull[8], 16, 4, 22, -4, 0, 0xFFFF00FF);   /* front, antihorario: no */
	set_quad(&r->cull[12], 23, 4, 29, -4, 0, 0xFF00FFFF);  /* front, horario: se ve */
	set_quad(r->quad_j, -22, -24, -10, -28, 0, 0xFFFFFFFF);
	set_quad(r->quad_k, 24, 30, 30, 26, 0, 0xFFFFFFFF);
	set_quad(r->quad_n, -10, 28, -6, 24, 0, 0xFFFFFFFF);
	/* P y Q (expect.h): sombreado (255, 128, 255, 128) → S = (1, 0,5, 1),
	 * Sa = 0,5. */
	set_quad(r->quad_p, 16, -29, 21, -31, 0, 0xFF80FF80);
	set_quad(r->quad_q, 23, -29, 28, -31, 0, 0xFF80FF80);
	/* O: ±2 en recorte, el doble del viewport chico. */
	set_quad(r->quad_o, -64, 64, 64, -64, 0, 0x00FF80FF);
	/* R: z de 0 (izquierda) a −64 (derecha, −2 en recorte): el plano
	 * cercano (z = −w) lo corta en el medio, en el píxel 20. */
	set_vtx(&r->quad_r[0], -31, 2, 0, 0, 0, 0xFF8080FF);
	set_vtx(&r->quad_r[1], -25, 2, -64, 0, 0, 0xFF8080FF);
	set_vtx(&r->quad_r[2], -25, -2, -64, 0, 0, 0xFF8080FF);
	set_vtx(&r->quad_r[3], -31, -2, 0, 0, 0, 0xFF8080FF);
	for (int i = 0; i < 16; i++) {
		r->red[i] = rgba16(31, 0, 0);
		r->blue[i] = rgba16(0, 0, 31);
		r->yellow[i] = rgba16(31, 31, 0);
		r->magenta[i] = rgba16(31, 0, 31);
	}

	/* Cubo: 6 caras de 4 vértices, en sentido antihorario visto desde
	 * fuera, con la normal de la cara en el color. */
	static const int8_t face[6][3][3] = {
		/* normal, u, v (u × v = normal) */
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
		{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
		{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
	};
	static const int8_t corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	for (int f = 0; f < 6; f++)
		for (int c = 0; c < 4; c++) {
			int p[3];
			for (int k = 0; k < 3; k++)
				p[k] = 30 * (face[f][0][k] + corner[c][0] * face[f][1][k] + corner[c][1] * face[f][2][k]);
			uint32_t nrm = (uint32_t)(uint8_t)(face[f][0][0] * 127) << 24 |
				       (uint32_t)(uint8_t)(face[f][0][1] * 127) << 16 |
				       (uint32_t)(uint8_t)(face[f][0][2] * 127) << 8 | 0xFF;
			set_vtx(&r->cube[f * 4 + c], p[0], p[1], p[2], 0, 0, nrm);
		}

	/* Tablero de 8×8: amarillo en (x + y) par, azul en impar. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			r->checker[y * 8 + x] = ((x + y) & 1) ? rgba16(0, 0, 31) : rgba16(31, 31, 0);
	/* Glifo I4 de 16×8: un marco de intensidad 15 con el centro en 0. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 16; x += 2) {
			int a = y == 0 || y == 7 || x == 0 ? 15 : 0;
			int b = y == 0 || y == 7 || x + 1 == 15 ? 15 : 0;
			r->glyph[(y * 16 + x) / 2] = (uint8_t)(a << 4 | b);
		}
	/* CI4 de 16×4: el índice es la columna; paleta de grises. */
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 16; x += 2)
			r->ci[(y * 16 + x) / 2] = (uint8_t)(x << 4 | (x + 1));
	for (int i = 0; i < 16; i++)
		r->palette[i] = rgba16((unsigned)i * 2, (unsigned)i * 2, (unsigned)i * 2);
	/* N: 8×8, mitad izquierda roja opaca y derecha café con alfa 0. */
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			r->edge[y * 8 + x] = x < 4 ? rgba16(31, 0, 0) : (uint16_t)(rgba16(20, 10, 4) & ~1u);
	/* L y M (expect.h): verde y una paleta con el índice 5 azul; el cuadro
	 * GFXTEST_CHANGE_FRAME cambia el texel (3,3) y el índice 5. */
	for (int i = 0; i < 16; i++) {
		r->chg[i] = rgba16(0, 31, 0);
		r->pal2[i] = i == 5 ? rgba16(0, 0, 31) : rgba16(31, 31, 31);
	}
}

static void change_textures(void)
{
	volatile struct rsp_data *r = R;
	r->chg[15] = rgba16(31, 0, 31);
	r->pal2[5] = rgba16(31, 16, 0);
}

static void load_checker(gfx **dl, unsigned cm, unsigned filter)
{
	(void)filter;
	dp_texture_image(dl, FMT_RGBA, SIZ_16, 1, R->checker);
	dp_tile(dl, FMT_RGBA, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(dl, SYNC_LOAD);
	/* 64 texels; una fila son 2 palabras: dxt = 2048 / 2. */
	dp_load_block(dl, 7, 64, 1024);
	dp_sync(dl, SYNC_PIPE);
	dp_tile(dl, FMT_RGBA, SIZ_16, 2, 0, 0, 0, cm, 3, 0);
	dp_tile_size(dl, 0, 0, 0, 7, 7);
}

static gfx *build_dl(uint16_t *color, unsigned frame)
{
	gfx *start = (gfx *)((uint32_t)&rsp_mem.dl[0] | 0x20000000u);
	gfx *dl = start;
	volatile struct rsp_data *r = R;

	/* Animación: el cubo gira y el mapeo de entorno se mueve. */
	unsigned a = frame, b = frame / 2;
	const int32_t ry[16] = { icos(a), 0, -isin(a), 0, 0, ONE, 0, 0, isin(a), 0, icos(a), 0, 0, 0, 0, ONE };
	const int32_t rx[16] = { ONE, 0, 0, 0, 0, icos(b), isin(b), 0, 0, -isin(b), icos(b), 0, 0, 0, 0, ONE };
	set_mtx(&r->rot_y, ry);
	set_mtx(&r->rot_x, rx);
	for (int i = 0; i < 4; i++) {
		static const int xs[4] = { -28, -12, -12, -28 }, ys[4] = { -6, -6, -18, -18 };
		int nx = (int)(isin(frame + (unsigned)i * 16) * 90 / ONE);
		int ny = (int)(icos(frame * 2 + (unsigned)i * 16) * 90 / ONE);
		set_vtx(&r->quad_i[i], xs[i], ys[i], 0, 0, 0,
			(uint32_t)(uint8_t)nx << 24 | (uint32_t)(uint8_t)ny << 16 | (uint32_t)(uint8_t)80 << 8 | 0xFF);
	}

	sp_viewport(&dl, r->vp);
	sp_geometry(&dl, 0xFFFFFFFFu, 0);
	dp_scissor(&dl, 0, 0, W, H);

	/* Borrado del z-buffer y del fondo. */
	dp_othermode(&dl, CYC_FILL, 0);
	dp_color_image(&dl, FMT_RGBA, SIZ_16, W, zbuf);
	dp_fill_color(&dl, 0xFFFCFFFCu);
	dp_fill_rect(&dl, 0, 0, W - 1, H - 1);
	dp_sync(&dl, SYNC_PIPE);
	dp_color_image(&dl, FMT_RGBA, SIZ_16, W, color);
	dp_z_image(&dl, zbuf);
	dp_fill_color(&dl, GFXTEST_BG_5551 << 16 | GFXTEST_BG_5551);
	dp_fill_rect(&dl, 0, 0, W - 1, H - 1);
	dp_sync(&dl, SYNC_PIPE);

	/* Objetos ortográficos. */
	sp_matrix(&dl, &r->ident, G_MTX_LOAD | G_MTX_PROJECTION);
	sp_matrix(&dl, &r->ortho, G_MTX_LOAD | G_MTX_MODELVIEW);

	/* A y B con z-buffer: el verde va detrás aunque se dibuja después. */
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT, Z_CMP | Z_UPD | BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_SHADE, CCA_0, CCA_0, CCA_0, CC_SHADE);
	sp_geometry(&dl, 0, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
	sp_vertex(&dl, r->quad_a, 4, 0);
	sp_vertex(&dl, r->quad_b, 4, 4);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_tri2(&dl, 4, 5, 6, 4, 6, 7);

	/* C: triángulo Gouraud sin z. */
	sp_geometry(&dl, G_ZBUFFER, 0);
	dp_othermode(&dl, CYC_1CYCLE, BL_XLU);
	sp_vertex(&dl, r->tri_c, 3, 0);
	sp_tri(&dl, 0, 1, 2);

	/* D: textura RGBA16 con muestreo puntual. */
	load_checker(&dl, 0, 0);
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT, BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_TEXEL0, CCA_0, CCA_0, CCA_0, CC_D_1);
	sp_texture(&dl, 0xFFFF, 0xFFFF, 0, 1);
	sp_vertex(&dl, r->quad_d, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* E: TEXRECT I4 ×2, alfa = intensidad, alpha compare con 0x80. */
	dp_texture_image(&dl, FMT_I, SIZ_16, 1, r->glyph);
	dp_tile(&dl, FMT_I, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 32, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_I, SIZ_4, 1, 0, 0, 0, 2, 0, 0);
	dp_tile_size(&dl, 0, 0, 0, 15, 7);
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT, AC_THRESHOLD | BL_XLU);
	dp_blend_color(&dl, 0x00000080);
	dp_prim_color(&dl, 0xFFFF00FF);
	dp_combine(&dl, CC_TEXEL0, CC_B_0, CC_PRIM, CC_D_0, CC_TEXEL0, CCA_0, CC_PRIM, CCA_0);
	sp_texrect(&dl, 0, 240, 30, 272, 46, 0, 0, 512, 512);

	/* F: TEXRECT en modo COPY, CI4 con paleta RGBA16. */
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->palette);
	dp_tile(&dl, FMT_RGBA, SIZ_4, 0, 0x100, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_tlut(&dl, 7, 16);
	dp_sync(&dl, SYNC_PIPE);
	dp_texture_image(&dl, FMT_CI, SIZ_16, 1, r->ci);
	dp_tile(&dl, FMT_CI, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_CI, SIZ_4, 1, 0, 0, 0, 2, 0, 0);
	dp_tile_size(&dl, 0, 0, 0, 15, 3);
	dp_othermode(&dl, CYC_COPY | TT_RGBA16, 0);
	sp_texrect(&dl, 0, 20, 200, 35, 203, 0, 0, 4 << 10, 1 << 10);

	/* G: cuadro blanco con alfa 0x80 mezclado sobre el fondo. */
	dp_othermode(&dl, CYC_1CYCLE, FORCE_BL | BL_XLU);
	dp_prim_color(&dl, 0xFFFFFF80);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_PRIM, CCA_0, CCA_0, CCA_0, CC_PRIM);
	sp_vertex(&dl, r->quad_g, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);

	/* Culling: cuadros de color plano con G_CULL_BACK y G_CULL_FRONT. Los
	 * de set_quad son horarios (0,1,2 va a la derecha y abajo); invertir el
	 * orden los hace antihorarios. */
	dp_othermode(&dl, CYC_1CYCLE, BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_SHADE, CCA_0, CCA_0, CCA_0, CC_SHADE);
	sp_vertex(&dl, r->cull, 16, 0);
	sp_geometry(&dl, G_CULL_FRONT | G_ZBUFFER, G_CULL_BACK | G_SHADE | G_SHADING_SMOOTH);
	sp_tri2(&dl, 0, 3, 2, 0, 2, 1);      /* antihorario */
	sp_tri2(&dl, 4, 5, 6, 4, 6, 7);      /* horario */
	sp_geometry(&dl, G_CULL_BACK, G_CULL_FRONT);
	sp_tri2(&dl, 8, 11, 10, 8, 10, 9);   /* antihorario */
	sp_tri2(&dl, 12, 13, 14, 12, 14, 15); /* horario */
	sp_geometry(&dl, G_CULL_FRONT, 0);

	/* J: 2 ciclos, (TEXEL1 − TEXEL0)·PRIM_ALPHA + TEXEL0 y luego COMBINED;
	 * rojo en la tile 0 (TMEM 0) y azul en la 1 (TMEM 256 bytes). */
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->red);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->blue);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 32, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 1, 0, 0, 0, 0, 2, 0);
	dp_tile_size(&dl, 0, 0, 0, 3, 3);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 1, 32, 1, 0, 0, 2, 0);
	dp_tile_size(&dl, 1, 0, 0, 3, 3);
	dp_othermode(&dl, CYC_2CYCLE | TF_POINT, BL_XLU);
	dp_prim_color(&dl, 0xFFFFFF80);
	{
		/* Ciclo 0: a=TEXEL1, b=TEXEL0, c=PRIM_ALPHA (10), d=TEXEL0; alfa 1.
		 * Ciclo 1: COMBINED. Se arma a mano: dp_combine pone el mismo en los dos. */
		uint32_t hi = 2u << 20 | 10u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u;
		uint32_t lo = 1u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 6u << 9 |
			      0u << 6 | 7u << 3 | 0u;
		cmd(&dl, 0xFC000000u | hi, lo);
	}
	sp_texture(&dl, 0xFFFF, 0xFFFF, 0, 1);
	sp_geometry(&dl, G_CULL_BACK | G_CULL_FRONT | G_ZBUFFER, 0);
	sp_vertex(&dl, r->quad_j, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* H: cubo iluminado en perspectiva. */
	sp_matrix(&dl, &r->persp, G_MTX_LOAD | G_MTX_PROJECTION);
	sp_matrix(&dl, &r->trans, G_MTX_LOAD | G_MTX_MODELVIEW);
	sp_matrix(&dl, &r->rot_y, G_MTX_MUL | G_MTX_MODELVIEW);
	sp_matrix(&dl, &r->rot_x, G_MTX_MUL | G_MTX_MODELVIEW);
	sp_num_lights(&dl, 1);
	sp_light(&dl, &r->sun, 0);
	sp_light(&dl, &r->ambient, 1);
	sp_geometry(&dl, 0, G_ZBUFFER | G_SHADE | G_LIGHTING | G_CULL_BACK | G_SHADING_SMOOTH);
	dp_othermode(&dl, CYC_1CYCLE, Z_CMP | Z_UPD | BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_SHADE, CCA_0, CCA_0, CCA_0, CC_SHADE);
	for (int half = 0; half < 2; half++) {
		sp_vertex(&dl, &r->cube[half * 12], 12, 0);
		for (int f = 0; f < 3; f++)
			sp_tri2(&dl, 4 * f, 4 * f + 1, 4 * f + 2, 4 * f, 4 * f + 2, 4 * f + 3);
	}

	/* I: mapeo de entorno (G_TEXTURE_GEN) con el tablero, bilineal. */
	sp_matrix(&dl, &r->ident, G_MTX_LOAD | G_MTX_PROJECTION);
	sp_matrix(&dl, &r->ortho, G_MTX_LOAD | G_MTX_MODELVIEW);
	sp_lookat(&dl, &r->look_x, 0);
	sp_lookat(&dl, &r->look_y, 1);
	sp_geometry(&dl, G_CULL_BACK | G_ZBUFFER, G_LIGHTING | G_TEXTURE_GEN | G_SHADING_SMOOTH);
	load_checker(&dl, 0, 1);
	dp_othermode(&dl, CYC_1CYCLE | TF_BILERP | TP_PERSP, BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_TEXEL0, CCA_0, CCA_0, CCA_0, CC_D_1);
	sp_texture(&dl, 0x0200, 0x0200, 0, 1);
	sp_vertex(&dl, r->quad_i, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* K: w = 1000 y PRIM·TEXEL0_ALPHA con el tablero (alfa 1): naranja.
	 * El término de TEXEL0_ALPHA pasa por una entrada que el fragment
	 * program lee con MUL; sin corrección de perspectiva saldría ÷1000. */
	sp_matrix(&dl, &r->far, G_MTX_LOAD | G_MTX_PROJECTION);
	sp_geometry(&dl, G_LIGHTING | G_TEXTURE_GEN, 0);
	load_checker(&dl, 0, 0);
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT | TP_PERSP, BL_XLU);
	dp_prim_color(&dl, 0xFF8000FF);
	dp_combine(&dl, CC_PRIM, CC_B_0, CC_C_TEXEL0_ALPHA, CC_D_0, CCA_0, CCA_0, CCA_0, CC_D_1);
	sp_texture(&dl, 0xFFFF, 0xFFFF, 0, 1);
	sp_vertex(&dl, r->quad_k, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* P: 2 ciclos, (TEXEL0 − 0)·TEXEL1 + 0 y luego (COMBINED − 0)·SHADE +
	 * 0; amarillo en la tile 0 y magenta en la 1: rojo × S = (1, 0, 0).
	 * Textura × textura: no cabe en la combinación lineal del RSX (y la
	 * aproximación de antes daría amarillo × S). */
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->yellow);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->magenta);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 32, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 1, 0, 0, 0, 0, 2, 0);
	dp_tile_size(&dl, 0, 0, 0, 3, 3);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 1, 32, 1, 0, 0, 2, 0);
	dp_tile_size(&dl, 1, 0, 0, 3, 3);
	dp_othermode(&dl, CYC_2CYCLE | TF_POINT, BL_XLU);
	{
		/* Ciclo 0: a=TEXEL0, b=0, c=TEXEL1 (2), d=0; alfa 0·0 + 1.
		 * Ciclo 1: a=COMBINED, b=0, c=SHADE (4), d=0; alfa 0·0 + 1. */
		uint32_t hi = 1u << 20 | 2u << 15 | 7u << 12 | 7u << 9 | 0u << 5 | 4u;
		uint32_t lo = 15u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 7u << 15 | 7u << 12 | 6u << 9 |
			      7u << 6 | 7u << 3 | 6u;
		cmd(&dl, 0xFC000000u | hi, lo);
	}
	sp_texture(&dl, 0xFFFF, 0xFFFF, 0, 1);
	sp_geometry(&dl, G_CULL_BACK | G_CULL_FRONT | G_ZBUFFER | G_LIGHTING | G_TEXTURE_GEN, G_SHADE | G_SHADING_SMOOTH);
	sp_vertex(&dl, r->quad_p, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);

	/* Q: la niebla por luz de GoldenEye: ciclo 0 (TEXEL0 − ENV)·SHADE_ALPHA
	 * + ENV, ciclo 1 (COMBINED − 0)·SHADE + 0, con ENV azul y Sa = 0,5:
	 * (0,5, 0,5, 0,5) × S = (0,5, 0,25, 0,5). Sombreado × sombreado: la
	 * aproximación de antes (sin el factor Sa) daría T0 × S = (1, 0,5, 0). */
	dp_env_color(&dl, 0x0000FFFF);
	{
		/* Ciclo 0: a=TEXEL0, b=ENV (5), c=SHADE_ALPHA (11), d=ENV (5);
		 * alfa 0·0 + 1. Ciclo 1: a=COMBINED, b=0, c=SHADE, d=0; alfa 0·0 + 1. */
		uint32_t hi = 1u << 20 | 11u << 15 | 7u << 12 | 7u << 9 | 0u << 5 | 4u;
		uint32_t lo = 5u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 5u << 15 | 7u << 12 | 6u << 9 |
			      7u << 6 | 7u << 3 | 6u;
		cmd(&dl, 0xFC000000u | hi, lo);
	}
	sp_vertex(&dl, r->quad_q, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* L: TEXRECT ×2 con una textura RGBA16 de 4×4 que cambia el CPU. */
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->chg);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 1, 0, 0, 0, 0, 2, 0);
	dp_tile_size(&dl, 0, 0, 0, 3, 3);
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT, BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_TEXEL0, CCA_0, CCA_0, CCA_0, CC_D_1);
	sp_texrect(&dl, 0, 270, 220, 278, 228, 0, 0, 512, 512);

	/* M: como F, con la paleta que cambia el CPU. */
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->pal2);
	dp_tile(&dl, FMT_RGBA, SIZ_4, 0, 0x100, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_tlut(&dl, 7, 16);
	dp_sync(&dl, SYNC_PIPE);
	dp_texture_image(&dl, FMT_CI, SIZ_16, 1, r->ci);
	dp_tile(&dl, FMT_CI, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 16, 2048);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_CI, SIZ_4, 1, 0, 0, 0, 2, 0, 0);
	dp_tile_size(&dl, 0, 0, 0, 15, 3);
	dp_othermode(&dl, CYC_COPY | TT_RGBA16, 0);
	sp_texrect(&dl, 0, 250, 220, 265, 223, 0, 0, 4 << 10, 1 << 10);

	/* N: textura RGBA16 con alfa de 1 bit y G_RM_TEX_EDGE (FORCE_BL, in·0 +
	 * in·1, cobertura × alfa; el medidor de energía de SM64): la mitad con
	 * alfa 0 no se escribe y se ve el fondo. */
	sp_matrix(&dl, &r->ident, G_MTX_LOAD | G_MTX_PROJECTION);
	dp_texture_image(&dl, FMT_RGBA, SIZ_16, 1, r->edge);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 0, 0, 7, 0, 0, 0, 0);
	dp_sync(&dl, SYNC_LOAD);
	dp_load_block(&dl, 7, 64, 1024);
	dp_sync(&dl, SYNC_PIPE);
	dp_tile(&dl, FMT_RGBA, SIZ_16, 2, 0, 0, 0, 2, 3, 0);
	dp_tile_size(&dl, 0, 0, 0, 7, 7);
	dp_othermode(&dl, CYC_1CYCLE | TF_POINT, RM_TEX_EDGE);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_TEXEL0, CCA_0, CCA_0, CCA_0, CC_TEXEL0);
	sp_texture(&dl, 0xFFFF, 0xFFFF, 0, 1);
	sp_vertex(&dl, r->quad_n, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_texture(&dl, 0, 0, 0, 0);

	/* O: viewport chico y clip ratio 1 con las tijeras de toda la pantalla
	 * (la demo de Mario Kart 64): lo que sale de la ventana no se dibuja. */
	sp_matrix(&dl, &r->ident, G_MTX_LOAD | G_MTX_PROJECTION);
	sp_matrix(&dl, &r->ortho, G_MTX_LOAD | G_MTX_MODELVIEW);
	dp_othermode(&dl, CYC_1CYCLE, BL_XLU);
	dp_combine(&dl, CC_A_0, CC_B_0, CC_C_0, CC_SHADE, CCA_0, CCA_0, CCA_0, CC_SHADE);
	sp_geometry(&dl, G_ZBUFFER | G_CULL_FRONT | G_CULL_BACK | G_LIGHTING | G_TEXTURE_GEN,
		    G_SHADE | G_SHADING_SMOOTH);
	sp_clip_ratio(&dl, 1);
	sp_viewport(&dl, r->vp_o);
	sp_vertex(&dl, r->quad_o, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);
	sp_viewport(&dl, r->vp);
	sp_clip_ratio(&dl, 2);

	/* R: cruza el plano cercano; sin z-buffer, lo de detrás no se dibuja. */
	sp_vertex(&dl, r->quad_r, 4, 0);
	sp_tri2(&dl, 0, 1, 2, 0, 2, 3);

	/* Tramo de otro microcódigo (como S2DEX en Zelda OoT): G_LOAD_UCODE a
	 * otra dirección, comandos que la HLE debe saltar (si los ejecutara, el
	 * FILLRECT rojo taparía la escena y expect.h lo vería) y la vuelta al
	 * microcódigo de la tarea. En GFXTEST_CUT_FRAME la lista termina dentro
	 * del tramo, sin volver ni llegar al G_RDPFULLSYNC: la interrupción del
	 * DP tiene que llegar de todos modos. */
#ifndef GBI_F3D  /* F3D no tiene G_LOAD_UCODE */
	sp_load_ucode(&dl, PHYS(r->ucode) + 0x100, PHYS(r->ucode_data), sizeof r->ucode_data);
	dp_othermode(&dl, CYC_FILL, 0);
	dp_fill_color(&dl, 0xF801F801u);
	dp_fill_rect(&dl, 0, 0, W - 1, H - 1);
	if (frame == GFXTEST_CUT_FRAME) {
		sp_end(&dl);
		return start;
	}
	sp_load_ucode(&dl, PHYS(r->ucode), PHYS(r->ucode_data), sizeof r->ucode_data);
#endif

	dp_sync(&dl, SYNC_FULL);
	sp_end(&dl);
	return start;
}

/* ---- RSP ------------------------------------------------------------------ */

static void sp_dma_to(uint32_t mem, const volatile void *src, uint32_t len)
{
	SP_MEM_ADDR = mem;
	SP_DRAM_ADDR = PHYS(src);
	SP_RD_LEN = len - 1;
}

static void start_task(const gfx *dl, uint32_t dl_bytes)
{
	volatile struct rsp_data *r = R;
	volatile struct task *t = &r->task;
	t->type = 1;
	t->flags = 0;
	t->ucode_boot = 0;
	t->ucode_boot_size = 0;
	t->ucode = PHYS(r->ucode);
	t->ucode_size = sizeof r->ucode;
	t->ucode_data = PHYS(r->ucode_data);
	t->ucode_data_size = sizeof r->ucode_data;
	t->dram_stack = PHYS(r->dram_stack);
	t->dram_stack_size = sizeof r->dram_stack;
	t->output_buff = 0;
	t->output_buff_size = 0;
	t->data_ptr = PHYS(dl);
	t->data_size = dl_bytes;
	t->yield_data_ptr = 0;
	t->yield_data_size = 0;

	sp_dma_to(0x1000, r->ucode, 16);          /* IMEM: el "microcódigo" */
	sp_dma_to(0xFC0, t, sizeof(struct task)); /* DMEM 0xFC0: la tarea */
	SP_PC = 0;
	/* Quitar HALT y BROKE, interrupción en BREAK, señales 0–2 en cero. */
	SP_STATUS = 0x001 | 0x004 | 0x100 | 0x200 | 0x800 | 0x2000;
}

static uint32_t count(void);

static int wait_intr(uint32_t bit)
{
	uint32_t t0 = count();
	for (uint32_t i = 0;; i++) {
		if (MI_INTR & bit)
			return 1;
		/* Count solo de vez en cuando: leerlo en cada vuelta (MFC0) saca
		 * al recompilador de Salsa64 a su camino lento y el bucle se
		 * arrastra (9 campos/s en la PS3). */
		if ((i & 1023) == 0 && count() - t0 >= TIMEOUT_TICKS)
			return 0;
	}
}

/* ---- Video e ISViewer (como vitest) --------------------------------------- */

static void vi_setup(int pal, const void *origin)
{
	VI_CONTROL = 0;
	VI_ORIGIN = PHYS(origin);
	VI_WIDTH = W;
	VI_V_INTR = 2;
	VI_V_CURRENT = 0;
	if (pal) {
		VI_BURST = 0x0404233A;
		VI_V_SYNC = 0x271;
		VI_H_SYNC = 0x00150C69;
		VI_H_SYNC_LEAP = 0x0C6F0C6E;
		VI_H_VIDEO = 0x00800300;
		VI_V_VIDEO = 0x005F0239;
		VI_V_BURST = 0x0009026B;
	} else {
		VI_BURST = 0x03E52239;
		VI_V_SYNC = 0x20D;
		VI_H_SYNC = 0x00000C15;
		VI_H_SYNC_LEAP = 0x0C150C15;
		VI_H_VIDEO = 0x006C02EC;
		VI_V_VIDEO = 0x002501FF;
		VI_V_BURST = 0x000E0204;
	}
	VI_X_SCALE = 0x200;
	VI_Y_SCALE = 0x400;
	VI_CONTROL = 0x3202;  /* RGBA5551 */
}

static int isv_present(void)
{
	ISV_MAGIC = 0x56495354;
	return ISV_MAGIC == 0x56495354;
}

static char msg[200];
static unsigned msg_len;

static void put(const char *s)
{
	while (*s && msg_len < sizeof msg)
		msg[msg_len++] = *s++;
}

static void put_u(uint32_t v)
{
	char tmp[11];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && msg_len < sizeof msg)
		msg[msg_len++] = tmp[--n];
}

static void isv_flush(void)
{
	for (unsigned i = 0; i < msg_len; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4; k++)
			w = w << 8 | (i + k < msg_len ? (uint8_t)msg[i + k] : 0);
		REG(ISV_BUF + i) = w;
	}
	ISV_LEN = msg_len;
	msg_len = 0;
}

static uint32_t count(void)
{
	uint32_t v;
	__asm__ volatile("mfc0 %0, $9\n\tnop" : "=r"(v));
	return v;
}

/* ---- Programa ------------------------------------------------------------- */

int main(void)
{
	int pal = ((BOOT_FLAGS >> 16) & 0xFF) == 0;
	int isv = isv_present();
	uint32_t cur = 0, frame = 0, errors = 0, first_error = 0;
	uint32_t sp_ticks = 0, dp_ticks = 0;
	const char *what = "";

	init_data();
	vi_setup(pal, fb[0]);

	for (;;) {
		uint16_t *color = fb[cur];
		if (frame == GFXTEST_CHANGE_FRAME)
			change_textures();
		gfx *dl = build_dl(color, frame);
		uint32_t dl_bytes = 8 * 1024;

		uint32_t t0 = count();
		start_task(dl, dl_bytes);
		if (!wait_intr(MI_SP)) {
			errors++, what = "sin interrupción SP";
		} else {
			uint32_t t1 = count();
			uint32_t st = SP_STATUS;
			if ((st & (SP_HALT | SP_BROKE | SP_TASKDONE)) != (SP_HALT | SP_BROKE | SP_TASKDONE))
				errors++, what = "SP_STATUS sin HALT|BROKE|TASKDONE";
			SP_STATUS = 0x008;  /* bajar la interrupción SP */
			if (!wait_intr(MI_DP)) {
				errors++, what = "sin interrupción DP";
			} else {
				uint32_t t2 = count();
				sp_ticks += t1 - t0;
				dp_ticks += t2 - t0;
			}
			MI_MODE = 0x0800;  /* bajar la interrupción DP */
		}
		if (errors && !first_error)
			first_error = frame + 1;

		/* Mostrar este cuadro desde el próximo campo. */
		VI_V_CURRENT = 0;
		wait_intr(MI_VI);
		VI_V_CURRENT = 0;
		VI_ORIGIN = PHYS(color);
		cur ^= 1;
		frame++;

		if (frame == GFXTEST_FRAMES && isv) {
			put(errors ? GFXTEST_FAIL_TEXT : GFXTEST_OK_TEXT);
			put(" cuadros=");
			put_u(frame);
			put(" errores=");
			put_u(errors);
			if (errors) {
				put(" primero=");
				put_u(first_error);
				put(" (");
				put(what);
				put(")");
			}
			put(" count_sp=");
			put_u(sp_ticks / frame);
			put(" count_dp=");
			put_u(dp_ticks / frame);
			put("\n");
			isv_flush();
		}
	}
}
