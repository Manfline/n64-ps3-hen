/*
 * Salsa64 — audio del frontend en la PS3. Ver audio_out.h.
 *
 * El hilo de audio no escribe al log (salsa64_log no es seguro entre
 * hilos): deja contadores que audio_out_field lleva al log desde el hilo
 * principal.
 */
#include "audio_out.h"

#include <audio/audio.h>
#include <stdio.h>
#include <string.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include <spukit/spukit.h>

#include "platform/ps3/audio_spu.h"
#include "platform/ps3/log.h"
#include "platform/ps3/paths.h"
#include "rsp/audio/backend.h"

#define CFG_PATH  SALSA64_DATA_DIR "/audio.cfg"
#define CFG_MAX   1024
#define LEAD      4u        /* bloques llenos por delante del que suena */
#define STATS_US  5000000u
#define THREAD_PRIO 100     /* más urgente que el hilo principal (1001) */

static struct audio_sync sync_out __attribute__((aligned(128)));
static struct salsa64_audio_spu hle_spu;
static struct salsa64_audio_sync hle_ppe;
static struct aud_state hle_ppe_state;
static struct salsa64_n64 *machine;
static enum { HLE_NONE, HLE_SPE, HLE_PPE } hle;

static struct {
	bool open;
	u32 port;
	audioPortConfig config;
	sys_event_queue_t queue;
	sys_ipc_key_t key;
	sys_ppu_thread_t thread;
	volatile int quit;
	volatile uint64_t blocks;    /* bloques llenados */
	volatile uint64_t late;      /* veces que el puerto alcanzó al hilo */
	volatile uint64_t busy_us;   /* tiempo del hilo trabajando */
} port;

/* Para las estadísticas cada STATS_US. */
static struct {
	uint64_t t;
	uint32_t tasks, unknown_abi;
	uint64_t spu_ticks, wait_us, blocks, late, busy_us;
	struct audio_sync_stats out;
} prev;

/* ---- Configuración -------------------------------------------------------- */

struct audio_cfg {
	enum audio_policy policy;
	uint32_t level_ms;
	int hle;      /* HLE_* */
	bool output;
};

static void read_cfg(struct audio_cfg *c)
{
	static char text[CFG_MAX + 1];
	c->policy = AUDIO_STRETCH;
	c->level_ms = 60;
	c->hle = HLE_SPE;
	c->output = true;
	FILE *f = fopen(CFG_PATH, "r");
	if (!f)
		return;
	size_t len = fread(text, 1, CFG_MAX, f);
	fclose(f);
	text[len] = '\0';
	for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
		char key[32], val[32];
		if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2)
			continue;
		if (!strcmp(key, "politica"))
			c->policy = !strcmp(val, "silencio") ? AUDIO_SILENCE : AUDIO_STRETCH;
		else if (!strcmp(key, "nivel_ms"))
			sscanf(val, "%u", &c->level_ms);
		else if (!strcmp(key, "hle"))
			c->hle = !strcmp(val, "ppe") ? HLE_PPE : !strcmp(val, "no") ? HLE_NONE : HLE_SPE;
		else if (!strcmp(key, "salida"))
			c->output = strcmp(val, "no") != 0;
	}
	salsa64_log("audio: %s leído", CFG_PATH);
}

/* ---- Salida ------------------------------------------------------------------ */

static void ai_push(void *ctx, const uint8_t *p, uint32_t bytes, uint32_t rate, uint64_t cycle)
{
	(void)cycle;
	audio_sync_push(ctx, p, bytes, rate);
}

static float *block_ptr(uint32_t b)
{
	return (float *)(uintptr_t)port.config.audioDataStart + (size_t)b * 2 * AUDIO_BLOCK_SAMPLES;
}

/* Despierta con cada bloque que consume el puerto y llena los que faltan
 * hasta LEAD por delante del que suena. El índice de lectura del puerto
 * es el número de bloque (módulo numBlocks): se lleva una cuenta absoluta. */
static void audio_thread(void *arg)
{
	(void)arg;
	const uint32_t n = (uint32_t)port.config.numBlocks;
	uint64_t rabs = 0, next = 1;
	uint32_t last = 0;
	while (!port.quit) {
		sys_event_t ev;
		sysEventQueueReceive(port.queue, &ev, 100000);
		uint64_t t0 = sysGetSystemTime();
		uint32_t rmod = (uint32_t)(*(volatile uint64_t *)(uintptr_t)port.config.readIndex % n);
		rabs += (rmod + n - last) % n;
		last = rmod;
		if (next <= rabs) {
			port.late++;
			next = rabs + 1;
		}
		while (next <= rabs + LEAD) {
			audio_sync_pull(&sync_out, block_ptr((uint32_t)(next % n)), AUDIO_BLOCK_SAMPLES, sysGetSystemTime());
			next++;
			port.blocks++;
		}
		port.busy_us += sysGetSystemTime() - t0;
	}
	sysThreadExit(0);
}

static int open_port(void)
{
	audioPortParam params = { AUDIO_PORT_2CH, AUDIO_BLOCK_16, AUDIO_PORT_INITLEVEL, 1.0f };
	int rc;
	if ((rc = audioInit()) != 0) {
		salsa64_log("audio: audioInit falló (%d)", rc);
		return rc;
	}
	if ((rc = audioPortOpen(&params, &port.port)) != 0 ||
	    (rc = audioGetPortConfig(port.port, &port.config)) != 0) {
		salsa64_log("audio: no se abrió el puerto (%d)", rc);
		audioQuit();
		return rc;
	}
	if ((rc = audioCreateNotifyEventQueue(&port.queue, &port.key)) != 0 ||
	    (rc = audioSetNotifyEventQueue(port.key)) != 0) {
		salsa64_log("audio: sin cola de eventos (%d)", rc);
		audioPortClose(port.port);
		audioQuit();
		return rc;
	}
	sysEventQueueDrain(port.queue);
	memset(block_ptr(0), 0, (size_t)port.config.numBlocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
	port.quit = 0;
	if ((rc = audioPortStart(port.port)) != 0 ||
	    (rc = sysThreadCreate(&port.thread, audio_thread, NULL, THREAD_PRIO, 0x10000, THREAD_JOINABLE,
				  "salsa64_audio")) != 0) {
		salsa64_log("audio: no arrancó la salida (%d)", rc);
		audioPortStop(port.port);
		audioRemoveNotifyEventQueue(port.key);
		audioPortClose(port.port);
		sysEventQueueDestroy(port.queue, 0);
		audioQuit();
		return rc;
	}
	port.open = true;
	salsa64_log("audio: puerto %u abierto: %u canales, %u bloques de %u muestras a %u Hz", port.port,
		    (unsigned)port.config.channelCount, (unsigned)port.config.numBlocks, AUDIO_BLOCK_SAMPLES,
		    AUDIO_OUT_RATE);
	return 0;
}

/* ---- API --------------------------------------------------------------------- */

int audio_out_init(struct salsa64_n64 *n64)
{
	struct audio_cfg cfg;
	machine = n64;
	read_cfg(&cfg);
	audio_sync_init(&sync_out, cfg.policy, cfg.level_ms);

	hle = cfg.hle;
	if (hle == HLE_SPE) {
		int rc;
		spukit_init(6);
		if ((rc = salsa64_audio_spu_init(&hle_spu, &n64->bus)) != 0) {
			salsa64_log("audio: no arrancó el SPE de la HLE (%d): uso el PPE", rc);
			hle = HLE_PPE;
		}
	}
	if (hle == HLE_PPE)
		salsa64_audio_sync_init(&hle_ppe, &hle_ppe_state, &n64->bus);
	salsa64_rsp_set_audio(&n64->bus, hle == HLE_SPE ? &hle_spu.backend : hle == HLE_PPE ? &hle_ppe.backend : NULL);
	salsa64_ai_set_sink(&n64->bus, ai_push, &sync_out);
	salsa64_log("audio: HLE en %s, política %s, colchón %u ms",
		    hle == HLE_SPE ? "el SPE" : hle == HLE_PPE ? "el PPE" : "ninguna parte (sin HLE)",
		    cfg.policy == AUDIO_SILENCE ? "silencio" : "estirar", cfg.level_ms);
	memset(&prev, 0, sizeof prev);
	return cfg.output ? open_port() : -1;
}

const struct audio_sync_stats *audio_out_stats(void)
{
	return &sync_out.stats;
}

uint64_t audio_out_blocks(void)
{
	return port.blocks;
}

void audio_out_field(uint64_t now_us)
{
	if (!prev.t) {
		prev.t = now_us;
		return;
	}
	uint64_t dt = now_us - prev.t;
	if (dt < STATS_US)
		return;
	const struct salsa64_rsp_stats *rs = &machine->bus.rsp.stats;
	uint32_t tasks = rs->audio - prev.tasks;
	if (tasks) {
		char where[160] = "";
		if (hle == HLE_SPE) {
			uint32_t done = hle_spu.stats.tasks;
			static uint32_t done0;
			uint32_t n = done - done0 ? done - done0 : 1;
			done0 = done;
			snprintf(where, sizeof where, "; en el SPE %.3f ms por tarea (máx. %.3f), PPE esperando %.3f ms",
				 (hle_spu.spu_ticks - prev.spu_ticks) * 1e3 / spukit_tick_hz() / n,
				 hle_spu.spu_max * 1e3 / spukit_tick_hz(), (hle_spu.wait_us - prev.wait_us) / 1e3 / n);
			prev.spu_ticks = hle_spu.spu_ticks;
			prev.wait_us = hle_spu.wait_us;
		}
		const struct salsa64_audio_stats *as = hle == HLE_SPE ? &hle_spu.stats : &hle_ppe.stats;
		salsa64_log("audio: %u tareas (%.1f/s), ABI %s, %u con ABI desconocido; %u comandos sin "
			    "implementar (último 0x%08X), %u errores%s", tasks, tasks * 1e6 / dt,
			    salsa64_abi_name(machine->bus.rsp.abi), rs->unknown_abi - prev.unknown_abi, as->unknown,
			    as->last_unknown, as->errors, where);
	}
	prev.tasks = rs->audio;
	prev.unknown_abi = rs->unknown_abi;

	const struct audio_sync_stats *o = &sync_out.stats;
	uint64_t out = o->out_frames - prev.out.out_frames;
	if (port.open && out) {
		salsa64_log("audio: salida %.0f ms de colchón, emulación al %.0f %%, %s; en 5 s: %.0f %% en silencio, "
			    "%.0f %% estirado, %u faltas, %u silencios, %u saltos, %u descartadas, %u bloques tarde; "
			    "hilo %.2f %% del tiempo; del AI %u Hz",
			    audio_sync_level_ms(&sync_out), sync_out.speed_est * 100.0,
			    sync_out.stretching ? "estirando" : "normal",
			    100.0 * (o->silent_frames - prev.out.silent_frames) / out,
			    100.0 * (o->stretch_frames - prev.out.stretch_frames) / out,
			    o->underruns - prev.out.underruns, o->silences - prev.out.silences,
			    o->skips - prev.out.skips, o->dropped - prev.out.dropped,
			    (unsigned)(port.late - prev.late), 100.0 * (port.busy_us - prev.busy_us) / dt,
			    sync_out.rate);
	}
	prev.out = *o;
	prev.late = port.late;
	prev.busy_us = port.busy_us;
	prev.blocks = port.blocks;
	prev.t = now_us;
}

void audio_out_shutdown(void)
{
	if (!machine)
		return;
	salsa64_ai_set_sink(&machine->bus, NULL, NULL);
	salsa64_rsp_set_audio(&machine->bus, NULL);  /* espera la tarea en vuelo */
	if (hle == HLE_SPE)
		salsa64_audio_spu_shutdown(&hle_spu);
	if (port.open) {
		u64 rv;
		port.quit = 1;
		sysThreadJoin(port.thread, &rv);
		audioPortStop(port.port);
		audioRemoveNotifyEventQueue(port.key);
		audioPortClose(port.port);
		sysEventQueueDestroy(port.queue, 0);
		audioQuit();
		port.open = false;
	}
	salsa64_log("audio: %llu muestras del AI, %llu a la salida (%llu en silencio), %llu bloques",
		    (unsigned long long)sync_out.stats.in_frames, (unsigned long long)sync_out.stats.out_frames,
		    (unsigned long long)sync_out.stats.silent_frames, (unsigned long long)port.blocks);
	machine = NULL;
}
