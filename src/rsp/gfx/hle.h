/*
 * Salsa64 — HLE gráfica: estado interno y E/S por plataforma. Ver gfx.h.
 *
 * Todo el estado cabe en una gfx_state (~53 KB: la TMEM son 4 KB y el
 * directorio de la caché de texturas 32 KB): en el SPE vive en el local
 * store y persiste entre tareas, como el estado del RDP en la consola. El
 * estado del RSP (matrices, luces, segmentos) se reinicia en cada tarea,
 * como lo hace el microcódigo al cargarse.
 */
#ifndef SALSA64_RSP_GFX_HLE_H
#define SALSA64_RSP_GFX_HLE_H

#include <stdbool.h>
#include <stdint.h>

#include "rsp/gfx/gfx.h"

#define GFX_MAX_VTX     64   /* F3D: 16; F3DEX y F3DEX2: 32 */
#define GFX_MTX_STACK   32   /* F3D: 10, F3DEX2: 18 en la RDRAM */
#define GFX_DL_STACK    32   /* F3D: 10, F3DEX2: 18 */
#define GFX_MAX_LIGHTS  8    /* direccionales + la ambiental */
#define GFX_TEX_ARENA   128  /* texturas en el arena por tarea */
#define GFX_TC_ENTRIES  1024 /* texturas en la caché entre tareas */
#define GFX_TC_SLOTS    2048 /* índice por hash de la caché (potencia de 2) */
#define GFX_MAX_COMMANDS (1u << 20)

/* Códigos de recorte de un vértice. */
#define GFX_CLIP_XN 0x01u
#define GFX_CLIP_XP 0x02u
#define GFX_CLIP_YN 0x04u
#define GFX_CLIP_YP 0x08u
#define GFX_CLIP_ZF 0x10u
#define GFX_CLIP_W  0x20u  /* detrás de la cámara (w < 0) */

/* Vértice del búfer del RSP, ya transformado. */
struct gfx_vtx {
	float x, y, z, w;     /* coordenadas de recorte */
	float sx, sy, sz;     /* pantalla: píxeles y profundidad (0..0x3FF) */
	float s, t;           /* texels, con la escala de G_TEXTURE */
	uint8_t c[4];         /* color final (luz y niebla aplicadas) */
	uint32_t clip;
};

struct gfx_light {
	float col[3];   /* 0..255 */
	float dir[3];   /* dirección normalizada en espacio de la cámara */
};

struct gfx_tile {
	uint8_t fmt, siz;
	uint8_t pal;
	uint8_t cms, cmt;     /* bit 0 espejo, bit 1 limitar (G_TX_*) */
	uint8_t masks, maskt;
	uint8_t shifts, shiftt;
	uint16_t line;        /* bytes por fila en la TMEM */
	uint16_t tmem;        /* dirección en bytes */
	uint16_t uls, ult, lrs, lrt;  /* 10.2 */
};

/* Textura decodificada en el arena de la tarea en curso. */
struct gfx_texentry {
	uint32_t key[2];
	struct gfx_tex tex;
};

/* Caché de texturas entre tareas (texcache.c): directorio de lo que hay en
 * la región job->cache_ea. Las entradas forman una cola en el orden en que
 * ocupan la región (anillo); se reserva en `head_off` y se desaloja lo que
 * sigue, salvo lo que aún puede leer el consumidor (last_task > done_task),
 * que se salta. */
struct gfx_tc_entry {
	uint32_t key[2];
	uint32_t off, size;      /* lugar en la región (múltiplos de 128) */
	uint32_t last_task;      /* última tarea que la usó */
	uint16_t width, height, pitch;
	uint16_t slot;           /* posición en index */
};

struct gfx_texcache {
	uint64_t ea;             /* región con que se armó el directorio */
	uint32_t cap;
	uint32_t head_off;       /* donde va la próxima textura */
	uint32_t tail, count;    /* cola de entradas en e[] (circular) */
	uint32_t task, done_task;
	uint16_t index[GFX_TC_SLOTS];  /* posición en e[] + 1; 0 = libre */
	struct gfx_tc_entry e[GFX_TC_ENTRIES];
};

/* Última textura resuelta por tile en la tarea: vale mientras no haya
 * cargas a la TMEM ni cambios de tiles (tex_gen) ni del modo de paleta. */
struct gfx_tilememo {
	uint32_t gen;
	uint8_t tt, ok, premul;
	struct gfx_tex tex;
};

struct gfx_state {
	/* --- RSP (se reinicia por tarea) --- */
	float mv[GFX_MTX_STACK][16];
	int mv_top;
	float proj[16];
	float mvp[16];
	bool mvp_dirty;
	bool lights_dirty;
	uint32_t segments[16];
	uint32_t geom;
	struct gfx_light lights[GFX_MAX_LIGHTS + 1];
	int num_lights;
	float lookat[2][3];        /* X e Y, normalizados, espacio de la cámara */
	float light_model[GFX_MAX_LIGHTS + 1][3];  /* en espacio del modelo */
	float lookat_model[2][3];
	float vscale[3], vtrans[3];
	float fog_mul, fog_off;
	uint16_t tex_s, tex_t;     /* escala de G_TEXTURE (0.16) */
	uint8_t tex_tile, tex_level, tex_on;
	uint32_t rdphalf1, rdphalf2;
	uint32_t raw[44];          /* comando del RDP armado con G_RDPHALF_* */
	uint8_t raw_n, raw_len;    /* dobles palabras recibidas y esperadas */
	uint32_t dl_stack[GFX_DL_STACK];
	int dl_sp;
	uint32_t dl_pc;            /* siguiente comando de la display list */
	struct gfx_vtx vtx[GFX_MAX_VTX];

	/* --- RDP (persiste entre tareas) --- */
	uint32_t oh, ol;           /* othermode H y L */
	uint32_t cc_hi, cc_lo;     /* combiner */
	uint32_t prim, env, fog, blend, fill;  /* 0xRRGGBBAA, fill: como el RDP */
	uint8_t prim_lod, prim_min;
	uint16_t prim_z, prim_dz;
	uint16_t scissor[4];       /* 10.2 */
	uint32_t cimg_addr;
	uint16_t cimg_width;
	uint8_t cimg_fmt, cimg_siz;
	uint32_t zimg_addr;
	uint32_t timg_addr;
	uint16_t timg_width;
	uint8_t timg_fmt, timg_siz;
	struct gfx_tile tiles[8];
	uint32_t tex_gen;          /* cambia con cada carga a la TMEM o cambio de tile */
	uint8_t tmem[4096] __attribute__((aligned(16)));
	struct gfx_texcache tc;

	/* --- Salida de la tarea en curso --- */
	const struct gfx_job *job;
	struct gfx_result *res;
	bool dirty;                /* cambió algo del estado de dibujo */
	bool target_sent;          /* GFX_OP_TARGET de la color image actual */
	bool have_draw;            /* `draw` está abierto */
	struct gfx_draw draw __attribute__((aligned(16)));
	uint32_t vtx_count;        /* vértices escritos en la tarea */
	bool tc_on;                /* la tarea usa la caché de texturas */
	struct gfx_texentry arena_tex[GFX_TEX_ARENA];
	uint32_t narena_tex;
	struct gfx_tilememo tilememo[8];

	void *io;                  /* estado de la plataforma (gfx_io.h) */
};

/* ---- E/S: la implementa cada plataforma (io_mem.c, src/spu/gfx/io_spu.c) ----
 * Las direcciones son físicas de la RDRAM (ya sin segmento). */

enum gfx_io_slot { GFX_IO_VTX, GFX_IO_MTX, GFX_IO_SMALL, GFX_IO_TEX, GFX_IO_SLOTS };
#define GFX_IO_MAX 4096u  /* bytes por lectura (GFX_IO_TEX); 1 KB los demás */

/* Reloj creciente para medir etapas (0 si la plataforma no lo da). */
uint32_t gfx_io_clock(void);
/* Prepara la E/S para un trabajo. */
void gfx_io_begin(struct gfx_state *g);
/* Termina las escrituras pendientes y deja en res los bytes escritos. */
void gfx_io_end(struct gfx_state *g);
/* Puntero a `size` bytes de la RDRAM desde `addr`, válido hasta la siguiente
 * lectura con el mismo `slot`; NULL si se sale de la RDRAM. */
const uint8_t *gfx_io_read(struct gfx_state *g, enum gfx_io_slot slot, uint32_t addr, uint32_t size);
/* Siguiente comando de la display list (8 bytes). false si se sale. */
bool gfx_io_dl(struct gfx_state *g, uint32_t addr, uint32_t *w0, uint32_t *w1);
/* Lugar para n vértices de salida (≤ 6), o NULL si no caben. */
struct gfx_vertex *gfx_io_vtx(struct gfx_state *g, uint32_t n);
/* Escribe un registro de comando (size múltiplo de 16). false si no cabe. */
bool gfx_io_cmd(struct gfx_state *g, const void *rec, uint32_t size);
/* Área para decodificar una textura de `size` bytes (≤ GFX_TEX_MAX) y su
 * entrega al arena: devuelve el offset o -1 si no cabe. */
#define GFX_TEX_MAX 32768u
uint8_t *gfx_io_tex_begin(struct gfx_state *g, uint32_t size);
int32_t gfx_io_tex_commit(struct gfx_state *g, uint32_t size);
/* Entrega la textura (del área de gfx_io_tex_begin) a la región de la
 * caché, `off` bytes desde su inicio (múltiplo de 128). */
void gfx_io_tex_cache(struct gfx_state *g, uint32_t off, uint32_t size);

/* ---- Módulos ---- */

/* Cada familia de microcódigos decodifica sus comandos en su archivo
 * (f3dex2.c: F3DEX2 y F3DZEX2; f3d.c: F3D y F3DEX 1.x) y llama a lo de
 * abajo con los parámetros ya extraídos. */
void gfx_dl_f3dex2(struct gfx_state *g);
void gfx_dl_f3d(struct gfx_state *g);
/* dpc.c: comandos crudos del RDP [job->dl, job->dl_end) (GFX_JOB_DPC). */
void gfx_dl_dpc(struct gfx_state *g);

/* dl.c: recorrido de la display list y estado que el RSP pasa al RDP. */
/* Siguiente comando (avanza dl_pc). false: la tarea termina (fuera de la
 * RDRAM o demasiados comandos). */
bool gfx_dl_next(struct gfx_state *g, uint32_t *w0, uint32_t *w1);
/* G_DL: llamada (guarda el regreso) o salto. false si la pila se llenó. */
bool gfx_dl_call(struct gfx_state *g, uint32_t seg, bool branch);
/* G_ENDDL: regreso a quien llamó. false si terminó la lista principal. */
bool gfx_dl_end(struct gfx_state *g);
/* G_TEXRECT y G_TEXRECTFLIP: s, t y dsdx, dtdy llegan en los dos comandos
 * siguientes si sus opcodes están en ops_st y ops_d (hasta 4 opcodes, uno
 * por byte). Si no, el rectángulo se descarta y ese comando sigue normal. */
void gfx_dl_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t ops_st, uint32_t ops_d);
/* Una doble palabra de un comando del RDP que llega por G_RDPHALF_* (alta
 * en G_RDPHALF_1, baja en el siguiente); al completar el comando, lo
 * ejecuta. */
void gfx_dl_rdp_word(struct gfx_state *g, uint32_t hi, uint32_t lo);
/* G_BRANCH_Z: salta a la lista de G_RDPHALF_1 si la z de pantalla del
 * vértice es <= zval. */
void gfx_branch_z(struct gfx_state *g, uint32_t v, uint32_t zval);
/* G_TEXTURE: escala s, t (0.16) en `scale`, nivel, tile y encendido. */
void gfx_sp_texture(struct gfx_state *g, uint32_t scale, unsigned level, unsigned tile, unsigned on);
/* Geometry mode nuevo completo, con los bits de F3DEX2 (abajo). */
void gfx_sp_geometry(struct gfx_state *g, uint32_t geom);
/* G_SETOTHERMODE_H/L: `len` bits desde el bit `shift`. */
void gfx_sp_othermode(struct gfx_state *g, bool high, int shift, int len, uint32_t data);

/* geom.c: matrices, vértices, luces. */
void gfx_geom_reset(struct gfx_state *g);
void gfx_load_matrix(float m[16], const uint8_t *src);
void gfx_mtx_mul(float out[16], const float a[16], const float b[16]);
void gfx_load_vertices(struct gfx_state *g, uint32_t addr, uint32_t v0, uint32_t n);
void gfx_set_viewport(struct gfx_state *g, const uint8_t *vp);
void gfx_set_light(struct gfx_state *g, int index, const uint8_t *src);
void gfx_set_lookat(struct gfx_state *g, int axis, const uint8_t *src);
/* G_MTX: carga o multiplica (y apila) la modelview o la proyección. */
#define GFX_MTX_PUSH       0x01u
#define GFX_MTX_LOAD       0x02u
#define GFX_MTX_PROJECTION 0x04u
void gfx_matrix(struct gfx_state *g, uint32_t addr, uint32_t flags);
void gfx_pop_matrix(struct gfx_state *g, uint32_t n);
/* Matriz combinada: entera (gSPForceMatrix) o media palabra de dos
 * elementos (G_MW_MATRIX; `where` en bytes dentro del Mtx). */
void gfx_force_matrix(struct gfx_state *g, uint32_t addr);
void gfx_insert_matrix(struct gfx_state *g, uint32_t where, uint32_t val);
void gfx_num_lights(struct gfx_state *g, int n);
void gfx_light_color(struct gfx_state *g, int light, uint32_t rgba);
/* G_MODIFYVTX / G_MW_POINTS: `where` es G_MWO_POINT_* (geom.c). */
void gfx_modify_vertex(struct gfx_state *g, uint32_t v, uint32_t where, uint32_t val);
/* G_CULLDL: true si los vértices v0..v1 quedan todos fuera de un plano. */
bool gfx_vertices_culled(const struct gfx_state *g, uint32_t v0, uint32_t v1);
/* Pantalla → coordenadas homogéneas de salida. */
void gfx_vtx_out(const struct gfx_state *g, const struct gfx_vtx *v, struct gfx_vertex *o);

/* rdp.c: estado y primitivas del RDP. */
void gfx_rdp_reset(struct gfx_state *g);
void gfx_rdp_command(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_triangle(struct gfx_state *g, uint32_t a, uint32_t b, uint32_t c);
void gfx_texrect(struct gfx_state *g, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, bool flip);
void gfx_flush_draw(struct gfx_state *g);
/* Triángulos crudos del RDP (0xC8–0xCF) que llegan por G_RDPHALF_*: dobles
 * palabras del comando según su opcode y el triángulo con ellas (`w`:
 * pares alta, baja). */
unsigned gfx_rdp_raw_words(uint32_t op);
void gfx_rdp_triangle(struct gfx_state *g, const uint32_t *w);

/* texcache.c: hash de 64 bits por bloques de 16 bytes y caché de texturas
 * entre tareas. */
struct gfx_hash {
	uint32_t acc[8] __attribute__((aligned(16)));
	uint32_t len;
};
void gfx_hash_init(struct gfx_hash *h);
/* Bytes [start, start + len) de un anillo de `size` bytes (potencia de 2,
 * alineado a 16): lo que pase del final sigue desde el principio. */
void gfx_hash_ring(struct gfx_hash *h, const uint8_t *ring, uint32_t size, uint32_t start, uint32_t len);
void gfx_hash_words(struct gfx_hash *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void gfx_hash_final(const struct gfx_hash *h, uint32_t key[2]);

/* Al empezar una tarea: decide si usa la caché (g->tc_on) y la prepara. */
void gfx_tc_begin(struct gfx_state *g);
/* Textura con ese hash (y la marca usada en esta tarea), o NULL. */
const struct gfx_tc_entry *gfx_tc_find(struct gfx_state *g, const uint32_t key[2], bool *old);
/* Lugar para una textura nueva de `size` bytes, o NULL si no hay (todo lo
 * que habría que desalojar lo puede leer aún el consumidor). */
struct gfx_tc_entry *gfx_tc_insert(struct gfx_state *g, const uint32_t key[2], uint32_t size);

/* tmem.c: cargas a la TMEM y decodificación de una tile. */
void gfx_load_block(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_load_tile(struct gfx_state *g, uint32_t w0, uint32_t w1);
void gfx_load_tlut(struct gfx_state *g, uint32_t w0, uint32_t w1);
/* Decodifica la tile a RGBA8888 (o la toma de la caché) y llena `tex`
 * salvo flags y xf; con premul, RGB·A (otra entrada de la caché). false si
 * no hay lugar. */
bool gfx_tile_texture(struct gfx_state *g, int tile, struct gfx_tex *tex, bool premul);
/* Tamaño de la textura de una tile (potencia de 2 si se repite). */
void gfx_tile_size(const struct gfx_tile *t, uint32_t *w, uint32_t *h);

/* combine.c: combiner → combinación lineal. Devuelve true si tuvo que
 * aproximar. */
bool gfx_combine(const struct gfx_state *g, struct gfx_draw *d);

/* Segmentada → física. */
static inline uint32_t gfx_addr(const struct gfx_state *g, uint32_t seg)
{
	return (g->segments[(seg >> 24) & 15] + (seg & 0x00FFFFFF)) & 0x00FFFFFF;
}

/* Bits de geometry mode de F3DEX2: los que guarda gfx_state.geom con
 * cualquier microcódigo (f3d.c traduce los suyos). */
#define G_ZBUFFER            0x00000001u
#define G_SHADE              0x00000004u
#define G_CULL_FRONT         0x00000200u
#define G_CULL_BACK          0x00000400u
#define G_FOG                0x00010000u
#define G_LIGHTING           0x00020000u
#define G_TEXTURE_GEN        0x00040000u
#define G_TEXTURE_GEN_LINEAR 0x00080000u
#define G_LOD                0x00100000u
#define G_SHADING_SMOOTH     0x00200000u
#define G_CLIPPING           0x00800000u

/* othermode H */
#define G_MDSFT_CYCLETYPE  20
#define G_MDSFT_TEXTPERSP  19
#define G_MDSFT_TEXTLOD    16
#define G_MDSFT_TEXTLUT    14
#define G_MDSFT_TEXTFILT   12
#define G_CYC_1CYCLE 0
#define G_CYC_2CYCLE 1
#define G_CYC_COPY   2
#define G_CYC_FILL   3
#define G_TT_NONE    0
#define G_TT_RGBA16  2
#define G_TT_IA16    3
#define G_TF_POINT   0
#define G_TF_AVERAGE 3
#define G_TF_BILERP  2

/* othermode L */
#define G_AC_MASK        0x3u
#define G_AC_THRESHOLD   0x1u
#define G_ZS_PRIM        0x4u
#define AA_EN            0x0008u
#define Z_CMP            0x0010u
#define Z_UPD            0x0020u
#define IM_RD            0x0040u
#define CLR_ON_CVG       0x0080u
#define CVG_DST_MASK     0x0300u
#define ZMODE_MASK       0x0C00u
#define ZMODE_OPA        0x0000u
#define ZMODE_INTER      0x0400u
#define ZMODE_XLU        0x0800u
#define ZMODE_DEC        0x0C00u
#define CVG_X_ALPHA      0x1000u
#define ALPHA_CVG_SEL    0x2000u
#define FORCE_BL         0x4000u

/* Formatos de imagen. */
enum { G_IM_FMT_RGBA = 0, G_IM_FMT_YUV = 1, G_IM_FMT_CI = 2, G_IM_FMT_IA = 3, G_IM_FMT_I = 4 };
enum { G_IM_SIZ_4b = 0, G_IM_SIZ_8b = 1, G_IM_SIZ_16b = 2, G_IM_SIZ_32b = 3 };

#endif
