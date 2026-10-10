PROGRAM   := salsa64-spu-compute
TITLE     := Salsa64 prueba SPU
APPID     := SLSA00002
SRCS      := main.c
COMMON    := src/platform/ps3/log.c
LIBS      := -lnet -lsysmodule -lsysutil -lrt -llv2 -lm
SPU_PROGS := mix
mix_SRCS  := spu/mix.c

include ../../../mk/ps3.mk
