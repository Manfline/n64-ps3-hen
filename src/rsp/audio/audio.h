/*
 * Salsa64 — HLE de los microcódigos de audio (listas de audio): interfaz.
 *
 * Una tarea de audio de libultra es una lista de comandos (alist) que
 * decodifica ADPCM, cambia el tono, aplica envolventes, mezcla en la DMEM
 * del RSP y guarda el resultado en la RDRAM, de donde el juego lo pasa al
 * AI. Aquí se ejecuta la lista entera de una vez: en el SPE 1 de la PS3
 * (src/spu/audio, E/S por DMA), en el PPE o en el host (backend síncrono,
 * pruebas). Mismo código en los tres; nada depende de PSL1GHT.
 *
 * Adaptación de mupen64plus-rsp-hle (GPLv2 o posterior; avisos de copyright
 * en alist.c y abi.c, origen en docs/THIRD_PARTY.md).
 */
#ifndef SALSA64_RSP_AUDIO_AUDIO_H
#define SALSA64_RSP_AUDIO_AUDIO_H

#include <stdint.h>

/* ---- Trabajo y resultado ---------------------------------------------------
 * Caben en el descriptor y el resultado de 112 bytes de lib/spukit. */

#define AUD_JOB_TASK 2  /* kind del trabajo en spukit */

struct aud_job {
	uint64_t rdram_ea;   /* dirección de la RDRAM emulada (EA del PPE) */
	uint32_t rdram_size;
	uint32_t abi;        /* enum salsa64_abi */
	uint32_t alist;      /* data_ptr del OSTask: dirección física de la lista */
	uint32_t alist_size; /* data_size: bytes */
	uint32_t flags;
	uint32_t reserved[9];
};

/* Partes de la RDRAM que escribió la tarea (para el recompilador). Si son
 * más, las últimas se juntan en un solo tramo que las cubre. */
#define AUD_MAX_RANGES 10

struct aud_result {
	uint32_t commands;      /* comandos ejecutados */
	uint32_t unknown;       /* comandos sin implementar */
	uint32_t last_unknown;  /* el último (w1) */
	uint32_t errors;        /* ABI sin HLE, direcciones fuera de la RDRAM */
	uint32_t nranges;
	uint32_t reserved[3];
	uint32_t range_addr[AUD_MAX_RANGES];
	uint32_t range_len[AUD_MAX_RANGES];
};

/* ---- Estado ----------------------------------------------------------------
 * La DMEM del microcódigo (4 KB, con margen para que ningún comando se
 * salga del búfer) y el estado de cada familia de ABI, que persiste entre
 * tareas como en mupen64plus-rsp-hle. ~8,7 KB; en el SPE vive en el local
 * store. */

#define AUD_DMEM_SIZE 0x1000u
#define AUD_DMEM_SLACK 0x1040u  /* un comando empieza en < 4 KB y abarca ≤ 4 KB */

struct aud_abi1 {           /* ABI1: Super Mario 64, GoldenEye… */
	uint32_t segments[16];
	uint16_t in, out, count;
	uint16_t dry_right, wet_left, wet_right;
	int16_t dry, wet;
	int16_t vol[2], target[2];
	int32_t rate[2];
	uint32_t loop;
	int16_t table[16 * 8];  /* libro de códigos ADPCM / coeficientes de POLEF */
};

struct aud_nead {           /* nead (ABI2): Zelda OoT, Mario Kart 64, Star Fox 64… */
	uint16_t in, out, count;
	uint16_t env_values[3], env_steps[3];
	uint32_t loop;
	uint16_t filter_count;
	uint32_t filter_lut[2];
	int16_t table[16 * 8];
};

struct aud_job;
struct aud_result;

struct aud_state {
	uint8_t dmem[AUD_DMEM_SIZE + AUD_DMEM_SLACK] __attribute__((aligned(16)));
	struct aud_abi1 abi1;
	struct aud_nead nead;
	const struct aud_job *job;
	struct aud_result *res;
	void *io;               /* estado de la plataforma (io_mem.c, src/spu/audio) */
};

void aud_init(struct aud_state *st);
/* Ejecuta la lista de audio del trabajo y llena `res`. */
void aud_run(struct aud_state *st, const struct aud_job *job, struct aud_result *res);

#endif
