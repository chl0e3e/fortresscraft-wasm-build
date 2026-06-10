# fortresscraft-wasm-build

Server build pipeline for running FNA games (FortressCraft) in the browser via WebAssembly,
for SeeOneModLoader. **Contains zero game code** — builds the game-agnostic native bundle.

- `wasm-build/build-native.sh` — the -pthread native FNA stack (SDL3 3.4.4 + FNA3D 26.06 +
  FAudio 26.06 + mojoshader), ABI-matched to FNA 26.06. Runs on GitHub Actions / Docker.
- See `wasm-build/README.md` and (in the main tree) `browser-plan.md` for the full design.
