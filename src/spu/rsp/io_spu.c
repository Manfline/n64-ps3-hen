/*
 * Salsa64 — LLE del RSP en el SPE: la E/S de COP0 sobre el bus del PPE.
 *
 *   SP_STATUS y el semáforo viven en el bus (struct salsa64_sp) y los
 *   cambia también el CPU emulado: se leen y modifican con DMA atómico
 *   (getllar/putllc sobre la línea de 128 bytes que los contiene); el PPE
 *   usa lwarx/stwcx en los mismos registros, así ninguno pisa al otro.
 *   Cada LLE_POLL_INTERVAL instrucciones se relee SP_STATUS: un HALT del
 *   CPU detiene el microcódigo enseguida.
 *   Los registros del DMA del SP se llevan aquí y vuelven al bus al
 *   terminar; el DMA mismo se hace entre la RDRAM y la DMEM/IMEM del local
 *   store: lecturas por el tramo alineado a 16 que las contiene,
 *   escrituras en trozos de alineación natural (8 o múltiplos de 16),
 *   anotando los tramos escritos para el recompilador del PPE.
 *   Los registros del DP se llevan aquí: cada DPC_END (o descongelar)
 *   anota el tramo en el registro del trabajo (con los comandos copiados
 *   si vienen de la DMEM) y DPC_CURRENT avanza al final, como si el RDP
 *   consumiera al instante; el PPE lo reproduce al recoger el trabajo.
 *   La interrupción del SP (BREAK con INTR_BREAK, o pedida por SP_STATUS)
 *   queda anotada para el PPE; limpiarla desde el microcódigo no se
 *   transmite.
 *
 * Etiquetas DMA: 0 memoria y registros, 1 lecturas de la RDRAM, 2
 * escrituras, 3 capturas. La 31 es de spukit.
 */
#include <spukit/spu.h>
#include <stddef.h>
#include <string.h>

#include "core/endian.h"
#include "spu/rsp/io_spu.h"

#define TAG_MEM   0
#define TAG_READ  1
#define TAG_WRITE 2
#define TAG_CAP   3

/* Bits de SP_STATUS (core/rcp.h, sin incluirlo: el SPE no ve el bus). */
#define ST_HALT       0x0001u
#define ST_BROKE      0x0002u
#define ST_INTR_BREAK 0x0040u
#define ST_SIG(n)     (0x0080u << (n))
#define SP_STATUS_OFF    16u  /* regs[4] */
#define SP_SEMAPHORE_OFF 28u  /* regs[7] */

#define DP_XBUS        0x001u
#define DP_FREEZE      0x002u
#define DP_FLUSH       0x004u
#define DP_CBUF_READY  0x080u
#define DP_START_VALID 0x400u

struct lle_spu_io {
	const struct lle_job *job;
	uint32_t regs[4] __attribute__((aligned(16)));  /* MEM_ADDR, DRAM_ADDR, LEN, (sin uso) */
	uint32_t dp_start, dp_end, dp_current, dp_status;
	uint32_t irq, flags;
	uint32_t nentries, cap_used;
	uint32_t nwritten;
	struct lle_written written[LLE_WRITTEN_MAX];
	uint32_t dma_rows_in, dma_rows_out, atomic_retries;
	uint32_t c0_reads0[16], c0_writes0[16];  /* contadores al empezar el trabajo */
	uint32_t instr0;
};

static struct lle_spu_io io;
static struct lle_dpc_entry entries[LLE_DPC_MAX] __attribute__((aligned(128)));
static uint8_t line[128] __attribute__((aligned(128)));
static uint8_t rd_buf[LLE_MEM_SIZE + 32] __attribute__((aligned(128)));
/* Un DMA de varias filas (S2DEX carga fondos línea a línea: cientos de
 * filas con un salto entre ellas) se trae de una vez si el tramo que
 * abarcan cabe aquí; si no, fila a fila. */
#define RD_SPAN_MAX (32u << 10)
static uint8_t rd_span[RD_SPAN_MAX + 32] __attribute__((aligned(128)));
static uint8_t wr_buf[LLE_MEM_SIZE + 32] __attribute__((aligned(128)));
static uint8_t cap_buf[LLE_MEM_SIZE + 32] __attribute__((aligned(128)));
static uint8_t regs_buf[64] __attribute__((aligned(128)));
static struct lle_heartbeat heartbeat __attribute__((aligned(128)));

/* ---- DMA atómico sobre una palabra del bus ---- */

static uint32_t atomic_rmw(uint64_t ea, uint32_t clear, uint32_t set, uint32_t *old)
{
	uint64_t base = ea & ~127ull;
	uint32_t off = (uint32_t)(ea & 127), v;
	for (;;) {
		mfc_getllar(line, base, 0, 0);
		mfc_read_atomic_status();
		v = *(volatile uint32_t *)(line + off);
		*(volatile uint32_t *)(line + off) = (v & ~clear) | set;
		mfc_putllc(line, base, 0, 0);
		if (!(mfc_read_atomic_status() & MFC_PUTLLC_STATUS))
			break;
		io.atomic_retries++;
	}
	if (old)
		*old = v;
	return (v & ~clear) | set;
}

static uint32_t atomic_read(uint64_t ea)
{
	uint64_t base = ea & ~127ull;
	mfc_getllar(line, base, 0, 0);
	mfc_read_atomic_status();
	return *(volatile uint32_t *)(line + (ea & 127));
}

static uint64_t status_ea(void) { return io.job->sp_ea + SP_STATUS_OFF; }
static uint64_t semaphore_ea(void) { return io.job->sp_ea + SP_SEMAPHORE_OFF; }

/* ---- DMA del SP ---- */

static void note_written(uint32_t start, uint32_t len)
{
	if (io.nwritten && io.written[io.nwritten - 1].start + io.written[io.nwritten - 1].len == start) {
		io.written[io.nwritten - 1].len += len;
		return;
	}
	if (io.nwritten < LLE_WRITTEN_MAX)
		io.written[io.nwritten++] = (struct lle_written){ start, len };
	else
		io.flags |= LLE_RES_WRITTEN_ALL;
}

/* Copia `len` bytes (múltiplo de 8) entre la DMEM/IMEM (con vuelta en el
 * banco) y un búfer lineal. */
static void mem_copy(struct rsp_lle *s, uint32_t bank, uint32_t off, uint8_t *buf, uint32_t len, bool to_buf)
{
	uint8_t *m = bank ? s->imem : s->dmem;
	while (len) {
		uint32_t n = LLE_MEM_SIZE - off;
		if (n > len)
			n = len;
		if (to_buf)
			memcpy(buf, m + off, n);
		else
			memcpy(m + off, buf, n);
		buf += n;
		len -= n;
		off = (off + n) & 0xFFFu;
	}
}

static void sp_dma(struct rsp_lle *s, uint32_t v, bool to_rdram)
{
	const struct lle_job *job = io.job;
	uint32_t len = ((v & 0xFFF) | 7) + 1;
	uint32_t count = ((v >> 12) & 0xFF) + 1;
	uint32_t skip = (v >> 20) & 0xFF8;
	uint32_t mem = io.regs[0] & 0x1FF8;
	uint32_t bank = mem & 0x1000, off = mem & 0xFF8;
	uint32_t dram = io.regs[1] & 0xFFFFF8;

	if (to_rdram)
		io.dma_rows_out += count;
	else
		io.dma_rows_in += count;
	if (!to_rdram && count > 1) {
		/* Todas las filas de un tramo de la RDRAM con una sola lectura. */
		uint32_t stride = len + skip;
		uint64_t span = (uint64_t)(count - 1) * stride + len;
		if (span <= RD_SPAN_MAX && dram + span <= job->rdram_size) {
			const uint8_t *p = spukit_get_unaligned(rd_span, job->rdram_ea + dram, (uint32_t)span, TAG_READ);
			for (uint32_t r = 0; r < count; r++) {
				mem_copy(s, bank, off, (uint8_t *)p + r * stride, len, false);
				off = (off + len) & 0xFF8;
			}
			io.regs[0] = bank | off;
			io.regs[1] = (uint32_t)((dram + count * stride) & 0xFFFFF8);
			io.regs[2] = (v & 0xFFF00000u) | 0xFF8;
			return;
		}
	}
	for (uint32_t r = 0; r < count; r++) {
		uint32_t avail = dram < job->rdram_size ? job->rdram_size - dram : 0;
		uint32_t n = len < avail ? len : avail & ~7u;
		if (to_rdram) {
			if (n) {
				uint64_t ea = job->rdram_ea + dram;
				uint8_t *ls = wr_buf + (ea & 15);
				mem_copy(s, bank, off, ls, n, true);
				spukit_dma_wait(1u << TAG_WRITE);
				for (uint32_t left = n; left;) {
					uint32_t k = ((ea & 15) == 0 && left >= 16) ? left & ~15u : 8;
					mfc_put((volatile void *)ls, ea, k, TAG_WRITE, 0, 0);
					ls += k;
					ea += k;
					left -= k;
				}
				spukit_dma_wait(1u << TAG_WRITE);
				note_written(dram, n);
			}
		} else {
			if (n) {
				const uint8_t *p = spukit_get_unaligned(rd_buf, job->rdram_ea + dram, n, TAG_READ);
				mem_copy(s, bank, off, (uint8_t *)p, n, false);
			}
			if (n < len) {
				memset(rd_buf, 0, len - n);
				mem_copy(s, bank, (off + n) & 0xFFFu, rd_buf, len - n, false);
			}
		}
		off = (off + len) & 0xFF8;
		dram = (dram + len + skip) & 0xFFFFF8;
	}
	io.regs[0] = bank | off;
	io.regs[1] = dram;
	io.regs[2] = (v & 0xFFF00000u) | 0xFF8;
}

/* ---- DP ---- */

static void dp_flush(struct rsp_lle *s)
{
	if (io.dp_status & DP_FREEZE)
		return;
	io.dp_status |= DP_CBUF_READY;
	uint32_t start = io.dp_current, end = io.dp_end;
	if (end <= start)
		return;
	io.dp_current = end;
	if (io.nentries >= LLE_DPC_MAX) {
		io.flags |= LLE_RES_CAPTURE_FULL;
		return;
	}
	struct lle_dpc_entry *e = &entries[io.nentries];
	e->start = start;
	e->end = end;
	e->xbus = (io.dp_status & DP_XBUS) != 0;
	e->cap_off = ~0u;
	if (e->xbus) {
		uint32_t len = end - start;
		uint32_t len16 = (len + 15) & ~15u;
		if (len > LLE_MEM_SIZE || io.cap_used + len16 > LLE_CAPTURE_SIZE) {
			io.flags |= LLE_RES_CAPTURE_FULL;
			return;
		}
		spukit_dma_wait(1u << TAG_CAP);
		mem_copy(s, 0, start & 0xFFFu, cap_buf, len16, true);
		spukit_dma_put(cap_buf, io.job->log_ea + offsetof(struct lle_log, capture) + io.cap_used, len16, TAG_CAP);
		e->cap_off = io.cap_used;
		io.cap_used += len16;
	}
	io.nentries++;
}

/* ---- COP0 ---- */

uint32_t lle_io_read(struct rsp_lle *s, unsigned reg)
{
	switch (reg) {
	case LLE_C0_MEM_ADDR:
	case LLE_C0_DRAM_ADDR:
		return io.regs[reg];
	case LLE_C0_RD_LEN:
	case LLE_C0_WR_LEN:
		return io.regs[2];
	case LLE_C0_STATUS: {
		uint32_t st = atomic_read(status_ea());
		s->halt = (st & ST_HALT) != 0;
		return st;
	}
	case LLE_C0_DMA_FULL:
	case LLE_C0_DMA_BUSY:
		return 0;
	case LLE_C0_SEMAPHORE: {
		uint32_t old;
		atomic_rmw(semaphore_ea(), 0, 1, &old);
		return old;
	}
	case LLE_C0_DP_START: return io.dp_start;
	case LLE_C0_DP_END: return io.dp_end;
	case LLE_C0_DP_CURRENT: return io.dp_current;
	case LLE_C0_DP_STATUS: return io.dp_status;
	default: return 0;
	}
}

void lle_io_write(struct rsp_lle *s, unsigned reg, uint32_t v)
{
	switch (reg) {
	case LLE_C0_MEM_ADDR:
	case LLE_C0_DRAM_ADDR:
		io.regs[reg] = v;
		return;
	case LLE_C0_RD_LEN:
		sp_dma(s, v, false);
		return;
	case LLE_C0_WR_LEN:
		sp_dma(s, v, true);
		return;
	case LLE_C0_STATUS: {
		uint32_t clear = 0, set = 0;
		if ((v & 0x003) == 0x001) clear |= ST_HALT;
		if ((v & 0x003) == 0x002) set |= ST_HALT;
		if (v & 0x004) clear |= ST_BROKE;
		if ((v & 0x018) == 0x010) io.irq |= LLE_IRQ_SP;
		if ((v & 0x060) == 0x020) clear |= 0x20u;
		if ((v & 0x060) == 0x040) set |= 0x20u;
		if ((v & 0x180) == 0x080) clear |= ST_INTR_BREAK;
		if ((v & 0x180) == 0x100) set |= ST_INTR_BREAK;
		for (int i = 0; i < 8; i++) {
			uint32_t pair = (v >> (9 + 2 * i)) & 3;
			if (pair == 1)
				clear |= ST_SIG(i);
			else if (pair == 2)
				set |= ST_SIG(i);
		}
		uint32_t st = atomic_rmw(status_ea(), clear, set, NULL);
		s->halt = (st & ST_HALT) != 0;
		return;
	}
	case LLE_C0_SEMAPHORE:
		atomic_rmw(semaphore_ea(), ~0u, 0, NULL);
		return;
	case LLE_C0_DP_START:
		if (!(io.dp_status & DP_START_VALID))
			io.dp_start = v & 0xFFFFF8;
		io.dp_status |= DP_START_VALID;
		return;
	case LLE_C0_DP_END:
		io.dp_end = v & 0xFFFFF8;
		if (io.dp_status & DP_START_VALID) {
			io.dp_current = io.dp_start;
			io.dp_status &= ~DP_START_VALID;
		}
		dp_flush(s);
		return;
	case LLE_C0_DP_STATUS:
		if (v & 0x001) io.dp_status &= ~DP_XBUS;
		if (v & 0x002) io.dp_status |= DP_XBUS;
		if (v & 0x004) io.dp_status &= ~DP_FREEZE;
		if (v & 0x008) io.dp_status |= DP_FREEZE;
		if (v & 0x010) io.dp_status &= ~DP_FLUSH;
		if (v & 0x020) io.dp_status |= DP_FLUSH;
		if (v & 0x004)
			dp_flush(s);
		return;
	default:
		return;
	}
}

void lle_io_break(struct rsp_lle *s)
{
	uint32_t st = atomic_rmw(status_ea(), 0, ST_HALT | ST_BROKE, NULL);
	if (st & ST_INTR_BREAK)
		io.irq |= LLE_IRQ_SP;
	s->halt = true;
}

static void fill_counts(const struct rsp_lle *s, uint32_t *status_reads, uint32_t *sem_reads,
			uint32_t *status_writes, uint32_t *sem_writes, uint32_t *dp_reads)
{
	*status_reads = s->c0_reads[LLE_C0_STATUS] - io.c0_reads0[LLE_C0_STATUS];
	*sem_reads = s->c0_reads[LLE_C0_SEMAPHORE] - io.c0_reads0[LLE_C0_SEMAPHORE];
	*status_writes = s->c0_writes[LLE_C0_STATUS] - io.c0_writes0[LLE_C0_STATUS];
	*sem_writes = s->c0_writes[LLE_C0_SEMAPHORE] - io.c0_writes0[LLE_C0_SEMAPHORE];
	uint32_t dp = 0;
	for (unsigned i = 8; i < 16; i++)
		dp += s->c0_reads[i] - io.c0_reads0[i];
	*dp_reads = dp;
}

void lle_io_poll(struct rsp_lle *s)
{
	s->halt = (atomic_read(status_ea()) & ST_HALT) != 0;
	uint32_t n = (uint32_t)(s->instructions - io.instr0);
	if ((n & (LLE_HEARTBEAT - 1)) != 0)
		return;
	/* Latido: dónde va el trabajo (para un microcódigo que no termina). */
	spukit_dma_wait(1u << TAG_CAP);
	heartbeat.instructions = n;
	heartbeat.pc = s->pc;
	fill_counts(s, &heartbeat.status_reads, &heartbeat.sem_reads, &heartbeat.status_writes,
		    &heartbeat.sem_writes, &heartbeat.dp_reads);
	heartbeat.dma_rows_in = io.dma_rows_in;
	heartbeat.dma_rows_out = io.dma_rows_out;
	heartbeat.atomic_retries = io.atomic_retries;
	heartbeat.dpc_entries = io.nentries;
	for (int i = 0; i < 4; i++)
		heartbeat.r[i] = s->r[1 + i];
	spukit_dma_put(&heartbeat, io.job->log_ea + offsetof(struct lle_log, heartbeat), sizeof heartbeat, TAG_CAP);
}

/* ---- Un trabajo ---- */

int lle_spu_run(struct rsp_lle *s, const struct lle_job *job, struct lle_result *res)
{
	memset(&io, 0, sizeof io);
	io.job = job;
	memcpy(io.c0_reads0, s->c0_reads, sizeof io.c0_reads0);
	memcpy(io.c0_writes0, s->c0_writes, sizeof io.c0_writes0);
	io.instr0 = (uint32_t)s->instructions;

	/* DMEM e IMEM del bus, y los registros del DMA del SP. */
	spukit_dma_get(s->dmem, job->mem_ea, 2 * LLE_MEM_SIZE, TAG_MEM);
	const uint8_t *r = spukit_get_unaligned(regs_buf, job->sp_ea, 16, TAG_MEM);
	spukit_dma_wait(1u << TAG_MEM);
	for (int i = 0; i < 3; i++)
		io.regs[i] = salsa64_rd32(r + 4 * i);
	io.dp_start = job->dp_start;
	io.dp_end = job->dp_end;
	io.dp_current = job->dp_current;
	io.dp_status = job->dp_status;

	if (job->pc != s->pc)
		lle_set_pc(s, job->pc);
	s->halt = (atomic_read(status_ea()) & ST_HALT) != 0;
	uint32_t n = lle_run(s, job->max_instr);

	/* De vuelta al bus: memoria, registros del DMA, tramos del RDP. */
	spukit_dma_put(s->dmem, job->mem_ea, 2 * LLE_MEM_SIZE, TAG_MEM);
	for (int i = 0; i < 3; i++) {
		/* Un DMA de 4 bytes exige la misma fase de 16 en el LS y en la EA. */
		uint64_t ea = job->sp_ea + 4 * i;
		uint8_t *p = regs_buf + 32 + (ea & 15);
		salsa64_wr32(p, io.regs[i]);
		mfc_put((volatile void *)p, ea, 4, TAG_MEM, 0, 0);
	}
	if (io.nentries)
		spukit_dma_put(entries, job->log_ea + offsetof(struct lle_log, entries), (io.nentries * 16 + 15) & ~15u,
			       TAG_MEM);

	memset(res, 0, sizeof *res);
	res->pc = s->pc;
	res->flags = io.flags | (s->halt ? LLE_RES_HALTED : 0);
	res->instructions = n;
	res->irq = io.irq;
	res->dp_status = io.dp_status;
	res->dp_start = io.dp_start;
	res->dp_end = io.dp_end;
	res->dp_current = io.dp_current;
	res->dpc_entries = io.nentries;
	res->nwritten = io.nwritten;
	memcpy(res->written, io.written, sizeof res->written);
	fill_counts(s, &res->status_reads, &res->sem_reads, &res->status_writes, &res->sem_writes, &res->dp_reads);
	res->dma_rows_in = io.dma_rows_in;
	res->dma_rows_out = io.dma_rows_out;
	res->atomic_retries = io.atomic_retries;
	spukit_dma_wait((1u << TAG_MEM) | (1u << TAG_WRITE) | (1u << TAG_CAP));
	return 0;
}
