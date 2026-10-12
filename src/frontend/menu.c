/*
 * Salsa64 — menú para elegir ROM. Ver menu.h.
 *
 * Todo en un espacio lógico de lw × 720 (lw = 1280 con tele 16:9, 960 con
 * 4:3) que rsxkit estira a la pantalla. Las posiciones se dan como
 * fracciones del alto y del ancho para que la misma disposición sirva a las
 * dos proporciones.
 */
#include "menu.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <rsxkit/text.h>
#include <rsxkit/ui.h>

#include "core/rom.h"
#include "core/save.h"
#include "padmap.h"

#define BIT(b) (1u << (b))
#define DIRS   (BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT))
#define TAU    6.28318530718f

/* Colores 0xRRGGBB. */
#define C_TEXT   0xF2F4F8u
#define C_DIM    0xA8B4C4u
#define C_ACCENT 0x9CC8FFu
#define C_WARN   0xFFC070u

/* Columna: fila elegida a COL_Y del alto; la vecina a GAP, las demás cada
 * SPACING (fracciones del alto). */
#define COL_Y   0.42f
#define GAP     0.135f
#define SPACING 0.074f

/* ---- Lógica ---------------------------------------------------------------- */

void menu_settings_default(struct menu_settings *s)
{
	s->waves = true;
	s->overscan = MENU_OVERSCAN_NORMAL;
	s->resolution = MENU_RES_720P;
	s->scale = 0;
	s->fps = false;
}

static const char *overscan_name(int o)
{
	switch (o) {
	case MENU_OVERSCAN_OFF:    return "Apagado";
	case MENU_OVERSCAN_NORMAL: return "Normal";
	case MENU_OVERSCAN_WIDE:   return "Amplio";
	default:                   return "Personalizado";
	}
}

static const char *resolution_name(int r)
{
	return r == MENU_RES_1080P ? "1080p" : "720p";
}

/* «Auto (3×)», «1× nativa», «4×»... */
static const char *scale_name(const struct menu_settings *s, char *buf, size_t n)
{
	if (s->scale == 0)
		snprintf(buf, n, "Auto (%d×)", s->resolution == MENU_RES_1080P ? 4 : 3);
	else if (s->scale == 1)
		snprintf(buf, n, "1× nativa");
	else
		snprintf(buf, n, "%d×", s->scale);
	return buf;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
}

void menu_settings_parse(struct menu_settings *s, const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[96];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		char key[32], val[32];
		if (line[0] == '#' || sscanf(line, " %31[^= ] = %31s", key, val) != 2)
			continue;
		for (char *p = key; *p; p++)
			*p = (char)lower((unsigned char)*p);
		for (char *p = val; *p; p++)
			*p = (char)lower((unsigned char)*p);
		bool yes = !strcmp(val, "si") || !strcmp(val, "sí") || !strcmp(val, "1");
		bool no = !strcmp(val, "no") || !strcmp(val, "0");
		if (!strcmp(key, "fondo") && (yes || no))
			s->waves = yes;
		else if (!strcmp(key, "fps") && (yes || no))
			s->fps = yes;
	}
}

void menu_video_parse(struct menu_settings *s, const char *text, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t e = i;
		while (e < len && text[e] != '\n')
			e++;
		char line[96];
		size_t n = e - i < sizeof line - 1 ? e - i : sizeof line - 1;
		memcpy(line, text + i, n);
		line[n] = '\0';
		i = e + 1;
		char key[32], val[32];
		if (line[0] == '#')
			continue;
		if (sscanf(line, " %31[^= ] = %31s", key, val) != 2) {
			/* Formato viejo: la línea es la resolución. */
			if (!strncmp(line, "1080", 4))
				s->resolution = MENU_RES_1080P;
			else if (!strncmp(line, "720", 3))
				s->resolution = MENU_RES_720P;
			continue;
		}
		for (char *p = key; *p; p++)
			*p = (char)lower((unsigned char)*p);
		for (char *p = val; *p; p++)
			*p = (char)lower((unsigned char)*p);
		if (!strcmp(key, "salida")) {
			if (!strncmp(val, "1080", 4))
				s->resolution = MENU_RES_1080P;
			else if (!strncmp(val, "720", 3))
				s->resolution = MENU_RES_720P;
		} else if (!strcmp(key, "escala")) {
			if (!strcmp(val, "auto"))
				s->scale = 0;
			else if (val[0] >= '1' && val[0] <= '5' && (!val[1] || (val[1] == 'x' && !val[2])))
				s->scale = val[0] - '0';
		}
	}
}

int menu_video_format(const struct menu_settings *s, char *buf, size_t size)
{
	char scale[8] = "auto";
	if (s->scale >= 1 && s->scale <= 5)
		snprintf(scale, sizeof scale, "%d", s->scale);
	return snprintf(buf, size,
			"# Video de Salsa64 (se lee al arrancar)\n# salida = 720|1080; escala = auto|1|2|3|4|5\n"
			"salida = %s\nescala = %s\n",
			s->resolution == MENU_RES_1080P ? "1080" : "720", scale);
}

int menu_settings_format(const struct menu_settings *s, char *buf, size_t size)
{
	return snprintf(buf, size, "# Ajustes del menú de Salsa64\nfondo = %s\nfps = %s\n", s->waves ? "si" : "no",
			s->fps ? "si" : "no");
}

void menu_init(struct menu *m, const struct romlist *list, int sel, const struct menu_settings *set)
{
	memset(m, 0, sizeof *m);
	m->list = list;
	if (set)
		m->set = *set;
	else
		menu_settings_default(&m->set);
	m->sel = sel >= 0 && sel < (int)list->count ? sel : 0;
	m->pos = (float)m->sel;
}

int menu_selected(const struct menu *m)
{
	return m->page == MENU_PAGE_GAMES && m->sel < (int)m->list->count ? m->sel : -1;
}

void menu_set_status(struct menu *m, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m->status, sizeof m->status, fmt, ap);
	va_end(ap);
}

/* Fila mostrada de una opción: «Aplicar ahora» no ocupa lugar si no hay
 * nada que aplicar. */
static int opt_row(const struct menu *m, int opt)
{
	return opt > MENU_OPT_APPLY && !menu_video_pending(m) ? opt - 1 : opt;
}

static int opt_from_row(const struct menu *m, int row)
{
	return row >= MENU_OPT_APPLY && !menu_video_pending(m) ? row + 1 : row;
}

static void move(struct menu *m, uint32_t dir, bool wrap)
{
	bool games = m->page == MENU_PAGE_GAMES;
	int n = games ? menu_rows(m) : MENU_OPTS - !menu_video_pending(m);
	int opt_v = opt_row(m, m->opt), *s = games ? &m->sel : &opt_v;
	int page = games ? 6 : 1, v = *s;
	if (dir == BIT(SALSA64_DS3_UP))
		v = v > 0 ? v - 1 : wrap ? n - 1 : 0;
	else if (dir == BIT(SALSA64_DS3_DOWN))
		v = v < n - 1 ? v + 1 : wrap ? 0 : n - 1;
	else if (dir == BIT(SALSA64_DS3_LEFT))
		v = v - page < 0 ? 0 : v - page;
	else if (dir == BIT(SALSA64_DS3_RIGHT))
		v = v + page > n - 1 ? n - 1 : v + page;
	*s = v;
	if (!games)
		m->opt = opt_from_row(m, v);
}

/* Botones de la página de opciones que no son mover la selección. */
static enum menu_action options_buttons(struct menu *m, uint32_t pressed)
{
	uint32_t change = BIT(SALSA64_DS3_CROSS) | BIT(SALSA64_DS3_LEFT) | BIT(SALSA64_DS3_RIGHT);
	if (m->opt == MENU_OPT_APPLY && !menu_video_pending(m))
		m->opt = MENU_OPT_BACK;  /* ya se aplicó */
	if ((pressed & (BIT(SALSA64_DS3_CIRCLE) | BIT(SALSA64_DS3_TRIANGLE))) ||
	    (m->opt == MENU_OPT_BACK && (pressed & BIT(SALSA64_DS3_CROSS)))) {
		m->page = MENU_PAGE_GAMES;
		return MENU_NONE;
	}
	if (m->opt == MENU_OPT_WAVES && (pressed & change)) {
		m->set.waves = !m->set.waves;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_OVERSCAN && (pressed & change)) {
		/* Apagado → Normal → Amplio → Apagado (izquierda, al revés); un
		 * recorte personalizado pasa a Normal. */
		int o = m->set.overscan;
		if (o == MENU_OVERSCAN_CUSTOM)
			o = MENU_OVERSCAN_NORMAL;
		else if (pressed & BIT(SALSA64_DS3_LEFT))
			o = o == MENU_OVERSCAN_OFF ? MENU_OVERSCAN_WIDE : o - 1;
		else
			o = o == MENU_OVERSCAN_WIDE ? MENU_OVERSCAN_OFF : o + 1;
		m->set.overscan = o;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_RESOLUTION && (pressed & change)) {
		m->set.resolution = m->set.resolution == MENU_RES_1080P ? MENU_RES_720P : MENU_RES_1080P;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_APPLY && (pressed & BIT(SALSA64_DS3_CROSS)))
		return MENU_APPLY;
	if (m->opt == MENU_OPT_SCALE && (pressed & change)) {
		/* Auto → 1× → ... → 5× → Auto (izquierda, al revés). */
		int v = m->set.scale + (pressed & BIT(SALSA64_DS3_LEFT) ? 5 : 1);
		m->set.scale = v % 6;
		return MENU_SETTINGS;
	}
	if (m->opt == MENU_OPT_FPS && (pressed & change)) {
		m->set.fps = !m->set.fps;
		return MENU_SETTINGS;
	}
	return MENU_NONE;
}

static void open_options(struct menu *m)
{
	m->page = MENU_PAGE_OPTIONS;
	m->opt = MENU_OPT_WAVES;
	m->opt_pos = (float)opt_row(m, m->opt);
	m->rep_button = 0;
}

enum menu_action menu_input(struct menu *m, uint32_t buttons, uint64_t now_us)
{
	const uint32_t updown = BIT(SALSA64_DS3_UP) | BIT(SALSA64_DS3_DOWN);
	uint32_t pressed = buttons & ~m->held;
	m->held = buttons;
	if (!m->armed) {
		m->armed = buttons == 0;
		return MENU_NONE;
	}
	if (m->loading)
		return MENU_NONE;
	if (m->page == MENU_PAGE_OPTIONS) {
		/* Arriba y abajo mueven; lo demás cambia el valor o vuelve. */
		enum menu_action a = options_buttons(m, pressed & ~updown);
		if (a != MENU_NONE || m->page != MENU_PAGE_OPTIONS) {
			m->rep_button = 0;
			return a;
		}
		pressed &= updown;
		buttons &= updown;
	} else if (pressed & BIT(SALSA64_DS3_CROSS)) {
		if (menu_selected(m) >= 0)
			return MENU_OPEN;
		if (m->sel == menu_row_exit(m))
			return MENU_QUIT;
		open_options(m);
		return MENU_NONE;
	} else if (pressed & BIT(SALSA64_DS3_TRIANGLE)) {
		open_options(m);
		return MENU_NONE;
	}

	/* Una sola dirección a la vez: la recién pulsada manda. */
	uint32_t dir = pressed & DIRS;
	if (dir) {
		dir &= ~dir + 1;
		move(m, dir, true);
		m->rep_button = dir;
		m->rep_since = now_us;
		m->rep_next = now_us + MENU_REPEAT_DELAY_US;
	} else if (m->rep_button && (buttons & m->rep_button)) {
		if (now_us >= m->rep_next) {
			move(m, m->rep_button, false);
			m->rep_next = now_us + (now_us - m->rep_since >= MENU_REPEAT_FAST_AFTER_US ? MENU_REPEAT_FAST_US
												   : MENU_REPEAT_US);
		}
	} else {
		m->rep_button = 0;
	}
	return MENU_NONE;
}

uint32_t menu_autopilot(const struct menu *m, int target, uint32_t step)
{
	if (step & 1)
		return 0;
	if (m->page != MENU_PAGE_GAMES)
		return BIT(SALSA64_DS3_CIRCLE);
	if (m->sel < target)
		return BIT(SALSA64_DS3_DOWN);
	if (m->sel > target)
		return BIT(SALSA64_DS3_UP);
	return BIT(SALSA64_DS3_CROSS);
}

/* ---- Dibujo ------------------------------------------------------------------ */

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float smooth(float v)
{
	v = clampf(v, 0.0f, 1.0f);
	return v * v * (3.0f - 2.0f * v);
}

static uint32_t rgba(uint32_t rgb, float a)
{
	return rgb << 8 | (uint32_t)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
	uint32_t out = 0;
	for (int s = 0; s < 24; s += 8) {
		float x = (float)(a >> s & 0xFF), y = (float)(b >> s & 0xFF);
		out |= (uint32_t)(x + (y - x) * t + 0.5f) << s;
	}
	return out;
}

/* Sigue a `target` con un suavizado exponencial (rate por segundo). */
static void ease(float *x, float target, float dt, float rate)
{
	*x += (target - *x) * (1.0f - expf(-rate * dt));
	if (fabsf(target - *x) < 0.001f)
		*x = target;
}

static float cells_w(const char *s, float scale)
{
	return rsxkit_text_cells(s) * RSXKIT_FONT_W * scale;
}

/* Texto con sombra. */
static void stext(float x, float y, const char *s, float scale, uint32_t rgb, float a, uint32_t max)
{
	if (a <= 0.004f)
		return;
	float off = scale >= 2.0f ? 2.0f : 1.5f;
	rsxkit_ui_text(x + off, y + off, s, scale, rgba(0x000000u, a * 0.55f), max);
	rsxkit_ui_text(x, y, s, scale, rgba(rgb, a), max);
}

static uint32_t hsv(float h, float s, float v)
{
	float c = v * s, hp = fmodf(h, 360.0f) / 60.0f, x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f)), m = v - c;
	float r = 0, g = 0, b = 0;
	switch ((int)hp) {
	case 0: r = c, g = x; break;
	case 1: r = x, g = c; break;
	case 2: g = c, b = x; break;
	case 3: g = x, b = c; break;
	case 4: r = x, b = c; break;
	default: r = c, b = x; break;
	}
	return (uint32_t)((r + m) * 255.0f) << 16 | (uint32_t)((g + m) * 255.0f) << 8 | (uint32_t)((b + m) * 255.0f);
}

/* Color de la etiqueta del cartucho: sale del título (siempre el mismo). */
static uint32_t label_color(const char *title)
{
	uint32_t h = 2166136261u;
	for (const char *p = title; *p; p++)
		h = (h ^ (uint8_t)*p) * 16777619u;
	return hsv((float)(h % 360u), 0.55f, 0.88f);
}

static void background(const struct menu *m, float W, float H)
{
	/* Degradado que cambia despacio (un ciclo de 90 s) si el fondo está
	 * animado; fijo si no. */
	float s = m->set.waves ? 0.5f + 0.5f * sinf(m->t * TAU / 90.0f) : 0.0f;
	uint32_t top = mix(0x123058u, 0x0C3C50u, s), bottom = mix(0x2C1048u, 0x16224Eu, s);
	rsxkit_ui_gradient(0, 0, W, H, rgba(mix(top, 0x2A4C78u, 0.35f), 1.0f), rgba(top, 1.0f), rgba(bottom, 1.0f),
			   rgba(mix(bottom, 0x000000u, 0.35f), 1.0f));
	if (!m->set.waves)
		return;
	static const struct {
		float y, depth, a0, a1, f0, f1, s0, s1, phase, line;
		uint32_t crest, sheet;
	} waves[] = {
		{ 0.56f, 0.36f, 0.050f, 0.010f, 0.55f, 3.20f, 0.018f, 0.075f, 0.70f, 0.0f, 0x00000000u, 0xFFFFFF12u },
		{ 0.60f, 0.30f, 0.040f, 0.018f, 0.85f, 2.10f, 0.030f, -0.055f, 0.00f, 2.5f, 0xFFFFFF6Cu, 0xFFFFFF24u },
		{ 0.66f, 0.24f, 0.030f, 0.014f, 1.25f, 2.70f, -0.040f, 0.050f, 0.35f, 1.6f, 0xFFFFFF4Cu, 0xFFFFFF1Au },
	};
	for (unsigned i = 0; i < sizeof waves / sizeof waves[0]; i++) {
		rsxkit_wave w = {
			waves[i].y * H, waves[i].depth * H, { waves[i].a0 * H, waves[i].a1 * H }, { waves[i].f0, waves[i].f1 },
			{ waves[i].s0, waves[i].s1 }, waves[i].phase, waves[i].line, waves[i].crest, waves[i].sheet,
		};
		rsxkit_ui_wave(&w, m->t);
	}
}

/* Íconos, dibujados aquí con rectángulos y glifos de la fuente. */
static void icon_body(float x, float y, float s, float a)
{
	rsxkit_ui_gradient(x, y, s, s, rgba(0x404A5Cu, a), rgba(0x353E4Eu, a), rgba(0x262C38u, a), rgba(0x1F242Eu, a));
	rsxkit_ui_rect(x, y, s, s * 0.035f + 0.5f, rgba(0xFFFFFFu, a * 0.22f));
}

static void icon_cart(float x, float y, float s, uint32_t label, float a)
{
	icon_body(x, y, s, a);
	float m = s * 0.14f;
	rsxkit_ui_gradient(x + m, y + m, s - 2 * m, s * 0.5f, rgba(mix(label, 0xFFFFFFu, 0.25f), a), rgba(label, a),
			   rgba(label, a), rgba(mix(label, 0x000000u, 0.3f), a));
	for (int i = 0; i < 4; i++)
		rsxkit_ui_rect(x + s * (0.24f + 0.14f * i), y + s * 0.78f, s * 0.08f, s * 0.1f, rgba(0x0E1116u, a));
}

static void icon_options(float x, float y, float s, float a)
{
	icon_body(x, y, s, a);
	for (int i = 0; i < 3; i++) {
		float yy = y + s * (0.28f + 0.2f * i);
		rsxkit_ui_rect(x + s * 0.22f, yy, s * 0.56f, s * 0.07f, rgba(0xE8EEF6u, a));
		rsxkit_ui_rect(x + s * (0.3f + 0.18f * ((i * 2) % 3)), yy - s * 0.05f, s * 0.12f, s * 0.17f,
			       rgba(C_ACCENT, a));
	}
}

static void icon_glyph(float x, float y, float s, const char *g, float a)
{
	icon_body(x, y, s, a);
	float sc = s / 22.0f;
	rsxkit_ui_text(x + s * 0.5f - RSXKIT_FONT_W * sc * 0.5f, y + s * 0.5f - RSXKIT_FONT_H * sc * 0.5f, g, sc,
		       rgba(C_TEXT, a), 1);
}

static float row_y(float d, float H)
{
	float ad = fabsf(d), y = ad <= 1.0f ? ad * GAP : GAP + (ad - 1.0f) * SPACING;
	return H * (COL_Y + (d < 0 ? -y : y));
}

/* Una fila de la columna: ícono, título y, si está elegida, una línea más. */
struct row {
	const char *title, *sub;
	int icon;           /* 0 cartucho, 1 opciones, 2 glifo */
	const char *glyph;
	uint32_t label;
	const char *value;  /* a la derecha del título (opciones) */
};

static void draw_row(const struct row *r, float d, float x, float W, float H, float a)
{
	float ad = fabsf(d), k = smooth(1.0f - ad), y = row_y(d, H);
	/* Se desvanece hacia los bordes de la columna (lejos del encabezado y
	 * del pie). */
	float edge = smooth((y - H * 0.16f) / (H * 0.06f)) * smooth((H * 0.79f - y) / (H * 0.07f));
	float alpha = a * edge * clampf(1.0f - 0.17f * ad, 0.22f, 1.0f);
	if (alpha <= 0.004f)
		return;
	if (k > 0.0f)
		rsxkit_ui_gradient(x - 26.0f, y - 42.0f, W * 0.53f - x + 26.0f, 84.0f, rgba(0xFFFFFFu, 0.13f * k * alpha),
				   rgba(0xFFFFFFu, 0.0f), rgba(0xFFFFFFu, 0.07f * k * alpha), rgba(0xFFFFFFu, 0.0f));
	float s = 42.0f + 26.0f * k;
	if (r->icon == 0)
		icon_cart(x, y - s * 0.5f, s, r->label, alpha);
	else if (r->icon == 1)
		icon_options(x, y - s * 0.5f, s, alpha);
	else
		icon_glyph(x, y - s * 0.5f, s, r->glyph, alpha);
	float scale = 2.0f + 0.85f * k, tx = x + 90.0f, th = RSXKIT_FONT_H * scale;
	float ty = y - th * 0.5f - 10.0f * k;
	uint32_t max = (uint32_t)((W * 0.53f - tx) / (RSXKIT_FONT_W * scale));
	stext(tx, ty, r->title, scale, k > 0.5f ? 0xFFFFFFu : C_TEXT, alpha, max);
	if (r->value)
		stext(tx + cells_w(r->title, scale) + 24.0f, ty, r->value, scale, C_ACCENT, alpha, 0);
	float ks = smooth((k - 0.35f) / 0.65f);
	if (r->sub && ks > 0.0f)
		stext(tx, ty + th + 6.0f, r->sub, 1.5f, C_DIM, alpha * ks, (uint32_t)((W * 0.53f - tx) / 12.0f));
}

static const char *location(const char *path, char *buf, size_t n)
{
	if (!strncmp(path, "/dev_hdd0/", 10))
		return "Disco duro";
	if (!strncmp(path, "/dev_usb", 8) && strlen(path) > 10) {
		snprintf(buf, n, "Memoria USB %c", path[10]);
		return buf;
	}
	return path;
}

static void size_text(uint32_t bytes, char *buf, size_t n)
{
	if (bytes >= (1u << 20) && bytes % (1u << 20) == 0)
		snprintf(buf, n, "%u MB", bytes >> 20);
	else if (bytes >= (1u << 20))
		snprintf(buf, n, "%.1f MB", bytes / 1048576.0);
	else
		snprintf(buf, n, "%u KB", (bytes + 1023) >> 10);
}

static void info_line(float x, float *y, float vx, float vscale, const char *label, const char *value,
		      uint32_t color, float a, uint32_t max)
{
	stext(x, *y + 2.0f, label, 1.5f, C_DIM, a, 0);
	stext(vx, *y, value, vscale, color, a, max);
	*y += 34.0f;
}

static void panel(const struct menu *m, float W, float H, float a)
{
	float px = W * 0.555f, py = H * 0.17f, pw = W * 0.39f, ph = H * 0.56f;
	rsxkit_ui_gradient(px, py, pw, ph, rgba(0x000000u, a * 0.40f), rgba(0x000000u, a * 0.28f),
			   rgba(0x000000u, a * 0.24f), rgba(0x000000u, a * 0.14f));
	rsxkit_ui_rect(px, py, pw, 2.0f, rgba(0xFFFFFFu, a * 0.32f));

	/* Lo de adentro aparece cuando la columna se detiene. */
	bool games = m->page == MENU_PAGE_GAMES;
	float settle = 1.0f - clampf(fabsf(games ? m->sel - m->pos : opt_row(m, m->opt) - m->opt_pos) * 1.6f, 0.0f, 1.0f);
	float ca = a * smooth(settle) * (games ? 1.0f - m->page_t : m->page_t);
	/* Con tele 4:3 el panel es angosto: letra un poco más chica. */
	bool narrow = pw < 450.0f;
	float tscale = narrow ? 2.0f : 2.5f, vscale = narrow ? 1.5f : 1.75f, dscale = narrow ? 1.35f : 1.6f;
	float x = px + 26.0f, y = py + 24.0f, vx = x + (narrow ? 124.0f : 140.0f);
	uint32_t vmax = (uint32_t)((px + pw - 20.0f - vx) / (RSXKIT_FONT_W * vscale));
	uint32_t tmax = (uint32_t)((pw - 48.0f) / (RSXKIT_FONT_W * tscale));
	uint32_t lmax = (uint32_t)((pw - 48.0f) / (RSXKIT_FONT_W * dscale));
	char buf[ROMLIST_PATH_MAX + 32], loc[24];

	if (!games) {
		static const char *const waves_text[] = { "Ondas en movimiento de fondo.", "Apágalas para un fondo fijo.",
							  NULL, "Se guarda en menu.cfg.", NULL, NULL };
		static const char *const crop_text[] = { "Quita el borde de la imagen", "que una tele de tubo no deja",
							 "ver (ahí algunos juegos dejan", "basura).", NULL,
							 "Normal: 8 y 6 puntos por lado;", "amplio: 16 y 12.",
							 "Se guarda en overscan.cfg.", NULL, NULL };
		static const char *const res_text[] = { "Salida de video: 720p (3x)", "o 1080p (4x). Luego, abajo,", "«Aplicar ahora».",
							"Se guarda en video.cfg.", NULL, NULL };
		static const char *const fps_text[] = { "Campos del VI por segundo y", "velocidad, cuadros del juego",
							"y de la pantalla, en una", "esquina. En el juego,",
							"SELECT + triángulo.", "Se guarda en menu.cfg.", NULL, NULL };
		/* Escala interna: tamaño y texto dependen del valor. */
		char scale_buf[24], size_buf[32];
		int eff = m->set.scale ? m->set.scale : (m->set.resolution == MENU_RES_1080P ? 4 : 3);
		snprintf(size_buf, sizeof size_buf, "%d× = %d×%d puntos", eff, 320 * eff, 240 * eff);
		const char *const scale_text[] = { size_buf, "Mayor que la pantalla es", "supermuestreo: más nitidez",
						   "y bordes suaves, pero más", "trabajo para el RSX.",
						   "1×: la imagen original N64.", "Luego, «Aplicar ahora».", NULL, NULL };
		char want_buf[40];
		snprintf(want_buf, sizeof want_buf, "%s, escala %d×", resolution_name(m->set.resolution),
			 m->set.scale ? m->set.scale : (m->set.resolution == MENU_RES_1080P ? 4 : 3));
		const char *const apply_text[] = { "Pone la salida y la escala", "elegidas sin cerrar Salsa64:", want_buf, NULL,
						   "Si la tele no acepta el", "modo, queda el anterior.", NULL, NULL };
		static const char *const back_text[] = { "A la lista de juegos.", NULL, NULL };
		const char *title = m->opt == MENU_OPT_WAVES ? "Fondo animado"
				    : m->opt == MENU_OPT_OVERSCAN ? "Recorte del borde"
				    : m->opt == MENU_OPT_RESOLUTION ? "Resolución"
				    : m->opt == MENU_OPT_SCALE ? "Escala interna"
				    : m->opt == MENU_OPT_APPLY ? "Aplicar ahora"
				    : m->opt == MENU_OPT_FPS ? "Mostrar FPS" : "Volver";
		const char *const *text = m->opt == MENU_OPT_WAVES ? waves_text
					  : m->opt == MENU_OPT_OVERSCAN ? crop_text
					  : m->opt == MENU_OPT_RESOLUTION ? res_text
					  : m->opt == MENU_OPT_SCALE ? scale_text
					  : m->opt == MENU_OPT_APPLY ? apply_text
					  : m->opt == MENU_OPT_FPS ? fps_text : back_text;
		const char *value = m->opt == MENU_OPT_WAVES ? (m->set.waves ? "Sí" : "No")
				    : m->opt == MENU_OPT_OVERSCAN ? overscan_name(m->set.overscan)
				    : m->opt == MENU_OPT_RESOLUTION ? resolution_name(m->set.resolution)
				    : m->opt == MENU_OPT_SCALE ? scale_name(&m->set, scale_buf, sizeof scale_buf)
				    : m->opt == MENU_OPT_FPS ? (m->set.fps ? "Sí" : "No") : NULL;
		stext(x, y, title, tscale, 0xFFFFFFu, ca, tmax);
		y += 56.0f;
		/* Líneas cortas (caben en 4:3); NULL es un renglón en blanco, dos
		 * NULL seguidos terminan. */
		bool dim = false;  /* lo que va después del renglón en blanco */
		for (int i = 0; text[i] || text[i + 1]; i++, y += 30.0f) {
			if (!text[i])
				dim = true;
			else
				stext(x, y, text[i], dscale, dim ? C_DIM : C_TEXT, ca, lmax);
		}
		if (value)
			stext(x, y + 16.0f, value, tscale, C_ACCENT, ca, 0);
		if (m->note[0] && (m->opt == MENU_OPT_RESOLUTION || m->opt == MENU_OPT_SCALE || m->opt == MENU_OPT_APPLY))
			stext(x, py + ph - 44.0f, m->note, dscale, C_ACCENT, ca, lmax);
		if (m->opt == MENU_OPT_RESOLUTION) {
			y += 16.0f + 56.0f;
			if (m->out_w && m->out_h) {
				snprintf(buf, sizeof buf, "Salida actual: %ux%u", m->out_w, m->out_h);
				stext(x, y, buf, dscale, C_TEXT, ca, lmax);
				y += 30.0f;
			}
			/* Pidió 1080p al arrancar y la tele o el XMB no lo dieron. */
			if (m->res_run == MENU_RES_1080P && m->out_h && m->out_h != 1080) {
				stext(x, y, "No hay 1080p: actívalo en", dscale, C_DIM, ca, lmax);
				stext(x, y + 30.0f, "Ajustes de pantalla del XMB.", dscale, C_DIM, ca, lmax);
			}
		}
		return;
	}
	int s = menu_selected(m);
	if (s < 0) {
		bool exit = m->sel == menu_row_exit(m);
		stext(x, y, exit ? "Salir al XMB" : "Opciones", tscale, 0xFFFFFFu, ca, tmax);
		stext(x, y + 56.0f, exit ? "Cierra Salsa64 y vuelve" : "Fondo, recorte del borde,", dscale, C_TEXT, ca, lmax);
		stext(x, y + 86.0f, exit ? "al menú de la consola." : "resolución y FPS.", dscale, C_TEXT, ca, lmax);
		return;
	}
	const struct rom_entry *e = &m->list->e[s];
	stext(x, y, romlist_display_title(e), tscale, 0xFFFFFFu, ca, tmax);
	y += 60.0f;
	info_line(x, &y, vx, vscale, "Archivo", romlist_name(e), C_TEXT, ca, vmax);
	info_line(x, &y, vx, vscale, "Ubicación", location(e->path, loc, sizeof loc), C_TEXT, ca, vmax);
	if (e->code[0])
		snprintf(buf, sizeof buf, "%s, versión %u", e->code, e->version);
	else
		snprintf(buf, sizeof buf, "—");
	info_line(x, &y, vx, vscale, "Código", buf, C_TEXT, ca, vmax);
	size_text(e->size, buf, sizeof buf);
	info_line(x, &y, vx, vscale, "Tamaño", buf, C_TEXT, ca, vmax);
	const char *region = romlist_region_name(e->region),
		   *tv = e->tv == SALSA64_TV_PAL ? "PAL" : e->tv == SALSA64_TV_MPAL ? "PAL-M" : "NTSC";
	if (strcmp(region, "?"))
		snprintf(buf, sizeof buf, "%s (%s)", region, tv);
	else
		snprintf(buf, sizeof buf, "%s", tv);
	info_line(x, &y, vx, vscale, "Región", buf, C_TEXT, ca, vmax);
	bool emulated = e->save == SALSA64_SAVE_NONE || salsa64_save_emulated((enum salsa64_save_type)e->save);
	snprintf(buf, sizeof buf, "%s%s", romlist_save_label(e->save), emulated ? "" : " (sin emular)");
	info_line(x, &y, vx, vscale, "Guardado", buf, emulated ? C_TEXT : C_WARN, ca, vmax);
	if (m->last[0] && !strcmp(m->last, e->path))
		stext(x, py + ph - 44.0f, "● Último juego abierto", 1.5f, C_ACCENT, ca, 0);
}

static void games_column(const struct menu *m, float x, float W, float H, float a)
{
	int n = menu_rows(m);
	for (int i = 0; i < n; i++) {
		float d = i - m->pos;
		if (fabsf(d) > 7.0f)
			continue;
		struct row r = { 0 };
		if (i < (int)m->list->count) {
			const struct rom_entry *e = &m->list->e[i];
			r.title = romlist_display_title(e);
			r.label = label_color(r.title);
			r.sub = romlist_name(e);
		} else if (i == (int)m->list->count) {
			r.title = "Opciones";
			r.sub = "Fondo animado y recorte del borde";
			r.icon = 1;
		} else {
			r.title = "Salir al XMB";
			r.sub = "Vuelve al menú de la consola";
			r.icon = 2;
			r.glyph = "◀";
		}
		draw_row(&r, d, x, W, H, a);
	}
	if (!m->list->count) {
		float y = H * 0.16f;
		stext(x, y, "No encontré ROMs (.z64, .v64, .n64) en:", 1.6f, C_TEXT, a, 0);
		for (unsigned i = 0; i < m->ndirs && i < MENU_DIRS_MAX; i++)
			stext(x + 24.0f, y + 28.0f * (i + 1), m->dirs[i], 1.5f, C_DIM, a, 0);
	}
}

static void options_column(const struct menu *m, float x, float W, float H, float a)
{
	struct row waves = { "Fondo animado", "Ondas detrás del menú", 2, "~", 0, m->set.waves ? "Sí" : "No" };
	struct row crop = { "Recorte del borde", "Overscan de la imagen del juego", 2, "□", 0,
			    overscan_name(m->set.overscan) };
	struct row res = { "Resolución", "Salida de video", 2, "■", 0, resolution_name(m->set.resolution) };
	char sbuf[24];
	struct row scale = { "Escala interna", "Dibujo interno", 2, "▲", 0,
			     scale_name(&m->set, sbuf, sizeof sbuf) };
	struct row apply = { "Aplicar ahora", "Salida y escala elegidas", 2, "●", 0, NULL };
	struct row fps = { "Mostrar FPS", "Velocidad en una esquina del juego", 2, "#", 0, m->set.fps ? "Sí" : "No" };
	struct row back = { "Volver", "A la lista de juegos", 2, "◀", 0, NULL };
	draw_row(&waves, MENU_OPT_WAVES - m->opt_pos, x, W, H, a);
	draw_row(&crop, MENU_OPT_OVERSCAN - m->opt_pos, x, W, H, a);
	draw_row(&res, MENU_OPT_RESOLUTION - m->opt_pos, x, W, H, a);
	draw_row(&scale, MENU_OPT_SCALE - m->opt_pos, x, W, H, a);
	if (menu_video_pending(m))
		draw_row(&apply, MENU_OPT_APPLY - m->opt_pos, x, W, H, a);
	draw_row(&fps, (float)opt_row(m, MENU_OPT_FPS) - m->opt_pos, x, W, H, a);
	draw_row(&back, (float)opt_row(m, MENU_OPT_BACK) - m->opt_pos, x, W, H, a);
}

void menu_frame(struct menu *m, float lw, float lh, uint64_t now_us)
{
	float dt = m->last_us && now_us > m->last_us ? (float)(now_us - m->last_us) / 1e6f : 0.0f;
	if (dt > 0.1f)
		dt = 0.1f;
	m->last_us = now_us;
	m->t += dt;
	ease(&m->pos, (float)m->sel, dt, 14.0f);
	if (m->opt == MENU_OPT_APPLY && !menu_video_pending(m))
		m->opt = MENU_OPT_BACK;  /* ya se aplicó */
	ease(&m->opt_pos, (float)opt_row(m, m->opt), dt, 14.0f);
	ease(&m->page_t, (float)m->page, dt, 10.0f);
	m->intro = clampf(m->intro + dt / 0.45f, 0.0f, 1.0f);

	float W = lw, H = lh, a = smooth(m->intro);
	background(m, W, H);

	/* Encabezado. */
	stext(W * 0.06f, H * 0.055f, "Salsa64", 2.5f, 0xFFFFFFu, a, 0);
	stext(W * 0.06f + 7 * 20.0f + 22.0f, H * 0.055f + 8.0f, m->page == MENU_PAGE_GAMES ? "Juegos" : "Opciones",
	      2.0f, C_DIM, a, 0);
	char count[24];
	snprintf(count, sizeof count, m->list->count == 1 ? "%u juego" : "%u juegos", m->list->count);
	stext(W * 0.94f - cells_w(count, 1.75f), H * 0.055f + 10.0f, count, 1.75f, C_DIM, a, 0);

	/* Columna (se desliza al cambiar de página) y panel. */
	float x = W * 0.07f, p = smooth(m->page_t);
	if (p < 0.999f)
		games_column(m, x - p * W * 0.06f, W, H, a * (1.0f - p));
	if (p > 0.001f)
		options_column(m, x + (1.0f - p) * W * 0.06f, W, H, a * p);
	panel(m, W, H, a);

	/* Pie: mensaje, botones y cómo volver. */
	if (m->status[0])
		stext(W * 0.06f, H * 0.815f, m->status, 1.6f, C_WARN, a, (uint32_t)(W * 0.88f / 12.8f));
	stext(W * 0.06f, H * 0.865f,
	      m->page == MENU_PAGE_GAMES ? "×  Abrir      ↑↓  Elegir      △  Opciones" : "×  Cambiar      ○  Volver", 1.6f,
	      C_TEXT, a, 0);
	stext(W * 0.06f, H * 0.865f + 30.0f, "En el juego, START + SELECT vuelve a este menú.", 1.35f, C_DIM, a, 0);

	if (m->loading) {
		rsxkit_ui_rect(0, 0, W, H, rgba(0x000000u, 0.55f));
		char line[ROMLIST_PATH_MAX + 32];
		snprintf(line, sizeof line, "Cargando %s…", m->loading);
		float w = cells_w(line, 2.5f);
		if (w > W * 0.9f)
			w = W * 0.9f;
		stext((W - w) * 0.5f, H * 0.46f, line, 2.5f, 0xFFFFFFu, 1.0f, (uint32_t)(W * 0.9f / 20.0f));
	}
}
