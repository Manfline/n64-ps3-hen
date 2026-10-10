/*
 * Cell64 — Fase 0: triángulo en el RSX.
 *
 * Inicializa el RSX con doble búfer, dibuja un triángulo con color por
 * vértice (shaders Cg compilados offline con cgcomp) y en el cuadro
 * CHECK_FRAME lee píxeles del framebuffer para verificar el dibujo: esquina
 * con el color de fondo y, cerca de cada vértice, el canal dominante de ese
 * vértice. Sale con X/O, desde el menú del sistema o, en modo de prueba
 * automática (platform/ps3/autotest.h), tras FPS_FRAMES cuadros.
 *
 * En RPCS3 la verificación necesita "Write Color Buffers" activado; sin eso
 * el framebuffer no se copia a la memoria emulada (docs/rsx-notes.md).
 *
 * Inicialización basada en el sample rsxtest de PSL1GHT (MIT), ver
 * docs/THIRD_PARTY.md.
 */
#include <io/pad.h>
#include <malloc.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <string.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>

#include "platform/ps3/autotest.h"
#include "platform/ps3/log.h"
#include "triangle_fpo.h"
#include "triangle_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define CB_SIZE     0x80000            /* búfer de comandos: 512 KB */
#define HOST_SIZE   (32 * 1024 * 1024) /* memoria principal mapeada al RSX */
#define FB_COUNT    2
#define LABEL_INDEX 255
#define CLEAR_COLOR 0x00102040u        /* X8R8G8B8 */
#define CHECK_FRAME 30
#define FPS_FRAMES  120

typedef struct {
	float x, y, z;
	float r, g, b, a;
} vertex_t;

static const vertex_t tri[3] = {
	{ -0.6f, -0.6f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f }, /* abajo-izquierda: rojo */
	{  0.6f, -0.6f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f }, /* abajo-derecha: verde  */
	{  0.0f,  0.6f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f }, /* arriba: azul          */
};

static gcmContextData *ctx;
static u32 width, height;
static u32 color_pitch, depth_pitch, depth_offset;
static u32 color_offset[FB_COUNT];
static u32 *color_buf[FB_COUNT];
static u32 *depth_buf;
static u32 cur_fb;
static u32 label_val = 1;
static volatile int running = 1;

static rsxVertexProgram *vpo;
static rsxFragmentProgram *fpo;
static void *vp_ucode;
static u32 fp_offset;
static vertex_t *vbuf;

static void wait_finish(void)
{
	rsxSetWriteBackendLabel(ctx, LABEL_INDEX, label_val);
	rsxFlushBuffer(ctx);
	while (*(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != label_val)
		sysUsleep(30);
	label_val++;
}

static void wait_idle(void)
{
	rsxSetWriteBackendLabel(ctx, LABEL_INDEX, label_val);
	rsxSetWaitLabel(ctx, LABEL_INDEX, label_val);
	label_val++;
	wait_finish();
}

static int init_video(void)
{
	static const u32 ids[] = { VIDEO_RESOLUTION_720, VIDEO_RESOLUTION_1080,
				   VIDEO_RESOLUTION_480, VIDEO_RESOLUTION_576 };
	videoResolution res;
	s32 id = -1;

	for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
		if (videoGetResolutionAvailability(VIDEO_PRIMARY, ids[i], VIDEO_ASPECT_AUTO, 0) == 1 &&
		    videoGetResolution(ids[i], &res) == 0) {
			id = ids[i];
			break;
		}
	}
	if (id < 0) {
		cell64_log("ninguna resolución disponible");
		return -1;
	}

	videoConfiguration cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.resolution = (u8)id;
	cfg.format = VIDEO_BUFFER_FORMAT_XRGB;
	cfg.aspect = VIDEO_ASPECT_AUTO;
	cfg.pitch = res.width * 4;
	if (videoConfigure(VIDEO_PRIMARY, &cfg, NULL, 0) != 0) {
		cell64_log("videoConfigure falló");
		return -1;
	}

	width = res.width;
	height = res.height;
	return 0;
}

static int init_screen(void)
{
	void *host = memalign(1024 * 1024, HOST_SIZE);
	if (!host || rsxInit(&ctx, CB_SIZE, HOST_SIZE, host) != 0 || !ctx) {
		cell64_log("rsxInit falló");
		return -1;
	}
	if (init_video() != 0)
		return -1;

	wait_idle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);

	color_pitch = width * 4;
	depth_pitch = width * 4;
	for (u32 i = 0; i < FB_COUNT; i++) {
		color_buf[i] = rsxMemalign(64, height * color_pitch);
		if (!color_buf[i])
			return -1;
		rsxAddressToOffset(color_buf[i], &color_offset[i]);
		gcmSetDisplayBuffer(i, color_offset[i], color_pitch, width, height);
	}
	depth_buf = rsxMemalign(64, height * depth_pitch);
	if (!depth_buf)
		return -1;
	rsxAddressToOffset(depth_buf, &depth_offset);
	return 0;
}

static int init_programs(void)
{
	u32 vp_size, fp_size;
	void *fp_ucode;

	vpo = (rsxVertexProgram *)triangle_vpo;
	fpo = (rsxFragmentProgram *)triangle_fpo;
	rsxVertexProgramGetUCode(vpo, &vp_ucode, &vp_size);
	rsxFragmentProgramGetUCode(fpo, &fp_ucode, &fp_size);

	/* El RSX lee el fragment program desde su memoria. */
	u32 *fp_buf = rsxMemalign(64, fp_size);
	vbuf = rsxMemalign(128, sizeof tri);
	if (!fp_buf || !vbuf)
		return -1;
	memcpy(fp_buf, fp_ucode, fp_size);
	rsxAddressToOffset(fp_buf, &fp_offset);
	memcpy(vbuf, tri, sizeof tri);

	cell64_log("vertex program %u bytes, fragment program %u bytes", vp_size, fp_size);
	return 0;
}

static void set_render_target(u32 index)
{
	gcmSurface sf;
	memset(&sf, 0, sizeof sf);
	sf.colorFormat = GCM_SURFACE_X8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = color_offset[index];
	sf.colorPitch[0] = color_pitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorPitch[i] = 64;
	}
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depth_offset;
	sf.depthPitch = depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = width;
	sf.height = height;
	rsxSetSurface(ctx, &sf);
}

static void set_draw_env(void)
{
	f32 scale[4] = { width * 0.5f, height * -0.5f, 0.5f, 0.0f };
	f32 offset[4] = { width * 0.5f, height * 0.5f, 0.5f, 0.0f };

	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetViewport(ctx, 0, 0, width, height, 0.0f, 1.0f, scale, offset);
	rsxSetScissor(ctx, 0, 0, width, height);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	for (int i = 0; i < 8; i++)
		rsxSetViewportClip(ctx, i, width, height);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
}

static void draw_frame(void)
{
	u32 off;

	set_render_target(cur_fb);
	set_draw_env();
	rsxSetClearColor(ctx, CLEAR_COLOR);
	rsxSetClearDepthStencil(ctx, 0xffffff00);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A |
				     GCM_CLEAR_S | GCM_CLEAR_Z);

	rsxAddressToOffset(&vbuf[0].x, &off);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, off, sizeof(vertex_t), 3,
				 GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxAddressToOffset(&vbuf[0].r, &off);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, off, sizeof(vertex_t), 4,
				 GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);

	rsxLoadVertexProgram(ctx, vpo, vp_ucode);
	rsxLoadFragmentProgramLocation(ctx, fpo, fp_offset, GCM_LOCATION_RSX);
	rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, 0, 3);
}

static void flip(void)
{
	static int first = 1;

	if (!first) {
		while (gcmGetFlipStatus() != 0)
			sysUsleep(200);
	}
	gcmResetFlipStatus();
	gcmSetFlip(ctx, cur_fb);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);
	cur_fb ^= 1;
	first = 0;
}

static u32 pixel_at(const u32 *fb, float cx, float cy)
{
	u32 px = (u32)((cx + 1.0f) * 0.5f * width);
	u32 py = (u32)((1.0f - cy) * 0.5f * height);
	return fb[py * (color_pitch / 4) + px] & 0x00FFFFFFu;
}

/* 0 = rojo, 1 = verde, 2 = azul, -1 = sin dominante clara. */
static int dominant(u32 rgb)
{
	u32 c[3] = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF };
	for (int i = 0; i < 3; i++)
		if (c[i] > c[(i + 1) % 3] + 32 && c[i] > c[(i + 2) % 3] + 32)
			return i;
	return -1;
}

static int check_frame(const u32 *fb)
{
	const u32 corner = pixel_at(fb, 0.95f, 0.95f);
	const u32 near_r = pixel_at(fb, -0.5f, -0.55f);
	const u32 near_g = pixel_at(fb, 0.5f, -0.55f);
	const u32 near_b = pixel_at(fb, 0.0f, 0.5f);
	const u32 center = pixel_at(fb, 0.0f, 0.0f);

	cell64_log("píxeles: esquina=%06x rojo=%06x verde=%06x azul=%06x centro=%06x",
		   corner, near_r, near_g, near_b, center);
	return corner == (CLEAR_COLOR & 0x00FFFFFFu) && dominant(near_r) == 0 &&
	       dominant(near_g) == 1 && dominant(near_b) == 2 && center != corner;
}

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param, (void)user;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static int exit_pressed(void)
{
	padInfo info;
	padData data;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (int i = 0; i < MAX_PADS; i++) {
		if (info.status[i] && ioPadGetData(i, &data) == 0 &&
		    (data.BTN_CROSS || data.BTN_CIRCLE))
			return 1;
	}
	return 0;
}

int main(void)
{
	cell64_log_init("rsx-triangle");

	if (init_screen() != 0 || init_programs() != 0) {
		cell64_log("RESULTADO rsx-triangle: FALLO al inicializar");
		cell64_log_shutdown();
		return 1;
	}
	cell64_log("RSX listo: %ux%u", width, height);

	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	const int autotest = cell64_autotest();

	u64 t0 = sysGetSystemTime();
	for (u32 frame = 0; running; frame++) {
		sysUtilCheckCallback();
		if (exit_pressed())
			break;

		draw_frame();
		if (frame == CHECK_FRAME) {
			wait_finish();
			const int ok = check_frame(color_buf[cur_fb]);
			cell64_log("RESULTADO rsx-triangle: %s", ok ? "OK" : "FALLO");
		}
		flip();

		if (frame + 1 == FPS_FRAMES) {
			u64 us = sysGetSystemTime() - t0;
			cell64_log("%u cuadros en %u ms (vsync)", FPS_FRAMES, (unsigned)(us / 1000));
			if (autotest)
				break;
		}
	}

	gcmSetWaitFlip(ctx);
	rsxFinish(ctx, 1);
	ioPadEnd();
	cell64_log("salida normal");
	cell64_log_shutdown();
	return 0;
}
