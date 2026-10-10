# Retro64 — se ejecuta dentro de la imagen retro64-toolchain (tools/build.sh).
PHASE0_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/phase0/*/Makefile))

.PHONY: all phase0 clean $(PHASE0_DIRS)

all: phase0

phase0: $(PHASE0_DIRS)

$(PHASE0_DIRS):
	$(MAKE) -C $@

clean:
	rm -rf build
