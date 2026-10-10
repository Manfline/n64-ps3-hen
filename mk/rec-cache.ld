/*
 * Caché del recompilador: cola NOBITS al final del segmento de texto de lv2
 * (hdr_text de lv2.ld), como la que validó tests/phase0/jit-ppe/nobits.ld.
 *
 * Es el único lugar escribible y ejecutable con HEN (docs/jit-ppe.md). Al ir
 * detrás de todo lo que ocupa archivo, el segmento queda con memsz > filesz:
 * el cargador la rellena con ceros sin que ocupe espacio en el EBOOT. Se
 * agrega a lv2.ld con INSERT: pasar `-T rec-cache.ld -T lv2.ld`, en ese
 * orden (con -T, gcc ya no agrega lv2.ld solo). La sección la define
 * src/platform/ps3/rec_cache.c.
 */
SECTIONS
{
	.salsa64_rec_cache (NOLOAD) : ALIGN(64K) { KEEP (*(.salsa64_rec_cache)) } : hdr_text
}
INSERT AFTER .sys_proc_prx_param;
