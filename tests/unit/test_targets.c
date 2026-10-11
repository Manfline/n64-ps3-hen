/*
 * Salsa64 — pruebas de la elección del destino de render que se presenta
 * (src/rdp/targets.c): dos framebuffers alternos en direcciones distintas
 * (que se solapan en la cuenta) o en la misma dirección con otro ancho, una
 * color image de otro ancho sobre el framebuffer, el VI sostenido sin
 * dibujar y el reciclado de ranuras.
 */
#include "test.h"

#include <string.h>

#include "rdp/targets.h"

#define SIZ_16b 2u

static struct rdp_targets s;

static struct salsa64_vi_frame frame(uint32_t origin, uint32_t width, uint32_t height)
{
	struct salsa64_vi_frame f;
	memset(&f, 0, sizeof f);
	f.type = SALSA64_VI_RGBA5551;
	f.origin = origin;
	f.width = width;
	f.height = height;
	f.stride = width * 2;
	f.bpp = 2;
	return f;
}

/* Una tarea que dibuja en la color image (addr, width). */
static int draw(uint32_t addr, uint32_t width)
{
	bool fresh;
	int i = rdp_targets_get(&s, addr, width, SIZ_16b, 0, &fresh);
	rdp_targets_drawn(&s, i);
	return i;
}

static struct rdp_present present(uint32_t field, uint32_t origin, uint32_t width, uint32_t height)
{
	struct salsa64_vi_frame f = frame(origin, width, height);
	struct rdp_present p;
	rdp_targets_present(&s, &f, field, &p);
	return p;
}

/* Banjo-Kazooie: dos framebuffers de 292 × 216 (584 × 216 bytes aparte,
 * menos que las 219 líneas que se le suponen a uno de 292), VI en la
 * línea 1 de cada uno, alternando cada 2 campos; el RDP dibuja el otro
 * mientras se muestra uno. Debe presentarse el que empieza más cerca del
 * origen, no el dibujado hace menos (que lo cubre con sus últimas líneas). */
static void targets_two_buffers_overlapping(void)
{
	const uint32_t A = 0x3a5d00, B = 0x3c49c0, LINE = 292 * 2;
	rdp_targets_init(&s);
	int a = draw(A, 292);
	struct rdp_present p = present(1, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.y0, 1);
	CHECK_EQ(p.w, 292);
	CHECK_EQ(p.h, 213);
	/* Mientras se muestra A se dibuja B; luego el VI pasa a B. */
	int b = draw(B, 292);
	CHECK(b != a);
	p = present(2, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.age, 1);  /* A se dibujó en el campo 1 */
	p = present(3, B + LINE, 292, 213);
	CHECK_EQ(p.slot, b);
	CHECK_EQ(p.y0, 1);
	CHECK_EQ(p.age, 1);  /* B, dibujado antes de presentar el campo 2 */
	/* Mientras se muestra B se dibuja A otra vez: A cubre el origen de B
	 * (línea 217 de sus 219) y es más reciente, pero B empieza más cerca. */
	CHECK_EQ(draw(A, 292), a);
	p = present(4, B + LINE, 292, 213);
	CHECK_EQ(p.slot, b);
	CHECK_EQ(p.h, 213);
	p = present(5, A + LINE, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(s.created, 2);
}

/* GoldenEye: la misma dirección con un framebuffer de 320 y uno de 440
 * (cambio de resolución); se presenta el del ancho del VI. */
static void targets_same_addr_two_widths(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int w320 = draw(A, 320);
	struct rdp_present p = present(1, A + 640, 320, 237);
	CHECK_EQ(p.slot, w320);
	int w440 = draw(A, 440);
	CHECK(w440 != w320);
	p = present(2, A + 880, 440, 326);
	CHECK_EQ(p.slot, w440);
	CHECK_EQ(p.w, 440);
	/* El de 320 sigue reciente y cubre el origen, pero el VI muestra 440. */
	CHECK_EQ(draw(A, 320), w320);
	p = present(3, A + 880, 440, 326);
	CHECK_EQ(p.slot, w440);
	p = present(4, A + 640, 320, 237);
	CHECK_EQ(p.slot, w320);
}

/* Zelda en pausa: cada tarea pone dos color images de 64 de ancho (una en
 * la dirección del framebuffer) y después el framebuffer de 320; el VI
 * alterna dos framebuffers. Se presenta siempre el de 320 y ninguna de
 * las de 64 lo desplaza de su ranura. */
static void targets_aux_image_on_framebuffer(void)
{
	const uint32_t A = 0x3b5000, B = 0x3da800, X = 0x3b8800, Y = 0x3de000;
	rdp_targets_init(&s);
	int a = draw(A, 320), b = draw(B, 320);
	struct rdp_present p = present(1, A + 0x280, 320, 237);
	CHECK_EQ(p.slot, a);
	for (uint32_t field = 2; field < 40; field++) {
		bool even = (field & 1) == 0;
		draw(even ? Y : X, 64);
		draw(even ? B : A, 64);
		CHECK_EQ(draw(even ? B : A, 320), even ? b : a);
		/* El VI muestra el otro framebuffer (el dibujado el campo anterior). */
		p = present(field, (even ? A : B) + 0x280, 320, 237);
		CHECK_EQ(p.slot, even ? a : b);
		CHECK_EQ(p.y0, 1);
		CHECK_EQ(p.w, 320);
		CHECK_EQ(p.h, 237);
	}
	CHECK_EQ(s.created, 6);
}

/* El juego deja de dibujar y el VI sostiene el mismo origen (GoldenEye
 * mientras carga): se sigue presentando el destino, más allá de los
 * campos en que pasaría por viejo; al cambiar el origen, ya no. */
static void targets_held_frame(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int a = draw(A, 440);
	struct rdp_present p = present(1, A + 880, 440, 326);
	CHECK_EQ(p.slot, a);
	for (uint32_t field = 2; field < 2 + 3 * RDP_TARGETS_STALE; field++) {
		p = present(field, A + 880, 440, 326);
		CHECK_EQ(p.slot, a);
	}
	p = present(100, A + 880 + 0x40000, 440, 326);
	CHECK_EQ(p.slot, -1);
	/* Vuelve al origen de antes, pero ya se perdió el hilo: la RDRAM. */
	p = present(101, A + 880, 440, 326);
	CHECK_EQ(p.slot, -1);
}

/* SM64 apaga el VI 26 campos al cambiar de escena y sigue dibujando: lo
 * dibujado entonces cuenta como del campo en que vuelve la imagen. */
static void targets_drawn_while_blank(void)
{
	const uint32_t A = 0x3b5000;
	rdp_targets_init(&s);
	int a = draw(A, 320);
	struct rdp_present p = present(10, A, 320, 240);
	CHECK_EQ(p.slot, a);
	draw(A, 320);  /* en negro durante 60 campos: sin presentar */
	p = present(70, A, 320, 240);
	CHECK_EQ(p.slot, a);
	/* Sin dibujar nada en 60 campos y con otro origen entre medio: viejo. */
	p = present(100, A + 0x100000, 320, 240);
	CHECK_EQ(p.slot, -1);
	p = present(131, A, 320, 240);
	CHECK_EQ(p.slot, -1);
}

/* Reciclado: con las ranuras llenas no se toma la presentada ni, mientras
 * haya otras, un framebuffer del ancho del VI; primero las viejas, luego
 * las imágenes auxiliares. */
static void targets_recycle_order(void)
{
	const uint32_t A = 0x3b5000, B = 0x3da800;
	rdp_targets_init(&s);
	int a = draw(A, 320), b = draw(B, 320);
	present(1, A + 0x280, 320, 237);
	/* Llenar el resto con imágenes auxiliares de 64, una por campo. */
	for (int i = 2; i < RDP_TARGETS_MAX; i++) {
		draw(0x100000 + 0x2000 * i, 64);
		present(i, A + 0x280, 320, 237);
	}
	CHECK_EQ(s.created, RDP_TARGETS_MAX);
	/* Una más: sale la auxiliar más vieja (la del campo 2). */
	bool fresh;
	int n = rdp_targets_get(&s, 0x200000, 64, SIZ_16b, 0, &fresh);
	CHECK(fresh);
	CHECK(n != a && n != b);
	CHECK_EQ(s.t[n].addr, 0x200000);
	/* Los framebuffers siguen. */
	CHECK_EQ(draw(A, 320), a);
	CHECK_EQ(draw(B, 320), b);
	CHECK_EQ(s.created, RDP_TARGETS_MAX + 1);
	/* Si una ranura envejece (sin dibujar RDP_TARGETS_STALE campos), es la
	 * primera en irse aunque sea un framebuffer: B no se dibuja más. */
	for (uint32_t field = RDP_TARGETS_MAX; field < RDP_TARGETS_MAX + RDP_TARGETS_STALE + 2; field++) {
		draw(A, 320);
		for (int i = 2; i < RDP_TARGETS_MAX; i++)
			draw(0x100000 + 0x2000 * i, 64);
		draw(0x200000, 64);
		present(field, A + 0x280, 320, 237);
	}
	n = rdp_targets_get(&s, 0x300000, 64, SIZ_16b, 0, &fresh);
	CHECK(fresh);
	CHECK_EQ(n, b);
}

/* Conker: a mitad del cuadro dibuja en una color image CI8 de 64 de ancho
 * con tijeras hasta la línea 65 (es de 64 × 64): el destino nace con ese
 * alto y no con 48; si una tarea posterior llega más abajo, crece (se
 * vuelve a crear). El framebuffer de 292 × 216 no crece con 216. */
static void targets_height_from_scissor(void)
{
	const uint32_t A = 0x3d6300, X = 0xde080;
	rdp_targets_init(&s);
	bool fresh;
	int a = rdp_targets_get(&s, A, 292, SIZ_16b, 216, &fresh);
	CHECK(fresh);
	CHECK_EQ(s.t[a].lh, 219);
	int x = rdp_targets_get(&s, X, 64, 1u, 65, &fresh);
	CHECK(fresh);
	CHECK_EQ(s.t[x].lw, 64);
	CHECK_EQ(s.t[x].lh, 65);
	CHECK_EQ(s.t[x].bpp, 1);
	CHECK_EQ(rdp_targets_get(&s, X, 64, 1u, 60, &fresh), x);
	CHECK(!fresh);
	CHECK_EQ(s.t[x].lh, 65);
	CHECK_EQ(rdp_targets_get(&s, X, 64, 1u, 128, &fresh), x);
	CHECK(fresh);
	CHECK_EQ(s.t[x].lh, 128);
	CHECK_EQ(rdp_targets_get(&s, A, 292, SIZ_16b, 0, &fresh), a);
	CHECK(!fresh);
	CHECK_EQ(s.created, 3);
	/* La presentación usa el alto real: el VI muestra 213 líneas de A. */
	rdp_targets_drawn(&s, a);
	struct rdp_present p = present(1, A + 584, 292, 213);
	CHECK_EQ(p.slot, a);
	CHECK_EQ(p.h, 213);
}

/* Escala efectiva: automática por altura de pantalla, pedida (con tope en
 * 5) y reducida cuando el peor caso no cabe en el presupuesto de memoria
 * local del RSX. */
static void targets_scale_pick(void)
{
	bool cut = true;
	CHECK_EQ(rdp_scale_pick(0, 720, &cut), 3u);
	CHECK(!cut);
	CHECK_EQ(rdp_scale_pick(0, 1080, &cut), 4u);
	CHECK(!cut);
	CHECK_EQ(rdp_scale_pick(0, 480, &cut), 2u);
	CHECK_EQ(rdp_scale_pick(0, 100, NULL), 1u);
	for (uint32_t s = 1; s <= 5; s++) {
		CHECK_EQ(rdp_scale_pick(s, 720, &cut), s);
		CHECK(!cut);
	}
	CHECK_EQ(rdp_scale_pick(9, 720, &cut), 5u);  /* el tope no es por memoria */
	CHECK(!cut);
	/* Peor caso: 10 superficies de 320 × 240 × 25 (7,68 MB c/u) = 73 MiB. */
	CHECK(rdp_vram_worst(5) < RDP_VRAM_BUDGET);
	CHECK(rdp_vram_worst(5) > 70u << 20);
	CHECK_EQ(rdp_surface_bytes(1600, 1200), 1600u * 4 * 1200);
	CHECK_EQ(rdp_surface_bytes(321, 1), 1408u);  /* pitch a 128 */
	/* Con un presupuesto más chico baja la escala y avisa. */
	CHECK_EQ(rdp_scale_pick_budget(5, 720, 60u << 20, &cut), 4u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(0, 1080, 40u << 20, &cut), 3u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(5, 720, 1, &cut), 1u);
	CHECK(cut);
	CHECK_EQ(rdp_scale_pick_budget(3, 720, 100u << 20, &cut), 3u);
	CHECK(!cut);
	/* Escala por destino: el objetivo son 240 × escala líneas. */
	CHECK_EQ(rdp_target_scale(5, 320, 240), 5u);
	CHECK_EQ(rdp_target_scale(5, 640, 480), 2u);
	CHECK_EQ(rdp_target_scale(5, 64, 48), 5u);     /* auxiliar chica, con tope */
	CHECK_EQ(rdp_target_scale(5, 400, 300), 4u);
	CHECK_EQ(rdp_target_scale(3, 640, 480), 1u);
	CHECK_EQ(rdp_target_scale(3, 320, 240), 3u);
	CHECK_EQ(rdp_target_scale(1, 320, 240), 1u);
	CHECK_EQ(rdp_target_scale(5, 640, 2000), 1u);  /* nunca 0 */
	CHECK_EQ(rdp_target_scale(5, 5000, 100), 1u);  /* límite de 4096 */
	/* Ningún destino (en píxeles) pasa de uno de 320 × 240 a la escala-objetivo. */
	for (uint32_t g = 2; g <= 5; g++)  /* a 1× una de 640 × 480 queda nativa (más grande) */
		for (uint32_t w = 64; w <= 640; w += 16) {
			uint32_t lh = w * 3 / 4, sc = rdp_target_scale(g, w, lh);
			CHECK(w * sc * lh * sc <= 320 * g * 240 * g * 102 / 100);  /* lh = 3/4 w redondeado */
		}
	CHECK(640 * RDP_SCALE_MAX <= 4096);
}

void t_suite_targets(void)
{
	RUN(targets_height_from_scissor);
	RUN(targets_two_buffers_overlapping);
	RUN(targets_same_addr_two_widths);
	RUN(targets_aux_image_on_framebuffer);
	RUN(targets_held_frame);
	RUN(targets_drawn_while_blank);
	RUN(targets_recycle_order);
	RUN(targets_scale_pick);
}
