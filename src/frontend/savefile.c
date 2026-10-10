/*
 * Salsa64 — archivos de guardado del cartucho. Ver savefile.h.
 */
#include "savefile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CFG_MAX 4096

/* Lectura al abrir: un archivo de guardado o guardado.cfg. */
static uint8_t file_buf[SALSA64_SAVE_MAX + 1];

static long read_file(const char *path, uint8_t *buf, size_t size)
{
	FILE *in = fopen(path, "rb");
	if (!in)
		return -1;
	size_t n = fread(buf, 1, size, in);
	fclose(in);
	return (long)n;
}

static bool write_file(const char *path, const uint8_t *data, uint32_t len)
{
	FILE *out = fopen(path, "wb");
	if (!out)
		return false;
	bool ok = fwrite(data, 1, len, out) == len;
	return fclose(out) == 0 && ok;
}

/* Ajuste manual de guardado.cfg para esta ROM, o SALSA64_SAVE_AUTO. */
static int config_type(struct savefile *f, const uint8_t *rom, const char *cfg_path)
{
	if (!cfg_path)
		return SALSA64_SAVE_AUTO;
	long n = read_file(cfg_path, file_buf, CFG_MAX);
	if (n < 0)
		return SALSA64_SAVE_AUTO;
	int bad;
	char err[96];
	int type = salsa64_save_parse_config((const char *)file_buf, (size_t)n, rom, &bad, err, sizeof err);
	if (bad)
		f->log("guardado: %s con %d líneas que no entendí; la primera, %s", cfg_path, bad, err);
	return type;
}

enum salsa64_save_type savefile_open(struct savefile *f, struct salsa64_save *save, struct salsa64_bus *bus,
				     const uint8_t *rom, const char *dir, const char *cfg_path, int manual,
				     savefile_log_fn log)
{
	memset(f, 0, sizeof *f);
	f->save = save;
	f->bus = bus;
	f->log = log;
	if (manual == SALSA64_SAVE_AUTO)
		manual = config_type(f, rom, cfg_path);

	enum salsa64_save_source src;
	enum salsa64_save_type type = salsa64_save_detect(rom, manual, &src);
	salsa64_save_init(save, type);
	salsa64_save_attach(bus, NULL);
	if (src == SALSA64_SAVE_FROM_NONE) {
		f->log("guardado: no conozco el tipo de %.4s (versión %u, CRC1 %02X%02X%02X%02X): sin guardado; "
		       "se puede fijar en guardado.cfg", (const char *)rom + 0x3B, rom[0x3F],
		       rom[0x10], rom[0x11], rom[0x12], rom[0x13]);
		return SALSA64_SAVE_NONE;
	}
	if (save->type != type) {
		f->log("guardado: %s (%s) todavía no se emula: sin guardado", salsa64_save_name(type),
		       salsa64_save_source_name(src));
		return SALSA64_SAVE_NONE;
	}
	if (type == SALSA64_SAVE_NONE) {
		f->log("guardado: ninguno (%s)", salsa64_save_source_name(src));
		return SALSA64_SAVE_NONE;
	}

	salsa64_save_attach(bus, save);
	if (!dir) {
		f->log("guardado: %s (%s), %u bytes, solo en memoria", salsa64_save_name(type),
		       salsa64_save_source_name(src), save->size);
		return type;
	}
	char name[64];
	salsa64_save_filename(rom, type, name, sizeof name);
	mkdir(dir, 0777);
	snprintf(f->path, sizeof f->path, "%s/%s", dir, name);
	snprintf(f->tmp, sizeof f->tmp, "%s.tmp", f->path);

	const char *from = f->path;
	long n = read_file(from, file_buf, sizeof file_buf);
	if (n < 0) {
		from = f->tmp;  /* se cortó una escritura entre borrar y renombrar */
		n = read_file(from, file_buf, sizeof file_buf);
	}
	if (n < 0) {
		f->log("guardado: %s (%s), %u bytes; archivo nuevo %s", salsa64_save_name(type),
		       salsa64_save_source_name(src), save->size, f->path);
	} else if (salsa64_save_load(save, file_buf, (uint32_t)n)) {
		f->log("guardado: %s (%s), %u bytes leídos de %s", salsa64_save_name(type),
		       salsa64_save_source_name(src), save->size, from);
	} else {
		f->log("guardado: %s (%s): %s mide %ld bytes y se esperaban %u; se usó lo que cabe",
		       salsa64_save_name(type), salsa64_save_source_name(src), from, n, save->size);
	}
	return type;
}

bool savefile_flush(struct savefile *f)
{
	struct salsa64_save *s = f->save;
	if (!f->path[0] || !salsa64_save_dirty(s))
		return true;
	bool ok = write_file(f->tmp, s->data, s->size);
	if (ok) {
		remove(f->path);
		if (rename(f->tmp, f->path) != 0) {
			/* Sin rename: directo (no es atómico, pero no se pierde). */
			ok = write_file(f->path, s->data, s->size);
			if (ok)
				remove(f->tmp);
		}
	}
	if (!ok) {
		if (!f->failed)
			f->log("guardado: no se pudo escribir %s; se reintenta", f->path);
		f->failed = true;
		return false;
	}
	f->log("guardado: escrito %s (%u escrituras del juego)", f->path, s->writes - s->flushed);
	f->failed = false;
	salsa64_save_flushed(s);
	return true;
}

void savefile_poll(struct savefile *f, uint64_t now_us)
{
	if (!f->path[0] || !salsa64_save_due(f->save, now_us) || now_us < f->retry_us)
		return;
	if (!savefile_flush(f))
		f->retry_us = now_us + SAVEFILE_RETRY_US;
}

void savefile_close(struct savefile *f)
{
	if (!f->bus)
		return;  /* no se abrió */
	savefile_flush(f);
	salsa64_save_attach(f->bus, NULL);
	f->path[0] = '\0';
}
