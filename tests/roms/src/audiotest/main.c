/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — audiotest: ROM de prueba del AI y de la HLE de audio. Código
 * propio, sin libultra ni libdragon; se empaqueta con el IPL3 libre de
 * libdragon (nust64).
 *
 * El CPU genera un tono conocido (tone.h) y lo manda al AI en búferes de
 * 512 muestras estéreo a ~32 kHz (AI_DACRATE según el reloj del VI de la
 * región), con la cola de dos DMA del AI: escribe el siguiente cuando
 * AI_STATUS deja de estar FULL. Rota tres búferes, que nunca terminan en
 * un límite de 8 KB (el error del acarreo del AI).
 *
 * Fase 1 (CPU_BUFFERS búferes): el tono lo escribe el CPU. Verifica
 * AI_STATUS (BUSY con un DMA, FULL con dos), que AI_LENGTH baje al ritmo
 * del DAC, que llegue una interrupción del AI por DMA y que el intervalo
 * entre ellas (registro Count) sea la duración de un búfer, y que el AI
 * nunca se quede sin búfer. Imprime "audiotest: OK …" o "audiotest:
 * FALLO …".
 *
 * Fase 2 (sigue para siempre): el CPU escribe el tono por canales separados
 * y una tarea de audio del RSP lo intercala y lo guarda en el búfer del AI
 * (comandos de nead: LOADBUFF, CLEARBUFF, ADDMIXER, HILOGAIN ×1,
 * INTERLEAVE, SAVEBUFF; operaciones exactas). El microcódigo es un BREAK y
 * su segmento de datos lleva el texto "Salsa64 ABI NEAD": solo un emulador
 * con HLE que lo reconozca produce el búfer (en una N64 real esta fase
 * falla). Compara lo que dejó el RSP con el tono y, tras HLE_BUFFERS
 * búferes, imprime "audiotest: HLE OK …" o "audiotest: FALLO …".
 *
 * Lo que sale por el AI es el mismo tono continuo en las dos fases: el
 * host lo compara muestra por muestra (salsa64-run --audiotest-check) y en
 * la PS3 se oye: La 4 de 440 Hz (afinador), Do 5 a la izquierda, Mi 5 a la
 * derecha, Sol 5, Do 6 y silencio. La pantalla cambia de color con cada
 * nota.
 */
#include <stdint.h>

#include "tone.h"

#define REG(pa) (*(volatile uint32_t *)(0xA0000000u | (pa)))

#define SP_MEM_ADDR  REG(0x04040000)
#define SP_DRAM_ADDR REG(0x04040004)
#define SP_RD_LEN    REG(0x04040008)
#define SP_STATUS    REG(0x04040010)
#define SP_PC        REG(0x04080000)
#define MI_INTR      REG(0x04300008)
#define MI_INTR_MASK REG(0x0430000C)
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
#define AI_DRAM_ADDR REG(0x04500000)
#define AI_LEN       REG(0x04500004)
#define AI_CONTROL   REG(0x04500008)
#define AI_STATUS    REG(0x0450000C)
#define AI_DACRATE   REG(0x04500010)
#define AI_BITRATE   REG(0x04500014)
#define BOOT_FLAGS   REG(0x04000008)  /* tipo de TV en [23:16]: 0 PAL, 1 NTSC, 2 MPAL */

#define ISV_MAGIC    REG(0x13FF0000)
#define ISV_LEN      REG(0x13FF0014)
#define ISV_BUF      0x13FF0020u

#define AI_FULL      0x80000000u
#define AI_BUSY      0x40000000u
#define SP_HALT      0x001u
#define SP_BROKE     0x002u
#define SP_TASKDONE  0x200u
#define MI_SP        0x01u

#define W 320
#define H 240
#define CPU_BUFFERS 120     /* ~1,9 s */
#define HLE_BUFFERS 60      /* ~1 s más */
#define BUF_BYTES   (TONE_FRAMES * 4)
#define TIMEOUT     2000000u
#define PHYS(p)     ((uint32_t)(uintptr_t)(p) & 0x1FFFFFFFu)

/* Los escribe el manejador de interrupciones (start.S). */
volatile uint32_t ai_irqs, vi_fields, ai_counts[64];
uint32_t exc_save[4];

/* Fuera de .bss (start.S no los limpia): se usan solo por KSEG1. */
static uint16_t fb[W * H] __attribute__((section(".fb"), aligned(64)));
/* Tres búferes del AI desde 64 bytes después de un límite de 8 KB: ninguno
 * termina en un límite (0x0840, 0x1040, 0x1840 dentro de la página). */
static uint8_t ai_area[64 + 3 * BUF_BYTES] __attribute__((section(".fb"), aligned(8192)));

struct task {
	uint32_t type, flags, ucode_boot, ucode_boot_size, ucode, ucode_size, ucode_data, ucode_data_size;
	uint32_t dram_stack, dram_stack_size, output_buff, output_buff_size, data_ptr, data_size;
	uint32_t yield_data_ptr, yield_data_size;
};

/* Lo que lee el RSP. */
static struct {
	struct task task;
	uint32_t ucode[4];
	char ucode_data[64];
	uint32_t alist[2 * 16 * 2];
	int16_t left[TONE_FRAMES], right[TONE_FRAMES];
} rsp __attribute__((section(".fb"), aligned(64)));

#define R ((volatile __typeof__(rsp) *)((uint32_t)(uintptr_t)&rsp | 0x20000000u))

static volatile uint32_t *uncached32(const void *p)
{
	return (volatile uint32_t *)((uint32_t)(uintptr_t)p | 0x20000000u);
}

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

static uint32_t count(void)
{
	uint32_t v;
	__asm__ volatile("mfc0 %0, $9\n\tnop" : "=r"(v));
	return v;
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

static void enable_interrupts(void)
{
	volatile uint32_t *vec = (volatile uint32_t *)0xA0000180u;
	for (int i = 0; i < 4; i++)
		vec[i] = exc_vector[i];
	__asm__ volatile(".set push\n\t.set mips3\n\tcache 0x10, 0(%0)\n\t.set pop" : : "r"(0x80000180u));
	VI_V_CURRENT = 0;
	AI_STATUS = 0;
	MI_INTR_MASK = 0x20 | 0x80;  /* activar AI y VI */
	c0_set_status(c0_status() | 0x0401);  /* IE | IM2 */
}

/* ---- Video: un color por nota --------------------------------------------- */

static void vi_setup(int pal)
{
	VI_CONTROL = 0;
	VI_ORIGIN = PHYS(fb);
	VI_WIDTH = W;
	VI_V_INTR = 2;
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
	VI_X_SCALE = 0x200;
	VI_Y_SCALE = 0x400;
	VI_CONTROL = 0x3202;  /* RGBA5551 */
}

static void fill_screen(uint32_t note)
{
	static const uint16_t colors[TONE_NOTES] = {  /* RGBA5551 */
		0x07C1, 0xF801, 0x003F, 0xFFC1, 0xF83F, 0x0843,
	};
	uint32_t c = (uint32_t)colors[note] << 16 | colors[note];
	volatile uint32_t *p = uncached32(fb);
	for (int i = 0; i < W * H / 2; i++)
		p[i] = c;
}

/* ---- ISViewer ------------------------------------------------------------- */

static int isv_present(void)
{
	ISV_MAGIC = 0x56495354;
	return ISV_MAGIC == 0x56495354;
}

static char msg[200];
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

/* ---- Tono ----------------------------------------------------------------- */

static struct tone_gen gen;

/* Las muestras del siguiente búfer, intercaladas, en `out` (KSEG1). */
static void tone_cpu(volatile uint32_t *out)
{
	for (uint32_t i = 0; i < TONE_FRAMES; i++) {
		int16_t l, r;
		tone_next(&gen, &l, &r);
		out[i] = (uint32_t)(uint16_t)l << 16 | (uint16_t)r;
	}
}

/* ---- RSP: el tono por la HLE de audio (nead) ------------------------------ */

static uint32_t nlist;

static void acmd(uint32_t w1, uint32_t w2)
{
	R->alist[nlist++] = w1;
	R->alist[nlist++] = w2;
}

static void sp_dma_to(uint32_t mem, const volatile void *src, uint32_t len)
{
	SP_MEM_ADDR = mem;
	SP_DRAM_ADDR = PHYS(src);
	SP_RD_LEN = len - 1;
}

/* Los canales por separado en R->left/right; la tarea los intercala en
 * `out` en dos vueltas de 256 muestras: entrada 0x000/0x200 de la DMEM,
 * suma en 0x400, salida intercalada en 0x600. Devuelve 0 si la tarea no
 * terminó. */
static int tone_hle(volatile uint32_t *out)
{
	for (uint32_t i = 0; i < TONE_FRAMES; i++) {
		int16_t l, r;
		tone_next(&gen, &l, &r);
		R->left[i] = l;
		R->right[i] = r;
	}
	nlist = 0;
	for (uint32_t k = 0; k < 2; k++) {
		uint32_t half = TONE_FRAMES / 2;
		acmd(0x14000000u | 512u << 12 | 0x000, PHYS(&rsp.left[half * k]));   /* LOADBUFF */
		acmd(0x14000000u | 512u << 12 | 0x200, PHYS(&rsp.right[half * k]));
		acmd(0x02000000u | 0x400, 512);                                     /* CLEARBUFF */
		acmd(0x04000000u | 512u << 12, 0x0000u << 16 | 0x400);             /* ADDMIXER */
		acmd(0x0E000000u | 0x10u << 16 | 512, 0x0200u << 16);              /* HILOGAIN ×1 */
		acmd(0x0D000000u | 512u << 12 | 0x600, 0x0400u << 16 | 0x200);     /* INTERLEAVE */
		acmd(0x15000000u | 1024u << 12 | 0x600, PHYS(out) + 1024 * k);     /* SAVEBUFF */
	}

	volatile struct task *t = &R->task;
	t->type = 2;
	t->flags = 0;
	t->ucode_boot = 0;
	t->ucode_boot_size = 0;
	t->ucode = PHYS(rsp.ucode);
	t->ucode_size = sizeof rsp.ucode;
	t->ucode_data = PHYS(rsp.ucode_data);
	t->ucode_data_size = sizeof rsp.ucode_data;
	t->dram_stack = 0;
	t->dram_stack_size = 0;
	t->output_buff = 0;
	t->output_buff_size = 0;
	t->data_ptr = PHYS(rsp.alist);
	t->data_size = 4 * nlist;
	t->yield_data_ptr = 0;
	t->yield_data_size = 0;

	sp_dma_to(0x1000, R->ucode, 16);
	sp_dma_to(0xFC0, t, sizeof(struct task));
	SP_PC = 0;
	SP_STATUS = 0x001 | 0x004 | 0x100 | 0x200 | 0x800 | 0x2000;
	for (uint32_t i = 0; i < TIMEOUT; i++)
		if (MI_INTR & MI_SP) {
			uint32_t st = SP_STATUS;
			SP_STATUS = 0x008;  /* bajar la interrupción SP */
			return (st & (SP_HALT | SP_BROKE | SP_TASKDONE)) == (SP_HALT | SP_BROKE | SP_TASKDONE);
		}
	return 0;
}

/* Cuántas muestras de `out` no son el tono de R->left/right. */
static uint32_t hle_diffs(const volatile uint32_t *out)
{
	uint32_t bad = 0;
	for (uint32_t i = 0; i < TONE_FRAMES; i++)
		if (out[i] != ((uint32_t)(uint16_t)R->left[i] << 16 | (uint16_t)R->right[i]))
			bad++;
	return bad;
}

/* ---- Programa ------------------------------------------------------------- */

int main(void)
{
	uint32_t tv = (BOOT_FLAGS >> 16) & 0xFF;
	static const uint32_t clocks[3] = { 49656530, 48681812, 48628322 };
	/* Count avanza a 46,875 MHz; un búfer dura 512 × 46 875 000 × (dacrate + 1)
	 * / reloj del VI (sin división de 64 bits en la ROM: calculado aparte). */
	static const uint32_t expected[3] = { 750113, 749849, 750180 };
	if (tv > 2)
		tv = 1;
	uint32_t clock = clocks[tv];
	uint32_t dacrate = (clock + TONE_RATE / 2) / TONE_RATE - 1;
	int isv = isv_present();

	static const char marker[] = "Salsa64 ABI NEAD";
	for (unsigned i = 0; i < sizeof rsp.ucode_data; i++)
		R->ucode_data[i] = i < sizeof marker ? marker[i] : 0;
	R->ucode[0] = 0x0000000D;  /* break */
	R->ucode[1] = R->ucode[2] = R->ucode[3] = 0;

	fill_screen(0);
	vi_setup(tv == 0);
	enable_interrupts();

	uint32_t errors = 0, empty = 0, buffers = 0, hle_buffers = 0, hle_bad = 0, hle_fail = 0;
	uint32_t len_drop = 0, shown_note = 0;
	const char *what = "";
	if (AI_STATUS & (AI_FULL | AI_BUSY))
		errors++, what = "AI_STATUS ocupado al empezar";
	AI_DACRATE = dacrate;
	AI_BITRATE = 15;
	AI_CONTROL = 1;

	for (uint32_t b = 0;; b = b == 2 ? 0 : b + 1) {
		volatile uint32_t *buf = uncached32(ai_area + 64 + b * BUF_BYTES);
		int hle = buffers >= CPU_BUFFERS;

		if (!hle) {
			tone_cpu(buf);
		} else {
			if (!tone_hle(buf))
				hle_fail++;
			hle_bad += hle_diffs(buf);
			hle_buffers++;
		}

		/* Esperar lugar en la cola: la interrupción del AI avisa que empezó
		 * el DMA que esperaba. Se espera sobre una variable de la RAM (un
		 * bucle de espera que el emulador puede saltar), no sondeando
		 * AI_STATUS. Desde el segundo búfer el AI no debe quedarse vacío. */
		uint32_t st;
		for (;;) {
			uint32_t n = ai_irqs;
			st = AI_STATUS;
			if (!(st & AI_FULL))
				break;
			while (ai_irqs == n)
				;
		}
		if (buffers >= 2 && !(st & AI_BUSY))
			empty++;
		AI_DRAM_ADDR = PHYS(buf);
		AI_LEN = BUF_BYTES;
		st = AI_STATUS;
		if (buffers == 0 && (st & (AI_BUSY | AI_FULL)) != AI_BUSY)
			errors++, what = "AI_STATUS sin BUSY con un DMA";
		if (buffers == 1 && (st & (AI_BUSY | AI_FULL)) != (AI_BUSY | AI_FULL))
			errors++, what = "AI_STATUS sin FULL con dos DMA";
		buffers++;

		/* AI_LENGTH baja al ritmo del DAC: 50 000 ticks de Count son ~34
		 * muestras (136 bytes) justo después de que empieza un DMA. */
		if (buffers == 10) {
			uint32_t irq = ai_irqs;
			while (ai_irqs == irq)
				;
			uint32_t l0 = AI_LEN, c0 = count();
			while (count() - c0 < 50000)
				;
			uint32_t l1 = AI_LEN;
			len_drop = l0 - l1;
			if (l0 > BUF_BYTES || l1 > l0 || len_drop < 128 || len_drop > 144)
				errors++, what = "AI_LENGTH no baja al ritmo del DAC";
		}

		if (gen.note != shown_note) {
			shown_note = gen.note;
			fill_screen(shown_note);
		}

		if (buffers == CPU_BUFFERS && isv) {
			/* Promedio de las últimas 32 interrupciones. */
			uint32_t n = ai_irqs, per = (ai_counts[(n - 1) & 63] - ai_counts[(n - 33) & 63]) / 32;
			uint32_t want = expected[tv], tol = want / 200;
			if (per + tol < want || per > want + tol)
				errors++, what = "intervalo entre interrupciones del AI";
			if (n + 1 < buffers || n > buffers)
				errors++, what = "una interrupción del AI por DMA";
			if (empty)
				errors++, what = "el AI se quedó sin búfer";
			put(errors ? "audiotest: FALLO" : "audiotest: OK");
			put(" frecuencia=");
			put_u((clock + (dacrate + 1) / 2) / (dacrate + 1));
			put(" Hz búferes=");
			put_u(buffers);
			put(" interrupciones=");
			put_u(n);
			put(" count/búfer=");
			put_u(per);
			put(" esperado=");
			put_u(want);
			put(" len=");
			put_u(len_drop);
			put(" vacíos=");
			put_u(empty);
			if (errors) {
				put(" (");
				put(what);
				put(")");
			}
			put("\n");
			isv_flush();
		}
		if (hle_buffers == HLE_BUFFERS && isv) {
			int ok = !errors && !hle_fail && !hle_bad && !empty;
			put(ok ? "audiotest: HLE OK" : "audiotest: FALLO HLE");
			put(" búferes=");
			put_u(hle_buffers);
			put(" tareas sin terminar=");
			put_u(hle_fail);
			put(" muestras distintas=");
			put_u(hle_bad);
			put(" vacíos=");
			put_u(empty);
			put("\n");
			isv_flush();
		}
	}
}
