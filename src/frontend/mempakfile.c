/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — archivo del Controller Pak. Ver mempakfile.h.
 */
#include "mempakfile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MEMPAK_QUIET_LOG_US 2000000u

/* Lectura al abrir (un byte de más para ver si el archivo es más grande). */
static uint8_t file_buf[SALSA64_MEMPAK_TOTAL + 1];

static inline void lwsync(void)
{
#ifdef __powerpc__
	__asm__ volatile("lwsync" ::: "memory");
#else
	__sync_synchronize();
#endif
}

static bool write_file(const char *path, const uint8_t *data, uint32_t len)
{
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	bool ok = fwrite(data, 1, len, out) == len;
	return fclose(out) == 0 && ok;
}

static long read_file(const char *path)
{
	FILE *in = fopen(path, "rb");
	if (!in)
		return -1;
	size_t n = fread(file_buf, 1, sizeof file_buf, in);
	fclose(in);
	return (long)n;
}

void mempakfile_open(struct mempakfile *f, struct salsa64_mempak *mp, struct salsa64_bus *bus,
		     const uint8_t *rom, const char *dir, unsigned ports, savefile_log_fn log)
{
	memset(f, 0, sizeof *f);
	f->mp = mp;
	f->bus = bus;
	f->log = log;
	salsa64_mempak_init(mp);
	bus->mempak = NULL;
	if (!(ports & ((1u << SALSA64_MEMPAK_PORTS) - 1))) {
		f->bus = NULL;
		return;
	}
	for (unsigned p = 0; p < SALSA64_MEMPAK_PORTS; p++)
		mp->present[p] = (ports >> p) & 1;
	bus->mempak = mp;
	if (!dir) {
		f->log("accesorio: Controller Pak (controles %02X), solo en memoria", ports & 0xF);
		return;
	}
	char name[64];
	salsa64_save_basename(rom, name, sizeof name);
	mkdir(dir, 0777);
	snprintf(f->path, sizeof f->path, "%s/%s.mpk", dir, name);
	snprintf(f->tmp, sizeof f->tmp, "%s.tmp", f->path);

	const char *from = f->path;
	long n = read_file(from);
	if (n < 0) {
		from = f->tmp;  /* se cortó una escritura entre borrar y renombrar */
		n = read_file(from);
	}
	if (n < 0)
		f->log("accesorio: Controller Pak (controles %02X); archivo nuevo %s", ports & 0xF, f->path);
	else if (salsa64_mempak_load(mp, file_buf, (uint32_t)n))
		f->log("accesorio: Controller Pak (controles %02X), leído de %s", ports & 0xF, from);
	else
		f->log("accesorio: %s mide %ld bytes y se esperaban %u; se usó lo que cabe", from, n,
		       SALSA64_MEMPAK_TOTAL);
}

static bool write_pak(struct mempakfile *f, const uint8_t *data, uint32_t writes, const char *why)
{
	bool ok = write_file(f->tmp, data, SALSA64_MEMPAK_TOTAL);
	if (ok) {
		remove(f->path);
		if (rename(f->tmp, f->path) != 0) {
			ok = write_file(f->path, data, SALSA64_MEMPAK_TOTAL);
			if (ok)
				remove(f->tmp);
		}
	}
	if (!ok) {
		if (!f->failed)
			f->log("accesorio: no se pudo escribir %s; se reintenta", f->path);
		f->failed = true;
		return false;
	}
	f->log("accesorio: escrito %s %s (%u escrituras del juego)", why, f->path, writes);
	f->failed = false;
	return true;
}

bool mempakfile_flush(struct mempakfile *f)
{
	struct salsa64_mempak *mp = f->mp;
	if (!f->path[0] || !salsa64_mempak_dirty(mp))
		return true;
	if (!write_pak(f, &mp->data[0][0], mp->writes - mp->flushed, "a pedido"))
		return false;
	salsa64_mempak_flushed(mp);
	return true;
}

/* Una línea de log con lo que el juego pidió al accesorio, cuando se calma
 * (2 s sin comandos nuevos) o al cerrar (`now_us` = ~0). Sirve para ver, en
 * una consola real, si el juego llega a hablar con el pak y cómo le va. */
static void log_traffic(struct mempakfile *f, uint64_t now_us)
{
	struct salsa64_mempak *mp = f->mp;
	uint32_t total = mp->cmd_status + mp->cmd_reads + mp->cmd_writes + mp->cmd_absent + mp->cmd_bad_addr;
	if (total == f->logged_total)
		return;
	if (total != f->seen_total) {
		f->seen_total = total;
		f->seen_us = now_us;
		if (now_us != ~0ull)
			return;
	}
	if (now_us != ~0ull && now_us - f->seen_us < MEMPAK_QUIET_LOG_US)
		return;
	f->log("accesorio: %u consultas de estado, %u lecturas, %u escrituras (%u bloques cambiados), "
	       "%u a un control sin pak, %u con la dirección mal verificada (la última 0x%04X)",
	       mp->cmd_status, mp->cmd_reads, mp->cmd_writes, mp->writes, mp->cmd_absent, mp->cmd_bad_addr,
	       mp->last_bad_addr);
	f->logged_total = total;
}

void mempakfile_stage(struct mempakfile *f, uint64_t now_us)
{
	struct salsa64_mempak *mp = f->mp;
	if (f->bus)
		log_traffic(f, now_us);
	if (!f->path[0] || f->pending || !salsa64_mempak_due(mp, now_us))
		return;
	memcpy(f->stage, mp->data, SALSA64_MEMPAK_TOTAL);
	f->stage_writes = mp->writes - mp->flushed;
	salsa64_mempak_flushed(mp);
	lwsync();  /* la copia, antes de anunciarla */
	f->pending = 1;
}

void mempakfile_write_staged(struct mempakfile *f, uint64_t now_us)
{
	if (!f->pending || now_us < f->retry_us)
		return;
	lwsync();  /* la copia, después de ver pending */
	if (!write_pak(f, f->stage, f->stage_writes, "diferido")) {
		f->retry_us = now_us + SAVEFILE_RETRY_US;
		return;
	}
	f->retry_us = 0;
	lwsync();
	f->pending = 0;
}

void mempakfile_poll(struct mempakfile *f, uint64_t now_us)
{
	mempakfile_stage(f, now_us);
	mempakfile_write_staged(f, now_us);
}

void mempakfile_close(struct mempakfile *f)
{
	if (!f->bus)
		return;  /* no se abrió, o sin pak */
	if (f->pending) {
		f->retry_us = 0;
		mempakfile_write_staged(f, ~0ull);
	}
	log_traffic(f, ~0ull);
	mempakfile_flush(f);
	f->bus->mempak = NULL;
	f->path[0] = '\0';
	f->bus = NULL;
}
