PROGRAM := salsa64-save-tests
TITLE   := Salsa64 prueba de guardado
APPID   := SLSA00020
SRCS    := main.c

# El núcleo y frontend/savefile.c (lo mismo que usa el frontend), con las
# tres ROMs savetest embebidas.
R       := ../../..
COMMON  := src/platform/ps3/log.c src/frontend/savefile.c \
           $(patsubst $(R)/%,%,$(wildcard $(R)/src/core/*.c $(R)/src/cpu/*.c))
DATA    := savetest_eeprom4k savetest_eeprom16k savetest_sram
savetest_eeprom4k_FILE  := tests/roms/homebrew/salsa64-savetest-eeprom4k.z64
savetest_eeprom16k_FILE := tests/roms/homebrew/salsa64-savetest-eeprom16k.z64
savetest_sram_FILE      := tests/roms/homebrew/salsa64-savetest-sram.z64
CFLAGS  := -ffp-contract=off -fno-math-errno
LIBS    := -lnet -lsysutil -lsysmodule -lrt -llv2 -lm

include $(R)/mk/ps3.mk
