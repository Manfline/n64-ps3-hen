/*
 * Cola NOBITS al final del segmento de texto (hdr_text de lv2.ld).
 *
 * Al ir detrás de todo lo que ocupa archivo, el segmento queda con
 * memsz > filesz: el cargador debe rellenar la cola con ceros, como hace con
 * .bss, sin que ocupe espacio en el EBOOT. Se agrega a lv2.ld con INSERT:
 * hay que pasarlo con -T antes de -T lv2.ld (ver Makefile).
 */
SECTIONS
{
	.salsa64_jit_nobits (NOLOAD) : ALIGN(64K) { KEEP (*(.salsa64_jit_nobits)) } : hdr_text
}
INSERT AFTER .sys_proc_prx_param;
