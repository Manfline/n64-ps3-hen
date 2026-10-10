# syntax=docker/dockerfile:1.7
#
# Compila la ROM de pruebas nemu64-test (github.com/thelemmy/nemu64-test, MIT)
# de forma reproducible: commit, toolchain de Rust y nust64 fijos.
#
# El ROM lleva el IPL3 abierto de libdragon (Unlicense) que trae nust64 0.4.1;
# antes de usarlo se comprueba su MD5 contra el publicado por libdragon para la
# revisión r7, así que el ROM no contiene código de Nintendo.
#
# Uso: tools/build-nemu64-test.sh (deja el ROM en tests/roms/homebrew/).

ARG DEBIAN=debian:12-slim

FROM ${DEBIAN} AS build
ARG TARGETARCH
ARG RUSTUP_VERSION=1.29.1
ARG RUST_TOOLCHAIN=nightly-2026-07-16
# nust64 0.4.1 (con su Cargo.lock) no compila con nightly: proc-macro2 1.0.49
# detecta nightly y usa features que ya no existen. Se instala con un stable fijo.
ARG RUST_STABLE=1.90.0
ARG NUST64_VERSION=0.4.1
# MD5 de ipl3_prod.z64 de libdragon r7 (boot/README.md de libdragon, "ChangeLog";
# nust64 0.4.1 declara en su CHANGELOG que trae r7).
ARG IPL3_PROD_MD5=7f014ed41cc53bb1a5e44f211f8c3dec
ARG NEMU64_REF=9a8b9f7d94ee2f6f57d7feed70c98c22cdc30e6c

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev \
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
 && /tmp/rustup-init -y --no-modify-path --profile minimal \
      --default-toolchain "${RUST_TOOLCHAIN}" -c rust-src -c rustfmt \
 && rustup toolchain install "${RUST_STABLE}" --profile minimal \
 && rm /tmp/rustup-init

# nust64 con su Cargo.lock; se verifica el IPL3 que trae antes de usarlo.
RUN cargo "+${RUST_STABLE}" install nust64 --version "${NUST64_VERSION}" --locked \
 && ipl3="$(find "${CARGO_HOME}/registry/src" -path "*/nust64-${NUST64_VERSION}/src/ipl3/ipl3_prod.z64")" \
 && echo "${IPL3_PROD_MD5}  ${ipl3}" | md5sum -c -

WORKDIR /build
RUN curl -fsSL "https://github.com/thelemmy/nemu64-test/archive/${NEMU64_REF}.tar.gz" | tar -xz \
 && mv "nemu64-test-${NEMU64_REF}" nemu64-test

WORKDIR /build/nemu64-test
# Conjunto "base" (el de omisión). Sin ISViewer no se ve nada en el host: la
# ROM lo detecta sola (src/isviewer.rs).
RUN cargo build --release --locked \
 && nust64 --elf target/mips-nintendo64-none/release/n64-systemtest \
 && mkdir /out \
 && cp target/mips-nintendo64-none/release/n64-systemtest.z64 /out/nemu64-test.z64 \
 && cp LICENSE /out/nemu64-test.LICENSE \
 && { cargo tree --locked --prefix none --format '{p} | {l}' --edges normal,build | sort -u \
      >/out/nemu64-test.crates || true; } \
 && sha256sum /out/nemu64-test.z64

# Avisos de licencia de lo que queda enlazado en el ROM: los crates que no son
# proc-macro ni de compilación, compiler-builtins y la nota de core/alloc. El
# IPL3 de libdragon es de dominio público (Unlicense).
RUN set -e; out=/out/nemu64-test.NOTICES; \
    { echo "Avisos de licencia del código enlazado en nemu64-test.z64"; \
      echo "(generado por docker/nemu64-test.Dockerfile)"; \
      echo; echo "nemu64-test: ver nemu64-test.LICENSE (MIT)."; \
      echo "core y alloc de Rust ${RUST_TOOLCHAIN}: The Rust Project Developers, MIT OR Apache-2.0"; \
      echo "(https://github.com/rust-lang/rust, archivos COPYRIGHT, LICENSE-MIT y LICENSE-APACHE)."; \
      echo "IPL3 de libdragon r7 (vía nust64 ${NUST64_VERSION}): Unlicense (dominio público)."; } >"$out"; \
    for c in arbitrary-int arrayref linked_list_allocator lock_api num-traits oorandom scopeguard spinning_top; do \
      d="$(find "${CARGO_HOME}/registry/src" -maxdepth 2 -type d -name "${c}-[0-9]*" | sort | tail -1)"; \
      for f in "$d"/LICENSE*; do \
        printf '\n==== %s (%s)\n\n' "$(basename "$d")" "$(basename "$f")" >>"$out"; cat "$f" >>"$out"; \
      done; \
    done; \
    f="$(rustc --print sysroot)/lib/rustlib/src/rust/library/compiler-builtins/LICENSE.txt"; \
    printf '\n==== compiler-builtins (LICENSE.txt)\n\n' >>"$out"; cat "$f" >>"$out"

FROM scratch AS rom
COPY --from=build /out/ /
