/*
 * Retro64 — ensamblador MIPS mínimo para las pruebas unitarias: cada macro
 * da la palabra de 32 bits de una instrucción. Los inmediatos se recortan a
 * 16 bits; los desplazamientos de salto van en instrucciones (no en bytes).
 */
#ifndef RETRO64_TESTS_MIPS_H
#define RETRO64_TESTS_MIPS_H

#include <stdint.h>

enum {
	ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7,
	S0, S1, S2, S3, S4, S5, S6, S7, T8, T9, K0, K1, GP, SP, FP, RA
};

#define M_R(op, rs, rt, rd, sa, fn) \
	((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | \
	 (uint32_t)(rd) << 11 | (uint32_t)(sa) << 6 | (uint32_t)(fn))
#define M_I(op, rs, rt, imm) \
	((uint32_t)(op) << 26 | (uint32_t)(rs) << 21 | (uint32_t)(rt) << 16 | ((uint32_t)(imm) & 0xFFFF))
#define M_J(op, target) ((uint32_t)(op) << 26 | ((uint32_t)(target) & 0x03FFFFFF))

#define NOP                 0u
/* SPECIAL */
#define SLL(rd, rt, sa)     M_R(0, 0, rt, rd, sa, 0x00)
#define SRL(rd, rt, sa)     M_R(0, 0, rt, rd, sa, 0x02)
#define SRA(rd, rt, sa)     M_R(0, 0, rt, rd, sa, 0x03)
#define SLLV(rd, rt, rs)    M_R(0, rs, rt, rd, 0, 0x04)
#define SRLV(rd, rt, rs)    M_R(0, rs, rt, rd, 0, 0x06)
#define SRAV(rd, rt, rs)    M_R(0, rs, rt, rd, 0, 0x07)
#define JR(rs)              M_R(0, rs, 0, 0, 0, 0x08)
#define JALR(rd, rs)        M_R(0, rs, 0, rd, 0, 0x09)
#define SYSCALL             M_R(0, 0, 0, 0, 0, 0x0C)
#define BREAK               M_R(0, 0, 0, 0, 0, 0x0D)
#define SYNC                M_R(0, 0, 0, 0, 0, 0x0F)
#define MFHI(rd)            M_R(0, 0, 0, rd, 0, 0x10)
#define MTHI(rs)            M_R(0, rs, 0, 0, 0, 0x11)
#define MFLO(rd)            M_R(0, 0, 0, rd, 0, 0x12)
#define MTLO(rs)            M_R(0, rs, 0, 0, 0, 0x13)
#define DSLLV(rd, rt, rs)   M_R(0, rs, rt, rd, 0, 0x14)
#define DSRLV(rd, rt, rs)   M_R(0, rs, rt, rd, 0, 0x16)
#define DSRAV(rd, rt, rs)   M_R(0, rs, rt, rd, 0, 0x17)
#define MULT(rs, rt)        M_R(0, rs, rt, 0, 0, 0x18)
#define MULTU(rs, rt)       M_R(0, rs, rt, 0, 0, 0x19)
#define DIV(rs, rt)         M_R(0, rs, rt, 0, 0, 0x1A)
#define DIVU(rs, rt)        M_R(0, rs, rt, 0, 0, 0x1B)
#define DMULT(rs, rt)       M_R(0, rs, rt, 0, 0, 0x1C)
#define DMULTU(rs, rt)      M_R(0, rs, rt, 0, 0, 0x1D)
#define DDIV(rs, rt)        M_R(0, rs, rt, 0, 0, 0x1E)
#define DDIVU(rs, rt)       M_R(0, rs, rt, 0, 0, 0x1F)
#define ADD(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x20)
#define ADDU(rd, rs, rt)    M_R(0, rs, rt, rd, 0, 0x21)
#define SUB(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x22)
#define SUBU(rd, rs, rt)    M_R(0, rs, rt, rd, 0, 0x23)
#define AND(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x24)
#define OR(rd, rs, rt)      M_R(0, rs, rt, rd, 0, 0x25)
#define XOR(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x26)
#define NOR(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x27)
#define SLT(rd, rs, rt)     M_R(0, rs, rt, rd, 0, 0x2A)
#define SLTU(rd, rs, rt)    M_R(0, rs, rt, rd, 0, 0x2B)
#define DADD(rd, rs, rt)    M_R(0, rs, rt, rd, 0, 0x2C)
#define DADDU(rd, rs, rt)   M_R(0, rs, rt, rd, 0, 0x2D)
#define DSUB(rd, rs, rt)    M_R(0, rs, rt, rd, 0, 0x2E)
#define DSUBU(rd, rs, rt)   M_R(0, rs, rt, rd, 0, 0x2F)
#define TGE(rs, rt)         M_R(0, rs, rt, 0, 0, 0x30)
#define TEQ(rs, rt)         M_R(0, rs, rt, 0, 0, 0x34)
#define TNE(rs, rt)         M_R(0, rs, rt, 0, 0, 0x36)
#define DSLL(rd, rt, sa)    M_R(0, 0, rt, rd, sa, 0x38)
#define DSRL(rd, rt, sa)    M_R(0, 0, rt, rd, sa, 0x3A)
#define DSRA(rd, rt, sa)    M_R(0, 0, rt, rd, sa, 0x3B)
#define DSLL32(rd, rt, sa)  M_R(0, 0, rt, rd, sa, 0x3C)
#define DSRL32(rd, rt, sa)  M_R(0, 0, rt, rd, sa, 0x3E)
#define DSRA32(rd, rt, sa)  M_R(0, 0, rt, rd, sa, 0x3F)
/* REGIMM */
#define BLTZ(rs, off)       M_I(1, rs, 0x00, off)
#define BGEZ(rs, off)       M_I(1, rs, 0x01, off)
#define BLTZL(rs, off)      M_I(1, rs, 0x02, off)
#define BGEZL(rs, off)      M_I(1, rs, 0x03, off)
#define TEQI(rs, imm)       M_I(1, rs, 0x0C, imm)
#define BLTZAL(rs, off)     M_I(1, rs, 0x10, off)
#define BGEZAL(rs, off)     M_I(1, rs, 0x11, off)
/* Inmediatos y saltos */
#define J(target)           M_J(0x02, (target) >> 2)
#define JAL(target)         M_J(0x03, (target) >> 2)
#define BEQ(rs, rt, off)    M_I(0x04, rs, rt, off)
#define BNE(rs, rt, off)    M_I(0x05, rs, rt, off)
#define BLEZ(rs, off)       M_I(0x06, rs, 0, off)
#define BGTZ(rs, off)       M_I(0x07, rs, 0, off)
#define ADDI(rt, rs, imm)   M_I(0x08, rs, rt, imm)
#define ADDIU(rt, rs, imm)  M_I(0x09, rs, rt, imm)
#define SLTI(rt, rs, imm)   M_I(0x0A, rs, rt, imm)
#define SLTIU(rt, rs, imm)  M_I(0x0B, rs, rt, imm)
#define ANDI(rt, rs, imm)   M_I(0x0C, rs, rt, imm)
#define ORI(rt, rs, imm)    M_I(0x0D, rs, rt, imm)
#define XORI(rt, rs, imm)   M_I(0x0E, rs, rt, imm)
#define LUI(rt, imm)        M_I(0x0F, 0, rt, imm)
#define BEQL(rs, rt, off)   M_I(0x14, rs, rt, off)
#define BNEL(rs, rt, off)   M_I(0x15, rs, rt, off)
#define DADDI(rt, rs, imm)  M_I(0x18, rs, rt, imm)
#define DADDIU(rt, rs, imm) M_I(0x19, rs, rt, imm)
/* Memoria */
#define LDL(rt, off, base)  M_I(0x1A, base, rt, off)
#define LDR(rt, off, base)  M_I(0x1B, base, rt, off)
#define LB(rt, off, base)   M_I(0x20, base, rt, off)
#define LH(rt, off, base)   M_I(0x21, base, rt, off)
#define LWL(rt, off, base)  M_I(0x22, base, rt, off)
#define LW(rt, off, base)   M_I(0x23, base, rt, off)
#define LBU(rt, off, base)  M_I(0x24, base, rt, off)
#define LHU(rt, off, base)  M_I(0x25, base, rt, off)
#define LWR(rt, off, base)  M_I(0x26, base, rt, off)
#define LWU(rt, off, base)  M_I(0x27, base, rt, off)
#define SB(rt, off, base)   M_I(0x28, base, rt, off)
#define SH(rt, off, base)   M_I(0x29, base, rt, off)
#define SWL(rt, off, base)  M_I(0x2A, base, rt, off)
#define SW(rt, off, base)   M_I(0x2B, base, rt, off)
#define SDL(rt, off, base)  M_I(0x2C, base, rt, off)
#define SDR(rt, off, base)  M_I(0x2D, base, rt, off)
#define SWR(rt, off, base)  M_I(0x2E, base, rt, off)
#define LL(rt, off, base)   M_I(0x30, base, rt, off)
#define LWC1(ft, off, base) M_I(0x31, base, ft, off)
#define LDC1(ft, off, base) M_I(0x35, base, ft, off)
#define LD(rt, off, base)   M_I(0x37, base, rt, off)
#define SC(rt, off, base)   M_I(0x38, base, rt, off)
#define SWC1(ft, off, base) M_I(0x39, base, ft, off)
#define SDC1(ft, off, base) M_I(0x3D, base, ft, off)
#define SD(rt, off, base)   M_I(0x3F, base, rt, off)
/* COP0 */
#define MFC0(rt, rd)        M_R(0x10, 0x00, rt, rd, 0, 0)
#define DMFC0(rt, rd)       M_R(0x10, 0x01, rt, rd, 0, 0)
#define MTC0(rt, rd)        M_R(0x10, 0x04, rt, rd, 0, 0)
#define DMTC0(rt, rd)       M_R(0x10, 0x05, rt, rd, 0, 0)
#define TLBR                M_R(0x10, 0x10, 0, 0, 0, 0x01)
#define TLBWI               M_R(0x10, 0x10, 0, 0, 0, 0x02)
#define TLBWR               M_R(0x10, 0x10, 0, 0, 0, 0x06)
#define TLBP                M_R(0x10, 0x10, 0, 0, 0, 0x08)
#define ERET                M_R(0x10, 0x10, 0, 0, 0, 0x18)
/* COP1 */
#define F_S 0x10
#define F_D 0x11
#define F_W 0x14
#define F_L 0x15
#define MFC1(rt, fs)        M_R(0x11, 0x00, rt, fs, 0, 0)
#define DMFC1(rt, fs)       M_R(0x11, 0x01, rt, fs, 0, 0)
#define CFC1(rt, fs)        M_R(0x11, 0x02, rt, fs, 0, 0)
#define MTC1(rt, fs)        M_R(0x11, 0x04, rt, fs, 0, 0)
#define DMTC1(rt, fs)       M_R(0x11, 0x05, rt, fs, 0, 0)
#define CTC1(rt, fs)        M_R(0x11, 0x06, rt, fs, 0, 0)
#define BC1F(off)           M_I(0x11, 0x08, 0, off)
#define BC1T(off)           M_I(0x11, 0x08, 1, off)
#define BC1FL(off)          M_I(0x11, 0x08, 2, off)
#define BC1TL(off)          M_I(0x11, 0x08, 3, off)
#define FOP(fmt, fn, fd, fs, ft) M_R(0x11, fmt, ft, fs, fd, fn)
#define ADD_F(fmt, fd, fs, ft)   FOP(fmt, 0x00, fd, fs, ft)
#define SUB_F(fmt, fd, fs, ft)   FOP(fmt, 0x01, fd, fs, ft)
#define MUL_F(fmt, fd, fs, ft)   FOP(fmt, 0x02, fd, fs, ft)
#define DIV_F(fmt, fd, fs, ft)   FOP(fmt, 0x03, fd, fs, ft)
#define SQRT_F(fmt, fd, fs)      FOP(fmt, 0x04, fd, fs, 0)
#define ABS_F(fmt, fd, fs)       FOP(fmt, 0x05, fd, fs, 0)
#define MOV_F(fmt, fd, fs)       FOP(fmt, 0x06, fd, fs, 0)
#define NEG_F(fmt, fd, fs)       FOP(fmt, 0x07, fd, fs, 0)
#define ROUND_L(fmt, fd, fs)     FOP(fmt, 0x08, fd, fs, 0)
#define TRUNC_L(fmt, fd, fs)     FOP(fmt, 0x09, fd, fs, 0)
#define ROUND_W(fmt, fd, fs)     FOP(fmt, 0x0C, fd, fs, 0)
#define TRUNC_W(fmt, fd, fs)     FOP(fmt, 0x0D, fd, fs, 0)
#define CEIL_W(fmt, fd, fs)      FOP(fmt, 0x0E, fd, fs, 0)
#define FLOOR_W(fmt, fd, fs)     FOP(fmt, 0x0F, fd, fs, 0)
#define CVT_S(fmt, fd, fs)       FOP(fmt, 0x20, fd, fs, 0)
#define CVT_D(fmt, fd, fs)       FOP(fmt, 0x21, fd, fs, 0)
#define CVT_W(fmt, fd, fs)       FOP(fmt, 0x24, fd, fs, 0)
#define CVT_L(fmt, fd, fs)       FOP(fmt, 0x25, fd, fs, 0)
#define C_F(fmt, cond, fs, ft)   FOP(fmt, 0x30 | (cond), 0, fs, ft)
#define C_UN  0x1
#define C_EQ  0x2
#define C_ULE 0x7
#define C_SEQ 0xA
#define C_LT  0xC
#define C_LE  0xE

#endif
