/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: instrucciones que el código generado le pasaba
 * al intérprete y ahora traduce (2026-10-10): conversiones y raíz de la FPU,
 * CFC1/CTC1, LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR y MFC0/DMFC0. Diseño en
 * docs/rec-ppe.md.
 *
 * Solo lo incluye rec_emit.c, a mitad de archivo: usa su contexto (struct
 * ctx, struct dec) y sus funciones estáticas. decode() las sigue marcando
 * como antes (al intérprete); decode_ext() las promueve a nativas salvo con
 * SALSA64_REC_OPT_RARE_EXEC, que así repite lo de antes para medir.
 *
 * La referencia es el intérprete (cop1.c, interp.c, cop0.c): mismos bits,
 * mismas causas y banderas de FCR31, mismo ciclo. Lo que el camino nativo no
 * puede dar igual salta a la sección fría y lo corre el intérprete.
 */

/* ---- Decodificación ------------------------------------------------------------ */

static void decode_promote(uint32_t i, struct dec *d)
{
	unsigned op = I_OP(i), fmt = I_RS(i), f = I_FUNCT(i);
	switch (op) {
	case 0x10:  /* MFC0 DMFC0: solo leen y el bloque sigue; Random, el intérprete */
		if (fmt <= 0x01) {
			d->kind = K_NORMAL;
			d->reads &= ~BIT(I_RT(i));
			d->how = I_RD(i) == SALSA64_CP0_RANDOM ? H_EXEC : H_NATIVE;
		}
		return;
	case 0x11:
		if (fmt == 0x02 || fmt == 0x06 ||                                  /* CFC1 CTC1 */
		    (fmt == FMT_W && (f == 0x20 || f == 0x21)) ||                  /* CVT.S.W CVT.D.W */
		    ((fmt & ~1u) == FMT_S &&                                       /* S y D */
		     (f == 0x04 || (f >= 0x08 && f <= 0x0F) || f == 0x24 || f == 0x25 ||
		      (f == 0x20 && fmt == FMT_D) || (f == 0x21 && fmt == FMT_S))))
			d->how = H_NATIVE;
		return;
	case 0x1A: case 0x1B: case 0x22: case 0x26:                        /* LDL LDR LWL LWR */
	case 0x2A: case 0x2C: case 0x2D: case 0x2E:                        /* SWL SDL SDR SWR */
		d->how = H_NATIVE;
		return;
	default:
		return;
	}
}

static void decode_ext(const struct salsa64_rec *rec, uint32_t i, struct dec *d)
{
	decode(i, d);
	if (!(rec->opts & SALSA64_REC_OPT_RARE_EXEC))
		decode_promote(i, d);
}

/* ---- Camino lento ---------------------------------------------------------------- */

/* Las instrucciones k..last, traducidas en nativo, las corre el intérprete
 * una por una, en la sección fría, y se vuelve a `resume`. Se llega antes de
 * que el camino rápido escriba ningún registro, así que los del PPE tienen
 * los valores de antes de la instrucción k. */
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

/* ---- LWL, LWR, LDL, LDR, SWL, SWR, SDL, SDR ---------------------------------------- */

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
 * Sueltas (interp.c: op_lwl_lwr y compañía). El desplazamiento sale de la
 * dirección, así que la mezcla usa desplazamientos por registro (microcódigo
 * en el PPE, pero mucho menos que una llamada al intérprete). Los
 * almacenamientos leen la palabra, la mezclan y la escriben. Fuera de la
 * RDRAM, o con código en esos 128 bytes, el intérprete.
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

/* ¿LWL y LWR (o SWL y SWR), en cualquier orden, consecutivos y nativos sobre
 * los mismos 4 bytes? Juntos cargan (con signo) o escriben esos 4 bytes, sea
 * cual sea la alineación: es como el compilador hace un acceso no alineado.
 * Una carga cuya base es rt no: la primera cambia la base. */
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

/* Si k y k + 1 (antes de `end`) son un par de unaligned_pair, los traduce
 * juntos, byte por byte: sin desplazamientos por registro y, al escribir, sin
 * leer la palabra (un par que escribe la palabra que el anterior acaba de
 * escribir chocaría: load-hit-store). */
static bool emit_unaligned_pair(struct ctx *c, unsigned k, unsigned end)
{
	if (!unaligned_pair(c, k, end))
		return false;
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
	return true;
}

/* ---- COP0: MFC0 y DMFC0 ------------------------------------------------------- */

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

/* En modo kernel, el único en que corre un bloque, no hay excepción posible.
 * Count es (cycles >> 1) + count_offset con cycles = r29 + k + 1, lo que el
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

/* ---- COP1: CFC1 y CTC1 ---------------------------------------------------------- */

static void emit_cfc1(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), fs = I_FS(i), d = dst(c, rt, T_V);
	if (fs == 31) {
		E(c, ppc_lwz(T_X, OFF(fcr31), R_CPU));
		E(c, ppc_extsw(d, T_X));
	} else {
		E(c, ppc_li(d, fs == 0 ? SALSA64_FCR0_VALUE : 0));
	}
	put(c, rt, d);
}

/* Solo FCR31 se escribe. Una causa con su excepción habilitada (E siempre lo
 * está) la lanza: eso lo hace el intérprete. */
static void emit_ctc1(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	if (I_FS(i) != 31)
		return;
	int slow = new_label(c), resume = new_label(c);
	unsigned v = get(c, I_RT(i), T_S1);
	E(c, ppc_lis(T_X, 0x0183));
	E(c, ppc_ori(T_X, T_X, 0xFFFF));
	E(c, ppc_and(T_X, v, T_X));                  /* bits escribibles (cop1.c) */
	E(c, ppc_rlwinm(T_Y, T_X, 25, 27, 31));      /* habilitaciones */
	E(c, ppc_ori(T_Y, T_Y, 0x20));
	E(c, ppc_rlwinm(0, T_X, 20, 26, 31));        /* causas */
	E(c, ppc_and(0, 0, T_Y));
	E(c, ppc_cmpwi(CR_T, 0, 0));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);
	E(c, ppc_stw(T_X, OFF(fcr31), R_CPU));
	bind(c, resume);
	emit_slow_exec(c, k, k, slow, resume);
}

/* ---- FPU: raíz y conversiones ------------------------------------------------------ */

/*
 * Como el resto de la FPU nativa (emit_fpu): los bits del operando se
 * cargan de cpu->fpr a un registro entero para clasificarlo y, aparte, el
 * valor a uno de punto flotante; el resultado va de ahí a cpu->fpr. Nunca
 * se pasa un valor entre registros enteros y de punto flotante.
 */

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
 * cpu->fpr, y llevarlo a un registro de punto flotante obliga a escribirlo y
 * releerlo (load-hit-store; ppu-gcc hace eso con `(double)x`). |x| ≤ 2^31 se
 * normaliza con el 1 más alto en el bit 31; en D es exacto, en S se redondea
 * a 24 bits al más cercano par (sumando 0x7F más el bit que queda abajo) y
 * es inexacto si sobran bits. CVT.S.W necesita redondeo al más cercano e I
 * deshabilitada; CVT.D.W nunca tiene causa.
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

/* Operandos como en emit_fpu: fs con el bit bajo ignorado si FR = 0; en S y
 * W, la mitad baja (+4); fd, el registro físico completo. */
static void emit_fpu_ext(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned fmt = I_FMT(i), funct = I_FUNCT(i), fs = I_FS(i);
	bool is_s = fmt == FMT_S;
	unsigned fsr = c->rec->fr ? fs : fs & ~1u;
	int32_t oa = OFF(fpr) + 8 * (int32_t)fsr + (fmt == FMT_D ? 0 : 4);
	int32_t od = OFF(fpr) + 8 * (int32_t)I_FD(i);
	int slow = new_label(c), resume = new_label(c);

	if (fmt == FMT_W)
		emit_fpu_from_w(c, funct == 0x20, oa, od, slow);
	else if (funct == 0x04)
		emit_fpu_sqrt(c, is_s, oa, od, slow);
	else if (funct == 0x20 || funct == 0x21)
		emit_fpu_cvt(c, is_s, oa, od, slow);
	else
		emit_fpu_to_int(c, is_s, funct, oa, od, slow);
	bind(c, resume);
	emit_slow_exec(c, k, k, slow, resume);  /* no toca registros generales */
}

/* ---- Despacho ---------------------------------------------------------------------- */

/* Lo nativo de este archivo (decode_promote); false si es otra cosa. */
static bool emit_ext(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned fmt = I_RS(i), f = I_FUNCT(i);
	switch (I_OP(i)) {
	case 0x10:
		emit_cop0(c, k);
		return true;
	case 0x11:
		if (fmt == 0x02) {
			emit_cfc1(c, k);
			return true;
		}
		if (fmt == 0x06) {
			emit_ctc1(c, k);
			return true;
		}
		if (fmt == FMT_W || ((fmt & ~1u) == FMT_S && (f == 0x04 || (f >= 0x08 && f <= 0x0F) ||
							       (f >= 0x20 && f <= 0x25)))) {
			emit_fpu_ext(c, k);
			return true;
		}
		return false;
	case 0x1A: case 0x1B: case 0x22: case 0x26:
	case 0x2A: case 0x2C: case 0x2D: case 0x2E:
		emit_unaligned(c, k);
		return true;
	default:
		return false;
	}
}
