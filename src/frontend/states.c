/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — estados guardados en la PS3. Ver states.h.
 */
#include "states.h"

#include <malloc.h>
#include <spukit/lzstream.h>
#include <spukit/spukit.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/systime.h>

#include "platform/ps3/bgjob.h"
#include "platform/ps3/log.h"
#include "spu_lz_bin.h"

static uint8_t scratch[SALSA64_STATE_SCRATCH] __attribute__((aligned(128)));
static uint16_t thumb_px[SALSA64_STATE_THUMB_W * SALSA64_STATE_THUMB_H];
static bool thumb_ok;
static uint64_t thumb_us;

static inline void lwsync(void) { __asm__ volatile("lwsync" ::: "memory"); }

/* ---- E/S del archivo ------------------------------------------------------------ */

/* Las escrituras se juntan en un búfer: el flujo comprimido llega en
 * pedazos de 4 bytes (encabezados) y de hasta 64 KB. */
#define WBUF_BYTES (128u << 10)
static uint8_t wbuf[WBUF_BYTES] __attribute__((aligned(128)));

struct fsio {
	s32 fd;
	uint64_t us;    /* tiempo en el sistema de archivos */
	uint32_t bytes;
	uint32_t fill;  /* bytes en wbuf (solo al escribir) */
};

static bool fs_flush(struct fsio *io)
{
	if (!io->fill)
		return true;
	uint64_t t0 = sysGetSystemTime(), done = 0;
	bool ok = sysLv2FsWrite(io->fd, wbuf, io->fill, &done) == 0 && done == io->fill;
	io->us += sysGetSystemTime() - t0;
	io->fill = 0;
	return ok;
}

static bool fs_write(void *ctx, const void *data, uint32_t len)
{
	struct fsio *io = ctx;
	const uint8_t *p = data;
	io->bytes += len;
	while (len) {
		if (!io->fill && len >= WBUF_BYTES / 2) {
			/* Grande: directo, sin pasar por el búfer. */
			uint64_t t0 = sysGetSystemTime(), done = 0;
			bool ok = sysLv2FsWrite(io->fd, p, len, &done) == 0 && done == len;
			io->us += sysGetSystemTime() - t0;
			return ok;
		}
		uint32_t n = WBUF_BYTES - io->fill < len ? WBUF_BYTES - io->fill : len;
		memcpy(wbuf + io->fill, p, n);
		io->fill += n;
		p += n;
		len -= n;
		if (io->fill == WBUF_BYTES && !fs_flush(io))
			return false;
	}
	return true;
}

/* Escribe `len` bytes en la posición absoluta `offset` y deja el archivo ahí. */
static bool fs_patch(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len)
{
	struct fsio *io = ctx;
	if (!fs_flush(io))
		return false;
	uint64_t t0 = sysGetSystemTime(), pos = 0, done = 0;
	bool ok = sysLv2FsLSeek64(io->fd, offset, 0, &pos) == 0 && pos == offset &&
		  sysLv2FsWrite(io->fd, data, len, &done) == 0 && done == len;
	io->us += sysGetSystemTime() - t0;
	return ok;
}

static bool fs_read(void *ctx, void *data, uint32_t len)
{
	struct fsio *io = ctx;
	uint64_t t0 = sysGetSystemTime(), done = 0;
	bool ok = sysLv2FsRead(io->fd, data, len, &done) == 0 && done == len;
	io->us += sysGetSystemTime() - t0;
	io->bytes += len;
	return ok;
}

static void slot_path(const struct salsa64_bus *bus, unsigned slot, char *buf, size_t n)
{
	char name[64];
	salsa64_state_filename(bus->rom, slot, name, sizeof name);
	snprintf(buf, n, "%s/%s", STATES_DIR, name);
}

/* ---- Escritura en segundo plano ------------------------------------------------- */

static struct {
	bool ready;                      /* foto reservada (states_init) */
	uint8_t *small, *ram;
	uint32_t ram_cap;
	struct salsa64_state_snap snap;
	char path[128], tmp[136];
	unsigned slot;
	uint64_t t_submit;
	volatile int pending;            /* hay una escritura pedida y no recogida por states_poll */
	volatile int finished;           /* el trabajo terminó: res vale */
	struct states_done res;
	spukit_lzs *lzs;                 /* el compresor (un SPE, o el PPE si no se pudo) */
	bool lz_tried;
} bg;

/* Un sumidero que junta lo que llega en un búfer. */
struct col {
	uint8_t *at;
	uint32_t n, cap;
};

static int col_sink(void *ctx, const void *d, uint32_t len)
{
	struct col *c = ctx;
	if (len > c->cap - c->n)
		return -1;
	memcpy(c->at + c->n, d, len);
	c->n += len;
	return 0;
}

/* Bloques de prueba del compresor: lo que da el SPE tiene que ser, byte a
 * byte, lo que da el PPE, o se comprime en el PPE. */
static bool lz_selftest(spukit_lzs *z)
{
	enum { LEN = SPUKIT_LZ_BLOCK + 30000, OUT = 2 * (SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK) + 64) };
	uint8_t *buf = memalign(128, ((LEN + 127) & ~127u) + 2 * OUT);
	if (!buf)
		return false;
	uint8_t *got = buf + ((LEN + 127) & ~127u), *ref = got + OUT;
	uint32_t x = 12345;
	for (uint32_t i = 0; i < LEN; i++) {
		x = x * 1664525u + 1013904223u;
		buf[i] = (i / 4096) % 3 == 0 ? (uint8_t)(x >> 16) : (i / 4096) % 3 == 1 ? 0 : (uint8_t)("salsa64"[i % 7] + (x >> 29));
	}
	struct col col = { got, 0, OUT };
	bool ok = spukit_lzs_run(z, buf, LEN, col_sink, &col, NULL) == 0;
	/* Referencia: los mismos bloques en el PPE. */
	uint32_t rn = 0;
	for (uint32_t off = 0; ok && off < LEN; off += SPUKIT_LZ_BLOCK) {
		uint32_t n = LEN - off < SPUKIT_LZ_BLOCK ? LEN - off : SPUKIT_LZ_BLOCK;
		uint32_t c = spukit_lzs_ppe_block(z, buf + off, n, ref + rn + 4);
		spukit_lz_frame(ref + rn, c ? c : n, c == 0);
		if (!c)
			memcpy(ref + rn + 4, buf + off, n);
		rn += 4 + (c ? c : n);
	}
	ok = ok && rn == col.n && memcmp(got, ref, rn) == 0;
	free(buf);
	return ok;
}

static void lz_open(void)
{
	if (bg.lz_tried)
		return;
	bg.lz_tried = true;
	uint64_t t0 = sysGetSystemTime();
	spukit_init(6);
	spukit_lzs *z = spukit_lzs_open(spu_lz_bin);
	const char *how = "en un SPE";
	if (z && spukit_lzs_has_spu(z) && !lz_selftest(z)) {
		salsa64_log("estados: el compresor del SPE no da lo mismo que el del PPE: se comprime en el PPE");
		spukit_lzs_close(z);
		z = NULL;
	}
	if (z && !spukit_lzs_has_spu(z))
		how = "en el PPE (el SPE no arrancó)";
	if (!z) {
		z = spukit_lzs_open(NULL);
		how = "en el PPE";
	}
	bg.lzs = z;
	salsa64_log("estados: compresor LZ %s (%.1f ms con la prueba)", how, (sysGetSystemTime() - t0) / 1e3);
}

struct lz_cb {
	spukit_lzs *z;
	spukit_lzs_stats st;
	salsa64_state_write_fn write;
	void *wctx;
};

static int lz_sink(void *ctx, const void *d, uint32_t len)
{
	struct lz_cb *c = ctx;
	return c->write(c->wctx, d, len) ? 0 : -1;
}

static bool lz_compress_cb(void *cctx, const uint8_t *src, uint32_t len, salsa64_state_write_fn write, void *wctx)
{
	struct lz_cb *c = cctx;
	c->write = write;
	c->wctx = wctx;
	return spukit_lzs_run(c->z, src, len, lz_sink, c, &c->st) == 0;
}

/* El trabajo del hilo de escritura: comprime la RDRAM de la foto y escribe
 * el archivo (a un .tmp que se renombra al final: un corte a mitad no pisa
 * el estado anterior). */
static void bg_write(void *arg)
{
	(void)arg;
	struct states_done *r = &bg.res;
	uint64_t t0 = sysGetSystemTime();
	memset(r, 0, sizeof *r);
	r->slot = bg.slot;
	lz_open();
	sysLv2FsMkdir(STATES_DIR, 0777);  /* si ya existe, da error y no pasa nada */

	struct fsio io = { -1, 0, 0, 0 };
	struct lz_cb c;
	memset(&c, 0, sizeof c);
	c.z = bg.lzs;
	bool ok = sysLv2FsOpen(bg.tmp, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &io.fd, 0666, NULL, 0) == 0;
	if (!ok) {
		snprintf(r->err, sizeof r->err, "no se pudo crear %s", bg.tmp);
	} else {
		ok = salsa64_state_write(&bg.snap, lz_compress_cb, &c, fs_write, fs_patch, &io, r->err, sizeof r->err);
		if (ok && !fs_flush(&io)) {
			snprintf(r->err, sizeof r->err, "no se pudo escribir el archivo");
			ok = false;
		}
		uint64_t tc = sysGetSystemTime();
		if (sysLv2FsClose(io.fd) != 0 && ok) {
			snprintf(r->err, sizeof r->err, "no se pudo cerrar el archivo");
			ok = false;
		}
		io.us += sysGetSystemTime() - tc;
	}
	if (ok) {
		uint64_t tr = sysGetSystemTime();
		sysLv2FsChmod(bg.tmp, 0666);
		if (sysLv2FsRename(bg.tmp, bg.path) != 0) {
			/* Sin reemplazo atómico: quitar el viejo y volver a intentar. */
			sysLv2FsUnlink(bg.path);
			if (sysLv2FsRename(bg.tmp, bg.path) != 0) {
				snprintf(r->err, sizeof r->err, "no se pudo renombrar %s", bg.tmp);
				ok = false;
			}
		}
		io.us += sysGetSystemTime() - tr;
	}
	if (!ok && io.fd >= 0)
		sysLv2FsUnlink(bg.tmp);
	r->ok = ok;
	r->us_io = io.us;
	r->us_spu = spukit_tick_hz() ? c.st.spu_ticks * 1000000ull / spukit_tick_hz() : 0;
	r->bytes = io.bytes;
	r->raw_bytes = bg.snap.small_len + bg.snap.ram_len;
	r->blocks = c.st.blocks;
	r->stored = c.st.stored;
	r->spu_blocks = c.st.spu_blocks;
	r->spu = spukit_lzs_has_spu(bg.lzs);
	r->us_total = sysGetSystemTime() - bg.t_submit;
	(void)t0;
	lwsync();
	bg.finished = 1;
}

bool states_init(uint32_t rdram_size)
{
	if (bg.ready)
		return true;
	bg.ram_cap = (rdram_size + 127u) & ~127u;
	bg.small = memalign(128, SALSA64_STATE_SECTIONS);
	bg.ram = memalign(128, bg.ram_cap);
	if (!bg.small || !bg.ram) {
		free(bg.small);
		free(bg.ram);
		bg.small = bg.ram = NULL;
		salsa64_log("estados: sin memoria para la foto (%u KB): se guarda todo seguido, sin comprimir",
			    (bg.ram_cap + SALSA64_STATE_SECTIONS) >> 10);
		return false;
	}
	if (!bgjob_init())
		salsa64_log("estados: sin hilo de escritura: se guarda todo seguido (comprimido)");
	bg.ready = true;
	salsa64_log("estados: foto de %u KB reservada; escritura en segundo plano %s", (bg.ram_cap + SALSA64_STATE_SECTIONS) >> 10,
		    bgjob_ready() ? "con un hilo propio" : "no disponible");
	return true;
}

void states_shutdown(uint64_t wait_us)
{
	if (bg.pending && bg.finished) {
		salsa64_log("estado: la escritura de la ranura %u terminó %s al cerrar%s%s", bg.slot,
			    bg.res.ok ? "bien" : "con error", bg.res.ok ? "" : ": ", bg.res.ok ? "" : bg.res.err);
		bg.pending = 0;
		bg.finished = 0;
	}
	if (bgjob_ready()) {
		if (!bgjob_wait(wait_us)) {
			salsa64_log("estados: la escritura en segundo plano no terminó en %.1f s: se sigue sin esperarla",
				    wait_us / 1e6);
			return;
		}
		bgjob_shutdown();
	}
	if (bg.lzs) {
		spukit_lzs_close(bg.lzs);
		bg.lzs = NULL;
	}
	bg.lz_tried = false;
}

bool states_busy(void)
{
	return bg.pending && !bg.finished;
}

bool states_poll(struct states_done *d)
{
	if (!bg.pending || !bg.finished)
		return false;
	lwsync();
	*d = bg.res;
	bg.pending = 0;
	bg.finished = 0;
	return true;
}

/* Espera a que no haya escritura en marcha. false si pasó el plazo. */
static bool bg_idle(uint64_t timeout_us)
{
	if (!bg.pending)
		return true;
	uint64_t t0 = sysGetSystemTime();
	while (!bg.finished) {
		if (timeout_us && sysGetSystemTime() - t0 >= timeout_us)
			return false;
		sysUsleep(1000);
	}
	lwsync();
	return true;
}

bool states_slot_exists(const struct salsa64_bus *bus, unsigned slot)
{
	char path[128];
	sysFSStat st;
	if (!bus->rom)
		return false;
	if (bg.pending && !bg.finished && bg.slot == slot)
		return true;  /* se está escribiendo ahora mismo */
	slot_path(bus, slot, path, sizeof path);
	return sysLv2FsStat(path, &st) == 0 && st.st_size >= SALSA64_STATE_HEADER;
}

/* Sin foto: todo aquí y ahora, la RDRAM sin comprimir, como antes. */
static bool save_sync(struct salsa64_n64 *n64, const char *path, const char *tmp, struct states_timing *t,
		      char *err, size_t err_size, uint64_t t0)
{
	sysLv2FsMkdir(STATES_DIR, 0777);
	struct fsio io = { -1, 0, 0, 0 };
	if (sysLv2FsOpen(tmp, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &io.fd, 0666, NULL, 0) != 0) {
		snprintf(err, err_size, "no se pudo crear %s", tmp);
		return false;
	}
	const struct salsa64_state_thumb th = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, thumb_px };
	bool ok = salsa64_state_save(n64, thumb_ok ? &th : NULL, fs_write, &io, scratch, err, err_size) && fs_flush(&io);
	uint64_t tc = sysGetSystemTime();
	if (sysLv2FsClose(io.fd) != 0 && ok) {
		snprintf(err, err_size, "no se pudo cerrar el archivo");
		ok = false;
	}
	io.us += sysGetSystemTime() - tc;
	if (!ok) {
		sysLv2FsUnlink(tmp);
		return false;
	}
	sysLv2FsChmod(tmp, 0666);
	if (sysLv2FsRename(tmp, path) != 0) {
		sysLv2FsUnlink(path);
		if (sysLv2FsRename(tmp, path) != 0) {
			snprintf(err, err_size, "no se pudo renombrar %s", tmp);
			return false;
		}
	}
	t->us_io = io.us;
	t->bytes = io.bytes;
	t->us_total = sysGetSystemTime() - t0;
	return true;
}

bool states_save(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size)
{
	uint64_t t0 = sysGetSystemTime();
	char path[128], tmp[136];
	memset(t, 0, sizeof *t);
	slot_path(&n64->bus, slot, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if (!bg.ready)
		return save_sync(n64, path, tmp, t, err, err_size, t0);

	/* La escritura anterior tiene que haber terminado: la foto es una sola. */
	if (!bg_idle(5000000)) {
		snprintf(err, err_size, "el estado anterior sigue escribiéndose");
		return false;
	}
	if (bg.pending && bg.finished) {
		/* Terminó y nadie la recogió: se cuenta aquí. */
		salsa64_log("estado: la escritura anterior (ranura %u) terminó %s", bg.slot, bg.res.ok ? "bien" : "con error");
		bg.pending = 0;
		bg.finished = 0;
	}
	const struct salsa64_state_thumb th = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, thumb_px };
	bg.snap = (struct salsa64_state_snap){ bg.small, bg.ram, 0, 0 };
	if (!salsa64_state_capture(n64, thumb_ok ? &th : NULL, &bg.snap, SALSA64_STATE_SECTIONS, bg.ram_cap, err,
				   err_size))
		return false;
	memcpy(bg.path, path, sizeof bg.path);
	memcpy(bg.tmp, tmp, sizeof bg.tmp);
	bg.slot = slot;
	bg.t_submit = t0;
	bg.finished = 0;
	bg.pending = 1;
	lwsync();
	t->async = true;
	if (!bgjob_submit(bg_write, NULL)) {
		/* Sin hilo: aquí mismo (más largo, pero guarda). */
		bg_write(NULL);
		t->async = false;
	}
	t->us_total = sysGetSystemTime() - t0;
	return true;
}

bool states_load(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size,
		 bool *broken)
{
	char path[128];
	memset(t, 0, sizeof *t);
	*broken = false;
	/* Cargar lo que se está escribiendo: esperar a que esté en el disco. */
	if (!bg_idle(10000000)) {
		snprintf(err, err_size, "el estado sigue escribiéndose");
		return false;
	}
	uint64_t t0 = sysGetSystemTime();
	slot_path(&n64->bus, slot, path, sizeof path);
	struct fsio io = { -1, 0, 0, 0 };
	if (sysLv2FsOpen(path, SYS_O_RDONLY, &io.fd, 0, NULL, 0) != 0) {
		snprintf(err, err_size, "no hay estado");
		return false;
	}
	bool ok = salsa64_state_load(n64, fs_read, &io, scratch, err, err_size, broken);
	sysLv2FsClose(io.fd);
	t->us_io = io.us;
	t->bytes = io.bytes;
	t->us_total = sysGetSystemTime() - t0;
	return ok;
}

void states_thumb_capture(const rsxkit_rect *r)
{
	uint32_t pitch;
	uint64_t t0 = sysGetSystemTime();
	const uint32_t *px = rsxkit_back_buffer(&pitch);
	thumb_ok = false;
	thumb_us = 0;
	if (!px || !r->w || !r->h)
		return;
	/* Una lectura por píxel de la miniatura: leer la memoria del RSX desde
	 * el PPE es lento, así que nada de promediar (19 200 lecturas). */
	for (uint32_t y = 0; y < SALSA64_STATE_THUMB_H; y++) {
		uint32_t sy = (uint32_t)r->y + (uint32_t)((2 * y + 1) * (uint32_t)r->h / (2 * SALSA64_STATE_THUMB_H));
		const uint32_t *line = px + sy * (pitch / 4);
		for (uint32_t x = 0; x < SALSA64_STATE_THUMB_W; x++) {
			uint32_t sx = (uint32_t)r->x + (uint32_t)((2 * x + 1) * (uint32_t)r->w / (2 * SALSA64_STATE_THUMB_W));
			uint32_t c = line[sx];
			thumb_px[y * SALSA64_STATE_THUMB_W + x] =
				(uint16_t)(((c >> 19) & 31) << 11 | ((c >> 11) & 31) << 6 | ((c >> 3) & 31) << 1 | 1);
		}
	}
	thumb_ok = true;
	thumb_us = sysGetSystemTime() - t0;
}

void states_thumb_clear(void)
{
	thumb_ok = false;
}

uint64_t states_thumb_us(void)
{
	return thumb_us;
}
