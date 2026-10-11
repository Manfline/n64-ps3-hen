/*
 * Salsa64 — LLE del RSP: unidad escalar, COP0 y despacho. Ver lle.h.
 *
 * Modelo de saltos: `pc` es la instrucción que se va a ejecutar y
 * `next_pc` la siguiente; un salto tomado cambia `next_pc`, de modo que la
 * instrucción en `pc` (el delay slot) se ejecuta antes de llegar al
 * destino. Un salto en el delay slot de otro reemplaza el destino, como
 * en el hardware. Tras BREAK, SP_PC es la instrucción que seguía (si el
 * BREAK estaba en un delay slot, el destino del salto).
 *
 * El RSP no tiene multiplicador, ni LWL/LWR, ni excepciones: los opcodes
 * que no existen no hacen nada, salvo los de SPECIAL, que en el hardware
 * hacen rd = rs >> (rs & 31) (semántica observada en ares/nemu64-test).
 * Las cargas y almacenamientos escalares no alineados se hacen byte a
 * byte y la DMEM da la vuelta en 4 KB.
 */
#include "rsp/lle/lle.h"

#include <string.h>

#define OP_RS(op)   (((op) >> 21) & 31u)
#define OP_RT(op)   (((op) >> 16) & 31u)
#define OP_RD(op)   (((op) >> 11) & 31u)
#define OP_SA(op)   (((op) >> 6) & 31u)
#define OP_IMM(op)  ((int32_t)(int16_t)(op))
#define OP_UIMM(op) ((op) & 0xFFFFu)

static inline uint32_t fetch(const struct rsp_lle *s, uint32_t pc)
{
	const uint8_t *p = s->imem + (pc & 0xFFCu);
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint32_t rd8(const struct rsp_lle *s, uint32_t a)
{
	return s->dmem[a & 0xFFFu];
}

static inline uint32_t rd16(const struct rsp_lle *s, uint32_t a)
{
	return (rd8(s, a) << 8) | rd8(s, a + 1);
}

static inline uint32_t rd32(const struct rsp_lle *s, uint32_t a)
{
	return (rd16(s, a) << 16) | rd16(s, a + 2);
}

static inline void wr8(struct rsp_lle *s, uint32_t a, uint32_t v)
{
	s->dmem[a & 0xFFFu] = (uint8_t)v;
}

static inline void wr16(struct rsp_lle *s, uint32_t a, uint32_t v)
{
	wr8(s, a, v >> 8);
	wr8(s, a + 1, v);
}

static inline void wr32(struct rsp_lle *s, uint32_t a, uint32_t v)
{
	wr16(s, a, v >> 16);
	wr16(s, a + 2, v);
}

static inline void take(struct rsp_lle *s, uint32_t target)
{
	s->next_pc = target & 0xFFCu;
}

/* Tablas de recíproco y de raíz cuadrada inversa (512 entradas de 16 bits):
 * las fórmulas que reproducen las tablas del hardware (ares; nemu64-test
 * las verifica contra la consola). */
static void make_tables(struct rsp_lle *s)
{
	s->rcp_table[0] = 0xFFFF;
	for (unsigned i = 1; i < 512; i++) {
		uint64_t a = i + 512;
		uint64_t b = (1ull << 34) / a;
		s->rcp_table[i] = (uint16_t)((b + 1) >> 8);
	}
	for (unsigned i = 0; i < 512; i++) {
		uint64_t a = (i + 512) >> (i & 1);
		uint64_t b = 1ull << 17;
		/* El mayor b con a·(b+1)² < 2^44. */
		while (a * (b + 1) * (b + 1) < (1ull << 44))
			b++;
		s->rsq_table[i] = (uint16_t)(b >> 1);
	}
}

void lle_init(struct rsp_lle *s, uint8_t *dmem, uint8_t *imem)
{
	memset(s, 0, sizeof *s);
	s->dmem = dmem;
	s->imem = imem;
	make_tables(s);
	lle_reset(s);
}

void lle_reset(struct rsp_lle *s)
{
	memset(s->r, 0, sizeof s->r);
	memset(s->v, 0, sizeof s->v);
	memset(s->acc, 0, sizeof s->acc);
	s->vco = s->vcc = s->vce = 0;
	s->div_in = s->div_out = 0;
	s->div_dp = false;
	s->pc = 0;
	s->next_pc = 4;
	s->halt = true;
	s->instructions = 0;
	memset(s->c0_reads, 0, sizeof s->c0_reads);
	memset(s->c0_writes, 0, sizeof s->c0_writes);
}

void lle_set_pc(struct rsp_lle *s, uint32_t pc)
{
	s->pc = pc & 0xFFCu;
	s->next_pc = (s->pc + 4) & 0xFFCu;
}

static void special(struct rsp_lle *s, uint32_t op, uint32_t addr)
{
	uint32_t *r = s->r;
	uint32_t rs = r[OP_RS(op)], rt = r[OP_RT(op)];
	uint32_t rd = OP_RD(op);
	switch (op & 0x3F) {
	case 0x00: r[rd] = rt << OP_SA(op); break;                  /* SLL */
	case 0x02: r[rd] = rt >> OP_SA(op); break;                  /* SRL */
	case 0x03: r[rd] = (uint32_t)((int32_t)rt >> OP_SA(op)); break; /* SRA */
	case 0x04: r[rd] = rt << (rs & 31); break;                  /* SLLV */
	case 0x06: r[rd] = rt >> (rs & 31); break;                  /* SRLV */
	case 0x07: r[rd] = (uint32_t)((int32_t)rt >> (rs & 31)); break; /* SRAV */
	case 0x08: take(s, rs); break;                              /* JR */
	case 0x09: take(s, rs); r[rd] = (addr + 8) & 0xFFFu; break; /* JALR */
	case 0x0D: lle_io_break(s); break;                          /* BREAK */
	case 0x20: case 0x21: r[rd] = rs + rt; break;               /* ADD, ADDU */
	case 0x22: case 0x23: r[rd] = rs - rt; break;               /* SUB, SUBU */
	case 0x24: r[rd] = rs & rt; break;                          /* AND */
	case 0x25: r[rd] = rs | rt; break;                          /* OR */
	case 0x26: r[rd] = rs ^ rt; break;                          /* XOR */
	case 0x27: r[rd] = ~(rs | rt); break;                       /* NOR */
	case 0x2A: r[rd] = (int32_t)rs < (int32_t)rt; break;        /* SLT */
	case 0x2B: r[rd] = rs < rt; break;                          /* SLTU */
	default:   r[rd] = rs >> (rs & 31); break;                  /* no existen */
	}
}

static void regimm(struct rsp_lle *s, uint32_t op, uint32_t addr)
{
	int32_t rs = (int32_t)s->r[OP_RS(op)];
	uint32_t target = addr + 4 + ((uint32_t)OP_IMM(op) << 2);
	switch (OP_RT(op)) {
	case 0x00: if (rs < 0) take(s, target); break;                            /* BLTZ */
	case 0x01: if (rs >= 0) take(s, target); break;                           /* BGEZ */
	case 0x10: if (rs < 0) take(s, target); s->r[31] = (addr + 8) & 0xFFFu; break;  /* BLTZAL */
	case 0x11: if (rs >= 0) take(s, target); s->r[31] = (addr + 8) & 0xFFFu; break; /* BGEZAL */
	default: break;
	}
}

static void cop0(struct rsp_lle *s, uint32_t op)
{
	unsigned rd = OP_RD(op) & 15u;
	switch (OP_RS(op)) {
	case 0x00: {  /* MFC0 */
		uint32_t v = lle_io_read(s, rd);
		s->c0_reads[rd]++;
		if (OP_RT(op))
			s->r[OP_RT(op)] = v;
		break;
	}
	case 0x04:  /* MTC0 */
		s->c0_writes[rd]++;
		lle_io_write(s, rd, s->r[OP_RT(op)]);
		break;
	default:
		break;
	}
}

static void execute(struct rsp_lle *s, uint32_t op, uint32_t addr)
{
	uint32_t *r = s->r;
	uint32_t rs = r[OP_RS(op)], rt = OP_RT(op);
	uint32_t ea = rs + (uint32_t)OP_IMM(op);
	switch (op >> 26) {
	case 0x00: special(s, op, addr); break;
	case 0x01: regimm(s, op, addr); break;
	case 0x02: take(s, op << 2); break;                                      /* J */
	case 0x03: r[31] = (addr + 8) & 0xFFFu; take(s, op << 2); break;         /* JAL */
	case 0x04: if (rs == r[rt]) take(s, addr + 4 + ((uint32_t)OP_IMM(op) << 2)); break;  /* BEQ */
	case 0x05: if (rs != r[rt]) take(s, addr + 4 + ((uint32_t)OP_IMM(op) << 2)); break;  /* BNE */
	case 0x06: if ((int32_t)rs <= 0) take(s, addr + 4 + ((uint32_t)OP_IMM(op) << 2)); break; /* BLEZ */
	case 0x07: if ((int32_t)rs > 0) take(s, addr + 4 + ((uint32_t)OP_IMM(op) << 2)); break;  /* BGTZ */
	case 0x08: case 0x09: r[rt] = ea; break;                                 /* ADDI, ADDIU */
	case 0x0A: r[rt] = (int32_t)rs < OP_IMM(op); break;                      /* SLTI */
	case 0x0B: r[rt] = rs < (uint32_t)OP_IMM(op); break;                     /* SLTIU */
	case 0x0C: r[rt] = rs & OP_UIMM(op); break;                              /* ANDI */
	case 0x0D: r[rt] = rs | OP_UIMM(op); break;                              /* ORI */
	case 0x0E: r[rt] = rs ^ OP_UIMM(op); break;                              /* XORI */
	case 0x0F: r[rt] = OP_UIMM(op) << 16; break;                             /* LUI */
	case 0x10: cop0(s, op); break;
	case 0x12:                                                               /* COP2 */
		if (op & (1u << 25))
			lle_vu_op(s, op);
		else
			lle_vu_move(s, op);
		break;
	case 0x20: r[rt] = (uint32_t)(int8_t)rd8(s, ea); break;                  /* LB */
	case 0x21: r[rt] = (uint32_t)(int16_t)rd16(s, ea); break;                /* LH */
	case 0x23: case 0x27: r[rt] = rd32(s, ea); break;                        /* LW, LWU */
	case 0x24: r[rt] = rd8(s, ea); break;                                    /* LBU */
	case 0x25: r[rt] = rd16(s, ea); break;                                   /* LHU */
	case 0x28: wr8(s, ea, r[rt]); break;                                     /* SB */
	case 0x29: wr16(s, ea, r[rt]); break;                                    /* SH */
	case 0x2B: wr32(s, ea, r[rt]); break;                                    /* SW */
	case 0x32: lle_vu_load(s, op); break;                                    /* LWC2 */
	case 0x3A: lle_vu_store(s, op); break;                                   /* SWC2 */
	default: break;                                                          /* no existen */
	}
	r[0] = 0;
}

void lle_step(struct rsp_lle *s)
{
	uint32_t addr = s->pc;
	uint32_t op = fetch(s, addr);
	s->pc = s->next_pc;
	s->next_pc = (s->pc + 4) & 0xFFCu;
	execute(s, op, addr);
	s->instructions++;
}

uint32_t lle_run(struct rsp_lle *s, uint32_t max)
{
	uint32_t n = 0;
	while (n < max && !s->halt) {
		lle_step(s);
		n++;
		if ((n & (LLE_POLL_INTERVAL - 1)) == 0)
			lle_io_poll(s);
	}
	return n;
}
