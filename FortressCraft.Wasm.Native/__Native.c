/*
 * Empty translation unit, referenced from FortressCraft.Wasm.Native.csproj as a NativeFileReference.
 * Its file name registers "__Native" as a static P/Invoke module
 * (WasmApp.Common.targets: _WasmPInvokeModules uses %(FileName)), so FNA's
 *   [DllImport("__Native")] emscripten_set_main_loop / emscripten_cancel_main_loop
 * (SDL2_FNAPlatform.RunPlatformMainLoop) resolve to the Emscripten runtime symbols that
 * are already linked into dotnet.native.wasm, instead of throwing DllNotFoundException("__Native").
 *
 * Lives in the head (not the FNA submodule) because it is ours, not upstream FNA — a fresh
 * `submodules: recursive` checkout of FNA on the build server does not contain it.
 */
