/*
 * Salsa64 — recompilador: traducción de un bloque MIPS a PowerPC64.
 *
 * Escrito desde la documentación pública del VR4300 y de Power ISA; la
 * semántica de cada instrucción es la del intérprete (interp.c), que es la
 * referencia. Diseño en docs/rec-ppe.md.
 *
 * Registros del PPE dentro del código generado (ABI ELFv1 de PSL1GHT):
 *   r1  pila (marco de la entrada, rec.c)    r2  TOC (no se toca)
 *   r3–r12, r0  temporales y argumentos       r13 reservado (hilo)
 *   r14–r25  registros MIPS del bloque (GPR, HI = 32, LO = 33)
 *   r26 destino de JR/JALR (sobrevive al delay slot y a las llamadas)
 *   r27 sched->next   r28 code_flags   r29 cycles al entrar al bloque
 *   r30 RDRAM         r31 struct salsa64_cpu
 *   cr4 condición del salto (sobrevive al delay slot); cr0 y cr1 temporales.
 *
 * Forma de un bloque:
 *   entrada:  si cycles + n > next: salir (lo corre el intérprete hasta el
 *             evento); si hay COP1 y Status.CU1 = 0: salir igual
 *             carga de los registros que se leen antes de escribirse
 *   cuerpo:   una traducción por instrucción; los caminos lentos (E/S, TLB,
 *             código invalidado) van aparte, en la sección fría
 *   salida:   escritura de los registros modificados, cycles += n, y un
 *             enlace parcheable al bloque siguiente (o vuelta al cuerpo si
 *             el salto va al propio bloque).
 * Cada llamada a C deja antes cycles, cur_pc y delay_slot en la estructura
 * (como el intérprete) y después revisa redirect/irq_pending/rec_abort/mode
 * y el próximo evento; si algo cambió, escribe los registros y sale.
 */
#include <stddef.h>
#include <string.h>

#include "core/sched.h"
#include "cpu/fpu.h"
#include "cpu/ppc.h"
#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

/* ---- Registros y desplazamientos ------------------------------------------- */

enum {
	T_ADDR = 3,      /* dirección efectiva; primer argumento */
	T_HI = 7,        /* HI no asignado */
	T_V = 8,         /* destino no asignado */
	T_S1 = 9,        /* fuentes no asignadas */
	T_S2 = 10,
	T_X = 11,        /* auxiliares */
	T_Y = 12,
	R_CACHE0 = 14,
	R_TARGET = 26,
	R_NEXT = 27,
	R_FLAGS = 28,
	R_CYCLES = 29,
	R_RDRAM = 30,
	R_CPU = 31,
};
#define N_CACHE 12   /* sin registros fijos; con SALSA64_REC_OPT_PIN, 4 (r14–r17) */
#define CR_T  1   /* pruebas internas */
#define CR_BR 4   /* condición del salto */

#define REG_HI 32
#define REG_LO 33
#define BIT(r) (1ull << (r))

#define OFF(field) ((int32_t)offsetof(struct salsa64_cpu, field))

static int32_t off_reg(unsigned r)
{
	if (r == REG_HI)
		return OFF(hi);
	if (r == REG_LO)
		return OFF(lo);
	return OFF(gpr) + 8 * (int32_t)r;
}

/* ---- Decodificación --------------------------------------------------------- */

enum kind {
	K_NORMAL,
	K_BRANCH,     /* condicional, el delay slot siempre corre */
	K_BRANCH_L,   /* branch-likely: si no se toma, se anula el delay slot */
	K_JUMP,       /* J, JAL */
	K_JUMP_REG,   /* JR, JALR */
	K_END,        /* termina el bloque después de ella y vuelve a C */
};

enum how {
	H_NATIVE,
	H_EXEC,       /* la ejecuta el intérprete (salsa64_vr4300_exec_one) */
	H_EXC,        /* SYSCALL/BREAK: excepción segura */
};

struct dec {
	uint32_t w;
	uint8_t kind;
	uint8_t how;
	uint8_t cop1;     /* necesita Status.CU1 */
	uint64_t reads, writes;
};

static bool is_branch(const struct dec *d)
{
	return d->kind >= K_BRANCH && d->kind <= K_JUMP_REG;
}

static void decode(uint32_t i, struct dec *d)
{
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i);
	memset(d, 0, sizeof *d);
	d->w = i;
#define RD(x) (d->reads |= BIT(x))
#define WR(x) (d->writes |= BIT(x))
	switch (I_OP(i)) {
	case 0x00:
		switch (I_FUNCT(i)) {
		case 0x00: case 0x02: case 0x03:                       /* SLL SRL SRA */
		case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F:
			RD(rt); WR(rd); break;
		case 0x04: case 0x06: case 0x07: case 0x14: case 0x16: case 0x17:
		case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
		case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
			RD(rs); RD(rt); WR(rd); break;
		case 0x08: RD(rs); d->kind = K_JUMP_REG; break;                 /* JR */
		case 0x09: RD(rs); WR(rd); d->kind = K_JUMP_REG; break;         /* JALR */
		case 0x0C: case 0x0D: d->how = H_EXC; d->kind = K_END; break;   /* SYSCALL BREAK */
		case 0x0F: break;                                               /* SYNC */
		case 0x10: RD(REG_HI); WR(rd); break;
		case 0x11: RD(rs); WR(REG_HI); break;
		case 0x12: RD(REG_LO); WR(rd); break;
		case 0x13: RD(rs); WR(REG_LO); break;
		case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
			RD(rs); RD(rt); WR(REG_HI); WR(REG_LO); break;
		default:  /* traps y funciones inexistentes */
			RD(rs); RD(rt); d->how = H_EXEC; break;
		}
		break;
	case 0x01:
		switch (rt) {
		case 0x00: case 0x01: case 0x02: case 0x03:
			RD(rs); d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH; break;
		case 0x10: case 0x11: case 0x12: case 0x13:
			RD(rs); WR(31); d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH; break;
		default:  /* traps con inmediato y códigos inexistentes */
			RD(rs); d->how = H_EXEC; break;
		}
		break;
	case 0x02: d->kind = K_JUMP; break;
	case 0x03: WR(31); d->kind = K_JUMP; break;
	case 0x04: case 0x05: RD(rs); RD(rt); d->kind = K_BRANCH; break;
	case 0x06: case 0x07: RD(rs); d->kind = K_BRANCH; break;
	case 0x14: case 0x15: RD(rs); RD(rt); d->kind = K_BRANCH_L; break;
	case 0x16: case 0x17: RD(rs); d->kind = K_BRANCH_L; break;
	case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
	case 0x18: case 0x19:
		RD(rs); WR(rt); break;
	case 0x0F: WR(rt); break;
	case 0x10:
		if (rs <= 0x01) {                    /* MFC0 DMFC0: solo leen, el bloque sigue */
			WR(rt);
			if (rd == SALSA64_CP0_RANDOM)
				d->how = H_EXEC;
		} else {                             /* lo demás de COP0 vuelve a C después */
			RD(rt); WR(rt); d->how = H_EXEC; d->kind = K_END;
		}
		break;
	case 0x11:
		d->cop1 = 1;
		switch (rs) {
		case 0x08:
			if (!(rt & ~3u))
				d->kind = (rt & 2) ? K_BRANCH_L : K_BRANCH;
			else
				d->how = H_EXEC;
			break;
		case 0x00: case 0x01: case 0x02: WR(rt); break;         /* MFC1 DMFC1 CFC1 */
		case 0x04: case 0x05: case 0x06: RD(rt); break;         /* MTC1 DMTC1 CTC1 */
		case 0x10: case 0x11: {                                 /* S, D */
			unsigned f = I_FUNCT(i);
			bool cvt = (f == 0x20 && rs == FMT_D) || (f == 0x21 && rs == FMT_S) || f == 0x24 || f == 0x25;
			if (!(f <= 0x0F || cvt || f >= 0x30))
				d->how = H_EXEC;  /* CVT.S.S, CVT.D.D y códigos inexistentes: E */
			break;
		}
		case 0x14:                                              /* W: CVT.S.W CVT.D.W */
			if (I_FUNCT(i) != 0x20 && I_FUNCT(i) != 0x21)
				d->how = H_EXEC;
			break;
		default: d->how = H_EXEC; break;                        /* L, BC1 raros, inexistentes */
		}
		break;
	case 0x12: RD(rt); WR(rt); d->how = H_EXEC; break;          /* COP2 */
	case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27: case 0x37:
		RD(rs); WR(rt); break;                                  /* LB LH LW LBU LHU LWU LD */
	case 0x28: case 0x29: case 0x2B: case 0x3F:
		RD(rs); RD(rt); break;                                  /* SB SH SW SD */
	case 0x31: case 0x35: case 0x39: case 0x3D:
		RD(rs); d->cop1 = 1; break;                             /* LWC1 LDC1 SWC1 SDC1 */
	case 0x1A: case 0x1B: case 0x22: case 0x26:
		RD(rs); RD(rt); WR(rt); break;                          /* LDL LDR LWL LWR */
	case 0x38: case 0x3C:
		RD(rs); RD(rt); WR(rt); d->how = H_EXEC; break;         /* SC SCD */
	case 0x30: case 0x34:
		RD(rs); WR(rt); d->how = H_EXEC; break;                 /* LL LLD */
	case 0x2A: case 0x2C: case 0x2D: case 0x2E:
		RD(rs); RD(rt); break;                                  /* SWL SDL SDR SWR */
	case 0x2F: RD(rs); d->how = H_EXEC; break;                  /* CACHE */
	default: d->how = H_EXEC; break;                            /* COP2/COP3 y reservadas */
	}
#undef RD
#undef WR
	d->reads &= ~1ull;
	d->writes &= ~1ull;
}

/* SALSA64_REC_OPT_RARE_EXEC: lo que se tradujo el 2026-10-10 vuelve al
 * intérprete, para medir la diferencia con el mismo binario. */
static void decode_rare_exec(uint32_t i, struct dec *d)
{
	unsigned op = I_OP(i), fmt = I_RS(i), f = I_FUNCT(i);
	switch (op) {
	case 0x10:
		d->how = H_EXEC;
		d->kind = K_END;
		break;
	case 0x11:
		if (fmt == 0x02 || fmt == 0x06 || fmt == FMT_W ||
		    ((fmt & ~1u) == FMT_S && !(f <= 0x03 || (f >= 0x05 && f <= 0x07) || f >= 0x30)))
			d->how = H_EXEC;
		break;
	case 0x1A: case 0x1B: case 0x22: case 0x26: case 0x2A: case 0x2C: case 0x2D: case 0x2E:
		d->how = H_EXEC;
		break;
	}
}

/* ---- Ensamblador ------------------------------------------------------------- */

enum { SEC_HOT, SEC_COLD, SEC_ABS, SEC_NONE };

/* Salida pendiente de una instrucción (se emite al final, en la sección fría). */
struct pending_exit {
	uint16_t label;
	uint8_t k;
	uint8_t after;     /* la instrucción terminó: continuar en la siguiente */
	uint64_t mask;     /* registros a escribir */
};

struct ctx {
	struct salsa64_rec *rec;
	uint64_t pc;
	uint32_t pa;
	unsigned n;
	bool temp;
	bool has_branch;
	unsigned sec;
	struct dec d[SALSA64_REC_MAX_INSNS];
	uint8_t map[34];        /* registro MIPS → registro del PPE, 0 = en memoria */
	uint64_t cached;        /* en un registro del PPE (por bloque o fijo) */
	uint64_t pinned;        /* fijos entre bloques (SALSA64_REC_OPT_PIN) */
	uint64_t borrowed;      /* fijos que este bucle no usa: su registro del PPE queda para otros */
	uint64_t written;       /* asignados y modificados desde la entrada o el inicio del bucle */
	uint64_t written_start; /* written al empezar la instrucción en curso */
	uint8_t last_write[34]; /* última instrucción que escribe cada registro, + 1 (0 = ninguna) */
	uint64_t eager;         /* con su último valor, esperando para escribirse (OPT_EAGER) */
	uint8_t eager_due[34];  /* instrucción antes de la cual escribirlo */
	unsigned k;             /* instrucción en curso */
	bool in_ds;
	int body;               /* etiqueta del cuerpo (vuelta de los bucles) */
	int exit_before[SALSA64_REC_MAX_INSNS];
	int exit_after[SALSA64_REC_MAX_INSNS];
	struct pending_exit pending[2 * SALSA64_REC_MAX_INSNS];
	unsigned npending;
	/* Salto del bloque, para continuar después del delay slot. */
	uint8_t br_kind;
	uint8_t br_bo, br_bi;   /* condición de "tomado" en cr4 */
	uint64_t br_target, br_fall;
};

static void restore_borrowed(struct ctx *c);

static void E(struct ctx *c, uint32_t w)
{
	struct salsa64_rec *r = c->rec;
	if (r->nbuf[c->sec] >= SALSA64_REC_BUF_WORDS) {
		r->overflow = true;
		return;
	}
	r->buf[c->sec][r->nbuf[c->sec]++] = w;
}

static int new_label(struct ctx *c)
{
	struct salsa64_rec *r = c->rec;
	if (r->nlabels >= SALSA64_REC_MAX_LABELS) {
		r->overflow = true;
		return 0;
	}
	r->labels[r->nlabels].sec = SEC_NONE;
	r->labels[r->nlabels].pos = 0;
	return (int)r->nlabels++;
}

static void bind(struct ctx *c, int l)
{
	c->rec->labels[l].sec = (uint8_t)c->sec;
	c->rec->labels[l].pos = c->rec->nbuf[c->sec];
}

/* Etiqueta fija en la caché: `word` es relativa al inicio de la caché (puede
 * ser negativa para funciones de C). */
static int abs_label(struct ctx *c, int64_t word)
{
	int l = new_label(c);
	c->rec->labels[l].sec = SEC_ABS;
	c->rec->labels[l].pos = (uint32_t)(int32_t)word;
	return l;
}

static void fixup(struct ctx *c, int l, bool cond)
{
	struct salsa64_rec *r = c->rec;
	if (r->nfixups >= SALSA64_REC_MAX_FIXUPS) {
		r->overflow = true;
		return;
	}
	struct salsa64_rec_fixup *f = &r->fixups[r->nfixups++];
	f->at = r->nbuf[c->sec];
	f->label = (uint16_t)l;
	f->sec = (uint8_t)c->sec;
	f->cond = cond;
}

static void emit_b(struct ctx *c, int l, bool link)
{
	fixup(c, l, false);
	E(c, link ? ppc_bl(0) : ppc_b(0));
}

static void emit_bc(struct ctx *c, unsigned bo, unsigned bi, int l)
{
	fixup(c, l, true);
	E(c, ppc_bc(bo, bi, 0));
}

/* Carga una constante de 64 bits (2 instrucciones si es extensión de signo de
 * 32 bits, como las direcciones de KSEG0). */
static void li64(struct ctx *c, unsigned rt, uint64_t v)
{
	if ((int64_t)v == (int16_t)v) {
		E(c, ppc_li(rt, (int16_t)v));
	} else if ((int64_t)v == (int32_t)v) {
		E(c, ppc_lis(rt, (int16_t)(v >> 16)));
		if (v & 0xFFFF)
			E(c, ppc_ori(rt, rt, (uint32_t)v & 0xFFFF));
	} else {
		E(c, ppc_lis(rt, (int16_t)(v >> 48)));
		E(c, ppc_ori(rt, rt, (uint32_t)(v >> 32) & 0xFFFF));
		E(c, ppc_sldi(rt, rt, 32));
		E(c, ppc_oris(rt, rt, (uint32_t)(v >> 16) & 0xFFFF));
		E(c, ppc_ori(rt, rt, (uint32_t)v & 0xFFFF));
	}
}

/* Constante de 32 bits sin signo (argumentos `uint32_t`: la ABI los pasa
 * extendidos con ceros). */
static void li32u(struct ctx *c, unsigned rt, uint32_t v)
{
	E(c, ppc_li(rt, 0));
	if (v >> 16)
		E(c, ppc_oris(rt, rt, v >> 16));
	if (v & 0xFFFF)
		E(c, ppc_ori(rt, rt, v & 0xFFFF));
}

/* ---- Registros del bloque ------------------------------------------------------ */

/* Registro del PPE con el valor de `r`; si no está asignado, lo carga en `tmp`. */
static unsigned get(struct ctx *c, unsigned r, unsigned tmp)
{
	if (r == 0) {
		E(c, ppc_li(tmp, 0));
		return tmp;
	}
	if (c->map[r])
		return c->map[r];
	E(c, ppc_ld(tmp, off_reg(r), R_CPU));
	return tmp;
}

/* Registro donde calcular el nuevo valor de `r`. */
static unsigned dst(struct ctx *c, unsigned r, unsigned tmp)
{
	return (r && c->map[r]) ? c->map[r] : tmp;
}

/* El valor de `r` quedó en `reg` (el que dio dst).
 *
 * Con SALSA64_REC_OPT_EAGER, un registro asignado que recibe su último valor
 * del bloque se escribe en memoria dos instrucciones después, no a la
 * salida: si el bloque siguiente lo lee enseguida, el almacenamiento ya
 * salió de la cola (load-hit-store). No justo después: en el PPE, un
 * almacenamiento que espera el valor recién calculado detiene el pipeline
 * (medido: −37 % en un bucle de enteros). */
static void put(struct ctx *c, unsigned r, unsigned reg)
{
	if (r == 0 || (c->pinned & BIT(r)))
		return;  /* un registro fijo no se escribe hasta volver a C */
	if (!c->map[r]) {
		E(c, ppc_std(reg, off_reg(r), R_CPU));
		return;
	}
	c->written |= BIT(r);
	if ((c->rec->opts & SALSA64_REC_OPT_EAGER) && c->last_write[r] == c->k + 1) {
		c->eager |= BIT(r);
		c->eager_due[r] = (uint8_t)(c->k + 2);
	}
}

/* Escribe los registros de OPT_EAGER cuyo turno llegó, antes de la
 * instrucción k. */
static void flush_eager(struct ctx *c, unsigned k)
{
	for (unsigned r = 1; c->eager && r < 34; r++) {
		if ((c->eager & BIT(r)) && c->eager_due[r] <= k) {
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
			c->eager &= ~BIT(r);
			c->written &= ~BIT(r);
		}
	}
}

/* Escribe en cpu->gpr los registros de `mask` que están en el PPE. */
static void store_regs(struct ctx *c, uint64_t mask)
{
	mask &= c->cached;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
}

/* Al salir del bloque: los fijos siguen en sus registros. */
static void writeback(struct ctx *c, uint64_t mask)
{
	mask &= c->cached & ~c->pinned;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_std(c->map[r], off_reg(r), R_CPU));
}

static void reload(struct ctx *c, uint64_t mask)
{
	mask &= c->cached;
	for (unsigned r = 1; r < 34; r++)
		if (mask & BIT(r))
			E(c, ppc_ld(c->map[r], off_reg(r), R_CPU));
}

/* ---- Salidas ------------------------------------------------------------------ */

static uint64_t insn_pc(const struct ctx *c, unsigned k)
{
	return c->pc + 4ull * k;
}

/* r3 = dónde sigue la ejecución después de la instrucción k. */
static void emit_continue(struct ctx *c, unsigned k)
{
	if (!(c->has_branch && k == c->n - 1)) {
		li64(c, 3, insn_pc(c, k + 1));
		return;
	}
	switch (c->br_kind) {
	case K_JUMP_REG:
		E(c, ppc_mr(3, R_TARGET));
		return;
	case K_BRANCH: {
		int taken = new_label(c), join = new_label(c);
		emit_bc(c, c->br_bo, c->br_bi, taken);
		li64(c, 3, c->br_fall);
		emit_b(c, join, false);
		bind(c, taken);
		li64(c, 3, c->br_target);
		bind(c, join);
		return;
	}
	default:  /* J/JAL, o branch-likely (el delay slot solo corre si se tomó) */
		li64(c, 3, c->br_target);
		return;
	}
}

/* Salida desde la instrucción k: antes (excepción: no escribió nada) o
 * después de completarla. Se emite al final, en la sección fría. */
static int exit_label(struct ctx *c, unsigned k, bool after)
{
	int *slot = after ? &c->exit_after[k] : &c->exit_before[k];
	if (*slot >= 0)
		return *slot;
	struct pending_exit *p = &c->pending[c->npending++];
	p->label = (uint16_t)new_label(c);
	p->k = (uint8_t)k;
	p->after = after;
	p->mask = c->written_start | (after ? c->d[k].writes & c->cached : 0);
	*slot = p->label;
	return p->label;
}

static void emit_pending_exits(struct ctx *c)
{
	c->sec = SEC_COLD;
	for (unsigned e = 0; e < c->npending; e++) {
		const struct pending_exit *p = &c->pending[e];
		bind(c, p->label);
		writeback(c, p->mask);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)p->k + 1));
		if (p->after)
			emit_continue(c, p->k);
		else
			li64(c, 3, insn_pc(c, p->k));  /* no se usa: hay excepción (redirect) */
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_mid), false);
	}
}

/* Salida a una dirección fija, con los registros ya escritos y cycles al día.
 * Deja un enlace que rec.c parchea con un `b` al bloque destino:
 *   nop ; lis r3 ; ori r3 ; bl exit_link   (exit_link calcula el nop = LR - 16) */
static void emit_exit_to(struct ctx *c, uint64_t target)
{
	restore_borrowed(c);
	if (!c->temp && !c->rec->no_link && (int64_t)target == (int32_t)target) {
		E(c, PPC_NOP);
		E(c, ppc_lis(3, (int16_t)(target >> 16)));
		E(c, ppc_ori(3, 3, (uint32_t)target & 0xFFFF));
		emit_b(c, abs_label(c, c->rec->exit_link), true);
		return;
	}
	li64(c, 3, target);
	emit_b(c, abs_label(c, c->rec->exit_jump), false);
}

/* Salto tomado: vuelta al cuerpo si va al propio bloque (con la prueba del
 * próximo evento), o salida enlazada. Con no_link (--lockstep) los bucles
 * siguen: se compara al salir de ellos. */
static void emit_taken(struct ctx *c, uint64_t target)
{
	if (target != c->pc || c->temp) {
		emit_exit_to(c, target);
		return;
	}
	E(c, ppc_addi(0, R_CYCLES, (int32_t)c->n));
	E(c, ppc_cmpld(0, 0, R_NEXT));
	emit_bc(c, PPC_BO_FALSE_LIKELY, PPC_BI(0, PPC_GT), c->body);
	restore_borrowed(c);
	li64(c, 3, c->pc);
	emit_b(c, abs_label(c, c->rec->exit_mid), false);
}

/* ---- Llamadas a C ---------------------------------------------------------------- */

/* Lo que el intérprete deja en la estructura antes de cada instrucción. */
static void call_prep(struct ctx *c, unsigned k)
{
	E(c, ppc_addi(0, R_CYCLES, (int32_t)k + 1));
	E(c, ppc_std(0, OFF(cycles), R_CPU));
	li64(c, 0, insn_pc(c, k));
	E(c, ppc_std(0, OFF(cur_pc), R_CPU));
	E(c, ppc_li(0, c->in_ds));
	E(c, ppc_stb(0, OFF(delay_slot), R_CPU));
}

static void emit_call(struct ctx *c, uint64_t entry)
{
	struct salsa64_rec *r = c->rec;
	int64_t rel = (int64_t)entry - (int64_t)(uintptr_t)r->code;
	if (r->calls_near) {
		emit_b(c, abs_label(c, rel / 4), true);
		return;
	}
	li64(c, T_Y, entry);
	E(c, ppc_mtctr(T_Y));
	E(c, PPC_BCTRL);
}

/* Después de una llamada: salir si hubo excepción, interrupción, código
 * invalidado o cambio de modo, o si el bloque ya no cabe antes del próximo
 * evento. */
static void post_check(struct ctx *c, unsigned k)
{
	int out = exit_label(c, k, true);
	E(c, ppc_lwz(0, OFF(irq_pending), R_CPU));
	E(c, ppc_cmpwi(CR_T, 0, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), out);
	E(c, ppc_ld(T_X, OFF(sched), R_CPU));
	E(c, ppc_ld(R_NEXT, (int32_t)offsetof(struct salsa64_sched, next), T_X));
	E(c, ppc_addi(0, R_CYCLES, (int32_t)c->n));
	E(c, ppc_cmpld(CR_T, 0, R_NEXT));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), out);
}

/* Instrucción que corre el intérprete. */
static void emit_exec(struct ctx *c, unsigned k)
{
	const struct dec *d = &c->d[k];
	/* El intérprete lee y escribe cpu->gpr: antes, los que toca y están
	 * modificados (o fijos); después, los que escribe. */
	store_regs(c, (d->reads | d->writes) & (c->written | c->pinned));
	call_prep(c, k);
	E(c, ppc_mr(3, R_CPU));
	li32u(c, 4, d->w);
	emit_call(c, c->rec->h_exec);
	reload(c, d->writes);
	if (d->kind == K_END) {
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)k + 1));
		emit_continue(c, k);
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_mid), false);
		return;
	}
	post_check(c, k);
}

/* Camino lento de las instrucciones k..last traducidas en nativo: las corre
 * el intérprete, una por una, en la sección fría, y vuelve a `resume`. Se
 * llega antes de que el camino rápido escriba ningún registro, así que los
 * del PPE tienen los valores de antes de la instrucción k. */
static void emit_slow_exec(struct ctx *c, unsigned k, unsigned last, int slow, int resume)
{
	uint64_t touched = 0;
	for (unsigned j = k; j <= last; j++)
		touched |= c->d[j].reads | c->d[j].writes;
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, slow);
	store_regs(c, touched & (c->written_start | c->pinned));
	for (unsigned j = k; j <= last; j++) {
		call_prep(c, j);
		E(c, ppc_mr(3, R_CPU));
		li32u(c, 4, c->d[j].w);
		emit_call(c, c->rec->h_exec);
		reload(c, c->d[j].writes);
		post_check(c, j);
	}
	emit_b(c, resume, false);
	c->sec = sec;
}

/* Excepción segura (SYSCALL, BREAK) o condicional (desbordamiento, en la
 * sección fría). Siempre sale. */
static void emit_exception(struct ctx *c, unsigned k, unsigned code)
{
	call_prep(c, k);
	E(c, ppc_mr(3, R_CPU));
	E(c, ppc_li(4, (int32_t)code));
	emit_call(c, c->rec->h_exception);
	emit_b(c, exit_label(c, k, false), false);
}

/* Rama a un camino frío que lanza la excepción de desbordamiento. */
static void branch_overflow(struct ctx *c, unsigned bo, unsigned bi)
{
	int l = new_label(c);
	emit_bc(c, bo, bi, l);
	unsigned sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, l);
	emit_exception(c, c->k, SALSA64_EXC_OV);
	c->sec = sec;
}

/* ---- Memoria ------------------------------------------------------------------- */

/*
 * Dirección efectiva en r3 y prueba del camino rápido: KSEG0 o KSEG1 (el
 * bloque solo corre en modo kernel), dentro de la RDRAM y alineada. Con
 * RDRAM de 2^s bytes, (va >> s) vale -2^(31-s) en KSEG0 y -2^(31-s) + 2^(29-s)
 * en KSEG1; el `ori` junta los dos casos. Deja en r12 el desplazamiento
 * dentro de la RDRAM.
 */
static void emit_addr(struct ctx *c, unsigned rs, int16_t imm, unsigned align, int slow)
{
	struct salsa64_rec *r = c->rec;
	unsigned s = r->rdram_shift;
	int32_t k1 = 1 << (29 - s), region = -(1 << (31 - s)) | k1;
	unsigned base = get(c, rs, T_ADDR);
	E(c, ppc_addi(T_ADDR, base, imm));
	E(c, ppc_sradi(T_X, T_ADDR, s));
	E(c, ppc_ori(T_X, T_X, (uint32_t)k1));
	E(c, ppc_addi(T_X, T_X, -region));
	if (align) {
		E(c, ppc_clrldi(T_Y, T_ADDR, align == 1 ? 63 : align == 3 ? 62 : 61));
		E(c, ppc_or(T_X, T_X, T_Y));
	}
	E(c, ppc_cmpdi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	E(c, ppc_clrldi(T_Y, T_ADDR, 64 - s));
}

/* Registros de la FPU en cpu->fpr según el Status.FR con que se compila
 * (el host es big-endian: la mitad baja de un registro va en +4). */
static int32_t fpr32_off(const struct ctx *c, unsigned r)
{
	if (c->rec->fr || !(r & 1))
		return OFF(fpr) + 8 * (int32_t)r + 4;
	return OFF(fpr) + 8 * (int32_t)(r & ~1u);
}

static int32_t fpr64_off(const struct ctx *c, unsigned r)
{
	return OFF(fpr) + 8 * (int32_t)(c->rec->fr ? r : r & ~1u);
}

enum { DST_GPR, DST_FPR32, DST_FPR64 };

static void emit_load(struct ctx *c, unsigned k, unsigned kind, unsigned where)
{
	static const uint8_t align[] = { 0, 0, 1, 1, 3, 3, 7 };
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i);
	int slow = new_label(c), resume = new_label(c);

	emit_addr(c, I_RS(i), (int16_t)i, align[kind], slow);
	unsigned d = where == DST_GPR ? dst(c, rt, T_V) : T_V;
	switch (kind) {
	case SALSA64_REC_LB:  E(c, ppc_lbzx(d, R_RDRAM, T_Y)); E(c, ppc_extsb(d, d)); break;
	case SALSA64_REC_LBU: E(c, ppc_lbzx(d, R_RDRAM, T_Y)); break;
	case SALSA64_REC_LH:  E(c, ppc_lhzx(d, R_RDRAM, T_Y)); E(c, ppc_extsh(d, d)); break;
	case SALSA64_REC_LHU: E(c, ppc_lhzx(d, R_RDRAM, T_Y)); break;
	case SALSA64_REC_LW:  E(c, ppc_lwzx(d, R_RDRAM, T_Y)); E(c, ppc_extsw(d, d)); break;
	case SALSA64_REC_LWU: E(c, ppc_lwzx(d, R_RDRAM, T_Y)); break;
	default:              E(c, ppc_ldx(d, R_RDRAM, T_Y)); break;
	}
	bind(c, resume);
	/* El valor queda en d por los dos caminos. */
	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	{
		bind(c, slow);
		call_prep(c, k);
		E(c, ppc_mr(4, T_ADDR));
		E(c, ppc_mr(3, R_CPU));
		E(c, ppc_li(5, (int32_t)kind));
		emit_call(c, c->rec->h_load);
		E(c, ppc_lbz(0, OFF(redirect), R_CPU));
		E(c, ppc_cmpwi(CR_T, 0, 0));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), exit_label(c, k, false));
		E(c, ppc_mr(d, 3));
		/* Si sale antes de volver, el valor tiene que estar en su lugar. */
		if (where == DST_FPR32)
			E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
		else if (where == DST_FPR64)
			E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
		else if (rt && !c->map[rt])
			E(c, ppc_std(d, off_reg(rt), R_CPU));
		post_check(c, k);
		emit_b(c, resume, false);
	}
	c->sec = save_sec;
	if (where == DST_FPR32)
		E(c, ppc_stw(d, fpr32_off(c, rt), R_CPU));
	else if (where == DST_FPR64)
		E(c, ppc_std(d, fpr64_off(c, rt), R_CPU));
	else
		put(c, rt, d);
}

static void emit_store(struct ctx *c, unsigned k, unsigned kind, unsigned from)
{
	static const uint8_t align[] = { 0, 1, 3, 7 };
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), v;
	int slow = new_label(c), resume = new_label(c);

	if (from == DST_FPR32) {
		E(c, ppc_lwz(T_S1, fpr32_off(c, rt), R_CPU));
		v = T_S1;
	} else if (from == DST_FPR64) {
		E(c, ppc_ld(T_S1, fpr64_off(c, rt), R_CPU));
		v = T_S1;
	} else {
		v = get(c, rt, T_S1);
	}
	emit_addr(c, I_RS(i), (int16_t)i, align[kind], slow);
	/* Código recompilado en estos 128 bytes: por el camino lento. */
	E(c, ppc_srdi(T_X, T_Y, SALSA64_REC_FLAG_SHIFT));
	E(c, ppc_lbzx(T_X, R_FLAGS, T_X));
	E(c, ppc_cmpwi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	switch (kind) {
	case SALSA64_REC_SB: E(c, ppc_stbx(v, R_RDRAM, T_Y)); break;
	case SALSA64_REC_SH: E(c, ppc_sthx(v, R_RDRAM, T_Y)); break;
	case SALSA64_REC_SW: E(c, ppc_stwx(v, R_RDRAM, T_Y)); break;
	default:             E(c, ppc_stdx(v, R_RDRAM, T_Y)); break;
	}
	bind(c, resume);
	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, slow);
	call_prep(c, k);
	E(c, ppc_mr(5, v));
	E(c, ppc_mr(4, T_ADDR));
	E(c, ppc_mr(3, R_CPU));
	E(c, ppc_li(6, (int32_t)kind));
	emit_call(c, c->rec->h_store);
	post_check(c, k);
	emit_b(c, resume, false);
	c->sec = save_sec;
}

/* Almacenamiento en [T_Y, T_Y + bytes): por el camino lento si hay código
 * recompilado en esos 128 bytes (o en los siguientes, si los cruza). */
static void emit_code_check(struct ctx *c, unsigned bytes, int slow)
{
	E(c, ppc_srdi(T_X, T_Y, SALSA64_REC_FLAG_SHIFT));
	E(c, ppc_lbzx(T_X, R_FLAGS, T_X));
	if (bytes > 1) {
		E(c, ppc_addi(0, T_Y, (int32_t)bytes - 1));
		E(c, ppc_srdi(0, 0, SALSA64_REC_FLAG_SHIFT));
		E(c, ppc_lbzx(0, R_FLAGS, 0));
		E(c, ppc_or(T_X, T_X, 0));
	}
	E(c, ppc_cmpwi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
}

/*
 * LWL, LWR, LDL, LDR, SWL, SWR, SDL, SDR sueltos (interp.c: op_lwl_lwr y
 * compañía). El desplazamiento sale de la dirección, así que la mezcla usa
 * desplazamientos por registro (microcódigo en el PPE, pero mucho menos que
 * una llamada al intérprete). Los almacenamientos leen la palabra, la mezclan
 * y la escriben. Fuera de la RDRAM, o con código en esos 128 bytes, el
 * intérprete.
 */
static void emit_unaligned(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned op = I_OP(i), rt = I_RT(i);
	bool wide = op == 0x1A || op == 0x1B || op == 0x2C || op == 0x2D;
	bool left = op == 0x22 || op == 0x1A || op == 0x2A || op == 0x2C;
	bool store = op >= 0x28;
	int slow = new_label(c), resume = new_label(c);
	unsigned v = store ? get(c, rt, T_S1) : 0;

	emit_addr(c, I_RS(i), (int16_t)i, 0, slow);
	if (store)
		emit_code_check(c, 1, slow);
	/* T_X = desplazamiento en bits; T_HI = palabra (o doble palabra) alineada. */
	if (wide) {
		E(c, ppc_rlwinm(T_X, T_Y, 3, 26, 28));
		if (!left)
			E(c, ppc_subfic(T_X, T_X, 56));
		E(c, ppc_rldicr(T_Y, T_Y, 0, 60));
		E(c, ppc_ldx(T_HI, R_RDRAM, T_Y));
	} else {
		E(c, ppc_rlwinm(T_X, T_Y, 3, 27, 28));
		if (!left)
			E(c, ppc_subfic(T_X, T_X, 24));
		E(c, ppc_rlwinm(T_Y, T_Y, 0, 0, 29));
		E(c, ppc_lwzx(T_HI, R_RDRAM, T_Y));
	}
	/* r0 = máscara de los bytes que vienen de la memoria (o que se escriben). */
	E(c, ppc_li(0, -1));
	bool mask_left = store ? !left : left;  /* bytes altos: -1 << n; bajos: -1 >> n */
	if (wide)
		E(c, mask_left ? ppc_sld(0, 0, T_X) : ppc_srd(0, 0, T_X));
	else
		E(c, mask_left ? ppc_slw(0, 0, T_X) : ppc_srw(0, 0, T_X));

	if (store) {
		if (wide)
			E(c, left ? ppc_srd(T_V, v, T_X) : ppc_sld(T_V, v, T_X));
		else
			E(c, left ? ppc_srw(T_V, v, T_X) : ppc_slw(T_V, v, T_X));
		E(c, ppc_andc(T_HI, T_HI, 0));
		E(c, ppc_or(T_HI, T_HI, T_V));
		E(c, wide ? ppc_stdx(T_HI, R_RDRAM, T_Y) : ppc_stwx(T_HI, R_RDRAM, T_Y));
	} else {
		unsigned old = get(c, rt, T_S1), d = dst(c, rt, T_V);
		if (wide)
			E(c, left ? ppc_sld(T_HI, T_HI, T_X) : ppc_srd(T_HI, T_HI, T_X));
		else
			E(c, left ? ppc_slw(T_HI, T_HI, T_X) : ppc_srw(T_HI, T_HI, T_X));
		E(c, ppc_andc(d, old, 0));
		E(c, ppc_or(d, d, T_HI));
		if (!wide && left) {
			E(c, ppc_extsw(d, d));
		} else if (!wide) {
			/* LWR: solo con la palabra completa (n = 0) extiende el signo; si
			 * no, los 32 bits altos de rt quedan (andc no los tocó). */
			int keep = new_label(c);
			E(c, ppc_cmpwi(CR_T, T_X, 0));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), keep);
			E(c, ppc_extsw(d, d));
			bind(c, keep);
		}
		put(c, rt, d);
	}
	bind(c, resume);
	emit_slow_exec(c, k, k, slow, resume);
}

/* ¿LWL y LWR (o SWL y SWR), en cualquier orden, consecutivos sobre los
 * mismos 4 bytes? Juntos cargan (con signo) o escriben esos 4 bytes, sea
 * cual sea la alineación: es como el compilador hace un acceso no
 * alineado. Una carga cuya base es rt no: la primera cambia la base. */
static bool unaligned_pair(const struct ctx *c, unsigned k, unsigned end)
{
	if (k + 1 >= end || c->d[k].how != H_NATIVE || c->d[k + 1].how != H_NATIVE)
		return false;
	uint32_t a = c->d[k].w, b = c->d[k + 1].w;
	unsigned oa = I_OP(a), ob = I_OP(b);
	bool load = (oa == 0x22 && ob == 0x26) || (oa == 0x26 && ob == 0x22);
	bool store = (oa == 0x2A && ob == 0x2E) || (oa == 0x2E && ob == 0x2A);
	if (!(load || store) || I_RS(a) != I_RS(b) || I_RT(a) != I_RT(b))
		return false;
	if (load && (I_RT(a) == 0 || I_RT(a) == I_RS(a)))
		return false;
	bool left_first = oa == 0x22 || oa == 0x2A;
	int32_t l = (int16_t)(left_first ? a : b), r = (int16_t)(left_first ? b : a);
	return r == l + 3;
}

/* El par k, k + 1 de unaligned_pair, byte por byte: sin desplazamientos por
 * registro y, al escribir, sin leer la palabra (un par que escribe la
 * palabra que el anterior acaba de escribir chocaría: load-hit-store). */
static void emit_unaligned_pair(struct ctx *c, unsigned k)
{
	uint32_t a = c->d[k].w;
	unsigned rt = I_RT(a);
	bool store = I_OP(a) >= 0x28, left_first = I_OP(a) == 0x22 || I_OP(a) == 0x2A;
	int16_t imm = (int16_t)(left_first ? a : c->d[k + 1].w);  /* el de LWL/SWL: el primer byte */
	int slow = new_label(c), resume = new_label(c);

	flush_eager(c, k + 1);
	c->k = k;
	c->written_start = c->written;
	unsigned v = store ? get(c, rt, T_S1) : 0;

	emit_addr(c, I_RS(a), imm, 0, slow);
	/* El cuarto byte también dentro de la RDRAM. */
	E(c, ppc_addi(T_X, T_Y, 3));
	E(c, ppc_srdi(T_X, T_X, c->rec->rdram_shift));
	E(c, ppc_cmpdi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	if (store)
		emit_code_check(c, 4, slow);
	E(c, ppc_add(T_X, R_RDRAM, T_Y));
	if (store) {
		E(c, ppc_rlwinm(0, v, 8, 24, 31));
		E(c, ppc_stb(0, 0, T_X));
		E(c, ppc_rlwinm(0, v, 16, 24, 31));
		E(c, ppc_stb(0, 1, T_X));
		E(c, ppc_rlwinm(0, v, 24, 24, 31));
		E(c, ppc_stb(0, 2, T_X));
		E(c, ppc_stb(v, 3, T_X));
	} else {
		unsigned d = dst(c, rt, T_V);
		E(c, ppc_lbz(T_HI, 0, T_X));
		E(c, ppc_lbz(0, 1, T_X));
		E(c, ppc_rlwimi(0, T_HI, 8, 16, 23));     /* b0 b1 */
		E(c, ppc_lbz(T_HI, 2, T_X));
		E(c, ppc_rlwimi(T_HI, 0, 8, 8, 23));      /* b0 b1 b2 */
		E(c, ppc_lbz(0, 3, T_X));
		E(c, ppc_rlwimi(0, T_HI, 8, 0, 23));      /* b0 b1 b2 b3 */
		E(c, ppc_extsw(d, 0));
		c->k = k + 1;  /* la escritura de rt es la de la segunda */
		put(c, rt, d);
	}
	bind(c, resume);
	emit_slow_exec(c, k, k + 1, slow, resume);
}

/* ---- Enteros ----------------------------------------------------------------- */

/* d = (a < b) con signo: signo de (a - b) corregido por desbordamiento. */
static void emit_slt(struct ctx *c, unsigned d, unsigned a, unsigned b)
{
	E(c, ppc_subf(0, b, a));
	E(c, ppc_xor(T_X, a, b));
	E(c, ppc_xor(T_Y, 0, a));
	E(c, ppc_and(T_X, T_X, T_Y));
	E(c, ppc_xor(0, 0, T_X));
	E(c, ppc_srdi(d, 0, 63));
}

/* d = (a < b) sin signo: préstamo de a - b. */
static void emit_sltu(struct ctx *c, unsigned d, unsigned a, unsigned b)
{
	E(c, ppc_subf(0, b, a));
	E(c, ppc_eqv(T_X, a, b));
	E(c, ppc_and(T_X, T_X, 0));
	E(c, ppc_andc(T_Y, b, a));
	E(c, ppc_or(0, T_X, T_Y));
	E(c, ppc_srdi(d, 0, 63));
}

/* LO en r0 y HI en r12 al llegar a `join`. */
static void emit_hilo(struct ctx *c, bool sx)
{
	unsigned lo = dst(c, REG_LO, T_V), hi = dst(c, REG_HI, T_HI);
	if (sx) {
		E(c, ppc_extsw(lo, 0));
		E(c, ppc_extsw(hi, T_Y));
	} else {
		E(c, ppc_mr(lo, 0));
		E(c, ppc_mr(hi, T_Y));
	}
	put(c, REG_LO, lo);
	put(c, REG_HI, hi);
}

static void emit_div(struct ctx *c, unsigned funct, unsigned a, unsigned b)
{
	int zero = new_label(c), join = new_label(c), neg1 = -1;
	bool is64 = funct >= 0x1E, sgn = !(funct & 1);
	unsigned x = T_X, y = T_Y;
	if (is64) {
		x = a;
		E(c, ppc_mr(T_Y, b));
	} else if (sgn) {
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_extsw(T_Y, b));
	} else {
		E(c, ppc_clrldi(T_X, a, 32));
		E(c, ppc_clrldi(T_Y, b, 32));
	}
	E(c, ppc_cmpdi(CR_T, y, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), zero);
	if (is64 && sgn) {  /* INT64_MIN / -1 desborda en el PPE */
		neg1 = new_label(c);
		E(c, ppc_cmpdi(CR_T, y, -1));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), neg1);
	}
	E(c, sgn ? ppc_divd(0, x, y) : ppc_divdu(0, x, y));
	E(c, ppc_mulld(T_Y, 0, y));
	E(c, ppc_subf(T_Y, T_Y, x));
	bind(c, join);
	emit_hilo(c, !is64);

	unsigned save_sec = c->sec;
	c->sec = SEC_COLD;
	bind(c, zero);
	if (sgn) {  /* LO = dividendo < 0 ? 1 : -1 */
		E(c, ppc_sradi(T_Y, x, 63));
		E(c, ppc_add(T_Y, T_Y, T_Y));
		E(c, ppc_addi(T_Y, T_Y, 1));
		E(c, ppc_neg(0, T_Y));
	} else {
		E(c, ppc_li(0, -1));
	}
	E(c, ppc_mr(T_Y, x));  /* HI = dividendo */
	emit_b(c, join, false);
	if (neg1 >= 0) {
		bind(c, neg1);
		E(c, ppc_neg(0, x));
		E(c, ppc_li(T_Y, 0));
		emit_b(c, join, false);
	}
	c->sec = save_sec;
}

static void emit_special(struct ctx *c, uint32_t i)
{
	unsigned rs = I_RS(i), rt = I_RT(i), rd = I_RD(i), sa = I_SA(i), funct = I_FUNCT(i);
	unsigned a, b, d;

	switch (funct) {
	case 0x00:                                                        /* SLL */
		if (i == 0)
			return;
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(d, a, sa, 0, 31 - sa));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x02:                                                        /* SRL */
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(d, a, (32 - sa) & 31, sa, 31));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x03:                                                        /* SRA */
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		E(c, ppc_sradi(d, a, sa));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x04: case 0x06: case 0x07:                                  /* SLLV SRLV SRAV */
		a = get(c, rt, T_S1);
		b = get(c, rs, T_S2);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(T_X, b, 0, 27, 31));
		E(c, funct == 0x04 ? ppc_slw(d, a, T_X) : funct == 0x06 ? ppc_srw(d, a, T_X) : ppc_srad(d, a, T_X));
		E(c, ppc_extsw(d, d));
		put(c, rd, d);
		return;
	case 0x14: case 0x16: case 0x17:                                  /* DSLLV DSRLV DSRAV */
		a = get(c, rt, T_S1);
		b = get(c, rs, T_S2);
		d = dst(c, rd, T_V);
		E(c, ppc_rlwinm(T_X, b, 0, 26, 31));
		E(c, funct == 0x14 ? ppc_sld(d, a, T_X) : funct == 0x16 ? ppc_srd(d, a, T_X) : ppc_srad(d, a, T_X));
		put(c, rd, d);
		return;
	case 0x0F:                                                        /* SYNC */
		return;
	case 0x10: case 0x12:                                             /* MFHI MFLO */
		a = get(c, funct == 0x10 ? REG_HI : REG_LO, T_S1);
		d = dst(c, rd, T_V);
		if (d != a)
			E(c, ppc_mr(d, a));
		put(c, rd, d);
		return;
	case 0x11: case 0x13: {                                           /* MTHI MTLO */
		unsigned r = funct == 0x11 ? REG_HI : REG_LO;
		a = get(c, rs, T_S1);
		d = dst(c, r, T_V);
		if (d != a)
			E(c, ppc_mr(d, a));
		put(c, r, d);
		return;
	}
	case 0x18:                                                        /* MULT */
		/* rs con sus 64 bits por rt como entero de 35 bits (interp.c). */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_sldi(T_X, b, 29));
		E(c, ppc_sradi(T_X, T_X, 29));
		E(c, ppc_mulld(0, a, T_X));
		E(c, ppc_sradi(T_Y, 0, 32));
		emit_hilo(c, true);
		return;
	case 0x19:                                                        /* MULTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_clrldi(T_X, a, 32));
		E(c, ppc_clrldi(T_Y, b, 32));
		E(c, ppc_mulld(0, T_X, T_Y));
		E(c, ppc_sradi(T_Y, 0, 32));
		emit_hilo(c, true);
		return;
	case 0x1C: case 0x1D:                                             /* DMULT DMULTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_mulld(0, a, b));
		E(c, funct == 0x1C ? ppc_mulhd(T_Y, a, b) : ppc_mulhdu(T_Y, a, b));
		emit_hilo(c, false);
		return;
	case 0x1A: case 0x1B: case 0x1E: case 0x1F:                       /* DIV DIVU DDIV DDIVU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		emit_div(c, funct, a, b);
		return;
	case 0x20: case 0x22:                                             /* ADD SUB */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_extsw(T_Y, b));
		E(c, funct == 0x20 ? ppc_add(T_Y, T_X, T_Y) : ppc_subf(T_Y, T_Y, T_X));
		E(c, ppc_extsw(T_X, T_Y));
		E(c, ppc_cmpd(CR_T, T_X, T_Y));
		branch_overflow(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ));
		d = dst(c, rd, T_V);
		E(c, ppc_mr(d, T_X));
		put(c, rd, d);
		return;
	case 0x21: case 0x23:                                             /* ADDU SUBU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x21 ? ppc_add(T_X, a, b) : ppc_subf(T_X, b, a));
		E(c, ppc_extsw(d, T_X));
		put(c, rd, d);
		return;
	case 0x24: case 0x25: case 0x26: case 0x27:                       /* AND OR XOR NOR */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x24 ? ppc_and(d, a, b) : funct == 0x25 ? ppc_or(d, a, b)
		   : funct == 0x26 ? ppc_xor(d, a, b) : ppc_nor(d, a, b));
		put(c, rd, d);
		return;
	case 0x2A: case 0x2B:                                             /* SLT SLTU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		if (funct == 0x2A)
			emit_slt(c, d, a, b);
		else
			emit_sltu(c, d, a, b);
		put(c, rd, d);
		return;
	case 0x2C: case 0x2E:                                             /* DADD DSUB */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		if (funct == 0x2C) {
			E(c, ppc_add(0, a, b));
			E(c, ppc_xor(T_X, a, 0));
			E(c, ppc_xor(T_Y, b, 0));
		} else {
			E(c, ppc_subf(0, b, a));
			E(c, ppc_xor(T_X, a, b));
			E(c, ppc_xor(T_Y, a, 0));
		}
		E(c, ppc_and(T_X, T_X, T_Y));
		E(c, ppc_cmpdi(CR_T, T_X, 0));
		branch_overflow(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_LT));
		d = dst(c, rd, T_V);
		E(c, ppc_mr(d, 0));
		put(c, rd, d);
		return;
	case 0x2D: case 0x2F:                                             /* DADDU DSUBU */
		a = get(c, rs, T_S1);
		b = get(c, rt, T_S2);
		d = dst(c, rd, T_V);
		E(c, funct == 0x2D ? ppc_add(d, a, b) : ppc_subf(d, b, a));
		put(c, rd, d);
		return;
	case 0x38: case 0x3A: case 0x3B: case 0x3C: case 0x3E: case 0x3F: {  /* DSLL DSRL DSRA (+32) */
		unsigned n = sa + (funct >= 0x3C ? 32 : 0);
		a = get(c, rt, T_S1);
		d = dst(c, rd, T_V);
		switch (funct & 3) {
		case 0:  E(c, ppc_sldi(d, a, n)); break;
		case 2:  E(c, ppc_srdi(d, a, n)); break;
		default: E(c, ppc_sradi(d, a, n)); break;
		}
		put(c, rd, d);
		return;
	}
	default:
		return;  /* decode() manda lo demás al intérprete */
	}
}

static void emit_imm(struct ctx *c, uint32_t i)
{
	unsigned op = I_OP(i), rs = I_RS(i), rt = I_RT(i), a, d;
	int32_t simm = (int16_t)i;
	uint32_t uimm = i & 0xFFFF;

	switch (op) {
	case 0x08:                                                        /* ADDI */
		a = get(c, rs, T_S1);
		E(c, ppc_extsw(T_X, a));
		E(c, ppc_addi(T_Y, T_X, simm));
		E(c, ppc_extsw(T_X, T_Y));
		E(c, ppc_cmpd(CR_T, T_X, T_Y));
		branch_overflow(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ));
		d = dst(c, rt, T_V);
		E(c, ppc_mr(d, T_X));
		put(c, rt, d);
		return;
	case 0x09:                                                        /* ADDIU */
		d = dst(c, rt, T_V);
		if (rs == 0) {
			E(c, ppc_li(d, simm));
		} else {
			a = get(c, rs, T_S1);
			E(c, ppc_addi(T_X, a, simm));
			E(c, ppc_extsw(d, T_X));
		}
		put(c, rt, d);
		return;
	case 0x0A: case 0x0B:                                             /* SLTI SLTIU */
		a = get(c, rs, T_S1);
		E(c, ppc_li(T_S2, simm));
		d = dst(c, rt, T_V);
		if (op == 0x0A)
			emit_slt(c, d, a, T_S2);
		else
			emit_sltu(c, d, a, T_S2);
		put(c, rt, d);
		return;
	case 0x0C: case 0x0D: case 0x0E:                                  /* ANDI ORI XORI */
		a = get(c, rs, T_S1);
		d = dst(c, rt, T_V);
		E(c, op == 0x0C ? ppc_andi_(d, a, uimm) : op == 0x0D ? ppc_ori(d, a, uimm) : ppc_xori(d, a, uimm));
		put(c, rt, d);
		return;
	case 0x0F:                                                        /* LUI */
		d = dst(c, rt, T_V);
		E(c, ppc_lis(d, simm));
		put(c, rt, d);
		return;
	case 0x18:                                                        /* DADDI */
		a = get(c, rs, T_S1);
		E(c, ppc_addi(T_Y, a, simm));
		E(c, ppc_li(T_X, simm));
		E(c, ppc_xor(T_X, T_X, T_Y));
		E(c, ppc_xor(0, a, T_Y));
		E(c, ppc_and(T_X, T_X, 0));
		E(c, ppc_cmpdi(CR_T, T_X, 0));
		branch_overflow(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_LT));
		d = dst(c, rt, T_V);
		E(c, ppc_mr(d, T_Y));
		put(c, rt, d);
		return;
	case 0x19:                                                        /* DADDIU */
		a = get(c, rs, T_S1);
		d = dst(c, rt, T_V);
		E(c, ppc_addi(d, a, simm));
		put(c, rt, d);
		return;
	default:
		return;
	}
}

static void emit_cop1(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), fs = I_FS(i), d, v;
	switch (I_RS(i)) {
	case 0x00:                                                        /* MFC1 */
		E(c, ppc_lwz(T_X, fpr32_off(c, fs), R_CPU));
		d = dst(c, rt, T_V);
		E(c, ppc_extsw(d, T_X));
		put(c, rt, d);
		return;
	case 0x01:                                                        /* DMFC1 */
		d = dst(c, rt, T_V);
		E(c, ppc_ld(d, fpr64_off(c, fs), R_CPU));
		put(c, rt, d);
		return;
	case 0x04:                                                        /* MTC1 */
		v = get(c, rt, T_S1);
		E(c, ppc_stw(v, fpr32_off(c, fs), R_CPU));
		return;
	case 0x05:                                                        /* DMTC1 */
		v = get(c, rt, T_S1);
		E(c, ppc_std(v, fpr64_off(c, fs), R_CPU));
		return;
	case 0x02:                                                        /* CFC1 */
		d = dst(c, rt, T_V);
		if (fs == 31) {
			E(c, ppc_lwz(T_X, OFF(fcr31), R_CPU));
			E(c, ppc_extsw(d, T_X));
		} else {
			E(c, ppc_li(d, fs == 0 ? SALSA64_FCR0_VALUE : 0));
		}
		put(c, rt, d);
		return;
	case 0x06: {                                                      /* CTC1 */
		if (fs != 31)
			return;  /* solo FCR31 se escribe */
		int slow = new_label(c), resume = new_label(c);
		v = get(c, rt, T_S1);
		E(c, ppc_lis(T_X, 0x0183));
		E(c, ppc_ori(T_X, T_X, 0xFFFF));
		E(c, ppc_and(T_X, v, T_X));                  /* bits escribibles (cop1.c) */
		/* Una causa con su excepción habilitada (E siempre lo está) la
		 * lanza: eso lo hace el intérprete. */
		E(c, ppc_rlwinm(T_Y, T_X, 25, 27, 31));      /* habilitaciones */
		E(c, ppc_ori(T_Y, T_Y, 0x20));
		E(c, ppc_rlwinm(0, T_X, 20, 26, 31));        /* causas */
		E(c, ppc_and(0, 0, T_Y));
		E(c, ppc_cmpwi(CR_T, 0, 0));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		E(c, ppc_stw(T_X, OFF(fcr31), R_CPU));
		bind(c, resume);
		emit_slow_exec(c, k, k, slow, resume);
		return;
	}
	default:
		return;
	}
}

/* ---- COP0 ---------------------------------------------------------------------- */

/* Campo de struct salsa64_cpu de cada registro de COP0, como lo lee
 * salsa64_cpu_cop0_read (Count, Random y PRId aparte). */
static const struct { int16_t off; uint8_t is64; } cop0_fields[32] = {
#define F32(f) { (int16_t)offsetof(struct salsa64_cpu, f), 0 }
#define F64(f) { (int16_t)offsetof(struct salsa64_cpu, f), 1 }
	[SALSA64_CP0_INDEX] = F32(index),       [SALSA64_CP0_ENTRYLO0] = F32(entry_lo0),
	[SALSA64_CP0_ENTRYLO1] = F32(entry_lo1), [SALSA64_CP0_CONTEXT] = F64(context),
	[SALSA64_CP0_PAGEMASK] = F32(page_mask), [SALSA64_CP0_WIRED] = F32(wired),
	[7] = F64(cop0_latch),                  [SALSA64_CP0_BADVADDR] = F64(bad_vaddr),
	[SALSA64_CP0_ENTRYHI] = F64(entry_hi),  [SALSA64_CP0_COMPARE] = F32(compare),
	[SALSA64_CP0_STATUS] = F32(status),     [SALSA64_CP0_CAUSE] = F32(cause),
	[SALSA64_CP0_EPC] = F64(epc),           [SALSA64_CP0_CONFIG] = F32(config),
	[SALSA64_CP0_LLADDR] = F32(lladdr),     [SALSA64_CP0_WATCHLO] = F32(watch_lo),
	[SALSA64_CP0_WATCHHI] = F32(watch_hi),  [SALSA64_CP0_XCONTEXT] = F64(xcontext),
	[21] = F64(cop0_latch), [22] = F64(cop0_latch), [23] = F64(cop0_latch),
	[24] = F64(cop0_latch), [25] = F64(cop0_latch),
	[SALSA64_CP0_PARITYERROR] = F32(parity_error), [SALSA64_CP0_CACHEERROR] = F32(cache_error),
	[SALSA64_CP0_TAGLO] = F32(tag_lo),      [SALSA64_CP0_TAGHI] = F32(tag_hi),
	[SALSA64_CP0_ERROREPC] = F64(error_epc), [31] = F64(cop0_latch),
#undef F32
#undef F64
};

/* MFC0 y DMFC0 (Random lo corre el intérprete). En modo kernel, el único
 * en que corre un bloque, no hay excepción posible. Count es
 * (cycles >> 1) + count_offset con cycles = r29 + k + 1, lo que el
 * intérprete tiene al ejecutar la instrucción k. */
static void emit_cop0(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), rd = I_RD(i), d = dst(c, rt, T_V);
	bool wide = I_RS(i) == 0x01;
	int32_t off = cop0_fields[rd].off;

	if (rd == SALSA64_CP0_COUNT) {
		E(c, ppc_addi(0, R_CYCLES, (int32_t)k + 1));
		E(c, ppc_srdi(0, 0, 1));
		E(c, ppc_lwz(T_X, OFF(count_offset), R_CPU));
		E(c, ppc_add(T_X, 0, T_X));
		E(c, wide ? ppc_clrldi(d, T_X, 32) : ppc_extsw(d, T_X));
	} else if (rd == SALSA64_CP0_PRID) {
		E(c, ppc_li(d, SALSA64_PRID_VALUE));
	} else if (!cop0_fields[rd].is64) {
		E(c, ppc_lwz(d, off, R_CPU));
		if (!wide)
			E(c, ppc_extsw(d, d));
	} else if (wide) {
		E(c, ppc_ld(d, off, R_CPU));
	} else {
		E(c, ppc_lwz(d, off + 4, R_CPU));  /* mitad baja (el host es big-endian) */
		E(c, ppc_extsw(d, d));
	}
	put(c, rt, d);
}

/* ---- FPU ----------------------------------------------------------------------- */

/*
 * Lo mismo que el camino rápido del intérprete (fpu.h: fpu_fast_arith,
 * fpu_fast_abs_neg, fpu_fast_compare, MOV), con las mismas operaciones de
 * doble precisión en el mismo orden, así que da los mismos bits y las mismas
 * causas en FCR31. Lo que ese camino no cubre (redondeo dirigido, I
 * habilitada, operandos fuera de la franja segura, NaN, desnormales, suma
 * con exponentes a más de 28 de distancia en S) salta a la sección fría,
 * donde lo hace el intérprete.
 *
 * Operandos como en cop1.c: fs con el bit bajo ignorado si FR = 0; ft y fd,
 * el registro tal cual. En S, el valor está en la mitad baja (+4).
 */

/* r6 = FCR31 leído antes; r5 = 1 si el resultado es inexacto (o NULL: no).
 * Borra las causas, pone I si hace falta y escribe solo si cambió. */
static void emit_fcr31_cause(struct ctx *c, bool inexact_in_r5)
{
	int same = new_label(c);
	E(c, ppc_rlwinm(7, 6, 0, 20, 13));  /* causas (bits 12–17) en 0 */
	if (inexact_in_r5) {
		E(c, ppc_neg(0, 5));
		E(c, ppc_andi_(0, 0, 0x1004));  /* causa I y bandera I */
		E(c, ppc_or(7, 7, 0));
	}
	E(c, ppc_cmpw(CR_T, 7, 6));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), same);
	E(c, ppc_stw(7, OFF(fcr31), R_CPU));
	bind(c, same);
}

/* Con los bits de un operando S o D en `bits`: ¿exponente sesgado (con el
 * signo encima si `with_sign`: así un negativo queda fuera) en
 * [lo, lo + width], o magnitud cero? Si no, salta a `slow`. Deja el campo
 * en `e`. */
static void emit_fpu_range(struct ctx *c, bool is_s, unsigned bits, unsigned e, bool with_sign,
			   int32_t lo, uint32_t width, int slow)
{
	int ok = new_label(c);
	if (is_s)
		E(c, ppc_rlwinm(e, bits, 9, with_sign ? 23 : 24, 31));
	else
		E(c, ppc_rldicl(e, bits, 12, with_sign ? 52 : 53));
	E(c, ppc_addi(0, e, -lo));
	E(c, ppc_cmplwi(CR_T, 0, width));
	emit_bc(c, PPC_BO_FALSE_LIKELY, PPC_BI(CR_T, PPC_GT), ok);
	E(c, is_s ? ppc_rlwinm(0, bits, 0, 1, 31) : ppc_clrldi(0, bits, 1));
	E(c, ppc_cmpdi(CR_T, 0, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	bind(c, ok);
}

/* Franja segura del exponente (fpu_fast_band). */
static void emit_fpu_band(struct ctx *c, bool is_s, unsigned bits, unsigned e, int slow)
{
	emit_fpu_range(c, is_s, bits, e, false, is_s ? 67 : 583, is_s ? 120 : 880, slow);
}

/* Lee FCR31 en r6 y salta a `slow` si tiene alguno de los bits de `mask`
 * (0x03: redondeo dirigido; 0x80: I habilitada). */
static void emit_fcr31_gate(struct ctx *c, uint32_t mask, int slow)
{
	E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
	E(c, ppc_andi_(0, 6, mask));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(0, PPC_EQ), slow);
}

/* Resultado S desde el registro de punto flotante `fr`: los 32 bits altos de
 * fd en 0. */
static void emit_store_s(struct ctx *c, unsigned fr, int32_t od)
{
	E(c, ppc_stfs(fr, od + 4, R_CPU));
	E(c, ppc_li(0, 0));
	E(c, ppc_stw(0, od, R_CPU));
}

/*
 * SQRT (cop1.c, fpu_arith): positivo normal o ±0, al más cercano e I
 * deshabilitada; lo demás (V, E, infinito) lo hace el intérprete. fsqrt da
 * la raíz en double redondeada al más cercano; I sale como en el camino
 * completo: en S, si el float difiere del double o, si no, del residuo
 * a - r·r (exacto con fnmsub); en D, del residuo.
 */
static void emit_fpu_sqrt(struct ctx *c, bool is_s, int32_t oa, int32_t od, int slow)
{
	int done = new_label(c);
	emit_fcr31_gate(c, 0x83, slow);
	E(c, is_s ? ppc_lwz(9, oa, R_CPU) : ppc_ld(9, oa, R_CPU));
	emit_fpu_range(c, is_s, 9, 12, true, 1, is_s ? 253 : 2045, slow);
	E(c, is_s ? ppc_lfs(1, oa, R_CPU) : ppc_lfd(1, oa, R_CPU));
	E(c, ppc_fsqrt(0, 1));
	E(c, ppc_li(5, 1));
	if (is_s) {
		E(c, ppc_frsp(3, 0));
		E(c, ppc_fcmpu(CR_T, 3, 0));
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	}
	E(c, ppc_fnmsub(4, 0, 0, 1));  /* a - r·r */
	E(c, ppc_fsub(5, 1, 1));
	E(c, ppc_fcmpu(CR_T, 4, 5));
	emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	E(c, ppc_li(5, 0));
	bind(c, done);
	if (is_s)
		emit_store_s(c, 3, od);
	else
		E(c, ppc_stfd(0, od, R_CPU));
	emit_fcr31_cause(c, true);
}

/* CVT.D.S y CVT.S.D (cop1.c, fpu_float). */
static void emit_fpu_cvt(struct ctx *c, bool is_s, int32_t oa, int32_t od, int slow)
{
	if (is_s) {
		/* CVT.D.S de un normal o ±0: exacto, sin causa (lfs ya convierte). */
		E(c, ppc_lwz(9, oa, R_CPU));
		emit_fpu_range(c, true, 9, 12, false, 1, 253, slow);
		E(c, ppc_lfs(1, oa, R_CPU));
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_stfd(1, od, R_CPU));
		emit_fcr31_cause(c, false);
		return;
	}
	/* CVT.S.D con 2^-126 ≤ |x| < 2^127 (o ±0): ni desborda ni queda
	 * diminuto; frsp redondea al más cercano y el inexacto es frsp ≠ x. */
	int done = new_label(c);
	emit_fcr31_gate(c, 0x83, slow);
	E(c, ppc_ld(9, oa, R_CPU));
	emit_fpu_range(c, false, 9, 12, false, 897, 252, slow);
	E(c, ppc_lfd(1, oa, R_CPU));
	E(c, ppc_frsp(3, 1));
	E(c, ppc_li(5, 1));
	E(c, ppc_fcmpu(CR_T, 3, 1));
	emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	E(c, ppc_li(5, 0));
	bind(c, done);
	emit_store_s(c, 3, od);
	emit_fcr31_cause(c, true);
}

/*
 * CVT.S.W y CVT.D.W (cop1.c, fpu_int) en enteros: el entero está en
 * cpu->fpr, y pasarlo a un registro de punto flotante obliga a escribirlo y
 * releerlo (load-hit-store). |x| ≤ 2^31 se normaliza con el 1 más alto en el
 * bit 31; en D es exacto, en S se redondea a 24 bits al más cercano par
 * (sumando 0x7F más el bit que queda abajo) y es inexacto si sobran bits.
 * CVT.S.W necesita redondeo al más cercano e I deshabilitada; CVT.D.W nunca
 * tiene causa.
 */
static void emit_fpu_from_w(struct ctx *c, bool to_s, int32_t oa, int32_t od, int slow)
{
	int store = new_label(c);
	if (to_s)
		emit_fcr31_gate(c, 0x83, slow);
	else
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
	E(c, ppc_lwz(9, oa, R_CPU));
	E(c, ppc_li(11, 0));
	E(c, ppc_li(5, 0));
	E(c, ppc_extsw(9, 9));
	E(c, ppc_cmpdi(CR_T, 9, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), store);  /* +0 */
	E(c, ppc_sradi(10, 9, 63));
	E(c, ppc_xor(11, 9, 10));
	E(c, ppc_subf(11, 10, 11));      /* |x| */
	E(c, ppc_cntlzw(12, 11));
	E(c, ppc_slw(11, 11, 12));       /* el 1 más alto en el bit 31 */
	if (to_s) {
		E(c, ppc_andi_(5, 11, 0xFF));   /* lo que no cabe en 24 bits */
		E(c, ppc_rlwinm(0, 11, 24, 31, 31));
		E(c, ppc_add(11, 11, 0));
		E(c, ppc_addi(11, 11, 0x7F));
		E(c, ppc_srdi(11, 11, 8));      /* mantisa con el 1 implícito (o 2^24 si llevó) */
		E(c, ppc_subfic(12, 12, 157));  /* exponente - 1: el 1 implícito suma el que falta */
		E(c, ppc_sldi(12, 12, 23));
		E(c, ppc_add(11, 11, 12));
		E(c, ppc_rlwinm(0, 9, 0, 0, 0));
		E(c, ppc_or(11, 11, 0));
		E(c, ppc_neg(5, 5));
		E(c, ppc_srdi(5, 5, 63));       /* 1 si inexacto */
	} else {
		E(c, ppc_sldi(11, 11, 21));
		E(c, ppc_subfic(12, 12, 1053));
		E(c, ppc_sldi(12, 12, 52));
		E(c, ppc_add(11, 11, 12));
		E(c, ppc_rldicr(0, 9, 0, 0));
		E(c, ppc_or(11, 11, 0));
	}
	bind(c, store);
	E(c, ppc_std(11, od, R_CPU));  /* en S, los 32 bits altos en 0 */
	emit_fcr31_cause(c, to_s);
}

static const double k_one = 1.0;

/*
 * ROUND, TRUNC, CEIL y FLOOR a W y L, y CVT.W y CVT.L con redondeo al más
 * cercano (cop1.c, fpu_to_int), con I deshabilitada. Solo exponentes con los
 * que ningún modo se sale del rango (W: |x| < 2^31 en S, < 2^30 en D; L:
 * < 2^53), o ±0. fctid usa el modo del FPSCR, que nunca cambia (al más
 * cercano par, como fpu_round); fctidz trunca; CEIL y FLOOR corrigen el
 * truncado en 1 (exacto). Inexacto: el entero vuelto a double difiere de x.
 */
static void emit_fpu_to_int(struct ctx *c, bool is_s, unsigned funct, int32_t oa, int32_t od, int slow)
{
	bool cvt = funct >= 0x24, to64 = cvt ? funct == 0x25 : funct < 0x0C;
	unsigned mode = cvt ? 0 : funct & 3;  /* 0 más cercano, 1 a cero, 2 a +inf, 3 a -inf */
	uint32_t emax = (is_s ? 127 : 1023) + (to64 ? 52 : is_s ? 30 : 29);
	int done = new_label(c);

	emit_fcr31_gate(c, cvt ? 0x83 : 0x80, slow);
	E(c, is_s ? ppc_lwz(9, oa, R_CPU) : ppc_ld(9, oa, R_CPU));
	emit_fpu_range(c, is_s, 9, 12, false, 1, emax - 1, slow);
	E(c, is_s ? ppc_lfs(1, oa, R_CPU) : ppc_lfd(1, oa, R_CPU));
	E(c, mode == 0 ? ppc_fctid(0, 1) : ppc_fctidz(0, 1));
	E(c, ppc_fcfid(2, 0));
	E(c, ppc_li(5, 0));
	E(c, ppc_fcmpu(CR_T, 2, 1));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), done);
	E(c, ppc_li(5, 1));
	if (mode >= 2) {
		/* CEIL: truncado < x, +1; FLOOR: truncado > x, -1. */
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, mode == 2 ? PPC_LT : PPC_GT), done);
		li64(c, T_Y, (uintptr_t)&k_one);
		E(c, ppc_lfd(3, 0, T_Y));
		E(c, mode == 2 ? ppc_fadd(2, 2, 3) : ppc_fsub(2, 2, 3));
		E(c, ppc_fctidz(0, 2));
	}
	bind(c, done);
	E(c, ppc_stfd(0, od, R_CPU));
	if (!to64) {  /* W: los 32 bits altos en 0 */
		E(c, ppc_li(0, 0));
		E(c, ppc_stw(0, od, R_CPU));
	}
	emit_fcr31_cause(c, true);
}

static void emit_fpu(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned fmt = I_FMT(i), funct = I_FUNCT(i), fs = I_FS(i), ft = I_FT(i), fd = I_FD(i);
	bool is_s = fmt == FMT_S;
	unsigned fsr = c->rec->fr ? fs : fs & ~1u;
	int32_t lo = fmt == FMT_D ? 0 : 4;  /* S y W: la mitad baja */
	int32_t oa = OFF(fpr) + 8 * (int32_t)fsr + lo, ob = OFF(fpr) + 8 * (int32_t)ft + lo;
	int32_t od = OFF(fpr) + 8 * (int32_t)fd;

	if (funct == 0x06 && fmt != FMT_W) {  /* MOV: los 64 bits, sin tocar FCR31 */
		E(c, ppc_ld(0, OFF(fpr) + 8 * (int32_t)fsr, R_CPU));
		E(c, ppc_std(0, od, R_CPU));
		return;
	}

	int slow = new_label(c), resume = new_label(c);
	if (fmt == FMT_W) {
		emit_fpu_from_w(c, funct == 0x20, oa, od, slow);
	} else if (funct == 0x04) {
		emit_fpu_sqrt(c, is_s, oa, od, slow);
	} else if ((funct >= 0x08 && funct <= 0x0F) || funct == 0x24 || funct == 0x25) {
		emit_fpu_to_int(c, is_s, funct, oa, od, slow);
	} else if (funct == 0x20 || funct == 0x21) {
		emit_fpu_cvt(c, is_s, oa, od, slow);
	} else if (funct == 0x05 || funct == 0x07) {  /* ABS, NEG */
		int ok = new_label(c);
		unsigned emax = is_s ? 0xFF : 0x7FF;
		if (is_s) {
			E(c, ppc_lwz(9, oa, R_CPU));
			E(c, ppc_rlwinm(11, 9, 9, 24, 31));
			E(c, ppc_rlwinm(12, 9, 0, 9, 31));
		} else {
			E(c, ppc_ld(9, oa, R_CPU));
			E(c, ppc_rldicl(11, 9, 12, 53));
			E(c, ppc_clrldi(12, 9, 12));
		}
		/* NaN y desnormales (mantisa ≠ 0 con exponente 0 o máximo): lento. */
		E(c, ppc_cmpdi(CR_T, 12, 0));
		emit_bc(c, PPC_BO_TRUE_LIKELY, PPC_BI(CR_T, PPC_EQ), ok);
		E(c, ppc_cmpwi(CR_T, 11, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		E(c, ppc_cmpwi(CR_T, 11, (int32_t)emax));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		bind(c, ok);
		if (is_s) {
			E(c, funct == 0x07 ? ppc_xoris(9, 9, 0x8000) : ppc_rlwinm(9, 9, 0, 1, 31));
		} else if (funct == 0x07) {
			E(c, ppc_li(0, 1));
			E(c, ppc_sldi(0, 0, 63));
			E(c, ppc_xor(9, 9, 0));
		} else {
			E(c, ppc_clrldi(9, 9, 1));
		}
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_std(9, od, R_CPU));  /* en S, los 32 bits altos en 0 */
		emit_fcr31_cause(c, false);
	} else if (funct >= 0x30) {  /* C.cond sin NaN, en enteros */
		unsigned cond = funct & 15;
		int set = new_label(c), done = new_label(c);
		if (is_s) {
			E(c, ppc_lwz(9, oa, R_CPU));
			E(c, ppc_lwz(10, ob, R_CPU));
			E(c, ppc_rlwinm(11, 9, 0, 1, 31));
			E(c, ppc_rlwinm(12, 10, 0, 1, 31));
			E(c, ppc_lis(0, 0x7F80));
			E(c, ppc_extsw(9, 9));
			E(c, ppc_extsw(10, 10));
		} else {
			E(c, ppc_ld(9, oa, R_CPU));
			E(c, ppc_ld(10, ob, R_CPU));
			E(c, ppc_clrldi(11, 9, 1));
			E(c, ppc_clrldi(12, 10, 1));
			E(c, ppc_li(0, 0x7FF));
			E(c, ppc_sldi(0, 0, 52));
		}
		E(c, ppc_cmpld(CR_T, 11, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
		E(c, ppc_cmpld(CR_T, 12, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
		/* Signo y magnitud → entero ordenado: (m ^ s) - s con s = 0 o -1. */
		E(c, ppc_sradi(9, 9, 63));
		E(c, ppc_sradi(10, 10, 63));
		E(c, ppc_xor(11, 11, 9));
		E(c, ppc_subf(11, 9, 11));
		E(c, ppc_xor(12, 12, 10));
		E(c, ppc_subf(12, 10, 12));
		E(c, ppc_cmpd(CR_T, 11, 12));
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_rlwinm(7, 6, 0, 20, 13));  /* causas en 0 */
		E(c, ppc_rlwinm(7, 7, 0, 9, 7));    /* C (bit 23) en 0 */
		if (cond & 4)
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_LT), set);
		if (cond & 2)
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), set);
		emit_b(c, done, false);
		bind(c, set);
		E(c, ppc_oris(7, 7, 0x0080));
		bind(c, done);
		int same = new_label(c);
		E(c, ppc_cmpw(CR_T, 7, 6));
		emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), same);
		E(c, ppc_stw(7, OFF(fcr31), R_CPU));
		bind(c, same);
	} else {  /* ADD SUB MUL DIV */
		int done = new_label(c);
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_andi_(0, 6, 0x83));  /* RM ≠ 0 o I habilitada */
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(0, PPC_EQ), slow);
		E(c, is_s ? ppc_lwz(9, oa, R_CPU) : ppc_ld(9, oa, R_CPU));
		E(c, is_s ? ppc_lwz(10, ob, R_CPU) : ppc_ld(10, ob, R_CPU));
		emit_fpu_band(c, is_s, 9, 12, slow);
		emit_fpu_band(c, is_s, 10, 4, slow);
		if (funct == 0x03) {  /* entre cero: Z o V */
			E(c, is_s ? ppc_rlwinm(0, 10, 0, 1, 31) : ppc_clrldi(0, 10, 1));
			E(c, ppc_cmpdi(CR_T, 0, 0));
			emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
		}
		E(c, is_s ? ppc_lfs(1, oa, R_CPU) : ppc_lfd(1, oa, R_CPU));
		E(c, is_s ? ppc_lfs(2, ob, R_CPU) : ppc_lfd(2, ob, R_CPU));
		if (funct == 0x01)
			E(c, ppc_fneg(2, 2));  /* SUB = ADD con -b */
		switch (funct) {
		case 0x00: case 0x01: E(c, ppc_fadd(0, 1, 2)); break;
		case 0x02:            E(c, ppc_fmul(0, 1, 2)); break;
		default:              E(c, ppc_fdiv(0, 1, 2)); break;
		}
		E(c, ppc_li(5, 1));
		if (is_s) {
			E(c, ppc_frsp(3, 0));
			E(c, ppc_fcmpu(CR_T, 3, 0));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);  /* (double)f != r */
			if (funct <= 0x01) {
				/* Exponentes a más de 28 (y ninguno cero): TwoSum, lento. */
				int exact = new_label(c);
				E(c, ppc_cmpwi(CR_T, 12, 0));
				emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
				E(c, ppc_cmpwi(CR_T, 4, 0));
				emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
				E(c, ppc_subf(11, 4, 12));
				E(c, ppc_addi(11, 11, 28));  /* no r0: en addi vale 0 */
				E(c, ppc_cmplwi(CR_T, 11, 56));
				emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
				bind(c, exact);
				E(c, ppc_li(5, 0));
			} else if (funct == 0x02) {
				E(c, ppc_li(5, 0));  /* el producto de dos float es exacto en double */
			} else {
				E(c, ppc_fnmsub(4, 0, 2, 1));  /* a - r·b */
				E(c, ppc_fsub(5, 1, 1));
				E(c, ppc_fcmpu(CR_T, 4, 5));
				emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
				E(c, ppc_li(5, 0));
			}
		} else {
			if (funct <= 0x01) {  /* TwoSum */
				E(c, ppc_fsub(4, 0, 1));       /* bb = r - a */
				E(c, ppc_fsub(5, 0, 4));       /* r - bb */
				E(c, ppc_fsub(5, 1, 5));       /* a - (r - bb) */
				E(c, ppc_fsub(3, 2, 4));       /* b - bb */
				E(c, ppc_fadd(4, 5, 3));
			} else if (funct == 0x02) {
				E(c, ppc_fmsub(4, 1, 2, 0));   /* a·b - r */
			} else {
				E(c, ppc_fnmsub(4, 0, 2, 1));  /* a - r·b */
			}
			E(c, ppc_fsub(5, 1, 1));
			E(c, ppc_fcmpu(CR_T, 4, 5));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
			E(c, ppc_li(5, 0));
		}
		bind(c, done);
		if (is_s)
			emit_store_s(c, 3, od);
		else
			E(c, ppc_stfd(0, od, R_CPU));
		emit_fcr31_cause(c, true);
	}
	bind(c, resume);
	emit_slow_exec(c, k, k, slow, resume);  /* no toca registros generales */
}

static void emit_insn(struct ctx *c, unsigned k)
{
	const struct dec *d = &c->d[k];
	uint32_t i = d->w;

	flush_eager(c, k);
	c->k = k;
	c->written_start = c->written;
	if (d->how == H_EXEC) {
		emit_exec(c, k);
		return;
	}
	if (d->how == H_EXC) {
		emit_exception(c, k, I_FUNCT(i) == 0x0C ? SALSA64_EXC_SYS : SALSA64_EXC_BP);
		return;
	}
	switch (I_OP(i)) {
	case 0x00: emit_special(c, i); return;
	case 0x10: emit_cop0(c, k); return;
	case 0x11:
		if ((I_FMT(i) & ~1u) == FMT_S || I_FMT(i) == FMT_W)
			emit_fpu(c, k);
		else
			emit_cop1(c, k);
		return;
	case 0x1A: case 0x1B: case 0x22: case 0x26:
	case 0x2A: case 0x2C: case 0x2D: case 0x2E:
		emit_unaligned(c, k);
		return;
	case 0x20: emit_load(c, k, SALSA64_REC_LB, DST_GPR); return;
	case 0x21: emit_load(c, k, SALSA64_REC_LH, DST_GPR); return;
	case 0x23: emit_load(c, k, SALSA64_REC_LW, DST_GPR); return;
	case 0x24: emit_load(c, k, SALSA64_REC_LBU, DST_GPR); return;
	case 0x25: emit_load(c, k, SALSA64_REC_LHU, DST_GPR); return;
	case 0x27: emit_load(c, k, SALSA64_REC_LWU, DST_GPR); return;
	case 0x37: emit_load(c, k, SALSA64_REC_LD, DST_GPR); return;
	case 0x31: emit_load(c, k, SALSA64_REC_LWU, DST_FPR32); return;
	case 0x35: emit_load(c, k, SALSA64_REC_LD, DST_FPR64); return;
	case 0x28: emit_store(c, k, SALSA64_REC_SB, DST_GPR); return;
	case 0x29: emit_store(c, k, SALSA64_REC_SH, DST_GPR); return;
	case 0x2B: emit_store(c, k, SALSA64_REC_SW, DST_GPR); return;
	case 0x3F: emit_store(c, k, SALSA64_REC_SD, DST_GPR); return;
	case 0x39: emit_store(c, k, SALSA64_REC_SW, DST_FPR32); return;
	case 0x3D: emit_store(c, k, SALSA64_REC_SD, DST_FPR64); return;
	default:   emit_imm(c, i); return;
	}
}

/* ---- Saltos ------------------------------------------------------------------- */

/* ¿Consultaría el intérprete idle_hint en este salto? Saltos condicionales
 * (también los de enlace y BC1) y J, tomados hacia atrás a menos de
 * SALSA64_IDLE_BODY instrucciones, que la tabla de descartados no tenga. */
static bool idle_candidate(const struct ctx *c, const struct dec *d, uint64_t bpc)
{
	uint32_t i = d->w;
	if (d->kind == K_JUMP_REG || (d->kind == K_JUMP && I_OP(i) != 0x02))
		return false;
	uint64_t target = I_OP(i) == 0x02 ? ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2
					  : bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);
	if (bpc - target >= 4 * SALSA64_IDLE_BODY)
		return false;
	const struct salsa64_cpu *cpu = c->rec->cpu;
	return cpu->idle_reject[(bpc >> 2) & (SALSA64_IDLE_CACHE - 1)] != ((uint32_t)bpc ^ i);
}

/* El salto en b y su delay slot en b + 1 (la última del bloque). */
static void emit_branch(struct ctx *c, unsigned b)
{
	const struct dec *d = &c->d[b];
	uint32_t i = d->w;
	unsigned op = I_OP(i), rs = I_RS(i), rt = I_RT(i), a, x;
	uint64_t bpc = insn_pc(c, b);
	bool likely = d->kind == K_BRANCH_L;

	flush_eager(c, b);
	c->k = b;
	c->written_start = c->written;
	c->br_kind = d->kind;
	c->br_fall = bpc + 8;
	c->br_target = bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);

	switch (op) {
	case 0x02: case 0x03:                                             /* J JAL */
		c->br_target = ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		if (op == 0x03) {
			x = dst(c, 31, T_V);
			li64(c, x, c->br_fall);
			put(c, 31, x);
		}
		break;
	case 0x00:                                                        /* JR JALR */
		a = get(c, rs, R_TARGET);
		if (a != R_TARGET)
			E(c, ppc_mr(R_TARGET, a));
		if (I_FUNCT(i) == 0x09 && I_RD(i)) {
			x = dst(c, I_RD(i), T_V);
			li64(c, x, c->br_fall);
			put(c, I_RD(i), x);
		}
		break;
	case 0x04: case 0x05: case 0x14: case 0x15:                       /* BEQ BNE (L) */
		a = get(c, rs, T_S1);
		if (rt == 0) {
			E(c, ppc_cmpdi(CR_BR, a, 0));
		} else {
			x = get(c, rt, T_S2);
			E(c, ppc_cmpd(CR_BR, a, x));
		}
		c->br_bi = PPC_BI(CR_BR, PPC_EQ);
		c->br_bo = (op & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;
		break;
	case 0x06: case 0x07: case 0x16: case 0x17:                       /* BLEZ BGTZ (L) */
		a = get(c, rs, T_S1);
		E(c, ppc_cmpdi(CR_BR, a, 0));
		c->br_bi = PPC_BI(CR_BR, PPC_GT);
		c->br_bo = (op & 1) ? PPC_BO_TRUE : PPC_BO_FALSE;
		break;
	case 0x01:                                                        /* REGIMM */
		a = get(c, rs, T_S1);
		E(c, ppc_cmpdi(CR_BR, a, 0));
		c->br_bi = PPC_BI(CR_BR, PPC_LT);
		c->br_bo = (rt & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;  /* BGEZ: no menor que 0 */
		if (rt & 0x10) {  /* enlace después de leer rs */
			x = dst(c, 31, T_V);
			li64(c, x, c->br_fall);
			put(c, 31, x);
		}
		break;
	case 0x11:                                                        /* BC1F BC1T (L) */
		E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
		E(c, ppc_rlwinm(0, 0, 9, 31, 31));  /* bit 23, C */
		E(c, ppc_cmpwi(CR_BR, 0, 0));
		c->br_bi = PPC_BI(CR_BR, PPC_EQ);
		c->br_bo = (rt & 1) ? PPC_BO_FALSE : PPC_BO_TRUE;
		break;
	}

	/* Bucle de espera posible (idle.c): el camino tomado vuelve a C antes del
	 * delay slot, en el mismo estado en que el intérprete consulta idle_hint;
	 * rec.c llama a salsa64_vr4300_idle y, si la tabla lo descarta, invalida
	 * el bloque para compilarlo sin la consulta. */
	bool idle = idle_candidate(c, d, bpc);
	int idle_exit = -1;
	if (idle) {
		idle_exit = new_label(c);
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, idle_exit);
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 1));
		li64(c, 3, bpc);
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->rec->exit_idle), false);
		c->sec = sec;
		if (d->kind == K_BRANCH)
			emit_bc(c, c->br_bo, c->br_bi, idle_exit);
	}

	if (likely) {
		/* No tomado: se salta el delay slot y cuenta solo el salto. */
		int nt = new_label(c);
		emit_bc(c, c->br_bo == PPC_BO_TRUE ? PPC_BO_FALSE : PPC_BO_TRUE, c->br_bi, nt);
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, nt);
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)b + 1));
		emit_exit_to(c, c->br_fall);
		c->sec = sec;
	}
	if (idle && d->kind != K_BRANCH) {  /* J o branch-likely tomado: siempre a C */
		emit_b(c, idle_exit, false);
		return;
	}

	c->in_ds = true;
	emit_insn(c, b + 1);
	c->in_ds = false;

	writeback(c, c->written);
	E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)c->n));
	switch (d->kind) {
	case K_JUMP_REG:
		E(c, ppc_mr(3, R_TARGET));
		restore_borrowed(c);
		emit_b(c, abs_label(c, c->temp || c->rec->no_link ? c->rec->exit_jump : c->rec->exit_jr), false);
		return;
	case K_BRANCH: {
		if (idle) {  /* el tomado ya salió antes del delay slot */
			emit_exit_to(c, c->br_fall);
			return;
		}
		int taken = new_label(c);
		emit_bc(c, c->br_bo, c->br_bi, taken);
		emit_exit_to(c, c->br_fall);
		bind(c, taken);
		emit_taken(c, c->br_target);
		return;
	}
	default:  /* J/JAL o branch-likely tomado */
		emit_taken(c, c->br_target);
		return;
	}
}

/* ---- Bloque ------------------------------------------------------------------- */

static uint32_t fetch(const struct salsa64_rec *rec, uint32_t pa)
{
	return salsa64_rd32(rec->cpu->rdram + pa);
}

static void decode_at(const struct salsa64_rec *rec, uint32_t pa, struct dec *d)
{
	uint32_t i = fetch(rec, pa);
	decode(i, d);
	if (rec->opts & SALSA64_REC_OPT_RARE_EXEC)
		decode_rare_exec(i, d);
}

/* Elige los registros MIPS más usados del bloque para r14–r25 (o r14–r17
 * con registros fijos, que ya están en r18–r25).
 *
 * Con registros fijos, un bucle (bloque que vuelve sobre sí mismo) que
 * necesita más de 4 registros propios toma prestados los fijos que no usa:
 * los guarda en cpu->gpr al entrar y los repone en cada salida
 * (restore_borrowed). En la PS3, un bucle de enteros con 9 temporales pasó
 * de 128 a ~750 MIPS así; fuera de los bucles no compensa. */
static void allocate(struct ctx *c, bool loop)
{
	unsigned count[34] = { 0 }, nslots = 0;
	uint8_t slots[N_CACHE];
	uint64_t used = 0;
	for (unsigned k = 0; k < c->n; k++) {
		uint64_t m = c->d[k].reads | c->d[k].writes;
		used |= m;
		for (unsigned r = 1; r < 34; r++)
			if (m & BIT(r))
				count[r]++;
	}
	memset(c->map, 0, sizeof c->map);
	c->cached = c->pinned = c->borrowed = 0;
	if (c->rec->opts & SALSA64_REC_OPT_PIN) {
		uint64_t pins = 0;
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
			pins |= BIT(salsa64_rec_pin[k]);
		unsigned demand = (unsigned)__builtin_popcountll(used & ~pins & ~1ull);
		for (unsigned r = R_CACHE0; r < SALSA64_REC_PIN_PPC; r++)
			slots[nslots++] = (uint8_t)r;
		for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++) {
			unsigned m = salsa64_rec_pin[k], p = SALSA64_REC_PIN_PPC + k;
			if (loop && !(used & BIT(m)) && nslots < demand && nslots < N_CACHE) {
				c->borrowed |= BIT(m);
				slots[nslots++] = (uint8_t)p;
			} else {
				c->map[m] = (uint8_t)p;
				c->pinned |= BIT(m);
				count[m] = 0;
			}
		}
		c->cached = c->pinned;
	} else {
		for (unsigned r = R_CACHE0; r < R_CACHE0 + N_CACHE; r++)
			slots[nslots++] = (uint8_t)r;
	}
	for (unsigned slot = 0; slot < nslots; slot++) {
		unsigned best = 0;
		for (unsigned r = 1; r < 34; r++)
			if (count[r] > count[best] && !(c->cached & BIT(r)))
				best = r;
		if (!best)
			break;
		c->map[best] = slots[slot];
		c->cached |= BIT(best);
		count[best] = 0;
	}
}

/* Registro del PPE de un fijo prestado (ver allocate). */
static unsigned pin_ppc(unsigned m)
{
	for (unsigned k = 0; k < SALSA64_REC_PIN_N; k++)
		if (salsa64_rec_pin[k] == m)
			return SALSA64_REC_PIN_PPC + k;
	return 0;
}

/* Antes de cualquier salida del bloque: los fijos prestados vuelven a su
 * registro (el que sigue los espera ahí). */
static void restore_borrowed(struct ctx *c)
{
	for (unsigned r = 1; c->borrowed && r < 32; r++)
		if (c->borrowed & BIT(r))
			E(c, ppc_ld(pin_ppc(r), off_reg(r), R_CPU));
}

/* Ubica las secciones en la caché a partir de la palabra `base`, resuelve los
 * saltos y copia. Devuelve las palabras usadas, o 0 si algo no cupo. */
static uint32_t finalize(struct salsa64_rec *rec, uint32_t base, uint32_t limit)
{
	uint32_t nhot = rec->nbuf[SEC_HOT], ncold = rec->nbuf[SEC_COLD];
	if (rec->overflow || base + nhot + ncold > limit)
		return 0;
	uint32_t start[2] = { base, base + nhot };
	for (uint32_t f = 0; f < rec->nfixups; f++) {
		const struct salsa64_rec_fixup *fx = &rec->fixups[f];
		const struct salsa64_rec_label *l = &rec->labels[fx->label];
		int64_t to;
		if (l->sec == SEC_ABS)
			to = (int32_t)l->pos;
		else if (l->sec == SEC_HOT || l->sec == SEC_COLD)
			to = (int64_t)start[l->sec] + l->pos;
		else
			return 0;  /* etiqueta sin fijar: error del compilador */
		int64_t from = (int64_t)start[fx->sec] + fx->at;
		int64_t disp = (to - from) * 4;
		uint32_t *w = &rec->buf[fx->sec][fx->at];
		if (fx->cond) {
			if (!ppc_fits_bc(disp))
				return 0;
			*w = (*w & ~0xFFFCu) | ((uint32_t)disp & 0xFFFC);
		} else {
			if (!ppc_fits_b(disp))
				return 0;
			*w = (*w & ~0x03FFFFFCu) | ((uint32_t)disp & 0x03FFFFFC);
		}
	}
	memcpy(rec->code + start[0], rec->buf[SEC_HOT], nhot * 4);
	memcpy(rec->code + start[1], rec->buf[SEC_COLD], ncold * 4);
	salsa64_rec_sync(rec->code + base, (nhot + ncold) * 4);
	return nhot + ncold;
}

uint32_t salsa64_rec_compile(struct salsa64_rec *rec, uint64_t pc, unsigned max, bool temp,
			     unsigned *n_out, uint32_t *bytes_out)
{
	static struct ctx ctx;  /* grande para la pila; el recompilador corre en un solo hilo */
	struct ctx *c = &ctx;
	uint32_t pa = (uint32_t)pc & 0x1FFFFFFF;

	memset(c, 0, sizeof *c);
	c->rec = rec;
	c->pc = pc;
	c->pa = pa;
	c->temp = temp;
	if (max > SALSA64_REC_MAX_INSNS)
		max = SALSA64_REC_MAX_INSNS;

	/* Decodificación: hasta un salto con su delay slot, una instrucción que
	 * termina el bloque, el máximo o el final de la RDRAM. Un salto cuyo
	 * delay slot no se puede compilar (otro salto, COP0…) queda fuera. */
	unsigned n = 0;
	bool any_cop1 = false;
	while (n < max && pa + 4 * n + 4 <= rec->rdram_size) {
		struct dec *d = &c->d[n];
		decode_at(rec, pa + 4 * n, d);
		if (is_branch(d)) {
			if (n + 1 >= max || pa + 4 * n + 8 > rec->rdram_size)
				break;
			decode_at(rec, pa + 4 * n + 4, &c->d[n + 1]);
			if (c->d[n + 1].kind != K_NORMAL)
				break;
			n += 2;
			c->has_branch = true;
			break;
		}
		n++;
		if (d->kind == K_END)
			break;
	}
	if (n == 0)
		return 0;
	c->n = n;
	for (unsigned k = 0; k < n; k++) {
		any_cop1 |= c->d[k].cop1;
		c->exit_before[k] = c->exit_after[k] = -1;
	}
	/* ¿Bucle? El salto final vuelve al inicio (emit_taken). */
	bool loop = false;
	if (c->has_branch && !temp) {
		uint32_t i = c->d[n - 2].w;
		uint64_t bpc = pc + 4ull * (n - 2), target = 0;
		if (I_OP(i) == 0x02 || I_OP(i) == 0x03)
			target = ((bpc + 4) & ~0x0FFFFFFFull) | (uint64_t)I_TARGET(i) << 2;
		else if (c->d[n - 2].kind != K_JUMP_REG)
			target = bpc + 4 + ((uint64_t)(int64_t)(int16_t)i << 2);
		loop = target == pc;
	}
	allocate(c, loop);

	uint64_t seen = 0, livein = 0;
	for (unsigned k = 0; k < n; k++) {
		livein |= c->d[k].reads & ~seen;
		seen |= c->d[k].writes;
		for (unsigned r = 1; r < 34; r++)
			if (c->d[k].writes & BIT(r))
				c->last_write[r] = (uint8_t)(k + 1);
	}

	rec->nbuf[0] = rec->nbuf[1] = 0;
	rec->nlabels = rec->nfixups = 0;
	rec->overflow = false;
	c->sec = SEC_HOT;

	/* Entrada: ¿cabe antes del próximo evento?, ¿COP1 usable? */
	int start_exit = new_label(c);
	E(c, ppc_addi(0, R_CYCLES, (int32_t)n));
	E(c, ppc_cmpld(0, 0, R_NEXT));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(0, PPC_GT), start_exit);
	if (any_cop1) {
		E(c, ppc_lwz(0, OFF(status), R_CPU));
		E(c, ppc_andis_(0, 0, SALSA64_ST_CU1 >> 16));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(0, PPC_EQ), start_exit);
	}
	for (unsigned r = 1; c->borrowed && r < 32; r++)  /* fijos prestados: a cpu->gpr */
		if (c->borrowed & BIT(r))
			E(c, ppc_std(pin_ppc(r), off_reg(r), R_CPU));
	reload(c, livein & ~c->pinned);
	c->body = new_label(c);
	bind(c, c->body);

	for (unsigned k = 0; k < n; k++) {
		if (c->has_branch && k == n - 2) {
			emit_branch(c, k);
			break;
		}
		if (unaligned_pair(c, k, c->has_branch ? n - 2 : n)) {
			emit_unaligned_pair(c, k);
			k++;
			continue;
		}
		emit_insn(c, k);
		if (c->d[k].kind == K_END)
			break;
	}
	if (!c->has_branch && c->d[n - 1].kind != K_END) {
		writeback(c, c->written);
		E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)n));
		emit_exit_to(c, pc + 4ull * n);
	}

	c->sec = SEC_COLD;
	bind(c, start_exit);
	li64(c, 3, pc);
	emit_b(c, abs_label(c, rec->exit_mid), false);
	emit_pending_exits(c);

	uint32_t base = temp ? rec->scratch : rec->used;
	uint32_t limit = temp ? rec->code_words : rec->scratch;
	uint32_t words = finalize(rec, base, limit);
	if (!words)
		return 0;
	if (!temp)
		rec->used = (base + words + 31) & ~31u;  /* bloques alineados a 128 bytes */
	if (rec->on_compile)
		rec->on_compile(rec, pc, base, words, temp);
	*n_out = n;
	*bytes_out = 4 * n;
	return base;
}
