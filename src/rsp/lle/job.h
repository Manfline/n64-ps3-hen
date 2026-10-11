/*
 * Salsa64 — LLE del RSP en un SPE: formato del trabajo de lib/spukit que
 * comparten el PPE (src/platform/ps3/rsp_spu.c) y el programa SPU
 * (src/spu/rsp). Ver docs/rsp-lle.md.
 *
 * Cada trabajo ejecuta el microcódigo desde `pc` hasta que el RSP se
 * detiene (BREAK o HALT desde el CPU) o hasta `max_instr` instrucciones;
 * en ese caso el PPE publica otro trabajo que sigue donde quedó, así el
 * CPU puede interactuar entre medias (semáforo, señales, HALT). La DMEM y
 * la IMEM viajan al local store al empezar y vuelven al bus al terminar.
 *
 * Lo que el microcódigo hace con el exterior y el SPE no puede hacer solo
 * queda anotado para que el PPE lo aplique al recoger el trabajo: tramos
 * entregados al RDP (DPC_START/DPC_END, con los comandos copiados si venían
 * de la DMEM, que el microcódigo reescribe enseguida), tramos de la RDRAM
 * escritos por DMA (el recompilador descarta el código que dependa de
 * ellos) y la interrupción del SP. SP_STATUS y el semáforo sí los cambia
 * el SPE directamente, con DMA atómico sobre la línea que los contiene.
 */
#ifndef SALSA64_RSP_LLE_JOB_H
#define SALSA64_RSP_LLE_JOB_H

#include <stdint.h>

#define LLE_JOB_RUN   1
#define LLE_JOB_RESET 2

struct lle_job {
	uint64_t sp_ea;      /* struct salsa64_sp del bus: regs[8] y pc */
	uint64_t mem_ea;     /* DMEM e IMEM del bus (8 KB, alineadas a 128) */
	uint64_t rdram_ea;
	uint64_t log_ea;     /* struct lle_log (alineada a 128) */
	uint32_t rdram_size;
	uint32_t pc;         /* SP_PC */
	uint32_t max_instr;
	uint32_t dp_status;  /* DPC_STATUS al empezar (xbus, freeze) */
	uint32_t dp_start, dp_end, dp_current;
	uint32_t reserved;
};

#define LLE_WRITTEN_MAX 5

struct lle_written {
	uint32_t start, len;
};

struct lle_result {
	uint32_t pc;
	uint32_t flags;         /* LLE_RES_* */
	uint32_t instructions;
	uint32_t irq;           /* LLE_IRQ_* pendientes de aplicar */
	uint32_t dp_status, dp_start, dp_end, dp_current;
	uint32_t dpc_entries;   /* entradas válidas en lle_log.entries */
	uint32_t nwritten;
	struct lle_written written[LLE_WRITTEN_MAX];
	/* Qué hizo el microcódigo con COP0 en este trabajo: lecturas de
	 * SP_STATUS y del semáforo, escrituras de SP_STATUS y del semáforo,
	 * filas de DMA leídas y escritas, reintentos del DMA atómico y
	 * lecturas de los registros del DP. */
	uint32_t status_reads, sem_reads, status_writes, sem_writes;
	uint32_t dma_rows_in, dma_rows_out, atomic_retries, dp_reads;
};

/* Latido del trabajo en curso, que el SPE escribe cada LLE_HEARTBEAT
 * instrucciones en lle_log.heartbeat: para ver dónde está un microcódigo
 * que no termina. */
#define LLE_HEARTBEAT 4096u
struct lle_heartbeat {
	uint32_t instructions;  /* del trabajo en curso */
	uint32_t pc;
	uint32_t status_reads, sem_reads, status_writes, sem_writes;
	uint32_t dma_rows_in, dma_rows_out, atomic_retries, dp_reads;
	uint32_t dpc_entries;
	uint32_t r[4];          /* $1, $2, $3, $4 */
	uint32_t pad[17];
} __attribute__((aligned(128)));

#define LLE_RES_HALTED      0x1u  /* se detuvo; si no, agotó max_instr */
#define LLE_RES_WRITTEN_ALL 0x2u  /* más tramos de los que caben: toda la RDRAM */
#define LLE_RES_CAPTURE_FULL 0x4u /* se perdieron comandos xbus (captura llena) */
#define LLE_IRQ_SP 0x1u

/* Tramo entregado al RDP; con xbus los comandos están copiados en
 * lle_log.capture desde cap_off (end - start bytes). */
struct lle_dpc_entry {
	uint32_t start, end, xbus, cap_off;
};

#define LLE_DPC_MAX      256u
#define LLE_CAPTURE_SIZE (64u << 10)

struct lle_log {
	struct lle_dpc_entry entries[LLE_DPC_MAX];
	uint8_t capture[LLE_CAPTURE_SIZE];
	struct lle_heartbeat heartbeat;
} __attribute__((aligned(128)));

#endif
