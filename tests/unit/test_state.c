/*
 * Salsa64 — pruebas de los estados guardados (core/state.c) y del estado
 * de las HLE entre tareas (rsp/gfx/state.c, rsp/audio/state.c).
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/save.h"
#include "core/state.h"
#include "rsp/audio/audio.h"
#include "rsp/gfx/hle.h"

/* Archivos en memoria: tres (el estado en N y dos finales para comparar). */
#define FILE_MAX (SALSA64_RDRAM_MAX + (256u << 10))

struct memfile {
	uint8_t data[FILE_MAX];
	uint32_t len, pos;
};

static struct memfile fa, fb, fc;
static uint8_t scratch[SALSA64_STATE_SCRATCH] __attribute__((aligned(16)));

static bool mem_write(void *ctx, const void *d, uint32_t n)
{
	struct memfile *f = ctx;
	if (n > FILE_MAX - f->len)
		return false;
	memcpy(f->data + f->len, d, n);
	f->len += n;
	return true;
}

static bool mem_read(void *ctx, void *d, uint32_t n)
{
	struct memfile *f = ctx;
	if (n > f->len - f->pos)
		return false;
	memcpy(d, f->data + f->pos, n);
	f->pos += n;
	return true;
}

static bool save_to(struct memfile *f)
{
	char err[128];
	f->len = f->pos = 0;
	bool ok = salsa64_state_save(&t_n64, NULL, mem_write, f, scratch, err, sizeof err);
	if (!ok)
		t_fail(__FILE__, __LINE__, "guardar: %s", err);
	return ok;
}

static bool load_from(struct memfile *f, char *err, size_t n)
{
	bool broken;
	f->pos = 0;
	return salsa64_state_load(&t_n64, mem_read, f, scratch, err, n, &broken);
}

static bool mem_patch(void *ctx, uint32_t off, const uint8_t *d, uint32_t n)
{
	struct memfile *f = ctx;
	if (off > f->len || n > f->len - off)
		return false;
	memcpy(f->data + off, d, n);
	return true;
}

/* Foto y archivo comprimido (formato 2), con el compresor del hilo. */
static uint8_t snap_small[SALSA64_STATE_SECTIONS] __attribute__((aligned(16)));
static uint8_t snap_ram[SALSA64_RDRAM_MAX] __attribute__((aligned(16)));
static struct salsa64_state_lz zlz;

static bool zip_to(struct memfile *f)
{
	char err[128];
	struct salsa64_state_snap snap = { snap_small, snap_ram, 0, 0 };
	f->len = f->pos = 0;
	bool ok = salsa64_state_capture(&t_n64, NULL, &snap, sizeof snap_small, sizeof snap_ram, err, sizeof err) &&
		  salsa64_state_write(&snap, salsa64_state_compress_host, &zlz, mem_write, mem_patch, f, err,
				      sizeof err);
	if (!ok)
		t_fail(__FILE__, __LINE__, "guardar comprimido: %s", err);
	return ok;
}

/* ROM falsa: solo la cabecera (CRC y título). */
static uint8_t rom[0x1000];

static void rom_on(uint32_t crc1)
{
	memset(rom, 0, sizeof rom);
	salsa64_wr32(rom, 0x80371240);
	salsa64_wr32(rom + 0x10, crc1);
	salsa64_wr32(rom + 0x14, 0x55667788);
	memcpy(rom + 0x20, "SALSA64 PRUEBA      ", 20);
	salsa64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
}

static void rom_off(void)
{
	salsa64_bus_set_rom(&t_n64.bus, NULL, 0);
}

/* Un poco de todo: registros, FPU, COP0, TLB, RCP, DMEM, PIF, RDRAM. */
static void scribble(uint32_t seed)
{
	for (unsigned i = 1; i < 32; i++)
		R[i] = 0x0123456789ABCDEFull * (i + seed);
	CPU->hi = 0x1111 + seed;
	CPU->lo = 0x2222 + seed;
	for (unsigned i = 0; i < 32; i++)
		CPU->fpr[i] = 0x3FF0000000000000ull + i + seed;
	CPU->fcr31 = 0x01000003;
	CPU->epc = 0xFFFFFFFF80001234ull + seed;
	CPU->compare = 0x12345 + seed;
	CPU->count_offset = 77 + seed;
	t_tlb_set(5, 0x0000000000400000ull, T_LO(0x100, 1, 1, 0), T_LO(0x101, 1, 1, 0), 0);
	t_n64.bus.vi.regs[SALSA64_VI_ORIGIN] = 0x100000 + seed;
	t_n64.bus.ai.addr[0] = 0x200000 + seed;
	t_n64.bus.pi[SALSA64_PI_DRAM_ADDR] = 0x300000 + seed;
	t_n64.bus.si[SALSA64_SI_DRAM_ADDR] = 0x400000 + seed;
	t_n64.bus.mi.mask = 0x15;
	t_n64.bus.dp[SALSA64_DP_STATUS] = 0x80 + seed;
	for (unsigned i = 0; i < SALSA64_SP_MEM_SIZE; i++)
		t_n64.bus.sp_mem[i] = (uint8_t)(i * 7 + seed);
	for (unsigned i = 0; i < SALSA64_PIF_RAM_SIZE; i++)
		t_n64.bus.pif_ram[i] = (uint8_t)(i + seed);
	t_n64.bus.pif.held[1] = SALSA64_PAD_A;
	for (uint32_t pa = 0x10000; pa < 0x10000 + 4096; pa += 4)
		t_wr32(pa, pa * 2654435761u + seed);
	t_wr32(SALSA64_RDRAM_MAX - 4, 0xFEEDF00D + seed);
}

static void state_round_trip(void)
{
	rom_on(0x11223344);
	scribble(0);
	uint64_t gpr9 = R[9], fpr3 = CPU->fpr[3], vi_when = t_n64.sched.when[SALSA64_EV_VI_FIELD];
	CHECK(save_to(&fa));
	CHECK_EQ(fa.len, salsa64_state_size(&t_n64, false));

	/* Otra máquina encima: reset y garabatos distintos. */
	salsa64_n64_reset(&t_n64);
	scribble(99);
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(R[9], gpr9);
	CHECK_EQ(CPU->fpr[3], fpr3);
	CHECK_EQ(CPU->tlb[5].entry_lo[0] >> 6, 0x100);
	CHECK_EQ(t_n64.bus.vi.regs[SALSA64_VI_ORIGIN], 0x100000);
	CHECK_EQ(t_n64.bus.sp_mem[100], (uint8_t)(700));
	CHECK_EQ(t_n64.bus.pif.held[1], SALSA64_PAD_A);
	CHECK_EQ(t_rd32(SALSA64_RDRAM_MAX - 4), 0xFEEDF00D);
	CHECK_EQ(t_n64.sched.when[SALSA64_EV_VI_FIELD], vi_when);
	CHECK(CPU->redirect);

	/* Guardar otra vez da el mismo archivo, byte por byte. */
	CHECK(save_to(&fb));
	CHECK_EQ(fb.len, fa.len);
	CHECK(memcmp(fa.data, fb.data, fa.len) == 0);
	rom_off();
}

/* Estado comprimido (formato 2): se carga igual que uno sin comprimir y
 * deja la máquina idéntica (guardar de nuevo, sin comprimir, da el mismo
 * archivo), ocupa menos y sus bloques incompresibles van tal cual. */
static void state_zip_round_trip(void)
{
	rom_on(0x11223344);
	scribble(0);
	/* Un tramo de RDRAM sin patrón (bloques "stored") y el resto casi cero. */
	uint32_t x = 0x12345678;
	for (uint32_t pa = 0x200000; pa < 0x240000; pa += 4) {
		x = x * 1664525u + 1013904223u;
		t_wr32(pa, x);
	}
	CHECK(save_to(&fa));
	CHECK(zip_to(&fb));
	CHECK(fb.len < fa.len / 4);
	CHECK(fb.len > 0x40000);  /* los bloques sin patrón no se achican */
	CHECK_EQ(salsa64_rd32(fb.data + 8), SALSA64_STATE_VERSION);
	CHECK_EQ(salsa64_rd32(fb.data + 16), fb.len);
	CHECK(salsa64_rd32(fb.data + 60) & SALSA64_STATE_FLAG_ZIP);

	salsa64_n64_reset(&t_n64);
	scribble(99);
	char err[128] = "";
	CHECK(load_from(&fb, err, sizeof err));
	CHECK(save_to(&fc));
	CHECK_EQ(fc.len, fa.len);
	CHECK(memcmp(fa.data, fc.data, fa.len) == 0);

	/* Y los viejos, sin comprimir, siguen cargando. */
	salsa64_n64_reset(&t_n64);
	CHECK(load_from(&fa, err, sizeof err));
	CHECK(save_to(&fc));
	CHECK(memcmp(fa.data, fc.data, fa.len) == 0);
	rom_off();
}

/* Un estado comprimido dañado no desborda nada: falla, y si ya empezó a
 * escribir la RDRAM lo dice (broken). Cortado antes de RDRZ no toca nada. */
static void state_zip_corrupt(void)
{
	rom_on(0x11223344);
	scribble(0);
	CHECK(zip_to(&fa));
	char err[160];
	bool broken;
	uint32_t at = SALSA64_STATE_HEADER;
	while (at + 8 <= fa.len && memcmp(fa.data + at, "RDRZ", 4) != 0)
		at += 8 + salsa64_rd32(fa.data + at + 4);
	CHECK(at + 8 < fa.len);
	uint32_t first = at + 8 + 12;   /* encabezado del primer bloque */

	/* Largo de bloque absurdo: falla antes de tocar nada. */
	memcpy(fb.data, fa.data, fa.len);
	fb.len = fa.len;
	salsa64_wr32(fb.data + first, 0x7FFFFFFFu);
	fb.pos = 0;
	R[9] = 42;
	CHECK(!salsa64_state_load(&t_n64, mem_read, &fb, scratch, err, sizeof err, &broken));
	CHECK(!broken);
	CHECK_EQ(R[9], 42);

	/* Un byte roto dentro de un bloque comprimido: o falla o da otra RDRAM,
	 * pero sin salirse de ningún búfer (ASan lo vigila). */
	for (uint32_t k = 0; k < 64; k++) {
		memcpy(fb.data, fa.data, fa.len);
		fb.len = fa.len;
		fb.data[first + 4 + (k * 7) % 40] ^= (uint8_t)(1u << (k & 7));
		fb.pos = 0;
		salsa64_state_load(&t_n64, mem_read, &fb, scratch, err, sizeof err, &broken);
	}

	/* Cortado a mitad de los bloques. */
	memcpy(fb.data, fa.data, fa.len);
	fb.len = fa.len - 1000;
	fb.pos = 0;
	CHECK(!salsa64_state_load(&t_n64, mem_read, &fb, scratch, err, sizeof err, &broken));
	rom_off();
}

/* Un programa que mezcla enteros, FPU, Count y almacenamientos (el VI y
 * Count/Compare siguen con sus eventos; sin interrupciones). */
static void load_program(void)
{
	R[T0] = T_DATA;
	R[T1] = 0;
	R[T2] = 0x9E3779B9;
	CPU->fpr[2] = t_dbits(1.0);
	CPU->fpr[4] = t_dbits(0.1);
	T_LOAD(ADDIU(T1, T1, 1),
	       XOR(T2, T2, T1),
	       SLL(T3, T2, 3),
	       ADDU(T2, T2, T3),
	       ANDI(T5, T1, 0xFF),
	       SLL(T5, T5, 2),
	       ADDU(T5, T5, T0),
	       SW(T2, 0, T5),
	       ADD_F(F_D, 2, 2, 4),
	       MFC0(T6, SALSA64_CP0_COUNT),
	       ADDU(T2, T2, T6),
	       J(T_CODE & 0x0FFFFFFF),
	       NOP);
}

static void state_determinism(void)
{
	rom_on(0x11223344);
	load_program();
	salsa64_n64_run(&t_n64, 3000000);   /* hasta N (pasa un campo del VI) */
	CHECK(save_to(&fa));
	salsa64_n64_run(&t_n64, 2500000);   /* de corrido hasta M */
	CHECK(save_to(&fb));
	uint64_t pc_m = CPU->pc;

	/* Otra corrida desde cero que carga el estado de N y sigue hasta M. */
	salsa64_n64_reset(&t_n64);
	scribble(5);
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	salsa64_n64_run(&t_n64, 2500000);
	CHECK(save_to(&fc));
	CHECK_EQ(CPU->pc, pc_m);
	CHECK_EQ(fc.len, fb.len);
	if (fc.len == fb.len && memcmp(fb.data, fc.data, fb.len) != 0) {
		uint32_t i = 0;
		while (fb.data[i] == fc.data[i])
			i++;
		t_fail(__FILE__, __LINE__, "los estados en M difieren desde el byte %u", i);
	}
	CHECK(t_n64.bus.vi.fields >= 2);
	rom_off();
}

static void state_rejects(void)
{
	rom_on(0x11223344);
	scribble(0);
	CHECK(save_to(&fa));
	uint64_t gpr9 = R[9];
	char err[160];

	/* Otro juego. */
	rom_on(0x0BADC0DE);
	R[9] = 42;
	CHECK(!load_from(&fa, err, sizeof err));
	CHECK(strstr(err, "otro juego") != NULL);
	CHECK_EQ(R[9], 42);
	rom_on(0x11223344);

	/* Versión más nueva y anterior a la mínima. */
	salsa64_wr32(fa.data + 8, SALSA64_STATE_VERSION + 1);
	CHECK(!load_from(&fa, err, sizeof err));
	CHECK(strstr(err, "más nueva") != NULL);
	salsa64_wr32(fa.data + 8, SALSA64_STATE_MIN_VERSION - 1);
	CHECK(!load_from(&fa, err, sizeof err));
	CHECK(strstr(err, "anterior") != NULL);
	salsa64_wr32(fa.data + 8, SALSA64_STATE_VERSION);

	/* No es un estado. */
	fa.data[0] ^= 0xFF;
	CHECK(!load_from(&fa, err, sizeof err));
	fa.data[0] ^= 0xFF;

	/* Otro tamaño de RDRAM. */
	salsa64_wr32(fa.data + 52, 4u << 20);
	CHECK(!load_from(&fa, err, sizeof err));
	CHECK(strstr(err, "MB") != NULL);
	salsa64_wr32(fa.data + 52, SALSA64_RDRAM_MAX);

	/* Cortado antes de la RDRAM: no toca nada. */
	uint32_t len = fa.len;
	fa.len = 4096;
	CHECK(!load_from(&fa, err, sizeof err));
	CHECK_EQ(R[9], 42);
	fa.len = len;

	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(R[9], gpr9);
	rom_off();
}

/* Una sección más corta (un estado de antes de agregar campos al final):
 * lo que falta queda en cero y el resto carga. */
static void state_short_section(void)
{
	rom_on(0x11223344);
	scribble(0);
	CPU->idle_cycles = 1234;  /* el último campo de CPU0 */
	CHECK(save_to(&fa));
	uint32_t pos = SALSA64_STATE_HEADER, cut = 8;
	while (pos < fa.len && salsa64_rd32(fa.data + pos) != (('C' << 24) | ('P' << 16) | ('U' << 8) | '0'))
		pos += 8 + salsa64_rd32(fa.data + pos + 4);
	CHECK(pos < fa.len);
	uint32_t len = salsa64_rd32(fa.data + pos + 4);
	uint32_t end = pos + 8 + len;
	memmove(fa.data + end - cut, fa.data + end, fa.len - end);
	fa.len -= cut;
	salsa64_wr32(fa.data + pos + 4, len - cut);
	salsa64_wr32(fa.data + 16, fa.len);
	uint64_t gpr9 = R[9];
	R[9] = 0;
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(R[9], gpr9);
	CHECK_EQ(CPU->idle_cycles, 0);
	rom_off();
}

/* El guardado del cartucho va en el estado y vuelve al cargarlo (marcado
 * para escribirlo al disco). */
static void state_cart_save(void)
{
	static struct salsa64_save sv;
	rom_on(0x11223344);
	salsa64_save_init(&sv, SALSA64_SAVE_EEPROM_4K);
	salsa64_save_attach(&t_n64.bus, &sv);
	sv.data[3] = 0xAB;
	CHECK(save_to(&fa));
	sv.data[3] = 0x00;
	uint32_t writes = sv.writes;
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(sv.data[3], 0xAB);
	CHECK(sv.writes != writes);
	salsa64_save_attach(&t_n64.bus, NULL);
	rom_off();
}

/* FlashRAM: los 128 KB van en CART y el estado del chip en FLSH, con su
 * versión; una versión que no se conoce deja el chip como al encender (los
 * datos se cargan igual). */
static void state_flash(void)
{
	static struct salsa64_save sv;
	rom_on(0x11223344);
	salsa64_save_init(&sv, SALSA64_SAVE_FLASHRAM);
	salsa64_save_attach(&t_n64.bus, &sv);
	sv.data[0x1FFFF] = 0x5A;
	sv.flash.mode = SALSA64_FLASH_LOAD_PAGE;
	sv.flash.erase = 1;
	sv.flash.erase_sector = 6;
	sv.flash.page[5] = 0x42;
	sv.flash.commands = 77;
	CHECK(save_to(&fa));
	CHECK_EQ(fa.len, salsa64_state_size(&t_n64, false));

	salsa64_save_init(&sv, SALSA64_SAVE_FLASHRAM);
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(sv.data[0x1FFFF], 0x5A);
	CHECK_EQ(sv.flash.mode, SALSA64_FLASH_LOAD_PAGE);
	CHECK_EQ(sv.flash.erase, 1);
	CHECK_EQ(sv.flash.erase_sector, 6);
	CHECK_EQ(sv.flash.page[5], 0x42);
	CHECK_EQ(sv.flash.page[6], 0xFF);
	CHECK_EQ(sv.flash.commands, 77);
	CHECK(salsa64_save_dirty(&sv));  /* al disco con el ritmo de siempre */

	uint32_t at = SALSA64_STATE_HEADER;
	while (at + 12 <= fa.len && memcmp(fa.data + at, "FLSH", 4) != 0)
		at++;
	CHECK(at + 12 <= fa.len);
	CHECK_EQ(salsa64_rd32(fa.data + at + 8), 1);
	salsa64_wr32(fa.data + at + 8, 99);
	salsa64_save_init(&sv, SALSA64_SAVE_FLASHRAM);
	sv.flash.mode = SALSA64_FLASH_STATUS;
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(sv.flash.mode, SALSA64_FLASH_READ_ARRAY);
	CHECK_EQ(sv.flash.page[5], 0xFF);
	CHECK_EQ(sv.data[0x1FFFF], 0x5A);
	salsa64_save_attach(&t_n64.bus, NULL);
	rom_off();
}

/* Una tarea gráfica en vuelo se recoge antes de guardar: su fin llega a su
 * hora con el resultado, sin volver a llamar al backend. */
static int fin_calls;
static uint32_t fin_flags;

static void fin_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	(void)ctx, (void)task, (void)gbi;
}

static uint32_t fin_finish(void *ctx)
{
	(void)ctx;
	fin_calls++;
	return fin_flags;
}

static const struct salsa64_gfx_backend fin_backend = { .start = fin_start, .finish = fin_finish };

static void state_rsp_quiesce(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	rom_on(0x11223344);
	salsa64_rsp_set_gfx(bus, &fin_backend);
	fin_calls = 0;
	fin_flags = SALSA64_GFX_FULLSYNC;
	/* Tarea gráfica arrancada a mano. */
	bus->rsp.busy = SALSA64_TASK_GFX;
	bus->sp.regs[SALSA64_SP_STATUS] &= ~(uint32_t)SALSA64_SP_STATUS_HALT;
	uint64_t end = CPU->cycles + 5000;
	salsa64_sched_set(&t_n64.sched, SALSA64_EV_SP_TASK, end);

	CHECK(salsa64_rsp_quiesce(bus));
	CHECK_EQ(fin_calls, 1);
	CHECK(bus->rsp.collected);
	CHECK(salsa64_rsp_quiesce(bus));  /* una vez sola */
	CHECK_EQ(fin_calls, 1);
	CHECK(save_to(&fa));

	/* El fin llega a su hora, con DP por FULLSYNC. */
	salsa64_sched_dispatch(&t_n64.sched, end - 1);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	salsa64_sched_dispatch(&t_n64.sched, end);
	CHECK_EQ(fin_calls, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(bus->sp.regs[SALSA64_SP_STATUS] & SALSA64_SP_STATUS_TASKDONE);
	CHECK(t_n64.sched.when[SALSA64_EV_DP_SYNC] != SALSA64_SCHED_NEVER);

	/* Cargado: la tarea sigue en curso y recogida, y termina igual. */
	char err[128] = "";
	CHECK(load_from(&fa, err, sizeof err));
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	CHECK(bus->rsp.collected);
	CHECK_EQ(t_n64.sched.when[SALSA64_EV_SP_TASK], end);
	salsa64_sched_dispatch(&t_n64.sched, end);
	CHECK_EQ(fin_calls, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK(t_n64.sched.when[SALSA64_EV_DP_SYNC] != SALSA64_SCHED_NEVER);

	/* Sin recoger no se guarda. */
	bus->rsp.busy = SALSA64_TASK_GFX;
	bus->rsp.collected = false;
	fa.len = 0;
	CHECK(!salsa64_state_save(&t_n64, NULL, mem_write, &fa, scratch, err, sizeof err));
	salsa64_rsp_set_gfx(bus, NULL);
	rom_off();
}

/* Estado de la HLE gráfica entre tareas: ida y vuelta del formato. */
static struct gfx_state ga, gb;

static void gfx_state_round_trip(void)
{
	static uint8_t blob[GFX_STATE_BYTES];
	gfx_init(&ga);
	gfx_init(&gb);
	ga.oh = 0x00100CF0;
	ga.cc_lo = 0xFFFDF6FB;
	ga.prim = 0x80402010;
	ga.scissor[2] = 1234;
	ga.cimg_addr = 0x123456;
	ga.tiles[7].tmem = 0x100;
	ga.tiles[3].lrt = 0x3FF;
	for (unsigned i = 0; i < sizeof ga.tmem; i++)
		ga.tmem[i] = (uint8_t)(i ^ 0x5A);
	gfx_state_save(&ga, blob);
	uint32_t gen = gb.tex_gen;
	CHECK(gfx_state_load(&gb, blob));
	CHECK_EQ(gb.oh, ga.oh);
	CHECK_EQ(gb.cc_lo, ga.cc_lo);
	CHECK_EQ(gb.prim, ga.prim);
	CHECK_EQ(gb.scissor[2], 1234);
	CHECK_EQ(gb.cimg_addr, 0x123456);
	CHECK_EQ(gb.tiles[7].tmem, 0x100);
	CHECK_EQ(gb.tiles[3].lrt, 0x3FF);
	CHECK(memcmp(gb.tmem, ga.tmem, sizeof ga.tmem) == 0);
	CHECK(gb.tex_gen != gen);
	blob[3] ^= 1;  /* otro formato */
	CHECK(!gfx_state_load(&gb, blob));
}

static struct aud_state aa, ab;

static void audio_state_round_trip(void)
{
	static uint8_t blob[AUD_STATE_BYTES];
	aud_init(&aa);
	aud_init(&ab);
	for (unsigned i = 0; i < sizeof aa.dmem; i++)
		aa.dmem[i] = (uint8_t)(i * 3);
	for (unsigned i = 0; i < sizeof aa.mp3; i++)
		aa.mp3[i] = (uint8_t)(i * 5);
	aa.abi1.segments[15] = 0x00ABCDEF;
	aa.abi1.rate[1] = -123456;
	aa.abi1.table[255] = -2;
	aa.nead.env_steps[2] = 0xBEEF;
	aa.nead.filter_lut[1] = 0xCAFE;
	aa.naudio.vol[1] = -7;
	aa.naudio.loop = 0x00102030;
	aud_state_save(&aa, blob);
	CHECK(aud_state_load(&ab, blob));
	CHECK(memcmp(ab.dmem, aa.dmem, sizeof aa.dmem) == 0);
	CHECK(memcmp(ab.mp3, aa.mp3, sizeof aa.mp3) == 0);
	CHECK(memcmp(&ab.abi1, &aa.abi1, sizeof aa.abi1) == 0);
	CHECK(memcmp(&ab.nead, &aa.nead, sizeof aa.nead) == 0);
	CHECK(memcmp(&ab.naudio, &aa.naudio, sizeof aa.naudio) == 0);
}

static void state_filename(void)
{
	char name[64];
	rom_on(0x11223344);
	salsa64_state_filename(rom, 3, name, sizeof name);
	CHECK(strcmp(name, "SALSA64_PRUEBA-11223344.st3") == 0);
	rom_off();
}

void t_suite_state(void)
{
	RUN(state_round_trip);
	RUN(state_zip_round_trip);
	RUN(state_zip_corrupt);
	RUN(state_determinism);
	RUN(state_rejects);
	RUN(state_short_section);
	RUN(state_cart_save);
	RUN(state_flash);
	RUN(state_rsp_quiesce);
	RUN(gfx_state_round_trip);
	RUN(audio_state_round_trip);
	RUN(state_filename);
}
