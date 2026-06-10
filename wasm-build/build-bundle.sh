#!/usr/bin/env bash
# =============================================================================
# build-bundle.sh — build the publishable FortressCraft.Wasm.Native bundle.
#
# Runs on the SERVER, AFTER build-native.sh has produced wasm-build/out/*.a.
# Steps: provision the FNA-version-independent toolchain (custom mono runtime +
# OffscreenCanvas-patched emsdk, reused from celeste's prebuilt release), publish
# the net10 head, then apply the 3 post-build JS patches that make threads +
# OffscreenCanvas actually work.
#
# Output: wasm-build/bundle/  (self-contained site: index.html, main.js, _framework/)
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$ROOT/.." && pwd)"
HEAD="$REPO/FortressCraft.Wasm.Native"
STATICS="$ROOT/statics"
# celeste's prebuilt toolchain release (FNA-version-INDEPENDENT: mono runtime + emsdk).
STATICS_RELEASE="${STATICS_RELEASE:-5ca6e290-3dbe-49dd-b7f8-647e3af0a709}"
BASE="https://github.com/r58Playz/FNA-WASM-Build/releases/download/$STATICS_RELEASE"

# ---- 0. sanity: native libs present -----------------------------------------
for f in SDL3.a FNA3D.a mojoshader.a FAudio.a; do
  [ -f "$ROOT/out/$f" ] || { echo "MISSING $ROOT/out/$f — run build-native.sh first"; exit 1; }
done

# ---- 1. provision toolchain statics (cached) --------------------------------
mkdir -p "$STATICS"
if [ ! -d "$STATICS/dotnet/runtimes" ]; then
  echo "==== fetch + unzip custom mono runtime pack (dotnet.zip) ===="
  curl -fSL "$BASE/dotnet.zip" -o "$STATICS/dotnet.zip"
  rm -rf "$STATICS/dotnet"; mkdir -p "$STATICS/dotnet"
  unzip -q -o "$STATICS/dotnet.zip" -d "$STATICS/dotnet"
fi
if [ ! -d "$STATICS/emsdk/upstream" ] && [ ! -d "$STATICS/emsdk/emscripten" ]; then
  echo "==== fetch + unzip OffscreenCanvas-patched emsdk (emsdk.zip, ~660 MB) ===="
  curl -fSL "$BASE/emsdk.zip" -o "$STATICS/emsdk.zip"
  rm -rf "$STATICS/emsdk"; mkdir -p "$STATICS/emsdk"
  unzip -q -o "$STATICS/emsdk.zip" -d "$STATICS/emsdk"
fi

# ---- 2. publish the head (untrimmed, plain dll for the patcher) -------------
echo "==== dotnet publish FortressCraft.Wasm.Native ===="
rm -rf "$HEAD/bin" "$HEAD/obj"
dotnet publish "$HEAD/FortressCraft.Wasm.Native.csproj" -c Release \
  -p:PublishTrimmed=false --nodereuse:false

PUB="$(dirname "$(find "$HEAD/bin/Release" -name 'index.html' -path '*wwwroot*' | head -1)")"
FW="$PUB/_framework"
echo "published wwwroot: $PUB"

# ---- 3. the 3 post-build JS patches (from celeste's Makefile) ---------------
echo "==== apply OffscreenCanvas + runtime seds ===="
# (a) transfer the <canvas class="canvas"> to the worker thread that runs FNA.
sed -i 's/var offscreenCanvases \?= \?{};/var offscreenCanvases={};if(globalThis.window\&\&!window.TRANSFERRED_CANVAS){transferredCanvasNames=[".canvas"];window.TRANSFERRED_CANVAS=true;}/' "$FW"/dotnet.native.*.js
# (b) mono runtime ULEB fix (jiterpreter table size).
sed -i 's/this.appendULeb(32768)/this.appendULeb(65535)/' "$FW"/dotnet.runtime.*.js
# (c) route EM_ASM GL calls to the owning thread instead of asserting.
sed -i 's/return runEmAsmFunction(code, sigPtr, argbuf);/return runMainThreadEmAsm(code, sigPtr, argbuf, 1);/' "$FW"/dotnet.native.*.js

# ---- 4. stage the bundle ----------------------------------------------------
OUT="$ROOT/bundle"
rm -rf "$OUT"; mkdir -p "$OUT"
cp -r "$PUB"/. "$OUT/"
echo "############################################################"
echo "# bundle ready: $OUT"
echo "#   serve with COOP/COEP (FortressCraft.Wasm/serve-mt.py), map /Content -> install dir"
echo "############################################################"
ls "$OUT"
