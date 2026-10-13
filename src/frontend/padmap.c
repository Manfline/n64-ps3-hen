/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — mapeo del DualShock 3 al control de N64. Ver padmap.h.
 */
#include "padmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEADZONE_MAX 100

static const char *const ds3_names[SALSA64_DS3_NBUTTONS] = {
	"CRUZ", "CIRCULO", "CUADRADO", "TRIANGULO", "L1", "R1", "L2", "R2",
	"L3", "R3", "START", "SELECT", "ARRIBA", "ABAJO", "IZQUIERDA", "DERECHA",
};

static const struct {
	const char *name;
	uint16_t bit;
} n64_inputs[SALSA64_IN_COUNT] = {
	[SALSA64_IN_A]       = { "A", SALSA64_PAD_A },
	[SALSA64_IN_B]       = { "B", SALSA64_PAD_B },
	[SALSA64_IN_Z]       = { "Z", SALSA64_PAD_Z },
	[SALSA64_IN_START]   = { "START", SALSA64_PAD_START },
	[SALSA64_IN_L]       = { "L", SALSA64_PAD_L },
	[SALSA64_IN_R]       = { "R", SALSA64_PAD_R },
	[SALSA64_IN_D_UP]    = { "ARRIBA", SALSA64_PAD_D_UP },
	[SALSA64_IN_D_DOWN]  = { "ABAJO", SALSA64_PAD_D_DOWN },
	[SALSA64_IN_D_LEFT]  = { "IZQUIERDA", SALSA64_PAD_D_LEFT },
	[SALSA64_IN_D_RIGHT] = { "DERECHA", SALSA64_PAD_D_RIGHT },
	[SALSA64_IN_C_UP]    = { "C_ARRIBA", SALSA64_PAD_C_UP },
	[SALSA64_IN_C_DOWN]  = { "C_ABAJO", SALSA64_PAD_C_DOWN },
	[SALSA64_IN_C_LEFT]  = { "C_IZQUIERDA", SALSA64_PAD_C_LEFT },
	[SALSA64_IN_C_RIGHT] = { "C_DERECHA", SALSA64_PAD_C_RIGHT },
};

void salsa64_padmap_default(struct salsa64_padmap *map)
{
	static const uint8_t def[SALSA64_IN_COUNT] = {
		[SALSA64_IN_A] = SALSA64_DS3_CROSS,
		[SALSA64_IN_B] = SALSA64_DS3_SQUARE,
		[SALSA64_IN_Z] = SALSA64_DS3_L2,
		[SALSA64_IN_START] = SALSA64_DS3_START,
		[SALSA64_IN_L] = SALSA64_DS3_L1,
		[SALSA64_IN_R] = SALSA64_DS3_R1,
		[SALSA64_IN_D_UP] = SALSA64_DS3_UP,
		[SALSA64_IN_D_DOWN] = SALSA64_DS3_DOWN,
		[SALSA64_IN_D_LEFT] = SALSA64_DS3_LEFT,
		[SALSA64_IN_D_RIGHT] = SALSA64_DS3_RIGHT,
		[SALSA64_IN_C_UP] = SALSA64_DS3_NONE,
		[SALSA64_IN_C_DOWN] = SALSA64_DS3_NONE,
		[SALSA64_IN_C_LEFT] = SALSA64_DS3_NONE,
		[SALSA64_IN_C_RIGHT] = SALSA64_DS3_NONE,
	};
	memcpy(map->source, def, sizeof def);
	memset(map->source2, SALSA64_DS3_NONE, sizeof map->source2);
	map->deadzone = 12;
	map->range = 80;      /* lo que da un control original a fondo */
	map->c_threshold = 64;
}

/* Un eje del stick izquierdo: 0–255 del DS3 → ±range, con zona muerta. */
static int8_t stick_axis(uint8_t raw, int invert, unsigned dz, unsigned range)
{
	int v = (int)raw - 128;
	if (invert)
		v = -v;
	int mag = v < 0 ? -v : v;
	if (mag > 127)
		mag = 127;
	if ((unsigned)mag <= dz)
		return 0;
	int span = 127 - (int)dz;
	int out = ((mag - (int)dz) * (int)range + span / 2) / span;
	return (int8_t)(v < 0 ? -out : out);
}

void salsa64_padmap_apply(const struct salsa64_padmap *map, const struct salsa64_ds3 *in,
			  struct salsa64_pad *out)
{
	uint16_t b = 0;
	for (int k = 0; k < SALSA64_IN_COUNT; k++) {
		uint8_t src = map->source[k], src2 = map->source2[k];
		if ((src < SALSA64_DS3_NBUTTONS && (in->buttons & (1u << src))) ||
		    (src2 < SALSA64_DS3_NBUTTONS && (in->buttons & (1u << src2))))
			b |= n64_inputs[k].bit;
	}
	int t = map->c_threshold, cx = (int)in->rx - 128, cy = (int)in->ry - 128;
	if (cx > t)
		b |= SALSA64_PAD_C_RIGHT;
	else if (cx < -t)
		b |= SALSA64_PAD_C_LEFT;
	if (cy > t)
		b |= SALSA64_PAD_C_DOWN;
	else if (cy < -t)
		b |= SALSA64_PAD_C_UP;

	unsigned dz = map->deadzone > DEADZONE_MAX ? DEADZONE_MAX : map->deadzone;
	out->buttons = b;
	out->x = stick_axis(in->lx, 0, dz, map->range);
	out->y = stick_axis(in->ly, 1, dz, map->range);
	out->connected = 1;
}

/* ---- Archivo de configuración ------------------------------------------- */

static int same_word(const char *a, size_t alen, const char *b)
{
	size_t blen = strlen(b);
	if (alen != blen)
		return 0;
	for (size_t i = 0; i < alen; i++) {
		char c = a[i];
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		if (c != b[i])
			return 0;
	}
	return 1;
}

static int is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r';
}

/* Recorta espacios a los dos lados de [*s, *s + *n). */
static void trim(const char **s, size_t *n)
{
	while (*n && is_space(**s))
		(*s)++, (*n)--;
	while (*n && is_space((*s)[*n - 1]))
		(*n)--;
}

static int parse_number(const char *s, size_t n, long lo, long hi, uint8_t *out)
{
	char tmp[16];
	if (n == 0 || n >= sizeof tmp)
		return 0;
	memcpy(tmp, s, n);
	tmp[n] = '\0';
	char *end;
	long v = strtol(tmp, &end, 10);
	if (*end || v < lo || v > hi)
		return 0;
	*out = (uint8_t)v;
	return 1;
}

/* Nombre de un botón del DS3 → índice, o -1. */
static int ds3_button(const char *s, size_t n)
{
	for (int d = 0; d < SALSA64_DS3_NBUTTONS; d++)
		if (same_word(s, n, ds3_names[d]))
			return d;
	return -1;
}

/* Uno o dos botones del DS3 separados por espacios o una coma
 * ("R2", "L2 R2", "L2, R2"). Devuelve cuántos leyó (0 si algo sobra o no
 * es un botón). */
static int parse_buttons(const char *s, size_t n, int out[2])
{
	int count = 0;
	size_t i = 0;
	while (i < n) {
		while (i < n && (is_space(s[i]) || s[i] == ','))
			i++;
		if (i == n)
			break;
		size_t start = i;
		while (i < n && !is_space(s[i]) && s[i] != ',')
			i++;
		if (count == 2)
			return 0;
		int d = ds3_button(s + start, i - start);
		if (d < 0)
			return 0;
		out[count++] = d;
	}
	return count;
}

/* Una línea "clave = valor" ya sin comentarios ni espacios a los lados. */
static int parse_line(struct salsa64_padmap *map, const char *line, size_t n)
{
	const char *eq = memchr(line, '=', n);
	if (!eq)
		return 0;
	const char *key = line, *val = eq + 1;
	size_t klen = (size_t)(eq - line), vlen = n - klen - 1;
	trim(&key, &klen);
	trim(&val, &vlen);

	if (same_word(key, klen, "ZONA_MUERTA"))
		return parse_number(val, vlen, 0, DEADZONE_MAX, &map->deadzone);
	if (same_word(key, klen, "RANGO"))
		return parse_number(val, vlen, 1, 127, &map->range);
	if (same_word(key, klen, "UMBRAL_C"))
		return parse_number(val, vlen, 1, 127, &map->c_threshold);

	for (int k = 0; k < SALSA64_IN_COUNT; k++) {
		if (!same_word(key, klen, n64_inputs[k].name))
			continue;
		if (same_word(val, vlen, "NADA")) {
			map->source[k] = SALSA64_DS3_NONE;
			map->source2[k] = SALSA64_DS3_NONE;
			return 1;
		}
		int d[2];
		int count = parse_buttons(val, vlen, d);
		if (count == 0)
			return 0;
		map->source[k] = (uint8_t)d[0];
		map->source2[k] = count == 2 ? (uint8_t)d[1] : SALSA64_DS3_NONE;
		return 1;
	}
	return 0;
}

int salsa64_padmap_parse(struct salsa64_padmap *map, const char *text, size_t len,
			 char *err, size_t err_size)
{
	int bad = 0;
	unsigned lineno = 0;
	size_t i = 0;
	if (err && err_size)
		err[0] = '\0';
	while (i < len) {
		const char *line = text + i;
		const char *nl = memchr(line, '\n', len - i);
		size_t n = nl ? (size_t)(nl - line) : len - i;
		i += n + 1;
		lineno++;
		const char *hash = memchr(line, '#', n);
		if (hash)
			n = (size_t)(hash - line);
		trim(&line, &n);
		if (n == 0 || parse_line(map, line, n))
			continue;
		if (!bad && err && err_size)
			snprintf(err, err_size, "línea %u: \"%.*s\"", lineno, (int)(n > 40 ? 40 : n), line);
		bad++;
	}
	return bad;
}

void salsa64_pad_describe(const struct salsa64_pad *pad, char *buf, size_t size)
{
	static const struct {
		const char *name;
		uint16_t bit;
	} names[] = {
		{ "A", SALSA64_PAD_A }, { "B", SALSA64_PAD_B }, { "Z", SALSA64_PAD_Z },
		{ "START", SALSA64_PAD_START }, { "L", SALSA64_PAD_L }, { "R", SALSA64_PAD_R },
		{ "ARRIBA", SALSA64_PAD_D_UP }, { "ABAJO", SALSA64_PAD_D_DOWN },
		{ "IZQUIERDA", SALSA64_PAD_D_LEFT }, { "DERECHA", SALSA64_PAD_D_RIGHT },
		{ "C_ARRIBA", SALSA64_PAD_C_UP }, { "C_ABAJO", SALSA64_PAD_C_DOWN },
		{ "C_IZQUIERDA", SALSA64_PAD_C_LEFT }, { "C_DERECHA", SALSA64_PAD_C_RIGHT },
	};
	size_t used = 0;
	if (size == 0)
		return;
	buf[0] = '\0';
	for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
		if (!(pad->buttons & names[k].bit))
			continue;
		int w = snprintf(buf + used, size - used, "%s ", names[k].name);
		if (w < 0 || (size_t)w >= size - used)
			return;
		used += (size_t)w;
	}
	snprintf(buf + used, size - used, "stick=(%d,%d)", pad->x, pad->y);
}
