/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — sincronía entre el audio emulado y la salida real.
 *
 * El AI produce muestras al ritmo de la emulación (hoy Zelda corre al ~27 %
 * de la velocidad real en la PS3); la salida las consume a 48 kHz de reloj
 * de pared. Entre los dos hay un anillo de un productor y un consumidor sin
 * bloqueos: la emulación nunca espera al audio y el audio nunca espera a la
 * emulación (si falta, sale otra cosa; nunca se repite a ciegas lo último
 * del puerto, que es lo que suena como un zumbido).
 *
 * Camino del consumidor (un hilo del PPE en la PS3; pruebas en el host):
 *
 *   anillo (Hz de la N64) → estirador WSOLA (Hz de la N64) → remuestreo
 *   cúbico a 48 kHz → salida
 *
 * El estirador suma granos de 20 ms con ventana de Hann cada 10 ms. A
 * velocidad normal toma los granos uno detrás de otro: la suma reconstruye
 * la señal exacta. Cuando la emulación va lenta (o rápida), avanza en la
 * entrada menos (o más) de 10 ms por grano y elige, en ±4 ms, el inicio que
 * mejor continúa el grano anterior: el audio dura más (o menos) sin cambiar
 * el tono. Todo cambio de lugar (saltos, silencios, reanudaciones) pasa por
 * esa suma con ventana, así que nunca hay un escalón: no hay chasquidos.
 *
 * Política (audio.cfg; docs/estado.md):
 *   estirar (por omisión)  la velocidad de lectura sigue a la de la
 *            emulación (medida en ~1 s) y corrige el nivel del anillo. A
 *            velocidad completa (±3 %) no se estira: la frecuencia del
 *            remuestreo se corrige hasta ±0,5 % (inaudible) para mantener
 *            el nivel. Si el anillo se vacía, repite el material más
 *            reciente hasta 150 ms (como un sostenido) y después calla
 *            hasta juntar el nivel objetivo.
 *   silencio nunca estira: si falta audio se desvanece en 10 ms, calla
 *            hasta juntar el nivel objetivo y sigue donde se quedó (sin
 *            perder nada); suena entrecortado pero con el tono y el ritmo
 *            exactos.
 * En las dos, si el anillo se llena de más (emulación más rápida que el
 * tiempo real), se salta lo viejo hasta el nivel objetivo, con la misma
 * transición suave.
 *
 * Quién marca el ritmo: la emulación va al paso de la tele emulada
 * (pacer.c, reloj de tiempo real) y el audio la sigue: la corrección de
 * ±0,5 % absorbe la deriva entre el reloj del PPE y el del DAC de la
 * consola, y el estirador cubre lo que la emulación no alcance (o se pase).
 * Así el audio nunca frena ni acelera a la emulación. Si algún día el audio
 * marcara el ritmo (esperar a que baje el nivel del anillo en vez de
 * dormir hasta la hora del campo), pacer.c tiene el enganche: su función
 * de dormir y pacer_nudge.
 *
 * Nivel objetivo: 60 ms o 1,5 veces lo que entrega el juego de una vez (lo
 * que sea mayor), más un grano y la búsqueda. Latencia total en la PS3:
 * eso más lo que va adelantado en el puerto de audio (~21 ms).
 */
#ifndef SALSA64_FRONTEND_AUDIO_SYNC_H
#define SALSA64_FRONTEND_AUDIO_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_OUT_RATE 48000u
#define AUDIO_RING     65536u   /* muestras estéreo: ~2 s a 32 kHz */
#define AUDIO_HOP_MAX  512u     /* 10 ms hasta 51,2 kHz */

enum audio_policy {
	AUDIO_STRETCH,
	AUDIO_SILENCE,
};

struct audio_sync_stats {
	uint64_t in_frames;        /* recibidas del AI */
	uint64_t out_frames;       /* entregadas a 48 kHz */
	uint64_t silent_frames;    /* de esas, en silencio por falta de audio */
	uint64_t stretch_frames;   /* de esas, estirando o comprimiendo */
	uint32_t underruns;        /* veces que se quedó sin audio */
	uint32_t silences;         /* veces que calló por eso */
	uint32_t skips;            /* saltos por desborde */
	uint32_t dropped;          /* muestras que no cupieron en el anillo */
};

struct audio_sync {
	/* ---- Productor (hilo de la emulación) ---- */
	int16_t ring[AUDIO_RING][2];
	volatile uint64_t wpos;     /* muestras escritas desde el principio */
	volatile uint64_t low;      /* la más vieja que el consumidor aún lee */
	volatile uint32_t rate;     /* Hz de lo último que llegó */
	volatile uint32_t chunk;    /* muestras por entrega (promedio) */
	volatile uint32_t dropped;

	/* ---- Consumidor (hilo de la salida) ---- */
	enum audio_policy policy;
	uint32_t min_level_ms;
	uint32_t in_rate;           /* con la que se armó el estirador */
	uint32_t hop, grain, search;
	float window[2 * AUDIO_HOP_MAX];
	float tail[AUDIO_HOP_MAX][2];
	float hopbuf[AUDIO_HOP_MAX][2];
	uint32_t hop_i, hop_n;
	int64_t prev;               /* inicio del grano anterior; −1: ninguno */
	double rpos;                /* posición nominal del grano siguiente */
	bool playing;
	bool stretching;
	uint32_t starve;            /* muestras de entrada repetidas sin datos nuevos */
	double speed;               /* velocidad de lectura actual (1 = normal) */
	double speed_est;           /* velocidad de la emulación medida */
	double drc;                 /* corrección de la frecuencia (±0,5 %) */
	uint64_t meas_pos, meas_us;
	bool measured;
	float x[4][2];              /* ventana del remuestreo cúbico */
	double frac;
	struct audio_sync_stats stats;
};

void audio_sync_init(struct audio_sync *as, enum audio_policy policy, uint32_t min_level_ms);

/* Productor: muestras del AI (16 bits estéreo en el orden de la N64). */
void audio_sync_push(struct audio_sync *as, const uint8_t *samples, uint32_t bytes, uint32_t rate_hz);

/* Consumidor: `frames` muestras estéreo a 48 kHz en `out` (flotantes
 * intercalados, ±1). `now_us`: reloj de pared, para medir la velocidad de
 * la emulación. */
void audio_sync_pull(struct audio_sync *as, float *out, uint32_t frames, uint64_t now_us);

/* Nivel del anillo en ms (consumidor). */
double audio_sync_level_ms(const struct audio_sync *as);

#endif
