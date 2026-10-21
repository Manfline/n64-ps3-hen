/*
 * Salsa64 — pruebas del RSP a bajo nivel (src/rsp/lle) sobre el bus: el
 * CPU escribe código en la IMEM y datos en la DMEM, quita HALT y el
 * microcódigo corre en el intérprete; se comprueban la DMEM, SP_PC,
 * SP_STATUS y los registros a través de COP0 y las interacciones con el
 * planificador (rebanadas y sincronía al leer SP_STATUS), el DMA desde
 * el RSP y el camino DPC con xbus.
 *
 * Los valores de la unidad vectorial son los de la semántica documentada
 * (n64brew.dev, ares): VMULF con redondeo y saturación, VCH con sus
 * banderas, el recíproco por tabla.
 */
#include "test.h"

#include <string.h>

#include "core/endian.h"
#include "rsp/lle/core.h"

#define SP_REG(r)  (0x04040000u + 4 * (r))
#define SP_PC_REG  0x04080000u
#define DP_REG(r)  (0x04100000u + 4 * (r))
#define MI_INTR    0x04300008u
#define IMEM       0x04001000u
#define DMEM       0x04000000u

/* Codificación del RSP: la escalar es la de MIPS (mips.h); lo propio
 * (COP2, cargas y almacenamientos vectoriales) aquí. */
#define CFC2(rt, rd) ((0x12u << 26) | (2u << 21) | ((rt) << 16) | ((rd) << 11))
#define MTC2(rt, vs, e) ((0x12u << 26) | (4u << 21) | ((rt) << 16) | ((vs) << 11) | ((e) << 7))
#define MFC2(rt, vs, e) ((0x12u << 26) | (0u << 21) | ((rt) << 16) | ((vs) << 11) | ((e) << 7))
#define VU(fn, vd, vs, vt, e) ((0x12u << 26) | (1u << 25) | ((e) << 21) | ((vt) << 16) | ((vs) << 11) | ((vd) << 6) | (fn))
#define LWC2(sub, vt, e, base, off) ((0x32u << 26) | ((base) << 21) | ((vt) << 16) | ((sub) << 11) | ((e) << 7) | ((off) & 0x7Fu))
#define SWC2(sub, vt, e, base, off) ((0x3Au << 26) | ((base) << 21) | ((vt) << 16) | ((sub) << 11) | ((e) << 7) | ((off) & 0x7Fu))
#define LQV(vt, e, base, off) LWC2(4, vt, e, base, off)
#define SQV(vt, e, base, off) SWC2(4, vt, e, base, off)
#define VMULF 0x00
#define VMACF 0x08
#define VADD  0x10
#define VSAR  0x1D
#define VCH   0x25
#define VRCP  0x30
#define VRCPL 0x31
#define VRCPH 0x32

static struct salsa64_lle_host lle;

static void wr(uint32_t pa, uint32_t v) { salsa64_bus_write32(&t_n64.bus, pa, v, ~0u); }
static uint32_t rd(uint32_t pa) { return salsa64_bus_read32(&t_n64.bus, pa); }

static void setup(enum salsa64_lle_mode mode)
{
	salsa64_lle_host_init(&lle, &t_n64.bus);
	salsa64_rsp_set_lle(&t_n64.bus, &lle.backend, mode);
}

static void imem(const uint32_t *code, unsigned n)
{
	for (unsigned k = 0; k < n; k++)
		wr(IMEM + 4 * k, code[k]);
}

/* Arranca desde 0 y corre hasta `max` instrucciones (sin planificador). */
static uint32_t run_now(uint32_t max)
{
	wr(SP_PC_REG, 0);
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);  /* quitar HALT y BROKE */
	return salsa64_lle_host_run(&lle, max);
}

static void lle_scalar_and_break(void)
{
	setup(SALSA64_LLE_ALL);
	static const uint32_t code[] = {
		LUI(AT, 0x1234), ORI(AT, AT, 0x5678), ADDIU(V0, 0, 0x100), SW(AT, 4, V0),
		ADDIU(V1, AT, -8), SW(V1, 8, V0), NOP, BREAK,
	};
	imem(code, 8);
	uint32_t n = run_now(100);
	CHECK_EQ(n, 8);
	CHECK_EQ(rd(DMEM + 0x104), 0x12345678);
	CHECK_EQ(rd(DMEM + 0x108), 0x12345670);
	CHECK_EQ(rd(SP_PC_REG), 0x20);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)), SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_BROKE);
	wr(SP_REG(SALSA64_SP_STATUS), 0x4);  /* limpiar BROKE */
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)), SALSA64_SP_STATUS_HALT);
}

/* BREAK en el delay slot de un salto tomado: SP_PC queda en el destino. */
static void lle_break_in_delay_slot(void)
{
	setup(SALSA64_LLE_ALL);
	static const uint32_t code[] = { BEQ(0, 0, 6), BREAK };
	imem(code, 2);
	run_now(100);
	CHECK_EQ(rd(SP_PC_REG), 0x1C);
	static const uint32_t code2[] = { BNE(0, 0, 6), BREAK };
	imem(code2, 2);
	run_now(100);
	CHECK_EQ(rd(SP_PC_REG), 0x8);
}

/* Cargas y almacenamientos no alineados, byte a byte, con vuelta en 4 KB. */
static void lle_unaligned_memory(void)
{
	setup(SALSA64_LLE_ALL);
	static const uint32_t code[] = {
		LUI(AT, 0xAABB), ORI(AT, AT, 0xCCDD), ADDIU(V0, 0, 0x201), SW(AT, 0, V0),
		LW(V1, 0, V0), SW(V1, 0x300, 0), ADDIU(A0, 0, 0xFFE), SW(AT, 0, A0), BREAK,
	};
	imem(code, 9);
	run_now(100);
	CHECK_EQ(rd(DMEM + 0x200), 0x00AABBCC);
	CHECK_EQ(rd(DMEM + 0x204), 0xDD000000);
	CHECK_EQ(rd(DMEM + 0x300), 0xAABBCCDD);
	CHECK_EQ(rd(DMEM + 0xFFC), 0x0000AABB);  /* los otros dos bytes dan la vuelta */
	CHECK_EQ(rd(DMEM + 0x000) >> 16, 0xCCDD);
}

/* VMULF: acc = vs·vt·2 + 0x8000, vd = acc medio saturado; VMACF acumula. */
static void lle_vector_mulf_macf(void)
{
	setup(SALSA64_LLE_ALL);
	/* vs = 0x4000 (0,5), vt = 0x4000 → 0,25 = 0x2000; otro elemento con
	 * saturación: 0x8000 · 0x8000 → +1,0 satura a 0x7FFF. */
	wr(DMEM + 0x000, 0x40008000); wr(DMEM + 0x004, 0x7FFF0001); wr(DMEM + 0x008, 0); wr(DMEM + 0x00C, 0);
	wr(DMEM + 0x010, 0x40008000); wr(DMEM + 0x014, 0x7FFF0001); wr(DMEM + 0x018, 0); wr(DMEM + 0x01C, 0);
	static const uint32_t code[] = {
		LQV(1, 0, 0, 0), LQV(2, 0, 0, 1),
		VU(VMULF, 3, 1, 2, 0), SQV(3, 0, 0, 2),
		VU(VSAR, 4, 0, 0, 9), SQV(4, 0, 0, 3),   /* acc medio */
		VU(VSAR, 4, 0, 0, 10), SQV(4, 0, 0, 4),  /* acc bajo */
		VU(VMACF, 5, 1, 2, 0), SQV(5, 0, 0, 5),
		BREAK,
	};
	imem(code, 11);
	run_now(100);
	/* 0x4000·0x4000·2 + 0x8000 = 0x20008000 → medio 0x2000, bajo 0x8000. */
	CHECK_EQ(rd(DMEM + 0x020), 0x20007FFF);
	CHECK_EQ(rd(DMEM + 0x024), 0x7FFE0000);  /* 0x7FFF²·2 + 0x8000 = 0x7FFE8002 → 0x7FFE; 1·1·2 + 0x8000 → 0 */
	CHECK_EQ(rd(DMEM + 0x030), 0x20008000);  /* medio: 0x2000, 0x8000·0x8000·2 = 0x80000000 → 0x8000 */
	CHECK_EQ(rd(DMEM + 0x040), 0x80008000);  /* bajo: 0x8000 y 0x8000 */
	/* Segunda vuelta acumulada: 0x20008000 + 0x20000000 = 0x40008000 → 0x4000. */
	CHECK_EQ(rd(DMEM + 0x050) >> 16, 0x4000);
}

/* VCH: banderas de recorte (VCC, VCO, VCE) leídas con CFC2. */
static void lle_vector_vch_flags(void)
{
	setup(SALSA64_LLE_ALL);
	wr(DMEM + 0x000, 0x0005FFFB); wr(DMEM + 0x004, 0); wr(DMEM + 0x008, 0); wr(DMEM + 0x00C, 0);
	wr(DMEM + 0x010, 0x00030003); wr(DMEM + 0x014, 0); wr(DMEM + 0x018, 0); wr(DMEM + 0x01C, 0);
	static const uint32_t code[] = {
		LQV(1, 0, 0, 0), LQV(2, 0, 0, 1),
		VU(VCH, 3, 1, 2, 0), SQV(3, 0, 0, 2),
		CFC2(T0, 0), SW(T0, 0x30, 0),
		CFC2(T0, 1), SW(T0, 0x34, 0),
		CFC2(T0, 2), SW(T0, 0x38, 0),
		BREAK,
	};
	imem(code, 11);
	run_now(100);
	CHECK_EQ(rd(DMEM + 0x020), 0x0003FFFD);  /* min(5,3) = 3; -5 con 3: -3 */
	CHECK_EQ(rd(DMEM + 0x030), 0x00000302);  /* VCO: bajo n=1; alto n=0,1 */
	CHECK_EQ(rd(DMEM + 0x034), 0xFFFFFD02);  /* VCC (con signo): bajo n=1; alto todos menos n=1 */
	CHECK_EQ(rd(DMEM + 0x038), 0);
}

/* VRCP por tabla: 1/0x0100 y el caso cero; VRCPH/VRCPL en 32 bits. */
static void lle_vector_rcp(void)
{
	setup(SALSA64_LLE_ALL);
	wr(DMEM + 0x000, 0x01000000); wr(DMEM + 0x004, 0x00010000); wr(DMEM + 0x008, 0); wr(DMEM + 0x00C, 0);
	static const uint32_t code[] = {
		LQV(1, 0, 0, 0),
		VU(VRCP, 2, 0, 1, 0), MFC2(T0, 2, 0), SW(T0, 0x20, 0),     /* 1/0x100 */
		VU(VRCP, 2, 0, 1, 1), MFC2(T0, 2, 0), SW(T0, 0x24, 0),     /* 1/0: 0xFFFF */
		VU(VRCPH, 3, 0, 1, 2), VU(VRCPL, 3, 0, 1, 3),              /* 1/(0x00010000): 32 bits */
		MFC2(T0, 3, 0), SW(T0, 0x28, 0),
		VU(VRCPH, 4, 0, 1, 0), MFC2(T0, 4, 0), SW(T0, 0x2C, 0),    /* mitad alta del anterior */
		BREAK,
	};
	imem(code, 14);
	run_now(100);
	CHECK_EQ(rd(DMEM + 0x020), 0xFFFFFFC0);  /* 0x7FFFC0 → 0xFFC0, con signo por MFC2 */
	CHECK_EQ(rd(DMEM + 0x024), 0xFFFFFFFF);
	CHECK_EQ(rd(DMEM + 0x028), 0x00007FFF);  /* 1/(1,0 en s15.16): 0x0000.7FFF */
	CHECK_EQ(rd(DMEM + 0x02C), 0x00000000);  /* su mitad alta */
}

/* LQV/SQV no alineadas y LDV/MTC2/MFC2 por elemento. */
static void lle_vector_loads_stores(void)
{
	setup(SALSA64_LLE_ALL);
	for (unsigned k = 0; k < 0x40; k += 4)
		wr(DMEM + k, 0x00010203u + 0x04040404u * (k / 4));
	static const uint32_t code[] = {
		ADDIU(AT, 0, 0x4), LQV(1, 0, AT, 0),   /* desde 4: 12 bytes a los elementos 0–11 */
		SQV(1, 0, 0, 8),                       /* a 0x80 */
		ADDIU(A0, 0, 0x96), SQV(1, 0, A0, 0),  /* desde 0x96: 10 bytes hasta el bloque */
		LWC2(3, 2, 4, 0, 2),                   /* LDV e=4 desde 0x10: elementos 4–11 */
		SQV(2, 0, 0, 10),                      /* a 0xA0 */
		LUI(T0, 0xBEEF), ORI(T0, T0, 0xCAFE), MTC2(T0, 3, 3),
		MFC2(T1, 3, 3), SW(T1, 0xB0, 0),
		MFC2(T1, 3, 2), SW(T1, 0xB4, 0),
		BREAK,
	};
	imem(code, 14);
	run_now(100);
	CHECK_EQ(rd(DMEM + 0x80), 0x04050607);
	CHECK_EQ(rd(DMEM + 0x88), 0x0C0D0E0F);
	CHECK_EQ(rd(DMEM + 0x8C), 0x00000000);
	CHECK_EQ(rd(DMEM + 0x94), 0x00000405);  /* 0x96: elementos 0.. hasta 0x9F */
	CHECK_EQ(rd(DMEM + 0x9C), 0x0A0B0C0D);
	CHECK_EQ(rd(DMEM + 0xA0), 0x00000000);
	CHECK_EQ(rd(DMEM + 0xA4), 0x10111213);
	CHECK_EQ(rd(DMEM + 0xA8), 0x14151617);
	CHECK_EQ(rd(DMEM + 0xAC), 0x00000000);
	CHECK_EQ(rd(DMEM + 0xB0), 0xFFFFCAFE);  /* MFC2 e=3: bytes 3,4 del registro, con signo */
	CHECK_EQ(rd(DMEM + 0xB4), 0x000000CA);  /* e=2: byte 2 (cero) y byte 3 (0xCA) */
}

/* DMA desde el RSP por COP0: RDRAM → DMEM y DMEM → RDRAM. */
static void lle_dma_from_rsp(void)
{
	setup(SALSA64_LLE_ALL);
	for (unsigned k = 0; k < 0x40; k += 4)
		t_wr32(0x5000 + k, 0xD0000000u + k);
	static const uint32_t code[] = {
		ADDIU(AT, 0, 0x200), MTC0(AT, LLE_C0_MEM_ADDR),
		ADDIU(V0, 0, 0x5000), MTC0(V0, LLE_C0_DRAM_ADDR),
		ADDIU(V1, 0, 0x3F), MTC0(V1, LLE_C0_RD_LEN),
		MFC0(T0, LLE_C0_DMA_BUSY), SW(T0, 0x300, 0),
		LW(T1, 0x210, 0), ADDIU(T1, T1, 1), SW(T1, 0x210, 0),
		ADDIU(AT, 0, 0x200), MTC0(AT, LLE_C0_MEM_ADDR),
		ADDIU(V0, 0, 0x6000), MTC0(V0, LLE_C0_DRAM_ADDR),
		ADDIU(V1, 0, 0x3F), MTC0(V1, LLE_C0_WR_LEN),
		BREAK,
	};
	imem(code, 18);
	run_now(100);
	CHECK_EQ(rd(DMEM + 0x200), 0xD0000000);
	CHECK_EQ(rd(DMEM + 0x23C), 0xD000003C);
	CHECK_EQ(rd(DMEM + 0x300), 0);
	CHECK_EQ(t_rd32(0x6010), 0xD0000011);
	CHECK_EQ(t_rd32(0x603C), 0xD000003C);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_DRAM_ADDR)), 0x6040);
}

/* SP_STATUS y el semáforo desde el RSP: señales, interrupción, HALT. */
static void lle_status_from_rsp(void)
{
	setup(SALSA64_LLE_ALL);
	static const uint32_t code[] = {
		MFC0(T0, LLE_C0_SEMAPHORE), SW(T0, 0x100, 0),   /* 0 y lo toma */
		MFC0(T0, LLE_C0_SEMAPHORE), SW(T0, 0x104, 0),   /* 1 */
		MTC0(0, LLE_C0_SEMAPHORE),                      /* lo suelta */
		LUI(AT, 0x1), ORI(AT, AT, 0x4000), MTC0(AT, LLE_C0_STATUS),  /* señal 2 y señal 3 */
		MFC0(T0, LLE_C0_STATUS), SW(T0, 0x108, 0),
		ADDIU(AT, 0, 0x2), MTC0(AT, LLE_C0_STATUS),     /* HALT a sí mismo: no sigue */
		ADDIU(T1, 0, 0x77), SW(T1, 0x10C, 0), BREAK,
	};
	imem(code, 15);
	uint32_t n = run_now(100);
	CHECK_EQ(n, 12);
	CHECK_EQ(rd(DMEM + 0x100), 0);
	CHECK_EQ(rd(DMEM + 0x104), 1);
	CHECK_EQ(rd(DMEM + 0x108), SALSA64_SP_STATUS_SIG(2) | SALSA64_SP_STATUS_SIG(3));
	CHECK_EQ(rd(DMEM + 0x10C), 0);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)),
		 SALSA64_SP_STATUS_HALT | SALSA64_SP_STATUS_SIG(2) | SALSA64_SP_STATUS_SIG(3));
	CHECK_EQ(rd(SP_REG(SALSA64_SP_SEMAPHORE)), 0);  /* lo soltó el RSP */
	/* Quitar HALT sigue donde iba (sin tocar SP_PC). */
	wr(SP_REG(SALSA64_SP_STATUS), 0x1);
	salsa64_lle_host_run(&lle, 100);
	CHECK_EQ(rd(DMEM + 0x10C), 0x77);
	CHECK(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_BROKE);
}

/* Rebanadas por el planificador: el CPU sondea SP_STATUS y ve el RSP
 * avanzar a 2 instrucciones por cada 3 ciclos; BREAK con INTR_BREAK
 * interrumpe. */
static void lle_scheduler_slices(void)
{
	setup(SALSA64_LLE_ALL);
	/* 3000 vueltas de 3 instrucciones y BREAK: ~9000 instrucciones del RSP. */
	static const uint32_t code[] = {
		ADDIU(AT, 0, 3000),
		ADDIU(AT, AT, -1), BNE(AT, 0, -2), NOP,
		ADDIU(T0, 0, 0x42), SW(T0, 0x100, 0), BREAK,
	};
	imem(code, 7);
	wr(SP_REG(SALSA64_SP_STATUS), 0x100);  /* INTR_BREAK */
	wr(SP_PC_REG, 0);
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK(lle.running);
	t_step(1000);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, 0);
	CHECK(lle.instructions >= 600 && lle.instructions <= 800);  /* 1000 ciclos × 2/3, en rebanadas de ~1024 */
	CHECK_EQ(rd(DMEM + 0x100), 0);
	t_step(14000);
	CHECK_EQ(rd(SP_REG(SALSA64_SP_STATUS)) & SALSA64_SP_STATUS_HALT, SALSA64_SP_STATUS_HALT);
	CHECK_EQ(rd(DMEM + 0x100), 0x42);
	CHECK_EQ(lle.instructions, 1 + 3000 * 3 + 3);
	CHECK(!lle.running);
	CHECK_EQ(rd(MI_INTR) & 1u, 1u);
}

/* DPC con xbus: el RSP escribe comandos en la DMEM y los entrega por COP0;
 * el consumidor los recibe y SYNC_FULL trae la interrupción del DP. */
static struct {
	int runs;
	uint32_t start, end;
	bool xbus;
} dpc_seen;

static uint32_t fake_dpc(void *ctx, uint32_t start, uint32_t end, const uint8_t *dmem)
{
	(void)ctx;
	dpc_seen.runs++;
	dpc_seen.start = start;
	dpc_seen.end = end;
	dpc_seen.xbus = dmem != NULL;
	uint32_t w0 = dmem ? salsa64_rd32(dmem + ((end - 8) & 0xFFF)) : rd(DMEM + end - 8);
	return (w0 >> 24) == 0xE9 ? SALSA64_GFX_FULLSYNC : 0;
}

static const struct salsa64_dpc_backend fake_dpc_backend = { NULL, fake_dpc };

static void lle_dpc_xbus(void)
{
	setup(SALSA64_LLE_ALL);
	salsa64_rsp_set_dpc(&t_n64.bus, &fake_dpc_backend);
	memset(&dpc_seen, 0, sizeof dpc_seen);
	wr(DMEM + 0x800, 0xF7000000); wr(DMEM + 0x804, 0x12341234);  /* SETFILLCOLOR */
	wr(DMEM + 0x808, 0xE9000000); wr(DMEM + 0x80C, 0);           /* SYNC_FULL */
	static const uint32_t code[] = {
		ADDIU(AT, 0, 0x2), MTC0(AT, LLE_C0_DP_STATUS),     /* xbus */
		ADDIU(AT, 0, 0x800), MTC0(AT, LLE_C0_DP_START),
		ADDIU(AT, 0, 0x810), MTC0(AT, LLE_C0_DP_END),
		MFC0(T0, LLE_C0_DP_CURRENT), SW(T0, 0x100, 0),
		MFC0(T0, LLE_C0_DP_STATUS), SW(T0, 0x104, 0),
		BREAK,
	};
	imem(code, 11);
	run_now(100);
	CHECK_EQ(dpc_seen.runs, 1);
	CHECK_EQ(dpc_seen.start, 0x800);
	CHECK_EQ(dpc_seen.end, 0x810);
	CHECK(dpc_seen.xbus);
	CHECK_EQ(rd(DMEM + 0x100), 0x810);
	CHECK_EQ(rd(DMEM + 0x104) & SALSA64_DP_STATUS_XBUS, SALSA64_DP_STATUS_XBUS);
	CHECK_EQ(rd(MI_INTR) & (1u << SALSA64_IRQ_DP), 0);
	t_step(t_n64.bus.rsp.dp_cycles + 10);
	CHECK_EQ(rd(MI_INTR) & (1u << SALSA64_IRQ_DP), 1u << SALSA64_IRQ_DP);
	/* Desde el CPU, sin xbus y congelado: no llega hasta descongelar. */
	wr(DP_REG(SALSA64_DP_STATUS), 0x1 | 0x8);
	wr(DP_REG(SALSA64_DP_START), 0x1000);
	wr(DP_REG(SALSA64_DP_END), 0x1010);
	CHECK_EQ(dpc_seen.runs, 1);
	wr(DP_REG(SALSA64_DP_STATUS), 0x4);
	CHECK_EQ(dpc_seen.runs, 2);
	CHECK_EQ(dpc_seen.start, 0x1000);
	CHECK(!dpc_seen.xbus);
	CHECK_EQ(rd(DP_REG(SALSA64_DP_CURRENT)), 0x1010);
	salsa64_rsp_set_dpc(&t_n64.bus, NULL);
}

/* Modo auto: una tarea con HLE sigue en la HLE; sin HLE va al microcódigo.
 * Modo all: todo al microcódigo. */
static struct { int starts; } hle;

static void hle_start(void *ctx, const struct salsa64_task *task, enum salsa64_gbi gbi)
{
	(void)ctx, (void)task, (void)gbi;
	hle.starts++;
}

static uint32_t hle_finish(void *ctx)
{
	(void)ctx;
	return 0;
}

static const struct salsa64_gfx_backend hle_backend = { .start = hle_start, .finish = hle_finish };

static void task_gfx(uint32_t ucode_data, const char *text)
{
	wr(DMEM + 0xFC0, SALSA64_TASK_GFX);
	wr(DMEM + 0xFD8, ucode_data);
	wr(DMEM + 0xFDC, 0x100);
	wr(DMEM + 0xFF0, 0x2000);
	wr(DMEM + 0xFF4, 8);
	for (unsigned k = 0; text[k]; k++)
		t_n64.bus.rdram[ucode_data + k] = (uint8_t)text[k];
}

static void lle_mode_auto_and_all(void)
{
	setup(SALSA64_LLE_AUTO);
	salsa64_rsp_set_gfx(&t_n64.bus, &hle_backend);
	hle.starts = 0;
	static const uint32_t code[] = { ADDIU(T0, 0, 0x99), SW(T0, 0x100, 0), BREAK };
	imem(code, 3);
	task_gfx(0x3000, "Salsa64 GBI F3DEX2");
	wr(SP_PC_REG, 0);
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK_EQ(hle.starts, 1);
	CHECK(!lle.running);
	salsa64_rsp_reset(&t_n64.bus);
	wr(SP_REG(SALSA64_SP_STATUS), 0x2);  /* HALT: el reset no lo repone */

	/* Las dos familias de Rare también tienen HLE: auto no debe mandarlas al
	 * microcódigo real solo porque se reconocen de forma distinta. */
	task_gfx(0x3000, "Salsa64 GBI F3DPD");
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK_EQ(hle.starts, 2);
	CHECK(!lle.running);
	salsa64_rsp_reset(&t_n64.bus);
	wr(SP_REG(SALSA64_SP_STATUS), 0x2);

	task_gfx(0x3000, "Salsa64 GBI F3DEXBG");
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK_EQ(hle.starts, 3);
	CHECK(!lle.running);
	salsa64_rsp_reset(&t_n64.bus);
	wr(SP_REG(SALSA64_SP_STATUS), 0x2);

	task_gfx(0x3000, "microcodigo propio");
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK_EQ(hle.starts, 3);
	CHECK(lle.running);
	salsa64_lle_host_run(&lle, 100);
	CHECK_EQ(rd(DMEM + 0x100), 0x99);
	CHECK(t_n64.bus.rsp.stats.lle >= 1);
	salsa64_rsp_reset(&t_n64.bus);
	wr(SP_REG(SALSA64_SP_STATUS), 0x2);

	salsa64_rsp_set_lle(&t_n64.bus, &lle.backend, SALSA64_LLE_ALL);
	wr(DMEM + 0x100, 0);
	task_gfx(0x3000, "Salsa64 GBI F3DEX2");
	wr(SP_PC_REG, 0);
	wr(SP_REG(SALSA64_SP_STATUS), 0x5);
	CHECK_EQ(hle.starts, 3);
	CHECK(lle.running);
	salsa64_lle_host_run(&lle, 100);
	CHECK_EQ(rd(DMEM + 0x100), 0x99);
	salsa64_rsp_set_gfx(&t_n64.bus, NULL);
}

void t_suite_rsp_lle(void)
{
	RUN(lle_scalar_and_break);
	RUN(lle_break_in_delay_slot);
	RUN(lle_unaligned_memory);
	RUN(lle_vector_mulf_macf);
	RUN(lle_vector_vch_flags);
	RUN(lle_vector_rcp);
	RUN(lle_vector_loads_stores);
	RUN(lle_dma_from_rsp);
	RUN(lle_status_from_rsp);
	RUN(lle_scheduler_slices);
	RUN(lle_dpc_xbus);
	RUN(lle_mode_auto_and_all);
	salsa64_rsp_set_lle(&t_n64.bus, NULL, SALSA64_LLE_OFF);
}
