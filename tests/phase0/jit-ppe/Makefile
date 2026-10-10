# PRUEBA=cache (por defecto): cuánto se puede reservar en el segmento de
#   texto para la caché del recompilador: zona de nops de CAVE_MB en .text y
#   cola NOBITS de NOBITS_MB (memsz > filesz) al final del segmento.
# PRUEBA=mecanismos: los 8 mecanismos de memoria de la primera corrida, con
#   una zona de nops de 64 KB.
# Ver docs/jit-ppe.md.
PRUEBA    ?= cache
CAVE_MB   ?= 32
NOBITS_MB ?= 32

PROGRAM := salsa64-jit-ppe
TITLE   := Salsa64 prueba JIT PPE
APPID   := SLSA00003
SRCS    := main.c
COMMON  := src/platform/ps3/log.c
LIBS    := -lnet -lsysmodule -lsysutil -lrt -llv2 -lm

ifeq ($(PRUEBA),cache)
CFLAGS  := -DJIT_PRUEBA_CACHE -DJIT_CAVE_MB=$(CAVE_MB) -DJIT_NOBITS_MB=$(NOBITS_MB)
# Con -T, gcc ya no agrega lv2.ld: se pasa a mano y después del script con
# INSERT, que debe ir antes del script al que se inserta.
LDFLAGS := -T $(CURDIR)/nobits.ld -T lv2.ld
else ifneq ($(PRUEBA),mecanismos)
$(error PRUEBA debe ser cache o mecanismos)
endif

include ../../../mk/ps3.mk

# Recompilar main.c (y por tanto reenlazar) si cambian PRUEBA o los tamaños.
FLAGS_STAMP := $(OBJ)/flags.txt
$(shell mkdir -p $(OBJ); echo '$(CFLAGS) $(LDFLAGS)' | cmp -s - $(FLAGS_STAMP) || \
	echo '$(CFLAGS) $(LDFLAGS)' > $(FLAGS_STAMP))
$(OBJ)/tests/phase0/jit-ppe/main.c.o: $(FLAGS_STAMP)
