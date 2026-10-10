/*
 * Salsa64 — registros del RSP (SP) y del RDP (DP, comandos).
 *
 * SP_STATUS (halt, broke, interrupción, señales) y el semáforo se comportan
 * como en la consola. El DMA del SP entre la RDRAM y la DMEM/IMEM es
 * inmediato. Quitarle HALT al RSP le pasa la tarea de la DMEM a rsp.c, que
 * la procesa por HLE, la manda al microcódigo real (src/rsp/lle, si está)
 * o, si no la reconoce, lo detiene en el acto como si hubiera ejecutado
 * BREAK (BROKE y, si está habilitada, la interrupción), para que nadie
 * espere para siempre. Antes de cada acceso del CPU a un registro del SP
 * o del DP, el RSP a bajo nivel se pone al día (salsa64_rsp_sync).
 *
 * SP_STATUS se modifica con operaciones atómicas: en la PS3 el microcódigo
 * real corre en un SPE y cambia el mismo registro desde fuera (señales,
 * HALT al hacer BREAK) con DMA atómico sobre la línea que lo contiene.
 *
 * Los registros del DP: DPC_START/DPC_END entregan el tramo de comandos
 * al consumidor del RDP (bus->rsp.dpc; sin él, se descarta) y
 * DPC_CURRENT avanza hasta el final; DPC_STATUS lleva xbus (comandos en la
 * DMEM), freeze (los comandos esperan) y flush con sus pares de bits
 * (limpiar, poner), y los contadores se leen como cero.
 */
#include "core/bus.h"

#include <string.h>

#define SP_PC_REG 0x80000u

uint32_t salsa64_sp_read(struct salsa64_bus *bus, uint32_t reg)
{
	salsa64_rsp_sync(bus);
	if (reg == SP_PC_REG)
		return bus->sp.pc;
	if ((reg & ~0x1Fu) != 0x40000)
		return 0;

	uint32_t i = (reg >> 2) & 7;
	if (i == SALSA64_SP_SEMAPHORE)
		return __atomic_exchange_n(&bus->sp.regs[i], 1u, __ATOMIC_SEQ_CST);
	if (i == SALSA64_SP_DMA_FULL || i == SALSA64_SP_DMA_BUSY)
		return 0;
	/* RD_LEN y WR_LEN son el mismo registro al leerlos. */
	if (i == SALSA64_SP_WR_LEN)
		i = SALSA64_SP_RD_LEN;
	if (i == SALSA64_SP_STATUS)
		return __atomic_load_n(&bus->sp.regs[i], __ATOMIC_SEQ_CST);
	return bus->sp.regs[i];
}

/* DMA del SP (n64brew.dev: RSP, "DMA"): `count` filas de `len` bytes
 * (múltiplos de 8), con `skip` bytes entre filas en la RDRAM; la dirección
 * de la DMEM/IMEM da la vuelta dentro de su banco de 4 KB. Al terminar,
 * las direcciones quedan donde acabó la copia y el largo se lee como 0xFF8
 * con cuenta 0. */
static void sp_dma(struct salsa64_bus *bus, uint32_t v, bool to_rdram)
{
	uint32_t len = ((v & 0xFFF) | 7) + 1;
	uint32_t count = ((v >> 12) & 0xFF) + 1;
	uint32_t skip = (v >> 20) & 0xFF8;
	uint32_t mem = bus->sp.regs[SALSA64_SP_MEM_ADDR] & 0x1FF8;
	uint32_t bank = mem & 0x1000, off = mem & 0xFF8;
	uint32_t dram = bus->sp.regs[SALSA64_SP_DRAM_ADDR] & 0xFFFFF8;

	for (uint32_t r = 0; r < count; r++) {
		uint32_t row = dram;
		for (uint32_t k = 0; k < len; k += 8) {
			uint8_t *m = bus->sp_mem + bank + off;
			if (to_rdram) {
				if (dram + 8 <= bus->rdram_size)
					memcpy(bus->rdram + dram, m, 8);
			} else if (dram + 8 <= bus->rdram_size) {
				memcpy(m, bus->rdram + dram, 8);
			} else {
				memset(m, 0, 8);
			}
			off = (off + 8) & 0xFF8;
			dram = (dram + 8) & 0xFFFFF8;
		}
		if (to_rdram)
			salsa64_bus_rdram_written(bus, row, len);  /* código recompilado ahí: descartarlo */
		dram = (dram + skip) & 0xFFFFF8;
	}
	bus->sp.regs[SALSA64_SP_MEM_ADDR] = bank | off;
	bus->sp.regs[SALSA64_SP_DRAM_ADDR] = dram;
	bus->sp.regs[SALSA64_SP_RD_LEN] = (v & 0xFFF00000u) | 0xFF8;
}

/* Pares (limpiar, poner) de la escritura de SP_STATUS: los dos bits a la
 * vez no cambian nada (como en la consola; nemu64-test lo verifica). */
static void sp_write_status(struct salsa64_bus *bus, uint32_t v)
{
	uint32_t clear = 0, set = 0;

	if ((v & 0x003) == 0x001) clear |= SALSA64_SP_STATUS_HALT;
	if ((v & 0x003) == 0x002) set |= SALSA64_SP_STATUS_HALT;
	if (v & 0x004) clear |= SALSA64_SP_STATUS_BROKE;
	if ((v & 0x018) == 0x008) salsa64_mi_clear(bus, SALSA64_IRQ_SP);
	if ((v & 0x018) == 0x010) salsa64_mi_raise(bus, SALSA64_IRQ_SP);
	if ((v & 0x060) == 0x020) clear |= 0x20u;  /* single step */
	if ((v & 0x060) == 0x040) set |= 0x20u;
	if ((v & 0x180) == 0x080) clear |= SALSA64_SP_STATUS_INTR_BREAK;
	if ((v & 0x180) == 0x100) set |= SALSA64_SP_STATUS_INTR_BREAK;
	/* Señales 0–7: bits de estado 7–14, pares (limpiar, poner) desde el 9. */
	for (int i = 0; i < 8; i++) {
		uint32_t pair = (v >> (9 + 2 * i)) & 3;
		if (pair == 1)
			clear |= SALSA64_SP_STATUS_SIG(i);
		else if (pair == 2)
			set |= SALSA64_SP_STATUS_SIG(i);
	}
	uint32_t old = __atomic_fetch_and(&bus->sp.regs[SALSA64_SP_STATUS], ~clear, __ATOMIC_SEQ_CST);
	uint32_t now = __atomic_or_fetch(&bus->sp.regs[SALSA64_SP_STATUS], set, __ATOMIC_SEQ_CST);
	if ((old & SALSA64_SP_STATUS_HALT) && !(now & SALSA64_SP_STATUS_HALT))
		salsa64_rsp_start(bus);
}

void salsa64_sp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	salsa64_rsp_sync(bus);
	if (reg == SP_PC_REG) {
		bus->sp.pc = v & 0xFFC;
		return;
	}
	if ((reg & ~0x1Fu) != 0x40000)
		return;

	uint32_t i = (reg >> 2) & 7;
	switch (i) {
	case SALSA64_SP_STATUS:
		sp_write_status(bus, v);
		return;
	case SALSA64_SP_SEMAPHORE:
		__atomic_store_n(&bus->sp.regs[i], 0u, __ATOMIC_SEQ_CST);
		return;
	case SALSA64_SP_DMA_FULL:
	case SALSA64_SP_DMA_BUSY:
		return;
	case SALSA64_SP_RD_LEN:
		sp_dma(bus, v, false);
		return;
	case SALSA64_SP_WR_LEN:
		sp_dma(bus, v, true);
		return;
	default:
		bus->sp.regs[i] = v;
		return;
	}
}

uint32_t salsa64_dp_read(struct salsa64_bus *bus, uint32_t reg)
{
	salsa64_rsp_sync(bus);
	uint32_t i = reg >> 2;
	return i < SALSA64_DP_NREGS ? bus->dp[i] : 0;
}

/* [start, end) ya está en manos del RDP: lo consume el backend. */
static void dp_run(struct salsa64_bus *bus, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	const struct salsa64_dpc_backend *dpc = bus->rsp.dpc;
	bus->rsp.stats.dpc_runs++;
	if (dpc && dpc->run(dpc->ctx, start, end, dmem) & SALSA64_GFX_FULLSYNC)
		salsa64_rsp_dp_done(bus);
}

/* Entrega [DPC_CURRENT, DPC_END) al consumidor del RDP, si no está
 * congelado, y avanza DPC_CURRENT. */
static void dp_flush(struct salsa64_bus *bus)
{
	uint32_t *dp = bus->dp;
	if (dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_FREEZE)
		return;
	uint32_t start = dp[SALSA64_DP_CURRENT], end = dp[SALSA64_DP_END];
	dp[SALSA64_DP_STATUS] |= SALSA64_DP_STATUS_CBUF_READY;
	if (end <= start)
		return;
	dp[SALSA64_DP_CURRENT] = end;
	dp_run(bus, start, end, (dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_XBUS) ? bus->sp_mem : NULL);
}

void salsa64_dp_submit(struct salsa64_bus *bus, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	uint32_t *dp = bus->dp;
	dp[SALSA64_DP_START] = start;
	dp[SALSA64_DP_END] = end;
	dp[SALSA64_DP_CURRENT] = end;
	dp[SALSA64_DP_STATUS] = (dp[SALSA64_DP_STATUS] & ~(SALSA64_DP_STATUS_START_VALID | SALSA64_DP_STATUS_XBUS)) |
				SALSA64_DP_STATUS_CBUF_READY | (dmem ? SALSA64_DP_STATUS_XBUS : 0);
	if (end > start)
		dp_run(bus, start, end, dmem);
}

void salsa64_dp_write(struct salsa64_bus *bus, uint32_t reg, uint32_t v)
{
	salsa64_rsp_sync(bus);
	uint32_t *dp = bus->dp;
	uint32_t i = reg >> 2;
	switch (i) {
	case SALSA64_DP_START:
		/* Hasta que llegue el END, el primer START vale. */
		if (!(dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_START_VALID))
			dp[SALSA64_DP_START] = v & 0xFFFFF8;
		dp[SALSA64_DP_STATUS] |= SALSA64_DP_STATUS_START_VALID;
		return;
	case SALSA64_DP_END:
		dp[SALSA64_DP_END] = v & 0xFFFFF8;
		if (dp[SALSA64_DP_STATUS] & SALSA64_DP_STATUS_START_VALID) {
			dp[SALSA64_DP_CURRENT] = dp[SALSA64_DP_START];
			dp[SALSA64_DP_STATUS] &= ~SALSA64_DP_STATUS_START_VALID;
		}
		dp_flush(bus);
		return;
	case SALSA64_DP_STATUS: {
		uint32_t s = dp[SALSA64_DP_STATUS];
		if (v & 0x001) s &= ~SALSA64_DP_STATUS_XBUS;
		if (v & 0x002) s |= SALSA64_DP_STATUS_XBUS;
		if (v & 0x004) s &= ~SALSA64_DP_STATUS_FREEZE;
		if (v & 0x008) s |= SALSA64_DP_STATUS_FREEZE;
		if (v & 0x010) s &= ~SALSA64_DP_STATUS_FLUSH;
		if (v & 0x020) s |= SALSA64_DP_STATUS_FLUSH;
		if (v & 0x200) dp[SALSA64_DP_CLOCK] = 0;
		dp[SALSA64_DP_STATUS] = s;
		if (v & 0x004)
			dp_flush(bus);
		return;
	}
	case SALSA64_DP_CURRENT:
	case SALSA64_DP_CLOCK:
	case SALSA64_DP_BUFBUSY:
	case SALSA64_DP_PIPEBUSY:
	case SALSA64_DP_TMEM:
		return;  /* solo lectura */
	default:
		return;
	}
}
