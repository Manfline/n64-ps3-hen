# Salsa64 — se ejecuta dentro de la imagen salsa64-toolchain (tools/build.sh).
#   make            programas PS3 de las fases 0, 1 y 2 y el frontend
#   make frontend   el emulador para PS3 (build/salsa64/)
#   make rsxkit     lib/rsxkit sola (build/rsxkit/librsxkit.a)
#   make spukit     lib/spukit sola (build/spukit/libspukit.a)
#   make test-host  pruebas unitarias del núcleo con el gcc nativo
#   make host       ejecutor de ROMs para el host (build/host/salsa64-run)
PHASE0_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/phase0/*/Makefile))
PHASE1_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/phase1/*/Makefile))
PHASE2_DIRS := $(patsubst %/Makefile,%,$(wildcard tests/phase2/*/Makefile))

.PHONY: all phase0 phase1 phase2 frontend rsxkit spukit test-host host clean \
	$(PHASE0_DIRS) $(PHASE1_DIRS) $(PHASE2_DIRS)

all: phase0 phase1 phase2 frontend

frontend:
	$(MAKE) -C src/frontend

rsxkit:
	$(MAKE) -C lib/rsxkit OUT=$(CURDIR)/build/rsxkit

spukit:
	$(MAKE) -C lib/spukit OUT=$(CURDIR)/build/spukit

phase0: $(PHASE0_DIRS)

phase1: $(PHASE1_DIRS)

phase2: $(PHASE2_DIRS)

$(PHASE0_DIRS) $(PHASE1_DIRS) $(PHASE2_DIRS):
	$(MAKE) -C $@

test-host:
	$(MAKE) -C tests/unit run

host:
	$(MAKE) -C src/host

clean:
	rm -rf build
