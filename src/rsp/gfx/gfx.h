/*
 * Salsa64 — HLE del microcódigo gráfico (F3DEX2) y del RDP: interfaz.
 *
 * El mismo código corre en un SPE (src/spu/gfx, la configuración normal en
 * la PS3), en el PPE y en el host (backend síncrono, pruebas). Lee la
 * display list de la RDRAM y produce, por tarea:
 *
 *   vértices   gfx_vertex (32 bytes), en el formato que lee el RSX: posición
 *              homogénea en píxeles de la N64, s/t en texels y color
 *   comandos   registros gfx_cmd: cambios de destino (color image), borrado
 *              de profundidad y dibujos (gfx_draw: estado completo + un tramo
 *              de vértices, triángulos sueltos)
 *   texturas   texels RGBA8888 decodificados de la TMEM, en un arena
 *
 * Quien consume (src/rdp en el PPE) traduce los comandos a lib/rsxkit. Los
 * registros se escriben en el orden de bytes de quien produce y se leen en
 * el mismo (SPU y PPE son big-endian).
 *
 * Nada aquí depende de PSL1GHT: todo compila con gcc nativo.
 */
#ifndef SALSA64_RSP_GFX_GFX_H
#define SALSA64_RSP_GFX_GFX_H

#include <stdint.h>

/* ---- Trabajo y resultado ---------------------------------------------------
 * Caben en el descriptor y el resultado de 112 bytes de lib/spukit. */

#define GFX_JOB_TASK 1  /* kind del trabajo en spukit */

struct gfx_job {
	uint64_t rdram_ea;   /* dirección de la RDRAM emulada (EA del PPE) */
	uint64_t vtx_ea;     /* salida: vértices */
	uint64_t cmd_ea;     /* salida: comandos */
	uint64_t tex_ea;     /* salida: arena de texturas */
	uint32_t rdram_size;
	uint32_t vtx_cap;    /* capacidades en bytes */
	uint32_t cmd_cap;
	uint32_t tex_cap;
	uint32_t gbi;        /* enum salsa64_gbi */
	uint32_t dl;         /* data_ptr del OSTask: primera display list */
	uint32_t flags;      /* GFX_JOB_* */
	uint32_t reserved[5];
};

#define GFX_JOB_RESET 0x1u  /* olvidar el estado del RDP (reset de la consola) */

struct gfx_result {
	uint32_t vtx_bytes, cmd_bytes, tex_bytes;
	uint32_t flags;          /* GFX_RES_* */
	uint32_t commands;       /* comandos de display list ejecutados */
	uint32_t triangles;
	uint32_t rects;
	uint32_t draws;          /* registros gfx_draw */
	uint32_t textures;       /* texturas decodificadas */
	uint32_t tex_reused;     /* usos de una textura ya decodificada */
	uint32_t unknown_cmds;   /* comandos sin implementar */
	uint32_t last_unknown;   /* opcode del último */
	uint32_t approx_cc;      /* combiners aproximados */
	uint32_t dl_errors;      /* direcciones fuera de la RDRAM, pila llena */
	/* Tiempo por etapa en ticks de gfx_io_clock (en el SPE, el timebase;
	 * en el host y el PPE, 0): total, vértices, estado de dibujo (combiner
	 * y texturas), decodificación de texturas y cargas a la TMEM. */
	uint32_t t_total, t_vtx, t_state, t_tex, t_load;
	uint32_t loads;          /* cargas a la TMEM (LOADBLOCK, LOADTILE, LOADTLUT) */
	uint32_t load_reads;     /* lecturas de la RDRAM para esas cargas */
	uint32_t reserved[5];
};

#define GFX_RES_FULLSYNC     0x01u  /* G_RDPFULLSYNC: interrupción del DP */
#define GFX_RES_VTX_FULL     0x02u  /* se acabó un búfer de salida */
#define GFX_RES_CMD_FULL     0x04u
#define GFX_RES_TEX_FULL     0x08u
#define GFX_RES_LOOP         0x10u  /* demasiados comandos: se cortó */
#define GFX_RES_LOAD_UCODE   0x20u  /* G_LOAD_UCODE (cambio de microcódigo) */

/* ---- Vértice de salida -------------------------------------------------------
 * x, y: píxeles del framebuffer de la N64 multiplicados por w; z: profundidad
 * en [0, 1] multiplicada por w; w: la w de recorte (positiva delante de la
 * cámara). s, t: coordenadas de textura en texels (ya con la escala de
 * G_TEXTURE, antes de la tile). Color en bytes R, G, B, A. */
struct gfx_vertex {
	float x, y, z, w;
	float s, t;
	uint8_t r, g, b, a;
	uint32_t pad;
};

/* ---- Comandos -----------------------------------------------------------------
 * Registros de tamaño múltiplo de 16 bytes; cada uno empieza con gfx_cmd. */

enum gfx_op {
	GFX_OP_END = 0,
	GFX_OP_TARGET,  /* a = dirección física de la color image, b = ancho,
	                   c = formato << 8 | tamaño (G_IM_FMT_*, G_IM_SIZ_*) */
	GFX_OP_ZCLEAR,  /* a = dirección de la z image, b = valor (16 bits de la
	                   N64), c = 0; gfx_zclear */
	GFX_OP_DRAW,    /* gfx_draw */
};

struct gfx_cmd {
	uint16_t op;
	uint16_t size;  /* bytes del registro completo */
	uint32_t a, b, c;
};

struct gfx_zclear {
	struct gfx_cmd h;
	uint16_t rect[4];  /* x0, y0, x1, y1 en píxeles (x1, y1 exclusivos) */
	uint32_t pad[2];
};

/* Textura de un dibujo: texels RGBA8888 (bytes R, G, B, A) en el arena. */
struct gfx_tex {
	uint32_t offset;    /* desde el inicio del arena */
	uint16_t width, height;
	uint16_t pitch;     /* bytes por fila */
	uint8_t flags;      /* GFX_TEX_* */
	uint8_t pad;
	uint32_t key;       /* identidad de la textura (para quien cachee) */
	/* s, t (texels de la N64) → coordenadas normalizadas de la textura:
	 * u = s * xf[0] + xf[2], v = t * xf[1] + xf[3]. */
	float xf[4];
};

#define GFX_TEX_WRAP_S(f)   ((f) & 3u)         /* GFX_WRAP_* */
#define GFX_TEX_WRAP_T(f)   (((f) >> 2) & 3u)
#define GFX_TEX_BILINEAR    0x10u
enum { GFX_WRAP_CLAMP = 0, GFX_WRAP_REPEAT = 1, GFX_WRAP_MIRROR = 2 };

/* Combinación lineal que evalúa el RSX (lib/rsxkit: RSXKIT_PROG_LINCOMB):
 *   X_k = k[k][0] + k[k][1] * C + k[k][2] * C.aaaa     (C: color del vértice)
 *   color = T0 * X_0 + T0.aaaa * X_1 + T1 * X_2 + T1.aaaa * X_3 + X_4
 * con T0, T1 las dos texturas. Cubre exactamente los combiners de la N64
 * que no multiplican una textura por otra ni el sombreado por sí mismo. */
enum { GFX_K_T0, GFX_K_T0A, GFX_K_T1, GFX_K_T1A, GFX_K_ONE, GFX_K_TERMS };

/* Modo de mezcla con el framebuffer. */
enum gfx_blend {
	GFX_BLEND_OFF = 0,  /* opaco */
	GFX_BLEND_ALPHA,    /* src * a + dst * (1 - a) */
	GFX_BLEND_ADD,      /* src * a + dst */
	GFX_BLEND_KEEP,     /* no escribe color (solo profundidad) */
};

#define GFX_DEPTH_TEST   0x01u
#define GFX_DEPTH_WRITE  0x02u
#define GFX_DEPTH_DECAL  0x04u  /* ZMODE_DEC: desplazamiento de polígono */

enum { GFX_CULL_NONE = 0, GFX_CULL_FRONT = 1, GFX_CULL_BACK = 2 };
enum { GFX_ATEST_NONE = 0, GFX_ATEST_GEQUAL = 1 };

struct gfx_draw {
	struct gfx_cmd h;        /* a = primer vértice, b = número de vértices */
	uint32_t zimg;           /* z image en uso (dirección física) */
	uint16_t scissor[4];     /* x0, y0, x1, y1 en píxeles ×4 (10.2) */
	uint8_t cycle;           /* G_CYC_*: 0 1CYCLE, 1 2CYCLE, 2 COPY, 3 FILL */
	uint8_t cull;            /* GFX_CULL_* (en coordenadas de pantalla de la N64) */
	uint8_t depth;           /* GFX_DEPTH_* */
	uint8_t blend;           /* gfx_blend */
	uint8_t atest;           /* GFX_ATEST_* */
	uint8_t aref;            /* referencia de la prueba de alfa (0–255) */
	uint8_t fog;             /* 1: mezclar el color con fog_color según C.a */
	uint8_t ntex;            /* texturas usadas: 0, 1 o 2 */
	uint32_t fog_color;      /* 0xRRGGBBAA */
	uint32_t prim_kind;      /* informativo: 0 triángulos, 1 rectángulos, 2 relleno */
	uint32_t prim_tile;      /* informativo: tile de los rectángulos */
	struct gfx_tex tex[2];
	float k[GFX_K_TERMS][3][4];
};

#define GFX_ALIGN16(x) (((x) + 15u) & ~15u)

/* Volcado de la salida de una tarea, para depurar (src/rdp lo escribe en
 * la PS3; salsa64-run --raster-dump lo dibuja en el host). Todo en el
 * orden de bytes de quien lo escribió (big-endian en la PS3): cabecera,
 * comandos, vértices y arena de texturas, uno detrás de otro. */
#define GFX_DUMP_MAGIC 0x53344744u  /* "S4GD" */
struct gfx_dump_header {
	uint32_t magic;
	uint32_t cmd_bytes, vtx_bytes, tex_bytes;
};

/* ---- Ejecución -----------------------------------------------------------------
 * gfx_run interpreta la display list de un trabajo. Las funciones gfx_io_*
 * (gfx_io.h) las da cada plataforma. El estado persistente (RDP, TMEM) vive
 * en una gfx_state que pone quien llama. */
struct gfx_state;
void gfx_init(struct gfx_state *g);
void gfx_run(struct gfx_state *g, const struct gfx_job *job, struct gfx_result *res);

#endif
