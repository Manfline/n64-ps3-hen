/*
 * Salsa64 — LLE del RSP: intérprete del microcódigo real (Fase 6).
 *
 * El RSP es un MIPS de 32 bits sin multiplicador, con una unidad vectorial
 * (COP2: 32 registros de 8 × 16 bits, acumulador de 48 bits por elemento y
 * las banderas VCO/VCC/VCE), 4 KB de DMEM y 4 KB de IMEM, y COP0 con los
 * registros del SP (DMA, estado, semáforo) y del DP. Este intérprete es
 * portable: el mismo código corre en el host (pruebas, salsa64-run), en el
 * PPE y en un SPE (src/spu/rsp); lo que depende de la plataforma (COP0:
 * DMA con la RDRAM, SP_STATUS, DPC_*) va por las funciones lle_io_*.
 *
 * Orden de los datos: la DMEM y la IMEM son arreglos de bytes en el orden
 * de la N64 (big-endian); los vectores guardan sus 8 elementos como
 * enteros del host (`e[0]` es el elemento 0, los dos primeros bytes en
 * memoria). Así el mismo código vale en little-endian (host) y en
 * big-endian (PPE y SPE, donde un vector cargado de memoria coincide con
 * `e[]`).
 *
 * Precisión: cada instrucción cuenta como un ciclo (sin la segmentación
 * ni la emisión doble del RSP real); SP_STATUS.SSTEP no se emula.
 *
 * Referencias: n64brew.dev (Reality Signal Processor y su unidad vectorial)
 * y la semántica de ares (ISC) para los casos raros de la unidad vectorial
 * y las cargas/almacenamientos, verificada contra hardware por
 * nemu64-test. Ver docs/THIRD_PARTY.md.
 */
#ifndef SALSA64_RSP_LLE_LLE_H
#define SALSA64_RSP_LLE_LLE_H

#include <stdbool.h>
#include <stdint.h>

#define LLE_MEM_SIZE 0x1000u  /* DMEM e IMEM, 4 KB cada una */

/* Registros de COP0 (MFC0/MTC0): 0–7 los del SP, 8–15 los del DP. */
enum lle_cop0 {
	LLE_C0_MEM_ADDR, LLE_C0_DRAM_ADDR, LLE_C0_RD_LEN, LLE_C0_WR_LEN,
	LLE_C0_STATUS, LLE_C0_DMA_FULL, LLE_C0_DMA_BUSY, LLE_C0_SEMAPHORE,
	LLE_C0_DP_START, LLE_C0_DP_END, LLE_C0_DP_CURRENT, LLE_C0_DP_STATUS,
	LLE_C0_DP_CLOCK, LLE_C0_DP_BUSY, LLE_C0_DP_PIPE_BUSY, LLE_C0_DP_TMEM_BUSY,
};

struct lle_vec {
	uint16_t e[8];
} __attribute__((aligned(16)));

struct rsp_lle {
	/* --- escalar --- */
	uint32_t r[32];
	uint32_t pc;        /* instrucción que sigue */
	uint32_t next_pc;   /* la de después (un salto tomado la cambia) */

	/* --- vectorial --- */
	struct lle_vec v[32];
	struct lle_vec acc[3];  /* 0 alto, 1 medio, 2 bajo: 48 bits por elemento */
	uint16_t vco;           /* bits 0–7 acarreo (bajo), 8–15 alto */
	uint16_t vcc;           /* bits 0–7 bajo, 8–15 alto */
	uint16_t vce;           /* bits 0–7 */
	int16_t div_in, div_out;
	bool div_dp;

	/* --- control --- */
	bool halt;              /* lo pone la E/S: HALT en SP_STATUS */
	uint8_t *dmem, *imem;   /* 4 KB cada una, alineadas a 16 */
	uint64_t instructions;  /* ejecutadas desde lle_reset */
	/* Accesos a COP0 por registro (MFC0, MTC0), para saber en qué espera
	 * el microcódigo (semáforo, SP_STATUS, DMA, DP). */
	uint32_t c0_reads[16], c0_writes[16];
	void *io;               /* estado de la plataforma */

	uint16_t rcp_table[512];
	uint16_t rsq_table[512];
};

/* Prepara las tablas y la memoria; `dmem` e `imem` los pone quien llama. */
void lle_init(struct rsp_lle *s, uint8_t *dmem, uint8_t *imem);
/* Registros a cero, PC en 0, detenido. */
void lle_reset(struct rsp_lle *s);
/* Fija el PC (escritura de SP_PC). */
void lle_set_pc(struct rsp_lle *s, uint32_t pc);
/* Ejecuta hasta `max` instrucciones o hasta que s->halt; devuelve las
 * ejecutadas. Cada LLE_POLL_INTERVAL instrucciones llama a lle_io_poll. */
#define LLE_POLL_INTERVAL 256u
uint32_t lle_run(struct rsp_lle *s, uint32_t max);

/* Una instrucción (para pruebas). */
void lle_step(struct rsp_lle *s);

/* ---- E/S (COP0): la da cada plataforma (core.c en el host y el PPE,
 * src/spu/rsp/io_spu.c en el SPE) ---- */
uint32_t lle_io_read(struct rsp_lle *s, unsigned reg);
void lle_io_write(struct rsp_lle *s, unsigned reg, uint32_t v);
/* BREAK: HALT y BROKE en SP_STATUS (e interrupción si INTR_BREAK). */
void lle_io_break(struct rsp_lle *s);
/* Cada LLE_POLL_INTERVAL instrucciones: la plataforma puede refrescar
 * s->halt (en el SPE, el CPU puede haber puesto HALT). */
void lle_io_poll(struct rsp_lle *s);

/* ---- Internas (vu.c, vmem.c) ---- */
void lle_vu_op(struct rsp_lle *s, uint32_t op);     /* COP2 con el bit 25 */
void lle_vu_move(struct rsp_lle *s, uint32_t op);   /* MFC2, CFC2, MTC2, CTC2 */
void lle_vu_load(struct rsp_lle *s, uint32_t op);   /* LWC2 */
void lle_vu_store(struct rsp_lle *s, uint32_t op);  /* SWC2 */

static inline uint8_t lle_vbyte(const struct lle_vec *v, unsigned i)
{
	i &= 15;
	return (uint8_t)(v->e[i >> 1] >> ((i & 1) ? 0 : 8));
}

static inline void lle_vbyte_set(struct lle_vec *v, unsigned i, uint8_t b)
{
	i &= 15;
	if (i & 1)
		v->e[i >> 1] = (uint16_t)((v->e[i >> 1] & 0xFF00u) | b);
	else
		v->e[i >> 1] = (uint16_t)((v->e[i >> 1] & 0x00FFu) | ((unsigned)b << 8));
}

#endif
