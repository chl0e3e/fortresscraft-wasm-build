# =============================================================================
# Dockerfile — reproducible SERVER build of the -pthread native FNA stack.
#
#   docker build -t fc-wasm-native -f wasm-build/Dockerfile wasm-build
#   docker create --name fc fc-wasm-native && docker cp fc:/build/out ./wasm-build/out
#
# Produces /build/out/{SDL3,FNA3D,mojoshader,FAudio}.a  (ABI-matched to FNA 26.06).
# Use this when you have a Linux box but not GitHub Actions. The laptop only needs
# Docker; all compilation happens in the container.
# =============================================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ninja-build cmake git python3 python-is-python3 curl xz-utils \
      build-essential ca-certificates lld llvm clang \
 && rm -rf /var/lib/apt/lists/*

# emsdk 3.1.56 — same toolchain version FNA-WASM-Build pins.
ARG EMSDK_VERSION=3.1.56
RUN git clone https://github.com/emscripten-core/emsdk /emsdk \
 && /emsdk/emsdk install ${EMSDK_VERSION} \
 && /emsdk/emsdk activate ${EMSDK_VERSION}
ENV PATH="/emsdk:/emsdk/upstream/emscripten:/emsdk/node/*/bin:${PATH}"
# Make emcc resolvable for non-login shells.
SHELL ["/bin/bash", "-c"]

WORKDIR /build
COPY . /build
# emsdk env is sourced inside the script step via the activated PATH above.
RUN source /emsdk/emsdk_env.sh && bash build-native.sh

# Artifacts live at /build/out
