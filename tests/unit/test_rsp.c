/*
 * Salsa64 — pruebas del DMA del SP y de las tareas del RSP (rsp.c).
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "core/rom.h"

#define SP_REG(r) (0x04040000u + 4 * (r))

static void wr(uint32_t pa, uint32_t v)
{
	salsa64_bus_write32(&t_n64.bus, pa, v, ~0u);
}

static uint32_t rd(uint32_t pa)
{
	return salsa64_bus_read32(&t_n64.bus, pa);
}

static void sp_dma_rows(void)
{
	for (unsigned k = 0; k < 0x100; k += 4)
		t_wr32(0x3000 + k, 0xA0000000u + k);
	/* RDRAM → DMEM: 3 filas de 16 bytes saltando 8 entre filas. */
	wr(SP_REG(SALSA64_SP_MEM_ADDR), 0x0100);
	wr(SP_REG(SALSA64_SP_DRAM_ADDR), 0x3000);
	wr(SP_REG(SALSA64_SP_RD_LEN), (8u << 20) | (2u << 12) | 15);
	CHECK_EQ(rd(0x04000100), 0xA0000000);
	CHECK_EQ(rd(0x0400010C), 0xA000000C);
	CHECK_EQ(rd(0x04000110), 0xA0000018);  /* segunda fila: 16 + 8 */
	CHECK_EQ(rd(0x04000120), 0xA0000030);
	CHECK_EQ(rd(0x0400012C), 0xA000003C);
	CHECK_EQ(rd(0x04000130), 0);
	/* Las direcciones quedan al final; el largo se lee 0xFF8 con cuenta 0. */
	CHECK_EQ(rd(SP_REG(SALSA64_SP_MEM_ADDR)), 0x0130);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_DRAM_ADDR)), 0x3048);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_RD_LEN)), (8u << 20) | 0xFF8);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_WR_LEN)), (8u << 20) | 0xFF8);

	/* IMEM → RDRAM, largo no múltiplo de 8 (se redondea hacia arriba), y
	 * la dirección del SP da la vuelta dentro de la IMEM. */
	wr(0x04001FF8, 0x11111111);
	wr(0x04001FFC, 0x22222222);
	wr(0x04001000, 0x33333333);
	wr(SP_REG(SALSA64_SP_MEM_ADDR), 0x1FF8);
	wr(SP_REG(SALSA64_SP_DRAM_ADDR), 0x4000);
	wr(SP_REG(SALSA64_SP_WR_LEN), 11);
	CHECK_EQ(t_rd32(0x4000), 0x11111111);
	CHECK_EQ(t_rd32(0x4004), 0x22222222);
	CHECK_EQ(t_rd32(0x4008), 0x33333333);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_MEM_ADDR)), 0x1008);
}

/* Backend gráfico de mentira: cuenta llamadas y pide sincronizar el RDP. */
static struct {
	int starts, finishes;
	struct salsa64_task task;
	enum salsa64_gbi gbi;
} fake;

static void fake_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	(void)ctx;
	fake.starts++;
	fake.task = *task;
	fake.gbi = gbi;
}

static uint32_t fake_finish(void *ctx)
{
	(void)ctx;
	fake.finishes++;
	return SALSA64_GFX_FULLSYNC;
}

static const struct salsa64_gfx_backend fake_backend = { NULL, fake_start, fake_finish, NULL };

/* Backend que tarda: `done` dice que no las primeras `fake_busy` veces y
 * anota si el núcleo le avisó que el CPU estaba en un bucle de espera. */
static int fake_busy, fake_dones, fake_idle_seen;

static bool fake_done(void *ctx, bool idle)
{
	(void)ctx;
	fake_dones++;
	fake_idle_seen += idle;
	return fake_busy-- <= 0;
}

static const struct salsa64_gfx_backend slow_backend = { NULL, fake_start, fake_finish, fake_done };

static void put_task(uint32_t type, uint32_t ucode_data, uint32_t dl)
{
	uint8_t *d = t_n64.bus.sp_mem + SALSA64_TASK_DMEM;
	memset(d, 0, 64);
	salsa64_wr32(d + 0x00, type);
	salsa64_wr32(d + 0x10, 0x80100000);       /* ucode */
	salsa64_wr32(d + 0x18, 0x80000000 | ucode_data);
	salsa64_wr32(d + 0x1C, 0x800);
	salsa64_wr32(d + 0x30, 0x80000000 | dl);  /* data_ptr */
	salsa64_wr32(d + 0x34, 0x40);
}

static void put_text(uint32_t pa, const char *s)
{
	memcpy(t_n64.bus.rdram + pa, s, strlen(s));
}

static void rsp_gfx_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	salsa64_rsp_set_gfx(bus, &fake_backend);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);  /* quitar HALT y BROKE, int. en BREAK */

	CHECK_EQ(fake.starts, 1);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DEX2);
	CHECK_EQ(fake.task.data_ptr, 0x80006000);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
	CHECK_EQ(bus->mi.intr, 0);

	/* Fin de la tarea: BROKE, HALT, TASKDONE e interrupción SP; luego la DP. */
	uint64_t t0 = CPU->cycles;
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles);
	CHECK_EQ(fake.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, t0 + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));

	/* F3D también va al backend. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);  /* bajar la interrupción SP */
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI Fast3D");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 2);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3D);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 2);

	/* Y las variantes de Rare. */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI F3DEXBG");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 3);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DEXBG);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 3);
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	bus->mi.intr = 0;
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, "Salsa64 GBI F3DPD");
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 4);
	CHECK_EQ(fake.gbi, SALSA64_GBI_F3DPD);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.gfx_cycles + bus->rsp.dp_cycles);
	CHECK_EQ(fake.finishes, 4);

	/* Microcódigo desconocido: no llega al backend y se detiene en el acto;
	 * la interrupción del DP llega igual (quien espere el fin del RDP no
	 * se cuelga). */
	wr(SP_REG(SALSA64_SP_STATUS), 0x8);
	wr(0x04300000, 0x0800);  /* MI_MODE: bajar la interrupción DP */
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 4);
	CHECK_EQ(bus->rsp.stats.unknown_gbi, 1);
	CHECK_EQ(bus->rsp.stats.unknown_data, 0x80005000);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_BROKE);
	CHECK_EQ(bus->mi.intr & (1u << SALSA64_IRQ_DP), 0);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.dp_cycles);
	CHECK(bus->mi.intr & (1u << SALSA64_IRQ_DP));
	salsa64_rsp_set_gfx(bus, NULL);
}

/* El fin de la tarea se vuelve a programar mientras el backend no termina;
 * el CPU sigue sin HALT, otro arranque se ignora y TASKDONE llega cuando
 * el backend dice que sí. */
static void rsp_gfx_task_polls(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	memset(&fake, 0, sizeof fake);
	fake_busy = 3;
	fake_dones = fake_idle_seen = 0;
	salsa64_rsp_set_gfx(bus, &slow_backend);
	put_text(0x5000 + 0x123, "Salsa64 GBI F3DEX2");
	put_task(SALSA64_TASK_GFX, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(fake.starts, 1);

	uint64_t t0 = CPU->cycles, t = t0 + bus->rsp.gfx_cycles;
	for (int i = 0; i < 3; i++) {
		salsa64_sched_dispatch(&t_n64.sched, t);
		CHECK_EQ(fake.finishes, 0);
		CHECK_EQ(bus->rsp.busy, SALSA64_TASK_GFX);
		CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
		CHECK_EQ(bus->rsp.stats.polls, (uint32_t)(i + 1));
		CHECK_EQ(t_n64.sched.when[SALSA64_EV_SP_TASK], t + bus->rsp.poll_cycles);
		/* Con la tarea en curso no arranca otra: sp.c solo arranca al
		 * quitar un HALT puesto, y aunque llegara aquí se ignora. */
		wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
		salsa64_rsp_start(bus);
		CHECK_EQ(fake.starts, 1);
		CHECK_EQ(bus->rsp.stats.starts_busy, (uint32_t)(i + 1));
		t += bus->rsp.poll_cycles;
	}
	CHECK_EQ(fake_idle_seen, 0);  /* el CPU no saltó ciclos */
	/* Un salto de bucle de espera entre sondeos se le avisa al backend. */
	CPU->idle_cycles += 5000;
	fake_busy = 1;
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(fake_idle_seen, 1);
	CHECK_EQ(fake.finishes, 0);
	t += bus->rsp.poll_cycles;
	salsa64_sched_dispatch(&t_n64.sched, t);
	CHECK_EQ(fake_idle_seen, 1);  /* sin más saltos, sin aviso */
	CHECK_EQ(fake.finishes, 1);
	CHECK_EQ(bus->rsp.busy, 0);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, t + bus->rsp.dp_cycles);
	CHECK_EQ(bus->mi.intr, (1u << SALSA64_IRQ_SP) | (1u << SALSA64_IRQ_DP));
	CHECK_EQ(fake_dones, 5);
	salsa64_rsp_set_gfx(bus, NULL);
}

static void rsp_audio_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	put_task(SALSA64_TASK_AUDIO, 0x5000, 0x6000);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(bus->rsp.busy, SALSA64_TASK_AUDIO);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + bus->rsp.audio_cycles);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);
	salsa64_sched_dispatch(&t_n64.sched, CPU->cycles + 1000000);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);  /* sin DP */
}

/* Una tarea de libultra sin HLE (como la de tipo 4 que manda Zelda OoT al
 * arrancar) termina con TASKDONE: sin él, libultra la toma por un BREAK
 * suelto y nadie despierta. Lo que no parece tarea, sin señales. */
static void rsp_other_task(void)
{
	struct salsa64_bus *bus = &t_n64.bus;
	put_task(4, 0x5000, 0x6000);
	salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 0x14, 0x20);   /* ucode_size */
	salsa64_wr32(bus->sp_mem + SALSA64_TASK_DMEM + 0x0C, 0x3E8);  /* ucode_boot_size */
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4 | 0x100);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE | SALSA64_SP_STATUS_INTR_BREAK |
		 SALSA64_SP_STATUS_TASKDONE);
	CHECK_EQ(bus->mi.intr, 1u << SALSA64_IRQ_SP);

	wr(SP_REG(SALSA64_SP_STATUS), 0x8 | 0x2000);  /* bajar interrupción y TASKDONE */
	memset(bus->sp_mem + SALSA64_TASK_DMEM, 0, 64);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_TASKDONE, 0);
}

static enum salsa64_gbi detect(const char *text)
{
	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	put_text(0x5000 + 0x40, text);
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	return salsa64_rsp_detect_gbi(&t_n64.bus, &task);
}

/* Los 4 bytes que, tras el estado `state` del CRC-32 (sin la inversión
 * final), lo llevan a `target`: el CRC hacia atrás 32 bits. */
static void crc32_force(uint32_t state, uint32_t target, uint8_t out[4])
{
	uint32_t r = ~target;
	for (int i = 0; i < 32; i++)
		r = (r & 0x80000000u) ? ((r ^ 0xEDB88320u) << 1) | 1 : r << 1;
	r ^= state;
	for (int i = 0; i < 4; i++)
		out[i] = (uint8_t)(r >> (8 * i));
}

/* Microcódigo sin texto (Perfect Dark): por el CRC-32 de su código. Los
 * datos son sintéticos (ceros y 4 bytes calculados para dar ese CRC). */
static void rsp_detect_gbi_crc(void)
{
	const uint32_t code = 0x8000, size = 0x1000;
	uint8_t *p = t_n64.bus.rdram + code;
	memset(p, 0, size);
	uint32_t state = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < size - 4; i++) {
		state ^= p[i];
		for (int k = 0; k < 8; k++)
			state = (state >> 1) ^ (0xEDB88320u & (0u - (state & 1)));
	}
	crc32_force(state, 0xC543D0A8u, p + size - 4);
	CHECK_EQ(salsa64_crc32(p, size), 0xC543D0A8u);

	struct salsa64_task task;
	memset(&task, 0, sizeof task);
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	task.ucode = 0x80000000u | code;
	task.ucode_size = size;
	task.ucode_data = 0x80005000;
	task.ucode_data_size = 0x800;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_F3DPD);
	task.ucode_size = 0xFF8;  /* otro tamaño: no es él */
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
	task.ucode_size = size;
	p[17] ^= 1;
	CHECK_EQ(salsa64_rsp_detect_gbi(&t_n64.bus, &task), SALSA64_GBI_UNKNOWN);
	memset(p, 0, size);
}

static void rsp_detect_gbi(void)
{
	/* Solo el formato de los textos de identificación, sin copiarlos. */
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       fifo 2.08 "), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DZEX.NoN  fifo 2.06H "), SALSA64_GBI_F3DZEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       xbus 1.23 "), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("RSP Gfx ucode F3DEX         0.95 "), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("RSP Gfx ucode S2DEX  fifo 2.05 "), SALSA64_GBI_S2DEX);
	CHECK_EQ(detect("RSP Gfx ucode F3DEXBG.NoN fifo 2.08 "), SALSA64_GBI_F3DEXBG);
	CHECK_EQ(detect("RSP SW Version: 2.0X"), SALSA64_GBI_F3D);
	CHECK_EQ(detect("Salsa64 GBI F3DEX2"), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("Salsa64 GBI F3DEX1"), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("Salsa64 GBI Fast3D"), SALSA64_GBI_F3D);
	CHECK_EQ(detect("Salsa64 GBI F3DPD"), SALSA64_GBI_F3DPD);
	CHECK_EQ(detect("Salsa64 GBI F3DEXBG"), SALSA64_GBI_F3DEXBG);
	CHECK_EQ(detect("nada"), SALSA64_GBI_UNKNOWN);

	/* El texto para los logs: hasta el primer carácter no imprimible. */
	char text[32];
	detect("RSP SW Version: 2.0X, 01-01-96");
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, sizeof text);
	CHECK(strcmp(text, "RSP SW Version: 2.0X, 01-01-96") == 0);
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, 8);
	CHECK(strcmp(text, "RSP SW ") == 0);
	detect("nada");
	salsa64_rsp_ucode_text(&t_n64.bus, 0x80005000, text, sizeof text);
	CHECK_EQ(text[0], 0);
}

void t_suite_rsp(void)
{
	RUN(sp_dma_rows);
	RUN(rsp_gfx_task);
	RUN(rsp_gfx_task_polls);
	RUN(rsp_audio_task);
	RUN(rsp_other_task);
	RUN(rsp_detect_gbi);
	RUN(rsp_detect_gbi_crc);
}
