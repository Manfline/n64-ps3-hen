/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — RDP a bajo nivel: el flujo de comandos que llega por
 * DPC_START/DPC_END (lo que produce el RSP con el microcódigo real, o el
 * CPU directamente) se ejecuta sobre el estado del RDP de la HLE: los
 * comandos de estado y de carga con gfx_rdp_command, los rectángulos con
 * gfx_texrect y los triángulos crudos (0xC8–0xCF) con gfx_rdp_triangle,
 * que los convierte en triángulos para el RSX con sus atributos evaluados
 * en las esquinas. Con xbus los comandos vienen de la DMEM (gfx_io_dl lo
 * sabe por el trabajo) y lo demás (texturas, imágenes) de la RDRAM.
 *
 * Cada tramo es un trabajo (GFX_JOB_DPC) y produce su salida como una
 * tarea de la HLE; el estado del RDP persiste entre tramos.
 */
#include "rsp/gfx/hle.h"

#define G_TEXRECT     0xE4u
#define G_TEXRECTFLIP 0xE5u

#ifdef GFX_CENSUS
/* Censo gráfico (src/host/gfxcheck.c): cuántos comandos de cada opcode del RDP llegaron. */
uint32_t gfx_dpc_hist[64];
#endif

void gfx_dl_dpc(struct gfx_state *g)
{
	uint32_t pc = g->job->dl, end = g->job->dl_end;
	uint32_t w[44];
	while (pc + 8 <= end) {
		/* El RDP reconoce el comando por sus 6 bits bajos: los dos de arriba
		 * no cuentan, y los juegos que escriben sus propias listas los
		 * ponen como quieran (Killer Instinct Gold: 0x2F y 0xBA en vez de
		 * 0xEF y 0xFA, y 0x24 por 0xE4). La HLE los conoce con los dos en 1,
		 * como los trae una lista de F3D. */
		unsigned words = 1;
		for (unsigned k = 0; k < words; k++) {
			if (!gfx_io_dl(g, pc + 8 * k, &w[2 * k], &w[2 * k + 1])) {
				g->res->dl_errors++;
				return;
			}
			if (!k) {
				w[0] |= 0xC0000000u;
				words = gfx_rdp_raw_words(w[0] >> 24);
				if (pc + 8 * words > end)
					return;  /* el comando sigue en el próximo tramo */
			}
		}
		pc += 8 * words;
		g->res->commands++;
		uint32_t op = w[0] >> 24;
#ifdef GFX_CENSUS
		gfx_dpc_hist[op & 63]++;
#endif
		if (op >= 0xC8 && op <= 0xCF)
			gfx_rdp_triangle(g, w);
		else if (op == G_TEXRECT || op == G_TEXRECTFLIP)
			gfx_texrect(g, w[0], w[1], w[2], w[3], op == G_TEXRECTFLIP);
		else
			gfx_rdp_command(g, w[0], w[1]);
	}
}
