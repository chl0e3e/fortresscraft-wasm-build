/*
 * Emscripten.c — WASMFS mount helpers for FortressCraft.Wasm.Native.
 *
 * WASMFS backends let us serve the 610 MB Content dir without a 610 MB up-front download:
 *   - OPFS  : persistent browser storage (Origin Private File System) for saves.
 *   - fetch : lazily HTTP-fetches files on first read (the Content .xnb assets).
 *
 * Called from managed code (P/Invoke) during boot to mount the filesystems. Modeled on
 * celeste-wasm's loader/Emscripten.c, MINUS its SDL_CreateWindow/GetWindowFlags ABI shims:
 * those reconciled celeste's SDL bindings with a mismatched prebuilt SDL3. Our FNA 26.06
 * SDL3.Legacy.cs P/Invokes target SDL3 3.4.4 directly (same era), so no shim is needed.
 * If the linker later reports an SDL_CreateWindow signature clash, re-add them here.
 */
#include <emscripten/wasmfs.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/emscripten.h>
#include <stdio.h>
#include <unistd.h>

/* Mount an OPFS-backed directory (persistent). Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE int mount_opfs(char *dstdir) {
    backend_t opfs = wasmfs_create_opfs_backend();
    return wasmfs_create_directory(dstdir, 0777, opfs);
}

/* Mount an HTTP-fetch-backed directory: reads lazily fetch <srcdir>/<path> on first access. */
static backend_t fetch_backend = NULL;
EMSCRIPTEN_KEEPALIVE int mount_fetch(char *srcdir, char *dstdir) {
    if (!fetch_backend) fetch_backend = wasmfs_create_fetch_backend(srcdir);
    return wasmfs_create_directory(dstdir, 0777, fetch_backend);
}

/* Pre-declare a single fetchable file inside the fetch backend (optional eager registration). */
EMSCRIPTEN_KEEPALIVE int mount_fetch_file(char *path) {
    if (!fetch_backend) return -1;
    int ret = wasmfs_create_file(path, 0777, fetch_backend);
    if (ret >= 0) return close(ret);
    return ret;
}

/* Diagnostics: dump all managed/native thread stacks (handy for the deputy-thread debugging). */
void mono_threads_request_thread_dump(void);
EMSCRIPTEN_KEEPALIVE void perform_thread_dump(void) {
    mono_threads_request_thread_dump();
}
