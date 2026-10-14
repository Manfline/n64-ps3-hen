/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — laboratorio del recompilador RSP → SPU (docs/lab-recompilador.md):
 * traduce todos los trozos de 64 instrucciones de una imagen de IMEM de 4 KB
 * con el traductor de verdad (src/rsp/jit, rama del recompilador del RSP) y
 * escribe un .rsplab con, por trozo, las instrucciones del RSP, las palabras
 * del SPU y dónde empieza el código de cada instrucción. Lo lee
 * tools/lab/lab-spu.py. Se compila contra el árbol que tenga src/rsp/jit:
 *   tools/lab/rsp-build.sh [SRC] [OUT]
 *   rsp-lab imem.bin salida.rsplab
 * La imagen sale de un estado (sección SPME: DMEM 4 KB + IMEM 4 KB), de un
 * volcado de ares (0xA4001000) o de SALSA64_LLE_IMEM_DUMP. No se versiona.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsp/jit/jit.h"

uint32_t lle_io_read(struct rsp_lle *s, unsigned reg) { (void)s; (void)reg; return 0; }
void lle_io_write(struct rsp_lle *s, unsigned reg, uint32_t v) { (void)s; (void)reg; (void)v; }
void lle_io_break(struct rsp_lle *s) { s->halt = true; }
void lle_io_poll(struct rsp_lle *s) { (void)s; }

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "uso: rsp-lab imem.bin salida.rsplab\n");
		return 3;
	}
	static uint8_t imem[LLE_MEM_SIZE];
	static uint32_t code[64 * 1024];
	FILE *f = fopen(argv[1], "rb");
	if (!f || fread(imem, 1, sizeof imem, f) != sizeof imem) {
		fprintf(stderr, "no se pudo leer %s (4096 bytes)\n", argv[1]);
		return 3;
	}
	fclose(f);
	const uint32_t entry_ls = 0x21C90, pool_ls = 0x21AC0, arena_ls = 0x22D00;
	uint32_t enter, exit, miss, call, div_rtn[6];
	uint32_t stubs = jit_stubs_emit(code, sizeof code / 4, arena_ls, 0x11000, 0x21820, pool_ls, 0x21c00, &enter, &exit, &miss,
				       &call, div_rtn);
	stubs = (stubs + 31) & ~31u;
	struct jit_env env = { .entry_ls = entry_ls, .exit_ls = exit, .pool_ls = pool_ls, .x_ls = 0x21820 };
	memcpy(env.div_rtn, div_rtn, sizeof env.div_rtn);
	FILE *o = fopen(argv[2], "w");
	if (!o)
		return 3;
	fprintf(o, "# rsp-lab v1 arena_ls=%u\nS %u enter=%u exit=%u miss=%u call=%u\n", arena_ls, stubs, enter, exit, miss, call);
	fprintf(o, "SW");
	for (uint32_t i = 0; i < stubs; i++)
		fprintf(o, " %08x", code[i]);
	fprintf(o, "\n");
	uint32_t used = stubs;
	for (uint32_t pc0 = 0; pc0 < LLE_MEM_SIZE; pc0 += 4 * JIT_CHUNK_INSTR) {
		bool zero = true;
		for (unsigned i = 0; i < 4 * JIT_CHUNK_INSTR && zero; i++)
			zero = imem[pc0 + i] == 0;
		if (zero)
			continue;
		struct jit_chunk c;
		if (!jit_translate(imem, pc0, code + used, sizeof code / 4 - used, arena_ls + used * 4, &env, &c)) {
			fprintf(stderr, "no cabe el trozo 0x%03x\n", pc0);
			return 2;
		}
		fprintf(o, "C %03x words=%u code_ls=%u interp=%u\nE", c.pc0, c.words, c.code_ls, c.interp);
		for (unsigned i = 0; i < JIT_CHUNK_INSTR; i++)
			fprintf(o, " %u", (unsigned)((c.entry[i] - 0) ));
		fprintf(o, "\nI");
		for (unsigned i = 0; i <= JIT_CHUNK_INSTR; i++) {
			const uint8_t *p = imem + ((pc0 + 4 * i) & 0xFFC);
			fprintf(o, " %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
		}
		fprintf(o, "\nP");
		for (uint32_t i = 0; i < c.words; i++)
			fprintf(o, " %08x", code[used + i]);
		fprintf(o, "\n");
		used += (c.words + 31) & ~31u;
	}
	fclose(o);
	return 0;
}
