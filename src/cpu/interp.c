/*
 * Salsa64 — intérprete del VR4300: bucle, decodificación, enteros, saltos,
 * cargas y almacenamientos. COP0 en cop0.c y COP1 en cop1.c.
 *
 * Escrito desde la documentación pública (manual del VR4300 de NEC, MIPS IV
 * ISA, n64brew.dev); no deriva de código de otros emuladores.
 *
 * Notas del VR4300 que se reproducen a propósito:
 *   - Las operaciones de 32 bits usan los 32 bits bajos de sus entradas y
 *     extienden el signo del resultado a 64.
 *   - SRA/SRAV desplazan el registro de 64 bits completo y luego extienden
 *     el signo de los 32 bits bajos (bits altos entran al resultado).
 *   - DIV/DIVU/DDIV/DDIVU entre cero no son excepción: LO = ±1 o -1,
 *     HI = dividendo.
 *   - LWL y LWR con la palabra completa extienden el signo del resultado de
 *     32 bits; LWR parcial deja los 32 bits altos de rt sin tocar.
 *   - La dirección de retorno de JAL, JALR y BxxAL es next_pc: la
 *     instrucción que sigue al delay slot, también cuando el salto está a su
 *     vez en el delay slot de otro (entonces es el destino de ese + 4).
 *   - Status.RE en modo usuario (little-endian): la dirección física se
 *     invierte con XOR 7, 6, 4 o 0 según el tamaño (también al buscar
 *     instrucciones), y LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR toman el desplazamiento
 *     de esa dirección ya invertida. BadVAddr conserva la original.
 */
#include "core/sched.h"
#include "cpu/vr4300_internal.h"

#ifndef __SIZEOF_INT128__
#error "DMULT/DMULTU necesitan __int128 (ppu-gcc y gcc de 64 bits lo tienen)"
#endif

#define REQUIRE64(cpu)                                   \
	do {                                             \
		if (SALSA64_UNLIKELY(!(cpu)->allow64)) { \
			vr4300_reserved(cpu);            \
			return;                          \
		}                                        \
	} while (0)

static inline void trap_if(struct salsa64_cpu *cpu, bool cond)
{
	if (cond)
		salsa64_vr4300_exception(cpu, SALSA64_EXC_TR, 0);
}

/* ---- HI/LO --------------------------------------------------------------- */

static void op_div32(struct salsa64_cpu *cpu, uint64_t rs, uint64_t rt)
{
	int32_t a = (int32_t)rs, b = (int32_t)rt;
	if (b == 0) {
		cpu->lo = a < 0 ? 1 : ~0ull;
		cpu->hi = salsa64_sx32((uint32_t)a);
	} else {
		/* En 64 bits para que INT32_MIN / -1 no desborde: da INT32_MIN y 0. */
		int64_t q = (int64_t)a / b, r = (int64_t)a % b;
		cpu->lo = salsa64_sx32((uint32_t)q);
		cpu->hi = salsa64_sx32((uint32_t)r);
	}
}

static void op_divu32(struct salsa64_cpu *cpu, uint64_t rs, uint64_t rt)
{
	uint32_t a = (uint32_t)rs, b = (uint32_t)rt;
	if (b == 0) {
		cpu->lo = ~0ull;
		cpu->hi = salsa64_sx32(a);
	} else {
		cpu->lo = salsa64_sx32(a / b);
		cpu->hi = salsa64_sx32(a % b);
	}
}

static void op_div64(struct salsa64_cpu *cpu, uint64_t rs, uint64_t rt)
{
	int64_t a = (int64_t)rs, b = (int64_t)rt;
	if (b == 0) {
		cpu->lo = a < 0 ? 1 : ~0ull;
		cpu->hi = (uint64_t)a;
	} else if (a == INT64_MIN && b == -1) {
		cpu->lo = (uint64_t)a;
		cpu->hi = 0;
	} else {
		cpu->lo = (uint64_t)(a / b);
		cpu->hi = (uint64_t)(a % b);
	}
}

static void op_divu64(struct salsa64_cpu *cpu, uint64_t a, uint64_t b)
{
	if (b == 0) {
		cpu->lo = ~0ull;
		cpu->hi = a;
	} else {
		cpu->lo = a / b;
		cpu->hi = a % b;
	}
}

/* ---- SPECIAL ------------------------------------------------------------- */

static void op_special(struct salsa64_cpu *cpu, uint32_t i)
{
	uint64_t *r = cpu->gpr;
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i), sa = I_SA(i);

	switch (I_FUNCT(i)) {
	case 0x00: r[rd] = salsa64_sx32((uint32_t)r[rt] << sa); return;                  /* SLL */
	case 0x02: r[rd] = salsa64_sx32((uint32_t)r[rt] >> sa); return;                  /* SRL */
	case 0x03: r[rd] = salsa64_sx32((uint32_t)((int64_t)r[rt] >> sa)); return;       /* SRA */
	case 0x04: r[rd] = salsa64_sx32((uint32_t)r[rt] << (r[rs] & 31)); return;        /* SLLV */
	case 0x06: r[rd] = salsa64_sx32((uint32_t)r[rt] >> (r[rs] & 31)); return;        /* SRLV */
	case 0x07: r[rd] = salsa64_sx32((uint32_t)((int64_t)r[rt] >> (r[rs] & 31))); return; /* SRAV */
	case 0x08: vr4300_jump(cpu, r[rs]); return;                                       /* JR */
	case 0x09: {                                                                      /* JALR */
		uint64_t target = r[rs];
		r[rd] = cpu->next_pc;
		vr4300_jump(cpu, target);
		return;
	}
	case 0x0C: salsa64_vr4300_exception(cpu, SALSA64_EXC_SYS, 0); return;             /* SYSCALL */
	case 0x0D: salsa64_vr4300_exception(cpu, SALSA64_EXC_BP, 0); return;              /* BREAK */
	case 0x0F: return;                                                                /* SYNC */
	case 0x10: r[rd] = cpu->hi; return;                                               /* MFHI */
	case 0x11: cpu->hi = r[rs]; return;                                               /* MTHI */
	case 0x12: r[rd] = cpu->lo; return;                                               /* MFLO */
	case 0x13: cpu->lo = r[rs]; return;                                               /* MTLO */
	case 0x14: REQUIRE64(cpu); r[rd] = r[rt] << (r[rs] & 63); return;                 /* DSLLV */
	case 0x16: REQUIRE64(cpu); r[rd] = r[rt] >> (r[rs] & 63); return;                 /* DSRLV */
	case 0x17: REQUIRE64(cpu); r[rd] = (uint64_t)((int64_t)r[rt] >> (r[rs] & 63)); return; /* DSRAV */
	case 0x18: {                                                                      /* MULT */
		/* El multiplicador toma rs con sus 64 bits y rt como entero de 35
		 * bits con signo; con entradas que son extensión de signo de 32 bits
		 * da lo mismo que 32 × 32 (nemu64-test, arithmetic/mul.rs). */
		int64_t b = (int64_t)(r[rt] << 29) >> 29;
		uint64_t p = r[rs] * (uint64_t)b;
		cpu->lo = salsa64_sx32((uint32_t)p);
		cpu->hi = salsa64_sx32((uint32_t)(p >> 32));
		return;
	}
	case 0x19: {                                                                      /* MULTU */
		uint64_t p = (uint64_t)(uint32_t)r[rs] * (uint32_t)r[rt];
		cpu->lo = salsa64_sx32((uint32_t)p);
		cpu->hi = salsa64_sx32((uint32_t)(p >> 32));
		return;
	}
	case 0x1A: op_div32(cpu, r[rs], r[rt]); return;                                   /* DIV */
	case 0x1B: op_divu32(cpu, r[rs], r[rt]); return;                                  /* DIVU */
	case 0x1C: {                                                                      /* DMULT */
		REQUIRE64(cpu);
		__int128 p = (__int128)(int64_t)r[rs] * (int64_t)r[rt];
		cpu->lo = (uint64_t)p;
		cpu->hi = (uint64_t)((unsigned __int128)p >> 64);
		return;
	}
	case 0x1D: {                                                                      /* DMULTU */
		REQUIRE64(cpu);
		unsigned __int128 p = (unsigned __int128)r[rs] * r[rt];
		cpu->lo = (uint64_t)p;
		cpu->hi = (uint64_t)(p >> 64);
		return;
	}
	case 0x1E: REQUIRE64(cpu); op_div64(cpu, r[rs], r[rt]); return;                   /* DDIV */
	case 0x1F: REQUIRE64(cpu); op_divu64(cpu, r[rs], r[rt]); return;                  /* DDIVU */
	case 0x20: {                                                                      /* ADD */
		uint32_t a = (uint32_t)r[rs], b = (uint32_t)r[rt], s = a + b;
		if ((~(a ^ b) & (a ^ s)) >> 31)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rd] = salsa64_sx32(s);
		return;
	}
	case 0x21: r[rd] = salsa64_sx32((uint32_t)r[rs] + (uint32_t)r[rt]); return;      /* ADDU */
	case 0x22: {                                                                      /* SUB */
		uint32_t a = (uint32_t)r[rs], b = (uint32_t)r[rt], d = a - b;
		if (((a ^ b) & (a ^ d)) >> 31)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rd] = salsa64_sx32(d);
		return;
	}
	case 0x23: r[rd] = salsa64_sx32((uint32_t)r[rs] - (uint32_t)r[rt]); return;      /* SUBU */
	case 0x24: r[rd] = r[rs] & r[rt]; return;                                         /* AND */
	case 0x25: r[rd] = r[rs] | r[rt]; return;                                         /* OR */
	case 0x26: r[rd] = r[rs] ^ r[rt]; return;                                         /* XOR */
	case 0x27: r[rd] = ~(r[rs] | r[rt]); return;                                      /* NOR */
	case 0x2A: r[rd] = (int64_t)r[rs] < (int64_t)r[rt]; return;                       /* SLT */
	case 0x2B: r[rd] = r[rs] < r[rt]; return;                                         /* SLTU */
	case 0x2C: {                                                                      /* DADD */
		REQUIRE64(cpu);
		uint64_t a = r[rs], b = r[rt], s = a + b;
		if ((~(a ^ b) & (a ^ s)) >> 63)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rd] = s;
		return;
	}
	case 0x2D: REQUIRE64(cpu); r[rd] = r[rs] + r[rt]; return;                         /* DADDU */
	case 0x2E: {                                                                      /* DSUB */
		REQUIRE64(cpu);
		uint64_t a = r[rs], b = r[rt], d = a - b;
		if (((a ^ b) & (a ^ d)) >> 63)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rd] = d;
		return;
	}
	case 0x2F: REQUIRE64(cpu); r[rd] = r[rs] - r[rt]; return;                         /* DSUBU */
	case 0x30: trap_if(cpu, (int64_t)r[rs] >= (int64_t)r[rt]); return;                /* TGE */
	case 0x31: trap_if(cpu, r[rs] >= r[rt]); return;                                  /* TGEU */
	case 0x32: trap_if(cpu, (int64_t)r[rs] < (int64_t)r[rt]); return;                 /* TLT */
	case 0x33: trap_if(cpu, r[rs] < r[rt]); return;                                   /* TLTU */
	case 0x34: trap_if(cpu, r[rs] == r[rt]); return;                                  /* TEQ */
	case 0x36: trap_if(cpu, r[rs] != r[rt]); return;                                  /* TNE */
	case 0x38: REQUIRE64(cpu); r[rd] = r[rt] << sa; return;                           /* DSLL */
	case 0x3A: REQUIRE64(cpu); r[rd] = r[rt] >> sa; return;                           /* DSRL */
	case 0x3B: REQUIRE64(cpu); r[rd] = (uint64_t)((int64_t)r[rt] >> sa); return;      /* DSRA */
	case 0x3C: REQUIRE64(cpu); r[rd] = r[rt] << (sa + 32); return;                    /* DSLL32 */
	case 0x3E: REQUIRE64(cpu); r[rd] = r[rt] >> (sa + 32); return;                    /* DSRL32 */
	case 0x3F: REQUIRE64(cpu); r[rd] = (uint64_t)((int64_t)r[rt] >> (sa + 32)); return; /* DSRA32 */
	default:   vr4300_reserved(cpu); return;
	}
}

/* ---- REGIMM -------------------------------------------------------------- */

static void op_regimm(struct salsa64_cpu *cpu, uint32_t i)
{
	/* Se lee rs antes de escribir el enlace en r31. */
	int64_t s = (int64_t)cpu->gpr[I_RS(i)];
	uint64_t imm = I_SIMM(i);

	switch (I_RT(i)) {
	case 0x00: vr4300_branch(cpu, s < 0, i); return;                 /* BLTZ */
	case 0x01: vr4300_branch(cpu, s >= 0, i); return;                /* BGEZ */
	case 0x02: vr4300_branch_likely(cpu, s < 0, i); return;          /* BLTZL */
	case 0x03: vr4300_branch_likely(cpu, s >= 0, i); return;         /* BGEZL */
	case 0x08: trap_if(cpu, s >= (int64_t)imm); return;              /* TGEI */
	case 0x09: trap_if(cpu, (uint64_t)s >= imm); return;             /* TGEIU */
	case 0x0A: trap_if(cpu, s < (int64_t)imm); return;               /* TLTI */
	case 0x0B: trap_if(cpu, (uint64_t)s < imm); return;              /* TLTIU */
	case 0x0C: trap_if(cpu, (uint64_t)s == imm); return;             /* TEQI */
	case 0x0E: trap_if(cpu, (uint64_t)s != imm); return;             /* TNEI */
	case 0x10: cpu->gpr[31] = cpu->next_pc; vr4300_branch(cpu, s < 0, i); return;         /* BLTZAL */
	case 0x11: cpu->gpr[31] = cpu->next_pc; vr4300_branch(cpu, s >= 0, i); return;        /* BGEZAL */
	case 0x12: cpu->gpr[31] = cpu->next_pc; vr4300_branch_likely(cpu, s < 0, i); return;  /* BLTZALL */
	case 0x13: cpu->gpr[31] = cpu->next_pc; vr4300_branch_likely(cpu, s >= 0, i); return; /* BGEZALL */
	default:   vr4300_reserved(cpu); return;
	}
}

/* ---- Cargas y almacenamientos no alineados (big-endian) ------------------- */

static void op_lwl_lwr(struct salsa64_cpu *cpu, uint32_t i, bool left)
{
	uint64_t va = cpu->gpr[I_RS(i)] + I_SIMM(i);
	uint32_t pa;
	if (!vr4300_vaddr(cpu, va, 0, SALSA64_ACC_LOAD, &pa))
		return;
	/* El desplazamiento sale de pa: con Status.RE ya viene invertido. */
	uint32_t w = vr4300_read32(cpu, pa & ~3u), old = (uint32_t)cpu->gpr[I_RT(i)], v;
	if (left) {
		unsigned shift = 8 * (pa & 3);
		v = (old & ~(0xFFFFFFFFu << shift)) | (w << shift);
	} else {
		unsigned shift = 8 * (3 - (pa & 3));
		v = (old & ~(0xFFFFFFFFu >> shift)) | (w >> shift);
		/* Si no carga el byte más significativo, LWR no extiende el signo:
		 * los 32 bits altos de rt quedan como estaban (nemu64-test). */
		if (shift) {
			cpu->gpr[I_RT(i)] = (cpu->gpr[I_RT(i)] & 0xFFFFFFFF00000000ull) | v;
			return;
		}
	}
	cpu->gpr[I_RT(i)] = salsa64_sx32(v);
}

static void op_ldl_ldr(struct salsa64_cpu *cpu, uint32_t i, bool left)
{
	REQUIRE64(cpu);
	uint64_t va = cpu->gpr[I_RS(i)] + I_SIMM(i);
	uint32_t pa;
	if (!vr4300_vaddr(cpu, va, 0, SALSA64_ACC_LOAD, &pa))
		return;
	uint64_t d = vr4300_read64(cpu, pa & ~7u), old = cpu->gpr[I_RT(i)];
	if (left) {
		unsigned shift = 8 * (pa & 7);
		cpu->gpr[I_RT(i)] = (old & ~(~0ull << shift)) | (d << shift);
	} else {
		unsigned shift = 8 * (7 - (pa & 7));
		cpu->gpr[I_RT(i)] = (old & ~(~0ull >> shift)) | (d >> shift);
	}
}

static void op_swl_swr(struct salsa64_cpu *cpu, uint32_t i, bool left)
{
	uint64_t va = cpu->gpr[I_RS(i)] + I_SIMM(i);
	uint32_t pa, v = (uint32_t)cpu->gpr[I_RT(i)];
	if (!vr4300_vaddr(cpu, va, 0, SALSA64_ACC_STORE, &pa))
		return;
	if (left) {
		unsigned shift = 8 * (pa & 3);
		vr4300_write32_masked(cpu, pa & ~3u, v >> shift, 0xFFFFFFFFu >> shift);
	} else {
		unsigned shift = 8 * (3 - (pa & 3));
		vr4300_write32_masked(cpu, pa & ~3u, v << shift, 0xFFFFFFFFu << shift);
	}
}

static void op_sdl_sdr(struct salsa64_cpu *cpu, uint32_t i, bool left)
{
	REQUIRE64(cpu);
	uint64_t va = cpu->gpr[I_RS(i)] + I_SIMM(i), v = cpu->gpr[I_RT(i)];
	uint32_t pa;
	if (!vr4300_vaddr(cpu, va, 0, SALSA64_ACC_STORE, &pa))
		return;
	if (left) {
		unsigned shift = 8 * (pa & 7);
		vr4300_write64_masked(cpu, pa & ~7u, v >> shift, ~0ull >> shift);
	} else {
		unsigned shift = 8 * (7 - (pa & 7));
		vr4300_write64_masked(cpu, pa & ~7u, v << shift, ~0ull << shift);
	}
}

/* ---- Coprocesadores sin implementar -------------------------------------- */

static void op_cop_unusable_or_ri(struct salsa64_cpu *cpu, unsigned cop)
{
	if (!(cpu->status & (SALSA64_ST_CU0 << cop)))
		salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, cop);
	else
		vr4300_reserved(cpu);
}

static inline bool cop1_usable(struct salsa64_cpu *cpu)
{
	if (SALSA64_LIKELY(cpu->status & SALSA64_ST_CU1))
		return true;
	salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, 1);
	return false;
}

/* ---- Decodificador principal --------------------------------------------- */

static void execute(struct salsa64_cpu *cpu, uint32_t i)
{
	uint64_t *r = cpu->gpr;
	unsigned rs = I_RS(i), rt = I_RT(i);
	uint64_t va = r[rs] + I_SIMM(i);  /* dirección efectiva de cargas/almacenamientos */
	uint32_t pa;

	switch (I_OP(i)) {
	case 0x00: op_special(cpu, i); return;
	case 0x01: op_regimm(cpu, i); return;
	case 0x02:                                                               /* J */
		vr4300_jump(cpu, ((cpu->cur_pc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2);
		return;
	case 0x03:                                                               /* JAL */
		r[31] = cpu->next_pc;
		vr4300_jump(cpu, ((cpu->cur_pc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2);
		return;
	case 0x04: vr4300_branch(cpu, r[rs] == r[rt], i); return;               /* BEQ */
	case 0x05: vr4300_branch(cpu, r[rs] != r[rt], i); return;               /* BNE */
	case 0x06: vr4300_branch(cpu, (int64_t)r[rs] <= 0, i); return;          /* BLEZ */
	case 0x07: vr4300_branch(cpu, (int64_t)r[rs] > 0, i); return;           /* BGTZ */
	case 0x08: {                                                             /* ADDI */
		uint32_t a = (uint32_t)r[rs], b = (uint32_t)I_SIMM(i), s = a + b;
		if ((~(a ^ b) & (a ^ s)) >> 31)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rt] = salsa64_sx32(s);
		return;
	}
	case 0x09: r[rt] = salsa64_sx32((uint32_t)r[rs] + (uint32_t)I_SIMM(i)); return; /* ADDIU */
	case 0x0A: r[rt] = (int64_t)r[rs] < (int64_t)I_SIMM(i); return;         /* SLTI */
	case 0x0B: r[rt] = r[rs] < I_SIMM(i); return;                           /* SLTIU */
	case 0x0C: r[rt] = r[rs] & I_UIMM(i); return;                           /* ANDI */
	case 0x0D: r[rt] = r[rs] | I_UIMM(i); return;                           /* ORI */
	case 0x0E: r[rt] = r[rs] ^ I_UIMM(i); return;                           /* XORI */
	case 0x0F: r[rt] = salsa64_sx32(i << 16); return;                       /* LUI */
	case 0x10: salsa64_vr4300_cop0(cpu, i); return;
	case 0x11: salsa64_vr4300_cop1(cpu, i); return;
	case 0x12: salsa64_vr4300_cop2(cpu, i); return;                         /* COP2 */
	case 0x14: vr4300_branch_likely(cpu, r[rs] == r[rt], i); return;       /* BEQL */
	case 0x15: vr4300_branch_likely(cpu, r[rs] != r[rt], i); return;       /* BNEL */
	case 0x16: vr4300_branch_likely(cpu, (int64_t)r[rs] <= 0, i); return;  /* BLEZL */
	case 0x17: vr4300_branch_likely(cpu, (int64_t)r[rs] > 0, i); return;   /* BGTZL */
	case 0x18: {                                                             /* DADDI */
		REQUIRE64(cpu);
		uint64_t a = r[rs], b = I_SIMM(i), s = a + b;
		if ((~(a ^ b) & (a ^ s)) >> 63)
			salsa64_vr4300_exception(cpu, SALSA64_EXC_OV, 0);
		else
			r[rt] = s;
		return;
	}
	case 0x19: REQUIRE64(cpu); r[rt] = r[rs] + I_SIMM(i); return;          /* DADDIU */
	case 0x1A: op_ldl_ldr(cpu, i, true); return;                            /* LDL */
	case 0x1B: op_ldl_ldr(cpu, i, false); return;                           /* LDR */
	case 0x20:                                                               /* LB */
		if (vr4300_vaddr(cpu, va, 0, SALSA64_ACC_LOAD, &pa))
			r[rt] = salsa64_sx8(vr4300_read8(cpu, pa));
		return;
	case 0x21:                                                               /* LH */
		if (vr4300_vaddr(cpu, va, 1, SALSA64_ACC_LOAD, &pa))
			r[rt] = salsa64_sx16(vr4300_read16(cpu, pa));
		return;
	case 0x22: op_lwl_lwr(cpu, i, true); return;                            /* LWL */
	case 0x23:                                                               /* LW */
		if (vr4300_vaddr(cpu, va, 3, SALSA64_ACC_LOAD, &pa))
			r[rt] = salsa64_sx32(vr4300_read32(cpu, pa));
		return;
	case 0x24:                                                               /* LBU */
		if (vr4300_vaddr(cpu, va, 0, SALSA64_ACC_LOAD, &pa))
			r[rt] = vr4300_read8(cpu, pa);
		return;
	case 0x25:                                                               /* LHU */
		if (vr4300_vaddr(cpu, va, 1, SALSA64_ACC_LOAD, &pa))
			r[rt] = vr4300_read16(cpu, pa);
		return;
	case 0x26: op_lwl_lwr(cpu, i, false); return;                           /* LWR */
	case 0x27:                                                               /* LWU */
		REQUIRE64(cpu);
		if (vr4300_vaddr(cpu, va, 3, SALSA64_ACC_LOAD, &pa))
			r[rt] = vr4300_read32(cpu, pa);
		return;
	case 0x28:                                                               /* SB */
		if (vr4300_vaddr(cpu, va, 0, SALSA64_ACC_STORE, &pa))
			vr4300_write8(cpu, pa, r[rt]);
		return;
	case 0x29:                                                               /* SH */
		if (vr4300_vaddr(cpu, va, 1, SALSA64_ACC_STORE, &pa))
			vr4300_write16(cpu, pa, r[rt]);
		return;
	case 0x2A: op_swl_swr(cpu, i, true); return;                            /* SWL */
	case 0x2B:                                                               /* SW */
		if (vr4300_vaddr(cpu, va, 3, SALSA64_ACC_STORE, &pa))
			vr4300_write32(cpu, pa, (uint32_t)r[rt]);
		return;
	case 0x2C: op_sdl_sdr(cpu, i, true); return;                            /* SDL */
	case 0x2D: op_sdl_sdr(cpu, i, false); return;                           /* SDR */
	case 0x2E: op_swl_swr(cpu, i, false); return;                           /* SWR */
	case 0x2F:                                                               /* CACHE: sin cachés emuladas */
		if (cpu->mode != SALSA64_MODE_KERNEL && !(cpu->status & SALSA64_ST_CU0))
			salsa64_vr4300_exception(cpu, SALSA64_EXC_CPU, 0);
		else if (va & 3)  /* alineada a 4 basta; a 2 da AdEL */
			salsa64_vr4300_address_error(cpu, va, SALSA64_ACC_LOAD);
		return;
	case 0x30:                                                               /* LL */
		if (vr4300_vaddr(cpu, va, 3, SALSA64_ACC_LOAD, &pa)) {
			r[rt] = salsa64_sx32(vr4300_read32(cpu, pa));
			cpu->lladdr = pa >> 4;
			cpu->llbit = 1;
		}
		return;
	case 0x31:                                                               /* LWC1 */
		if (cop1_usable(cpu) && vr4300_vaddr(cpu, va, 3, SALSA64_ACC_LOAD, &pa))
			vr4300_fpr_set32(cpu, rt, vr4300_read32(cpu, pa));
		return;
	case 0x32: op_cop_unusable_or_ri(cpu, 2); return;                       /* LWC2 */
	case 0x34:                                                               /* LLD */
		REQUIRE64(cpu);
		if (vr4300_vaddr(cpu, va, 7, SALSA64_ACC_LOAD, &pa)) {
			r[rt] = vr4300_read64(cpu, pa);
			cpu->lladdr = pa >> 4;
			cpu->llbit = 1;
		}
		return;
	case 0x35:                                                               /* LDC1 */
		if (cop1_usable(cpu) && vr4300_vaddr(cpu, va, 7, SALSA64_ACC_LOAD, &pa))
			vr4300_fpr_set64(cpu, rt, vr4300_read64(cpu, pa));
		return;
	case 0x36: op_cop_unusable_or_ri(cpu, 2); return;                       /* LDC2 */
	case 0x37:                                                               /* LD */
		REQUIRE64(cpu);
		if (vr4300_vaddr(cpu, va, 7, SALSA64_ACC_LOAD, &pa))
			r[rt] = vr4300_read64(cpu, pa);
		return;
	case 0x38:                                                               /* SC */
		if (vr4300_vaddr(cpu, va, 3, SALSA64_ACC_STORE, &pa)) {
			if (cpu->llbit)
				vr4300_write32(cpu, pa, (uint32_t)r[rt]);
			r[rt] = cpu->llbit;
		}
		return;
	case 0x39:                                                               /* SWC1 */
		if (cop1_usable(cpu) && vr4300_vaddr(cpu, va, 3, SALSA64_ACC_STORE, &pa))
			vr4300_write32(cpu, pa, vr4300_fpr_get32(cpu, rt));
		return;
	case 0x3A: op_cop_unusable_or_ri(cpu, 2); return;                       /* SWC2 */
	case 0x3C:                                                               /* SCD */
		REQUIRE64(cpu);
		if (vr4300_vaddr(cpu, va, 7, SALSA64_ACC_STORE, &pa)) {
			if (cpu->llbit)
				vr4300_write64(cpu, pa, r[rt]);
			r[rt] = cpu->llbit;
		}
		return;
	case 0x3D:                                                               /* SDC1 */
		if (cop1_usable(cpu) && vr4300_vaddr(cpu, va, 7, SALSA64_ACC_STORE, &pa))
			vr4300_write64(cpu, pa, vr4300_fpr_get64(cpu, rt));
		return;
	case 0x3E: op_cop_unusable_or_ri(cpu, 2); return;                       /* SDC2 */
	case 0x3F:                                                               /* SD */
		REQUIRE64(cpu);
		if (vr4300_vaddr(cpu, va, 7, SALSA64_ACC_STORE, &pa))
			vr4300_write64(cpu, pa, r[rt]);
		return;
	default:   vr4300_reserved(cpu); return;  /* COP3, 0x1C–0x1F, 0x33, 0x3B */
	}
}

/* ---- Bucle --------------------------------------------------------------- */

static inline void step(struct salsa64_cpu *cpu)
{
	cpu->delay_slot = cpu->branch;
	cpu->branch = 0;
	cpu->cur_pc = cpu->pc;
	cpu->cycles++;

	if (SALSA64_UNLIKELY(cpu->irq_pending)) {
		salsa64_vr4300_exception(cpu, SALSA64_EXC_INT, 0);
		return;
	}

	uint32_t pa;
	if (SALSA64_UNLIKELY(!vr4300_vaddr(cpu, cpu->pc, 3, SALSA64_ACC_FETCH, &pa)))
		return;
	uint32_t instr = vr4300_read32(cpu, pa);
	cpu->pc = cpu->next_pc;
	cpu->next_pc += 4;
	execute(cpu, instr);
	cpu->gpr[0] = 0;
}

void salsa64_cpu_step(struct salsa64_cpu *cpu)
{
	step(cpu);
	if (cpu->cycles >= cpu->sched->next)
		salsa64_sched_dispatch(cpu->sched, cpu->cycles);
}

void salsa64_cpu_run(struct salsa64_cpu *cpu)
{
	struct salsa64_sched *s = cpu->sched;
	cpu->running = 1;
	while (cpu->running) {
		while (cpu->cycles < s->next)
			step(cpu);
		salsa64_sched_dispatch(s, cpu->cycles);
	}
}
