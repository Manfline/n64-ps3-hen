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

uint32_t aud_selftest_build(uint8_t *ram, uint32_t abi, uint32_t seed)
{
	struct builder b = { ram, 0, seed };
	memset(ram, 0, AUD_SELFTEST_RAM);
	inputs(&b);
	if (abi == SALSA64_ABI_AUDIO || abi == SALSA64_ABI_AUDIO_GE)
		build_abi1(&b);
	else if (abi >= SALSA64_ABI_NAUDIO && abi <= SALSA64_ABI_NAUDIO_CBFD)
		build_naudio(&b, abi == SALSA64_ABI_NAUDIO_MP3 || abi == SALSA64_ABI_NAUDIO_CBFD);
	else
		build_nead(&b);
	return 8 * b.n;
}
