/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Salsa64 — repetición de un volcado de la HLE en el RSX. Ver replay.h.
 */
#include "replay.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <rsxkit/rsxkit.h>

#include "platform/ps3/log.h"
#include "rdp/rdp.h"
#include "screenshot.h"

#define REPLAY_FRAMES 30
#define SHOT_FRAME    10

int salsa64_replay(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		salsa64_log("repetición: no se pudo abrir %s", path);
		return -1;
	}
	struct gfx_dump_header h;
	if (fread(&h, sizeof h, 1, f) != 1 || h.magic != GFX_DUMP_MAGIC) {
		fclose(f);
		salsa64_log("repetición: %s no es un volcado de la HLE", path);
		return -1;
	}
	const struct salsa64_gfx_sink *sink = salsa64_rdp_sink();
	struct gfx_job job;
	struct gfx_result res;
	memset(&job, 0, sizeof job);
	memset(&res, 0, sizeof res);
	sink->acquire(sink->ctx, &job);
	if (h.cmd_bytes > job.cmd_cap || h.vtx_bytes > job.vtx_cap || h.tex_bytes > job.tex_cap ||
	    fread((void *)(uintptr_t)job.cmd_ea, 1, h.cmd_bytes, f) != h.cmd_bytes ||
	    fread((void *)(uintptr_t)job.vtx_ea, 1, h.vtx_bytes, f) != h.vtx_bytes ||
	    fread((void *)(uintptr_t)job.tex_ea, 1, h.tex_bytes, f) != h.tex_bytes) {
		fclose(f);
		salsa64_log("repetición: volcado truncado o demasiado grande");
		return -1;
	}
	fclose(f);
	res.cmd_bytes = h.cmd_bytes;
	res.vtx_bytes = h.vtx_bytes;
	res.tex_bytes = h.tex_bytes;

	/* La primera color image define la imagen que se presenta. */
	struct salsa64_vi_frame vf;
	memset(&vf, 0, sizeof vf);
	for (uint32_t off = 0; off + sizeof(struct gfx_cmd) <= h.cmd_bytes;) {
		const struct gfx_cmd *c = (const struct gfx_cmd *)((const uint8_t *)(uintptr_t)job.cmd_ea + off);
		if (c->op == GFX_OP_END || !c->size)
			break;
		if (c->op == GFX_OP_TARGET) {
			uint32_t bpp = (c->c & 0xFF) == 3 ? 4 : 2;
			vf.type = bpp == 4 ? SALSA64_VI_RGBA8888 : SALSA64_VI_RGBA5551;
			vf.width = c->b;
			vf.height = c->b * 3 / 4 - 3;
			vf.bpp = bpp;
			vf.stride = c->b * bpp;
			vf.origin = c->a + vf.stride;
			break;
		}
		off += c->size;
	}
	if (!vf.width) {
		salsa64_log("repetición: el volcado no tiene color image");
		return -1;
	}
	salsa64_log("repetición de %s: %u bytes de comandos, %u de vértices, %u de texturas; imagen %ux%u",
		    path, h.cmd_bytes, h.vtx_bytes, h.tex_bytes, vf.width, vf.height);

	const rsxkit_view view = { RSXKIT_SCALE_FIT, RSXKIT_FILTER_LINEAR, 4.0f / 3.0f };
	rsxkit_rect r = { 0, 0, 0, 0 };
	salsa64_rdp_present(&vf, 1, &view, NULL, &r);  /* fija el campo de la tarea */
	sink->done(sink->ctx, &job, &res);
	salsa64_rdp_flush(~0u);
	for (int i = 0; i < REPLAY_FRAMES; i++) {
		rsxkit_frame_begin(0x000000);
		bool shown = salsa64_rdp_present(&vf, 1, &view, NULL, &r);
		if (i == SHOT_FRAME) {
			rsxkit_finish();
			salsa64_screenshot(SALSA64_DATA_DIR "/replay.ppm", &vf, &r);
			salsa64_log("repetición: pantalla en %s (%s)", SALSA64_DATA_DIR "/replay.ppm",
				    shown ? "destino del RSX" : "SIN destino");
		}
		rsxkit_frame_end();
	}
	return 0;
}
