/*
 * Salsa64 — pruebas de la sincronía del audio (src/frontend/audio_sync.c):
 * un productor simulado entrega un seno de 440 Hz a 32 kHz en búferes de
 * 533 muestras, más lento o más rápido que el tiempo real, y se analiza lo
 * que sale a 48 kHz: el tono (cruces por cero), que no haya escalones
 * (chasquidos), cuánto silencio hay y el nivel del anillo.
 */
#include "test.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "core/endian.h"
#include "frontend/audio_sync.h"

#define IN_RATE  32000u
#define CHUNK    533u
#define AMP      16000
#define SECONDS  10
#define STEP     256u  /* muestras por llamada, como un bloque del puerto */

static struct audio_sync as;
static float out[AUDIO_OUT_RATE * SECONDS];  /* canal izquierdo */
static uint8_t chunk_buf[CHUNK * 4];

struct sim_result {
	double max_step;      /* mayor salto entre muestras seguidas, tras el arranque */
	double freq_min, freq_max;  /* tono en ventanas con sonido pleno */
	unsigned windows;     /* ventanas con sonido pleno */
	double silent;        /* fracción en silencio tras el arranque */
	double max_level_ms;
};

/* Corre SECONDS segundos de pared con la emulación a `speed`; analiza desde
 * `warmup` segundos. */
static void simulate(enum audio_policy policy, double speed, double warmup, struct sim_result *r)
{
	audio_sync_init(&as, policy, 60);
	uint64_t produced = 0, n = 0;
	double emu_frames = 0.0;
	uint32_t total = AUDIO_OUT_RATE * SECONDS;
	float block[STEP * 2];
	r->max_level_ms = 0.0;
	uint64_t silent0 = 0, out0 = 0;

	for (uint32_t k = 0; k + STEP <= total; k += STEP) {
		uint64_t now_us = (uint64_t)k * 1000000u / AUDIO_OUT_RATE;
		/* La emulación avanza `speed` veces el tiempo de pared y entrega
		 * búferes completos. */
		emu_frames += speed * STEP * IN_RATE / AUDIO_OUT_RATE;
		while (produced + CHUNK <= emu_frames) {
			for (uint32_t i = 0; i < CHUNK; i++, n++) {
				int16_t v = (int16_t)lrint(AMP * sin(2.0 * M_PI * 440.0 * n / IN_RATE));
				salsa64_wr16(chunk_buf + 4 * i, (uint16_t)v);
				salsa64_wr16(chunk_buf + 4 * i + 2, (uint16_t)v);
			}
			audio_sync_push(&as, chunk_buf, sizeof chunk_buf, IN_RATE);
			produced += CHUNK;
		}
		audio_sync_pull(&as, block, STEP, now_us);
		for (uint32_t i = 0; i < STEP; i++)
			out[k + i] = block[2 * i];
		if (k >= warmup * AUDIO_OUT_RATE) {
			if (!out0) {
				out0 = as.stats.out_frames;
				silent0 = as.stats.silent_frames;
			}
			double lv = audio_sync_level_ms(&as);
			if (lv > r->max_level_ms)
				r->max_level_ms = lv;
		}
	}
	uint32_t from = (uint32_t)(warmup * AUDIO_OUT_RATE);
	r->silent = (double)(as.stats.silent_frames - silent0) / (as.stats.out_frames - out0);
	r->max_step = 0.0;
	for (uint32_t i = from + 1; i < total; i++) {
		double d = fabs(out[i] - out[i - 1]);
		if (d > r->max_step)
			r->max_step = d;
	}
	/* Tono: ventanas de 4096 con sonido pleno (RMS > 70 % del seno en cada
	 * cuarto: ni una entrada ni una salida de silencio adentro). */
	double full = AMP / 32768.0 / sqrt(2.0) * 0.7;
	r->freq_min = 1e9;
	r->freq_max = 0.0;
	r->windows = 0;
	for (uint32_t w = from; w + 4096 <= total; w += 4096) {
		double e[4] = { 0.0, 0.0, 0.0, 0.0 };
		unsigned cross = 0;
		for (uint32_t i = w; i < w + 4096; i++) {
			e[(i - w) / 1024] += out[i] * out[i];
			if (i > w && out[i - 1] < 0.0f && out[i] >= 0.0f)
				cross++;
		}
		bool quiet = false;
		for (int q = 0; q < 4; q++)
			quiet |= sqrt(e[q] / 1024) < full;
		if (quiet)
			continue;
		double f = cross * (double)AUDIO_OUT_RATE / 4096;
		r->windows++;
		if (f < r->freq_min)
			r->freq_min = f;
		if (f > r->freq_max)
			r->freq_max = f;
	}
}

/* Un seno de 440 Hz y amplitud 0,49 a 48 kHz cambia a lo sumo 0,028 por
 * muestra; un chasquido es un escalón mucho mayor. */
#define SMOOTH 0.06

static void full_speed_is_transparent(void)
{
	struct sim_result r;
	simulate(AUDIO_STRETCH, 1.0, 1.0, &r);
	CHECK(r.silent == 0.0);
	CHECK(r.max_step < SMOOTH);
	CHECK(r.windows >= 90);
	CHECK(r.freq_min >= 428.0 && r.freq_max <= 452.0);  /* 4096 muestras: ±1 cruce */
	CHECK_EQ(as.stats.stretch_frames, 0);
	CHECK_EQ(as.stats.skips, 0);
	CHECK(r.max_level_ms < 200.0);
	CHECK_EQ(as.stats.dropped, 0);
}

static void slow_emulation_stretches(void)
{
	struct sim_result r;
	simulate(AUDIO_STRETCH, 0.27, 3.0, &r);
	CHECK(as.stretching);
	CHECK(r.silent < 0.05);           /* casi todo suena */
	CHECK(r.max_step < SMOOTH);
	CHECK(r.windows >= 60);
	CHECK(r.freq_min >= 420.0 && r.freq_max <= 460.0);  /* el tono no baja */
	CHECK(fabs(as.speed_est - 0.27) < 0.05);
}

static void slow_emulation_silence_policy(void)
{
	struct sim_result r;
	simulate(AUDIO_SILENCE, 0.27, 3.0, &r);
	CHECK(!as.stretching);
	CHECK(r.silent > 0.5 && r.silent < 0.85);  /* suena ~27 % del tiempo */
	CHECK(r.max_step < SMOOTH);       /* entra y sale sin escalones */
	CHECK(r.windows >= 5);
	CHECK(r.freq_min >= 428.0 && r.freq_max <= 452.0);
	CHECK(as.stats.silences > 10);
}

static void fast_emulation_stays_bounded(void)
{
	struct sim_result r;
	simulate(AUDIO_STRETCH, 1.3, 3.0, &r);
	CHECK(r.max_step < SMOOTH);
	CHECK(r.max_level_ms < 700.0);
	CHECK(r.freq_min >= 420.0 && r.freq_max <= 460.0);
	simulate(AUDIO_SILENCE, 1.3, 3.0, &r);
	CHECK(r.max_step < SMOOTH);
	CHECK(r.max_level_ms < 700.0);
	CHECK(as.stats.skips > 0);
}

static void no_audio_is_silence(void)
{
	float block[STEP * 2];
	audio_sync_init(&as, AUDIO_STRETCH, 60);
	audio_sync_pull(&as, block, STEP, 0);
	for (uint32_t i = 0; i < 2 * STEP; i++)
		CHECK(block[i] == 0.0f);
	CHECK_EQ(as.stats.silent_frames, STEP);
}

/* Vaciado (cargar un estado guardado): lo que había en el anillo no vuelve
 * a sonar; lo nuevo sí, sin escalones. */
static void push_const(int16_t v, uint32_t frames)
{
	for (uint32_t done = 0; done < frames; done += CHUNK) {
		for (uint32_t i = 0; i < CHUNK; i++) {
			salsa64_wr16(chunk_buf + 4 * i, (uint16_t)v);
			salsa64_wr16(chunk_buf + 4 * i + 2, (uint16_t)v);
		}
		audio_sync_push(&as, chunk_buf, sizeof chunk_buf, IN_RATE);
	}
}

static void flush_drops_queued_audio(void)
{
	float block[STEP * 2];
	audio_sync_init(&as, AUDIO_STRETCH, 60);
	push_const(8000, IN_RATE / 4);  /* 250 ms de lo viejo */
	uint64_t t = 0;
	for (int k = 0; k < 8; k++, t += 5333)
		audio_sync_pull(&as, block, STEP, t);
	CHECK(fabsf(block[2 * STEP - 2] - 8000.0f / 32768.0f) < 0.01f);  /* sonaba lo viejo */
	audio_sync_flush(&as);
	push_const(-4000, IN_RATE / 4);  /* lo nuevo */
	float old_max = 0.0f, prev = block[2 * STEP - 2], step = 0.0f;
	int saw_new = 0;
	for (int k = 0; k < 40; k++, t += 5333) {
		audio_sync_pull(&as, block, STEP, t);
		for (uint32_t i = 0; i < STEP; i++) {
			float v = block[2 * i];
			if (k > 2 && v > old_max)
				old_max = v;  /* después del desvanecimiento, nada de lo viejo */
			if (fabsf(v - prev) > step)
				step = fabsf(v - prev);
			prev = v;
			saw_new |= fabsf(v + 4000.0f / 32768.0f) < 0.01f;
		}
	}
	CHECK(old_max < 0.01f);
	CHECK(saw_new);
	CHECK(step < 0.05f);
}

/* Pausa del juego (menú del sistema, cuadro de salir): 3 s sonando, 5 s
 * sin que la emulación entregue nada y 2 s más. Con `pause`, como en la
 * PS3 (audio_out_pause), si no solo el hueco. */
#define GAP_FROM 3.0
#define GAP_TO   8.0

struct gap_result {
	double max_step;        /* mayor salto entre muestras seguidas */
	float quiet_max;        /* mayor |muestra| en la pausa, tras el desvanecimiento */
	double back_after;      /* segundos desde el fin de la pausa hasta volver a sonar */
	uint32_t underruns_gap, silences_gap;  /* faltas y silencios durante la pausa */
	uint64_t stretch_after; /* muestras estiradas después */
	double speed_end;
};

static void simulate_gap(bool pause, struct gap_result *r)
{
	audio_sync_init(&as, AUDIO_STRETCH, 60);
	uint64_t produced = 0, n = 0;
	double emu_frames = 0.0;
	uint32_t total = AUDIO_OUT_RATE * SECONDS;
	float block[STEP * 2];
	uint32_t under0 = 0, sil0 = 0;
	uint64_t stretch0 = 0;
	bool paused = false;
	for (uint32_t k = 0; k + STEP <= total; k += STEP) {
		uint64_t now_us = (uint64_t)k * 1000000u / AUDIO_OUT_RATE;
		double t = (double)k / AUDIO_OUT_RATE;
		bool gap = t >= GAP_FROM && t < GAP_TO;
		if (gap != paused) {
			if (gap) {
				under0 = as.stats.underruns;
				sil0 = as.stats.silences;
			} else {
				r->underruns_gap = as.stats.underruns - under0;
				r->silences_gap = as.stats.silences - sil0;
				stretch0 = as.stats.stretch_frames;
			}
			if (pause)
				audio_sync_pause(&as, gap);
			paused = gap;
		}
		if (!gap) {
			emu_frames += (double)STEP * IN_RATE / AUDIO_OUT_RATE;
			while (produced + CHUNK <= emu_frames) {
				for (uint32_t i = 0; i < CHUNK; i++, n++) {
					int16_t v = (int16_t)lrint(AMP * sin(2.0 * M_PI * 440.0 * n / IN_RATE));
					salsa64_wr16(chunk_buf + 4 * i, (uint16_t)v);
					salsa64_wr16(chunk_buf + 4 * i + 2, (uint16_t)v);
				}
				audio_sync_push(&as, chunk_buf, sizeof chunk_buf, IN_RATE);
				produced += CHUNK;
			}
		}
		audio_sync_pull(&as, block, STEP, now_us);
		for (uint32_t i = 0; i < STEP; i++)
			out[k + i] = block[2 * i];
	}
	r->stretch_after = as.stats.stretch_frames - stretch0;
	r->speed_end = as.speed_est;
	r->max_step = 0.0;
	for (uint32_t i = AUDIO_OUT_RATE; i < total; i++) {
		double d = fabs(out[i] - out[i - 1]);
		if (d > r->max_step)
			r->max_step = d;
	}
	r->quiet_max = 0.0f;
	for (uint32_t i = (uint32_t)((GAP_FROM + 0.05) * AUDIO_OUT_RATE); i < GAP_TO * AUDIO_OUT_RATE; i++)
		if (fabsf(out[i]) > r->quiet_max)
			r->quiet_max = fabsf(out[i]);
	r->back_after = 99.0;
	for (uint32_t i = (uint32_t)(GAP_TO * AUDIO_OUT_RATE); i < total; i++) {
		if (fabsf(out[i]) > 0.3f) {
			r->back_after = (double)i / AUDIO_OUT_RATE - GAP_TO;
			break;
		}
	}
}

/* Con la pausa: se desvanece, calla sin contar faltas, vuelve a sonar al
 * juntar el nivel y sigue sin estirar ni chasquidos. */
static void pause_keeps_speed_and_resumes(void)
{
	struct gap_result r;
	simulate_gap(true, &r);
	CHECK(r.max_step < SMOOTH);
	CHECK(r.quiet_max == 0.0f);
	CHECK_EQ(r.underruns_gap, 0);
	CHECK_EQ(r.silences_gap, 0);
	CHECK(r.back_after < 0.15);
	CHECK_EQ(r.stretch_after, 0);
	CHECK(r.speed_end > 0.97 && r.speed_end < 1.03);
	CHECK_EQ(as.stats.pauses, 1);
}

/* Sin ella, el hueco cuenta como emulación lenta: la velocidad medida cae
 * y al volver el audio sale estirado un buen rato (por eso la pausa). */
static void gap_without_pause_stretches(void)
{
	struct gap_result r;
	simulate_gap(false, &r);
	CHECK(r.underruns_gap > 0);
	CHECK(r.stretch_after > AUDIO_OUT_RATE / 2);
	CHECK_EQ(as.stats.pauses, 0);
}

void t_suite_audio_sync(void)
{
	RUN(full_speed_is_transparent);
	RUN(slow_emulation_stretches);
	RUN(slow_emulation_silence_policy);
	RUN(fast_emulation_stays_bounded);
	RUN(no_audio_is_silence);
	RUN(flush_drops_queued_audio);
	RUN(pause_keeps_speed_and_resumes);
	RUN(gap_without_pause_stretches);
}
