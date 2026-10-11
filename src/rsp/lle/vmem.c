/*
 * Salsa64 — LLE del RSP: cargas y almacenamientos vectoriales (LWC2 y
 * SWC2) y los movimientos entre la unidad escalar y la vectorial (MFC2,
 * MTC2, CFC2, CTC2). Ver lle.h.
 *
 * Cada forma (byte, media palabra, palabra, doble, cuádruple, resto,
 * empaquetada, sin signo, media, cuarta, transpuesta y "wrap") tiene sus
 * reglas de alineación y de qué elementos toca; las de ares (ISC,
 * ares/n64/rsp/interpreter-vpu.cpp), verificadas contra la consola por
 * nemu64-test, reescritas en C sobre bytes de la DMEM. La DMEM da la
 * vuelta en 4 KB.
 */
#include "rsp/lle/lle.h"

#include <string.h>

static inline uint8_t rd8(const struct rsp_lle *s, uint32_t a)
{
	return s->dmem[a & 0xFFFu];
}

static inline void wr8(struct rsp_lle *s, uint32_t a, uint8_t v)
{
	s->dmem[a & 0xFFFu] = v;
}

/* Desplazamiento de 7 bits con signo, escalado por el tamaño del acceso. */
static inline int32_t offset(uint32_t op, int scale)
{
	int32_t imm = (int32_t)(op & 0x7F);
	if (imm & 0x40)
		imm -= 0x80;
	return imm * scale;
}

void lle_vu_load(struct rsp_lle *s, uint32_t op)
{
	unsigned e = (op >> 7) & 15, vtn = (op >> 16) & 31;
	uint32_t base = s->r[(op >> 21) & 31];
	struct lle_vec *vt = &s->v[vtn];
	uint32_t addr;

	switch ((op >> 11) & 0x1F) {
	case 0x00:  /* LBV */
		addr = base + (uint32_t)offset(op, 1);
		lle_vbyte_set(vt, e, rd8(s, addr));
		break;
	case 0x01: {  /* LSV */
		addr = base + (uint32_t)offset(op, 2);
		unsigned end = e + 2 < 16 ? e + 2 : 16;
		for (unsigned i = e; i < end; i++)
			lle_vbyte_set(vt, i, rd8(s, addr++));
		break;
	}
	case 0x02: {  /* LLV */
		addr = base + (uint32_t)offset(op, 4);
		unsigned end = e + 4 < 16 ? e + 4 : 16;
		for (unsigned i = e; i < end; i++)
			lle_vbyte_set(vt, i, rd8(s, addr++));
		break;
	}
	case 0x03: {  /* LDV */
		addr = base + (uint32_t)offset(op, 8);
		unsigned end = e + 8 < 16 ? e + 8 : 16;
		for (unsigned i = e; i < end; i++)
			lle_vbyte_set(vt, i, rd8(s, addr++));
		break;
	}
	case 0x04: {  /* LQV: hasta el final del bloque de 16 */
		addr = base + (uint32_t)offset(op, 16);
		int end = 16 + (int)e - (int)(addr & 15);
		if (end > 16)
			end = 16;
		for (int i = (int)e; i < end; i++)
			lle_vbyte_set(vt, (unsigned)i, rd8(s, addr++));
		break;
	}
	case 0x05: {  /* LRV: el resto del bloque de 16, al final del registro */
		addr = base + (uint32_t)offset(op, 16);
		int start = 16 - ((int)(addr & 15) - (int)e);
		addr &= ~15u;
		for (int i = start; i < 16; i++)
			lle_vbyte_set(vt, (unsigned)i, rd8(s, addr++));
		break;
	}
	case 0x06: {  /* LPV: bytes empaquetados a la mitad alta */
		addr = base + (uint32_t)offset(op, 8);
		int index = (int)(addr & 7) - (int)e;
		addr &= ~7u;
		for (int i = 0; i < 8; i++)
			vt->e[i] = (uint16_t)(rd8(s, addr + ((uint32_t)(index + i) & 15)) << 8);
		break;
	}
	case 0x07: {  /* LUV: como LPV, un bit más abajo */
		addr = base + (uint32_t)offset(op, 8);
		int index = (int)(addr & 7) - (int)e;
		addr &= ~7u;
		for (int i = 0; i < 8; i++)
			vt->e[i] = (uint16_t)(rd8(s, addr + ((uint32_t)(index + i) & 15)) << 7);
		break;
	}
	case 0x08: {  /* LHV: cada segundo byte */
		addr = base + (uint32_t)offset(op, 16);
		int index = (int)(addr & 7) - (int)e;
		addr &= ~7u;
		for (int i = 0; i < 8; i++)
			vt->e[i] = (uint16_t)(rd8(s, addr + ((uint32_t)(index + i * 2) & 15)) << 7);
		break;
	}
	case 0x09: {  /* LFV: cada cuarto byte, en dos mitades */
		addr = base + (uint32_t)offset(op, 16);
		int index = (int)(addr & 7) - (int)e;
		addr &= ~7u;
		struct lle_vec tmp;
		for (int i = 0; i < 4; i++) {
			tmp.e[i] = (uint16_t)(rd8(s, addr + ((uint32_t)(index + i * 4) & 15)) << 7);
			tmp.e[i + 4] = (uint16_t)(rd8(s, addr + ((uint32_t)(index + i * 4 + 8) & 15)) << 7);
		}
		unsigned end = e + 8 < 16 ? e + 8 : 16;
		for (unsigned i = e; i < end; i++)
			lle_vbyte_set(vt, i, lle_vbyte(&tmp, i));
		break;
	}
	case 0x0B: {  /* LTV: transpuesta sobre 8 registros */
		addr = base + (uint32_t)offset(op, 16);
		uint32_t begin = addr & ~7u;
		addr = begin + ((e + (addr & 8)) & 15);
		unsigned vbase = vtn & ~7u, voff = e >> 1;
		for (unsigned i = 0; i < 8; i++) {
			struct lle_vec *r = &s->v[vbase + voff];
			lle_vbyte_set(r, i * 2, rd8(s, addr++));
			if (addr == begin + 16)
				addr = begin;
			lle_vbyte_set(r, i * 2 + 1, rd8(s, addr++));
			if (addr == begin + 16)
				addr = begin;
			voff = (voff + 1) & 7;
		}
		break;
	}
	default:  /* LWV no existe en el RSP de la N64 */
		break;
	}
}

void lle_vu_store(struct rsp_lle *s, uint32_t op)
{
	unsigned e = (op >> 7) & 15, vtn = (op >> 16) & 31;
	uint32_t base = s->r[(op >> 21) & 31];
	const struct lle_vec *vt = &s->v[vtn];
	uint32_t addr;

	switch ((op >> 11) & 0x1F) {
	case 0x00:  /* SBV */
		addr = base + (uint32_t)offset(op, 1);
		wr8(s, addr, lle_vbyte(vt, e));
		break;
	case 0x01:  /* SSV */
		addr = base + (uint32_t)offset(op, 2);
		for (unsigned i = e; i < e + 2; i++)
			wr8(s, addr++, lle_vbyte(vt, i));
		break;
	case 0x02:  /* SLV */
		addr = base + (uint32_t)offset(op, 4);
		for (unsigned i = e; i < e + 4; i++)
			wr8(s, addr++, lle_vbyte(vt, i));
		break;
	case 0x03:  /* SDV */
		addr = base + (uint32_t)offset(op, 8);
		for (unsigned i = e; i < e + 8; i++)
			wr8(s, addr++, lle_vbyte(vt, i));
		break;
	case 0x04: {  /* SQV */
		addr = base + (uint32_t)offset(op, 16);
		unsigned end = e + (16 - (addr & 15));
		for (unsigned i = e; i < end; i++)
			wr8(s, addr++, lle_vbyte(vt, i));
		break;
	}
	case 0x05: {  /* SRV */
		addr = base + (uint32_t)offset(op, 16);
		unsigned end = e + (addr & 15);
		unsigned shift = 16 - (addr & 15);
		addr &= ~15u;
		for (unsigned i = e; i < end; i++)
			wr8(s, addr++, lle_vbyte(vt, i + shift));
		break;
	}
	case 0x06:  /* SPV */
		addr = base + (uint32_t)offset(op, 8);
		for (unsigned i = e; i < e + 8; i++) {
			if ((i & 15) < 8)
				wr8(s, addr++, lle_vbyte(vt, (i & 7) << 1));
			else
				wr8(s, addr++, (uint8_t)(vt->e[i & 7] >> 7));
		}
		break;
	case 0x07:  /* SUV */
		addr = base + (uint32_t)offset(op, 8);
		for (unsigned i = e; i < e + 8; i++) {
			if ((i & 15) < 8)
				wr8(s, addr++, (uint8_t)(vt->e[i & 7] >> 7));
			else
				wr8(s, addr++, lle_vbyte(vt, (i & 7) << 1));
		}
		break;
	case 0x08: {  /* SHV */
		addr = base + (uint32_t)offset(op, 16);
		unsigned index = addr & 7;
		addr &= ~7u;
		for (unsigned i = 0; i < 8; i++) {
			unsigned b = e + i * 2;
			uint8_t value = (uint8_t)((lle_vbyte(vt, b) << 1) | (lle_vbyte(vt, b + 1) >> 7));
			wr8(s, addr + ((index + i * 2) & 15), value);
		}
		break;
	}
	case 0x09: {  /* SFV: cuatro elementos según e */
		static const int8_t sel[16][4] = {
			{ 0, 1, 2, 3 }, { 6, 7, 4, 5 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 },
			{ 1, 2, 3, 0 }, { 7, 4, 5, 6 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 },
			{ 4, 5, 6, 7 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { 3, 0, 1, 2 },
			{ 5, 6, 7, 4 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { 0, 1, 2, 3 },
		};
		addr = base + (uint32_t)offset(op, 16);
		unsigned b = addr & 7;
		addr &= ~7u;
		for (unsigned i = 0; i < 4; i++) {
			int k = sel[e][i];
			wr8(s, addr + ((b + i * 4) & 15), k < 0 ? 0 : (uint8_t)(vt->e[k] >> 7));
		}
		break;
	}
	case 0x0A: {  /* SWV */
		addr = base + (uint32_t)offset(op, 16);
		unsigned b = addr & 7;
		addr &= ~7u;
		for (unsigned i = e; i < e + 16; i++)
			wr8(s, addr + ((b++) & 15), lle_vbyte(vt, i));
		break;
	}
	case 0x0B: {  /* STV: transpuesta de 8 registros */
		addr = base + (uint32_t)offset(op, 16);
		unsigned vbase = vtn & ~7u;
		unsigned element = 16 - (e & ~1u);
		unsigned b = (addr & 7) - (e & ~1u);
		addr &= ~7u;
		for (unsigned r = vbase; r < vbase + 8; r++) {
			wr8(s, addr + ((b++) & 15), lle_vbyte(&s->v[r], element++));
			wr8(s, addr + ((b++) & 15), lle_vbyte(&s->v[r], element++));
		}
		break;
	}
	default:
		break;
	}
}

void lle_vu_move(struct rsp_lle *s, uint32_t op)
{
	unsigned rt = (op >> 16) & 31, rd = (op >> 11) & 31, e = (op >> 7) & 15;
	switch ((op >> 21) & 0x1F) {
	case 0x00: {  /* MFC2 */
		if (!rt)
			break;
		uint32_t hi = lle_vbyte(&s->v[rd], e), lo = lle_vbyte(&s->v[rd], e + 1);
		s->r[rt] = (uint32_t)(int16_t)((hi << 8) | lo);
		break;
	}
	case 0x02: {  /* CFC2 */
		if (!rt)
			break;
		uint32_t v;
		switch (rd & 3) {
		case 0: v = s->vco; break;
		case 1: v = s->vcc; break;
		default: v = s->vce; break;
		}
		s->r[rt] = (uint32_t)(int16_t)v;
		break;
	}
	case 0x04:  /* MTC2 */
		lle_vbyte_set(&s->v[rd], e, (uint8_t)(s->r[rt] >> 8));
		if (e != 15)
			lle_vbyte_set(&s->v[rd], e + 1, (uint8_t)s->r[rt]);
		break;
	case 0x06:  /* CTC2 */
		switch (rd & 3) {
		case 0: s->vco = (uint16_t)s->r[rt]; break;
		case 1: s->vcc = (uint16_t)s->r[rt]; break;
		default: s->vce = (uint16_t)(s->r[rt] & 0xFF); break;
		}
		break;
	default:
		break;
	}
}
