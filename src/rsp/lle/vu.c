/*
 * Salsa64 — LLE del RSP: unidad vectorial (operaciones de COP2 con el bit
 * 25). Ver lle.h.
 *
 * Cada operación trabaja sobre 8 elementos de 16 bits de vs y de vt, con
 * vt "difundido" según el campo e (0–1 tal cual, 2–3 cuartos, 4–7 mitades,
 * 8–15 un solo elemento), y deja su resultado en el acumulador de 48 bits
 * por elemento (acc[0] alto, acc[1] medio, acc[2] bajo), del que vd toma
 * una rebanada saturada según la instrucción. VCO, VCC y VCE son las
 * banderas de acarreo, comparación y "extensión de comparación" que usan
 * las instrucciones de recorte (VCH/VCL/VCR) y VMRG.
 *
 * Semántica, caso por caso, de ares (ISC, Copyright (c) 2004-2025 ares
 * team, Near et al; ares/n64/rsp/interpreter-vpu.cpp, caminos SISD),
 * reescrita en C; nemu64-test la verifica contra la consola. Ver
 * docs/THIRD_PARTY.md.
 */
#include "rsp/lle/lle.h"

#include <string.h>

#define ACCH (s->acc[0].e)
#define ACCM (s->acc[1].e)
#define ACCL (s->acc[2].e)

static inline int16_t clamp16(int32_t v)
{
	return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

static inline int64_t clip48(int64_t v)
{
	return (int64_t)((uint64_t)v << 16) >> 16;
}

/* Difusión de vt según e. */
static void broadcast(struct lle_vec *out, const struct lle_vec *vt, unsigned e)
{
	static const uint8_t sel[16][8] = {
		{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
		{ 0, 0, 2, 2, 4, 4, 6, 6 }, { 1, 1, 3, 3, 5, 5, 7, 7 },
		{ 0, 0, 0, 0, 4, 4, 4, 4 }, { 1, 1, 1, 1, 5, 5, 5, 5 },
		{ 2, 2, 2, 2, 6, 6, 6, 6 }, { 3, 3, 3, 3, 7, 7, 7, 7 },
		{ 0, 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 1, 1, 1, 1, 1, 1 },
		{ 2, 2, 2, 2, 2, 2, 2, 2 }, { 3, 3, 3, 3, 3, 3, 3, 3 },
		{ 4, 4, 4, 4, 4, 4, 4, 4 }, { 5, 5, 5, 5, 5, 5, 5, 5 },
		{ 6, 6, 6, 6, 6, 6, 6, 6 }, { 7, 7, 7, 7, 7, 7, 7, 7 },
	};
	const uint8_t *k = sel[e & 15];
	for (int n = 0; n < 8; n++)
		out->e[n] = vt->e[k[n]];
}

static inline uint64_t acc_get(const struct rsp_lle *s, int n)
{
	return ((uint64_t)ACCH[n] << 32) | ((uint64_t)ACCM[n] << 16) | ACCL[n];
}

static inline void acc_set(struct rsp_lle *s, int n, uint64_t v)
{
	ACCH[n] = (uint16_t)(v >> 32);
	ACCM[n] = (uint16_t)(v >> 16);
	ACCL[n] = (uint16_t)v;
}

/* Rebanada saturada del acumulador: la media (slice) o la baja, con los
 * valores de saturación por signo del acumulador. */
static inline uint16_t acc_saturate(const struct rsp_lle *s, int n, bool slice, uint16_t negative,
				    uint16_t positive)
{
	if ((int16_t)ACCH[n] < 0) {
		if (ACCH[n] != 0xFFFF)
			return negative;
		if ((int16_t)ACCM[n] >= 0)
			return negative;
	} else {
		if (ACCH[n] != 0)
			return positive;
		if ((int16_t)ACCM[n] < 0)
			return positive;
	}
	return slice ? ACCM[n] : ACCL[n];
}

#define VCO_L(n) ((s->vco >> (n)) & 1u)
#define VCO_H(n) ((s->vco >> (8 + (n))) & 1u)
#define VCC_L(n) ((s->vcc >> (n)) & 1u)
#define VCC_H(n) ((s->vcc >> (8 + (n))) & 1u)
#define VCE_(n)  ((s->vce >> (n)) & 1u)

static inline void flag_set(uint16_t *f, int bit, bool v)
{
	*f = (uint16_t)((*f & ~(1u << bit)) | ((unsigned)v << bit));
}

static uint32_t clz32(uint32_t v)
{
	return v ? (uint32_t)__builtin_clz(v) : 32;
}

/* VRCP/VRCPL y VRSQ/VRSQL: `low` toma la palabra de 32 bits (VRCPH/VRSQH
 * antes dio la mitad alta); `sqrt` usa la otra tabla. */
static void divide(struct rsp_lle *s, uint32_t op, bool low, bool sqrt)
{
	unsigned e = (op >> 21) & 15, de = (op >> 11) & 7, vt = (op >> 16) & 31, vd = (op >> 6) & 31;
	int32_t input = low && s->div_dp ? (int32_t)(((uint32_t)(uint16_t)s->div_in << 16) | s->v[vt].e[e & 7])
					 : (int32_t)(int16_t)s->v[vt].e[e & 7];
	int32_t mask = input >> 31;
	int32_t data = input ^ mask;
	int32_t result;
	if (input > -32768)
		data -= mask;
	if (data == 0) {
		result = 0x7FFFFFFF;
	} else if (input == -32768) {
		result = (int32_t)0xFFFF0000u;
	} else {
		uint32_t shift = clz32((uint32_t)data);
		uint32_t index = (uint32_t)(((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22;
		if (sqrt) {
			result = s->rsq_table[(index & 0x1FE) | (shift & 1)];
			result = (0x10000 | result) << 14;
			result = (int32_t)((uint32_t)result >> ((31 - shift) >> 1)) ^ mask;
		} else {
			result = s->rcp_table[index];
			result = (0x10000 | result) << 14;
			result = (int32_t)((uint32_t)result >> (31 - shift)) ^ mask;
		}
	}
	s->div_dp = false;
	s->div_out = (int16_t)(result >> 16);
	broadcast(&s->acc[2], &s->v[vt], e);
	s->v[vd].e[de] = (uint16_t)result;
}

static void divide_high(struct rsp_lle *s, uint32_t op)
{
	unsigned e = (op >> 21) & 15, de = (op >> 11) & 7, vt = (op >> 16) & 31, vd = (op >> 6) & 31;
	broadcast(&s->acc[2], &s->v[vt], e);
	s->div_dp = true;
	s->div_in = (int16_t)s->v[vt].e[e & 7];
	s->v[vd].e[de] = (uint16_t)s->div_out;
}

void lle_vu_op(struct rsp_lle *s, uint32_t op)
{
	unsigned e = (op >> 21) & 15;
	unsigned vtn = (op >> 16) & 31, vsn = (op >> 11) & 31, vdn = (op >> 6) & 31;
	const struct lle_vec *vs = &s->v[vsn];
	struct lle_vec *vd = &s->v[vdn];
	struct lle_vec vte;
	broadcast(&vte, &s->v[vtn], e);

	switch (op & 0x3F) {
	case 0x00:  /* VMULF */
	case 0x01:  /* VMULU */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, (uint64_t)((int64_t)(int16_t)vs->e[n] * (int16_t)vte.e[n] * 2 + 0x8000));
			if ((op & 0x3F) == 0x00)
				vd->e[n] = acc_saturate(s, n, true, 0x8000, 0x7FFF);
			else
				vd->e[n] = (int16_t)ACCH[n] < 0 ? 0 : ((int16_t)ACCH[n] ^ (int16_t)ACCM[n]) < 0 ? 0xFFFF : ACCM[n];
		}
		break;
	case 0x02:  /* VRNDP */
	case 0x0A:  /* VRNDN */
		for (int n = 0; n < 8; n++) {
			int32_t product = (int16_t)vte.e[n];
			if (vsn & 1)
				product = (int32_t)((uint32_t)product << 16);
			int64_t acc = clip48((int64_t)acc_get(s, n));
			bool down = (op & 0x3F) == 0x0A;
			if (down ? acc < 0 : acc >= 0)
				acc = clip48(acc + product);
			acc_set(s, n, (uint64_t)acc);
			vd->e[n] = (uint16_t)clamp16((int32_t)(acc >> 16));
		}
		break;
	case 0x03:  /* VMULQ */
		for (int n = 0; n < 8; n++) {
			int32_t product = (int16_t)vs->e[n] * (int16_t)vte.e[n];
			if (product < 0)
				product += 31;
			ACCH[n] = (uint16_t)(product >> 16);
			ACCM[n] = (uint16_t)product;
			ACCL[n] = 0;
			vd->e[n] = (uint16_t)(clamp16(product >> 1) & ~15);
		}
		break;
	case 0x04:  /* VMUDL */
		for (int n = 0; n < 8; n++)
			acc_set(s, n, (uint16_t)(((uint32_t)vs->e[n] * vte.e[n]) >> 16));
		*vd = s->acc[2];
		break;
	case 0x05:  /* VMUDM */
		for (int n = 0; n < 8; n++)
			acc_set(s, n, (uint64_t)(int64_t)((int32_t)(int16_t)vs->e[n] * (int32_t)vte.e[n]));
		*vd = s->acc[1];
		break;
	case 0x06:  /* VMUDN */
		for (int n = 0; n < 8; n++)
			acc_set(s, n, (uint64_t)(int64_t)((int32_t)vs->e[n] * (int32_t)(int16_t)vte.e[n]));
		*vd = s->acc[2];
		break;
	case 0x07:  /* VMUDH */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, (uint64_t)(int64_t)((int32_t)(int16_t)vs->e[n] * (int16_t)vte.e[n]) << 16);
			vd->e[n] = acc_saturate(s, n, true, 0x8000, 0x7FFF);
		}
		break;
	case 0x08:  /* VMACF */
	case 0x09:  /* VMACU */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, acc_get(s, n) + (uint64_t)((int64_t)(int16_t)vs->e[n] * (int16_t)vte.e[n] * 2));
			if ((op & 0x3F) == 0x08)
				vd->e[n] = acc_saturate(s, n, true, 0x8000, 0x7FFF);
			else
				vd->e[n] = (int16_t)ACCH[n] < 0 ? 0 : (ACCH[n] || (int16_t)ACCM[n] < 0) ? 0xFFFF : ACCM[n];
		}
		break;
	case 0x0B:  /* VMACQ */
		for (int n = 0; n < 8; n++) {
			int32_t product = (int32_t)(((uint32_t)ACCH[n] << 16) | ACCM[n]);
			if (product < 0 && !(product & (1 << 5)))
				product += 32;
			else if (product >= 32 && !(product & (1 << 5)))
				product -= 32;
			ACCH[n] = (uint16_t)(product >> 16);
			ACCM[n] = (uint16_t)product;
			vd->e[n] = (uint16_t)(clamp16(product >> 1) & ~15);
		}
		break;
	case 0x0C:  /* VMADL */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, acc_get(s, n) + (((uint32_t)vs->e[n] * vte.e[n]) >> 16));
			vd->e[n] = acc_saturate(s, n, false, 0x0000, 0xFFFF);
		}
		break;
	case 0x0D:  /* VMADM */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, acc_get(s, n) + (uint64_t)(int64_t)((int32_t)(int16_t)vs->e[n] * (int32_t)vte.e[n]));
			vd->e[n] = acc_saturate(s, n, true, 0x8000, 0x7FFF);
		}
		break;
	case 0x0E:  /* VMADN */
		for (int n = 0; n < 8; n++) {
			acc_set(s, n, acc_get(s, n) + (uint64_t)(int64_t)((int32_t)vs->e[n] * (int32_t)(int16_t)vte.e[n]));
			vd->e[n] = acc_saturate(s, n, false, 0x0000, 0xFFFF);
		}
		break;
	case 0x0F:  /* VMADH */
		for (int n = 0; n < 8; n++) {
			uint32_t result = (uint32_t)(acc_get(s, n) >> 16) +
					  (uint32_t)((int32_t)(int16_t)vs->e[n] * (int16_t)vte.e[n]);
			ACCH[n] = (uint16_t)(result >> 16);
			ACCM[n] = (uint16_t)result;
			vd->e[n] = acc_saturate(s, n, true, 0x8000, 0x7FFF);
		}
		break;
	case 0x10:  /* VADD */
		for (int n = 0; n < 8; n++) {
			int32_t result = (int16_t)vs->e[n] + (int16_t)vte.e[n] + (int32_t)VCO_L(n);
			ACCL[n] = (uint16_t)result;
			vd->e[n] = (uint16_t)clamp16(result);
		}
		s->vco = 0;
		break;
	case 0x11:  /* VSUB */
		for (int n = 0; n < 8; n++) {
			int32_t result = (int16_t)vs->e[n] - (int16_t)vte.e[n] - (int32_t)VCO_L(n);
			ACCL[n] = (uint16_t)result;
			vd->e[n] = (uint16_t)clamp16(result);
		}
		s->vco = 0;
		break;
	case 0x13:  /* VABS */
		for (int n = 0; n < 8; n++) {
			int16_t a = (int16_t)vs->e[n], b = (int16_t)vte.e[n];
			if (a < 0) {
				if (b == -32768) {
					ACCL[n] = 0x8000;
					vd->e[n] = 0x7FFF;
				} else {
					ACCL[n] = (uint16_t)-b;
					vd->e[n] = (uint16_t)-b;
				}
			} else if (a > 0) {
				ACCL[n] = (uint16_t)b;
				vd->e[n] = (uint16_t)b;
			} else {
				ACCL[n] = 0;
				vd->e[n] = 0;
			}
		}
		break;
	case 0x14:  /* VADDC */
		for (int n = 0; n < 8; n++) {
			uint32_t result = (uint32_t)vs->e[n] + vte.e[n];
			ACCL[n] = (uint16_t)result;
			flag_set(&s->vco, n, (result >> 16) != 0);
		}
		s->vco &= 0x00FF;
		*vd = s->acc[2];
		break;
	case 0x15:  /* VSUBC */
		for (int n = 0; n < 8; n++) {
			uint32_t result = (uint32_t)vs->e[n] - vte.e[n];
			ACCL[n] = (uint16_t)result;
			flag_set(&s->vco, n, (result >> 16) != 0);
			flag_set(&s->vco, 8 + n, result != 0);
		}
		*vd = s->acc[2];
		break;
	case 0x1D:  /* VSAR */
		switch (e) {
		case 0x8: *vd = s->acc[0]; break;
		case 0x9: *vd = s->acc[1]; break;
		case 0xA: *vd = s->acc[2]; break;
		default: memset(vd, 0, sizeof *vd); break;
		}
		break;
	case 0x20:  /* VLT */
		for (int n = 0; n < 8; n++) {
			int16_t a = (int16_t)vs->e[n], b = (int16_t)vte.e[n];
			bool lt = a < b || (a == b && VCO_L(n) && VCO_H(n));
			flag_set(&s->vcc, n, lt);
			ACCL[n] = lt ? vs->e[n] : vte.e[n];
		}
		s->vcc &= 0x00FF;
		s->vco = 0;
		*vd = s->acc[2];
		break;
	case 0x21:  /* VEQ */
		for (int n = 0; n < 8; n++) {
			bool eq = !VCO_H(n) && vs->e[n] == vte.e[n];
			flag_set(&s->vcc, n, eq);
			ACCL[n] = eq ? vs->e[n] : vte.e[n];
		}
		s->vcc &= 0x00FF;
		s->vco = 0;
		*vd = s->acc[2];
		break;
	case 0x22:  /* VNE */
		for (int n = 0; n < 8; n++) {
			bool ne = vs->e[n] != vte.e[n] || VCO_H(n);
			flag_set(&s->vcc, n, ne);
			ACCL[n] = ne ? vs->e[n] : vte.e[n];
		}
		s->vcc &= 0x00FF;
		s->vco = 0;
		*vd = s->acc[2];
		break;
	case 0x23:  /* VGE */
		for (int n = 0; n < 8; n++) {
			int16_t a = (int16_t)vs->e[n], b = (int16_t)vte.e[n];
			bool ge = a > b || (a == b && (!VCO_L(n) || !VCO_H(n)));
			flag_set(&s->vcc, n, ge);
			ACCL[n] = ge ? vs->e[n] : vte.e[n];
		}
		s->vcc &= 0x00FF;
		s->vco = 0;
		*vd = s->acc[2];
		break;
	case 0x24:  /* VCL */
		for (int n = 0; n < 8; n++) {
			uint16_t a = vs->e[n], b = vte.e[n];
			if (VCO_L(n)) {
				if (VCO_H(n)) {
					ACCL[n] = VCC_L(n) ? (uint16_t)-b : a;
				} else {
					uint16_t sum = (uint16_t)(a + b);
					bool carry = ((uint32_t)a + b) != sum;
					bool le = VCE_(n) ? (!sum || !carry) : (!sum && !carry);
					flag_set(&s->vcc, n, le);
					ACCL[n] = le ? (uint16_t)-b : a;
				}
			} else {
				if (VCO_H(n)) {
					ACCL[n] = VCC_H(n) ? b : a;
				} else {
					bool ge = (int32_t)a - (int32_t)b >= 0;
					flag_set(&s->vcc, 8 + n, ge);
					ACCL[n] = ge ? b : a;
				}
			}
		}
		s->vco = 0;
		s->vce = 0;
		*vd = s->acc[2];
		break;
	case 0x25:  /* VCH */
		for (int n = 0; n < 8; n++) {
			int16_t a = (int16_t)vs->e[n], b = (int16_t)vte.e[n];
			uint16_t nb = (uint16_t)~vte.e[n];  /* vs == ~vt: "igual con signo contrario" */
			if ((a ^ b) < 0) {
				int16_t result = (int16_t)(a + b);
				ACCL[n] = result <= 0 ? (uint16_t)-b : (uint16_t)a;
				flag_set(&s->vcc, n, result <= 0);
				flag_set(&s->vcc, 8 + n, b < 0);
				flag_set(&s->vco, n, true);
				flag_set(&s->vco, 8 + n, result != 0 && vs->e[n] != nb);
				flag_set(&s->vce, n, result == -1);
			} else {
				int16_t result = (int16_t)(a - b);
				ACCL[n] = result >= 0 ? (uint16_t)b : (uint16_t)a;
				flag_set(&s->vcc, n, b < 0);
				flag_set(&s->vcc, 8 + n, result >= 0);
				flag_set(&s->vco, n, false);
				flag_set(&s->vco, 8 + n, result != 0 && vs->e[n] != nb);
				flag_set(&s->vce, n, false);
			}
		}
		*vd = s->acc[2];
		break;
	case 0x26:  /* VCR */
		for (int n = 0; n < 8; n++) {
			int16_t a = (int16_t)vs->e[n], b = (int16_t)vte.e[n];
			if ((a ^ b) < 0) {
				flag_set(&s->vcc, 8 + n, b < 0);
				bool le = a + b + 1 <= 0;
				flag_set(&s->vcc, n, le);
				ACCL[n] = le ? (uint16_t)~vte.e[n] : vs->e[n];
			} else {
				flag_set(&s->vcc, n, b < 0);
				bool ge = a - b >= 0;
				flag_set(&s->vcc, 8 + n, ge);
				ACCL[n] = ge ? vte.e[n] : vs->e[n];
			}
		}
		s->vco = 0;
		s->vce = 0;
		*vd = s->acc[2];
		break;
	case 0x27:  /* VMRG */
		for (int n = 0; n < 8; n++)
			ACCL[n] = VCC_L(n) ? vs->e[n] : vte.e[n];
		s->vco = 0;
		*vd = s->acc[2];
		break;
	case 0x28:  /* VAND */
		for (int n = 0; n < 8; n++)
			ACCL[n] = vs->e[n] & vte.e[n];
		*vd = s->acc[2];
		break;
	case 0x29:  /* VNAND */
		for (int n = 0; n < 8; n++)
			ACCL[n] = (uint16_t)~(vs->e[n] & vte.e[n]);
		*vd = s->acc[2];
		break;
	case 0x2A:  /* VOR */
		for (int n = 0; n < 8; n++)
			ACCL[n] = vs->e[n] | vte.e[n];
		*vd = s->acc[2];
		break;
	case 0x2B:  /* VNOR */
		for (int n = 0; n < 8; n++)
			ACCL[n] = (uint16_t)~(vs->e[n] | vte.e[n]);
		*vd = s->acc[2];
		break;
	case 0x2C:  /* VXOR */
		for (int n = 0; n < 8; n++)
			ACCL[n] = vs->e[n] ^ vte.e[n];
		*vd = s->acc[2];
		break;
	case 0x2D:  /* VNXOR */
		for (int n = 0; n < 8; n++)
			ACCL[n] = (uint16_t)~(vs->e[n] ^ vte.e[n]);
		*vd = s->acc[2];
		break;
	case 0x30: divide(s, op, false, false); break;  /* VRCP */
	case 0x31: divide(s, op, true, false); break;   /* VRCPL */
	case 0x32: divide_high(s, op); break;           /* VRCPH */
	case 0x33: {  /* VMOV */
		unsigned de = (op >> 11) & 7;
		vd->e[de] = vte.e[de];
		s->acc[2] = vte;
		break;
	}
	case 0x34: divide(s, op, false, true); break;   /* VRSQ */
	case 0x35: divide(s, op, true, true); break;    /* VRSQL */
	case 0x36: divide_high(s, op); break;           /* VRSQH */
	case 0x37:  /* VNOP */
	case 0x3F:  /* VNULL */
		break;
	default:
		/* Opcodes reservados: el acumulador bajo recibe vs + vt y vd
		 * queda en cero. */
		for (int n = 0; n < 8; n++)
			ACCL[n] = (uint16_t)(vs->e[n] + vte.e[n]);
		memset(vd, 0, sizeof *vd);
		break;
	}
}
