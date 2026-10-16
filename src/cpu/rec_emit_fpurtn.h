/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — la FPU compacta: las operaciones de la FPU como rutinas
 * compartidas del preámbulo (docs/fastmem.md §9, docs/cache-ppe.md §7.2).
 *
 * Lo que emit_fpu y emit_fpu_ext emiten en línea en cada sitio (compuerta
 * de FCR31, clasificación de los operandos, la operación con su redondeo y
 * el inexacto, el resultado y FCR31: ~35–40 palabras por operación) vive una
 * vez por clase de operación al final del preámbulo, y el sitio solo pasa
 * los desplazamientos de los operandos y llama. Misma semántica que el
 * código en línea (es el mismo código, con los desplazamientos en registros
 * en vez de inmediatos): lo que el camino nativo no cubre vuelve con cr1 ne
 * y el sitio llama al intérprete, como antes.
 *
 * Convención de las rutinas (fpu_rtn_abi):
 *   r3  desplazamiento del operando fs en struct salsa64_cpu (con el +4 de
 *       la mitad baja en S y W), r4 el de ft (binarias), r8 (T_V) el del
 *       registro fd completo; r10 el extra: la condición de C.cond, 1 en
 *       NEG, la funct de las conversiones a entero.
 *   Vuelve con cr1 eq si hizo la operación (fd y FCR31 ya escritos) o cr1
 *   ne si no (sin escribir nada). Pisa r0, r3–r6, r8–r12, f0–f5, cr0 y
 *   cr1; no toca r7 (la base en caché de un grupo) ni LR más allá del blr.
 */

/* ---- Operandos por registro ---------------------------------------------------- */

/* Los bits del operando en `rt` (entero) y su valor en `ft`, desde el
 * desplazamiento en `off`. */
static void fr_load(struct ctx *c, bool is_s, unsigned rt, unsigned ft, unsigned off)
{
	E(c, is_s ? ppc_lwzx(rt, R_CPU, off) : ppc_ldx(rt, R_CPU, off));
	if (ft)
		E(c, is_s ? ppc_lfsx(ft, R_CPU, off) : ppc_lfdx(ft, R_CPU, off));
}

/* Resultado S desde `fr` al registro fd cuyo desplazamiento está en T_V:
 * la mitad baja, y los 32 bits altos en 0. Pisa r12. */
static void fr_store_s(struct ctx *c, unsigned fr)
{
	E(c, ppc_addi(12, T_V, 4));
	E(c, ppc_stfsx(fr, R_CPU, 12));
	E(c, ppc_li(0, 0));
	E(c, ppc_stwx(0, R_CPU, T_V));
}

/* Fin de una rutina: cr1 eq (hecho) o ne (al intérprete). */
static void fr_return(struct ctx *c, bool ok)
{
	E(c, ppc_li(0, ok ? 0 : 1));
	E(c, ppc_cmpwi(1, 0, 0));
	E(c, PPC_BLR);
}

/* ---- Las rutinas --------------------------------------------------------------------- */

/* ADD, SUB, MUL, DIV (emit_fpu). */
static void fr_arith(struct ctx *c, bool is_s, unsigned funct, int slow)
{
	int done = new_label(c);
	emit_fcr31_gate(c, 0x83, slow);
	fr_load(c, is_s, 9, 1, 3);
	fr_load(c, is_s, 10, 2, 4);
	if (is_s && funct == 0x02) {
		emit_fpu_mul_s_range(c, slow);
	} else {
		emit_fpu_band(c, is_s, 9, 12, slow);
		emit_fpu_band(c, is_s, 10, 4, slow);
	}
	if (funct == 0x03) {  /* entre cero: Z o V */
		E(c, is_s ? ppc_rlwinm(0, 10, 0, 1, 31) : ppc_clrldi(0, 10, 1));
		E(c, ppc_cmpdi(CR_T, 0, 0));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	}
	if (funct == 0x01)
		E(c, ppc_fneg(2, 2));
	switch (funct) {
	case 0x00: case 0x01: E(c, ppc_fadd(0, 1, 2)); break;
	case 0x02:            E(c, ppc_fmul(0, 1, 2)); break;
	default:              E(c, ppc_fdiv(0, 1, 2)); break;
	}
	E(c, ppc_li(5, 1));
	if (is_s) {
		E(c, ppc_frsp(3, 0));
		E(c, ppc_fcmpu(CR_T, 3, 0));
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
		if (funct <= 0x01) {
			int exact = new_label(c);
			E(c, ppc_cmpwi(CR_T, 12, 0));
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
			E(c, ppc_cmpwi(CR_T, 4, 0));
			emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), exact);
			E(c, ppc_subf(11, 4, 12));
			E(c, ppc_addi(11, 11, 28));
			E(c, ppc_cmplwi(CR_T, 11, 56));
			emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
			bind(c, exact);
			E(c, ppc_li(5, 0));
		} else if (funct == 0x02) {
			E(c, ppc_li(5, 0));
		} else {
			E(c, ppc_fnmsub(4, 0, 2, 1));
			E(c, ppc_fsub(5, 1, 1));
			E(c, ppc_fcmpu(CR_T, 4, 5));
			emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
			E(c, ppc_li(5, 0));
		}
	} else {
		if (funct <= 0x01) {
			E(c, ppc_fsub(4, 0, 1));
			E(c, ppc_fsub(5, 0, 4));
			E(c, ppc_fsub(5, 1, 5));
			E(c, ppc_fsub(3, 2, 4));
			E(c, ppc_fadd(4, 5, 3));
		} else if (funct == 0x02) {
			E(c, ppc_fmsub(4, 1, 2, 0));
		} else {
			E(c, ppc_fnmsub(4, 0, 2, 1));
		}
		E(c, ppc_fsub(5, 1, 1));
		E(c, ppc_fcmpu(CR_T, 4, 5));
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
		E(c, ppc_li(5, 0));
	}
	bind(c, done);
	if (is_s)
		fr_store_s(c, 3);
	else
		E(c, ppc_stfdx(0, R_CPU, T_V));
	emit_fcr31_cause(c, true);
	fr_return(c, true);
}

/* C.cond sin NaN, en enteros (emit_fpu); la condición en r10 (bit 2: <,
 * bit 1: =; el bit 0, no ordenado, no aplica sin NaN). */
static void fr_cmp(struct ctx *c, bool is_s, int slow)
{
	int set = new_label(c), done = new_label(c), no_lt = new_label(c), no_eq = new_label(c);
	fr_load(c, is_s, 9, 0, 3);
	fr_load(c, is_s, 5, 0, 4);  /* ft en r5: r10 lleva la condición */
	if (is_s) {
		E(c, ppc_rlwinm(11, 9, 0, 1, 31));
		E(c, ppc_rlwinm(12, 5, 0, 1, 31));
		E(c, ppc_lis(0, 0x7F80));
		E(c, ppc_extsw(9, 9));
		E(c, ppc_extsw(5, 5));
	} else {
		E(c, ppc_clrldi(11, 9, 1));
		E(c, ppc_clrldi(12, 5, 1));
		E(c, ppc_li(0, 0x7FF));
		E(c, ppc_sldi(0, 0, 52));
	}
	E(c, ppc_cmpld(CR_T, 11, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
	E(c, ppc_cmpld(CR_T, 12, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_GT), slow);
	E(c, ppc_sradi(9, 9, 63));
	E(c, ppc_sradi(5, 5, 63));
	E(c, ppc_xor(11, 11, 9));
	E(c, ppc_subf(11, 9, 11));
	E(c, ppc_xor(12, 12, 5));
	E(c, ppc_subf(12, 5, 12));
	E(c, ppc_cmpd(CR_T, 11, 12));
	E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
	E(c, ppc_rlwinm(T_V, 6, 0, 20, 13));  /* causas en 0 */
	E(c, ppc_rlwinm(T_V, T_V, 0, 9, 7));  /* C (bit 23) en 0 */
	E(c, ppc_rlwinm(0, 10, 0, 29, 29));   /* condición & 4 */
	E(c, ppc_cmpwi(0, 0, 0));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(0, PPC_EQ), no_lt);
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_LT), set);
	bind(c, no_lt);
	E(c, ppc_rlwinm(0, 10, 0, 30, 30));   /* condición & 2 */
	E(c, ppc_cmpwi(0, 0, 0));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(0, PPC_EQ), no_eq);
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), set);
	bind(c, no_eq);
	emit_b(c, done, false);
	bind(c, set);
	E(c, ppc_oris(T_V, T_V, 0x0080));
	bind(c, done);
	int same = new_label(c);
	E(c, ppc_cmpw(CR_T, T_V, 6));
	emit_bc(c, PPC_BO_TRUE, PPC_BI(CR_T, PPC_EQ), same);
	E(c, ppc_stw(T_V, OFF(fcr31), R_CPU));
	bind(c, same);
	fr_return(c, true);
}

/* ABS y NEG (emit_fpu); r10 = 1 para NEG. */
static void fr_absneg(struct ctx *c, bool is_s, int slow)
{
	int ok = new_label(c), is_neg = new_label(c), join = new_label(c);
	unsigned emax = is_s ? 0xFF : 0x7FF;
	fr_load(c, is_s, 9, 0, 3);
	if (is_s) {
		E(c, ppc_rlwinm(11, 9, 9, 24, 31));
		E(c, ppc_rlwinm(12, 9, 0, 9, 31));
	} else {
		E(c, ppc_rldicl(11, 9, 12, 53));
		E(c, ppc_clrldi(12, 9, 12));
	}
	E(c, ppc_cmpdi(CR_T, 12, 0));
	emit_bc(c, PPC_BO_TRUE_LIKELY, PPC_BI(CR_T, PPC_EQ), ok);
	E(c, ppc_cmpwi(CR_T, 11, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	E(c, ppc_cmpwi(CR_T, 11, (int32_t)emax));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	bind(c, ok);
	E(c, ppc_cmpwi(0, 10, 0));
	emit_bc(c, PPC_BO_FALSE, PPC_BI(0, PPC_EQ), is_neg);
	E(c, is_s ? ppc_rlwinm(9, 9, 0, 1, 31) : ppc_clrldi(9, 9, 1));  /* ABS */
	emit_b(c, join, false);
	bind(c, is_neg);
	if (is_s) {
		E(c, ppc_xoris(9, 9, 0x8000));
	} else {
		E(c, ppc_li(0, 1));
		E(c, ppc_sldi(0, 0, 63));
		E(c, ppc_xor(9, 9, 0));
	}
	bind(c, join);
	E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
	E(c, ppc_stdx(9, R_CPU, T_V));  /* en S, los 32 bits altos en 0 */
	emit_fcr31_cause(c, false);
	fr_return(c, true);
}

/* SQRT (emit_fpu_sqrt). */
static void fr_sqrt(struct ctx *c, bool is_s, int slow)
{
	int done = new_label(c);
	emit_fcr31_gate(c, 0x83, slow);
	fr_load(c, is_s, 9, 1, 3);
	emit_fpu_range(c, is_s, 9, 12, true, 1, is_s ? 253 : 2045, slow);
	E(c, ppc_fsqrt(0, 1));
	E(c, ppc_li(5, 1));
	if (is_s) {
		E(c, ppc_frsp(3, 0));
		E(c, ppc_fcmpu(CR_T, 3, 0));
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	}
	E(c, ppc_fnmsub(4, 0, 0, 1));
	E(c, ppc_fsub(5, 1, 1));
	E(c, ppc_fcmpu(CR_T, 4, 5));
	emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	E(c, ppc_li(5, 0));
	bind(c, done);
	if (is_s)
		fr_store_s(c, 3);
	else
		E(c, ppc_stfdx(0, R_CPU, T_V));
	emit_fcr31_cause(c, true);
	fr_return(c, true);
}

/* CVT.D.S y CVT.S.D (emit_fpu_cvt). */
static void fr_cvt(struct ctx *c, bool is_s, int slow)
{
	if (is_s) {
		fr_load(c, true, 9, 1, 3);
		emit_fpu_range(c, true, 9, 12, false, 1, 253, slow);
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
		E(c, ppc_stfdx(1, R_CPU, T_V));
		emit_fcr31_cause(c, false);
		fr_return(c, true);
		return;
	}
	int done = new_label(c);
	emit_fcr31_gate(c, 0x83, slow);
	fr_load(c, false, 9, 1, 3);
	emit_fpu_range(c, false, 9, 12, false, 897, 252, slow);
	E(c, ppc_frsp(3, 1));
	E(c, ppc_li(5, 1));
	E(c, ppc_fcmpu(CR_T, 3, 1));
	emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_T, PPC_EQ), done);
	E(c, ppc_li(5, 0));
	bind(c, done);
	fr_store_s(c, 3);
	emit_fcr31_cause(c, true);
	fr_return(c, true);
}

/* CVT.S.W y CVT.D.W (emit_fpu_from_w). */
static void fr_from_w(struct ctx *c, bool to_s, int slow)
{
	int store = new_label(c);
	if (to_s)
		emit_fcr31_gate(c, 0x83, slow);
	else
		E(c, ppc_lwz(6, OFF(fcr31), R_CPU));
	E(c, ppc_lwzx(9, R_CPU, 3));
	E(c, ppc_li(11, 0));
	E(c, ppc_li(5, 0));
	E(c, ppc_extsw(9, 9));
	E(c, ppc_cmpdi(CR_T, 9, 0));
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), store);
	E(c, ppc_sradi(10, 9, 63));
	E(c, ppc_xor(11, 9, 10));
	E(c, ppc_subf(11, 10, 11));
	E(c, ppc_cntlzw(12, 11));
	E(c, ppc_slw(11, 11, 12));
	if (to_s) {
		E(c, ppc_rlwinm(5, 11, 0, 24, 31));
		E(c, ppc_rlwinm(0, 11, 24, 31, 31));
		E(c, ppc_add(11, 11, 0));
		E(c, ppc_addi(11, 11, 0x7F));
		E(c, ppc_srdi(11, 11, 8));
		E(c, ppc_subfic(12, 12, 157));
		E(c, ppc_sldi(12, 12, 23));
		E(c, ppc_add(11, 11, 12));
		E(c, ppc_rlwinm(0, 9, 0, 0, 0));
		E(c, ppc_or(11, 11, 0));
		E(c, ppc_neg(5, 5));
		E(c, ppc_srdi(5, 5, 63));
	} else {
		E(c, ppc_sldi(11, 11, 21));
		E(c, ppc_subfic(12, 12, 1053));
		E(c, ppc_sldi(12, 12, 52));
		E(c, ppc_add(11, 11, 12));
		E(c, ppc_rldicr(0, 9, 0, 0));
		E(c, ppc_or(11, 11, 0));
	}
	bind(c, store);
	E(c, ppc_stdx(11, R_CPU, T_V));
	emit_fcr31_cause(c, to_s);
	fr_return(c, true);
}

/* ROUND, TRUNC, CEIL, FLOOR a W y L, y CVT.W y CVT.L (emit_fpu_to_int):
 * una rutina por funct. */
static void fr_to_int(struct ctx *c, bool is_s, unsigned funct, int slow)
{
	bool cvt = funct >= 0x24, to64 = cvt ? funct == 0x25 : funct < 0x0C;
	unsigned mode = cvt ? TOINT_RM : funct & 3;
	uint32_t emax = (is_s ? 127 : 1023) + (to64 ? 52 : is_s ? 30 : 29);
	emit_fcr31_gate(c, 0x80, slow);
	fr_load(c, is_s, 9, 1, 3);
	emit_fpu_range(c, is_s, 9, 12, false, 1, emax - 1, slow);
	emit_toint_core(c, mode);
	E(c, ppc_stfdx(0, R_CPU, T_V));
	if (!to64) {
		E(c, ppc_li(0, 0));
		E(c, ppc_stwx(0, R_CPU, T_V));
	}
	emit_fcr31_cause(c, true);
	fr_return(c, true);
}

/* ---- El conjunto ------------------------------------------------------------------- */

/* Qué rutina es cada índice de rec->fpu_rtn (rec.h, SALSA64_FPR_*). */
static void fr_emit_one(struct ctx *c, unsigned kind, int slow)
{
	if (kind < SALSA64_FPR_CMP_S) {  /* ADD SUB MUL DIV × S, D */
		fr_arith(c, kind < 4, kind & 3, slow);
		return;
	}
	switch (kind) {
	case SALSA64_FPR_CMP_S:    fr_cmp(c, true, slow); return;
	case SALSA64_FPR_CMP_D:    fr_cmp(c, false, slow); return;
	case SALSA64_FPR_ABSNEG_S: fr_absneg(c, true, slow); return;
	case SALSA64_FPR_ABSNEG_D: fr_absneg(c, false, slow); return;
	case SALSA64_FPR_SQRT_S:   fr_sqrt(c, true, slow); return;
	case SALSA64_FPR_SQRT_D:   fr_sqrt(c, false, slow); return;
	case SALSA64_FPR_CVT_D_S:  fr_cvt(c, true, slow); return;
	case SALSA64_FPR_CVT_S_D:  fr_cvt(c, false, slow); return;
	case SALSA64_FPR_CVT_S_W:  fr_from_w(c, true, slow); return;
	case SALSA64_FPR_CVT_D_W:  fr_from_w(c, false, slow); return;
	default: {  /* a entero: SALSA64_FPR_TOINT + (S: 0, D: 10) + índice de la funct */
		unsigned t = kind - SALSA64_FPR_TOINT;
		bool is_s = t < 10;
		t %= 10;
		unsigned funct = t < 8 ? 0x08 + t : t == 8 ? 0x24 : 0x25;
		fr_to_int(c, is_s, funct, slow);
		return;
	}
	}
}

/* Índice de rutina de una conversión a entero (funct 0x08–0x0F, 0x24, 0x25). */
static unsigned fr_toint_kind(bool is_s, unsigned funct)
{
	unsigned t = funct >= 0x24 ? 8 + (funct - 0x24) : funct - 0x08;
	return SALSA64_FPR_TOINT + (is_s ? 0 : 10) + t;
}

/* Emite todas las rutinas a partir de la palabra `base` de la caché (el
 * final del preámbulo), sin pasar de `limit`, y deja sus entradas en
 * rec->fpu_rtn[]. false si no caben: los sitios siguen en línea. */
bool salsa64_rec_emit_fpu_rtns(struct salsa64_rec *rec, uint32_t base, uint32_t limit)
{
	static struct ctx ctx;
	struct ctx *c = &ctx;
	int start[SALSA64_FPR_N];
	memset(c, 0, sizeof *c);
	c->rec = rec;
	c->tramp = -1;
	rec->nbuf[0] = rec->nbuf[1] = 0;
	rec->nlabels = rec->nfixups = 0;
	rec->fm_npend = 0;
	rec->overflow = false;
	c->sec = SEC_HOT;
	for (unsigned k = 0; k < SALSA64_FPR_N; k++) {
		int slow = new_label(c);
		start[k] = new_label(c);
		bind(c, start[k]);
		fr_emit_one(c, k, slow);
		bind(c, slow);
		fr_return(c, false);
	}
	uint32_t words = finalize(rec, base, limit);
	if (!words)
		return false;
	for (unsigned k = 0; k < SALSA64_FPR_N; k++)
		rec->fpu_rtn[k] = base + rec->labels[start[k]].pos;
	rec->fpu_rtn_words = words;
	return true;
}
