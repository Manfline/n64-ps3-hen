/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — gfxtest: codificación propia de los comandos del RSP y del RDP
 * que usa la ROM (no es el gbi.h de libultra). Escrita a partir de la
 * documentación pública (n64brew.dev, "Reality Display Processor/Commands"
 * y la GBI de F3DEX2 descrita ahí; para F3D y F3DEX 1.x, la documentación
 * de la comunidad de SM64 sobre Fast3D; docs/THIRD_PARTY.md).
 *
 * La ROM se compila para una GBI (Makefile, GBI=...): F3DEX2 (por
 * omisión), F3D (Fast3D) o F3DEX (1.x). Las escenas son las mismas; cambia
 * la codificación de los comandos del RSP y el texto que identifica al
 * "microcódigo" (GBI_MARKER). Los del RDP son iguales en las tres.
 *
 * Cada función agrega un comando de 64 bits en *dl. Las direcciones son
 * físicas (el RSP no ve el espacio virtual del CPU).
 */
#ifndef SALSA64_GFXTEST_GBI_H
#define SALSA64_GFXTEST_GBI_H

#include <stdint.h>

typedef struct {
	uint32_t w0, w1;
} gfx;

#define PHYS(p) ((uint32_t)(uintptr_t)(p) & 0x1FFFFFFFu)

#if defined(GBI_F3D) || defined(GBI_F3DEX)
#define GBI_1 1  /* primera generación: F3D y F3DEX 1.x */
#endif

#if defined(GBI_F3D)
#define GBI_MARKER "Salsa64 GBI Fast3D"
#elif defined(GBI_F3DEX)
#define GBI_MARKER "Salsa64 GBI F3DEX1"
#else
#define GBI_MARKER "Salsa64 GBI F3DEX2"
#endif

/* Índice de vértice dentro de un comando: ×10 en F3D, ×2 en las demás. */
#ifdef GBI_F3D
#define VI(v) ((v) * 10u)
#else
#define VI(v) ((v) * 2u)
#endif

static inline void cmd(gfx **dl, uint32_t w0, uint32_t w1)
{
	volatile uint32_t *p = (volatile uint32_t *)*dl;  /* el DL está en KSEG1 */
	p[0] = w0;
	p[1] = w1;
	(*dl)++;
}

/* ---- RSP ---- */
#ifdef GBI_1
#define G_MTX_NOPUSH     0x00u
#define G_MTX_PUSH       0x04u
#define G_MTX_MUL        0x00u
#define G_MTX_LOAD       0x02u
#define G_MTX_MODELVIEW  0x00u
#define G_MTX_PROJECTION 0x01u

#define G_ZBUFFER            0x00000001u
#define G_SHADE              0x00000004u
#define G_SHADING_SMOOTH     0x00000200u
#define G_CULL_FRONT         0x00001000u
#define G_CULL_BACK          0x00002000u
#define G_LIGHTING           0x00020000u
#define G_TEXTURE_GEN        0x00040000u
#else
#define G_MTX_NOPUSH     0x00u
#define G_MTX_PUSH       0x01u
#define G_MTX_MUL        0x00u
#define G_MTX_LOAD       0x02u
#define G_MTX_MODELVIEW  0x00u
#define G_MTX_PROJECTION 0x04u

#define G_ZBUFFER            0x00000001u
#define G_SHADE              0x00000004u
#define G_CULL_FRONT         0x00000200u
#define G_CULL_BACK          0x00000400u
#define G_LIGHTING           0x00020000u
#define G_TEXTURE_GEN        0x00040000u
#define G_SHADING_SMOOTH     0x00200000u
#endif

/* n vértices desde el lugar v0 del búfer (F3D: 16 lugares; F3DEX: 32). */
static inline void sp_vertex(gfx **dl, const volatile void *v, unsigned n, unsigned v0)
{
#if defined(GBI_F3D)
	cmd(dl, 0x04000000u | (n - 1) << 20 | v0 << 16 | n * 16, PHYS(v));
#elif defined(GBI_F3DEX)
	cmd(dl, 0x04000000u | VI(v0) << 16 | n << 10 | (n * 16 - 1), PHYS(v));
#else
	cmd(dl, 0x01000000u | n << 12 | (v0 + n) << 1, PHYS(v));
#endif
}

static inline void sp_tri(gfx **dl, unsigned a, unsigned b, unsigned c)
{
#ifdef GBI_1
	cmd(dl, 0xBF000000u, VI(a) << 16 | VI(b) << 8 | VI(c));
#else
	cmd(dl, 0x05000000u | a << 17 | b << 9 | c << 1, 0);
#endif
}

/* F3D no tiene G_TRI2: van dos G_TRI1. */
static inline void sp_tri2(gfx **dl, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f)
{
#if defined(GBI_F3D)
	sp_tri(dl, a, b, c);
	sp_tri(dl, d, e, f);
#elif defined(GBI_F3DEX)
	cmd(dl, 0xB1000000u | VI(a) << 16 | VI(b) << 8 | VI(c), VI(d) << 16 | VI(e) << 8 | VI(f));
#else
	cmd(dl, 0x06000000u | a << 17 | b << 9 | c << 1, d << 17 | e << 9 | f << 1);
#endif
}

static inline void sp_matrix(gfx **dl, const volatile void *m, unsigned flags)
{
#ifdef GBI_1
	cmd(dl, 0x01000040u | flags << 16, PHYS(m));
#else
	cmd(dl, 0xDA380000u | (flags ^ G_MTX_PUSH), PHYS(m));
#endif
}

static inline void sp_viewport(gfx **dl, const volatile void *vp)
{
#ifdef GBI_1
	cmd(dl, 0x03800010u, PHYS(vp));  /* G_MV_VIEWPORT */
#else
	cmd(dl, 0xDC080008u, PHYS(vp));
#endif
}

/* gSPClipRatio: el RSP recorta los triángulos contra el viewport por r
 * (G_MOVEWORD G_MW_CLIP en las cuatro posiciones; las positivas con -r). */
static inline void sp_clip_ratio(gfx **dl, int r)
{
	static const unsigned off[4] = { 0x04, 0x0C, 0x14, 0x1C };
	for (int i = 0; i < 4; i++) {
		uint32_t v = (uint16_t)(i < 2 ? r : -r);
#ifdef GBI_1
		cmd(dl, 0xBC000004u | off[i] << 8, v);
#else
		cmd(dl, 0xDB040000u | off[i], v);
#endif
	}
}

/* En la primera generación, G_CLEARGEOMETRYMODE y G_SETGEOMETRYMODE. */
static inline void sp_geometry(gfx **dl, uint32_t clear, uint32_t set)
{
#ifdef GBI_1
	if (clear)
		cmd(dl, 0xB6000000u, clear);
	if (set)
		cmd(dl, 0xB7000000u, set);
#else
	cmd(dl, 0xD9000000u | (~clear & 0x00FFFFFFu), set);
#endif
}

static inline void sp_texture(gfx **dl, unsigned s, unsigned t, unsigned tile, unsigned on)
{
#ifdef GBI_1
	cmd(dl, 0xBB000000u | tile << 8 | on, s << 16 | t);
#else
	cmd(dl, 0xD7000000u | tile << 8 | on << 1, s << 16 | t);
#endif
}

static inline void sp_num_lights(gfx **dl, unsigned n)
{
#ifdef GBI_1
	cmd(dl, 0xBC000002u, 0x80000000u + (n + 1) * 32);  /* fin de las luces en la DMEM */
#else
	cmd(dl, 0xDB020000u, n * 24);
#endif
}

/* Luz i (0 = la primera; la ambiental va en i = número de luces). */
static inline void sp_light(gfx **dl, const volatile void *l, unsigned i)
{
#ifdef GBI_1
	cmd(dl, 0x03000010u | (0x86u + 2 * i) << 16, PHYS(l));  /* G_MV_L0 + 2·i */
#else
	cmd(dl, 0xDC08000Au | ((i + 2) * 24 / 8) << 8, PHYS(l));
#endif
}

static inline void sp_lookat(gfx **dl, const volatile void *l, unsigned axis)
{
#ifdef GBI_1
	cmd(dl, axis ? 0x03820010u : 0x03840010u, PHYS(l));  /* G_MV_LOOKATY / X */
#else
	cmd(dl, 0xDC08000Au | (axis * 24 / 8) << 8, PHYS(l));
#endif
}

/* gSPLoadUcodeEx: G_RDPHALF_1 con los datos y G_LOAD_UCODE con el texto
 * (en la primera generación, 0xB4 y 0xAF; F3D no lo tiene, pero F3DEX 1.x
 * sí y la HLE lo trata igual). */
static inline void sp_load_ucode(gfx **dl, uint32_t ucode, uint32_t data, unsigned dsize)
{
#ifdef GBI_1
	cmd(dl, 0xB4000000u, data);
	cmd(dl, 0xAF000000u | ((dsize - 1) & 0xFFFFu), ucode);
#else
	cmd(dl, 0xE1000000u, data);
	cmd(dl, 0xDD000000u | ((dsize - 1) & 0xFFFFu), ucode);
#endif
}

static inline void sp_end(gfx **dl)
{
#ifdef GBI_1
	cmd(dl, 0xB8000000u, 0);
#else
	cmd(dl, 0xDF000000u, 0);
#endif
}

/* ---- RDP ---- */
enum { FMT_RGBA = 0, FMT_CI = 2, FMT_IA = 3, FMT_I = 4 };
enum { SIZ_4 = 0, SIZ_8 = 1, SIZ_16 = 2, SIZ_32 = 3 };

/* othermode H */
#define CYC_1CYCLE (0u << 20)
#define CYC_2CYCLE (1u << 20)
#define CYC_COPY   (2u << 20)
#define CYC_FILL   (3u << 20)
#define TT_RGBA16  (2u << 14)
#define TF_POINT   (0u << 12)
#define TF_BILERP  (2u << 12)
#define TP_PERSP   (1u << 19)

/* othermode L */
#define AC_THRESHOLD 0x0001u
#define AA_EN        0x0008u
#define Z_CMP        0x0010u
#define Z_UPD        0x0020u
#define CVG_X_ALPHA  0x1000u
#define ALPHA_CVG_SEL 0x2000u
#define FORCE_BL     0x4000u
/* Mezcla en los dos ciclos: in·a + mem·(1−a). */
#define BL_XLU       ((0u << 30) | (0u << 26) | (1u << 22) | (0u << 18) | \
                      (0u << 28) | (0u << 24) | (1u << 20) | (0u << 16))
/* Sin FORCE_BL el blender deja pasar el color: BL_XLU sirve para lo opaco. */
/* in·0 + in·1 en los dos ciclos, con la cobertura multiplicada por el alfa:
 * los texels con alfa 0 no se escriben (bordes de texturas recortadas). */
#define BL_IN        ((0u << 30) | (3u << 26) | (0u << 22) | (2u << 18) | \
                      (0u << 28) | (3u << 24) | (0u << 20) | (2u << 16))
#define RM_TEX_EDGE  (AA_EN | CVG_X_ALPHA | ALPHA_CVG_SEL | FORCE_BL | BL_IN)

static inline void dp_othermode(gfx **dl, uint32_t h, uint32_t l)
{
	cmd(dl, 0xEF000000u | h, l);
}

/* Combiner igual en los dos ciclos: (a − b)·c + d para color y para alfa. */
enum { CC_COMBINED = 0, CC_TEXEL0 = 1, CC_TEXEL1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ENV = 5 };
#define CC_A_0   15u
#define CC_B_0   15u
#define CC_C_0   31u
#define CC_C_TEXEL0_ALPHA 8u
#define CC_D_0   7u
#define CC_D_1   6u
#define CCA_0    7u

static inline void dp_combine(gfx **dl, unsigned a, unsigned b, unsigned c, unsigned d, unsigned aa, unsigned ab,
			      unsigned ac, unsigned ad)
{
	uint32_t hi = a << 20 | c << 15 | aa << 12 | ac << 9 | a << 5 | c;
	uint32_t lo = b << 28 | b << 24 | aa << 21 | ac << 18 | d << 15 | ab << 12 | ad << 9 | d << 6 | ab << 3 | ad;
	cmd(dl, 0xFC000000u | hi, lo);
}

static inline void dp_color_image(gfx **dl, unsigned fmt, unsigned siz, unsigned width, const volatile void *p)
{
	cmd(dl, 0xFF000000u | fmt << 21 | siz << 19 | (width - 1), PHYS(p));
}

static inline void dp_z_image(gfx **dl, const volatile void *p)
{
	cmd(dl, 0xFE000000u, PHYS(p));
}

static inline void dp_texture_image(gfx **dl, unsigned fmt, unsigned siz, unsigned width, const volatile void *p)
{
	cmd(dl, 0xFD000000u | fmt << 21 | siz << 19 | (width - 1), PHYS(p));
}

/* line en palabras de 64 bits; tmem en palabras de 64 bits. */
static inline void dp_tile(gfx **dl, unsigned fmt, unsigned siz, unsigned line, unsigned tmem, unsigned tile,
			   unsigned pal, unsigned cm, unsigned mask, unsigned shift)
{
	cmd(dl, 0xF5000000u | fmt << 21 | siz << 19 | line << 9 | tmem,
	    tile << 24 | pal << 20 | cm << 18 | mask << 14 | shift << 10 | cm << 8 | mask << 4 | shift);
}

/* Coordenadas en texels (se pasan a 10.2). */
static inline void dp_tile_size(gfx **dl, unsigned tile, unsigned uls, unsigned ult, unsigned lrs, unsigned lrt)
{
	cmd(dl, 0xF2000000u | (uls << 2) << 12 | ult << 2, tile << 24 | (lrs << 2) << 12 | lrt << 2);
}

static inline void dp_load_block(gfx **dl, unsigned tile, unsigned texels, unsigned dxt)
{
	cmd(dl, 0xF3000000u, tile << 24 | (texels - 1) << 12 | dxt);
}

static inline void dp_load_tlut(gfx **dl, unsigned tile, unsigned count)
{
	cmd(dl, 0xF0000000u, tile << 24 | ((count - 1) << 2) << 12);
}

static inline void dp_sync(gfx **dl, unsigned op)
{
	cmd(dl, op << 24, 0);
}
#define SYNC_LOAD 0xE6u
#define SYNC_PIPE 0xE7u
#define SYNC_TILE 0xE8u
#define SYNC_FULL 0xE9u

static inline void dp_fill_color(gfx **dl, uint32_t c)
{
	cmd(dl, 0xF7000000u, c);
}

/* Rectángulo de relleno, bordes inclusivos (modo FILL). */
static inline void dp_fill_rect(gfx **dl, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	cmd(dl, 0xF6000000u | (x1 << 2) << 12 | y1 << 2, (x0 << 2) << 12 | y0 << 2);
}

static inline void dp_scissor(gfx **dl, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	cmd(dl, 0xED000000u | (x0 << 2) << 12 | y0 << 2, (x1 << 2) << 12 | y1 << 2);
}

static inline void dp_prim_color(gfx **dl, uint32_t rgba)
{
	cmd(dl, 0xFA000000u, rgba);
}

static inline void dp_env_color(gfx **dl, uint32_t rgba)
{
	cmd(dl, 0xFB000000u, rgba);
}

static inline void dp_blend_color(gfx **dl, uint32_t rgba)
{
	cmd(dl, 0xF9000000u, rgba);
}

/* Rectángulo con textura: (x0, y0)–(x1, y1) en píxeles; s, t en s10.5;
 * dsdx, dtdy en s5.10. Va en tres comandos: el del RDP y dos G_RDPHALF_*
 * (en F3D, G_RDPHALF_2 y G_RDPHALF_CONT, como en Super Mario 64). */
static inline void sp_texrect(gfx **dl, unsigned tile, unsigned x0, unsigned y0, unsigned x1, unsigned y1,
			      int s, int t, int dsdx, int dtdy)
{
#if defined(GBI_F3D)
	const uint32_t half_st = 0xB3000000u, half_d = 0xB2000000u;
#elif defined(GBI_F3DEX)
	const uint32_t half_st = 0xB4000000u, half_d = 0xB3000000u;
#else
	const uint32_t half_st = 0xE1000000u, half_d = 0xF1000000u;
#endif
	cmd(dl, 0xE4000000u | (x1 << 2) << 12 | y1 << 2, tile << 24 | (x0 << 2) << 12 | y0 << 2);
	cmd(dl, half_st, (uint32_t)(uint16_t)s << 16 | (uint16_t)t);
	cmd(dl, half_d, (uint32_t)(uint16_t)dsdx << 16 | (uint16_t)dtdy);
}

#endif
