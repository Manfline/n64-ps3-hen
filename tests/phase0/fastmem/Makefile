# Viabilidad de fastmem en el PPE con lv2: regiones de direcciones, mapeo de
# páginas, fallos de página atrapados desde otro hilo y parche del código
# generado (docs/fastmem.md). Una cola NOBITS de 64 KB en el segmento de
# texto (nobits.ld, como la caché del recompilador) para el código generado.
PROGRAM := salsa64-fastmem
TITLE   := Salsa64 fastmem
APPID   := SLSA00111
SRCS    := main.c
COMMON  := src/platform/ps3/log.c
LIBS    := -lnet -lsysmodule -lsysutil -lrt -llv2 -lm

# Con -T, gcc ya no agrega lv2.ld: se pasa a mano y después del script con
# INSERT, que debe ir antes del script al que se inserta.
LDFLAGS := -T $(CURDIR)/nobits.ld -T lv2.ld

include ../../../mk/ps3.mk
