PROGRAM := salsa64-cpu-tests
TITLE   := Salsa64 pruebas del CPU
APPID   := SLSA00005
SRCS    := main.c

# El núcleo completo y las pruebas compartidas con el host (sin su main).
R       := ../../..
COMMON  := src/platform/ps3/log.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c)) \
           $(filter-out tests/unit/main_host.c,$(patsubst $(R)/%,%,$(wildcard $(R)/tests/unit/*.c)))
CFLAGS  := -ffp-contract=off -fno-math-errno -I$(CURDIR)/$(R)/tests/unit
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm

include $(R)/mk/ps3.mk
