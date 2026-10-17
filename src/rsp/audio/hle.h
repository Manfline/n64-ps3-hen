/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — HLE de audio: operaciones comunes (alist.c), E/S por plataforma
 * y acceso a la DMEM. Ver audio.h.
 *
 * La DMEM se guarda en el orden de bytes de la N64 (como la del bus): las
 * muestras se leen y escriben con salsa64_rd16/wr16, que en el SPE y el PPE
 * son accesos directos. Las direcciones de la DMEM de los comandos se toman
 * módulo 4 KB al empezar cada operación; el margen de aud_state.dmem cubre
 * lo que la operación recorre después.
 */
#ifndef SALSA64_RSP_AUDIO_HLE_H
#define SALSA64_RSP_AUDIO_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/endian.h"
#include "rsp/audio/audio.h"

/* ---- DMEM ------------------------------------------------------------------ */

static inline uint8_t *aud_dm(struct aud_state *st, uint32_t addr)
{
	return st->dmem + (addr & (AUD_DMEM_SIZE - 1));
}

static inline int16_t aud_ld16(const uint8_t *p)
{
	return (int16_t)salsa64_rd16(p);
}

static inline void aud_st16(uint8_t *p, int16_t v)
{
	salsa64_wr16(p, (uint16_t)v);
}

/* Muestra número `pos` de la DMEM (con la vuelta de 4 KB). */
static inline int16_t aud_sample(struct aud_state *st, uint32_t pos)
{
	return aud_ld16(st->dmem + ((pos & 0x7FF) << 1));
}

static inline void aud_set_sample(struct aud_state *st, uint32_t pos, int16_t v)
{
	aud_st16(st->dmem + ((pos & 0x7FF) << 1), v);
}

static inline int16_t aud_clamp16(int32_t x)
{
	return x < -32768 ? -32768 : x > 32767 ? 32767 : (int16_t)x;
}

static inline uint32_t aud_align(uint32_t x, uint32_t amount)
{
	return (x + amount - 1) & ~(amount - 1);
}

/* ---- RDRAM -----------------------------------------------------------------
 * Por la E/S de la plataforma; las escrituras se anotan en el resultado. */

bool aud_load(struct aud_state *st, void *dst, uint32_t addr, uint32_t size);
bool aud_store(struct aud_state *st, uint32_t addr, const void *src, uint32_t size);
/* `n` muestras de 16 bits de la RDRAM (orden de la N64) a enteros nativos y
 * al revés. */
bool aud_load_s16(struct aud_state *st, int16_t *dst, uint32_t addr, uint32_t n);
bool aud_store_s16(struct aud_state *st, uint32_t addr, const int16_t *src, uint32_t n);

/* ---- E/S: la implementa cada plataforma (io_mem.c, src/spu/audio/io_spu.c) -
 * Direcciones físicas de la RDRAM (24 bits). */

uint32_t aud_io_clock(void);
void aud_io_begin(struct aud_state *st);
void aud_io_end(struct aud_state *st);
bool aud_io_read(struct aud_state *st, void *dst, uint32_t addr, uint32_t size);
bool aud_io_write(struct aud_state *st, uint32_t addr, const void *src, uint32_t size);
/* Comando número `index` de la lista (8 bytes). */
bool aud_io_alist(struct aud_state *st, uint32_t index, uint32_t *w1, uint32_t *w2);

/* ---- Operaciones comunes (alist.c) ----------------------------------------- */

#define A_INIT 0x01
#define A_LOOP 0x02
#define A_LEFT 0x02
#define A_VOL  0x04
#define A_AUX  0x08

extern const int16_t aud_resample_lut[64 * 4];

/* La tabla de interpolación del cambio de tono (RESAMPLE y el de MusyX):
 * la del segmento de datos del microcódigo del propio juego (ucode_data de
 * la tarea, en la RDRAM) si se encuentra, y si no la de lut.c. Se busca una
 * vez por dirección de ucode_data (aud_find_lut, desde aud_run). */
void aud_find_lut(struct aud_state *st);
/* El arranque de libultra (rspboot) copia el segmento de datos del
 * microcódigo (ucode_data, ucode_data_size bytes) a la DMEM antes de cada
 * tarea. En los microcódigos con 2 KB de datos (GoldenEye, Diddy Kong Racing,
 * naudio) eso pisa los búferes de 0x4F0/0x5C0 a 0x7FF con lo que tenga la
 * RDRAM ahí, no con lo que dejó la tarea anterior. Lo hace aud_run. */
void aud_boot_data(struct aud_state *st);

static inline const int16_t *aud_lut(const struct aud_state *st)
{
	return st->lut_ok ? st->lut : aud_resample_lut;
}

void alist_clear(struct aud_state *st, uint16_t dmem, uint16_t count);
void alist_load(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count);
void alist_save(struct aud_state *st, uint16_t dmem, uint32_t address, uint16_t count);
void alist_move(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count);
void alist_copy_every_other_sample(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count);
void alist_repeat64(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint8_t count);
void alist_copy_blocks(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t block_size, uint8_t count);
void alist_interleave(struct aud_state *st, uint16_t dmemo, uint16_t left, uint16_t right, uint16_t count);

enum aud_envmix_kind { AUD_ENVMIX_EXP, AUD_ENVMIX_GE, AUD_ENVMIX_LIN, AUD_ENVMIX_LIN_PHASE, AUD_ENVMIX_LIN_CBFD };

/* ENVMIXER de ABI1: exponencial (el de libultra), lineal de GoldenEye o
 * lineal con los cuatro búferes (naudio); LIN_PHASE es el de naudio de
 * Donkey Kong 64, Perfect Dark, Banjo-Tooie y Conker, donde el bit bajo del
 * volumen seco invierte la fase del canal izquierdo y el del húmedo, la del
 * derecho. */
void alist_envmix(struct aud_state *st, enum aud_envmix_kind kind, bool init, bool aux,
		  uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi,
		  uint16_t count, int16_t dry, int16_t wet, const int16_t *vol, const int16_t *target,
		  const int32_t *rate, uint32_t address);
void alist_envmix_nead(struct aud_state *st, bool swap_wet_lr, uint16_t dmem_dl, uint16_t dmem_dr,
		       uint16_t dmem_wl, uint16_t dmem_wr, uint16_t dmemi, unsigned count, unsigned align,
		       uint16_t *env_values, const uint16_t *env_steps, const int16_t *xors);

void alist_mix(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count, int16_t gain);
void alist_multQ44(struct aud_state *st, uint16_t dmem, uint16_t count, int8_t gain);
void alist_add(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count, bool carry3);

void alist_adpcm(struct aud_state *st, bool init, bool loop, bool two_bit_per_sample, uint16_t dmemo,
		 uint16_t dmemi, uint16_t count, const int16_t *codebook, uint32_t loop_address,
		 uint32_t last_frame_address);
void alist_resample(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		    uint32_t pitch, uint32_t address);
void alist_resample_zoh(struct aud_state *st, uint16_t dmemo, uint16_t dmemi, uint16_t count,
			uint32_t pitch, uint32_t pitch_accu);
void alist_filter(struct aud_state *st, bool init, uint16_t dmem, uint16_t count, uint32_t address,
		  const uint32_t *lut_address, bool average);
void alist_polef(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		 uint16_t gain, int16_t *table, uint32_t address);
void alist_iirf(struct aud_state *st, bool init, uint16_t dmemo, uint16_t dmemi, uint16_t count,
		const int16_t *table, uint32_t address);
void alist_overload(struct aud_state *st, uint16_t dmem, uint16_t count, int16_t gain, uint16_t attenuation);

/* ---- MusyX (musyx.c) ---------------------------------------------------------
 * Una tarea de MusyX entera: job->alist es la dirección de la lista de
 * subtramas y job->alist_size cuántas son (no es una lista de comandos).
 * Sin estado propio: lo que persiste entre tareas vive en la RDRAM del
 * juego. */
#define AUD_MUSYX_MAX_SFD 32u
/* `early`: la compilación de Rogue Squadron (SALSA64_ABI_MUSYX1_RS). */
void aud_musyx(struct aud_state *st, bool v2, bool early);

/* ---- MP3 (mp3.c) ------------------------------------------------------------
 * Síntesis de un gránulo (576 muestras) de MPEG-1 Layer III: lee de la
 * RDRAM 8 bytes de ganancias y 3 bloques de 32 × 6 muestras de subbanda y
 * escribe encima las muestras PCM. `index` es la posición (par, 0–30) en
 * el anillo de la ventana de síntesis. */
void aud_mp3(struct aud_state *st, unsigned index, uint32_t address);
extern const int16_t aud_mp3_window[0x420];

#endif
