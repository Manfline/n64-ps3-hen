/*
 * Retro64 — pruebas del bus, orden de bytes y registros del RCP.
 */
#include "test.h"

#include "core/endian.h"

#define KSEG1(pa) (0xFFFFFFFFA0000000ull | (pa))

static void endian_accessors(void)
{
	uint8_t b[8];
	retro64_wr32(b, 0x12345678);
	CHECK_EQ(b[0], 0x12);
	CHECK_EQ(b[3], 0x78);
	retro64_wr64(b, 0x0102030405060708ull);
	CHECK_EQ(b[0], 0x01);
	CHECK_EQ(b[7], 0x08);
	CHECK_EQ(retro64_rd16(b + 2), 0x0304);
	CHECK_EQ(retro64_rd32(b + 4), 0x05060708);
	CHECK_EQ(retro64_rd64(b), 0x0102030405060708ull);
}

static void rdram_byte_order(void)
{
	R[T0] = T_DATA;
	R[T1] = 0x11223344;
	T_LOAD(SW(T1, 0, T0), LBU(T2, 0, T0), LBU(T3, 3, T0), LHU(T4, 2, T0));
	t_step(4);
	CHECK_EQ(R[T2], 0x11);
	CHECK_EQ(R[T3], 0x44);
	CHECK_EQ(R[T4], 0x3344);
}

static void kseg0_kseg1_alias(void)
{
	R[T0] = T_DATA;
	R[T1] = KSEG1(T_DATA_PA);
	R[T2] = 0xCAFEBABE;
	T_LOAD(SW(T2, 0x10, T0), LW(T3, 0x10, T1));
	t_step(2);
	CHECK_EQ(R[T3], 0xFFFFFFFFCAFEBABEull);
	CHECK_EQ(t_rd32(T_DATA_PA + 0x10), 0xCAFEBABE);
}

static void sp_dmem_through_bus(void)
{
	R[T0] = KSEG1(0x04000000);
	R[T1] = 0xDEADBEEF;
	R[T2] = 0x55;
	T_LOAD(SW(T1, 0x40, T0), LW(T3, 0x40, T0), LBU(T4, 0x43, T0), LH(T5, 0x40, T0),
	       SB(T2, 0x41, T0), LW(T6, 0x40, T0));
	t_step(6);
	CHECK_EQ(R[T3], 0xFFFFFFFFDEADBEEFull);
	CHECK_EQ(R[T4], 0xEF);
	CHECK_EQ(R[T5], 0xFFFFFFFFFFFFDEADull);
	/* SB fuera de la RDRAM escribe la palabra entera: el registro desplazado
	 * a su carril, con ceros alrededor (nemu64-test, sp_memory). */
	CHECK_EQ(R[T6], 0x00550000);
	/* Espejo cada 8 KB. */
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x04002040), 0x00550000);
}

static void cart_rom_read(void)
{
	static const uint8_t rom[16] = { 0x80, 0x37, 0x12, 0x40, 0, 0, 0, 0x0F,
					 0x80, 0x00, 0x04, 0x00, 1, 2, 3, 4 };
	retro64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
	R[T0] = KSEG1(0x10000000);
	T_LOAD(LW(T1, 0, T0), LW(T2, 8, T0), LBU(T3, 13, T0), LW(T4, 16, T0));
	t_step(4);
	CHECK_EQ(R[T1], 0xFFFFFFFF80371240ull);
	CHECK_EQ(R[T2], 0xFFFFFFFF80000400ull);
	CHECK_EQ(R[T3], 2);
	CHECK_EQ(R[T4], 0);  /* fuera de la ROM */
	retro64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void pif_ram_access(void)
{
	R[T0] = KSEG1(0x1FC007C0);
	R[T1] = 0x01020304;
	T_LOAD(SW(T1, 0x3C, T0), LW(T2, 0x3C, T0), LW(T3, -4, T0));
	t_step(3);
	CHECK_EQ(R[T2], 0x01020304);
	CHECK_EQ(t_n64.bus.pif_ram[63], 0x04);
	CHECK_EQ(R[T3], 0);  /* PIF ROM: no se incluye */
}

static void mi_version_and_mask(void)
{
	R[T0] = KSEG1(0x04300000);
	R[T1] = 0xAA;  /* poner SP, SI, AI, VI */
	T_LOAD(LW(T2, 4, T0), SW(T1, 0xC, T0), LW(T3, 0xC, T0),
	       ADDIU(T1, ZERO, 0x11), SW(T1, 0xC, T0), LW(T4, 0xC, T0));
	t_step(6);
	CHECK_EQ(R[T2], RETRO64_MI_VERSION_VALUE);
	CHECK_EQ(R[T3], 0x0F);
	CHECK_EQ(R[T4], 0x0A);  /* se limpiaron SP y AI */
}

static void pi_dma_raises_interrupt(void)
{
	static uint8_t rom[0x20];
	for (unsigned k = 0; k < sizeof rom; k++)
		rom[k] = (uint8_t)(0xA0 + k);
	retro64_bus_set_rom(&t_n64.bus, rom, sizeof rom);
	retro64_bus_write32(&t_n64.bus, 0x0430000C, 0x200, ~0u);  /* máscara PI */
	t_set_status(T_STATUS | RETRO64_ST_IE | 0x400);           /* IM2 */

	R[T0] = KSEG1(0x04600000);
	R[T1] = 0x3000;
	R[T2] = 0x10000004;
	R[T3] = 15;  /* 16 bytes */
	T_LOAD(SW(T1, 0, T0), SW(T2, 4, T0), SW(T3, 0xC, T0), NOP);
	t_step(3);
	CHECK_EQ(t_rd32(0x3000), 0xA4A5A6A7);
	CHECK_EQ(t_rd32(0x300C), 0xB0B1B2B3);
	CHECK_EQ(t_rd32(0x3010), 0);
	CHECK_EQ(t_n64.bus.mi.intr, 1u << RETRO64_IRQ_PI);
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x04600010) & RETRO64_PI_STATUS_INTR, RETRO64_PI_STATUS_INTR);
	CHECK(CPU->cause & 0x400);

	t_step(1);  /* la interrupción entra antes de la siguiente instrucción */
	CHECK_EXC(RETRO64_EXC_INT, T_CODE + 12, 0, T_VEC_GEN);

	retro64_bus_write32(&t_n64.bus, 0x04600010, 0x2, ~0u);
	CHECK_EQ(t_n64.bus.mi.intr, 0);
	CHECK_EQ(CPU->cause & 0x400, 0);
	retro64_bus_set_rom(&t_n64.bus, NULL, 0);
}

static void si_dma_to_pif_ram(void)
{
	for (unsigned k = 0; k < 64; k += 4)
		t_wr32(0x4000 + k, 0x10203040u + k);
	retro64_bus_write32(&t_n64.bus, 0x04800000, 0x4000, ~0u);
	retro64_bus_write32(&t_n64.bus, 0x04800010, 0x1FC007C0, ~0u);
	CHECK_EQ(retro64_rd32(t_n64.bus.pif_ram), 0x10203040);
	CHECK_EQ(retro64_rd32(t_n64.bus.pif_ram + 60), 0x1020307C);
	CHECK_EQ(retro64_bus_read32(&t_n64.bus, 0x04800018), RETRO64_SI_STATUS_INTR);
	retro64_bus_write32(&t_n64.bus, 0x04800018, 0, ~0u);
	CHECK_EQ(t_n64.bus.mi.intr, 0);
}

static void sp_status_and_semaphore(void)
{
	struct retro64_bus *bus = &t_n64.bus;
	CHECK_EQ(retro64_bus_read32(bus, 0x04040010), RETRO64_SP_STATUS_HALT);
	retro64_bus_write32(bus, 0x04040010, 0x400, ~0u);        /* poner señal 0 */
	CHECK_EQ(retro64_bus_read32(bus, 0x04040010), 0x81);
	retro64_bus_write32(bus, 0x04040010, 0x10, ~0u);         /* levantar interrupción SP */
	CHECK_EQ(bus->mi.intr, 1u << RETRO64_IRQ_SP);
	retro64_bus_write32(bus, 0x04040010, 0x08, ~0u);         /* bajarla */
	CHECK_EQ(bus->mi.intr, 0);
	/* RSP provisional: quitar HALT lo detiene en el acto con BROKE y, con
	 * interrupción en BREAK habilitada, la levanta. */
	retro64_bus_write32(bus, 0x04040010, 0x1 | 0x100, ~0u);
	CHECK_EQ(retro64_bus_read32(bus, 0x04040010), 0x80 | 0x40 | RETRO64_SP_STATUS_BROKE | RETRO64_SP_STATUS_HALT);
	CHECK_EQ(bus->mi.intr, 1u << RETRO64_IRQ_SP);
	CHECK_EQ(retro64_bus_read32(bus, 0x0404001C), 0);
	CHECK_EQ(retro64_bus_read32(bus, 0x0404001C), 1);
	retro64_bus_write32(bus, 0x0404001C, 0, ~0u);
	CHECK_EQ(retro64_bus_read32(bus, 0x0404001C), 0);
}

void t_suite_core(void)
{
	RUN(endian_accessors);
	RUN(rdram_byte_order);
	RUN(kseg0_kseg1_alias);
	RUN(sp_dmem_through_bus);
	RUN(cart_rom_read);
	RUN(pif_ram_access);
	RUN(mi_version_and_mask);
	RUN(pi_dma_raises_interrupt);
	RUN(si_dma_to_pif_ram);
	RUN(sp_status_and_semaphore);
}
