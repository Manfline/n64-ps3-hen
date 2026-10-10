/*
 * Salsa64 — pruebas del DMA del SP y de las tareas del RSP (rsp.c).
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"

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

static const struct salsa64_gfx_backend fake_backend = { NULL, fake_start, fake_finish };

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

	/* Microcódigo desconocido: no llega al backend y se detiene en el acto. */
	memset(t_n64.bus.rdram + 0x5000, 0, 0x800);
	wr(SP_REG(SALSA64_SP_STATUS), 0x1 | 0x4);
	CHECK_EQ(fake.starts, 1);
	CHECK_EQ(bus->rsp.stats.unknown_gbi, 1);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_BROKE);
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

static void rsp_detect_gbi(void)
{
	/* Solo el formato de los textos de identificación, sin copiarlos. */
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       fifo 2.08 "), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DZEX.NoN  fifo 2.06H "), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("RSP Gfx ucode F3DEX       xbus 1.23 "), SALSA64_GBI_F3DEX);
	CHECK_EQ(detect("RSP Gfx ucode S2DEX  fifo 2.05 "), SALSA64_GBI_S2DEX);
	CHECK_EQ(detect("RSP SW Version: 2.0X"), SALSA64_GBI_F3D);
	CHECK_EQ(detect("Salsa64 GBI F3DEX2"), SALSA64_GBI_F3DEX2);
	CHECK_EQ(detect("nada"), SALSA64_GBI_UNKNOWN);
}

void t_suite_rsp(void)
{
	RUN(sp_dma_rows);
	RUN(rsp_gfx_task);
	RUN(rsp_audio_task);
	RUN(rsp_detect_gbi);
}
