# syntax=docker/dockerfile:1.7
#
# Imagen de desarrollo de Cell64: ps3toolchain + PSL1GHT + cgcomp con NVIDIA Cg Toolkit.
#
# - El toolchain (binutils/GCC para PPU y SPU) y PSL1GHT se compilan para la
#   arquitectura nativa del builder (arm64 en las Macs): compilar GCC bajo
#   emulación tarda horas.
# - El Cg Toolkit solo existe para x86_64, y cgcomp lo carga con dlopen().
#   Se compila cgcomp en una etapa amd64 y se ejecuta con qemu-user dentro de
#   la imagen final (ver docker/cgcomp-wrapper.sh).
#
# Construir con tools/docker-image.sh.

ARG DEBIAN=debian:12-slim
ARG PS3TOOLCHAIN_REF=e13aa539c07abe86cdbcd095dc1340492dbca213
ARG PSL1GHT_REF=f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe
ARG CG_URL=https://developer.download.nvidia.com/cg/Cg_3.1/Cg-3.1_April2012_x86_64.tgz
ARG CG_SHA256=e8ff01e6cc38d1b3fd56a083f5860737dbd2f319a39037528fb1a74a89ae9878

# ---------------------------------------------------------------------------
# Etapa cg (amd64): cgcomp + libCg.so y las bibliotecas x86_64 que necesitan.
# ---------------------------------------------------------------------------
FROM --platform=linux/amd64 ${DEBIAN} AS cg
ARG PSL1GHT_REF
ARG CG_URL
ARG CG_SHA256

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "${CG_URL}" -o /tmp/cg.tgz \
 && echo "${CG_SHA256}  /tmp/cg.tgz" | sha256sum -c - \
 && mkdir /tmp/cg && tar -xzf /tmp/cg.tgz -C /tmp/cg && rm /tmp/cg.tgz

RUN curl -fsSL "https://github.com/ps3dev/PSL1GHT/archive/${PSL1GHT_REF}.tar.gz" | tar -xz -C /tmp \
 && make -C "/tmp/PSL1GHT-${PSL1GHT_REF}/tools/cgcomp"

# Raíz mínima x86_64: cgcomp, libCg.so, el cargador y sus dependencias.
RUN mkdir -p /sysroot/usr/lib64 /sysroot/opt/cell64/bin \
 && cp /tmp/cg/usr/lib64/libCg.so /sysroot/usr/lib64/ \
 && cp "/tmp/PSL1GHT-${PSL1GHT_REF}/tools/cgcomp/cgcomp" /sysroot/opt/cell64/bin/ \
 && for f in $(ldd /sysroot/opt/cell64/bin/cgcomp /sysroot/usr/lib64/libCg.so \
               | awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\/.*ld-linux/ {print $1}' | sort -u); do \
      cp --parents -L "$f" /sysroot; \
    done \
 && find /sysroot -type f | sort

# ---------------------------------------------------------------------------
# Etapa toolchain (nativa): ps3toolchain y PSL1GHT.
# ---------------------------------------------------------------------------
FROM ${DEBIAN} AS toolchain
ARG PS3TOOLCHAIN_REF
ARG PSL1GHT_REF

ENV PS3DEV=/usr/local/ps3dev
ENV PSL1GHT=${PS3DEV}
ENV PATH=${PATH}:${PS3DEV}/bin:${PS3DEV}/ppu/bin:${PS3DEV}/spu/bin
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      autoconf automake bison build-essential bzip2 ca-certificates curl flex git \
      libelf-dev libgmp-dev libncurses-dev libssl-dev libtool-bin patch pkg-config \
      python3 python3-dev python3-setuptools texinfo wget xz-utils zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /build "${PS3DEV}" \
 && curl -fsSL "https://github.com/ps3dev/ps3toolchain/archive/${PS3TOOLCHAIN_REF}.tar.gz" | tar -xz -C /build \
 && mv "/build/ps3toolchain-${PS3TOOLCHAIN_REF}" /build/ps3toolchain

# Un paso por capa: si uno falla, los anteriores quedan en caché.
WORKDIR /build/ps3toolchain
ENV BUILD_PS3TOOLCHAIN_ONLY=1
RUN ./toolchain.sh 1
RUN ./toolchain.sh 2
RUN ./toolchain.sh 4
RUN ./toolchain.sh 5
RUN ./toolchain.sh 6

RUN curl -fsSL "https://github.com/ps3dev/PSL1GHT/archive/${PSL1GHT_REF}.tar.gz" | tar -xz -C /build \
 && cd "/build/PSL1GHT-${PSL1GHT_REF}" \
 && make install-ctrl && make -j"$(nproc)" && make install

# ---------------------------------------------------------------------------
# Imagen final.
# ---------------------------------------------------------------------------
FROM ${DEBIAN}
ARG PS3TOOLCHAIN_REF
ARG PSL1GHT_REF

LABEL org.opencontainers.image.title="cell64-toolchain" \
      org.opencontainers.image.description="ps3toolchain + PSL1GHT + cgcomp para Cell64" \
      cell64.ps3toolchain.ref="${PS3TOOLCHAIN_REF}" \
      cell64.psl1ght.ref="${PSL1GHT_REF}"

ENV PS3DEV=/usr/local/ps3dev
ENV PSL1GHT=${PS3DEV}
ENV PATH=${PATH}:${PS3DEV}/bin:${PS3DEV}/ppu/bin:${PS3DEV}/spu/bin
ENV DEBIAN_FRONTEND=noninteractive

# build-essential: pruebas unitarias del host. qemu-user-static: cgcomp x86_64.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl git libelf1 libgmp10 libssl3 make \
      python3 qemu-user-static xxd zlib1g \
 && rm -rf /var/lib/apt/lists/*

COPY --from=toolchain ${PS3DEV} ${PS3DEV}
COPY --from=cg /sysroot /opt/cg-amd64
COPY docker/cgcomp-wrapper.sh ${PS3DEV}/bin/cgcomp

# Prueba de humo: el wrapper debe compilar un vertex shader trivial.
RUN chmod 755 ${PS3DEV}/bin/cgcomp \
 && printf 'void main(float4 p : POSITION, out float4 o : POSITION) { o = p; }\n' > /tmp/t.vcg \
 && cgcomp -v /tmp/t.vcg /tmp/t.vpo \
 && test -s /tmp/t.vpo \
 && rm /tmp/t.vcg /tmp/t.vpo \
 && ppu-gcc --version | head -1 && spu-gcc --version | head -1

WORKDIR /src
