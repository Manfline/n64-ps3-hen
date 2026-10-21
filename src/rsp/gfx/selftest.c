/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — tareas gráficas de prueba. Ver selftest.h.
 */
#include "rsp/gfx/selftest.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/gfx/gfx.h"

/* Mapa de la RDRAM de mentira. */
#define DL_PA      0x0000u  /* display list (hasta 0x1000) */
#define VP_PA      0x1000u  /* viewport */
#define PROJ_PA    0x1040u  /* matrices */
#define MV_PA      0x1080u
#define VTX_PA     0x1100u  /* 8 vértices de 16 bytes */
#define TEX_PA     0x2000u  /* RGBA16 16 × 32 (1 KB) */
#define CI_PA      0x2800u  /* CI4 16 × 16 (128 bytes) */
#define TLUT_PA    0x2900u  /* 16 colores RGBA16 */
#define S2_DATA_PA 0x3000u  /* "datos" del S2DEX2: solo su texto */
#define S2_TEXT_PA 0x3800u  /* "código" del S2DEX2: solo la dirección */
#define BG_PA      0x3C00u  /* uObjBg */
#define BG_IMG_PA  0x4000u  /* RGBA16 64 × 32 (4 KB) */
#define FLX_DATA_PA 0x5000u /* "datos" del F3DFLX: solo su texto */
#define FLX_TEXT_PA 0x5400u /* "código" del F3DFLX: solo la dirección */
#define FLX_TAB_PA  0x5800u /* tabla de alfa de F3DFLX (256 bytes) */
#define F5VTX_PA   0x1200u  /* Rogue Squadron: 8 vértices de 8 bytes */
#define F5COL_PA   0x1240u  /* tabla de colores de los vértices (8 palabras) */
#define F5HGT_PA   0x1280u  /* alturas de la malla generada (bytes) */
#define F5CLR_PA   0x1300u  /* colores de la malla generada (palabras) */
#define INLIGHT_PA 0x1400u  /* Indiana Jones: luces (0x30 bytes, a 0xB00 de la DMEM) */
#define INTAB_PA   0x1440u  /* y la tabla de desplazamientos (16 bytes, a 0x380) */
#define INCOL_PA   0x1460u  /* colores que entran a la iluminación (32 bytes, a 0x480) */
#define INPART_PA  0x1500u  /* 32 vértices de partículas (256 bytes) */
#define CIMG_PA    0x10000u /* color image y z image: solo las direcciones */
#define ZIMG_PA    0x18000u

enum fam { FAM_F3D, FAM_F3DEX, FAM_F3DWR, FAM_F3DEX2, FAM_F5, FAM_INDY };

struct b {
	uint8_t *ram;
	uint32_t pc;
	enum fam fam;
	uint32_t rnd;
	bool tex_on;  /* la última G_TEXTURE: Rogue Squadron manda las coordenadas con el triángulo si es así */
};

static uint32_t rnd(struct b *b)
{
	b->rnd = b->rnd * 1664525u + 1013904223u;
	return b->rnd >> 8;
}

static void dl(struct b *b, uint32_t w0, uint32_t w1)
{
	salsa64_wr32(b->ram + b->pc, w0);
	salsa64_wr32(b->ram + b->pc + 4, w1);
	b->pc += 8;
}

static bool gen2(const struct b *b)
{
	return b->fam == FAM_F3DEX2;
}

/* Mtx de libultra: 16 partes enteras y 16 fracciones (s15.16). */
static void put_matrix(uint8_t *p, const int32_t m[16])
{
	for (int i = 0; i < 16; i++) {
		salsa64_wr16(p + 2 * i, (uint16_t)((uint32_t)m[i] >> 16));
		salsa64_wr16(p + 32 + 2 * i, (uint16_t)m[i]);
	}
}

static void othermode_h(struct b *b, int shift, int len, uint32_t data)
{
	if (b->fam == FAM_INDY)
		/* El microcódigo de Indiana Jones cuenta los bits desde el de arriba: la máscara son
		 * len bits y va corrida 31 - (shift + len - 1). */
		dl(b, 0xBA000000u | (uint32_t)(31 - (shift + len - 1)) << 8 | (uint32_t)(len - 1), data);
	else if (gen2(b))
		dl(b, 0xE3000000u | (uint32_t)(32 - shift - len) << 8 | (uint32_t)(len - 1), data);
	else
		dl(b, 0xBA000000u | (uint32_t)shift << 8 | (uint32_t)len, data);
}

static void cycle(struct b *b, unsigned c)
{
	othermode_h(b, 20, 2, c << 20);
}

/* Combiner igual en los dos ciclos (como tests/unit/test_gfx.c). */
static void combine(struct b *b, unsigned a, unsigned bb, unsigned c, unsigned d, unsigned aa, unsigned ab,
		    unsigned ac, unsigned ad)
{
	uint32_t hi = a << 20 | c << 15 | aa << 12 | ac << 9 | a << 5 | c;
	uint32_t lo = bb << 28 | bb << 24 | aa << 21 | ac << 18 | d << 15 | ab << 12 | ad << 9 | d << 6 | ab << 3 | ad;
	dl(b, 0xFC000000u | hi, lo);
}
enum { CC_T0 = 1, CC_T1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ZERO = 7, CC_C_T0A = 8, CC_C_ZERO = 31, CC_A_ZERO = 7 };

static void texture(struct b *b, unsigned tile, unsigned on)
{
	b->tex_on = on != 0;
	if (gen2(b))
		dl(b, 0xD7000000u | tile << 8 | on << 1, 0x80008000u);
	else
		dl(b, 0xBB000000u | tile << 8 | on, 0x80008000u);
}

static void vtx(struct b *b, uint32_t addr, unsigned v0, unsigned n)
{
	switch (b->fam) {
	case FAM_F5:
		/* Siempre desde el lugar 0, de 8 bytes cada uno. */
		(void)v0, (void)addr;
		dl(b, 0x04000000u | n << 10, F5VTX_PA);
		break;
	case FAM_INDY:
		/* Una DMA a la DMEM 0x170 y de ahí a los lugares 0 a n - 1. */
		(void)v0, (void)addr;
		dl(b, 0x01117000u | (8 * n - 1), n << 24 | F5VTX_PA);
		break;
	case FAM_F3DEX2:
		dl(b, 0x01000000u | n << 12 | (v0 + n) << 1, addr);
		break;
	case FAM_F3DEX:
		dl(b, 0x04000000u | v0 << 17 | n << 10 | (16 * n - 1), addr);
		break;
	case FAM_F3DWR:
		dl(b, 0x04000000u | (v0 * 5) << 16 | n << 9 | (16 * n - 1), addr);
		break;
	default:
		dl(b, 0x04000000u | (n - 1) << 20 | v0 << 16 | 16 * n, addr);
		break;
	}
}

static void tri(struct b *b, unsigned v0, unsigned v1, unsigned v2)
{
	switch (b->fam) {
	case FAM_F5:
		/* G_TRI1: índices ×5, la palabra de los colores (un índice de la tabla
		 * por vértice) y, con textura, las coordenadas de cada vértice. */
		dl(b, 0xBF000000u | (b->tex_on ? 2u : 0u), (v0 * 5) << 16 | (v1 * 5) << 8 | v2 * 5);
		dl(b, (v0 * 4) << 16 | (v1 * 4) << 8 | v2 * 4, 0);
		if (b->tex_on) {
			uint32_t r = rnd(b);
			dl(b, (r & 0x1FFF) << 16 | ((r >> 4) & 0x1FFF), (r >> 3) << 16 | (r & 0x0FFF));
			dl(b, (r >> 8) << 16 | (r & 0x3FF), 0);
		}
		break;
	case FAM_INDY:
		/* G_TRI1: las direcciones de los lugares (0x600 + 40 por vértice), la palabra de los colores
		 * (desplazamientos de 4 bytes en la tabla de 0xD40) y, con textura, las coordenadas. */
		dl(b, 0xBF000000u | (b->tex_on ? 0x200u : 0u), (0x600 + 40 * v0) << 16 | (0x600 + 40 * v1));
		dl(b, (v0 * 4) << 16 | (v1 * 4) << 8 | v2 * 4, (0x600 + 40 * v2) << 16);
		if (b->tex_on) {
			uint32_t r = rnd(b);
			dl(b, (r & 0x1FFF) << 16 | ((r >> 4) & 0x1FFF), (r >> 3) << 16 | (r & 0x0FFF));
			dl(b, (r >> 8) << 16 | (r & 0x3FF), 0);
		}
		break;
	case FAM_F3DEX2:
		dl(b, 0x05000000u | (v0 * 2) << 16 | (v1 * 2) << 8 | v2 * 2, 0);
		break;
	case FAM_F3DEX:
		dl(b, 0xBF000000u, (v0 * 2) << 16 | (v1 * 2) << 8 | v2 * 2);
		break;
	case FAM_F3DWR:
		dl(b, 0xBF000000u, (v0 * 5) << 16 | (v1 * 5) << 8 | v2 * 5);
		break;
	default:
		dl(b, 0xBF000000u, (v0 * 10) << 16 | (v1 * 10) << 8 | v2 * 10);
		break;
	}
}

/* TEXRECT de (x0, y0) a (x1, y1) en píxeles con la tile `tile`, s, t desde
 * 0 y un texel por píxel (en copia, 4 por paso: dsdx 4.0). */
static void texrect(struct b *b, unsigned tile, unsigned x0, unsigned y0, unsigned x1, unsigned y1, bool copy)
{
	if (b->fam == FAM_F5 || b->fam == FAM_INDY) {
		/* 16 bytes: las palabras de s y t y de dsdx y dtdy siguen a las dos primeras, sin opcode. */
		dl(b, 0xE4000000u | (x1 << 2) << 12 | y1 << 2, tile << 24 | (x0 << 2) << 12 | y0 << 2);
		dl(b, 0, (copy ? 4u << 10 : 1u << 10) << 16 | 1u << 10);
		return;
	}
	dl(b, 0xE4000000u | (x1 << 2) << 12 | y1 << 2, tile << 24 | (x0 << 2) << 12 | y0 << 2);
	dl(b, gen2(b) ? 0xE1000000u : 0xB4000000u, 0);
	dl(b, gen2(b) ? 0xF1000000u : 0xB3000000u, (copy ? 4u << 10 : 1u << 10) << 16 | 1u << 10);
}

static void put_texels(struct b *b, uint32_t addr, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes; i += 2)
		salsa64_wr16(b->ram + addr + i, (uint16_t)(rnd(b) | 1));
}

uint32_t gfx_selftest_build(uint8_t *ram, uint32_t gbi, uint32_t seed)
{
	struct b b = { ram, DL_PA, FAM_F3D, seed * 2654435761u + 1, false };
	memset(ram, 0, GFX_SELFTEST_RAM);
	switch (gbi) {
	case SALSA64_GBI_F3DEX:
		b.fam = FAM_F3DEX;
		break;
	case SALSA64_GBI_F3DWR:
		b.fam = FAM_F3DWR;
		break;
	case SALSA64_GBI_F3DEX2:
	case SALSA64_GBI_F3DZEX2:
	case SALSA64_GBI_F3DEXBG:
		b.fam = FAM_F3DEX2;
		break;
	case SALSA64_GBI_F5ROGUE:
		b.fam = FAM_F5;
		break;
	case SALSA64_GBI_F5INDY:
		b.fam = FAM_INDY;
		break;
	default:
		break;
	}

	/* Datos: viewport de 320 × 240, proyección que lleva [−128, 128] a
	 * [−1, 1] (potencias de 2: lo mismo en el SPE y en el PPE), modelview
	 * identidad, ocho vértices con colores y s, t, texturas y paleta. */
	static const int16_t vp[8] = { 640, 480, 511, 0, 640, 480, 511, 0 };
	for (int i = 0; i < 8; i++)
		salsa64_wr16(ram + VP_PA + 2 * i, (uint16_t)vp[i]);
	int32_t m[16] = { 0 };
	m[0] = m[5] = m[10] = 512;  /* 1/128 */
	m[15] = 65536;
	put_matrix(ram + PROJ_PA, m);
	m[0] = m[5] = m[10] = 65536;
	put_matrix(ram + MV_PA, m);
	static const int16_t pos[8][3] = {
		{ -96, -80, 0 }, { 96, -80, 16 }, { 0, 90, 32 }, { -64, 64, -16 },
		{ 64, 64, 8 }, { 112, -32, -32 }, { -112, 16, 48 }, { 32, -112, 0 },
	};
	for (int i = 0; i < 8; i++) {
		uint8_t *p = ram + VTX_PA + 16 * i;
		for (int k = 0; k < 3; k++)
			salsa64_wr16(p + 2 * k, (uint16_t)pos[i][k]);
		salsa64_wr16(p + 8, (uint16_t)((i & 1) * 16 * 32));
		salsa64_wr16(p + 10, (uint16_t)((i >> 1 & 1) * 32 * 32));
		uint32_t c = rnd(&b);
		p[12] = (uint8_t)c;
		p[13] = (uint8_t)(c >> 8);
		p[14] = (uint8_t)(c >> 16);
		p[15] = 0xFF;
	}
	if (gbi == SALSA64_GBI_TURBO3D) {
		/* Dos objetos: carga en 253..255 y reutilización sin matriz ni
		 * vértices nuevos. Datos propios para comparar SPE y PPE. */
		const uint32_t glob = 0x6000, obj = 0x6100, obj2 = 0x6200, tri = 0x6300, rdp = 0x6400;
		salsa64_wr32(ram + glob + 8, 0x00080000);
		memcpy(ram + glob + 80, ram + VP_PA, 16);
		salsa64_wr32(ram + glob + 96, rdp);
		salsa64_wr32(ram + rdp, 0xFF10013F); /* RGBA16, 320 píxeles */
		salsa64_wr32(ram + rdp + 4, CIMG_PA);
		salsa64_wr32(ram + rdp + 8, 0xED000000);
		salsa64_wr32(ram + rdp + 12, (320u * 4 << 12) | (240u * 4));
		salsa64_wr32(ram + rdp + 16, 0xFCFFFFFF);
		salsa64_wr32(ram + rdp + 20, 0xFFFE793C); /* SHADE */
		salsa64_wr32(ram + obj, 4);
		ram[obj + 8] = 3; ram[obj + 9] = 253; ram[obj + 10] = 1;
		memcpy(ram + obj + 24, ram + PROJ_PA, 64);
		salsa64_wr32(ram + obj2, 4);
		ram[obj2 + 10] = 1; ram[obj2 + 11] = 1;
		salsa64_wr32(ram + obj2 + 12, rdp + 32);
		salsa64_wr32(ram + rdp + 32, 0xE9000000);
		ram[tri] = 253; ram[tri + 1] = 254; ram[tri + 2] = 255;
		dl(&b, glob, obj); dl(&b, VTX_PA, tri);
		dl(&b, 0, obj2); dl(&b, 0, tri);
		dl(&b, 0, 0); dl(&b, 0, 0);
		return DL_PA;
	}
	if (b.fam == FAM_INDY) {
		/* Luces: tipo 1 (con posición) en 0xB10, sus posiciones en 0xB00 y 0xB08; la tabla de
		 * desplazamientos de 0x380 a vértices de 0x170; colores de entrada; 32 partículas. */
		for (int i = 0; i < 0x30; i += 4)
			salsa64_wr32(ram + INLIGHT_PA + i, rnd(&b) << 8 | 0x01);
		salsa64_wr32(ram + INLIGHT_PA + 0x10, 0x60C0F001u);
		salsa64_wr32(ram + INLIGHT_PA + 0x14, 0x00080000u | (rnd(&b) & 0x7FFF));
		for (int i = 0; i < 16; i++)
			ram[INTAB_PA + i] = (uint8_t)(8 * (i & 3));
		for (int i = 0; i < 32; i += 4)
			salsa64_wr32(ram + INCOL_PA + i, rnd(&b) << 8 | 0xFF);
		for (int i = 0; i < 32; i++) {
			uint8_t *p = ram + INPART_PA + 8 * i;
			salsa64_wr16(p, (uint16_t)(rnd(&b) & 0x7F) - 64);
			salsa64_wr16(p + 2, (uint16_t)(rnd(&b) & 0x7F) - 64);
			salsa64_wr16(p + 4, (uint16_t)(rnd(&b) & 0x3F));
			salsa64_wr16(p + 6, (uint16_t)((i & 3) << 8 | (i & 1)));
		}
	}
	if (b.fam == FAM_F5 || b.fam == FAM_INDY) {
		for (int i = 0; i < 8; i++) {
			for (int k = 0; k < 3; k++)
				salsa64_wr16(ram + F5VTX_PA + 8 * i + 2 * k, (uint16_t)pos[i][k]);
			salsa64_wr32(ram + F5COL_PA + 4 * i, rnd(&b) << 8 | 0xFF);
		}
		for (int i = 0; i < 48; i++)
			ram[F5HGT_PA + i] = (uint8_t)((rnd(&b) & 7) - 4);
		for (int i = 0; i < 48; i++)
			salsa64_wr32(ram + F5CLR_PA + 4 * i, rnd(&b) << 8 | 0xFF);
	}
	put_texels(&b, TEX_PA, 16 * 32 * 2);
	put_texels(&b, CI_PA, 16 * 16 / 2);
	put_texels(&b, TLUT_PA, 16 * 2);
	put_texels(&b, BG_IMG_PA, 64 * 32 * 2);
	static const char s2[] = "RSP Gfx ucode S2DEX  fifo 2.05  Salsa64 selftest";
	static const char flx[] = "RSP Gfx ucode F3DFLX.Rej fifo 2.03F Salsa64 selftest";
	memcpy(ram + S2_DATA_PA, s2, sizeof s2);
	memcpy(ram + FLX_DATA_PA, flx, sizeof flx);
	for (unsigned i = 0; i < 256; i++)
		ram[FLX_TAB_PA + i] = (uint8_t)i;
	uint8_t *bg = ram + BG_PA;
	salsa64_wr16(bg + 0, 0);            /* imageX (u10.5) */
	salsa64_wr16(bg + 2, 64 << 2);      /* imageW (u10.2) */
	salsa64_wr16(bg + 4, 24 << 2);      /* frameX (s10.2) */
	salsa64_wr16(bg + 6, 64 << 2);      /* frameW */
	salsa64_wr16(bg + 8, 0);            /* imageY */
	salsa64_wr16(bg + 10, 32 << 2);     /* imageH */
	salsa64_wr16(bg + 12, 160 << 2);    /* frameY */
	salsa64_wr16(bg + 14, 32 << 2);     /* frameH */
	salsa64_wr32(bg + 16, BG_IMG_PA);   /* imagePtr */
	bg[22] = 0;                         /* RGBA */
	bg[23] = 2;                         /* 16 bits */

	/* S2DEX es una tarea completa, no solo el tramo que F3DEX2 carga con
	 * G_LOAD_UCODE. Las dos codificaciones hacen una copia de fondo y cierran
	 * la lista por sus propios opcodes. */
	if (gbi == SALSA64_GBI_S2DEX || gbi == SALSA64_GBI_S2DEX1) {
		dl(&b, 0xFF100000u | 319, CIMG_PA);
		dl(&b, 0xED000000u, (320u << 2) << 12 | 240u << 2);
		dl(&b, gbi == SALSA64_GBI_S2DEX1 ? 0x02000000u : 0x0A000000u, BG_PA);
		dl(&b, 0xE9000000u, 0);  /* G_RDPFULLSYNC */
		dl(&b, gbi == SALSA64_GBI_S2DEX1 ? 0xB8000000u : 0xDF000000u, 0);
		return DL_PA;
	}

	/* Estado: color image, z image, tijeras, viewport y matrices. */
	if (b.fam == FAM_F5 || b.fam == FAM_INDY) {
		/* Cada lista empieza con la palabra del salto de software. */
		dl(&b, 0, 0);
	}
	dl(&b, 0xFF100000u | 319, CIMG_PA);
	dl(&b, 0xFE000000u, ZIMG_PA);
	dl(&b, 0xED000000u, (320u << 2) << 12 | 240u << 2);
	if (b.fam == FAM_F5) {
		/* El viewport (16 bytes) y la escala de las coordenadas de textura (palabras
		 * en +12 y +16) van en la lista, tras la primera palabra. */
		dl(&b, 0x03800000u, 0);
		for (int i = 0; i < 8; i += 4)
			dl(&b, (uint32_t)(uint16_t)vp[i] << 16 | (uint16_t)vp[i + 1], (uint32_t)(uint16_t)vp[i + 2] << 16 | (uint16_t)vp[i + 3]);
		dl(&b, 0x03820000u, 0);
		dl(&b, 0, 0x00000000u);
		dl(&b, 0x00200020u, 0);
		dl(&b, 0x02000000u, F5COL_PA);
	} else if (b.fam == FAM_INDY) {
		/* Todo es una DMA a la DMEM: el viewport (0x130), la matriz combinada (0x5C0, de donde sale
		 * la que se usa) y la de los vértices de la DMEM (0xE40). */
		dl(&b, 0x0101300Fu, VP_PA);
		dl(&b, 0x0105C03Fu, PROJ_PA);
		dl(&b, 0x010E403Fu, PROJ_PA);
		dl(&b, 0x0103800Fu, INTAB_PA);
		dl(&b, 0x01300000u | 0x5B8u << 8 | 3u, 1u << 24 | INLIGHT_PA);  /* una luz (y 4 bytes a 0x5B8) */
		dl(&b, 0x010B002Fu, INLIGHT_PA);
		dl(&b, 0x0104801Fu, INCOL_PA);
	} else {
		dl(&b, gen2(&b) ? 0xDC080008u : 0x03800010u, VP_PA);
	}
	if (b.fam != FAM_INDY) {
		dl(&b, gen2(&b) ? 0xDA380007u : 0x01030040u, PROJ_PA);
		dl(&b, gen2(&b) ? 0xDA380003u : 0x01020040u, MV_PA);
	}
	if (gen2(&b)) {
		/* También cubre el lote SIMD de vértices: color directo, niebla y
		 * cuatro vértices por G_VTX, sin luces ni TEXGEN. */
		dl(&b, 0xD9000000u, 0x00210004u | 0x1u);  /* G_SHADE | G_SHADING_SMOOTH | G_FOG | G_ZBUFFER */
		dl(&b, 0xDB080000u, 0x01000000u);          /* G_MW_FOG: factor 256, sesgo 0 */
	}
	else if (b.fam == FAM_INDY)
		dl(&b, 0xB7000000u, 0x00000005u);         /* G_ZBUFFER y G_SHADE */
	else
		dl(&b, 0xB7000000u, 0x00000205u);         /* lo mismo con los bits de F3D */

	/* Relleno: z image (un borrado de z) y un rectángulo de color. */
	cycle(&b, 3);
	dl(&b, 0xFF100000u | 319, ZIMG_PA);
	dl(&b, 0xF7000000u, 0xFFFCFFFCu);
	dl(&b, 0xF6000000u | (319u << 2) << 12 | 239u << 2, 0);
	dl(&b, 0xE7000000u, 0);
	dl(&b, 0xFF100000u | 319, CIMG_PA);
	dl(&b, 0xF7000000u, 0x00010001u * (rnd(&b) & 0xFFFE));
	dl(&b, 0xF6000000u | (200u << 2) << 12 | 60u << 2, (8u << 2) << 12 | 8u << 2);
	dl(&b, 0xE7000000u, 0);

	/* La tabla de colores de los triángulos de Indiana Jones (0xD40): una DMA. */
	if (b.fam == FAM_INDY)
		dl(&b, 0x010D401Fu, F5COL_PA);

	/* Dos triángulos con el sombreado, sin textura. */
	cycle(&b, 0);
	combine(&b, CC_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_A_ZERO, CC_A_ZERO, CC_A_ZERO, CC_SHADE);
	texture(&b, 0, 0);
	vtx(&b, VTX_PA, 0, 8);
	if (gbi == SALSA64_GBI_F3DZEX2) {
		/* G_BRANCH_W: v0 siempre queda por debajo del máximo s32 y salta a
		 * la primera instrucción que seguiría de todos modos. */
		dl(&b, 0xE1000000u, b.pc + 16);
		dl(&b, 0x04000000u, 0x7FFFFFFFu);
	}
	tri(&b, 0, 1, 2);
	tri(&b, 3, 4, 5);

	/* Texturas: RGBA16 16 × 32 en la tile 0 (LOADBLOCK) y CI4 16 × 16 en la
	 * tile 1 con su paleta (LOADTLUT). */
	dl(&b, 0xFD100000u, TEX_PA);                                   /* SETTIMG RGBA 16b */
	dl(&b, 0xF5100000u, 7u << 24);                                 /* SETTILE 7, tmem 0 */
	dl(&b, 0xE6000000u, 0);
	dl(&b, 0xF3000000u, 7u << 24 | 511u << 12 | 512u);             /* LOADBLOCK 512 texels, dxt 1/4 */
	dl(&b, 0xE7000000u, 0);
	dl(&b, 0xF5100000u | 4u << 9, 0u << 24 | 5u << 14 | 4u << 4);  /* tile 0: line 4, máscaras 32 × 16 */
	dl(&b, 0xF2000000u, 0u << 24 | (15u << 2) << 12 | 31u << 2);
	dl(&b, 0xFD500000u, CI_PA);                                    /* SETTIMG CI 16b (bloque) */
	dl(&b, 0xF5500000u | 0x80u, 7u << 24);
	dl(&b, 0xE6000000u, 0);
	dl(&b, 0xF3000000u, 7u << 24 | 63u << 12 | 2048u);
	dl(&b, 0xE7000000u, 0);
	dl(&b, 0xF5400000u | 1u << 9 | 0x80u, 1u << 24 | 4u << 14 | 4u << 4);  /* tile 1: CI4, line 1 */
	dl(&b, 0xF2000000u, 1u << 24 | (15u << 2) << 12 | 15u << 2);
	dl(&b, 0xFD100000u, TLUT_PA);
	dl(&b, 0xF5000000u | 0x100u, 7u << 24);
	dl(&b, 0xE6000000u, 0);
	dl(&b, 0xF0000000u, 7u << 24 | (15u << 2) << 12);              /* LOADTLUT 16 colores */
	dl(&b, 0xE7000000u, 0);

	/* Un ciclo: TEXEL0 · SHADE. */
	combine(&b, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_A_ZERO, CC_SHADE, CC_A_ZERO);
	texture(&b, 0, 1);
	tri(&b, 0, 1, 2);
	tri(&b, 6, 7, 3);

	/* Dos ciclos con dos texturas y la paleta: TEXEL1 · TEXEL0.a, y PRIM. */
	cycle(&b, 1);
	othermode_h(&b, 14, 2, 2u << 14);  /* G_TT_RGBA16 */
	dl(&b, 0xFA000000u, rnd(&b) | 0xFF);
	combine(&b, CC_T1, CC_ZERO, CC_C_T0A, CC_PRIM, CC_T1, CC_A_ZERO, CC_T0, CC_A_ZERO);
	tri(&b, 4, 5, 6);
	othermode_h(&b, 14, 2, 0);

	/* Rectángulos: uno en un ciclo y uno en copia. */
	cycle(&b, 0);
	combine(&b, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_A_ZERO, CC_SHADE, CC_A_ZERO);
	texrect(&b, 0, 200, 100, 216, 132, false);
	cycle(&b, 2);
	texrect(&b, 0, 240, 100, 255, 131, true);

	/* Segunda generación: un tramo S2DEX2 con un fondo (G_BG_COPY) y la
	 * vuelta al microcódigo de la tarea. */
	if (gen2(&b)) {
		dl(&b, 0xE1000000u, S2_DATA_PA);
		dl(&b, 0xDD000000u | (sizeof s2 - 1), S2_TEXT_PA);
		dl(&b, 0xE3000000u | (32 - 20 - 2) << 8 | 1, 2u << 20);  /* copia */
		dl(&b, 0x0A000000u, BG_PA);
		dl(&b, 0xE1000000u, 0);
		dl(&b, 0xDD000000u, GFX_SELFTEST_UCODE);
		cycle(&b, 0);
		combine(&b, CC_ZERO, CC_ZERO, CC_C_ZERO, CC_SHADE, CC_A_ZERO, CC_A_ZERO, CC_A_ZERO, CC_SHADE);
		texture(&b, 0, 0);
		dl(&b, 0x06000000u | 0 << 16 | 2 << 8 | 4, 2u << 16 | 4u << 8 | 6);  /* G_TRI2 */
		if (gbi == SALSA64_GBI_F3DEX2) {
			/* F-Zero X cambia de F3DEX2 a F3DFLX dentro de la lista y sube
			 * la tabla que usa para el alfa de sus vértices. */
			dl(&b, 0xE1000000u, FLX_DATA_PA);
			dl(&b, 0xDD000000u | (sizeof flx - 1), FLX_TEXT_PA);
			dl(&b, 0xD6000000u | (0x8B0u / 8) << 13 | 255u, FLX_TAB_PA);
		}
	}

	/* Rogue Squadron: las tres mallas generadas (5 × 5, 3 × 3 y el cuadrilátero) y un
	 * rectángulo de textura desde el vértice 1. */
	if (b.fam == FAM_F5) {
		cycle(&b, 0);
		combine(&b, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_A_ZERO, CC_SHADE, CC_A_ZERO);
		texture(&b, 0, 1);
		for (unsigned mode = 0; mode < 3; mode++) {
			if (mode < 2) {
				dl(&b, 0x05000000u | (mode ? 1u : 0u), 0);
				dl(&b, F5HGT_PA, F5CLR_PA);
				dl(&b, 0x00700070u, 0);
				dl(&b, 0, 0x00000020u);
			} else {
				dl(&b, 0x05000200u, 0x00100020u);
				dl(&b, 0x00300040u, rnd(&b) << 8 | 0xFF);
				dl(&b, rnd(&b) << 8 | 0xFF, rnd(&b) << 8 | 0xFF);
				dl(&b, rnd(&b) << 8 | 0xFF, 0x00000020u);
			}
			dl(&b, 0xFFD00000u, 48);
		}
		dl(&b, 0xBD000500u, 0xFF8040FFu);
		dl(&b, 0, 0x00200020u);
		dl(&b, 0x04000400u, 0);
	}

	/* Indiana Jones: la iluminación de las dos clases con una luz puntual, los vértices con la matriz
	 * de 0xE40 y los rectángulos de las partículas. */
	if (b.fam == FAM_INDY) {
		cycle(&b, 0);
		combine(&b, CC_T0, CC_ZERO, CC_SHADE, CC_ZERO, CC_T0, CC_A_ZERO, CC_SHADE, CC_A_ZERO);
		texture(&b, 0, 1);
		dl(&b, 0x01248000u | 0x1Fu, 7u << 24 | INCOL_PA);   /* iluminación 2 */
		dl(&b, 0x01648000u | 0x1Fu, 7u << 24 | INCOL_PA);   /* iluminación 1 */
		dl(&b, 0x05000000u | 0x00FFu, 0x00000018u);          /* los vértices 0 a 7 con la matriz de 0xE40 */
		tri(&b, 0, 1, 2);
		dl(&b, 0x05100000u | 0x8000u, 0x80000024u);           /* partículas: un bloque de 32 vértices */
		dl(&b, INPART_PA, 0xFFFFFFFFu);
		dl(&b, 0x00100010u, 0x00040004u);
		dl(&b, 0x01000100u, 0x00020002u);
		dl(&b, 0x0000000Fu, 0);
	}

	dl(&b, 0xE9000000u, 0);  /* G_RDPFULLSYNC */
	dl(&b, gen2(&b) ? 0xDF000000u : 0xB8000000u, 0);
	return DL_PA;
}

/* Dos flotantes casi iguales: los mismos bits, los dos casi cero (el SPE deja
 * a veces 1 en vez de 0: su división corrige el último bit con un entero) o
 * normales del mismo signo a lo más `ulps` pasos. */
static bool close_f(uint32_t x, uint32_t y, uint32_t ulps)
{
	if (x == y)
		return true;
	uint32_t ex = x >> 23 & 0xFF, ey = y >> 23 & 0xFF;
	if (!ex && !ey)
		return true;
	uint32_t d = x > y ? x - y : y - x;
	return ex && ey && ex != 0xFF && ey != 0xFF && (x >> 31) == (y >> 31) && d <= ulps;
}

/* Palabras [i, i + n) de a y b: flotantes (float_words) o exactas. */
static uint32_t words(const uint8_t *a, const uint8_t *b, uint32_t off, uint32_t n, bool floats, uint32_t ulps,
		      uint32_t *bad, uint32_t *first)
{
	for (uint32_t i = off; i + 4 <= off + n; i += 4) {
		uint32_t x, y;
		memcpy(&x, a + i, 4);
		memcpy(&y, b + i, 4);
		if (floats ? close_f(x, y, ulps) : x == y)
			continue;
		if (!(*bad)++)
			*first = i;
	}
	return *bad;
}

uint32_t gfx_selftest_compare_vtx(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t ulps, uint32_t *first)
{
	uint32_t bad = 0;
	*first = 0;
	for (uint32_t v = 0; v + sizeof(struct gfx_vertex) <= n; v += sizeof(struct gfx_vertex)) {
		words(a, b, v, offsetof(struct gfx_vertex, r), true, ulps, &bad, first);
		words(a, b, v + offsetof(struct gfx_vertex, r), sizeof(struct gfx_vertex) - offsetof(struct gfx_vertex, r),
		      false, ulps, &bad, first);
	}
	return bad;
}

uint32_t gfx_selftest_compare_cmd(const uint8_t *a, const uint8_t *b, uint32_t n, uint32_t ulps, uint32_t *first)
{
	uint32_t bad = 0, off = 0;
	*first = 0;
	while (off + sizeof(struct gfx_cmd) <= n) {
		struct gfx_cmd h;
		memcpy(&h, a + off, sizeof h);
		uint32_t size = h.size;
		if (size < sizeof h || size > n - off || (size & 15)) {
			/* No es un registro: lo que queda, exacto. */
			words(a, b, off, n - off, false, ulps, &bad, first);
			break;
		}
		if (h.op == GFX_OP_DRAW && size >= sizeof(struct gfx_draw)) {
			/* Flotantes: xf de las dos texturas y los términos k. */
			uint32_t t0 = offsetof(struct gfx_draw, tex), ts = sizeof(struct gfx_tex);
			uint32_t xf = offsetof(struct gfx_tex, xf);
			uint32_t at = off;
			for (int t = 0; t < 2; t++) {
				uint32_t f = off + t0 + (uint32_t)t * ts + xf;
				words(a, b, at, f - at, false, ulps, &bad, first);
				words(a, b, f, sizeof(float) * 4, true, ulps, &bad, first);
				at = f + sizeof(float) * 4;
			}
			uint32_t k = off + offsetof(struct gfx_draw, k);
			words(a, b, at, k - at, false, ulps, &bad, first);
			words(a, b, k, sizeof(((struct gfx_draw *)0)->k), true, ulps, &bad, first);
			at = k + sizeof(((struct gfx_draw *)0)->k);
			words(a, b, at, off + size - at, false, ulps, &bad, first);
		} else if (h.op == GFX_OP_VGROUP) {
			words(a, b, off, sizeof h, false, ulps, &bad, first);
			words(a, b, off + sizeof h, size - sizeof h, true, ulps, &bad, first);
		} else {
			words(a, b, off, size, false, ulps, &bad, first);
		}
		off += size;
	}
	return bad;
}
