/*
 * Salsa64 — estados guardados. Ver state.h.
 *
 * Cada sección se describe con una sola función para los dos sentidos
 * (struct st: guardar escribe los campos en big-endian, cargar los lee en
 * el mismo orden). Los punteros y lo que se deriva de otros campos no se
 * guardan: se recalculan al cargar.
 */
#include "core/state.h"

#include <stdio.h>
#include <string.h>

#include "core/endian.h"
#include "core/save.h"
#include "cpu/rec.h"
#include "cpu/vr4300_internal.h"

static const uint8_t magic[8] = { 'S', 'L', 'S', '6', '4', 'S', 'T', 0 };

#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define TAG_THMB TAG('T', 'H', 'M', 'B')
#define TAG_CPU0 TAG('C', 'P', 'U', '0')
#define TAG_SCHD TAG('S', 'C', 'H', 'D')
#define TAG_RCP0 TAG('R', 'C', 'P', '0')
#define TAG_SPME TAG('S', 'P', 'M', 'E')
#define TAG_PIF0 TAG('P', 'I', 'F', '0')
#define TAG_CART TAG('C', 'A', 'R', 'T')
#define TAG_HLEG TAG('H', 'L', 'E', 'G')
#define TAG_HLEA TAG('H', 'L', 'E', 'A')
#define TAG_RDRM TAG('R', 'D', 'R', 'M')

#define SECTION_HDR 8u
#define MAX_SECTIONS 32

/* Cabecera de la ROM. */
#define ROM_CRC1  0x10
#define ROM_CRC2  0x14
#define ROM_TITLE 0x20

/* ---- Serializador ------------------------------------------------------------ */

struct st {
	uint8_t *base;   /* NULL al contar */
	uint32_t pos, cap;
	bool load;
	bool overflow;   /* guardar: no cupo en scratch */
};

static struct st st_reader(const uint8_t *data, uint32_t len)
{
	return (struct st){ (uint8_t *)data, 0, len, true, false };
}

static void st_raw(struct st *s, void *v, uint32_t n)
{
	if (!s->base) {
		s->pos += n;  /* contar */
		return;
	}
	if (s->cap - s->pos < n) {
		if (s->load) {
			/* Sección más corta (estado de antes de agregar este campo). */
			uint32_t have = s->cap - s->pos;
			memcpy(v, s->base + s->pos, have);
			memset((uint8_t *)v + have, 0, n - have);
		} else {
			s->overflow = true;
		}
		s->pos = s->cap;
		return;
	}
	if (s->load)
		memcpy(v, s->base + s->pos, n);
	else
		memcpy(s->base + s->pos, v, n);
	s->pos += n;
}

static void st_u8(struct st *s, uint8_t *v)
{
	st_raw(s, v, 1);
}

static void st_u16(struct st *s, uint16_t *v)
{
	uint8_t b[2];
	if (!s->load)
		salsa64_wr16(b, *v);
	st_raw(s, b, 2);
	if (s->load)
		*v = salsa64_rd16(b);
}

static void st_u32(struct st *s, uint32_t *v)
{
	uint8_t b[4];
	if (!s->load)
		salsa64_wr32(b, *v);
	st_raw(s, b, 4);
	if (s->load)
		*v = salsa64_rd32(b);
}

static void st_u64(struct st *s, uint64_t *v)
{
	uint8_t b[8];
	if (!s->load)
		salsa64_wr64(b, *v);
	st_raw(s, b, 8);
	if (s->load)
		*v = salsa64_rd64(b);
}

static void st_bool(struct st *s, bool *v)
{
	uint8_t b = *v;
	st_u8(s, &b);
	*v = b != 0;
}

static void st_s8(struct st *s, int8_t *v)
{
	st_raw(s, v, 1);
}

static void st_u32s(struct st *s, uint32_t *v, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		st_u32(s, &v[i]);
}

/* ---- Secciones ------------------------------------------------------------------ */

static void sec_cpu(struct st *s, struct salsa64_cpu *c)
{
	for (unsigned i = 0; i < 32; i++)
		st_u64(s, &c->gpr[i]);
	st_u64(s, &c->hi);
	st_u64(s, &c->lo);
	st_u64(s, &c->cycles);
	/* cur_pc y delay_slot son de la instrucción en curso: en el límite del
	 * campo no hay ninguna y la siguiente los reescribe (el intérprete
	 * siempre, el recompilador solo cuando los necesita). Van como los
	 * pondrá ella, así el estado no depende de quién ejecutó antes. */
	uint64_t cur = c->pc;
	uint8_t ds = c->branch;
	st_u64(s, &cur);
	st_u64(s, &c->pc);
	st_u64(s, &c->next_pc);
	st_u8(s, &ds);
	st_u8(s, &c->branch);
	if (s->load) {
		c->cur_pc = cur;
		c->delay_slot = ds;
	}
	st_u8(s, &c->llbit);
	st_u32(s, &c->status);
	st_u32(s, &c->cause);
	st_u32(s, &c->compare);
	st_u32(s, &c->count_offset);
	st_u32(s, &c->index);
	st_u32(s, &c->entry_lo0);
	st_u32(s, &c->entry_lo1);
	st_u32(s, &c->page_mask);
	st_u32(s, &c->wired);
	st_u32(s, &c->config);
	st_u32(s, &c->lladdr);
	st_u32(s, &c->watch_lo);
	st_u32(s, &c->watch_hi);
	st_u32(s, &c->parity_error);
	st_u32(s, &c->cache_error);
	st_u32(s, &c->tag_lo);
	st_u32(s, &c->tag_hi);
	st_u64(s, &c->random_epoch);
	st_u64(s, &c->context);
	st_u64(s, &c->xcontext);
	st_u64(s, &c->bad_vaddr);
	st_u64(s, &c->entry_hi);
	st_u64(s, &c->epc);
	st_u64(s, &c->error_epc);
	st_u64(s, &c->cop0_latch);
	for (unsigned i = 0; i < 32; i++)
		st_u64(s, &c->fpr[i]);
	st_u32(s, &c->fcr31);
	st_u64(s, &c->cop2_latch);
	for (unsigned i = 0; i < SALSA64_TLB_ENTRIES; i++) {
		struct salsa64_tlb_entry *t = &c->tlb[i];
		st_u64(s, &t->entry_hi);
		st_u32(s, &t->entry_lo[0]);
		st_u32(s, &t->entry_lo[1]);
		st_u32(s, &t->page_mask);
		st_u64(s, &t->vpn_mask);
		st_u32(s, &t->odd_bit);
		st_u32(s, &t->global);
	}
	/* Los ciclos saltados en bucles de espera: el RSP los compara entre
	 * sondeos (poll_idle). */
	st_u64(s, &c->idle_cycles);
}

static void sec_sched(struct st *s, struct salsa64_sched *sc)
{
	/* Los números de enum salsa64_event son fijos (sched.h): un evento
	 * nuevo va al final y en un estado viejo queda sin programar. */
	uint32_t n = SALSA64_EV_MAX;
	st_u32(s, &n);
	for (uint32_t i = 0; i < n; i++) {
		uint64_t w = i < SALSA64_EV_MAX ? sc->when[i] : SALSA64_SCHED_NEVER;
		st_u64(s, &w);
		if (s->load && i < SALSA64_EV_MAX)
			sc->when[i] = w;
	}
	if (s->load)
		for (uint32_t i = n; i < SALSA64_EV_MAX; i++)
			sc->when[i] = SALSA64_SCHED_NEVER;
}

static void sec_rcp(struct st *s, struct salsa64_bus *b)
{
	st_u32(s, &b->mi.mode);
	st_u32(s, &b->mi.intr);
	st_u32(s, &b->mi.mask);

	st_u32s(s, b->sp.regs, SALSA64_SP_NREGS);
	st_u32(s, &b->sp.pc);
	st_u32(s, &b->sp.pc_writes);

	/* Tarea del RSP en curso. */
	struct salsa64_rsp *r = &b->rsp;
	uint32_t gbi = r->gbi, abi = r->abi;
	st_u32(s, &r->busy);
	st_u32(s, &gbi);
	st_u32(s, &abi);
	st_bool(s, &r->gfx_nonear);
	st_bool(s, &r->dp_held);
	st_u64(s, &r->poll_idle);
	st_bool(s, &r->collected);
	st_u32(s, &r->collected_flags);
	if (s->load) {
		r->gbi = (enum salsa64_gbi)gbi;
		r->abi = abi < SALSA64_ABI_COUNT ? (enum salsa64_abi)abi : SALSA64_ABI_UNKNOWN;
	}

	st_u32s(s, b->dp, SALSA64_DP_NREGS);

	st_u32s(s, b->vi.regs, SALSA64_VI_NREGS);
	st_u32(s, &b->vi.clock_hz);
	st_u32(s, &b->vi.lines);
	st_u32(s, &b->vi.field);
	st_u32(s, &b->vi.fields);
	st_u64(s, &b->vi.field_start);
	st_u64(s, &b->vi.field_cycles);

	struct salsa64_ai *ai = &b->ai;
	st_u32s(s, ai->regs, SALSA64_AI_NREGS);
	st_u32(s, &ai->count);
	st_u32s(s, ai->addr, 2);
	st_u32s(s, ai->len, 2);
	st_bool(s, &ai->carry);
	st_bool(s, &ai->started);
	st_u32(s, &ai->left);
	st_u64(s, &ai->mark);
	st_u32(s, &ai->dmas);

	st_u32s(s, b->pi, SALSA64_PI_NREGS);
	st_u32s(s, b->ri, SALSA64_RI_NREGS);
	st_u32s(s, b->si, SALSA64_SI_NREGS);
	st_u32s(s, b->rdram_regs, SALSA64_RDRAM_NREGS);
}

static void pad_state(struct st *s, struct salsa64_pad *p)
{
	st_u16(s, &p->buttons);
	st_s8(s, &p->x);
	st_s8(s, &p->y);
	st_u8(s, &p->connected);
}

static void sec_pif(struct st *s, struct salsa64_bus *b)
{
	struct salsa64_pif *p = &b->pif;
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		pad_state(s, &p->pads[i]);
	for (unsigned i = 0; i < SALSA64_PIF_CHANNELS; i++)
		st_s8(s, &p->handshake[i]);
	st_u8(s, &p->parsed);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		st_u16(s, &p->held[i]);
	for (unsigned i = 0; i < SALSA64_PIF_PORTS; i++)
		pad_state(s, &p->sent[i]);
	st_u32s(s, p->reads, SALSA64_PIF_PORTS);
	st_raw(s, b->pif_ram, SALSA64_PIF_RAM_SIZE);
}

/* ---- Guardar ------------------------------------------------------------------- */

static void fail(char *err, size_t n, const char *msg)
{
	if (err && n)
		snprintf(err, n, "%s", msg);
}

/* Abre una sección en `s` (escribe la etiqueta y deja lugar al largo);
 * devuelve dónde empieza su cabecera. */
static uint32_t section_begin(struct st *s, uint32_t tag)
{
	uint32_t hdr = s->pos;
	uint32_t zero = 0;
	st_u32(s, &tag);
	st_u32(s, &zero);
	return hdr;
}

static void section_end(struct st *s, uint32_t hdr)
{
	if (s->base && !s->overflow)
		salsa64_wr32(s->base + hdr + 4, s->pos - hdr - SECTION_HDR);
}

static uint32_t cart_bytes(const struct salsa64_bus *b)
{
	return b->save && b->save->type != SALSA64_SAVE_NONE ? b->save->size : 0;
}

/* Todas las secciones salvo la RDRAM, en `s` (o solo contadas si s->base
 * es NULL). */
static bool save_sections(struct salsa64_n64 *n64, const struct salsa64_state_thumb *thumb, struct st *s,
			  char *err, size_t err_size)
{
	struct salsa64_bus *b = &n64->bus;
	struct salsa64_rsp *r = &b->rsp;
	uint32_t h;

	if (thumb && thumb->pixels && thumb->width && thumb->height) {
		h = section_begin(s, TAG_THMB);
		uint16_t w = thumb->width, ht = thumb->height, fmt = 1, pad = 0;
		st_u16(s, &w);
		st_u16(s, &ht);
		st_u16(s, &fmt);  /* 1: RGBA5551 */
		st_u16(s, &pad);
		for (uint32_t i = 0; i < (uint32_t)w * ht; i++) {
			uint16_t px = thumb->pixels[i];
			st_u16(s, &px);
		}
		section_end(s, h);
	}

	h = section_begin(s, TAG_CPU0);
	sec_cpu(s, &n64->cpu);
	section_end(s, h);

	h = section_begin(s, TAG_SCHD);
	sec_sched(s, &n64->sched);
	section_end(s, h);

	h = section_begin(s, TAG_RCP0);
	sec_rcp(s, b);
	section_end(s, h);

	h = section_begin(s, TAG_SPME);
	st_raw(s, b->sp_mem, SALSA64_SP_MEM_SIZE);
	section_end(s, h);

	h = section_begin(s, TAG_PIF0);
	sec_pif(s, b);
	section_end(s, h);

	if (cart_bytes(b)) {
		h = section_begin(s, TAG_CART);
		uint32_t type = b->save->type, size = b->save->size;
		st_u32(s, &type);
		st_u32(s, &size);
		st_raw(s, b->save->data, size);
		section_end(s, h);
	}

	const struct {
		uint32_t tag, size;
		bool (*save)(void *ctx, uint8_t *buf);
		void *ctx;
	} hle[2] = {
		{ TAG_HLEG, r->gfx ? r->gfx->state_size : 0, r->gfx ? r->gfx->save_state : NULL,
		  r->gfx ? r->gfx->ctx : NULL },
		{ TAG_HLEA, r->audio ? r->audio->state_size : 0, r->audio ? r->audio->save_state : NULL,
		  r->audio ? r->audio->ctx : NULL },
	};
	for (unsigned k = 0; k < 2; k++) {
		if (!hle[k].save || !hle[k].size)
			continue;
		h = section_begin(s, hle[k].tag);
		if (s->base) {
			if (s->overflow || s->cap - s->pos < hle[k].size) {
				s->overflow = true;
				break;
			}
			if (!hle[k].save(hle[k].ctx, s->base + s->pos)) {
				fail(err, err_size,
				     k ? "la HLE de audio no entregó su estado" : "la HLE gráfica no entregó su estado");
				return false;
			}
		}
		s->pos += hle[k].size;
		section_end(s, h);
	}

	/* La RDRAM, al final: solo su cabecera pasa por scratch. */
	h = section_begin(s, TAG_RDRM);
	if (s->base && !s->overflow)
		salsa64_wr32(s->base + h + 4, b->rdram_size);
	if (s->overflow) {
		fail(err, err_size, "el estado no cabe en el búfer de paso");
		return false;
	}
	return true;
}

static bool state_ready(const struct salsa64_bus *b, char *err, size_t err_size)
{
	const struct salsa64_rsp *r = &b->rsp;
	if (!b->rom || b->rom_size < 0x40) {
		fail(err, err_size, "no hay ROM");
		return false;
	}
	if (r->busy == SALSA64_TASK_LLE || (r->lle && r->lle_mode != SALSA64_LLE_OFF)) {
		fail(err, err_size, "sin estados con el RSP a bajo nivel (lle)");
		return false;
	}
	if (r->busy && !r->collected) {
		fail(err, err_size, "el RSP tiene una tarea en vuelo (falta salsa64_rsp_quiesce)");
		return false;
	}
	return true;
}

uint32_t salsa64_state_size(const struct salsa64_n64 *n64, bool thumb)
{
	static const uint16_t px;
	const struct salsa64_state_thumb t = { SALSA64_STATE_THUMB_W, SALSA64_STATE_THUMB_H, &px };
	struct st s = { NULL, SALSA64_STATE_HEADER, 0, false, false };
	save_sections((struct salsa64_n64 *)n64, thumb ? &t : NULL, &s, NULL, 0);
	return s.pos + n64->bus.rdram_size;
}

bool salsa64_state_save(struct salsa64_n64 *n64, const struct salsa64_state_thumb *thumb,
			salsa64_state_write_fn write, void *ctx, uint8_t *scratch, char *err, size_t err_size)
{
	struct salsa64_bus *b = &n64->bus;
	if (!state_ready(b, err, err_size))
		return false;
	struct st s = { scratch, SALSA64_STATE_HEADER, SALSA64_STATE_SCRATCH, false, false };
	if (!save_sections(n64, thumb, &s, err, err_size))
		return false;

	uint32_t small = s.pos;
	uint32_t total = small + b->rdram_size;
	uint8_t *hd = scratch;
	memset(hd, 0, SALSA64_STATE_HEADER);
	memcpy(hd, magic, sizeof magic);
	salsa64_wr32(hd + 8, SALSA64_STATE_VERSION);
	salsa64_wr32(hd + 12, SALSA64_STATE_HEADER);
	salsa64_wr32(hd + 16, total);
	salsa64_wr32(hd + 20, salsa64_rd32(b->rom + ROM_CRC1));
	salsa64_wr32(hd + 24, salsa64_rd32(b->rom + ROM_CRC2));
	salsa64_wr32(hd + 28, b->rom_size);
	memcpy(hd + 32, b->rom + ROM_TITLE, 20);
	salsa64_wr32(hd + 52, b->rdram_size);
	salsa64_wr32(hd + 56, b->vi.fields);
	salsa64_wr32(hd + 60, thumb && thumb->pixels ? SALSA64_STATE_FLAG_THUMB : 0);

	if (!write(ctx, scratch, small) || !write(ctx, b->rdram, b->rdram_size)) {
		fail(err, err_size, "no se pudo escribir el archivo");
		return false;
	}
	return true;
}

/* ---- Cargar --------------------------------------------------------------------- */

bool salsa64_state_peek(const uint8_t header[SALSA64_STATE_HEADER], struct salsa64_state_info *info, char *err,
			size_t err_size)
{
	memset(info, 0, sizeof *info);
	if (memcmp(header, magic, sizeof magic) != 0) {
		fail(err, err_size, "no es un estado de Salsa64");
		return false;
	}
	info->version = salsa64_rd32(header + 8);
	uint32_t hsize = salsa64_rd32(header + 12);
	info->file_size = salsa64_rd32(header + 16);
	info->crc1 = salsa64_rd32(header + 20);
	info->crc2 = salsa64_rd32(header + 24);
	info->rom_size = salsa64_rd32(header + 28);
	memcpy(info->title, header + 32, 20);
	info->title[20] = '\0';
	info->rdram_size = salsa64_rd32(header + 52);
	info->field = salsa64_rd32(header + 56);
	info->flags = salsa64_rd32(header + 60);
	if (info->version > SALSA64_STATE_VERSION) {
		if (err && err_size)
			snprintf(err, err_size, "estado de una versión más nueva de Salsa64 (formato %u; esta lee hasta el %u)",
				 (unsigned)info->version, SALSA64_STATE_VERSION);
		return false;
	}
	if (info->version < SALSA64_STATE_MIN_VERSION) {
		if (err && err_size)
			snprintf(err, err_size, "estado de una versión anterior de Salsa64 que ya no se puede cargar "
				 "(formato %u; esta lee desde el %u)", (unsigned)info->version, SALSA64_STATE_MIN_VERSION);
		return false;
	}
	if (hsize != SALSA64_STATE_HEADER) {
		fail(err, err_size, "cabecera del estado con un tamaño inesperado");
		return false;
	}
	return true;
}

struct section {
	uint32_t tag;
	uint8_t *data;
	uint32_t len;
};

static const struct section *find_section(const struct section *t, unsigned n, uint32_t tag)
{
	for (unsigned i = 0; i < n; i++)
		if (t[i].tag == tag)
			return &t[i];
	return NULL;
}

/* El estado de una HLE: si el backend no lo sabe cargar o el tamaño es de
 * otro formato, la HLE sigue con el suyo (casi siempre da igual: los
 * juegos vuelven a poner el estado del RDP en cada cuadro). */
static void load_hle(const struct section *sec, uint32_t size, bool (*load)(void *, const uint8_t *), void *ctx)
{
	if (sec && load && size && sec->len == size)
		load(ctx, sec->data);
}

bool salsa64_state_load(struct salsa64_n64 *n64, salsa64_state_read_fn read, void *ctx, uint8_t *scratch,
			char *err, size_t err_size, bool *broken)
{
	struct salsa64_bus *b = &n64->bus;
	struct salsa64_rsp *r = &b->rsp;
	struct salsa64_state_info info;
	*broken = false;
	if (!state_ready(b, err, err_size))
		return false;
	if (!read(ctx, scratch, SALSA64_STATE_HEADER)) {
		fail(err, err_size, "archivo demasiado corto");
		return false;
	}
	if (!salsa64_state_peek(scratch, &info, err, err_size))
		return false;
	if (info.crc1 != salsa64_rd32(b->rom + ROM_CRC1) || info.crc2 != salsa64_rd32(b->rom + ROM_CRC2)) {
		if (err && err_size)
			snprintf(err, err_size, "el estado es de otro juego (\"%s\", CRC %08X-%08X)", info.title,
				 (unsigned)info.crc1, (unsigned)info.crc2);
		return false;
	}
	if (info.rdram_size != b->rdram_size) {
		if (err && err_size)
			snprintf(err, err_size, "el estado tiene %u MB de RDRAM y la consola emulada %u MB",
				 (unsigned)(info.rdram_size >> 20), (unsigned)(b->rdram_size >> 20));
		return false;
	}

	/* Las secciones chicas a scratch; la RDRAM, directo a su lugar (y nada
	 * se aplica hasta tenerlo todo). */
	struct section sec[MAX_SECTIONS];
	unsigned nsec = 0;
	uint8_t *p = scratch, *end = scratch + SALSA64_STATE_SCRATCH;
	uint32_t pos = SALSA64_STATE_HEADER;
	bool rdram_done = false;
	while (pos < info.file_size) {
		uint8_t hdr[SECTION_HDR];
		if (info.file_size - pos < SECTION_HDR || !read(ctx, hdr, SECTION_HDR)) {
			fail(err, err_size, "archivo cortado (cabecera de sección)");
			*broken = rdram_done;
			return false;
		}
		pos += SECTION_HDR;
		uint32_t tag = salsa64_rd32(hdr), len = salsa64_rd32(hdr + 4);
		if (len > info.file_size - pos) {
			fail(err, err_size, "archivo cortado (sección incompleta)");
			*broken = rdram_done;
			return false;
		}
		if (tag == TAG_RDRM) {
			if (len != b->rdram_size) {
				fail(err, err_size, "la RDRAM del estado no tiene el tamaño de la cabecera");
				return false;
			}
			if (!find_section(sec, nsec, TAG_CPU0) || !find_section(sec, nsec, TAG_SCHD) ||
			    !find_section(sec, nsec, TAG_RCP0)) {
				fail(err, err_size, "faltan secciones en el estado");
				return false;
			}
			if (!read(ctx, b->rdram, len)) {
				fail(err, err_size, "no se pudo leer la RDRAM del estado: la emulación quedó a medias");
				*broken = true;
				return false;
			}
			rdram_done = true;
		} else if (nsec < MAX_SECTIONS && len <= (uint32_t)(end - p)) {
			if (!read(ctx, p, len)) {
				fail(err, err_size, "archivo cortado");
				*broken = rdram_done;
				return false;
			}
			sec[nsec++] = (struct section){ tag, p, len };
			p += len;
		} else {
			/* No cabe o sobra: se lee y se descarta por tramos. */
			uint8_t trash[256];
			for (uint32_t left = len; left;) {
				uint32_t n = left < sizeof trash ? left : (uint32_t)sizeof trash;
				if (!read(ctx, trash, n)) {
					fail(err, err_size, "archivo cortado");
					*broken = rdram_done;
					return false;
				}
				left -= n;
			}
		}
		pos += len;
	}
	if (!rdram_done) {
		fail(err, err_size, "el estado no trae la RDRAM");
		return false;
	}

	/* Aplicar. Lo que no se guarda (punteros, backends, configuración) se
	 * queda como está. */
	const struct section *cs;
	struct st s;
	struct salsa64_cpu *c = &n64->cpu;

	cs = find_section(sec, nsec, TAG_CPU0);
	s = st_reader(cs->data, cs->len);
	sec_cpu(&s, c);
	salsa64_vr4300_update_status(c);
	c->redirect = 1;
	c->running = 0;
	c->tlb_writes++;  /* el recompilador revisa sus traducciones */

	cs = find_section(sec, nsec, TAG_SCHD);
	s = st_reader(cs->data, cs->len);
	sec_sched(&s, &n64->sched);
	n64->sched.when[SALSA64_EV_STOP] = SALSA64_SCHED_NEVER;
	n64->sched.when[SALSA64_EV_RSP_LLE] = SALSA64_SCHED_NEVER;
	salsa64_sched_cancel(&n64->sched, SALSA64_EV_STOP);  /* recalcula `next` */

	cs = find_section(sec, nsec, TAG_RCP0);
	s = st_reader(cs->data, cs->len);
	sec_rcp(&s, b);
	if (r->busy == SALSA64_TASK_LLE || (r->busy && !r->collected)) {
		/* No debería pasar (se guarda recogida): se termina sin resultado. */
		r->collected = r->busy != SALSA64_TASK_LLE;
		if (r->busy == SALSA64_TASK_LLE)
			r->busy = 0;
	}

	if ((cs = find_section(sec, nsec, TAG_SPME)) != NULL) {
		s = st_reader(cs->data, cs->len);
		st_raw(&s, b->sp_mem, SALSA64_SP_MEM_SIZE);
	}
	if ((cs = find_section(sec, nsec, TAG_PIF0)) != NULL) {
		s = st_reader(cs->data, cs->len);
		sec_pif(&s, b);
	}
	if ((cs = find_section(sec, nsec, TAG_CART)) != NULL && b->save && cs->len >= 8) {
		uint32_t type = salsa64_rd32(cs->data), size = salsa64_rd32(cs->data + 4);
		struct salsa64_save *sv = b->save;
		if (type == (uint32_t)sv->type && size == sv->size && size <= cs->len - 8 &&
		    memcmp(sv->data, cs->data + 8, size) != 0) {
			memcpy(sv->data, cs->data + 8, size);
			sv->writes++;  /* al disco con el ritmo de siempre */
		}
	}
	load_hle(find_section(sec, nsec, TAG_HLEG), r->gfx ? r->gfx->state_size : 0, r->gfx ? r->gfx->load_state : NULL,
		 r->gfx ? r->gfx->ctx : NULL);
	load_hle(find_section(sec, nsec, TAG_HLEA), r->audio ? r->audio->state_size : 0,
		 r->audio ? r->audio->load_state : NULL, r->audio ? r->audio->ctx : NULL);

	/* El código generado puede depender de la RDRAM de antes. */
	if (c->rec)
		salsa64_rec_flush(c->rec);
	return true;
}

void salsa64_state_filename(const uint8_t *rom, unsigned slot, char *buf, size_t size)
{
	char base[48];
	salsa64_save_basename(rom, base, sizeof base);
	snprintf(buf, size, "%s.st%u", base, slot);
}
