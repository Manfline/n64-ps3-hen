/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — listas de audio de prueba para comparar dos ejecuciones de la
 * HLE (el SPE contra el PPE al arrancar en la PS3; el host en las pruebas
 * unitarias). Ver selftest.h.
 *
 * Cada lista pasa por casi todos los comandos de su ABI con entradas
 * seudoaleatorias y deja la DMEM entera y los estados en la RDRAM de
 * mentira; algunas direcciones de la RDRAM no están alineadas a 16, para
 * probar los trozos del DMA del SPE.
 */
#include <stdbool.h>
#include <string.h>

#include "core/endian.h"
#include "core/rcp.h"
#include "rsp/audio/selftest.h"

/* Mapa de la RDRAM de mentira. */
#define L_ALIST  0x0000u
#define L_PCM    0x0400u  /* 1024 muestras */
#define L_ADPCM  0x0C00u  /* cuadros ADPCM */
#define L_BOOK   0x0D00u  /* libro de códigos (16 × 8 coeficientes) */
#define L_STATE  0x1000u  /* estados de ADPCM, RESAMPLE, ENVMIXER, FILTER, POLEF */
#define L_LUT    0x1400u  /* tabla de FILTER */
#define L_OUT    0x2000u  /* la DMEM guardada y lo demás */
#define L_MP3    0x3400u  /* naudio: 8 bytes de ganancias y 0x480 de subbandas */
#define L_POLEF  0x3900u  /* naudio: coeficientes con los dos primeros en cero */

struct builder {
	uint8_t *ram;
	uint32_t n;      /* comandos */
	uint32_t seed;
};

static uint32_t rnd(struct builder *b)
{
	b->seed = b->seed * 1664525u + 1013904223u;
	return b->seed >> 8;
}

static void cmd(struct builder *b, uint32_t w1, uint32_t w2)
{
	salsa64_wr32(b->ram + L_ALIST + 8 * b->n, w1);
	salsa64_wr32(b->ram + L_ALIST + 8 * b->n + 4, w2);
	b->n++;
}

static void fill16(struct builder *b, uint32_t at, uint32_t n, int32_t range)
{
	for (uint32_t i = 0; i < n; i++)
		salsa64_wr16(b->ram + at + 2 * i, (uint16_t)(int16_t)((int32_t)(rnd(b) % (2 * range + 1)) - range));
}

static void inputs(struct builder *b)
{
	fill16(b, L_PCM, 1024, 30000);
	for (uint32_t i = 0; i < 0x100; i++)
		b->ram[L_ADPCM + i] = (uint8_t)rnd(b);
	fill16(b, L_BOOK, 128, 2500);
	fill16(b, L_STATE, 0x200, 3000);
	fill16(b, L_LUT, 8, 9000);
}

static void build_nead(struct builder *b)
{
	cmd(b, 0x0B000000u | 0x100, L_BOOK);                               /* LOADADPCM */
	cmd(b, 0x0F000000u, L_STATE);                                      /* SETLOOP */
	cmd(b, 0x14000000u | 0x90u << 12 | 0x000, L_ADPCM);                /* LOADBUFF */
	cmd(b, 0x08000000u | 0x000, 0x0400u << 16 | 0x100);                /* SETBUFF */
	cmd(b, 0x01000000u | 0x02u << 16, L_STATE + 0x40);                 /* ADPCM con bucle */
	cmd(b, 0x08000000u | 0x420, 0x0600u << 16 | 0x100);
	cmd(b, 0x05000000u | 0x5555, L_STATE + 0x80);                      /* RESAMPLE */
	cmd(b, 0x14000000u | 0x200u << 12 | 0x800, L_PCM);
	cmd(b, 0x14000000u | 0x200u << 12 | 0xA00, L_PCM + 0x408);         /* sin alinear a 16 */
	cmd(b, 0x02000000u | 0xC00, 0x200);                                /* CLEARBUFF */
	cmd(b, 0x02000000u | 0xE00, 0x100);
	cmd(b, 0x12000000u | 0x9Au << 16 | 0x0123, 0x0400u << 16 | 0xFF00); /* ENVSETUP1 */
	cmd(b, 0x16000000u, 0x7000u << 16 | 0x5000);                       /* ENVSETUP2 */
	cmd(b, 0x13000000u | 0x60u << 16 | 0x80u << 8 | 0x1D, 0x80u << 24 | 0xA0u << 16 | 0xC0u << 8 | 0xE0); /* ENVMIXER */
	cmd(b, 0x0C000000u | 0x100u << 12 | 0x6000, 0x0800u << 16 | 0x0A00); /* MIXER */
	cmd(b, 0x04000000u | 0x100u << 12, 0x0C00u << 16 | 0x0E00);         /* ADDMIXER */
	cmd(b, 0x0E000000u | 0x28u << 16 | 0x100, 0x0E00u << 16);           /* HILOGAIN */
	cmd(b, 0x07000000u | 0x02u << 16 | 0x100, L_LUT);                   /* FILTER: cuenta y tabla */
	cmd(b, 0x07000000u | 0x800, L_STATE + 0x100);                       /* FILTER */
	cmd(b, 0x0A000000u | 0x800, 0x0300u << 16 | 0x3E);                  /* DMEMMOVE */
	cmd(b, 0x09000000u | 0x02u << 16 | 0x300, 0x0100u << 16);           /* DUPLICATE */
	cmd(b, 0x11000000u | 0x40, 0x0A00u << 16 | 0x0380);                 /* INTERL */
	cmd(b, 0x10000000u | 0x02u << 16 | 0x800, 0x0200u << 16 | 0x40);    /* copia por bloques */
	cmd(b, 0x0D000000u | 0x100u << 12 | 0x000, 0x0800u << 16 | 0x0A00); /* INTERLEAVE */
	cmd(b, 0x15000000u | 0x400u << 12 | 0x000, L_OUT);                  /* SAVEBUFF: la DMEM */
	cmd(b, 0x15000000u | 0x400u << 12 | 0x400, L_OUT + 0x408);
	cmd(b, 0x15000000u | 0x400u << 12 | 0x800, L_OUT + 0x810);
	cmd(b, 0x15000000u | 0x400u << 12 | 0xC00, L_OUT + 0xC18);
	cmd(b, 0x08000000u | 0x800, 0x0100u << 16 | 0x80);
	cmd(b, 0x06000000u | 0x6000, 0x1234);                               /* RESAMPLE_ZOH */
	cmd(b, 0x15000000u | 0x80u << 12 | 0x100, L_OUT + 0x1028);
	cmd(b, 0x00000000u, 0);                                             /* sin implementar */
}

static void build_abi1(struct builder *b)
{
	cmd(b, 0x07000000u, 0x03u << 24 | L_PCM);                           /* SEGMENT 3 */
	cmd(b, 0x07000000u, 0x04u << 24 | L_STATE);                         /* SEGMENT 4 */
	cmd(b, 0x0B000000u | 0x100, L_BOOK);                                /* LOADADPCM */
	cmd(b, 0x0F000000u, 0x04u << 24);                                   /* SETLOOP */
	cmd(b, 0x08000000u | 0x000, 0x0080u << 16 | 0x90);
	cmd(b, 0x04000000u, L_ADPCM);                                       /* LOADBUFF (segmento 0) */
	cmd(b, 0x08000000u | 0x000, 0x0100u << 16 | 0x100);
	cmd(b, 0x01000000u | 0x01u << 16, 0x04u << 24 | 0x40);              /* ADPCM A_INIT */
	cmd(b, 0x08000000u | 0x120, 0x0300u << 16 | 0x100);
	cmd(b, 0x05000000u | 0x01u << 16 | 0x6789, 0x04u << 24 | 0x80);     /* RESAMPLE A_INIT */
	cmd(b, 0x08000000u | 0x500, 0x0700u << 16 | 0x200);
	cmd(b, 0x04000000u, 0x03u << 24 | 0x208);                           /* LOADBUFF sin alinear */
	cmd(b, 0x02000000u | 0x700, 0x200);                                 /* CLEARBUFF */
	cmd(b, 0x02000000u | 0x900, 0x200);
	cmd(b, 0x08000000u | 0x08u << 16 | 0x900, 0x0300u << 16 | 0x500);   /* SETBUFF A_AUX */
	cmd(b, 0x09000000u | 0x08u << 16 | 0x6000, 0x2000);                 /* SETVOL A_AUX */
	cmd(b, 0x09000000u | 0x06u << 16 | 0x1000, 0);                      /* volumen izquierdo */
	cmd(b, 0x09000000u | 0x02u << 16 | 0x7000, 0x00012000);             /* destino izquierdo */
	cmd(b, 0x09000000u | 0x04u << 16 | 0x7000, 0);
	cmd(b, 0x09000000u | 0x00u << 16 | 0x0800, 0x0000E000);
	cmd(b, 0x08000000u | 0x300, 0x0700u << 16 | 0x100);
	cmd(b, 0x03000000u | 0x09u << 16, 0x04u << 24 | 0xC0);              /* ENVMIXER A_INIT|A_AUX */
	cmd(b, 0x03000000u | 0x08u << 16, 0x04u << 24 | 0xC0);              /* y sigue del estado */
	cmd(b, 0x08000000u | 0x700, 0x0100u << 16 | 0x100);
	cmd(b, 0x0C000000u | 0x5000, 0x0700u << 16 | 0x0900);               /* MIXER */
	cmd(b, 0x0E000000u | 0x01u << 16 | 0x3000, 0x04u << 24 | 0x110);    /* POLEF A_INIT */
	cmd(b, 0x0A000000u | 0x700, 0x0000u << 16 | 0x80);                  /* DMEMMOVE */
	cmd(b, 0x08000000u | 0x000, 0x0B00u << 16 | 0x100);
	cmd(b, 0x0D000000u, 0x0700u << 16 | 0x0900);                        /* INTERLEAVE */
	cmd(b, 0x08000000u | 0x000, 0x0000u << 16 | 0x400);
	cmd(b, 0x06000000u, L_OUT);                                         /* SAVEBUFF */
	cmd(b, 0x08000000u | 0x000, 0x0400u << 16 | 0x400);
	cmd(b, 0x06000000u, L_OUT + 0x408);
	cmd(b, 0x08000000u | 0x000, 0x0800u << 16 | 0x400);
	cmd(b, 0x06000000u, L_OUT + 0x818);
}

/* naudio: direcciones de la DMEM relativas a 0x4F0; los de MP3 cambian los
 * comandos 0, 7, 8 y 0x0E. */
static void build_naudio(struct builder *b, bool mp3)
{
	salsa64_wr32(b->ram + L_MP3, 0x00020000u);
	salsa64_wr32(b->ram + L_MP3 + 4, 0xFFFE8000u);
	fill16(b, L_MP3 + 8, 0x240, 2000);
	salsa64_wr32(b->ram + L_POLEF, 0);
	fill16(b, L_POLEF + 4, 14, 4000);

	cmd(b, 0x0B000000u | 0x100, L_BOOK);                                /* LOADADPCM */
	cmd(b, 0x0F000000u, L_STATE);                                       /* SETLOOP */
	cmd(b, 0x04000000u | 0x90u << 12 | 0x004, L_ADPCM);                 /* LOADBUFF → 0x4F4 */
	cmd(b, 0x01000000u | (L_STATE + 0x40), 0x1u << 28 | 0x100u << 16 | 0x4u << 12 | 0x200); /* ADPCM A_INIT */
	cmd(b, 0x01000000u | (L_STATE + 0x60), 0x2u << 28 | 0x60u << 16 | 0x4u << 12 | 0x400);  /* ADPCM con bucle */
	cmd(b, 0x05000000u | (L_STATE + 0x80), 0x1u << 30 | 0x5555u << 14 | 0x208u << 2);      /* RESAMPLE A_INIT → MAIN */
	cmd(b, 0x05000000u | (L_STATE + 0x90), 0x9999u << 14 | 0x210u << 2 | 1);               /* RESAMPLE → MAIN2 */
	cmd(b, 0x04000000u | 0x5C0u << 12 | 0x4E0, L_PCM + 0x10);           /* secos y efecto */
	cmd(b, 0x02000000u | 0x7C0, 0x40);                                  /* CLEARBUFF */
	cmd(b, 0x09000000u | 0x06u << 16 | 0x6000, 0x5000u << 16 | 0x2800); /* SETVOL: vol. izq., seco, efecto */
	cmd(b, 0x09000000u | 0x04u << 16 | 0x7000, 0x00001234);             /* destino y velocidad der. */
	cmd(b, 0x09000000u | 0x00u << 16 | 0x1000, 0xFFFF8000u);            /* destino y velocidad izq. */
	if (!mp3)
		cmd(b, 0x0E000000u, 0x0000ABCD);                            /* parte baja de la velocidad der. */
	cmd(b, 0x03000000u | 0x01u << 16 | 0x5000, L_STATE + 0x100);        /* ENVMIXER A_INIT */
	cmd(b, 0x03000000u | 0x5800, L_STATE + 0x100);                      /* y sigue del estado */
	cmd(b, 0x0C000000u | 0x6000, 0x170u << 16 | 0x4E0);                 /* MIXER MAIN2 → seco izq. */
	cmd(b, 0x0A000000u | 0x010, 0x0300u << 16 | 0x3E);                  /* DMEMMOVE */
	if (mp3) {
		cmd(b, 0x0E000000u | 0x01u << 16 | 0x1234, L_STATE + 0x180); /* filtro IIR sobre MAIN */
		cmd(b, 0x0B000000u | 0x20, L_POLEF);
		cmd(b, 0x0E000000u | 0x01u << 16 | 0x3000, 0x01u << 24 | (L_STATE + 0x1A0)); /* POLEF sobre MAIN2 */
		cmd(b, 0x00000000u | 0x020, 0x9000u << 16 | 0x0003);        /* OVERLOAD */
		cmd(b, 0x07000000u | 0x0A, L_MP3);                          /* MP3 */
		cmd(b, 0x08000000u, 0);                                     /* MP3ADDY */
		cmd(b, 0x07000000u | 0x06, L_MP3);                          /* y otra vez sobre su salida */
	}
	cmd(b, 0x0D000000u, 0);                                             /* INTERLEAVE */
	cmd(b, 0x06000000u | 0x400u << 12 | 0xB10, L_OUT);                  /* SAVEBUFF: la DMEM desde 0 */
	cmd(b, 0x06000000u | 0x400u << 12 | 0xF10, L_OUT + 0x408);
	cmd(b, 0x06000000u | 0x400u << 12 | 0x310, L_OUT + 0x810);
	cmd(b, 0x06000000u | 0x400u << 12 | 0x710, L_OUT + 0xC18);
}


/* MusyX: dos subtramas (SFD) con voces PCM16 y ADPCM, volúmenes base,
 * eco con dos tomas y, en v2, el intercalado final. Su propio mapa de la
 * RDRAM de mentira; direcciones sin alinear a 16 para los trozos del DMA
 * del SPE. `data_size` de la tarea es el número de subtramas, no bytes. */
#define M_SFD     0x0000u  /* 2 SFD de 0xA10 (v1) o de 0xA38 (v2) bytes */
#define M_STATE   0x1480u  /* últimas muestras, volúmenes base, cc0, últimas 4 del eco */
#define M_SFX     0x1760u  /* parámetros del eco (0x48 bytes) */
#define M_ECHO    0x17B2u  /* búfer circular de 3 subtramas (576 muestras) */
#define M_BOOK    0x1C40u  /* libro de ADPCM: 8 predictores × 16 coeficientes */
#define M_ADPCM   0x1D43u  /* 2 fuentes de 20 bytes por cuadro */
#define M_PCM     0x1E46u  /* muestras PCM16 */
#define M_OUT     0x2100u  /* salida de cada subtrama: 0x500 de por medio (v2 escribe 0x480) */
#define M_MIX     0x3200u  /* v2: 2 fuentes de 3 subtramas */
#define M_MIX_TBL 0x3B00u  /* v2: tabla de 8 entradas (dirección, ganancia) */
#define M_SUB     0x3B20u  /* v2: la subtrama de ptr_1c */

static void put16(struct builder *b, uint32_t at, uint32_t v)
{
	salsa64_wr16(b->ram + at, (uint16_t)v);
}

static void put32(struct builder *b, uint32_t at, uint32_t v)
{
	salsa64_wr32(b->ram + at, v);
}

/* Una voz de 0x50 bytes en `at`. */
static void musyx_voice(struct builder *b, uint32_t at, bool adpcm, uint32_t env, uint32_t step, uint32_t out_ptr)
{
	for (unsigned k = 0; k < 4; k++) {
		put32(b, at + 4 * k, (env + 0x00030000u * k) & 0xFFFF0000u);
		put32(b, at + 0x10 + 4 * k, (k == 2 ? 0 : step + 0x155u * k) & 0xFFFFFFFFu);
	}
	put16(b, at + 0x20, rnd(b));                    /* pitch Q16 inicial */
	put16(b, at + 0x22, 0x0C00 + (rnd(b) & 0x3FF)); /* pitch Q4.12: ~0,75–1,0 */
	if (adpcm) {
		put32(b, at + 0x24, M_ADPCM);               /* tramo 0: 3 cuadros de 20 bytes */
		put32(b, at + 0x28, 0);
		put16(b, at + 0x2C, 3 * 20);
		put16(b, at + 0x2E, 0);
		put32(b, at + 0x30, M_ADPCM + 60);          /* tramo 1: 2 cuadros */
		put32(b, at + 0x34, 0);
		put16(b, at + 0x38, 2 * 20);
		put16(b, at + 0x3A, 0);
		b->ram[at + 0x3C] = 3;                      /* cuadros del tramo 0 */
		b->ram[at + 0x3D] = 2;
		b->ram[at + 0x3E] = 5;                      /* muestras que se saltan */
		b->ram[at + 0x3F] = 0x24;                   /* ≥ 32: salto de brecha */
		put32(b, at + 0x40, M_BOOK);
	} else {
		put32(b, at + 0x24, M_PCM);
		put32(b, at + 0x28, M_PCM + 100);           /* segundo trozo del mismo tramo */
		put16(b, at + 0x2C, 2 * 60);
		put16(b, at + 0x2E, 2 * 40);
		put32(b, at + 0x30, M_PCM + 200);
		put32(b, at + 0x34, 0);
		put16(b, at + 0x38, 2 * 50);
		put16(b, at + 0x3A, 0);
		b->ram[at + 0x3C] = 0;                      /* PCM16 */
		b->ram[at + 0x3D] = 0;
		b->ram[at + 0x3E] = 3;
		b->ram[at + 0x3F] = 0;
		put16(b, at + 0x40, 100);                   /* muestras del tramo 0 */
		put16(b, at + 0x42, 50);                    /* y del 1 */
	}
	put32(b, at + 0x44, out_ptr);
	put16(b, at + 0x48, adpcm ? 96 : 100);          /* fin del bucle: las muestras cargadas */
	put16(b, at + 0x4A, adpcm ? 0x8000 : 0x8004);   /* reinicio (0x8000: absoluto) */
	put16(b, at + 0x4C, 0x1234);
	put16(b, at + 0x4E, 0);
}

static void build_musyx(struct builder *b, bool v2)
{
	const uint32_t hdr = v2 ? 0x28u : 0x10u, stride = hdr + 32 * 0x50;

	fill16(b, M_PCM, 300, 12000);
	for (uint32_t i = 0; i < 100; i++)
		b->ram[M_ADPCM + i] = (uint8_t)rnd(b);
	/* El primer byte de nibbles de cada cuadro lleva predictor y escala. */
	fill16(b, M_BOOK, 128, 2500);
	fill16(b, M_ECHO, 576, 3000);
	fill16(b, M_STATE, 4 * 32, 400);                    /* últimas muestras de 32 voces */
	for (uint32_t i = 0; i < 8; i++)
		put16(b, M_STATE + 0x100 + 2 * i, rnd(b) & 0x3F);  /* volúmenes base */
	fill16(b, M_STATE + 0x110, 192, 200);               /* cc0 */
	fill16(b, M_STATE + (v2 ? 0x110 : 0x290), 4, 300);  /* últimas 4 del eco (v2 comparte con cc0) */

	/* Eco: dirección y largo del búfer, 2 tomas, filtro FIR. */
	put32(b, M_SFX + 0x00, M_ECHO);
	put32(b, M_SFX + 0x04, 576);
	put16(b, M_SFX + 0x08, 2);
	put16(b, M_SFX + 0x0A, 24000);
	put32(b, M_SFX + 0x0C, 200);
	put32(b, M_SFX + 0x10, 421);
	put16(b, M_SFX + 0x2C, 20000);
	put16(b, M_SFX + 0x2E, 15000);
	put16(b, M_SFX + 0x3C, 0x6000);
	put16(b, M_SFX + 0x3E, 0x3000);
	put16(b, M_SFX + 0x40, 9000);
	put16(b, M_SFX + 0x42, 16000);
	put16(b, M_SFX + 0x44, 4000);
	put16(b, M_SFX + 0x46, 600);

	for (uint32_t k = 0; k < 2; k++) {
		uint32_t at = M_SFD + k * stride;
		put16(b, at + 0x0, 3);                          /* voces */
		put16(b, at + 0x2, k + 1);                      /* índice del eco */
		put32(b, at + 0x4, k ? 0x00000003u : 0);        /* voces activas */
		put32(b, at + 0x8, M_STATE);
		put32(b, at + 0xC, k ? M_SFX : 0);              /* la primera sin eco */
		if (v2) {
			put32(b, at + 0x10, 0);
			b->ram[at + 0x14] = 0;
			b->ram[at + 0x15] = k ? 0x05 : 0;        /* fuentes extra de volumen base */
			put16(b, at + 0x16, k ? 0x0003 : 0);     /* intercalado final */
			put32(b, at + 0x18, M_MIX_TBL);
			put32(b, at + 0x1C, M_SUB);
			put32(b, at + 0x20, M_OUT + 0xA00 + 0x300 * k);
			put32(b, at + 0x24, M_STATE + 0x40);
		}
		musyx_voice(b, at + hdr, false, 0x00200000u + 0x00010000u * k, 0x0000A000u, 0);
		musyx_voice(b, at + hdr + 0x50, true, 0x00180000u, k ? 0xFFFFC000u : 0x00008000u, 0);
		musyx_voice(b, at + hdr + 0xA0, false, 0x00100000u, 0, M_OUT + 0x500 * k);
	}
	if (v2) {
		fill16(b, M_MIX, 2 * 576, 5000);
		put32(b, M_MIX_TBL + 0, M_MIX);
		put16(b, M_MIX_TBL + 4, 14000);
		put32(b, M_MIX_TBL + 8, M_MIX + 576 * 2);
		put16(b, M_MIX_TBL + 12, 9000);
		fill16(b, M_SUB, 192, 1000);
	}
	/* Los libros: el nibble de cada cuadro (escala y predictor) sale de los datos. */
	b->n = 2;
}

uint32_t aud_selftest_build(uint8_t *ram, uint32_t abi, uint32_t seed)
{
	struct builder b = { ram, 0, seed };
	memset(ram, 0, AUD_SELFTEST_RAM);
	if (abi == SALSA64_ABI_MUSYX1 || abi == SALSA64_ABI_MUSYX1_RS || abi == SALSA64_ABI_MUSYX2) {
		build_musyx(&b, abi == SALSA64_ABI_MUSYX2);
		return b.n;  /* subtramas, no bytes */
	}
	inputs(&b);
	if (abi == SALSA64_ABI_AUDIO || abi == SALSA64_ABI_AUDIO_GE)
		build_abi1(&b);
	else if (abi >= SALSA64_ABI_NAUDIO && abi <= SALSA64_ABI_NAUDIO_CBFD)
		build_naudio(&b, abi == SALSA64_ABI_NAUDIO_MP3 || abi == SALSA64_ABI_NAUDIO_CBFD);
	else
		build_nead(&b);
	return 8 * b.n;
}
