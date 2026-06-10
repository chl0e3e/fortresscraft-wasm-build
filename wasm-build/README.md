# wasm-build — server build pipeline for FortressCraft-in-the-browser

Compiles the **build-once, GAME-AGNOSTIC native bundle** on a server (GitHub Actions / Docker /
any Ubuntu box) so the dev laptop never has to run emscripten. The bundle is runtime + FNA +
native libs + a reflective loader — **no game code** — so SeeOneModLoader injects the player's own
browser-patched `FortressCraft.dll` per-player (link-free — no SDK on the player's machine). The
build inputs contain zero copyrighted code. See `../browser-plan.md` for the whole picture.

## What gets built where

| Piece | Built by | Why server-side |
|---|---|---|
| `out/{SDL3,FNA3D,mojoshader,FAudio}.a` | `build-native.sh` | emscripten compile of the FNA native stack, `-pthread`, **pinned to FNA 26.06** (ABI-matched to our managed FNA) |
| toolchain `statics/{dotnet,emsdk}` | `build-bundle.sh` (downloads) | the custom threaded mono runtime + OffscreenCanvas-patched emsdk — FNA-version-INDEPENDENT, reused from celeste's prebuilt release |
| `bundle/` (publishable site) | `build-bundle.sh` | `dotnet publish` the net10 head + the 3 OffscreenCanvas/runtime JS seds |

## Run it

### GitHub Actions (recommended — the "server")
1. Push a repo containing `wasm-build/`, `FortressCraft.Wasm.Native/`, and `FNA/` (open source) —
   **no game code is needed**, so the repo can be public.
2. Move the two `.github-workflow-*.yml` files into `.github/workflows/`.
3. Run the **"Build FortressCraft wasm bundle"** workflow. Download the
   `fortresscraft-wasm-bundle` artifact (the game-agnostic bundle).
4. Per-player: SeeOneModLoader injects a browser-patched `FortressCraft.dll` (from the player's
   install) into `_framework` and refreshes the boot manifest. The loader boots it reflectively.

### Docker (any Linux server, no GitHub)
```bash
docker build -t fc-wasm-native -f wasm-build/Dockerfile wasm-build   # native .a only
docker create --name fc fc-wasm-native && docker cp fc:/build/out ./wasm-build/out
# then run build-bundle.sh on a box with the .NET 10 SDK + wasm-tools workload
```

### Bare Ubuntu
```bash
# emsdk 3.1.56 active, .NET 10 SDK + `dotnet workload install wasm-tools` first
cd wasm-build && bash build-native.sh && bash build-bundle.sh
```

## Serve the bundle
```bash
# COOP/COEP (SharedArrayBuffer) + map /Content -> the FortressCraft install dir
python ../FortressCraft.Wasm/serve-mt.py     # point WWW= at wasm-build/bundle
```

## Version pins (keep in lockstep with our FNA)
- SDL `release-3.4.4`, FNA3D `26.06`, FAudio `26.06`, emsdk `3.1.56`, .NET `10.0.x`.
- `patches/FNA3D.patch` — drops the web-incompatible SDL_GPU driver, forces `-pthread`.
- `patches/emsdk-offscreencanvas.patch` — the `callback.c` async fix (already baked into the
  prebuilt `emsdk.zip`; kept here for reference / a from-scratch emsdk build).
