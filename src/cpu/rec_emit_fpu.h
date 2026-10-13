/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — recompilador: la FPU con los registros residentes en el PPE
 * (2026-10-11). Diseño en docs/rec-ppe.md, «FPU residente». Lo incluye
 * rec_emit.c (usa su contexto y sus funciones estáticas). Solo con
 * SALSA64_REC_OPT_FPU_RES: medida en la PS3 es más lenta que el camino por
 * cpu->fpr (emit_fpu, emit_cop1 y rec_emit_ext.h), que es el de omisión.
 * Por qué, en docs/rec-ppe.md («FPU residente»): cada fcmpu que alimenta un
 * salto o un crop espera toda la latencia de la FPU, y fabs agrega 10
 * ciclos a la clasificación.
 *
 * Resumen:
 *   - cpu->fpr[p] vive en f14–f25 mientras el bloque lo usa (los 12 más
 *     usados; el resto va por temporales). Un registro del PPE tiene el
 *     valor en un formato: S (la mitad baja como double, lo que da lfs) o
 *     D (los 64 bits); un uso en otro formato lo recarga de casa.
 *   - Casa (cpu->fpr) está siempre al día: cada resultado se escribe en
 *     cuanto está (el almacenamiento espera al valor, pero ya se esperó por
 *     él en la comparación). Así ninguna salida ni llamada a C tiene que
 *     escribir registros de la FPU, y el intérprete (camino lento) lee lo
 *     correcto.
 *   - Clasificación sin pasar por enteros: la franja segura del intérprete
 *     (fpu_fast_band: |x| en [2^-60, 2^61) en S, [2^-440, 2^441) en D, o
 *     cero) se prueba con fabs y tres fcmpu contra constantes que la
 *     entrada deja en f27–f31 (rec.c); en el PPE fcmpu da su bit del CR al
 *     ciclo siguiente y mover bits entre la FPU y los enteros pasa por
 *     memoria (load-hit-store). NaN e infinito fallan las comparaciones.
 *   - Inexacto, en punto flotante: S: frsp ≠ resultado; la suma en double
 *     de dos float es exacta si y solo si (r − a == b) ∧ (r − b == a), que
 *     reemplaza la prueba de exponentes y al TwoSum (prueba en la sección
 *     «FPU residente» de docs/rec-ppe.md); D: lo mismo para sumas, residuo
 *     por fma para producto, cociente y raíz, como cop1.c.
 *   - FCR31 perezoso: C, la causa I de la última operación, la bandera I
 *     acumulada y «RM ≠ 0 o I habilitada» viven en bits del CR (cr2, no
 *     volátil: sobrevive a las llamadas a C) y se escriben en cpu->fcr31 al
 *     salir del bloque, antes de que el intérprete corra una instrucción de
 *     COP1 y en CFC1. Cada operación cuesta uno o dos crops en vez de
 *     cargar, modificar y guardar FCR31.
 *   - Un resultado fuera de lo que el camino nativo cubre va al intérprete
 *     por una llamada a C (como antes), y vuelve con fd recargado.
 */

/* Formato con que un registro del PPE tiene el valor de cpu->fpr[p]. */
enum { FR_NONE, FR_S, FR_D };

/* Bits de cr2 y cr3.3: el estado perezoso de FCR31. */
enum {
	FCR_C = PPC_BI(2, 0),     /* bit C de FCR31 (válido si fneed_c o fc_written) */
	FCR_IC = PPC_BI(2, 1),    /* causa I de la última operación nativa (válido si fpu_ran) */
	FCR_IS = PPC_BI(2, 2),    /* bandera I acumulada desde la última escritura de FCR31 */
	FCR_GATE = PPC_BI(2, 3),  /* RM ≠ 0 o I habilitada: la operación va al intérprete */
	FCR_IEN = PPC_BI(3, 3),   /* I habilitada (ROUND/TRUNC/CEIL/FLOOR no miran RM) */
};

/* Registros de punto flotante del PPE. */
enum {
	F_R = 0, F_T1 = 1, F_T2 = 2, F_T3 = 3, F_TA = 4, F_TB = 5,  /* temporales de una operación */
	F_OPA = 6, F_OPB = 7,                                        /* operandos sin registro del bloque */
	F_RES0 = 14, F_RESN = 12,                                    /* f14–f25: cpu->fpr[p] residentes */
	F_ONE = 26, F_ZERO = 27, F_LO_S = 28, F_HI_S = 29, F_LO_D = 30, F_HI_D = 31,  /* constantes (rec.c) */
};

/* CR de trabajo: bits de cr3 y los campos cr5–cr7 (cr4 es la condición del
 * salto; cr0 y cr1 los usan otras cosas, pero dentro de una operación son
 * libres). */
enum { CRF_A = PPC_BI(3, 0), CRF_B = PPC_BI(3, 1), CRF_X = PPC_BI(3, 2), CR_FA = 5, CR_FB = 6, CR_FZ = 7 };

static int32_t fhome(unsigned p) { return OFF(fpr) + 8 * (int32_t)p; }
static int32_t fhome_lo(unsigned p) { return OFF(fpr) + 8 * (int32_t)p + 4; }
static int32_t fhome_w(unsigned p) { return OFF(rec_fw) + 8 * (int32_t)p; }

/* Ranura (cpu->fpr[p]) de un acceso de 32 bits al registro r; *hi: con FR = 0
 * e índice impar es la mitad alta de la pareja (nunca residente). */
static unsigned fslot32(const struct ctx *c, unsigned r, bool *hi)
{
	if (c->rec->fr || !(r & 1)) {
		*hi = false;
		return r;
	}
	*hi = true;
	return r & ~1u;
}

static unsigned fslot64(const struct ctx *c, unsigned r)
{
	return c->rec->fr ? r : r & ~1u;
}

/* ---- Qué toca cada instrucción ------------------------------------------------- */

/* Accesos de una instrucción a cpu->fpr: hasta dos lecturas y una escritura,
 * con el formato en que el valor se usa (FR_NONE: bits, mitad alta o
 * entero). -1: ninguno. */
struct finfo {
	int8_t ra, rb, wd;
	uint8_t fa, fb, fw;
	bool hi;      /* la escritura (o la lectura) es la mitad alta de la pareja */
};

static void fpu_info(const struct ctx *c, uint32_t i, struct finfo *f)
{
	unsigned op = I_OP(i), fmt = I_FMT(i), funct = I_FUNCT(i), rt = I_RT(i);
	bool hi = false;
	f->ra = f->rb = f->wd = -1;
	f->fa = f->fb = f->fw = FR_NONE;
	f->hi = false;
	switch (op) {
	case 0x31: f->wd = (int8_t)fslot32(c, rt, &hi); f->hi = hi; return;          /* LWC1 */
	case 0x35: f->wd = (int8_t)fslot64(c, rt); return;                            /* LDC1 */
	case 0x39: f->ra = (int8_t)fslot32(c, rt, &hi); f->fa = hi ? FR_NONE : FR_S; f->hi = hi; return;  /* SWC1 */
	case 0x3D: f->ra = (int8_t)fslot64(c, rt); f->fa = FR_D; return;              /* SDC1 */
	case 0x11: break;
	default: return;
	}
	unsigned fs = I_FS(i), fsr = fslot64(c, fs), ft = I_FT(i), fd = I_FD(i);
	switch (fmt) {
	case 0x00: f->ra = (int8_t)fslot32(c, fs, &hi); f->hi = hi; return;   /* MFC1 */
	case 0x01: f->ra = (int8_t)fslot64(c, fs); return;                     /* DMFC1 */
	case 0x04: f->wd = (int8_t)fslot32(c, fs, &hi); f->hi = hi; return;   /* MTC1 */
	case 0x05: f->wd = (int8_t)fslot64(c, fs); return;                     /* DMTC1 */
	case FMT_S: case FMT_D: {
		unsigned ff = fmt == FMT_S ? FR_S : FR_D;
		if (funct <= 0x03 || funct >= 0x30) {                           /* ADD SUB MUL DIV, C.cond */
			f->ra = (int8_t)fsr; f->fa = ff;
			f->rb = (int8_t)ft; f->fb = ff;
			if (funct <= 0x03) { f->wd = (int8_t)fd; f->fw = ff; }
		} else if (funct == 0x04 || funct == 0x05 || funct == 0x07) {   /* SQRT ABS NEG */
			f->ra = (int8_t)fsr; f->fa = ff; f->wd = (int8_t)fd; f->fw = ff;
		} else if (funct == 0x06) {                                     /* MOV: los 64 bits */
			f->ra = (int8_t)fsr; f->wd = (int8_t)fd;
		} else if (funct >= 0x08 && funct <= 0x0F) {                    /* a W y L */
			f->ra = (int8_t)fsr; f->fa = ff; f->wd = (int8_t)fd;
		} else if (funct == 0x20 || funct == 0x21) {                    /* CVT.S.D CVT.D.S */
			f->ra = (int8_t)fsr; f->fa = ff; f->wd = (int8_t)fd; f->fw = funct == 0x20 ? FR_S : FR_D;
		} else if (funct == 0x24 || funct == 0x25) {                    /* CVT.W CVT.L */
			f->ra = (int8_t)fsr; f->fa = ff; f->wd = (int8_t)fd;
		}
		return;
	}
	case FMT_W: case FMT_L:
		if (funct == 0x20 || funct == 0x21) {                           /* CVT.S CVT.D desde entero */
			f->ra = (int8_t)fsr; f->wd = (int8_t)fd; f->fw = funct == 0x20 ? FR_S : FR_D;
		}
		return;
	default:
		return;
	}
}

/* ¿Alguna instrucción después de k convierte la palabra W de p (CVT.S.W o
 * CVT.D.W) antes de que otra escriba p? Entonces quien escribe p deja la
 * palabra con signo extendido en cpu->rec_fw[p], para que la conversión la
 * cargue con lfd (fcfid) sin pasar la palabra por un registro entero. */
static bool fpu_w_wanted(const struct ctx *c, unsigned k, unsigned p)
{
	for (unsigned j = k + 1; j < c->n; j++) {
		uint32_t w = c->d[j].w;
		struct finfo f;
		if (c->d[j].how != H_NATIVE)
			continue;
		fpu_info(c, w, &f);
		if (I_OP(w) == 0x11 && I_FMT(w) == FMT_W && f.ra == (int)p)
			return true;
		if (f.wd == (int)p)
			return false;
	}
	return false;
}

/* Formato del próximo uso de p después de k (FR_NONE si no hay, si se
 * escribe antes o si el uso no es de punto flotante). */
static unsigned fpu_next_use(const struct ctx *c, unsigned k, unsigned p)
{
	for (unsigned j = k + 1; j < c->n; j++) {
		struct finfo f;
		if (c->d[j].how != H_NATIVE)
			continue;
		fpu_info(c, c->d[j].w, &f);
		if (f.ra == (int)p)
			return f.fa;
		if (f.rb == (int)p)
			return f.fb;
		if (f.wd == (int)p)
			return FR_NONE;
	}
	return FR_NONE;
}

/* ---- Registros --------------------------------------------------------------- */

/* El valor de cpu->fpr[p] en formato fmt en un registro del PPE: el del
 * bloque (cargándolo de casa si tenía otro formato), o `tmp`. */
static unsigned fpu_operand(struct ctx *c, unsigned p, unsigned fmt, unsigned tmp)
{
	unsigned f = c->fmap[p] ? c->fmap[p] : tmp;
	if (!c->fmap[p] || c->ffmt[p] != fmt) {
		E(c, fmt == FR_S ? ppc_lfs(f, fhome_lo(p), R_CPU) : ppc_lfd(f, fhome(p), R_CPU));
		if (c->fmap[p])
			c->ffmt[p] = fmt;
	}
	return f;
}

static unsigned fpu_dest(const struct ctx *c, unsigned p, unsigned tmp)
{
	return c->fmap[p] ? c->fmap[p] : tmp;
}

/* El resultado de formato fmt para cpu->fpr[p] quedó en `f`: a casa (en S,
 * los 32 bits altos en 0, como cop1.c) y anotar lo que tiene el registro. */
static void fpu_result(struct ctx *c, unsigned p, unsigned f, unsigned fmt)
{
	if (fmt == FR_S) {
		E(c, ppc_stfs(f, fhome_lo(p), R_CPU));
		E(c, ppc_li(0, 0));
		E(c, ppc_stw(0, fhome(p), R_CPU));
	} else {
		E(c, ppc_stfd(f, fhome(p), R_CPU));
	}
	c->ffmt[p] = c->fmap[p] ? fmt : FR_NONE;
	c->fw_ok[p] = 0;
}

/* Se escribió cpu->fpr[p] por fuera de la FPU (sin valor en el registro). */
static void fpu_invalidate(struct ctx *c, unsigned p, bool hi)
{
	if (!hi || c->ffmt[p] == FR_D)
		c->ffmt[p] = FR_NONE;
	c->fw_ok[p] = 0;
}

/* Recarga el registro del bloque de p en formato fmt (tras el intérprete). */
static void fpu_reload(struct ctx *c, int p, unsigned fmt)
{
	if (p < 0 || !c->fmap[p] || fmt == FR_NONE)
		return;
	E(c, fmt == FR_S ? ppc_lfs(c->fmap[p], fhome_lo(p), R_CPU) : ppc_lfd(c->fmap[p], fhome(p), R_CPU));
	c->ffmt[p] = fmt;
}

/* ---- FCR31 perezoso ------------------------------------------------------------ */

/* Deriva de la palabra en r0 (FCR31) los bits que el bloque necesita. */
static void fpu_derive(struct ctx *c, bool gate, bool ien, bool cbit)
{
	if (gate) {
		E(c, ppc_li(T_X, 0x83));
		E(c, ppc_and(T_X, 0, T_X));
		E(c, ppc_cmpwi(CR_T, T_X, 0));
		E(c, ppc_crnot(FCR_GATE, PPC_BI(CR_T, PPC_EQ)));
	}
	if (ien) {
		E(c, ppc_rlwinm(T_X, 0, 25, 31, 31));  /* bit 7 */
		E(c, ppc_cmpwi(CR_T, T_X, 0));
		E(c, ppc_crnot(FCR_IEN, PPC_BI(CR_T, PPC_EQ)));
	}
	if (cbit) {
		E(c, ppc_rlwinm(T_X, 0, 9, 31, 31));   /* bit 23 */
		E(c, ppc_cmpwi(CR_T, T_X, 0));
		E(c, ppc_crnot(FCR_C, PPC_BI(CR_T, PPC_EQ)));
	}
}

/* A la entrada del bloque (después de las pruebas de la entrada, antes del
 * cuerpo): los bits que hacen falta y los registros vivos a la entrada. */
static void fpu_entry(struct ctx *c)
{
	if (c->fneed_gate || c->fneed_ien || c->fneed_c) {
		E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
		fpu_derive(c, c->fneed_gate, c->fneed_ien, c->fneed_c);
	}
	E(c, ppc_crclr(FCR_IS));
	for (unsigned p = 0; p < 32; p++) {
		if (c->fmap[p] && c->flive[p] != FR_NONE) {
			E(c, c->flive[p] == FR_S ? ppc_lfs(c->fmap[p], fhome_lo(p), R_CPU)
						 : ppc_lfd(c->fmap[p], fhome(p), R_CPU));
			c->ffmt[p] = c->flive[p];
		}
	}
}

/* Escribe en cpu->fcr31 lo pendiente: con `ran`, las causas son la de la
 * última operación (cr2.1) y la bandera I acumulada se suma (cr2.2); con
 * `cbit`, C es cr2.0. Lo demás de FCR31 no cambia en nativo. Usa r0. */
static void fpu_flush_bits(struct ctx *c, bool ran, bool cbit)
{
	if (!ran && !cbit)
		return;
	E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
	if (ran) {
		int l1 = new_label(c), l2 = new_label(c);
		E(c, ppc_rlwinm(0, 0, 0, 20, 13));  /* causas (bits 12–17) en 0 */
		emit_bc(c, PPC_BO_FALSE, FCR_IC, l1);
		E(c, ppc_ori(0, 0, 0x1000));
		bind(c, l1);
		emit_bc(c, PPC_BO_FALSE, FCR_IS, l2);
		E(c, ppc_ori(0, 0, 0x0004));
		bind(c, l2);
	}
	if (cbit) {
		int l3 = new_label(c);
		E(c, ppc_rlwinm(0, 0, 0, 9, 7));    /* C (bit 23) en 0 */
		emit_bc(c, PPC_BO_FALSE, FCR_C, l3);
		E(c, ppc_oris(0, 0, 0x0080));
		bind(c, l3);
	}
	E(c, ppc_stw(0, OFF(fcr31), R_CPU));
}

/* Lo pendiente a memoria sin cambiar el estado (para una salida). */
static void fpu_flush(struct ctx *c)
{
	if (c->fpu_res)
		fpu_flush_bits(c, c->fpu_ran, c->fc_written);
}

/* Lo pendiente a memoria y el estado limpio: antes de que el intérprete lea
 * FCR31 y en la vuelta de un bucle. C y la compuerta siguen valiendo. */
static void fpu_flush_reset(struct ctx *c)
{
	if (!c->fpu_res)
		return;
	fpu_flush_bits(c, c->fpu_ran, c->fc_written);
	if (c->fpu_ran)
		E(c, ppc_crclr(FCR_IS));
	c->fpu_ran = false;
	c->fc_written = false;
}

/* Después de que el intérprete corrió una instrucción de COP1 (cpu->fcr31
 * es la verdad): si dejó una causa que no es I (Z, V, O, U sin excepción,
 * raro) el bloque sale (el estado perezoso no la representa); si no, la
 * causa I y C se vuelven a derivar y la bandera acumulada queda en 0 (ya
 * está en memoria). La compuerta también (CTC1 por el intérprete). */
static void fpu_after_cop1_call(struct ctx *c, unsigned k)
{
	int ok = new_label(c);
	E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
	E(c, ppc_rlwinm(T_X, 0, 0, 14, 18));  /* causas U O Z V E (bits 13–17) */
	E(c, ppc_cmpwi(CR_T, T_X, 0));
	emit_bc(c, PPC_BO_TRUE_LIKELY, PPC_BI(CR_T, PPC_EQ), ok);
	/* Salida después de k con la memoria como verdad (sin escribir FCR31). */
	writeback(c, c->written_start | (c->d[k].writes & c->cached));
	E(c, ppc_addi(R_CYCLES, R_CYCLES, (int32_t)k + 1));
	emit_continue(c, k);
	restore_borrowed(c);
	emit_b(c, abs_label(c, c->rec->exit_mid), false);
	bind(c, ok);
	E(c, ppc_rlwinm(T_X, 0, 20, 31, 31));  /* causa I (bit 12) */
	E(c, ppc_cmpwi(CR_T, T_X, 0));
	E(c, ppc_crnot(FCR_IC, PPC_BI(CR_T, PPC_EQ)));
	E(c, ppc_crclr(FCR_IS));
	fpu_derive(c, c->fneed_gate, c->fneed_ien, c->fneed_c || c->fc_written);
	c->fpu_ran = true;  /* las causas en memoria son 0 o I: cr2.1 las representa */
}

/* Camino lento de la instrucción k de COP1 (sección fría): FCR31 a memoria
 * (con el estado de antes de la operación: ran0, cw0; a él se llega antes de
 * que el camino rápido toque el CR), el intérprete, revisión de la salida,
 * el estado derivado de nuevo y fd recargado en el formato que deja el
 * camino rápido. Vuelve a `resume` con el mismo estado que el rápido. */
static void fpu_slow(struct ctx *c, unsigned k, int slow, int resume, int reload_p, unsigned reload_fmt,
		     bool ran0, bool cw0)
{
	unsigned sec = c->sec;
	bool ran1 = c->fpu_ran, cw1 = c->fc_written;
	c->sec = SEC_COLD;
	bind(c, slow);
	fpu_flush_bits(c, ran0, cw0);
	emit_exec_call(c, k);
	post_check_mem(c, k);
	fpu_after_cop1_call(c, k);  /* deja fpu_ran; deriva C si el rápido lo escribió */
	fpu_reload(c, reload_p, reload_fmt);
	emit_b(c, resume, false);
	c->sec = sec;
	c->fpu_ran = ran1;
	c->fc_written = cw1;
}

/* ---- Clasificación ---------------------------------------------------------- */

/* bit = (|f| ≥ LO ∧ |f| < HI) ∨ f == 0 (la franja segura de fpu_fast_band);
 * NaN e infinito dan 0. `tmp` recibe |f|. Las comparaciones se emiten
 * seguidas: en el PPE el bit del CR de fcmpu está al ciclo siguiente. */
static void fpu_band(struct ctx *c, unsigned f, unsigned tmp, bool is_s, unsigned bit)
{
	E(c, ppc_fabs(tmp, f));
	E(c, ppc_fcmpu(CR_FA, tmp, is_s ? F_LO_S : F_LO_D));
	E(c, ppc_fcmpu(CR_FB, tmp, is_s ? F_HI_S : F_HI_D));
	E(c, ppc_fcmpu(CR_FZ, f, F_ZERO));
	E(c, ppc_crandc(bit, PPC_BI(CR_FB, PPC_LT), PPC_BI(CR_FA, PPC_LT)));
	E(c, ppc_cror(bit, bit, PPC_BI(CR_FZ, PPC_EQ)));
}

/* Los dos operandos: CRF_A = los dos en la franja; deja cr7 eq = (fb == 0). */
static void fpu_band2(struct ctx *c, unsigned fa, unsigned fb, bool is_s)
{
	E(c, ppc_fabs(F_TA, fa));
	E(c, ppc_fabs(F_TB, fb));
	E(c, ppc_fcmpu(CR_FA, F_TA, is_s ? F_LO_S : F_LO_D));
	E(c, ppc_fcmpu(CR_FB, F_TA, is_s ? F_HI_S : F_HI_D));
	E(c, ppc_fcmpu(CR_FZ, fa, F_ZERO));
	E(c, ppc_crandc(CRF_A, PPC_BI(CR_FB, PPC_LT), PPC_BI(CR_FA, PPC_LT)));
	E(c, ppc_cror(CRF_A, CRF_A, PPC_BI(CR_FZ, PPC_EQ)));
	E(c, ppc_fcmpu(CR_FA, F_TB, is_s ? F_LO_S : F_LO_D));
	E(c, ppc_fcmpu(CR_FB, F_TB, is_s ? F_HI_S : F_HI_D));
	E(c, ppc_fcmpu(CR_FZ, fb, F_ZERO));
	E(c, ppc_crandc(CRF_B, PPC_BI(CR_FB, PPC_LT), PPC_BI(CR_FA, PPC_LT)));
	E(c, ppc_cror(CRF_B, CRF_B, PPC_BI(CR_FZ, PPC_EQ)));
	E(c, ppc_crand(CRF_A, CRF_A, CRF_B));
}

/* La operación dejó en el bit `exact` si fue exacta: causa I de esta
 * operación y bandera acumulada. */
static void fpu_inexact_from(struct ctx *c, unsigned exact)
{
	E(c, ppc_crnot(FCR_IC, exact));
	E(c, ppc_crorc(FCR_IS, FCR_IS, exact));
	c->fpu_ran = true;
}

/* La operación no tiene causa (ABS, NEG, MOV no entra aquí, C.cond, CVT.D.S,
 * CVT.D.W): causas en 0. */
static void fpu_no_cause(struct ctx *c)
{
	E(c, ppc_crclr(FCR_IC));
	c->fpu_ran = true;
}

/* ---- Aritmética: ADD SUB MUL DIV, SQRT, ABS, NEG, MOV, C.cond --------------------- */

static void emit_fpu_res(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	bool is_s = I_FMT(i) == FMT_S, ran0 = c->fpu_ran, cw0 = c->fc_written;
	unsigned fmt = is_s ? FR_S : FR_D, funct = I_FUNCT(i);
	unsigned pa = fslot64(c, I_FS(i)), pb = I_FT(i), pd = I_FD(i);
	int slow = new_label(c), resume = new_label(c);

	if (funct == 0x06) {  /* MOV: los 64 bits de casa, sin tocar FCR31 */
		unsigned fd = fpu_dest(c, pd, 0);
		if (c->fmap[pa] && c->ffmt[pa] == FR_D) {
			if (fd && fd != c->fmap[pa])
				E(c, ppc_fmr(fd, c->fmap[pa]));
			if (fd)
				E(c, ppc_stfd(fd, fhome(pd), R_CPU));
			else {
				E(c, ppc_stfd(c->fmap[pa], fhome(pd), R_CPU));
			}
			c->ffmt[pd] = fd ? FR_D : FR_NONE;
		} else {
			E(c, ppc_ld(0, fhome(pa), R_CPU));
			E(c, ppc_std(0, fhome(pd), R_CPU));
			if (fd && c->fmap[pa] && c->ffmt[pa] == FR_S) {
				if (fd != c->fmap[pa])
					E(c, ppc_fmr(fd, c->fmap[pa]));
				c->ffmt[pd] = FR_S;
			} else {
				c->ffmt[pd] = FR_NONE;
			}
		}
		c->fw_ok[pd] = 0;
		return;
	}

	if (funct >= 0x30) {  /* C.cond: NaN al intérprete; C a cr2.0, causas en 0 */
		unsigned cond = funct & 15;
		unsigned fa = fpu_operand(c, pa, fmt, F_OPA), fb = fpu_operand(c, pb, fmt, F_OPB);
		E(c, ppc_fcmpu(CR_FA, fa, fb));
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, PPC_BI(CR_FA, PPC_SO), slow);
		if ((cond & 6) == 6)
			E(c, ppc_cror(FCR_C, PPC_BI(CR_FA, PPC_LT), PPC_BI(CR_FA, PPC_EQ)));
		else if (cond & 4)
			E(c, ppc_crmove(FCR_C, PPC_BI(CR_FA, PPC_LT)));
		else if (cond & 2)
			E(c, ppc_crmove(FCR_C, PPC_BI(CR_FA, PPC_EQ)));
		else
			E(c, ppc_crclr(FCR_C));
		fpu_no_cause(c);
		c->fc_written = true;
		bind(c, resume);
		fpu_slow(c, k, slow, resume, -1, FR_NONE, ran0, cw0);
		return;
	}

	if (funct == 0x05 || funct == 0x07) {  /* ABS NEG: en la franja o cero, sin causa */
		unsigned fa = fpu_operand(c, pa, fmt, F_OPA), fd = fpu_dest(c, pd, F_R);
		fpu_band(c, fa, F_TA, is_s, CRF_A);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
		E(c, funct == 0x05 ? ppc_fabs(fd, fa) : ppc_fneg(fd, fa));
		fpu_result(c, pd, fd, fmt);
		fpu_no_cause(c);
		bind(c, resume);
		fpu_slow(c, k, slow, resume, (int)pd, fmt, ran0, cw0);
		return;
	}

	if (funct == 0x04) {  /* SQRT: positivo en la franja o ±0, redondeo al más cercano, I deshabilitada */
		unsigned fa = fpu_operand(c, pa, fmt, F_OPA), fd = fpu_dest(c, pd, F_T1);
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, FCR_GATE, slow);
		fpu_band(c, fa, F_TA, is_s, CRF_A);
		E(c, ppc_crandc(CRF_A, CRF_A, PPC_BI(CR_FZ, PPC_LT)));  /* negativo: V */
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
		E(c, ppc_fsqrt(F_R, fa));
		E(c, ppc_fnmsub(F_T2, F_R, F_R, fa));  /* a - r·r */
		if (is_s) {
			E(c, ppc_frsp(F_T1, F_R));
			E(c, ppc_fcmpu(CR_FA, F_T1, F_R));
			E(c, ppc_fcmpu(CR_FB, F_T2, F_ZERO));
			E(c, ppc_crand(CRF_X, PPC_BI(CR_FA, PPC_EQ), PPC_BI(CR_FB, PPC_EQ)));
			if (fd != F_T1)
				E(c, ppc_fmr(fd, F_T1));
		} else {
			E(c, ppc_fcmpu(CR_FB, F_T2, F_ZERO));
			E(c, ppc_crmove(CRF_X, PPC_BI(CR_FB, PPC_EQ)));
			E(c, ppc_fmr(fd, F_R));
		}
		fpu_result(c, pd, fd, fmt);
		fpu_inexact_from(c, CRF_X);
		bind(c, resume);
		fpu_slow(c, k, slow, resume, (int)pd, fmt, ran0, cw0);
		return;
	}

	/* ADD SUB MUL DIV (fpu_fast_arith): redondeo al más cercano, I
	 * deshabilitada, operandos en la franja o cero, divisor distinto de 0. */
	unsigned fa = fpu_operand(c, pa, fmt, F_OPA), fb = fpu_operand(c, pb, fmt, F_OPB);
	unsigned fd = fpu_dest(c, pd, F_T1);
	/* Si fd es un operando, las comparaciones de después leerían el
	 * resultado en vez del operando: el resultado va a F_T1 y se copia al
	 * final (en D, F_R se copia al final siempre). */
	unsigned fr = (fd == fa || fd == fb) ? F_T1 : fd;
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, FCR_GATE, slow);
	fpu_band2(c, fa, fb, is_s);
	if (funct == 0x03)
		E(c, ppc_crandc(CRF_A, CRF_A, PPC_BI(CR_FZ, PPC_EQ)));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
	switch (funct) {
	case 0x00: E(c, ppc_fadd(F_R, fa, fb)); break;
	case 0x01: E(c, ppc_fsub(F_R, fa, fb)); break;  /* = a + (-b), bit a bit */
	case 0x02: E(c, ppc_fmul(F_R, fa, fb)); break;
	default:   E(c, ppc_fdiv(F_R, fa, fb)); break;
	}
	/* Residuo o diferencias (leen los operandos antes de escribir fd). */
	if (funct <= 0x01) {
		E(c, ppc_fsub(F_T2, F_R, fa));                                   /* r - a (ADD) */
		if (funct == 0x00) {
			E(c, ppc_fsub(F_T3, F_R, fb));                           /* r - b */
		} else {
			E(c, ppc_fneg(F_T2, F_T2));                              /* a - r == b ⟺ r - a == -b */
			E(c, ppc_fadd(F_T3, F_R, fb));                           /* r + b == a */
		}
	} else if (funct == 0x02 && !is_s) {
		E(c, ppc_fmsub(F_T2, fa, fb, F_R));                              /* a·b - r */
	} else if (funct == 0x03) {
		E(c, ppc_fnmsub(F_T2, F_R, fb, fa));                             /* a - r·b */
	}
	if (is_s) {
		E(c, ppc_frsp(fr, F_R));
		E(c, ppc_fcmpu(CR_FA, fr, F_R));
	}
	/* CRF_X = exacto. */
	if (funct <= 0x01) {
		E(c, ppc_fcmpu(CR_FB, F_T2, fb));
		E(c, ppc_fcmpu(CR_FZ, F_T3, fa));
		E(c, ppc_crand(CRF_X, PPC_BI(CR_FB, PPC_EQ), PPC_BI(CR_FZ, PPC_EQ)));
		if (is_s)
			E(c, ppc_crand(CRF_X, CRF_X, PPC_BI(CR_FA, PPC_EQ)));
	} else if (funct == 0x02) {
		if (is_s) {
			E(c, ppc_crmove(CRF_X, PPC_BI(CR_FA, PPC_EQ)));  /* el producto de dos float es exacto en double */
		} else {
			E(c, ppc_fcmpu(CR_FB, F_T2, F_ZERO));
			E(c, ppc_crmove(CRF_X, PPC_BI(CR_FB, PPC_EQ)));
		}
	} else {
		E(c, ppc_fcmpu(CR_FB, F_T2, F_ZERO));
		E(c, ppc_crmove(CRF_X, PPC_BI(CR_FB, PPC_EQ)));
		if (is_s)
			E(c, ppc_crand(CRF_X, CRF_X, PPC_BI(CR_FA, PPC_EQ)));
	}
	if (!is_s)
		E(c, ppc_fmr(fd, F_R));
	else if (fr != fd)
		E(c, ppc_fmr(fd, fr));
	fpu_result(c, pd, fd, fmt);
	fpu_inexact_from(c, CRF_X);
	bind(c, resume);
	fpu_slow(c, k, slow, resume, (int)pd, fmt, ran0, cw0);
}

/* ---- Conversiones ------------------------------------------------------------ */

static const double k_fpu_lim[3] = { 0x1p31, 0x1p30, 0x1p53 };  /* W desde S, W desde D, L */

/* CVT.S.D, CVT.D.S; CVT.S.W, CVT.D.W (fcfid); ROUND/TRUNC/CEIL/FLOOR/CVT a W
 * y L (fctid). Mismas reglas que rec_emit_ext.h, con los operandos en
 * registros. */
static void emit_fpu_cvt_res(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned fmt = I_FMT(i), funct = I_FUNCT(i), pa = fslot64(c, I_FS(i)), pd = I_FD(i);
	bool ran0 = c->fpu_ran, cw0 = c->fc_written;
	int slow = new_label(c), resume = new_label(c);

	if (fmt == FMT_W) {  /* CVT.S.W CVT.D.W: la palabra con signo a double con fcfid */
		bool to_s = funct == 0x20;
		unsigned fd = fpu_dest(c, pd, F_T1);
		if (to_s)
			emit_bc(c, PPC_BO_TRUE_UNLIKELY, FCR_GATE, slow);
		if (!c->fw_ok[pa]) {
			E(c, ppc_lwz(T_X, fhome_lo(pa), R_CPU));
			E(c, ppc_extsw(T_X, T_X));
			E(c, ppc_std(T_X, fhome_w(pa), R_CPU));
		}
		E(c, ppc_lfd(F_T2, fhome_w(pa), R_CPU));
		if (to_s) {
			E(c, ppc_fcfid(F_R, F_T2));
			E(c, ppc_frsp(fd, F_R));
			E(c, ppc_fcmpu(CR_FA, fd, F_R));
			fpu_result(c, pd, fd, FR_S);
			fpu_inexact_from(c, PPC_BI(CR_FA, PPC_EQ));
		} else {
			E(c, ppc_fcfid(fd, F_T2));  /* exacto */
			fpu_result(c, pd, fd, FR_D);
			fpu_no_cause(c);
		}
		bind(c, resume);
		fpu_slow(c, k, slow, resume, (int)pd, to_s ? FR_S : FR_D, ran0, cw0);
		return;
	}

	bool is_s = fmt == FMT_S;
	unsigned sfmt = is_s ? FR_S : FR_D;
	if (funct == 0x21) {  /* CVT.D.S: el double del float, exacto y sin causa */
		unsigned fa = fpu_operand(c, pa, FR_S, F_OPA), fd = fpu_dest(c, pd, F_R);
		fpu_band(c, fa, F_TA, true, CRF_A);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
		if (fd != fa)
			E(c, ppc_fmr(fd, fa));
		fpu_result(c, pd, fd, FR_D);
		fpu_no_cause(c);
		bind(c, resume);
		fpu_slow(c, k, slow, resume, (int)pd, FR_D, ran0, cw0);
		return;
	}
	if (funct == 0x20) {  /* CVT.S.D: en la franja de S (ni desborda ni queda diminuto), frsp */
		unsigned fa = fpu_operand(c, pa, FR_D, F_OPA), fd = fpu_dest(c, pd, F_T1);
		unsigned fr = fd == fa ? F_T1 : fd;  /* fd = fs: no comparar el resultado consigo mismo */
		emit_bc(c, PPC_BO_TRUE_UNLIKELY, FCR_GATE, slow);
		fpu_band(c, fa, F_TA, true, CRF_A);
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
		E(c, ppc_frsp(fr, fa));
		E(c, ppc_fcmpu(CR_FA, fr, fa));
		if (fr != fd)
			E(c, ppc_fmr(fd, fr));
		fpu_result(c, pd, fd, FR_S);
		fpu_inexact_from(c, PPC_BI(CR_FA, PPC_EQ));
		bind(c, resume);
		fpu_slow(c, k, slow, resume, (int)pd, FR_S, ran0, cw0);
		return;
	}

	/* A entero: solo |x| en la franja y bajo el límite con que ningún modo
	 * se sale del rango (W: 2^31 en S, 2^30 en D; L: 2^53), o ±0. fctid usa
	 * el modo del FPSCR (al más cercano par); fctidz trunca; CEIL y FLOOR
	 * corrigen en 1. Inexacto: el entero vuelto a double difiere. */
	bool cvt = funct >= 0x24, to64 = cvt ? funct == 0x25 : funct < 0x0C;
	unsigned mode = cvt ? 0 : funct & 3;
	unsigned fa = fpu_operand(c, pa, sfmt, F_OPA);
	emit_bc(c, PPC_BO_TRUE_UNLIKELY, cvt ? FCR_GATE : FCR_IEN, slow);
	li64(c, T_Y, (uintptr_t)&k_fpu_lim[to64 ? 2 : is_s ? 0 : 1]);
	E(c, ppc_lfd(F_T3, 0, T_Y));
	E(c, ppc_fabs(F_TA, fa));
	E(c, ppc_fcmpu(CR_FA, F_TA, is_s ? F_LO_S : F_LO_D));
	E(c, ppc_fcmpu(CR_FB, F_TA, F_T3));
	E(c, ppc_fcmpu(CR_FZ, fa, F_ZERO));
	E(c, ppc_crandc(CRF_A, PPC_BI(CR_FB, PPC_LT), PPC_BI(CR_FA, PPC_LT)));
	E(c, ppc_cror(CRF_A, CRF_A, PPC_BI(CR_FZ, PPC_EQ)));
	emit_bc(c, PPC_BO_FALSE_UNLIKELY, CRF_A, slow);
	E(c, mode == 0 ? ppc_fctid(F_R, fa) : ppc_fctidz(F_R, fa));
	E(c, ppc_fcfid(F_T1, F_R));
	E(c, ppc_fcmpu(CR_FA, F_T1, fa));
	E(c, ppc_crmove(CRF_X, PPC_BI(CR_FA, PPC_EQ)));
	if (mode >= 2) {
		/* CEIL: truncado < x, +1; FLOOR: truncado > x, -1 (exacto). */
		int done = new_label(c);
		emit_bc(c, PPC_BO_FALSE, PPC_BI(CR_FA, mode == 2 ? PPC_LT : PPC_GT), done);
		E(c, mode == 2 ? ppc_fadd(F_T1, F_T1, F_ONE) : ppc_fsub(F_T1, F_T1, F_ONE));
		E(c, ppc_fctidz(F_R, F_T1));
		bind(c, done);
	}
	E(c, ppc_stfd(F_R, fhome(pd), R_CPU));
	if (!to64) {  /* W: los 32 bits altos en 0 */
		E(c, ppc_li(0, 0));
		E(c, ppc_stw(0, fhome(pd), R_CPU));
	}
	c->ffmt[pd] = FR_NONE;
	c->fw_ok[pd] = 0;
	if (!to64 && fpu_w_wanted(c, k, pd)) {
		E(c, ppc_stfd(F_R, fhome_w(pd), R_CPU));  /* el entero de 64 bits ya tiene el signo extendido */
		c->fw_ok[pd] = 1;
	}
	fpu_inexact_from(c, CRF_X);
	bind(c, resume);
	fpu_slow(c, k, slow, resume, -1, FR_NONE, ran0, cw0);
}

/* ---- MFC1, MTC1, DMFC1, DMTC1, CFC1, CTC1 ----------------------------------------- */

static void emit_cop1_res(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned rt = I_RT(i), fs = I_FS(i), d, v, p;
	bool hi;
	switch (I_RS(i)) {
	case 0x00:                                                        /* MFC1: de casa */
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
		p = fslot32(c, fs, &hi);
		E(c, ppc_stw(v, fpr32_off(c, fs), R_CPU));
		fpu_invalidate(c, p, hi);
		if (!hi && fpu_w_wanted(c, k, p)) {
			E(c, ppc_extsw(T_X, v));
			E(c, ppc_std(T_X, fhome_w(p), R_CPU));
			c->fw_ok[p] = 1;
		}
		return;
	case 0x05:                                                        /* DMTC1 */
		v = get(c, rt, T_S1);
		p = fslot64(c, fs);
		E(c, ppc_std(v, fpr64_off(c, fs), R_CPU));
		fpu_invalidate(c, p, false);
		return;
	case 0x02:                                                        /* CFC1: FCR31 con lo pendiente */
		d = dst(c, rt, T_V);
		if (fs == 31) {
			if (c->fpu_ran || c->fc_written) {
				fpu_flush_reset(c);  /* deja la palabra en r0 (y en memoria) */
				E(c, ppc_extsw(d, 0));
			} else {
				E(c, ppc_lwz(T_X, OFF(fcr31), R_CPU));
				E(c, ppc_extsw(d, T_X));
			}
		} else {
			E(c, ppc_li(d, fs == 0 ? SALSA64_FCR0_VALUE : 0));
		}
		put(c, rt, d);
		return;
	case 0x06: {                                                      /* CTC1 */
		if (fs != 31)
			return;
		int slow = new_label(c), resume = new_label(c);
		v = get(c, rt, T_S1);
		E(c, ppc_lis(T_X, 0x0183));
		E(c, ppc_ori(T_X, T_X, 0xFFFF));
		E(c, ppc_and(T_X, v, T_X));                  /* bits escribibles (cop1.c) */
		E(c, ppc_rlwinm(T_Y, T_X, 25, 27, 31));      /* habilitaciones */
		E(c, ppc_ori(T_Y, T_Y, 0x20));
		E(c, ppc_rlwinm(0, T_X, 20, 26, 31));        /* causas */
		E(c, ppc_and(0, 0, T_Y));
		E(c, ppc_cmpwi(CR_T, 0, 0));
		emit_bc(c, PPC_BO_FALSE_UNLIKELY, PPC_BI(CR_T, PPC_EQ), slow);  /* una causa habilitada: la lanza el intérprete */
		E(c, ppc_stw(T_X, OFF(fcr31), R_CPU));
		/* Lo pendiente queda sin efecto (FCR31 entero se reemplaza). */
		c->fpu_ran = false;
		c->fc_written = false;
		E(c, ppc_crclr(FCR_IS));
		E(c, ppc_mr(0, T_X));
		fpu_derive(c, c->fneed_gate, c->fneed_ien, c->fneed_c);
		bind(c, resume);
		/* Camino lento: el intérprete escribe FCR31 (lee rt de cpu->gpr) y
		 * lanza la excepción (y si no la lanzara, el estado se deriva de
		 * nuevo). */
		unsigned sec = c->sec;
		c->sec = SEC_COLD;
		bind(c, slow);
		store_regs(c, BIT(I_RT(i)) & (c->written | c->pinned));
		emit_exec_call(c, k);
		post_check_mem(c, k);
		E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
		E(c, ppc_crclr(FCR_IS));
		fpu_derive(c, c->fneed_gate, c->fneed_ien, c->fneed_c);
		emit_b(c, resume, false);
		c->sec = sec;
		return;
	}
	default:
		return;
	}
}

/* Lo nativo de COP1 (decode y decode_promote) con la FPU residente. */
static void emit_cop1_dispatch_res(struct ctx *c, unsigned k)
{
	uint32_t i = c->d[k].w;
	unsigned fmt = I_FMT(i), f = I_FUNCT(i);
	if (fmt == FMT_S || fmt == FMT_D) {
		if (f <= 0x07 || f >= 0x30)
			emit_fpu_res(c, k);
		else
			emit_fpu_cvt_res(c, k);
	} else if (fmt == FMT_W) {
		emit_fpu_cvt_res(c, k);
	} else {
		emit_cop1_res(c, k);
	}
}

/* ---- Entrada, reparto y vuelta del bucle -------------------------------------- */

/* Qué necesita el bloque de la FPU residente: registros del PPE para los
 * cpu->fpr[p] más usados, los vivos a la entrada (leídos antes de escribirse,
 * en el formato de su primer uso) y qué bits de FCR31 derivar. */
static void fpu_allocate(struct ctx *c)
{
	unsigned count[32] = { 0 };
	uint64_t seen = 0;
	bool any = false;

	memset(c->fmap, 0, sizeof c->fmap);
	memset(c->ffmt, 0, sizeof c->ffmt);
	memset(c->flive, 0, sizeof c->flive);
	memset(c->fw_ok, 0, sizeof c->fw_ok);
	c->fpu_res = c->fneed_gate = c->fneed_ien = c->fneed_c = false;
	c->fpu_ran = c->fc_written = false;
	if (!(c->rec->opts & SALSA64_REC_OPT_FPU_RES))
		return;
	for (unsigned k = 0; k < c->n; k++) {
		uint32_t w = c->d[k].w;
		unsigned op = I_OP(w);
		struct finfo f;
		if (!(op == 0x11 || op == 0x31 || op == 0x35 || op == 0x39 || op == 0x3D))
			continue;
		any = true;
		if (c->d[k].how != H_NATIVE)
			continue;
		fpu_info(c, w, &f);
		if (f.ra >= 0) {
			count[f.ra]++;
			if (f.fa != FR_NONE && !(seen & BIT(f.ra)) && c->flive[f.ra] == FR_NONE)
				c->flive[f.ra] = (uint8_t)f.fa;
		}
		if (f.rb >= 0) {
			count[f.rb]++;
			if (f.fb != FR_NONE && !(seen & BIT(f.rb)) && c->flive[f.rb] == FR_NONE)
				c->flive[f.rb] = (uint8_t)f.fb;
		}
		if (f.wd >= 0) {
			count[f.wd]++;
			if (!f.hi)
				seen |= BIT(f.wd);
		}
		if (op == 0x11) {
			unsigned fmt = I_FMT(w), funct = I_FUNCT(w);
			if (fmt == 0x08)
				c->fneed_c = true;                                   /* BC1 lee C */
			else if ((fmt == FMT_S || fmt == FMT_D) && (funct <= 0x04 || funct == 0x20 || funct >= 0x24))
				c->fneed_gate = true;                                /* aritmética, raíz, CVT.S.D, CVT.W/L */
			else if ((fmt == FMT_S || fmt == FMT_D) && funct >= 0x08 && funct <= 0x0F)
				c->fneed_ien = true;                                 /* ROUND TRUNC CEIL FLOOR */
			else if (fmt == FMT_W && funct == 0x20)
				c->fneed_gate = true;                                /* CVT.S.W */
		}
	}
	if (!any)
		return;
	c->fpu_res = true;
	for (unsigned slot = 0; slot < F_RESN; slot++) {
		unsigned best = 32;
		for (unsigned p = 0; p < 32; p++)
			if (count[p] && !c->fmap[p] && (best == 32 || count[p] > count[best]))
				best = p;
		if (best == 32)
			break;
		c->fmap[best] = (uint8_t)(F_RES0 + slot);
	}
}

/* Antes de volver al cuerpo (bucle): FCR31 a memoria y el estado como a la
 * entrada; los registros vivos a la entrada con el formato con que se
 * cargaron. */
static void fpu_loop_back(struct ctx *c)
{
	if (!c->fpu_res)
		return;
	fpu_flush_reset(c);
	for (unsigned p = 0; p < 32; p++)
		if (c->fmap[p] && c->flive[p] != FR_NONE && c->ffmt[p] != c->flive[p])
			fpu_reload(c, (int)p, c->flive[p]);
}

/* Estado que depende del camino (el delay slot de un branch-likely interno
 * corre solo si se tomó; un salto interno con destino dentro del bloque
 * llega con otro estado): struct fpu_snap está en rec_emit.c. */
static void fpu_snapshot(const struct ctx *c, struct fpu_snap *s)
{
	memcpy(s->ffmt, c->ffmt, sizeof s->ffmt);
	memcpy(s->fw_ok, c->fw_ok, sizeof s->fw_ok);
	s->ran = c->fpu_ran;
	s->cw = c->fc_written;
}

static void fpu_restore(struct ctx *c, const struct fpu_snap *s)
{
	memcpy(c->ffmt, s->ffmt, sizeof c->ffmt);
	memcpy(c->fw_ok, s->fw_ok, sizeof c->fw_ok);
	c->fpu_ran = s->ran;
	c->fc_written = s->cw;
}

/* Un salto interno llega a la instrucción actual con el estado `s` (el de
 * después de su delay slot); el código de aquí en adelante asume el estado
 * actual. Deja los registros y los bits de FCR31 como los asume (en la
 * sección fría, en el camino tomado). */
static void fpu_reconcile(struct ctx *c, const struct fpu_snap *s)
{
	if (!c->fpu_res)
		return;
	for (unsigned p = 0; p < 32; p++) {
		if (!c->fmap[p])
			continue;
		if (c->ffmt[p] != FR_NONE && s->ffmt[p] != c->ffmt[p])
			E(c, c->ffmt[p] == FR_S ? ppc_lfs(c->fmap[p], fhome_lo(p), R_CPU) : ppc_lfd(c->fmap[p], fhome(p), R_CPU));
		if (c->fw_ok[p] && !s->fw_ok[p]) {
			E(c, ppc_lwz(T_X, fhome_lo(p), R_CPU));
			E(c, ppc_extsw(T_X, T_X));
			E(c, ppc_std(T_X, fhome_w(p), R_CPU));
		}
	}
	/* Lo pendiente que el código de aquí no va a escribir, a memoria; lo que
	 * el código de aquí sí va a escribir y en este camino no se calculó, de
	 * memoria (en este camino no corrió ninguna operación desde la entrada,
	 * así que la bandera I acumulada es 0). */
	if ((s->ran && !c->fpu_ran) || (s->cw && !c->fc_written)) {
		fpu_flush_bits(c, s->ran && !c->fpu_ran, s->cw && !c->fc_written);
		if (s->ran && !c->fpu_ran)
			E(c, ppc_crclr(FCR_IS));
	}
	if ((!s->ran && c->fpu_ran) || (!s->cw && c->fc_written)) {
		E(c, ppc_lwz(0, OFF(fcr31), R_CPU));
		if (!s->ran && c->fpu_ran) {
			E(c, ppc_rlwinm(T_X, 0, 20, 31, 31));  /* causa I (bit 12) */
			E(c, ppc_cmpwi(CR_T, T_X, 0));
			E(c, ppc_crnot(FCR_IC, PPC_BI(CR_T, PPC_EQ)));
		}
		if (!s->cw && c->fc_written)
			fpu_derive(c, false, false, true);
	}
}

/* ---- Cargas y almacenamientos de COP1 (desde emit_load y emit_store) ------------------ */

/* LWC1/LDC1: el valor cargado está en `d` (registro entero) y en casa; si el
 * registro del bloque lo va a usar, se carga también directo de la RDRAM
 * (T_Y = desplazamiento; sin pasar por memoria). */
static void fpu_loaded(struct ctx *c, unsigned k, unsigned rt, bool wide, unsigned d, bool from_rdram)
{
	bool hi = false;
	unsigned p = wide ? fslot64(c, rt) : fslot32(c, rt, &hi);
	fpu_invalidate(c, p, hi);
	if (hi)
		return;
	unsigned want = c->fmap[p] ? fpu_next_use(c, k, p) : FR_NONE;
	if (want == FR_D && !wide)
		want = FR_NONE;
	if (want == FR_S) {
		if (from_rdram) {
			if (wide) {
				E(c, ppc_addi(T_X, T_Y, 4));
				E(c, ppc_lfsx(c->fmap[p], R_RDRAM, T_X));
			} else {
				E(c, ppc_lfsx(c->fmap[p], R_RDRAM, T_Y));
			}
		} else {
			E(c, ppc_lfs(c->fmap[p], fhome_lo(p), R_CPU));
		}
		c->ffmt[p] = FR_S;
	} else if (want == FR_D) {
		E(c, from_rdram ? ppc_lfdx(c->fmap[p], R_RDRAM, T_Y) : ppc_lfd(c->fmap[p], fhome(p), R_CPU));
		c->ffmt[p] = FR_D;
	}
	if (!wide && fpu_w_wanted(c, k, p)) {
		E(c, ppc_extsw(T_X, d));
		E(c, ppc_std(T_X, fhome_w(p), R_CPU));
		c->fw_ok[p] = 1;
	}
}

/* SWC1/SDC1: registro del PPE con el valor a almacenar, o 0 si hay que
 * leerlo de casa. */
static unsigned fpu_store_reg(const struct ctx *c, unsigned rt, bool wide)
{
	bool hi = false;
	unsigned p = wide ? fslot64(c, rt) : fslot32(c, rt, &hi);
	if (hi || !c->fmap[p] || c->ffmt[p] != (wide ? FR_D : FR_S))
		return 0;
	return c->fmap[p];
}
