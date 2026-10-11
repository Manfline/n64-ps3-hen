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

void gfx_dl_dpc(struct gfx_state *g)
{
	uint32_t pc = g->job->dl, end = g->job->dl_end;
	uint32_t w[44];
	while (pc + 8 <= end) {
		if (!gfx_io_dl(g, pc, &w[0], &w[1])) {
			g->res->dl_errors++;
			return;
		}
		uint32_t op = w[0] >> 24;
		unsigned words = gfx_rdp_raw_words(op);
		if (pc + 8 * words > end)
			return;  /* el comando sigue en el próximo tramo */
		for (unsigned k = 1; k < words; k++) {
			if (!gfx_io_dl(g, pc + 8 * k, &w[2 * k], &w[2 * k + 1])) {
				g->res->dl_errors++;
				return;
			}
		}
		pc += 8 * words;
		g->res->commands++;
		if (op >= 0xC8 && op <= 0xCF)
			gfx_rdp_triangle(g, w);
		else if (op == G_TEXRECT || op == G_TEXRECTFLIP)
			gfx_texrect(g, w[0], w[1], w[2], w[3], op == G_TEXRECTFLIP);
		else
			gfx_rdp_command(g, w[0], w[1]);
	}
}
