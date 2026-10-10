PROGRAM   := salsa64-spu-jobs
TITLE     := Salsa64 trabajos SPU
APPID     := SLSA00006
SRCS      := main.c
COMMON    := src/platform/ps3/log.c
LIBS      := -lnet -lsysmodule -lsysutil -lrt -llv2 -lm
SPUKIT    := 1
SPU_PROGS := scale
scale_SRCS := spu/scale.c

include ../../../mk/ps3.mk
