/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — sincronía entre el audio emulado y la salida real. Ver
 * audio_sync.h. Código propio; WSOLA (solapamiento y suma con búsqueda de
 * la mejor continuación) es la técnica conocida de Verhelst y Roelands
 * (1993), escrita desde su descripción.
 */
#include "audio_sync.h"

#include <math.h>
#include <string.h>

#include "core/endian.h"

#define MASK          (AUDIO_RING - 1)
#define MAX_STARVE_MS 150    /* repetir a lo sumo esto antes de callar */
#define SKIP_EXTRA_MS 500    /* nivel de más que dispara un salto */
#define MEASURE_US    250000 /* cada cuánto se mide la velocidad */

static uint64_t load_acquire(const volatile uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_release(volatile uint64_t *p, uint64_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void audio_sync_init(struct audio_sync *as, enum audio_policy policy, uint32_t min_level_ms)
{
	memset(as, 0, sizeof *as);
	as->policy = policy;
	as->min_level_ms = min_level_ms ? min_level_ms : 60;
	as->prev = -1;
	as->speed = 1.0;
	as->speed_est = 1.0;
}

/* ---- Productor ---------------------------------------------------------------- */

void audio_sync_push(struct audio_sync *as, const uint8_t *samples, uint32_t bytes, uint32_t rate_hz)
{
	uint32_t n = bytes / 4;
	uint64_t w = as->wpos, low = load_acquire(&as->low);
	uint64_t room = AUDIO_RING - (w - low);
	if (n > room) {
		as->dropped += n - (uint32_t)room;
		n = (uint32_t)room;
	}
	for (uint32_t i = 0; i < n; i++) {
		int16_t *f = as->ring[(w + i) & MASK];
		f[0] = (int16_t)salsa64_rd16(samples + 4 * i);
		f[1] = (int16_t)salsa64_rd16(samples + 4 * i + 2);
	}
	as->rate = rate_hz;
	as->chunk = as->chunk ? (as->chunk * 3 + bytes / 4) / 4 : bytes / 4;
	store_release(&as->wpos, w + n);
}

/* ---- Consumidor: estirador ------------------------------------------------------ */

static void setup(struct audio_sync *as, uint32_t rate)
{
	as->in_rate = rate;
	uint32_t hop = rate / 100;
	hop = hop < 64 ? 64 : hop > AUDIO_HOP_MAX ? AUDIO_HOP_MAX : hop;
	as->hop = hop;
	as->grain = 2 * hop;
	as->search = rate / 250;
	for (uint32_t n = 0; n < as->grain; n++)
		as->window[n] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * n / as->grain));
	memset(as->tail, 0, sizeof as->tail);
	as->prev = -1;
	as->hop_i = as->hop_n = 0;
}

static double target_level(const struct audio_sync *as)
{
	double base = as->in_rate * (double)as->min_level_ms / 1000.0, chunk = 1.5 * as->chunk;
	return (base > chunk ? base : chunk) + as->grain + as->search;
}

static float sample(const struct audio_sync *as, int64_t i, int c)
{
	return as->ring[(uint64_t)i & MASK][c] * (1.0f / 32768.0f);
}

static float mono(const struct audio_sync *as, int64_t i)
{
	const int16_t *f = as->ring[(uint64_t)i & MASK];
	return (float)(f[0] + f[1]);
}

/* El inicio en [lo, hi] cuyo primer medio grano mejor continúa lo que
 * seguía al grano anterior ([nat, nat + hop)): correlación normalizada
 * sobre una de cada 4 muestras, candidatos de 2 en 2 y un ajuste de ±1. */
static int64_t best_start(const struct audio_sync *as, int64_t nat, int64_t lo, int64_t hi)
{
	if (lo >= hi)
		return hi;
	float ref[AUDIO_HOP_MAX / 4 + 1];
	uint32_t m = 0;
	for (uint32_t k = 0; k < as->hop; k += 4)
		ref[m++] = mono(as, nat + k);
	float best = -1e30f;
	int64_t bq = hi;
	for (int pass = 0; pass < 2; pass++) {
		int64_t a = pass ? bq - 1 : lo, b = pass ? bq + 1 : hi, step = pass ? 1 : 2;
		if (a < lo)
			a = lo;
		if (b > hi)
			b = hi;
		for (int64_t q = a; q <= b; q += step) {
			float c = 0.0f, e = 1e-3f;
			for (uint32_t j = 0; j < m; j++) {
				float v = mono(as, q + 4 * j);
				c += ref[j] * v;
				e += v * v;
			}
			float score = c / sqrtf(e);
			if (score > best) {
				best = score;
				bq = q;
			}
		}
	}
	return bq;
}

/* Calla: lo que quedaba del grano anterior se desvanece en este salto. */
static void go_silent(struct audio_sync *as, double resume_at)
{
	as->playing = false;
	as->prev = -1;
	as->starve = 0;
	as->rpos = resume_at;
	as->stats.silences++;
}

/* Un salto de salida (hop muestras a la frecuencia de entrada). */
static void make_hop(struct audio_sync *as)
{
	uint64_t w = load_acquire(&as->wpos);
	double target = target_level(as);
	uint32_t hop = as->hop;
	int64_t avail_hi = (int64_t)w - (int64_t)as->grain;  /* último inicio con un grano completo */
	int64_t low = (int64_t)as->low;
	int64_t q = -1;

	if (!as->playing && (double)w - as->rpos >= target) {
		as->playing = true;
		as->prev = -1;
		as->starve = 0;
	}
	if (as->playing) {
		double level = (double)w - as->rpos;
		if (level > target + as->in_rate * (SKIP_EXTRA_MS / 1000.0)) {
			/* Desborde: saltar lo viejo; el grano nuevo entra con su
			 * ventana mientras el anterior sale con la suya. */
			as->rpos = (double)w - target;
			as->prev = -1;
			as->stats.skips++;
		}
		bool starving;
		if (as->stretching && as->prev >= 0) {
			int64_t want = (int64_t)floor(as->rpos);
			starving = want > avail_hi;
			int64_t lo = want - as->search, hi = want + as->search;
			if (hi > avail_hi)
				hi = avail_hi;
			if (lo < low)
				lo = low;
			if (lo > hi)
				lo = hi;
			q = hi >= low ? best_start(as, as->prev + hop, lo, hi) : -1;
			as->rpos += as->speed * hop;
			if (as->rpos > (double)avail_hi + as->search)
				as->rpos = (double)avail_hi + as->search;  /* no adelantarse a los datos */
		} else {
			q = as->prev >= 0 ? as->prev + hop : (int64_t)floor(as->rpos);
			starving = q > avail_hi;
			if (starving) {
				if (as->policy == AUDIO_STRETCH && as->prev >= 0) {
					/* Repetir lo más reciente que mejor continúe. */
					int64_t lo = avail_hi - 2 * (int64_t)as->search;
					q = avail_hi >= low ? best_start(as, as->prev + hop, lo < low ? low : lo, avail_hi) : -1;
				} else {
					q = -1;
				}
			}
			if (q >= 0)
				as->rpos = (double)(q + hop);
		}
		if (starving) {
			if (!as->starve)
				as->stats.underruns++;
			as->starve += hop;
			if (as->policy == AUDIO_SILENCE || q < 0 ||
			    as->starve >= as->in_rate * (MAX_STARVE_MS / 1000.0)) {
				int64_t resume = as->prev >= 0 ? as->prev + hop : (int64_t)floor(as->rpos);
				if (resume > (int64_t)w)
					resume = (int64_t)w;
				go_silent(as, (double)resume);
				q = -1;
			}
		} else {
			as->starve = 0;
		}
	}

	if (q >= 0) {
		for (uint32_t i = 0; i < hop; i++) {
			for (int c = 0; c < 2; c++) {
				as->hopbuf[i][c] = as->tail[i][c] + as->window[i] * sample(as, q + i, c);
				as->tail[i][c] = as->window[hop + i] * sample(as, q + hop + i, c);
			}
		}
		as->prev = q;
	} else {
		memcpy(as->hopbuf, as->tail, hop * sizeof as->tail[0]);
		memset(as->tail, 0, hop * sizeof as->tail[0]);
		as->prev = -1;
	}
	as->hop_i = 0;
	as->hop_n = hop;

	/* Lo más viejo que se puede volver a leer. */
	int64_t keep = as->prev >= 0 && as->prev < as->rpos ? as->prev : (int64_t)floor(as->rpos);
	keep -= (int64_t)as->search + 4;
	if (keep < low)
		keep = low;
	if (keep > (int64_t)w)
		keep = (int64_t)w;
	store_release(&as->low, (uint64_t)keep);
}

static void next_in(struct audio_sync *as, float f[2])
{
	if (as->hop_i == as->hop_n)
		make_hop(as);
	f[0] = as->hopbuf[as->hop_i][0];
	f[1] = as->hopbuf[as->hop_i][1];
	as->hop_i++;
}

/* ---- Consumidor: control y remuestreo -------------------------------------------- */

static void control(struct audio_sync *as, uint64_t now_us)
{
	uint64_t w = load_acquire(&as->wpos);
	if (!as->measured) {
		as->measured = true;
		as->meas_pos = w;
		as->meas_us = now_us;
	} else if (now_us - as->meas_us >= MEASURE_US) {
		double inst = (double)(w - as->meas_pos) / ((now_us - as->meas_us) / 1e6) / as->in_rate;
		as->speed_est = 0.75 * as->speed_est + 0.25 * inst;
		as->meas_pos = w;
		as->meas_us = now_us;
	}

	double target = target_level(as), err = ((double)w - as->rpos - target) / target;
	if (as->policy == AUDIO_STRETCH) {
		double dev = fabs(as->speed_est - 1.0);
		if (!as->stretching && dev > 0.05)
			as->stretching = true;
		else if (as->stretching && dev < 0.03)
			as->stretching = false;
	} else {
		as->stretching = false;
	}
	if (as->stretching) {
		double s = as->speed_est * (1.0 + 0.5 * err);
		as->speed = s < 0.1 ? 0.1 : s > 2.0 ? 2.0 : s;
		as->drc = 0.0;
	} else {
		double d = 0.005 * err;
		as->speed = 1.0;
		as->drc = d < -0.005 ? -0.005 : d > 0.005 ? 0.005 : d;
	}
}

void audio_sync_pull(struct audio_sync *as, float *out, uint32_t frames, uint64_t now_us)
{
	uint32_t rate = as->rate;
	if (!rate) {
		memset(out, 0, frames * 2 * sizeof *out);
		as->stats.out_frames += frames;
		as->stats.silent_frames += frames;
		return;
	}
	if (rate != as->in_rate)
		setup(as, rate);
	control(as, now_us);

	double ratio = (double)as->in_rate / AUDIO_OUT_RATE * (1.0 + as->drc);
	for (uint32_t k = 0; k < frames; k++) {
		while (as->frac >= 1.0) {
			memmove(as->x[0], as->x[1], 3 * sizeof as->x[0]);
			next_in(as, as->x[3]);
			as->frac -= 1.0;
		}
		float t = (float)as->frac;
		for (int c = 0; c < 2; c++) {
			float x0 = as->x[0][c], x1 = as->x[1][c], x2 = as->x[2][c], x3 = as->x[3][c];
			float y = x1 + 0.5f * t * (x2 - x0 + t * (2.0f * x0 - 5.0f * x1 + 4.0f * x2 - x3 +
								 t * (3.0f * (x1 - x2) + x3 - x0)));
			out[2 * k + c] = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
		}
		as->frac += ratio;
		if (!as->playing)
			as->stats.silent_frames++;
		else if (as->stretching)
			as->stats.stretch_frames++;
	}
	as->stats.out_frames += frames;
	as->stats.in_frames = as->wpos;
	as->stats.dropped = as->dropped;
}

double audio_sync_level_ms(const struct audio_sync *as)
{
	return as->in_rate ? ((double)load_acquire(&as->wpos) - as->rpos) * 1000.0 / as->in_rate : 0.0;
}
