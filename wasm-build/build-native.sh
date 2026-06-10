#!/usr/bin/env bash
# =============================================================================
# build-native.sh — build the -pthread native FNA stack for FortressCraft.Wasm
#
# Produces ABI-matched static archives for OUR FNA 26.06 managed assembly:
#   out/FNA3D.a  out/mojoshader.a  out/FAudio.a  out/SDL3.a
#
# Runs on a Linux SERVER (GitHub Actions / Docker / any Ubuntu box) — NOT on the
# dev laptop. Modeled on r58Playz/FNA-WASM-Build (.github/workflows/WASM.FNA.yml),
# pinned to the versions that match FortressCraft's FNA 26.06 submodules so the
# native ABI lines up exactly with the managed P/Invoke surface.
#
# OffscreenCanvas note: OffscreenCanvas support is a LINK-TIME concern handled by
# the .NET wasm head (-sOFFSCREENCANVAS_SUPPORT + the frozen patched emsdk), NOT
# baked into these .a files. SDL3 >= 3.2 already contains the emscripten offscreen
# video path; we just need it compiled -pthread, which this script does.
#
# Prereqs (the Dockerfile / workflow install these):
#   emsdk 3.1.56 activated (emcc on PATH), ninja, cmake, git, build-essential
# =============================================================================
set -euo pipefail

# ---- pinned versions (match FortressCraft FNA 26.06) ------------------------
SDL_VERSION="${SDL_VERSION:-release-3.4.4}"     # libsdl-org/SDL  (SDL3)
FNA3D_VERSION="${FNA3D_VERSION:-26.06}"          # FNA-XNA/FNA3D   (our submodule)
FAUDIO_VERSION="${FAUDIO_VERSION:-26.06}"        # FNA-XNA/FAudio  (our submodule)

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${WORK:-$ROOT/work}"
OUT="${OUT:-$ROOT/out}"
PATCHES="$ROOT/patches"
mkdir -p "$WORK" "$OUT"

echo "############################################################"
echo "# emcc: $(command -v emcc)"; emcc -v 2>&1 | head -1
echo "# SDL=$SDL_VERSION FNA3D=$FNA3D_VERSION FAudio=$FAUDIO_VERSION"
echo "# work=$WORK out=$OUT"
echo "############################################################"

# ---- 1. SDL3 (-pthread) -----------------------------------------------------
# Static libSDL3.a with pthreads + sem. FNA3D/FAudio link against its headers.
if [ ! -f "$WORK/SDL/emscripten-build/libSDL3.a" ]; then
  echo "==== SDL3 $SDL_VERSION ===="
  rm -rf "$WORK/SDL"
  git clone --depth=1 -b "$SDL_VERSION" https://github.com/libsdl-org/SDL "$WORK/SDL"
  mkdir -p "$WORK/SDL/emscripten-build"
  ( cd "$WORK/SDL/emscripten-build"
    CFLAGS="-pthread" emcmake cmake -S .. \
      -DSDL_WERROR=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=prefix \
      -DSDL_PTHREADS=ON -DSDL_PTHREADS_SEM=ON -GNinja
    ninja )
fi
SDL_INC="$WORK/SDL/include"
SDL_LIB="$WORK/SDL/emscripten-build/libSDL3.a"

# ---- 2. FNA3D (+ mojoshader, -pthread, SDL3) --------------------------------
echo "==== FNA3D $FNA3D_VERSION (+mojoshader) ===="
rm -rf "$WORK/FNA3D"
git clone --recursive --depth=1 -b "$FNA3D_VERSION" https://github.com/FNA-XNA/FNA3D "$WORK/FNA3D"
# wasm patch (regenerated for FNA3D 26.06): force -pthread on FNA3D + mojoshader compile
# flags. The SDL_GPU-driver exclusion is handled natively by -DBUILD_WASM=ON (see cmake below).
git -C "$WORK/FNA3D" apply "$PATCHES/FNA3D.patch"
mkdir -p "$WORK/FNA3D/build"
( cd "$WORK/FNA3D/build"
  # BUILD_WASM=ON makes FNA3D 26.06 auto-skip the SDL_GPU driver (no web backend);
  # the patch only adds -pthread to the FNA3D + mojoshader compile flags.
  emcmake cmake .. \
    -DBUILD_SDL3=ON \
    -DBUILD_WASM=ON \
    -DSDL3_INCLUDE_DIRS="$SDL_INC" \
    -DSDL3_LIBRARIES="$SDL_LIB" \
    -DBUILD_SHARED_LIBS=OFF -GNinja
  ninja )

# ---- 3. FAudio (-pthread, SDL3) ---------------------------------------------
echo "==== FAudio $FAUDIO_VERSION ===="
rm -rf "$WORK/FAudio"
git clone --depth=1 -b "$FAUDIO_VERSION" https://github.com/FNA-XNA/FAudio "$WORK/FAudio"
mkdir -p "$WORK/FAudio/build"
( cd "$WORK/FAudio/build"
  CFLAGS="-pthread" CXXFLAGS="-pthread" emcmake cmake .. \
    -DBUILD_SDL3=ON \
    -DSDL3_INCLUDE_DIRS="$SDL_INC" \
    -DSDL3_LIBRARIES="$SDL_LIB" \
    -DBUILD_SHARED_LIBS=OFF -GNinja
  ninja )

# ---- 4. consolidate -> out/ (filename sans 'lib' MUST match [DllImport] name) -
echo "==== consolidate -> $OUT ===="
cp -v "$SDL_LIB"                                       "$OUT/SDL3.a"
cp -v "$WORK/FNA3D/build/libFNA3D.a"                  "$OUT/FNA3D.a"
find "$WORK/FNA3D/build" -name 'libmojoshader*.a' -exec cp -v {} "$OUT/mojoshader.a" \;
cp -v "$WORK/FAudio/build/libFAudio.a"               "$OUT/FAudio.a"

echo "############################################################"
ls -la "$OUT"
echo "# native FNA stack built. Link these into FortressCraft.Wasm.Native."
echo "############################################################"
