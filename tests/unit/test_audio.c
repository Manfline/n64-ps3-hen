/*
 * Salsa64 — pruebas de la HLE de audio (src/rsp/audio): reconocimiento del
 * ABI, comandos de nead y ABI1 con listas sintéticas y resultados
 * calculados a mano, partes de la RDRAM escritas y la tarea completa por
 * el núcleo (rsp.c) con el backend síncrono.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "rsp/audio/backend.h"
#include "rsp/audio/hle.h"
#include "rsp/audio/selftest.h"

#define ALIST  0x100000u  /* la lista de comandos */
#define SRC    0x200000u
#define SRC2   0x200100u
#define DST    0x200200u
#define STATE  0x200400u
#define BOOK   0x200600u

static struct aud_state st;
static struct aud_result res;
static uint32_t nlist;

static uint8_t *ram(uint32_t pa)
{
	return t_n64.bus.rdram + pa;
}

static void put16(uint32_t pa, int16_t v)
{
	salsa64_wr16(ram(pa), (uint16_t)v);
}

static int16_t get16(uint32_t pa)
{
	return (int16_t)salsa64_rd16(ram(pa));
}

static void cmd(uint32_t w1, uint32_t w2)
{
	t_wr32(ALIST + 8 * nlist, w1);
	t_wr32(ALIST + 8 * nlist + 4, w2);
	nlist++;
}

static void run_abi(enum salsa64_abi abi)
{
	struct aud_job job;
	memset(&job, 0, sizeof job);
	job.rdram_ea = (uint64_t)(uintptr_t)t_n64.bus.rdram;
	job.rdram_size = t_n64.bus.rdram_size;
	job.abi = abi;
	job.alist = ALIST;
	job.alist_size = 8 * nlist;
	aud_run(&st, &job, &res);
	nlist = 0;
}

static void fresh(void)
{
	aud_init(&st);
	nlist = 0;
}

/* ---- Codificación de nead (Zelda OoT) -------------------------------------- */

static void n_loadbuff(uint16_t dmem, uint32_t addr, uint16_t count)
{
	cmd(0x14000000u | (uint32_t)count << 12 | dmem, addr);
}

static void n_savebuff(uint16_t dmem, uint32_t addr, uint16_t count)
{
	cmd(0x15000000u | (uint32_t)count << 12 | dmem, addr);
}

static void n_setbuff(uint16_t in, uint16_t out, uint16_t count)
{
	cmd(0x08000000u | in, (uint32_t)out << 16 | count);
}

/* ---- Reconocimiento del ABI ------------------------------------------------- */

static enum salsa64_abi detect_with(uint32_t w0, uint32_t w10, uint32_t w28, uint32_t w30)
{
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	task.type = SALSA64_TASK_AUDIO;
	task.ucode_data = 0x80300000u;
	memset(ram(0x300000), 0, 0x40);
	t_wr32(0x300000, w0);
	t_wr32(0x300010, w10);
	t_wr32(0x300028, w28);
	t_wr32(0x300030, w30);
	return salsa64_rsp_detect_abi(&t_n64.bus, &task);
}

static void detect_abi(void)
{
	CHECK_EQ(detect_with(1, 0, 0x1E24138C, 0xF0000F00), SALSA64_ABI_AUDIO);
	CHECK_EQ(detect_with(1, 0, 0x1DC8138C, 0xF0000F00), SALSA64_ABI_AUDIO_GE);
	CHECK_EQ(detect_with(1, 0, 0x12345678, 0xF0000F00), SALSA64_ABI_UNKNOWN);
	CHECK_EQ(detect_with(1, 0x1F681230, 0, 0), SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(detect_with(1, 0x11181350, 0, 0), SALSA64_ABI_NEAD_MK);
	CHECK_EQ(detect_with(1, 0x110412CC, 0, 0), SALSA64_ABI_NEAD_SF);
	CHECK_EQ(detect_with(1, 0x111812E0, 0, 0), SALSA64_ABI_NEAD_SFJ);
	CHECK_EQ(detect_with(0, 0x1AE8143C, 0, 0), SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(detect_with(0, 0x0000127C, 0, 0), SALSA64_ABI_NAUDIO);
	CHECK_EQ(detect_with(0, 0x00000001, 0, 0), SALSA64_ABI_MUSYX1);
	CHECK_EQ(detect_with(0, 0, 0, 0), SALSA64_ABI_UNKNOWN);
	/* Las ROMs de prueba de Salsa64. */
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	task.ucode_data = 0x300000;
	memset(ram(0x300000), 0, 0x40);
	memcpy(ram(0x300000), "Salsa64 ABI NEAD", 16);
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_NEAD_OOT);
	memcpy(ram(0x300000), "Salsa64 ABI1\0\0\0\0", 16);
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_AUDIO);
	/* Fuera de la RDRAM. */
	task.ucode_data = 0x7FFFF0;
	CHECK_EQ(salsa64_rsp_detect_abi(&t_n64.bus, &task), SALSA64_ABI_UNKNOWN);
}

/* ---- nead -------------------------------------------------------------------- */

static void nead_load_mix_save(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, (int16_t)(i * 1000 - 8000));
		put16(SRC2 + 2 * i, 30000);
	}
	n_loadbuff(0x400, SRC, 32);
	n_loadbuff(0x500, SRC2, 32);
	cmd(0x0C000000u | 32u << 12 | 0x4000, 0x0400u << 16 | 0x500);  /* MIXER ×0,5 */
	n_savebuff(0x500, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.commands, 4);
	CHECK_EQ(res.errors, 0);
	for (int i = 0; i < 16; i++) {
		int32_t s = i * 1000 - 8000, want = 30000 + ((s * 0x4000) >> 15);
		CHECK_EQ(get16(DST + 2 * i), want > 32767 ? 32767 : want);
	}
	CHECK_EQ(res.nranges, 1);
	CHECK_EQ(res.range_addr[0], DST);
	CHECK_EQ(res.range_len[0], 32);
}

static void nead_buffer_moves(void)
{
	fresh();
	for (int i = 0; i < 64; i++)
		put16(SRC + 2 * i, (int16_t)(i + 1));
	n_loadbuff(0x100, SRC, 128);
	cmd(0x02000000u | 0x300, 64);                           /* CLEARBUFF */
	cmd(0x0A000000u | 0x100, 0x0300u << 16 | 6);            /* DMEMMOVE: 6 → 8 bytes */
	cmd(0x09000000u | 2u << 16 | 0x100, 0x0400u << 16);     /* DUPLICATE 128 B × 2 */
	cmd(0x11000000u | 4, 0x0100u << 16 | 0x600);            /* INTERL: 4 muestras pares */
	cmd(0x04000000u | 16u << 12, 0x0100u << 16 | 0x0400);   /* ADDMIXER 16 bytes */
	cmd(0x0E000000u | 0x20u << 16 | 8, 0x0100u << 16);      /* HILOGAIN ×2 (Q4.4), 8 bytes */
	n_savebuff(0x300, DST, 16);
	n_savebuff(0x400, DST + 16, 16);
	n_savebuff(0x600, DST + 32, 8);
	n_savebuff(0x100, DST + 48, 8);
	n_savebuff(0x480, DST + 64, 8);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* DMEMMOVE copió 8 bytes (4 muestras) y el resto quedó en cero. */
	CHECK_EQ(get16(DST + 0), 1);
	CHECK_EQ(get16(DST + 6), 4);
	CHECK_EQ(get16(DST + 8), 0);
	/* ADDMIXER: 0x400 = duplicado de 0x100 más 0x100 → 2 × (i + 1). */
	CHECK_EQ(get16(DST + 16), 2);
	CHECK_EQ(get16(DST + 30), 16);
	/* INTERL: muestras 0, 2, 4, 6 → 1, 3, 5, 7. */
	CHECK_EQ(get16(DST + 32), 1);
	CHECK_EQ(get16(DST + 34), 3);
	CHECK_EQ(get16(DST + 38), 7);
	/* HILOGAIN ×2 sobre las 4 primeras de 0x100. */
	CHECK_EQ(get16(DST + 48), 2);
	CHECK_EQ(get16(DST + 54), 8);
	/* La segunda copia de DUPLICATE (0x480) no pasó por ADDMIXER. */
	CHECK_EQ(get16(DST + 64), 1);
	CHECK_EQ(get16(DST + 70), 4);
	/* Cinco SAVEBUFF: los tres primeros, contiguos, se juntan. */
	CHECK_EQ(res.nranges, 3);
	CHECK_EQ(res.range_addr[0], DST);
	CHECK_EQ(res.range_len[0], 40);
}

static void nead_interleave(void)
{
	fresh();
	for (int i = 0; i < 8; i++) {
		put16(SRC + 2 * i, (int16_t)(100 + i));
		put16(SRC2 + 2 * i, (int16_t)(-100 - i));
	}
	n_loadbuff(0x100, SRC, 16);
	n_loadbuff(0x200, SRC2, 16);
	cmd(0x0D000000u | 16u << 12 | 0x300, 0x0100u << 16 | 0x200);
	n_savebuff(0x300, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	for (int i = 0; i < 8; i++) {
		CHECK_EQ(get16(DST + 4 * i), 100 + i);
		CHECK_EQ(get16(DST + 4 * i + 2), -100 - i);
	}
}

static void nead_envmixer(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, 10000);
		put16(SRC2 + 2 * i, 100);
	}
	n_loadbuff(0x100, SRC, 32);
	for (uint16_t b = 0x200; b <= 0x500; b += 0x100)
		n_loadbuff(b, SRC2, 32);
	/* env_values[2] = 0x8000, paso 0; pasos 0 y 0x1000 para izquierda y derecha. */
	cmd(0x12000000u | 0x80u << 16, 0x0000u << 16 | 0x1000);
	cmd(0x16000000u, 0x8000u << 16 | 0x4000);               /* izquierda 0,5; derecha 0,25 */
	/* ENVMIXER: entrada 0x100, 16 muestras; secas 0x200/0x300, efecto 0x400/0x500. */
	cmd(0x13000000u | 0x10u << 16 | 16u << 8, 0x20u << 24 | 0x30u << 16 | 0x40u << 8 | 0x50);
	n_savebuff(0x200, DST, 32);
	n_savebuff(0x300, DST + 32, 32);
	n_savebuff(0x400, DST + 64, 32);
	n_savebuff(0x500, DST + 96, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Primeras 8: l = 10000·0x8000 >> 16 = 5000, r = 2500; efecto ×0,5. */
	CHECK_EQ(get16(DST + 0), 5100);
	CHECK_EQ(get16(DST + 32), 2600);
	CHECK_EQ(get16(DST + 64), 2600);
	CHECK_EQ(get16(DST + 96), 1350);
	/* Siguientes 8: la derecha sube 0x1000 → 0x5000: r = 3125, r2 = 1562. */
	CHECK_EQ(get16(DST + 16), 5100);
	CHECK_EQ(get16(DST + 48), 3225);
	CHECK_EQ(get16(DST + 112), 1662);
	CHECK_EQ(st.nead.env_values[1], 0x6000);

	/* Bits 0 y 1: inversión de las secas (XOR con −1). */
	n_loadbuff(0x200, SRC2, 32);
	cmd(0x16000000u, 0x8000u << 16 | 0x4000);
	cmd(0x13000000u | 0x10u << 16 | 8u << 8 | 0x3, 0x20u << 24 | 0x30u << 16 | 0x40u << 8 | 0x50);
	n_savebuff(0x200, DST, 16);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(get16(DST + 0), 100 + (5000 ^ -1));
}

static void nead_resample(void)
{
	fresh();
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, 1000);
	n_loadbuff(0x108, SRC, 32);
	n_setbuff(0x108, 0x200, 32);
	cmd(0x05000000u | 0x01u << 16 | 0x8000, STATE);  /* pitch 1,0, A_INIT */
	n_savebuff(0x200, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Con pitch 1,0 la fase queda en 0: la salida k mira las entradas
	 * k−4 … k−1 (cero antes del búfer) con la fila 0 de la tabla. */
	int32_t c0 = aud_resample_lut[0], c1 = aud_resample_lut[1];
	CHECK_EQ(get16(DST + 0), 0);
	CHECK_EQ(get16(DST + 4), (1000 * c0) >> 15);
	CHECK_EQ(get16(DST + 6), (1000 * (c0 + c1)) >> 15);
	CHECK_EQ(get16(DST + 8), 1000);
	CHECK_EQ(get16(DST + 30), 1000);
	/* Estado: las 4 últimas entradas y el acumulador. */
	CHECK_EQ(get16(STATE + 0), 1000);
	CHECK_EQ(get16(STATE + 6), 1000);
	CHECK_EQ(get16(STATE + 8), 0);

	/* Continuación a la mitad del tono: una entrada constante sale igual
	 * en todas las fases (cada fila suma 32768). */
	n_loadbuff(0x108, SRC, 32);
	n_setbuff(0x108, 0x200, 32);
	cmd(0x05000000u | 0x4000, STATE);
	n_savebuff(0x200, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	for (int i = 0; i < 16; i++)
		CHECK_EQ(get16(DST + 2 * i), 1000);
	CHECK_EQ(get16(STATE + 8), 0);  /* 16 × 0,5 = 8 muestras enteras */
}

static void lut_rows_sum_to_one(void)
{
	for (int p = 0; p < 64; p++) {
		const int16_t *r = aud_resample_lut + 4 * p;
		CHECK_EQ(r[0] + r[1] + r[2] + r[3], 32768);
	}
	/* Simétrica: la fase p y la 64 − p son espejo. */
	for (int p = 1; p < 64; p++)
		CHECK_EQ(aud_resample_lut[4 * p], aud_resample_lut[4 * (64 - p) + 3]);
}

/* ADPCM con 4 bits por muestra: byte de escala/predictor y 8 bytes. */
static void put_adpcm_frame(uint32_t pa, uint8_t code)
{
	static const uint8_t data[8] = { 0x12, 0x34, 0x56, 0x70, 0x9A, 0xBC, 0xDE, 0xF0 };
	ram(pa)[0] = code;
	memcpy(ram(pa + 1), data, 8);
}

static const int8_t nibbles[16] = { 1, 2, 3, 4, 5, 6, 7, 0, -7, -6, -5, -4, -3, -2, -1, 0 };

static void nead_adpcm(void)
{
	fresh();
	memset(ram(BOOK), 0, 256);
	put_adpcm_frame(SRC, 0x00);    /* escala 0: el residuo es el nibble */
	put_adpcm_frame(SRC + 9, 0xC0);  /* escala 12: nibble << 12 */
	memset(ram(SRC + 18), 0, 14);
	cmd(0x0B000000u | 256, BOOK);  /* LOADADPCM: libro en cero */
	n_loadbuff(0x100, SRC, 32);
	n_setbuff(0x100, 0x200, 64);   /* dos cuadros */
	cmd(0x01000000u | 0x01u << 16, STATE);  /* ADPCM A_INIT */
	n_savebuff(0x200, DST, 96);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* Primero las 16 muestras previas (cero), luego 16 por cuadro. */
	CHECK_EQ(get16(DST + 0), 0);
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(get16(DST + 32 + 2 * i), nibbles[i]);
		CHECK_EQ(get16(DST + 64 + 2 * i), nibbles[i] * 4096);
	}
	/* El estado queda con el último cuadro. */
	CHECK_EQ(get16(STATE + 0), 4096);
	CHECK_EQ(get16(STATE + 28), -1 * 4096);

	/* Predictor: book1[0] = 1,0 (Q11) suma la penúltima muestra previa a
	 * la primera de cada mitad. */
	memset(ram(BOOK), 0, 256);
	put16(BOOK, 2048);
	for (int i = 0; i < 16; i++)
		put16(STATE + 2 * i, (int16_t)(i == 14 ? 100 : i == 15 ? 200 : 0));
	cmd(0x0B000000u | 256, BOOK);
	n_loadbuff(0x100, SRC, 16);
	n_setbuff(0x100, 0x200, 32);
	cmd(0x01000000u, STATE);  /* continúa desde el estado */
	n_savebuff(0x200, DST, 64);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(get16(DST + 28), 100);              /* las previas, tal cual */
	CHECK_EQ(get16(DST + 32), 1 + 100);          /* 1.ª mitad: + último[14] */
	CHECK_EQ(get16(DST + 34), 2);
	CHECK_EQ(get16(DST + 48), nibbles[8] + 7);   /* 2.ª mitad: + nueva[6] */
}

static void nead_filter(void)
{
	fresh();
	/* Dos tablas cuyo promedio es un retardo de una muestra (L[1] = 1,0). */
	for (int i = 0; i < 8; i++) {
		put16(BOOK + 2 * i, (int16_t)(i == 1 ? 0x7FFF : 0));
		put16(STATE + 0x10 + 2 * i, (int16_t)(i == 1 ? 0x7FFF : 0));
		put16(STATE + 2 * i, (int16_t)(i == 7 ? 555 : 0));  /* muestras previas */
	}
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, (int16_t)(i + 1));
	n_loadbuff(0x100, SRC, 32);
	cmd(0x07000000u | 0x02u << 16 | 32, BOOK);   /* cuenta y primera tabla */
	cmd(0x07000000u | 0x100, STATE);             /* filtra (segunda tabla en +0x10) */
	n_savebuff(0x100, DST, 32);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.errors, 0);
	/* out[n] = x[n − 1] · 0x7FFF / 32768, redondeado. */
	CHECK_EQ(get16(DST + 0), 555);
	CHECK_EQ(get16(DST + 2), 1);
	CHECK_EQ(get16(DST + 30), 15);
	/* Estado: el último bloque de entrada. */
	CHECK_EQ(get16(STATE + 14), 16);
}

static void nead_errors_and_unknown(void)
{
	fresh();
	n_savebuff(0x100, 0x7FFFF8, 16);  /* se sale de los 8 MB */
	cmd(0x00000000u, 0);              /* el comando 0 de OoT no existe */
	cmd(0x7F000000u, 0);              /* fuera de la tabla */
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.commands, 3);
	CHECK_EQ(res.errors, 1);
	CHECK_EQ(res.unknown, 2);
	CHECK_EQ(res.last_unknown, 0x7F000000u);
	CHECK_EQ(res.nranges, 0);

	/* Un ABI sin HLE no ejecuta nada. */
	n_savebuff(0x100, DST, 16);
	run_abi(SALSA64_ABI_NAUDIO_MP3);
	CHECK_EQ(res.commands, 0);
	CHECK_EQ(res.errors, 1);
}

static void ranges_merge_when_full(void)
{
	fresh();
	for (uint32_t i = 0; i < AUD_MAX_RANGES + 2; i++)
		n_savebuff(0x100, DST + 0x100 * i, 8);
	run_abi(SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(res.nranges, AUD_MAX_RANGES);
	CHECK_EQ(res.range_addr[0], DST);
	uint32_t last = AUD_MAX_RANGES - 1;
	CHECK_EQ(res.range_addr[last], DST + 0x100 * last);
	CHECK_EQ(res.range_addr[last] + res.range_len[last], DST + 0x100 * (AUD_MAX_RANGES + 1) + 8);
}

/* ---- ABI1 (Super Mario 64) --------------------------------------------------- */

static void abi1_segments_envmixer(void)
{
	fresh();
	for (int i = 0; i < 16; i++)
		put16(SRC + 2 * i, 1000);
	cmd(0x07000000u, 0x05u << 24 | SRC);           /* SEGMENT 5 → SRC */
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);  /* SETBUFF in 0x5C0, out 0x6C0, 32 bytes */
	cmd(0x04000000u, 0x05u << 24);                 /* LOADBUFF desde el segmento 5 */
	cmd(0x02000000u | 0x100, 32);                  /* CLEARBUFF dl */
	cmd(0x02000000u | 0x200, 32);                  /* CLEARBUFF dr */
	cmd(0x08000000u | 0x08u << 16 | 0x200, 0x0300u << 16 | 0x400);  /* SETBUFF A_AUX: dr, wl, wr */
	cmd(0x09000000u | 0x08u << 16 | 0x7FFF, 0);    /* SETVOL A_AUX: dry 0x7FFF, wet 0 */
	cmd(0x09000000u | 0x06u << 16 | 0x4000, 0);    /* izquierda, volumen 0,5 */
	cmd(0x09000000u | 0x02u << 16 | 0x4000, 0x10000);  /* izquierda, destino 0,5 */
	cmd(0x09000000u | 0x04u << 16 | 0x2000, 0);    /* derecha, volumen 0,25 */
	cmd(0x09000000u | 0x00u << 16 | 0x2000, 0x10000);
	cmd(0x03000000u | 0x01u << 16, 0x05u << 24 | (STATE - SRC));  /* ENVMIXER A_INIT */
	cmd(0x06000000u, 0x05u << 24 | (DST - SRC));   /* SAVEBUFF out (dl) */
	cmd(0x08000000u | 0x000, 0x0200u << 16 | 32);
	cmd(0x06000000u, 0x05u << 24 | (DST + 32 - SRC));  /* SAVEBUFF dr */
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(res.errors, 0);
	CHECK_EQ(res.unknown, 0);
	/* Ganancias: (0x4000 · 0x7FFF + 0x4000) >> 15 = 0x4000 y (0x2000 · 0x7FFF
	 * + 0x4000) >> 15 = 0x2000: 1000 → 500 y 250. */
	for (int i = 0; i < 16; i++) {
		CHECK_EQ(get16(DST + 2 * i), 500);
		CHECK_EQ(get16(DST + 32 + 2 * i), 250);
	}
	/* Sin A_INIT sigue desde el estado guardado: mismas ganancias. */
	cmd(0x07000000u, 0x05u << 24 | SRC);
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);
	cmd(0x04000000u, 0x05u << 24);
	cmd(0x02000000u | 0x100, 32);
	cmd(0x03000000u, 0x05u << 24 | (STATE - SRC));
	cmd(0x06000000u, 0x05u << 24 | (DST - SRC));
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(get16(DST + 0), 500);
	CHECK_EQ(get16(DST + 30), 500);
	/* Los segmentos se borran en cada tarea. */
	CHECK_EQ(st.abi1.segments[5], SRC);
}

static void abi1_mixer_interleave(void)
{
	fresh();
	for (int i = 0; i < 16; i++) {
		put16(SRC + 2 * i, (int16_t)(2 * i));
		put16(SRC2 + 2 * i, (int16_t)(-2 * i));
	}
	cmd(0x08000000u | 0x000, 0x0100u << 16 | 32);
	cmd(0x04000000u, SRC);                         /* segmento 0: dirección física */
	cmd(0x08000000u | 0x200, 0x0100u << 16 | 32);
	cmd(0x04000000u, SRC2);                        /* 0x7C0 = derecha */
	cmd(0x0C000000u | 0x7FFF, 0x0000u << 16 | 0x200);  /* MIXER: derecha += izquierda */
	cmd(0x08000000u | 0x000, 0x0300u << 16 | 32);
	cmd(0x0D000000u, 0x0000u << 16 | 0x200);       /* INTERLEAVE → 0x8C0 */
	cmd(0x08000000u | 0x000, 0x0300u << 16 | 64);
	cmd(0x06000000u, DST);
	run_abi(SALSA64_ABI_AUDIO);
	CHECK_EQ(res.errors, 0);
	for (int i = 0; i < 16; i++) {
		int32_t l = 2 * i, r = -2 * i + ((l * 0x7FFF) >> 15);
		CHECK_EQ(get16(DST + 4 * i), l);
		CHECK_EQ(get16(DST + 4 * i + 2), r);
	}
}

/* ---- Tarea completa por el núcleo ------------------------------------------- */

static void core_runs_audio_task(void)
{
	static struct aud_state core_state;
	static struct salsa64_audio_sync sync;
	struct salsa64_bus *bus = &t_n64.bus;
	salsa64_audio_sync_init(&sync, &core_state, bus);
	salsa64_rsp_set_audio(bus, &sync.backend);
	T_LOAD(BEQ(0, 0, 0xFFFF), NOP);

	memset(ram(0x300000), 0, 0x40);
	memcpy(ram(0x300000), "Salsa64 ABI NEAD", 16);
	for (int i = 0; i < 8; i++)
		put16(SRC + 2 * i, (int16_t)(7 * i));
	nlist = 0;
	n_loadbuff(0x100, SRC, 16);
	n_savebuff(0x100, DST, 16);
	uint32_t task[16] = { SALSA64_TASK_AUDIO, 0, 0, 0x1000, 0x80301000u, 0x100, 0x80300000u, 0x40,
			      0, 0, 0, 0, 0x80000000u | ALIST, 8 * nlist, 0, 0 };
	for (int i = 0; i < 16; i++)
		salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 4 * i, task[i]);
	bus->sp.regs[SALSA64_SP_STATUS] = SALSA64_SP_STATUS_HALT;
	salsa64_bus_write32(bus, 0x04040010u, 0x001, ~0u);  /* quitar HALT */
	CHECK_EQ(bus->rsp.abi, SALSA64_ABI_NEAD_OOT);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_AUDIO);
	salsa64_n64_run(&t_n64, bus->rsp.audio_cycles + 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(bus->sp.regs[SALSA64_SP_STATUS] & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(sync.stats.tasks, 1);
	CHECK_EQ(sync.stats.commands, 2);
	CHECK_EQ(get16(DST + 14), 49);

	/* ABI desconocido: termina igual, sin HLE. */
	uint32_t unknown0 = bus->rsp.stats.unknown_abi;
	memset(ram(0x300000), 0, 0x40);
	bus->sp.regs[SALSA64_SP_STATUS] = SALSA64_SP_STATUS_HALT;
	salsa64_bus_write32(bus, 0x04040010u, 0x001, ~0u);
	CHECK_EQ(bus->rsp.abi, SALSA64_ABI_UNKNOWN);
	salsa64_n64_run(&t_n64, bus->rsp.audio_cycles + 1);
	CHECK(bus->sp.regs[SALSA64_SP_STATUS] & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(sync.stats.tasks, 1);
	CHECK_EQ(bus->rsp.stats.unknown_abi, unknown0 + 1);
	salsa64_rsp_set_audio(bus, NULL);
}

/* Las listas que compara el arranque en la PS3 (SPE contra PPE): corren sin
 * errores, con un solo comando desconocido a propósito en nead, y dos
 * corridas desde el mismo estado dejan lo mismo. */
static void selftest_lists_are_clean(void)
{
	static uint8_t a[AUD_SELFTEST_RAM], b[AUD_SELFTEST_RAM];
	static struct aud_state sa, sb;
	static const uint32_t abis[] = { SALSA64_ABI_NEAD_OOT, SALSA64_ABI_AUDIO, SALSA64_ABI_NEAD_MK };
	aud_init(&sa);
	aud_init(&sb);
	for (unsigned i = 0; i < 3; i++) {
		struct aud_job job;
		struct aud_result ra, rb;
		memset(&job, 0, sizeof job);
		job.rdram_size = AUD_SELFTEST_RAM;
		job.abi = abis[i];
		job.alist_size = aud_selftest_build(a, abis[i], 12345u + i);
		memcpy(b, a, sizeof b);
		job.rdram_ea = (uint64_t)(uintptr_t)a;
		aud_run(&sa, &job, &ra);
		job.rdram_ea = (uint64_t)(uintptr_t)b;
		aud_run(&sb, &job, &rb);
		CHECK_EQ(ra.errors, 0);
		CHECK_EQ(ra.commands, job.alist_size / 8);
		CHECK_EQ(ra.unknown, abis[i] == SALSA64_ABI_NEAD_OOT ? 1 : 0);
		CHECK(memcmp(a, b, sizeof a) == 0);
		/* La DMEM guardada no queda en cero: los comandos hicieron algo. */
		unsigned nonzero = 0;
		for (uint32_t k = 0x2000; k < 0x3000; k++)
			nonzero += a[k] != 0;
		CHECK(nonzero > 1000);
	}
}

void t_suite_audio(void)
{
	RUN(detect_abi);
	RUN(nead_load_mix_save);
	RUN(nead_buffer_moves);
	RUN(nead_interleave);
	RUN(nead_envmixer);
	RUN(nead_resample);
	RUN(lut_rows_sum_to_one);
	RUN(nead_adpcm);
	RUN(nead_filter);
	RUN(nead_errors_and_unknown);
	RUN(ranges_merge_when_full);
	RUN(abi1_segments_envmixer);
	RUN(abi1_mixer_interleave);
	RUN(core_runs_audio_task);
	RUN(selftest_lists_are_clean);
}
