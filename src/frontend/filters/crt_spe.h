/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — CRT con cable (filtro original del autor portado de su versión
 * de RetroMac, v1.1): la señal y la decodificación de S-Video y compuesto
 * en un SPE, en lugar de las pasadas «señal» y «decodifica» del RSX (ver
 * crt_cable_signal.fcg y crt_cable_decode.cgh: la misma cuenta). Lo
 * comparten filters.c (PPE) y el programa SPU (src/spu/crt/main.c).
 *
 * Cada cuadro: el RSX copia la imagen del juego reducida a la memoria
 * principal (RGBA8) y escribe la etiqueta `label_in`; cada SPE espera esa
 * etiqueta, hace sus líneas y deja las tablas de muestras decodificadas
 * (RGB antes de la gamma, en fp16) y escribe su etiqueta `label_out`; el RSX
 * las espera antes de la pasada «linealizar» (gamma y saturación, a su
 * memoria local) y sigue con las líneas del tubo.
 *
 * La configuración (lo que no cambia de un cuadro a otro: pesos, las
 * muestras de cada columna y la fase de cada línea) la arma el PPE en la
 * memoria principal; el SPE la trae cuando cambia su `id`.
 */
#ifndef SALSA64_FRONTEND_FILTERS_CRT_SPE_H
#define SALSA64_FRONTEND_FILTERS_CRT_SPE_H

#include <stdint.h>

#define CRT_SPE_TAPS      27   /* muestras del filtro de la tele (S-Video y compuesto) */
#define CRT_SPE_MAX_SW    640  /* píxeles por línea de la imagen del juego */
#define CRT_SPE_MAX_COLS  768  /* muestras por línea (~712: 4 por ciclo de subportadora) */
#define CRT_SPE_MAX_LINES 480
#define CRT_SPE_PAD       32   /* muestras en 0 a cada lado (el modo BORDER del RSX) */
#define CRT_SPE_PX_PAD    8    /* píxeles en 0 a cada lado */
#define CRT_SPE_WORKERS   2    /* SPEs: cada uno, la mitad de las líneas */

/* Una muestra de una tabla (E: x = x0 + m·dt; O, en compuesto: medio paso
 * después): el promedio de la señal sostenida es el píxel n − 1 por
 * (1 − t) más el píxel n por t, y la subportadora va con la fase θ de la
 * línea 0 (cada línea la gira lo suyo). */
struct crt_spe_col {
	int32_t n;   /* píxel n (puede ser −1 o sw: fuera de la imagen vale 0) */
	float t;
	float c, s;  /* cos θ y sin θ */
};

struct crt_spe_config {
	uint32_t id;        /* cambia con cada configuración */
	uint32_t sw, sh;    /* imagen del juego */
	uint32_t cols;      /* muestras por línea */
	uint32_t grids;     /* 1 (S-Video) o 2 (compuesto: E y O) */
	int32_t jlo;        /* la muestra j del filtro de la columna m es m + jlo + j */
	float kc;           /* 1 en compuesto (luma y color en el mismo cable) */
	uint32_t pad0;
	float wl[CRT_SPE_TAPS];  /* pesos de luma */
	float wc[CRT_SPE_TAPS];  /* pesos de color (I y Q usan los mismos) */
	float pad1[2];
	float yiq[3][4];          /* filas RGB → Y, I, Q */
	float rgb[3][4];          /* filas Y, I, Q → RGB */
	float line[CRT_SPE_MAX_LINES][2];  /* cos y sin de 2π·frac(y·fase por línea + fase del cuadro) */
	struct crt_spe_col col[2][CRT_SPE_MAX_COLS];
} __attribute__((aligned(128)));

/* El trabajo de un SPE en un cuadro (en el descriptor de spukit, ≤ 112 bytes). */
struct crt_spe_job {
	uint64_t config_ea;
	uint64_t src_ea;            /* imagen reducida: RGBA8 (bytes A, R, G, B) */
	uint64_t out_ea[2];         /* tablas E y O: RGBA16F (R, G, B, 0 en fp16) */
	uint64_t label_in_ea;       /* la escribe el RSX con `seq` cuando la imagen está */
	uint64_t label_out_ea;      /* la escribe el SPE con `seq` cuando terminó */
	uint32_t config_id, config_bytes;
	uint32_t seq;
	uint32_t src_pitch, out_pitch;
	uint32_t y0, y1;            /* sus líneas (y0 múltiplo de 4) */
	uint32_t pad;
};

struct crt_spe_result {
	uint32_t wait_ticks;  /* esperando la etiqueta del RSX (decrementador, spukit_tick_hz) */
	uint32_t work_ticks;  /* sus líneas, hasta escribir su etiqueta */
	uint32_t lines;
	int32_t err;
};

#define CRT_SPE_JOB 1  /* kind del trabajo en spukit */

#endif
