/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — estados guardados en la PS3. Ver states.h.
 */
#include "states.h"

#include <sys/file.h>
#include <stdio.h>
#include <string.h>
#include <sys/systime.h>

static uint8_t scratch[SALSA64_STATE_SCRATCH] __attribute__((aligned(128)));
static uint16_t thumb_px[SALSA64_STATE_THUMB_W * SALSA64_STATE_THUMB_H];
static bool thumb_ok;
static uint64_t thumb_us;

struct fsio {
	s32 fd;
	uint64_t us;    /* tiempo en el sistema de archivos */
	uint32_t bytes;
};

static bool fs_write(void *ctx, const void *data, uint32_t len)
{
	struct fsio *io = ctx;
	uint64_t t0 = sysGetSystemTime(), done = 0;
	bool ok = sysLv2FsWrite(io->fd, data, len, &done) == 0 && done == len;
	io->us += sysGetSystemTime() - t0;
	io->bytes += len;
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

bool states_slot_exists(const struct salsa64_bus *bus, unsigned slot)
{
	char path[128];
	sysFSStat st;
	if (!bus->rom)
		return false;
	slot_path(bus, slot, path, sizeof path);
	return sysLv2FsStat(path, &st) == 0 && st.st_size >= SALSA64_STATE_HEADER;
}

bool states_save(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size)
{
	char path[128], tmp[136];
	uint64_t t0 = sysGetSystemTime();
	memset(t, 0, sizeof *t);
	slot_path(&n64->bus, slot, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	sysLv2FsMkdir(STATES_DIR, 0777);  /* si ya existe, da error y no pasa nada */

	struct fsio io = { -1, 0, 0 };
	if (sysLv2FsOpen(tmp, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &io.fd, 0666, NULL, 0) != 0) {
		snprintf(err, err_size, "no se pudo crear %s", tmp);
		return false;
	}
	const struct salsa64_state_thumb th = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, thumb_px };
	bool ok = salsa64_state_save(n64, thumb_ok ? &th : NULL, fs_write, &io, scratch, err, err_size);
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
		/* Sin reemplazo atómico: quitar el viejo y volver a intentar. */
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

bool states_load(struct salsa64_n64 *n64, unsigned slot, struct states_timing *t, char *err, size_t err_size,
		 bool *broken)
{
	char path[128];
	uint64_t t0 = sysGetSystemTime();
	memset(t, 0, sizeof *t);
	*broken = false;
	slot_path(&n64->bus, slot, path, sizeof path);
	struct fsio io = { -1, 0, 0 };
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
