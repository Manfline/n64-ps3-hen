# syntax=docker/dockerfile:1.7
#
# Compila las ROMs de prueba propias (tests/roms/src/vitest y, con
# --build-arg ROM=gfxtest, tests/roms/src/gfxtest) de forma reproducible: clang/lld de Debian 12 para MIPS (sin libultra ni
# libdragon) y nust64 fijo para empaquetar el ELF. GBI (gfxtest) elige la
# variante y NAME el nombre de la ROM (por omisión, salsa64-<ROM>).
#
# El ROM lleva el IPL3 abierto de libdragon (Unlicense) que trae nust64 0.4.1;
# antes de usarlo se comprueba su MD5 contra el publicado por libdragon para la
# revisión r7, así que el ROM no contiene código de Nintendo.
#
# Uso: tools/build-vitest.sh o tools/build-gfxtest.sh (dejan el ROM en
# tests/roms/homebrew/).
# Contexto de construcción: tests/roms/src.

ARG DEBIAN=debian:12-slim

FROM ${DEBIAN} AS toolchain
ARG TARGETARCH
ARG RUSTUP_VERSION=1.29.1
# nust64 0.4.1 con su Cargo.lock compila con este stable (no con nightly).
ARG RUST_STABLE=1.90.0
ARG NUST64_VERSION=0.4.1
# MD5 de ipl3_prod.z64 de libdragon r7 (boot/README.md de libdragon, "ChangeLog";
# nust64 0.4.1 declara en su CHANGELOG que trae r7).
ARG IPL3_PROD_MD5=7f014ed41cc53bb1a5e44f211f8c3dec

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev \
      clang-14 lld-14 llvm-14 make \
 && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:${PATH}

RUN case "${TARGETARCH}" in \
      arm64) triple=aarch64-unknown-linux-gnu; sum=15f6e4ce9f583b929c996c91562bad6d4454f3281de858b02cdfdef615fac433 ;; \
      amd64) triple=x86_64-unknown-linux-gnu;  sum=dda7234360b7f578ca8b0ddcb80145646fa61a67c1720a5abc7051b35c9fcb71 ;; \
      *) echo "arquitectura no soportada: ${TARGETARCH}" >&2; exit 1 ;; \
    esac \
 && curl -fsSL "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/${triple}/rustup-init" -o /tmp/rustup-init \
 && echo "${sum}  /tmp/rustup-init" | sha256sum -c - \
 && chmod +x /tmp/rustup-init \
 && /tmp/rustup-init -y --no-modify-path --profile minimal --default-toolchain "${RUST_STABLE}" \
 && rm /tmp/rustup-init

# nust64 con su Cargo.lock; se verifica el IPL3 que trae antes de usarlo.
RUN cargo install nust64 --version "${NUST64_VERSION}" --locked \
 && ipl3="$(find "${CARGO_HOME}/registry/src" -path "*/nust64-${NUST64_VERSION}/src/ipl3/ipl3_prod.z64")" \
 && echo "${IPL3_PROD_MD5}  ${ipl3}" | md5sum -c -

FROM toolchain AS build
ARG ROM=vitest
ARG TITLE="SALSA64 VITEST"
ARG GBI=""
ARG NAME=""
COPY ${ROM} /build/${ROM}
WORKDIR /build/${ROM}
RUN name="${NAME:-salsa64-${ROM}}" \
 && make ${GBI:+GBI=${GBI}} \
 && nust64 -n "${TITLE}" --elf build/salsa64-${ROM}.elf \
 && mkdir /out \
 && cp build/salsa64-${ROM}.z64 "/out/${name}.z64" \
 && { clang-14 --version | head -1; ld.lld-14 --version; nust64 --version 2>/dev/null || echo "nust64 ${NUST64_VERSION}"; } >"/out/${name}.buildinfo" \
 && sha256sum "/out/${name}.z64"

FROM scratch AS rom
COPY --from=build /out/ /
