/*
 * steam_api64.c — WebAssembly forwarding shim for the Steamworks flat API.
 * =============================================================================
 * PHASE 1 (identity, DLC, friends, stats/achievements): forwards a real Steam
 * surface from the browser game, through UNMODIFIED Steamworks.NET, to the
 * launcher's real Steam. Phase 2 (SteamNetworking P2P + callback dispatch) follows.
 *
 * Why this file exists
 * --------------------
 * In the browser there is no `steam_api64.dll`, so Steamworks.NET's 918
 * `[DllImport("steam_api64")]` calls have nothing to bind to. Rather than patch
 * Steam out of the game or rewrite Steamworks.NET, we supply the missing native
 * floor: this shim exports the flat `SteamAPI_*` C symbols Steamworks.NET expects
 * and FORWARDS them over a localhost bridge to SeeOneModLoader (the launcher),
 * which runs the real Steamworks.NET + steam_api64 natively. Steamworks.NET and
 * the game ship verbatim; this shim contains only the Valve flat-C ABI (no game
 * code), so it stays in the game-agnostic bundle, exactly like SDL3/FNA3D.
 *
 *     browser:  FortressCraft.dll -> Steamworks.NET (verbatim) -> THIS SHIM --\
 *     launcher: real Steamworks.NET + steam_api64.dll <----- localhost bridge -/
 *
 * Two ABI facts this shim is built around
 * ---------------------------------------
 * 1. Interface pointers are OPAQUE to the managed side. CSteamAPIContext.Init()
 *    only checks each `GetISteam*` accessor returns non-NULL, then passes that
 *    pointer straight back into the data call (e.g. ISteamUser_GetSteamID(ptr)).
 *    It never dereferences it. So each accessor returns a stable non-NULL TOKEN
 *    (distinct per interface, so a later phase can route by it), and the data
 *    call ignores the token and asks the bridge.
 * 2. wasm validates call signatures strictly — a managed/native arity mismatch is
 *    a hard LinkError. Every signature below is taken verbatim from
 *    Steamworks.NET/Steamworks/NativeMethods.cs. NB: GetISteamUtils is pipe-only
 *    (3 args); every other accessor is (client, hUser, hPipe, ver) (4 args).
 *
 * Symbol export (EMSCRIPTEN_KEEPALIVE) is REQUIRED, not cosmetic
 * -------------------------------------------------------------
 * Steamworks.NET is injected/interpreted, not present when this head is built, so
 * its P/Invokes are resolved by mono at RUNTIME via symbol-name lookup against the
 * module's exports (the same path celeste's injected code uses to reach native
 * libs). A symbol that isn't exported is invisible to that lookup and would become
 * an abort-stub. KEEPALIVE forces both retention and export. For the same reason
 * this file must be a DIRECT NativeFileReference (compiled+linked as a standalone
 * object), never an archive member — an unreferenced .a member is dropped before
 * KEEPALIVE can apply.
 *
 * Transport (EM_JS, synchronous, on the worker thread)
 * ----------------------------------------------------
 * The runtime + game run on the deputy worker. Steamworks flat calls are
 * synchronous, so we issue a SYNCHRONOUS XHR to the launcher and block THIS
 * worker — which never freezes the UI thread (that is the whole point of the
 * threaded/OffscreenCanvas build). EM_JS (not EM_ASM) runs on the calling thread,
 * so it is NOT affected by celeste's runEmAsmFunction->runMainThreadEmAsm reroute.
 * =============================================================================
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <emscripten.h>

/* ---- bridge: synchronous localhost round-trip ----------------------------- */
/* POSTs `req` to /__steam (same origin as the bundle server), returns the
 * response text in `out` (NUL-terminated, truncated to outcap), or -1 on error. */
EM_JS(int, sml_bridge_call, (const char *req, char *out, int outcap), {
    try {
        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/__steam', false);          /* false => synchronous   */
        xhr.setRequestHeader('Content-Type', 'text/plain');
        xhr.send(UTF8ToString(req));
        if (xhr.status !== 200) return -1;
        var resp = xhr.responseText || '';
        stringToUTF8(resp, out, outcap);
        return lengthBytesUTF8(resp);
    } catch (e) {
        return -1;
    }
});

/* Forward a request expecting a uint64 decimal reply; `fallback` on bridge error. */
static uint64_t bridge_u64(const char *req, uint64_t fallback) {
    char buf[160];
    int n = sml_bridge_call(req, buf, (int)sizeof(buf));
    if (n <= 0) return fallback;
    return strtoull(buf, NULL, 10);
}

/* Forward a request expecting a "1"/"0" reply. */
static int bridge_bool(const char *req) {
    char buf[16];
    int n = sml_bridge_call(req, buf, (int)sizeof(buf));
    return (n > 0 && buf[0] == '1') ? 1 : 0;
}

/* Forward a request expecting a raw-string reply, copied into caller's buffer.
 * Returns dst. Used for the flat API's `const char*` returns — Steam's real ones
 * point at interface-owned memory valid until the next call; the managed side copies
 * immediately (InteropHelp.PtrToStringUTF8), so a per-call static buffer is sufficient. */
static const char *bridge_str(const char *req, char *dst, int cap) {
    int n = sml_bridge_call(req, dst, cap);
    if (n < 0) dst[0] = '\0';
    return dst;
}

/* Wire protocol: one line per call, fields TAB-separated, "VERB[\targ...]".
 * Replies: bool -> "1"/"0"; int/u64 -> decimal; string -> raw text;
 * GetStat* -> "1\t<value>" on success or "0" on failure.
 * NOTE: args must be tab/newline-free (true for Steam stat/achievement identifiers;
 * RichPresence values are assumed single-line here — upgrade to length-prefix if needed).
 * Phase 2 binary payloads (P2P packets, callback structs) ride as base64 fields. */

/* Phase 2 callback dispatch (defined at the bottom): replays launcher-forwarded callbacks into
 * the game's registered Callback<T>/CallResult<T> via their vtables. Driven by RunCallbacks. */
static void sml_dispatch_callbacks(void);

/* Boot bring-up gate: the vtable call-through (C calling the game's managed Callback<T> fn-ptrs)
 * is the one piece never exercised before the browser boot. Default OFF so RunCallbacks drains the
 * launcher queue but can't crash the runtime — we first confirm the game boots without it, then
 * turn it on once the delegate ABI is verified. */
static int g_dispatch_enabled = 0;

/* ---- lifecycle ------------------------------------------------------------ */
/* FAIL CLOSED: return the launcher's real init result. If the bridge is absent or the player
 * doesn't own the app (DenySteamProvider / RealSteamProvider.Init()==false), this returns 0, the
 * managed SteamAPI.Init() fails, and the game refuses to run. No bridge => no play. */
EMSCRIPTEN_KEEPALIVE int  SteamAPI_Init(void)                 { return bridge_bool("Init"); }
EMSCRIPTEN_KEEPALIVE void SteamAPI_Shutdown(void)            { sml_bridge_call("Shutdown", (char[1]){0}, 1); }
EMSCRIPTEN_KEEPALIVE void SteamAPI_RunCallbacks(void) {
    static int once = 0;
    if (!once) { once = 1; printf("loader: SteamAPI_RunCallbacks reached (vtable dispatch %s)\n", g_dispatch_enabled ? "ON" : "OFF"); }
    sml_dispatch_callbacks();
}
EMSCRIPTEN_KEEPALIVE int  SteamAPI_RestartAppIfNecessary(uint32_t appid) { (void)appid; return 0; /* never relaunch via Steam */ }
EMSCRIPTEN_KEEPALIVE int  SteamAPI_IsSteamRunning(void)      { return 1; }
EMSCRIPTEN_KEEPALIVE int  SteamAPI_GetHSteamUser(void)       { return 1; /* non-zero handle */ }
EMSCRIPTEN_KEEPALIVE int  SteamAPI_GetHSteamPipe(void)       { return 1; /* non-zero or Init() bails */ }

/* ---- interface registry (opaque tokens) ----------------------------------- */
EMSCRIPTEN_KEEPALIVE void *SteamInternal_CreateInterface(const char *ver)                  { (void)ver; return (void *)(intptr_t)0x510100; }
EMSCRIPTEN_KEEPALIVE void *SteamInternal_ContextInit(void *p)                              { return p ? p : (void *)(intptr_t)0x510101; }
EMSCRIPTEN_KEEPALIVE void *SteamInternal_FindOrCreateUserInterface(int hUser, const char *ver)       { (void)hUser; (void)ver; return (void *)(intptr_t)0x510102; }
EMSCRIPTEN_KEEPALIVE void *SteamInternal_FindOrCreateGameServerInterface(int hUser, const char *ver) { (void)hUser; (void)ver; return (void *)(intptr_t)0x510103; }

/* The 4-arg ISteamClient accessors: (client, hUser, hPipe, ver) -> token. */
#define ACCESSOR4(Name, Token)                                                   \
    EMSCRIPTEN_KEEPALIVE void *SteamAPI_ISteamClient_GetISteam##Name(            \
        void *client, int hUser, int hPipe, const char *ver) {                   \
        (void)client; (void)hUser; (void)hPipe; (void)ver;                       \
        return (void *)(intptr_t)(Token);                                        \
    }

ACCESSOR4(User,               0x510001)
ACCESSOR4(GameServer,         0x510002)
ACCESSOR4(Friends,            0x510003)
ACCESSOR4(Matchmaking,        0x510004)
ACCESSOR4(MatchmakingServers, 0x510005)
ACCESSOR4(GenericInterface,   0x510006)
ACCESSOR4(UserStats,          0x510007)
ACCESSOR4(GameServerStats,    0x510008)
ACCESSOR4(Apps,               0x510009)
ACCESSOR4(Networking,         0x51000A)
ACCESSOR4(RemoteStorage,      0x51000B)
ACCESSOR4(Screenshots,        0x51000C)
ACCESSOR4(GameSearch,         0x51000D)
ACCESSOR4(HTTP,               0x51000E)
ACCESSOR4(Controller,         0x51000F)
ACCESSOR4(UGC,                0x510010)
ACCESSOR4(AppList,            0x510011)
ACCESSOR4(Music,              0x510012)
ACCESSOR4(MusicRemote,        0x510013)
ACCESSOR4(HTMLSurface,        0x510014)
ACCESSOR4(Inventory,          0x510015)
ACCESSOR4(Video,              0x510016)
ACCESSOR4(ParentalSettings,   0x510017)
ACCESSOR4(Input,              0x510018)
ACCESSOR4(Parties,            0x510019)
ACCESSOR4(RemotePlay,         0x51001A)

/* GetISteamUtils is pipe-only (3 args) — verbatim from NativeMethods.cs. */
EMSCRIPTEN_KEEPALIVE void *SteamAPI_ISteamClient_GetISteamUtils(void *client, int hPipe, const char *ver) {
    (void)client; (void)hPipe; (void)ver;
    return (void *)(intptr_t)0x51001B;
}

/* ---- data calls (forwarded) ----------------------------------------------- */
/* The slice's proof point: returns the launcher's REAL CSteamID. Signature from
 * NativeMethods.cs: `ulong ISteamUser_GetSteamID(IntPtr)`. */
EMSCRIPTEN_KEEPALIVE uint64_t SteamAPI_ISteamUser_GetSteamID(void *instancePtr) {
    (void)instancePtr;
    return bridge_u64("GetSteamID", 0);
}

/* --- ISteamApps: ownership / DLC (real subscription checks) --- */
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamApps_BIsDlcInstalled(void *p, uint32_t appID) {
    (void)p; char req[64]; snprintf(req, sizeof req, "IsDlcInstalled\t%u", appID);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamApps_BIsSubscribedApp(void *p, uint32_t appID) {
    (void)p; char req[64]; snprintf(req, sizeof req, "IsSubscribedApp\t%u", appID);
    return bridge_bool(req);
}

/* --- ISteamFriends: identity + rich presence --- */
EMSCRIPTEN_KEEPALIVE const char *SteamAPI_ISteamFriends_GetPersonaName(void *p) {
    (void)p; static char buf[256]; return bridge_str("GetPersonaName", buf, (int)sizeof buf);
}
EMSCRIPTEN_KEEPALIVE const char *SteamAPI_ISteamFriends_GetFriendPersonaName(void *p, uint64_t steamID) {
    (void)p; static char buf[256]; char req[64];
    snprintf(req, sizeof req, "GetFriendPersonaName\t%llu", (unsigned long long)steamID);
    return bridge_str(req, buf, (int)sizeof buf);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamFriends_GetFriendCount(void *p, int iFriendFlags) {
    (void)p; char req[48]; snprintf(req, sizeof req, "GetFriendCount\t%d", iFriendFlags);
    return (int)bridge_u64(req, 0);
}
EMSCRIPTEN_KEEPALIVE uint64_t SteamAPI_ISteamFriends_GetFriendByIndex(void *p, int iFriend, int iFriendFlags) {
    (void)p; char req[64]; snprintf(req, sizeof req, "GetFriendByIndex\t%d\t%d", iFriend, iFriendFlags);
    return bridge_u64(req, 0);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamFriends_SetRichPresence(void *p, const char *key, const char *value) {
    (void)p; char req[512];
    snprintf(req, sizeof req, "SetRichPresence\t%s\t%s", key ? key : "", value ? value : "");
    return bridge_bool(req);
}

/* --- ISteamUserStats: stats + achievements (real unlocks) --- */
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_RequestCurrentStats(void *p) {
    (void)p; return bridge_bool("RequestCurrentStats");
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_GetStatInt32(void *p, const char *name, int *pData) {
    (void)p; char req[128], buf[64];
    snprintf(req, sizeof req, "GetStatInt\t%s", name ? name : "");
    int n = sml_bridge_call(req, buf, (int)sizeof buf);
    if (n <= 0 || buf[0] != '1') return 0;
    if (pData) { const char *t = strchr(buf, '\t'); *pData = t ? (int)strtol(t + 1, NULL, 10) : 0; }
    return 1;
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_GetStatFloat(void *p, const char *name, float *pData) {
    (void)p; char req[128], buf[64];
    snprintf(req, sizeof req, "GetStatFloat\t%s", name ? name : "");
    int n = sml_bridge_call(req, buf, (int)sizeof buf);
    if (n <= 0 || buf[0] != '1') return 0;
    if (pData) { const char *t = strchr(buf, '\t'); *pData = t ? strtof(t + 1, NULL) : 0.0f; }
    return 1;
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_SetStatInt32(void *p, const char *name, int nData) {
    (void)p; char req[128]; snprintf(req, sizeof req, "SetStatInt\t%s\t%d", name ? name : "", nData);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_SetStatFloat(void *p, const char *name, float fData) {
    (void)p; char req[128]; snprintf(req, sizeof req, "SetStatFloat\t%s\t%.9g", name ? name : "", (double)fData);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_SetAchievement(void *p, const char *name) {
    (void)p; char req[128]; snprintf(req, sizeof req, "SetAchievement\t%s", name ? name : "");
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_StoreStats(void *p) {
    (void)p; return bridge_bool("StoreStats");
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_ResetAllStats(void *p, int bAchievementsToo) {
    (void)p; char req[48]; snprintf(req, sizeof req, "ResetAllStats\t%d", bAchievementsToo ? 1 : 0);
    return bridge_bool(req);
}

/* ===========================================================================================
 * PHASE 2 — SteamNetworking P2P (data plane) + async callback/CallResult forwarding (control plane)
 * ===========================================================================================
 * FortressCraft multiplayer is 100% Steam P2P (GameClientSteamTransport/GameServerSteamTransport).
 * Packets and callback structs are binary, so they ride as base64 fields in the line protocol.
 *
 * Callback model: this vendored Steamworks.NET uses the LEGACY vtable dispatch — Callback<T>.Register
 * hands us (the native steam_api64) a pinned CCallbackBase whose vtable holds managed function
 * pointers, and RunCallbacks is expected to call back through them. So we keep a registry and, on
 * RunCallbacks, pull the launcher's captured callbacks and invoke each matching vtable.
 *
 * VALIDATION BOUNDARY: the vtable call-through (reading the marshaled function pointer out of the
 * game's CCallbackBase and calling it from C) cannot be exercised without the wasm runtime. The
 * struct/vtable offsets below are taken verbatim from CCallbackBase.cs / CCallbackBaseVTable.cs.
 * =========================================================================================== */

/* ---- base64 (binary payloads over the text protocol) ---- */
static const char B64E[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_encode(const unsigned char *in, int len, char *out, int outcap) {
    int n = 0;
    for (int i = 0; i < len; i += 3) {
        int b0 = in[i], b1 = (i + 1 < len) ? in[i + 1] : 0, b2 = (i + 2 < len) ? in[i + 2] : 0;
        if (n + 4 < outcap) {
            out[n + 0] = B64E[b0 >> 2];
            out[n + 1] = B64E[((b0 & 3) << 4) | (b1 >> 4)];
            out[n + 2] = (i + 1 < len) ? B64E[((b1 & 15) << 2) | (b2 >> 6)] : '=';
            out[n + 3] = (i + 2 < len) ? B64E[b2 & 63] : '=';
        }
        n += 4;
    }
    if (n < outcap) out[n] = 0;
    return n;
}
static int b64_decode(const char *s, unsigned char *out, int outcap) {
    int v = 0, bits = 0, n = 0;
    for (; *s; s++) {
        int d = b64_val(*s);
        if (d < 0) continue;             /* skip '=' and any stray chars */
        v = (v << 6) | d; bits += 6;
        if (bits >= 8) { bits -= 8; if (n < outcap) out[n] = (unsigned char)((v >> bits) & 0xFF); n++; }
    }
    return n;
}

/* Shared scratch (single-threaded worker; each bridge call is synchronous and fully consumed
 * before the next). Sized for a frame of forwarded packets/callbacks; oversized replies truncate
 * (TODO: length-prefix or chunk if FortressCraft's reliable world-sync packets exceed this). */
static char g_send_buf[262144];
static char g_drain_buf[262144];      /* callbacks */
static char g_p2p_drain_buf[262144];  /* P2P — separate so a callback handler that reads packets
                                       * mid-dispatch can't clobber the callback buffer being iterated */
static int bridge_drain_into(const char *req, char *buf, int cap) {
    int n = sml_bridge_call(req, buf, cap);
    if (n < 0) { buf[0] = 0; return 0; }
    return n;
}
static int bridge_drain(const char *req) { return bridge_drain_into(req, g_drain_buf, (int)sizeof g_drain_buf); }

/* ---- P2P inbound queue: one batched DrainP2P refills it; the game reads one packet at a time ---- */
typedef struct p2p_node {
    uint64_t sender;
    int len;
    unsigned char *data;
    struct p2p_node *next;
} p2p_node;
static p2p_node *g_p2p_head = NULL, *g_p2p_tail = NULL;

static void p2p_push(uint64_t sender, const unsigned char *data, int len) {
    p2p_node *n = (p2p_node *)malloc(sizeof(p2p_node));
    if (!n) return;
    n->sender = sender; n->len = len; n->next = NULL;
    n->data = (unsigned char *)malloc(len > 0 ? len : 1);
    if (n->data && len > 0) memcpy(n->data, data, len);
    if (g_p2p_tail) g_p2p_tail->next = n; else g_p2p_head = n;
    g_p2p_tail = n;
}
/* Pull the launcher's pending packets (one batched DrainP2P, capped launcher-side) into the queue.
 * Each packet decodes into a right-sized buffer, so individual packet size isn't capped here. */
static void p2p_refill(int channel) {
    char req[48]; snprintf(req, sizeof req, "DrainP2P\t%d", channel);
    int n = bridge_drain_into(req, g_p2p_drain_buf, (int)sizeof g_p2p_drain_buf);
    if (n <= 0) return;
    char *line = g_p2p_drain_buf;
    while (*line) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            uint64_t sender = strtoull(line, NULL, 10);
            int cap = ((int)strlen(tab + 1) / 4) * 3 + 3;   /* base64 -> max decoded bytes */
            unsigned char *tmp = (unsigned char *)malloc(cap > 0 ? cap : 1);
            if (tmp) { int dl = b64_decode(tab + 1, tmp, cap); p2p_push(sender, tmp, dl); free(tmp); }
        }
        if (!nl) break;
        line = nl + 1;
    }
}

EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_SendP2PPacket(
        void *p, uint64_t steamIDRemote, const void *pubData, uint32_t cubData, int eP2PSendType, int nChannel) {
    (void)p;
    /* base64 the payload into the back of g_send_buf, prefix with the header. */
    char *b64 = g_send_buf + 256;
    b64_encode((const unsigned char *)pubData, (int)cubData, b64, (int)sizeof g_send_buf - 256);
    char hdr[96];
    int hl = snprintf(hdr, sizeof hdr, "SendP2P\t%llu\t%d\t%d\t",
                      (unsigned long long)steamIDRemote, eP2PSendType, nChannel);
    /* move header to sit directly before the base64 (b64 starts at +256, header < 96 bytes) */
    char *req = b64 - hl;
    memcpy(req, hdr, hl);
    return bridge_bool(req);
}

EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_IsP2PPacketAvailable(
        void *p, uint32_t *pcubMsgSize, int nChannel) {
    (void)p;
    if (!g_p2p_head) p2p_refill(nChannel);
    if (!g_p2p_head) { if (pcubMsgSize) *pcubMsgSize = 0; return 0; }
    if (pcubMsgSize) *pcubMsgSize = (uint32_t)g_p2p_head->len;
    return 1;
}

EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_ReadP2PPacket(
        void *p, void *pubDest, uint32_t cubDest, uint32_t *pcubMsgSize, uint64_t *psteamIDRemote, int nChannel) {
    (void)p; (void)nChannel;
    if (!g_p2p_head) p2p_refill(nChannel);
    if (!g_p2p_head) return 0;
    p2p_node *n = g_p2p_head;
    uint32_t copy = (uint32_t)n->len < cubDest ? (uint32_t)n->len : cubDest;
    if (pubDest && copy) memcpy(pubDest, n->data, copy);
    if (pcubMsgSize) *pcubMsgSize = (uint32_t)n->len;
    if (psteamIDRemote) *psteamIDRemote = n->sender;
    g_p2p_head = n->next; if (!g_p2p_head) g_p2p_tail = NULL;
    free(n->data); free(n);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser(void *p, uint64_t steamIDRemote) {
    (void)p; char req[64]; snprintf(req, sizeof req, "AcceptP2P\t%llu", (unsigned long long)steamIDRemote);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_CloseP2PSessionWithUser(void *p, uint64_t steamIDRemote) {
    (void)p; char req[64]; snprintf(req, sizeof req, "CloseP2P\t%llu", (unsigned long long)steamIDRemote);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_AllowP2PPacketRelay(void *p, int bAllow) {
    (void)p; char req[24]; snprintf(req, sizeof req, "AllowRelay\t%d", bAllow ? 1 : 0);
    return bridge_bool(req);
}

/* P2PSessionState_t (Pack=8): 4 bytes + int + int + uint + ushort. Matches Steamworks.NET layout. */
typedef struct {
    uint8_t  connActive, connecting, err, usingRelay;
    int32_t  bytesQueued;
    int32_t  packetsQueued;
    uint32_t remoteIP;
    uint16_t remotePort;
} sml_p2p_state;

EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamNetworking_GetP2PSessionState(
        void *p, uint64_t steamIDRemote, void *pConnectionState) {
    (void)p;
    char req[64], buf[160];
    snprintf(req, sizeof req, "P2PSessionState\t%llu", (unsigned long long)steamIDRemote);
    int n = sml_bridge_call(req, buf, (int)sizeof buf);
    if (n <= 0 || buf[0] != '1') return 0;
    /* reply: 1\tconn\tconnecting\terr\trelay\tbytes\tpkts\tip\tport */
    int v[8] = {0};
    char *t = strchr(buf, '\t');
    for (int i = 0; i < 8 && t; i++) { v[i] = (int)strtoll(t + 1, NULL, 10); t = strchr(t + 1, '\t'); }
    if (pConnectionState) {
        sml_p2p_state *s = (sml_p2p_state *)pConnectionState;
        s->connActive = (uint8_t)v[0]; s->connecting = (uint8_t)v[1];
        s->err = (uint8_t)v[2]; s->usingRelay = (uint8_t)v[3];
        s->bytesQueued = v[4]; s->packetsQueued = v[5];
        s->remoteIP = (uint32_t)v[6]; s->remotePort = (uint16_t)v[7];
    }
    return 1;
}

/* ---- callback/CallResult registry + vtable dispatch ---- */
/* gs flag from CCallbackBase.m_nCallbackFlags (offset 4) bit 2 (k_ECallbackFlagsGameServer): the game
 * registers game-server callbacks via Callback<T>.CreateGameServer; they dispatch from the GS pump. */
#define SML_MAX_REG 64
static struct { void *p; int icb; int gs; } g_cb[SML_MAX_REG]; static int g_cb_n = 0;
static struct { void *p; uint64_t call; } g_cr[SML_MAX_REG]; static int g_cr_n = 0;

EMSCRIPTEN_KEEPALIVE void SteamAPI_RegisterCallback(void *pCallback, int iCallback) {
    if (g_cb_n < SML_MAX_REG) {
        int gs = (*((unsigned char *)pCallback + 4) & 2) ? 1 : 0;   /* m_nCallbackFlags & GameServer */
        g_cb[g_cb_n].p = pCallback; g_cb[g_cb_n].icb = iCallback; g_cb[g_cb_n].gs = gs; g_cb_n++;
    }
}
EMSCRIPTEN_KEEPALIVE void SteamAPI_UnregisterCallback(void *pCallback) {
    for (int i = 0; i < g_cb_n; i++) if (g_cb[i].p == pCallback) { g_cb[i] = g_cb[--g_cb_n]; i--; }
}
EMSCRIPTEN_KEEPALIVE void SteamAPI_RegisterCallResult(void *pCallback, uint64_t hAPICall) {
    if (g_cr_n < SML_MAX_REG) { g_cr[g_cr_n].p = pCallback; g_cr[g_cr_n].call = hAPICall; g_cr_n++; }
}
EMSCRIPTEN_KEEPALIVE void SteamAPI_UnregisterCallResult(void *pCallback, uint64_t hAPICall) {
    for (int i = 0; i < g_cr_n; i++)
        if (g_cr[i].p == pCallback && g_cr[i].call == hAPICall) { g_cr[i] = g_cr[--g_cr_n]; i--; }
}

/* Read the managed vtable out of a CCallbackBase {m_vfptr@0} and call a slot.
 * CCallbackBaseVTable slot order (Sequential): [0]=RunCallResult, [1]=RunCallback, [2]=GetSize. */
static void call_run_callback(void *pcb, void *param) {
    uint32_t vt = *(uint32_t *)pcb;
    uint32_t fp = *(uint32_t *)(uintptr_t)(vt + 4);        /* slot 1 */
    ((void (*)(void *, void *))(uintptr_t)fp)(pcb, param);
}
static void call_run_callresult(void *pcb, void *param, int failed, uint64_t call) {
    uint32_t vt = *(uint32_t *)pcb;
    uint32_t fp = *(uint32_t *)(uintptr_t)(vt + 0);        /* slot 0 */
    ((void (*)(void *, void *, int, uint64_t))(uintptr_t)fp)(pcb, param, failed, call);
}

/* One forwarded record: "CB\t<icb>\t<b64>"  or  "CR\t<call>\t<icb>\t<failed>\t<b64>".
 * `gs` selects which registrations receive callbacks (client vs game-server pump). */
static void dispatch_record(char *line, int gs) {
    static unsigned char param[8192];
    if (line[0] == 'C' && line[1] == 'B') {
        char *a1 = strchr(line, '\t'); if (!a1) return;
        char *a2 = strchr(a1 + 1, '\t'); if (!a2) return;
        *a2 = 0;
        int icb = (int)strtol(a1 + 1, NULL, 10);
        b64_decode(a2 + 1, param, (int)sizeof param);
        for (int i = 0; i < g_cb_n; i++)
            if (g_cb[i].icb == icb && g_cb[i].gs == gs) call_run_callback(g_cb[i].p, param);
    } else if (line[0] == 'C' && line[1] == 'R') {
        char *a1 = strchr(line, '\t');        if (!a1) return;   /* call */
        char *a2 = strchr(a1 + 1, '\t');      if (!a2) return;   /* icb */
        char *a3 = strchr(a2 + 1, '\t');      if (!a3) return;   /* failed */
        char *a4 = strchr(a3 + 1, '\t');      if (!a4) return;   /* b64 */
        *a4 = 0;
        uint64_t call = strtoull(a1 + 1, NULL, 10);
        int failed = (int)strtol(a3 + 1, NULL, 10);
        b64_decode(a4 + 1, param, (int)sizeof param);
        for (int i = 0; i < g_cr_n; i++)              /* CallResults match by unique id, not gs */
            if (g_cr[i].call == call) {
                call_run_callresult(g_cr[i].p, param, failed, call);
                g_cr[i] = g_cr[--g_cr_n]; i--;        /* fire once */
            }
    }
}

static void sml_dispatch(int gs, const char *verb) {
    int n = bridge_drain(verb);
    if (n <= 0) return;
    if (!g_dispatch_enabled) return;   // drained (queue stays bounded) but not dispatched — see gate above
    char *line = g_drain_buf;
    while (*line) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        dispatch_record(line, gs);
        if (!nl) break;
        line = nl + 1;
    }
}
static void sml_dispatch_callbacks(void) { sml_dispatch(0, "DrainCallbacks"); }   /* client pump */

/* ===========================================================================================
 * PHASE 3 — hosting (game server). The launcher runs a REAL Steam game server (listen-server) on
 * the browser player's behalf; desktop players join it as an ordinary Steam server. Same patterns
 * as Phase 1/2 but on the SteamGameServer / SteamGameServerNetworking interfaces + a second pipe(2)
 * whose callbacks the launcher captures from the game-server pipe and forwards via DrainGSCallbacks.
 * The ISteamClient accessors the GS context needs are the same ones the client context uses, already
 * provided above — so no new accessors here.
 * =========================================================================================== */
static void bridge_void(const char *req) { char b[8]; sml_bridge_call(req, b, (int)sizeof b); }

/* GS lifecycle (entrypoint for init is SteamGameServer_InitSafe; pipe(2) distinguishes it from user pipe(1)). */
EMSCRIPTEN_KEEPALIVE int SteamGameServer_InitSafe(uint32_t ip, uint16_t steamPort, uint16_t gamePort,
                                                  uint16_t queryPort, int eServerMode, const char *version) {
    char req[160];
    snprintf(req, sizeof req, "GSInit\t%u\t%u\t%u\t%u\t%d\t%s",
             ip, (unsigned)steamPort, (unsigned)gamePort, (unsigned)queryPort, eServerMode, version ? version : "");
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE void SteamGameServer_Shutdown(void)      { bridge_void("GSShutdown"); }
EMSCRIPTEN_KEEPALIVE void SteamGameServer_RunCallbacks(void)  { sml_dispatch(1, "DrainGSCallbacks"); }
EMSCRIPTEN_KEEPALIVE int  SteamGameServer_GetHSteamPipe(void) { return 2; }
EMSCRIPTEN_KEEPALIVE int  SteamGameServer_GetHSteamUser(void) { return 1; }

/* GS setters — forward to the launcher's real game server. */
#define GS_SET_STR(Name, Field) \
  EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_##Name(void *p, const char *v) { \
    (void)p; char req[512]; snprintf(req, sizeof req, "GSSet\t" Field "\t%s", v ? v : ""); bridge_void(req); }
GS_SET_STR(SetProduct,         "Product")
GS_SET_STR(SetGameDescription, "GameDescription")
GS_SET_STR(SetModDir,          "ModDir")
GS_SET_STR(SetServerName,      "ServerName")
GS_SET_STR(SetMapName,         "MapName")
GS_SET_STR(SetGameData,        "GameData")
GS_SET_STR(SetGameTags,        "GameTags")

#define GS_SET_FLAG(Name, Field) \
  EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_##Name(void *p, int v) { \
    (void)p; char req[64]; snprintf(req, sizeof req, "GSSetFlag\t" Field "\t%d", v ? 1 : 0); bridge_void(req); }
GS_SET_FLAG(SetDedicatedServer,   "Dedicated")
GS_SET_FLAG(SetPasswordProtected, "PasswordProtected")

#define GS_SET_COUNT(Name, Field) \
  EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_##Name(void *p, int v) { \
    (void)p; char req[64]; snprintf(req, sizeof req, "GSSetCount\t" Field "\t%d", v); bridge_void(req); }
GS_SET_COUNT(SetMaxPlayerCount, "MaxPlayers")
GS_SET_COUNT(SetBotPlayerCount, "BotPlayers")

EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_LogOnAnonymous(void *p) { (void)p; bridge_void("GSLogOnAnonymous"); }
EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_LogOff(void *p)         { (void)p; bridge_void("GSLogOff"); }
EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_EnableHeartbeats(void *p, int v) {
    (void)p; char req[32]; snprintf(req, sizeof req, "GSEnableHeartbeats\t%d", v ? 1 : 0); bridge_void(req);
}
EMSCRIPTEN_KEEPALIVE void SteamAPI_ISteamGameServer_ForceHeartbeat(void *p) { (void)p; bridge_void("GSForceHeartbeat"); }

/* GS P2P — a second inbox, identical shape to the client's, refilled by GSDrainP2P. */
static p2p_node *g_gsp2p_head = NULL, *g_gsp2p_tail = NULL;
static void gsp2p_push(uint64_t sender, const unsigned char *data, int len) {
    p2p_node *n = (p2p_node *)malloc(sizeof(p2p_node)); if (!n) return;
    n->sender = sender; n->len = len; n->next = NULL;
    n->data = (unsigned char *)malloc(len > 0 ? len : 1);
    if (n->data && len > 0) memcpy(n->data, data, len);
    if (g_gsp2p_tail) g_gsp2p_tail->next = n; else g_gsp2p_head = n;
    g_gsp2p_tail = n;
}
static void gsp2p_refill(int channel) {
    char req[48]; snprintf(req, sizeof req, "GSDrainP2P\t%d", channel);
    int n = bridge_drain_into(req, g_p2p_drain_buf, (int)sizeof g_p2p_drain_buf);
    if (n <= 0) return;
    char *line = g_p2p_drain_buf;
    while (*line) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0; uint64_t s = strtoull(line, NULL, 10);
            int cap = ((int)strlen(tab + 1) / 4) * 3 + 3;
            unsigned char *tmp = (unsigned char *)malloc(cap > 0 ? cap : 1);
            if (tmp) { int dl = b64_decode(tab + 1, tmp, cap); gsp2p_push(s, tmp, dl); free(tmp); }
        }
        if (!nl) break; line = nl + 1;
    }
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamGameServerNetworking_SendP2PPacket(
        void *p, uint64_t steamIDRemote, const void *pubData, uint32_t cubData, int eP2PSendType, int nChannel) {
    (void)p; char *b64 = g_send_buf + 256;
    b64_encode((const unsigned char *)pubData, (int)cubData, b64, (int)sizeof g_send_buf - 256);
    char hdr[96];
    int hl = snprintf(hdr, sizeof hdr, "GSSendP2P\t%llu\t%d\t%d\t",
                      (unsigned long long)steamIDRemote, eP2PSendType, nChannel);
    char *req = b64 - hl; memcpy(req, hdr, hl);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamGameServerNetworking_IsP2PPacketAvailable(void *p, uint32_t *pcubMsgSize, int nChannel) {
    (void)p; if (!g_gsp2p_head) gsp2p_refill(nChannel);
    if (!g_gsp2p_head) { if (pcubMsgSize) *pcubMsgSize = 0; return 0; }
    if (pcubMsgSize) *pcubMsgSize = (uint32_t)g_gsp2p_head->len; return 1;
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamGameServerNetworking_ReadP2PPacket(
        void *p, void *pubDest, uint32_t cubDest, uint32_t *pcubMsgSize, uint64_t *psteamIDRemote, int nChannel) {
    (void)p; if (!g_gsp2p_head) gsp2p_refill(nChannel);
    if (!g_gsp2p_head) return 0;
    p2p_node *n = g_gsp2p_head;
    uint32_t copy = (uint32_t)n->len < cubDest ? (uint32_t)n->len : cubDest;
    if (pubDest && copy) memcpy(pubDest, n->data, copy);
    if (pcubMsgSize) *pcubMsgSize = (uint32_t)n->len;
    if (psteamIDRemote) *psteamIDRemote = n->sender;
    g_gsp2p_head = n->next; if (!g_gsp2p_head) g_gsp2p_tail = NULL;
    free(n->data); free(n);
    return 1;
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamGameServerNetworking_AcceptP2PSessionWithUser(void *p, uint64_t steamIDRemote) {
    (void)p; char req[64]; snprintf(req, sizeof req, "GSAcceptP2P\t%llu", (unsigned long long)steamIDRemote);
    return bridge_bool(req);
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamGameServerNetworking_CloseP2PSessionWithUser(void *p, uint64_t steamIDRemote) {
    (void)p; char req[64]; snprintf(req, sizeof req, "GSCloseP2P\t%llu", (unsigned long long)steamIDRemote);
    return bridge_bool(req);
}

/* ===========================================================================================
 * SAFE STUBS — flat functions the game calls that aren't fully forwarded yet. With
 * WasmAllowUndefinedSymbols, ANY flat function the game calls that we don't export becomes an
 * abort-stub that kills the runtime ("missing function: ..."). These return safe defaults so the
 * game BOOTS and runs single-player. Real forwarding for auth tickets, lobbies, and the server
 * browser is a later phase (they currently no-op / report empty). Signatures verbatim from
 * NativeMethods.cs — arity/types matter (a mismatch is a hard wasm LinkError).
 * =========================================================================================== */

/* ISteamUser: server-auth ticket. Return a non-zero handle + empty ticket so the game proceeds
 * (k_HAuthTicketInvalid == 0). Real ticket forwarding (+ GetAuthSessionTicketResponse_t) is later. */
EMSCRIPTEN_KEEPALIVE uint32_t SteamAPI_ISteamUser_GetAuthSessionTicket(
        void *p, void *pTicket, int cbMaxTicket, uint32_t *pcbTicket) {
    (void)p; (void)pTicket; (void)cbMaxTicket;
    if (pcbTicket) *pcbTicket = 0;
    return 1;
}

/* ISteamUserStats: another user's stat — not tracked locally; report "unavailable". */
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_GetUserStatInt32(void *p, uint64_t steamID, const char *name, int *pData) {
    (void)p; (void)steamID; (void)name; if (pData) *pData = 0; return 0;
}
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamUserStats_GetUserStatFloat(void *p, uint64_t steamID, const char *name, float *pData) {
    (void)p; (void)steamID; (void)name; if (pData) *pData = 0.0f; return 0;
}

/* ISteamMatchmaking: lobbies. No async call is issued (return 0 = invalid SteamAPICall_t), so no
 * LobbyCreated_t/LobbyEnter_t fires; setters report success. MP lobbies are a later phase. */
EMSCRIPTEN_KEEPALIVE uint64_t SteamAPI_ISteamMatchmaking_CreateLobby(void *p, int eLobbyType, int cMaxMembers) { (void)p; (void)eLobbyType; (void)cMaxMembers; return 0; }
EMSCRIPTEN_KEEPALIVE uint64_t SteamAPI_ISteamMatchmaking_JoinLobby(void *p, uint64_t steamIDLobby) { (void)p; (void)steamIDLobby; return 0; }
EMSCRIPTEN_KEEPALIVE uint64_t SteamAPI_ISteamMatchmaking_GetLobbyOwner(void *p, uint64_t steamIDLobby) { (void)p; (void)steamIDLobby; return 0; }
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamMatchmaking_SetLobbyData(void *p, uint64_t lobby, const char *key, const char *value) { (void)p; (void)lobby; (void)key; (void)value; return 1; }
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamMatchmaking_SetLobbyJoinable(void *p, uint64_t lobby, int bLobbyJoinable) { (void)p; (void)lobby; (void)bLobbyJoinable; return 1; }
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamMatchmaking_SetLobbyOwner(void *p, uint64_t lobby, uint64_t newOwner) { (void)p; (void)lobby; (void)newOwner; return 1; }
EMSCRIPTEN_KEEPALIVE int SteamAPI_ISteamMatchmaking_SetLobbyType(void *p, uint64_t lobby, int eLobbyType) { (void)p; (void)lobby; (void)eLobbyType; return 1; }

/* ISteamMatchmakingServers: server browser. Empty list (no servers); the server-browser response
 * interface isn't forwarded yet. Handles are opaque (null is fine — the game just gets 0 results). */
EMSCRIPTEN_KEEPALIVE void  SteamAPI_ISteamMatchmakingServers_CancelServerQuery(void *p, int hServerQuery) { (void)p; (void)hServerQuery; }
EMSCRIPTEN_KEEPALIVE int   SteamAPI_ISteamMatchmakingServers_GetServerCount(void *p, void *hRequest) { (void)p; (void)hRequest; return 0; }
EMSCRIPTEN_KEEPALIVE void *SteamAPI_ISteamMatchmakingServers_GetServerDetails(void *p, void *hRequest, int iServer) { (void)p; (void)hRequest; (void)iServer; return 0; }
EMSCRIPTEN_KEEPALIVE int   SteamAPI_ISteamMatchmakingServers_PingServer(void *p, uint32_t unIP, uint16_t usPort, void *pResponse) { (void)p; (void)unIP; (void)usPort; (void)pResponse; return 0; }
EMSCRIPTEN_KEEPALIVE void  SteamAPI_ISteamMatchmakingServers_ReleaseRequest(void *p, void *hRequest) { (void)p; (void)hRequest; }
EMSCRIPTEN_KEEPALIVE void *SteamAPI_ISteamMatchmakingServers_RequestInternetServerList(void *p, uint32_t iApp, void *ppchFilters, uint32_t nFilters, void *pResponse) { (void)p; (void)iApp; (void)ppchFilters; (void)nFilters; (void)pResponse; return 0; }
EMSCRIPTEN_KEEPALIVE void *SteamAPI_ISteamMatchmakingServers_RequestLANServerList(void *p, uint32_t iApp, void *pResponse) { (void)p; (void)iApp; (void)pResponse; return 0; }
