/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — vitest: ROM de prueba del VI. Código propio, sin libultra ni
 * libdragon; se empaqueta con el IPL3 libre de libdragon (nust64).
 *
 * Dibuja con el CPU, en dos framebuffers de 320×240 que alterna en cada
 * interrupción del VI (VI_V_INTR = 2), un patrón fijo y fácil de verificar:
 *
 *   y   0–159  8 barras de 40 píxeles: blanco, amarillo, cian, verde,
 *              magenta, rojo, azul, negro
 *   y 160–199  rampa de grises de izquierda (0) a derecha (255)
 *   y 200–239  fondo azul oscuro (0x20, 0x20, 0x60) con un cuadro naranja de
 *              24×24 en y 206–229 que avanza 3 píxeles por cuadro
 *
 * Cada FIELDS_PER_FORMAT cuadros cambia entre RGBA5551 y RGBA8888.
 *
 * Además se verifica a sí misma: tras 2 × FIELDS_PER_FORMAT cuadros compara
 * el tiempo medio entre interrupciones (registro Count) con la duración de un
 * campo del tipo de TV y revisa que VI_V_CURRENT valga la línea de VI_V_INTR
 * al atender la interrupción. Imprime por el ISViewer (si lo hay)
 * "vitest: OK …" o "vitest: FALLO …" y sigue animando.
 *
 * Valores de los registros del VI: los modos estándar NTSC/PAL de 320×240
 * documentados en n64brew.dev (Video Interface).
 */
#include <stdint.h>

#define REG(pa) (*(volatile uint32_t *)(0xA0000000u | (pa)))

#define VI_CONTROL   REG(0x04400000)
#define VI_ORIGIN    REG(0x04400004)
#define VI_WIDTH     REG(0x04400008)
#define VI_V_INTR    REG(0x0440000C)
#define VI_V_CURRENT REG(0x04400010)
#define VI_BURST     REG(0x04400014)
#define VI_V_SYNC    REG(0x04400018)
#define VI_H_SYNC    REG(0x0440001C)
#define VI_H_SYNC_LEAP REG(0x04400020)
#define VI_H_VIDEO   REG(0x04400024)
#define VI_V_VIDEO   REG(0x04400028)
#define VI_V_BURST   REG(0x0440002C)
#define VI_X_SCALE   REG(0x04400030)
#define VI_Y_SCALE   REG(0x04400034)
#define MI_INTR_MASK REG(0x0430000C)
#define BOOT_FLAGS   REG(0x04000008)  /* DMEM: el IPL3 de libdragon deja el tipo de TV en [23:16] */

#define ISV_MAGIC    REG(0x13FF0000)
#define ISV_LEN      REG(0x13FF0014)
#define ISV_BUF      0x13FF0020u

#define W 320
#define H 240
#define V_INTR_LINE 2
#define FIELDS_PER_FORMAT 60
#define SQUARE 24
#define CTRL_16 0x3202u  /* pixel advance 3, solo remuestreo, RGBA5551 */
#define CTRL_32 0x3203u  /* ídem, RGBA8888 */

/* Los escribe el manejador de interrupciones (start.S). */
volatile uint32_t vi_fields, vi_vcurrent, vi_count;

/* Fuera de .bss (start.S no los limpia por la caché): se escriben enteros
 * por KSEG1 en cada cuadro. */
static uint32_t fb[2][W * H] __attribute__((section(".fb"), aligned(64)));

static const uint32_t bars[8] = {  /* 0xRRGGBBAA */
	0xFFFFFFFF, 0xFFFF00FF, 0x00FFFFFF, 0x00FF00FF,
	0xFF00FFFF, 0xFF0000FF, 0x0000FFFF, 0x000000FF,
};
#define BG     0x202060FFu
#define ORANGE 0xFF8000FFu

void *memset(void *d, int c, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void *memcpy(void *d, const void *s, __SIZE_TYPE__ n)
{
	uint8_t *p = d;
	const uint8_t *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}

static uint32_t to5551(uint32_t c)
{
	return (c >> 27) << 11 | ((c >> 19) & 31) << 6 | ((c >> 11) & 31) << 1 | 1;
}

static volatile uint32_t *uncached(const void *p)
{
	return (volatile uint32_t *)((uint32_t)p | 0x20000000u);
}

/* Una línea del patrón en RGBA8888. */
static void pattern_line(uint32_t *line, int y, int square_x)
{
	for (int x = 0; x < W; x++) {
		uint32_t c;
		if (y < 160) {
			c = bars[x / 40];
		} else if (y < 200) {
			uint32_t g = (uint32_t)x * 255 / (W - 1);
			c = g << 24 | g << 16 | g << 8 | 0xFF;
		} else {
			c = BG;
			if (y >= 206 && y < 206 + SQUARE && x >= square_x && x < square_x + SQUARE)
				c = ORANGE;
		}
		line[x] = c;
	}
}

static void draw(uint32_t *buf, int fmt32, uint32_t frame)
{
	static uint32_t line[W];
	volatile uint32_t *out = uncached(buf);
	int square_x = (int)(frame * 3 % (W - SQUARE));

	for (int y = 0; y < H; y++) {
		/* Las líneas sin el cuadro se repiten: se recalcula solo al cambiar. */
		if (y == 0 || y == 160 || y == 200 || y == 206 || y == 206 + SQUARE)
			pattern_line(line, y, square_x);
		if (fmt32) {
			for (int x = 0; x < W; x++)
				*out++ = line[x];
		} else {
			for (int x = 0; x < W; x += 2)
				*out++ = to5551(line[x]) << 16 | to5551(line[x + 1]);
		}
	}
}

static void vi_setup(int pal, const uint32_t *origin)
{
	VI_CONTROL = 0;
	VI_ORIGIN = (uint32_t)origin & 0x1FFFFFFF;
	VI_WIDTH = W;
	VI_V_INTR = V_INTR_LINE;
	VI_V_CURRENT = 0;
	if (pal) {
		VI_BURST = 0x0404233A;
		VI_V_SYNC = 0x271;
		VI_H_SYNC = 0x00150C69;
		VI_H_SYNC_LEAP = 0x0C6F0C6E;
		VI_H_VIDEO = 0x00800300;
		VI_V_VIDEO = 0x005F0239;
		VI_V_BURST = 0x0009026B;
	} else {
		VI_BURST = 0x03E52239;
		VI_V_SYNC = 0x20D;
		VI_H_SYNC = 0x00000C15;
		VI_H_SYNC_LEAP = 0x0C150C15;
		VI_H_VIDEO = 0x006C02EC;
		VI_V_VIDEO = 0x002501FF;
		VI_V_BURST = 0x000E0204;
	}
	VI_X_SCALE = 0x200;  /* 320 / 640 en 2.10 */
	VI_Y_SCALE = 0x400;
	VI_CONTROL = CTRL_16;
}

static uint32_t c0_status(void)
{
	uint32_t v;
	__asm__ volatile("mfc0 %0, $12\n\tnop" : "=r"(v));
	return v;
}

static void c0_set_status(uint32_t v)
{
	__asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" : : "r"(v));
}

extern const uint32_t exc_vector[4];

static void enable_vi_interrupt(void)
{
	volatile uint32_t *vec = (volatile uint32_t *)0xA0000180u;
	for (int i = 0; i < 4; i++)
		vec[i] = exc_vector[i];
	/* La copia se hizo sin caché: invalidar la línea de la caché de
	 * instrucciones del vector. */
	__asm__ volatile(".set push\n\t.set mips3\n\tcache 0x10, 0(%0)\n\t.set pop" : : "r"(0x80000180u));
	VI_V_CURRENT = 0;
	MI_INTR_MASK = 0x80;  /* activar VI */
	c0_set_status(c0_status() | 0x0401);  /* IE | IM2 */
}

/* ---- ISViewer ------------------------------------------------------------ */

static int isv_present(void)
{
	ISV_MAGIC = 0x56495354;  /* "VIST": la ROM del cartucho no lo guardaría */
	return ISV_MAGIC == 0x56495354;
}

static char msg[160];
static unsigned msg_len;

static void put(const char *s)
{
	while (*s && msg_len < sizeof msg)
		msg[msg_len++] = *s++;
}

static void put_u(uint32_t v)
{
	char tmp[11];
	int n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n && msg_len < sizeof msg)
		msg[msg_len++] = tmp[--n];
}

static void isv_flush(void)
{
	for (unsigned i = 0; i < msg_len; i += 4) {
		uint32_t w = 0;
		for (unsigned k = 0; k < 4; k++)
			w = w << 8 | (i + k < msg_len ? (uint8_t)msg[i + k] : 0);
		REG(ISV_BUF + i) = w;
	}
	ISV_LEN = msg_len;
	msg_len = 0;
}

/* ---- Programa ------------------------------------------------------------ */

static void report(int pal, uint32_t fields, uint32_t ticks, uint32_t vcur, uint32_t skipped)
{
	/* Count avanza a 46,875 MHz: medio ciclo del VR4300. Campo esperado:
	 * NTSC 263 × 3094 / 48,681812 MHz; PAL 313 × 3178 / 49,656530 MHz. */
	uint32_t expected = pal ? 938994 : 783520;
	uint32_t per_field = ticks / fields;
	uint32_t tol = expected / 200;  /* ±0,5 % */
	int ok = per_field + tol >= expected && per_field <= expected + tol &&
		 (vcur >> 1) == V_INTR_LINE >> 1;

	put(ok ? "vitest: OK" : "vitest: FALLO");
	put(" tv=");
	put(pal ? "PAL" : "NTSC");
	put(" campos=");
	put_u(fields);
	put(" count/campo=");
	put_u(per_field);
	put(" esperado=");
	put_u(expected);
	put(" v_current=");
	put_u(vcur);
	put(" saltados=");
	put_u(skipped);
	put("\n");
	isv_flush();
}

int main(void)
{
	int pal = ((BOOT_FLAGS >> 16) & 0xFF) == 0;  /* 0 = PAL, 1 = NTSC, 2 = MPAL */
	int isv = isv_present();
	uint32_t cur = 0, frame = 0, skipped = 0;
	uint32_t first_fields = 0, first_count = 0;

	draw(fb[0], 0, 0);
	vi_setup(pal, fb[0]);
	enable_vi_interrupt();

	for (;;) {
		int fmt32 = (frame / FIELDS_PER_FORMAT) & 1;
		uint32_t *back = fb[cur ^ 1];
		draw(back, fmt32, frame);

		uint32_t seen = vi_fields;
		while (vi_fields == seen)
			;
		uint32_t now_fields = vi_fields;
		skipped += now_fields - seen - 1;

		VI_ORIGIN = (uint32_t)back & 0x1FFFFFFF;
		VI_CONTROL = fmt32 ? CTRL_32 : CTRL_16;
		cur ^= 1;
		frame++;

		if (frame == 2) {
			first_fields = now_fields;
			first_count = vi_count;
		} else if (frame == 2 * FIELDS_PER_FORMAT + 2 && isv) {
			report(pal, now_fields - first_fields, vi_count - first_count, vi_vcurrent, skipped);
		}
	}
}
