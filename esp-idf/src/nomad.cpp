/**
 * nomad — Nomad Network page-client task.
 *
 * Modelled on the lxmf task: storage is the API, zero mR includes (rides
 * rnsd's byte-array API: announce fan-out + rnsdLinkOpen + rnsdLinkRequest),
 * one itsPoll wait point, cmd sentinels drive behaviour. Never parses
 * Micron — bytes in, bytes out.
 *
 *   announce drift : subscribe RNSD_PORT_ANNOUNCES filtered to
 *                    nomadnetwork.node → nomad.nodes.<hex> LRU.
 *   navigate       : nomad.cmd.go = [<sid>|]<hash>[:<path>] → rnsdLinkOpen +
 *                    rnsdLinkRequest(path) → response bytes (REQUEST_RESPONSE
 *                    aux). Statuses → nomad.s<sid>.nav.*, page bytes →
 *                    nomad.s<sid>.page.*. Sessions are parallel browser
 *                    contexts (0-5 web tabs, 6 LCD), each holding one Link.
 *   page cache     : hash:path → bytes (RAM/PSRAM). Re-view = zero air time.
 *                    nomad.cmd.reload bypasses the cache READ; the entry
 *                    stays until fresh bytes replace it, so a failed reload
 *                    keeps the old copy reachable. Writers put a unique
 *                    value in the sentinel (tick/time), not a constant:
 *                    a constant re-write would be swallowed by the storage
 *                    SET-dedup if a change notify was ever dropped.
 *   bookmarks      : s.nomad.bookmarks.<id> = <hash>[:<path>]|<name>|<note>.
 */
#include "nomad.h"
#include "spangap.h"
#include "ports.h"
#include "rnsd.h"     /* rnsdLinkOpen / rnsdLinkRequest / release */
#include "mem.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

static const char* TAG = "nomad";

#define NOMAD_VERSION              1
#define NOMAD_PUBLISH_INTERVAL_MS  1000
#define NOMAD_ASPECT               "nomadnetwork.node"
#define NOMAD_FETCH_TAG            "nomad"          /* rnsdLinkOpen tag prefix: "nomad<sid>" (≤23) */
#define NOMAD_RESP_PORT            130              /* request-response aux port */
#define NOMAD_DEFAULT_PAGE         "/page/index.mu"

/* NOMAD_SESSIONS / NOMAD_LCD_SESSION live in nomad.h (the LCD slice and
 * frontends share them). Each session may hold one open Link, so nomad's
 * share of the device link budget is NOMAD_SESSIONS (the budget: 12 links
 * total — 6 web tabs, 1 LCD, 5 for other consumers like lxmf; rnsd
 * slots = 32, plenty). */

/* Page cache caps (PSRAM). Re-viewing a cached page costs zero air time. */
#define NOMAD_CACHE_MAX_ENTRIES    16
#define NOMAD_CACHE_MAX_BYTES      (512 * 1024)

/* Max page bytes published to the SPA in a single storage patch. The storage
 * DataChannel negotiates a 256 KB max-message-size (webrtc_task.cpp), so a page
 * rides one patch; 128 KB keeps us clear of that ceiling after JSON escaping.
 * Larger pages fall back to the on-device LCD / RAM cache. */
#define NOMAD_MAX_PAGE_PUBLISH     (128 * 1024)

/* RNSD_PORT_ANNOUNCES frame: hops(1) | dest_hash(16) | identity_hash(16) | app_data(N) */
constexpr size_t NOMAD_ANNOUNCE_HDR = 1 + 16 + 16;
constexpr size_t NOMAD_DEST_HASH_LEN = 16;

/* ── helpers (local; mirror lxmf.cpp) ── */

static uint64_t nowUnixMs(void) { return (uint64_t)(esp_timer_get_time() / 1000); }
static int      nowUnixS(void)  { return (int)(nowUnixMs() / 1000); }

static std::string bytesToHex(const uint8_t* data, size_t n)
{
    std::string out;
    out.resize(n * 2);
    for (size_t i = 0; i < n; ++i) std::snprintf(&out[2 * i], 3, "%02x", data[i]);
    return out;
}

static bool hexToDestHash(const std::string& s, uint8_t out[NOMAD_DEST_HASH_LEN])
{
    if (s.size() != NOMAD_DEST_HASH_LEN * 2) return false;
    for (size_t k = 0; k < NOMAD_DEST_HASH_LEN; ++k) {
        unsigned x = 0;
        if (std::sscanf(s.c_str() + 2 * k, "%2x", &x) != 1) return false;
        out[k] = (uint8_t)x;
    }
    return true;
}

/* Strip C0/DEL from network-supplied strings before logging (an embedded
 * ESC in an announce name otherwise hoses the browser log window). */
static std::string sanitizeForLog(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (uint8_t b : s) out += (b < 0x20 || b == 0x7F) ? '.' : (char)b;
    return out;
}

/* ---- minimal msgpack writers for the form-submit map (str keys/values,
 * matching Python umsgpack on the NomadNet node). Local — keeps the
 * "zero mR includes" rule; mirrors lxmf.cpp's mpPack* helpers. ---- */

static void mpMapHeader(std::vector<uint8_t>& o, size_t n)
{
    if (n <= 15) o.push_back((uint8_t)(0x80 | n));
    else if (n <= 0xffff) {
        o.push_back(0xde); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n);
    } else {
        o.push_back(0xdf);
        o.push_back((uint8_t)(n >> 24)); o.push_back((uint8_t)(n >> 16));
        o.push_back((uint8_t)(n >> 8));  o.push_back((uint8_t)n);
    }
}

static void mpStr(std::vector<uint8_t>& o, const std::string& s)
{
    size_t n = s.size();
    if (n <= 31) o.push_back((uint8_t)(0xa0 | n));
    else if (n <= 0xff) { o.push_back(0xd9); o.push_back((uint8_t)n); }
    else if (n <= 0xffff) {
        o.push_back(0xda); o.push_back((uint8_t)(n >> 8)); o.push_back((uint8_t)n);
    } else {
        o.push_back(0xdb);
        o.push_back((uint8_t)(n >> 24)); o.push_back((uint8_t)(n >> 16));
        o.push_back((uint8_t)(n >> 8));  o.push_back((uint8_t)n);
    }
    o.insert(o.end(), s.begin(), s.end());
}

/* ─────────────── page cache (RAM/PSRAM) ─────────────── */

struct PageEntry {
    std::string          key;        /* "<hash>:<path>" */
    std::vector<uint8_t> body;
    int                  fetched_s;
};
static std::vector<PageEntry> s_cache;
static size_t                 s_cache_bytes = 0;

static PageEntry* cacheGet(const std::string& key)
{
    for (auto& e : s_cache) if (e.key == key) return &e;
    return nullptr;
}

static void cacheEvictOldest(void)
{
    if (s_cache.empty()) return;
    size_t oldest = 0;
    for (size_t i = 1; i < s_cache.size(); ++i)
        if (s_cache[i].fetched_s < s_cache[oldest].fetched_s) oldest = i;
    s_cache_bytes -= s_cache[oldest].body.size();
    s_cache.erase(s_cache.begin() + oldest);
}

static void cachePut(const std::string& key, const uint8_t* body, size_t len)
{
    if (PageEntry* e = cacheGet(key)) {           /* refresh in place */
        s_cache_bytes -= e->body.size();
        e->body.assign(body, body + len);
        e->fetched_s = nowUnixS();
        s_cache_bytes += len;
    } else {
        PageEntry ne;
        ne.key = key;
        ne.body.assign(body, body + len);
        ne.fetched_s = nowUnixS();
        s_cache_bytes += len;
        s_cache.push_back(std::move(ne));
    }
    while (s_cache.size() > NOMAD_CACHE_MAX_ENTRIES ||
           (s_cache_bytes > NOMAD_CACHE_MAX_BYTES && s_cache.size() > 1))
        cacheEvictOldest();
}


/* ─────────────── navigation / fetch state (sessions) ─────────────── */

/* Parallel browsing sessions. Each session is an independent NomadNet
 * browser context with its own Link, in-flight request, and published
 * nav/page state (nomad.s<sid>.nav.* / nomad.s<sid>.page.*). Per session
 * the NomadNet model holds: ONE Link to the current node, reused for every
 * request to that node, dropped on node change / failure / idle-STALE
 * close; one request in flight at a time. Sessions 0-5 belong to the web
 * UI's tabs, session 6 to the LCD browser (NOMAD_LCD_SESSION). The page
 * cache is shared across sessions. Link tags are "nomad<sid>"; the
 * session id rides the conn ref (itsRef / disconnect-cb arg). */
struct Session {
    bool        active;     /* a request is in flight */
    bool        terminal;   /* done/failed already published for this fetch */
    bool        submit;     /* form submit (don't cache the response) */
    int         req_id;     /* rnsdLinkRequest correlation id */
    int         handle;     /* open RNSD_PORT_LINK handle (-1 = no link) */
    std::string link_hash;  /* node the open link is connected to ("" = none) */
    std::string hash;       /* 32-hex target of the current request */
    std::string path;
    int         started_s;
};
PSRAM_BSS static Session s_sess[NOMAD_SESSIONS] = {};  /* handles set to -1 in nomadTaskMain */

static bool validSid(int sid) { return sid >= 0 && sid < NOMAD_SESSIONS; }

static void sessTag(int sid, char out[16])
{
    std::snprintf(out, 16, NOMAD_FETCH_TAG "%d", sid);
}

static void sessKey(int sid, const char* tail, char* out, size_t outLen)
{
    std::snprintf(out, outLen, "nomad.s%d.%s", sid, tail);
}

static void navSet(int sid, const char* status)
{
    char k[48];
    sessKey(sid, "nav.status", k, sizeof k);
    storageSet(k, status);
}

/* "<sid>|rest" command-value prefix → session id; bare values (CLI habit,
 * old callers) fall back to session 0. */
static int parseSid(std::string& cmd)
{
    if (cmd.size() >= 2 && cmd[1] == '|' && cmd[0] >= '0' && cmd[0] <= '9') {
        int sid = cmd[0] - '0';
        cmd.erase(0, 2);
        if (validSid(sid)) return sid;
        return 0;
    }
    return 0;
}

/* One-line page preview for the serial log — a short hex head + a sanitized
 * text fragment (CR/LF/controls folded to '.') + ellipsis. Never dumps the
 * whole page (no raw newlines/CRs in the log). */
static void logPage(const std::string& key, const uint8_t* b, size_t len)
{
    size_t hx = len < 12 ? len : 12;
    std::string frag;
    size_t fn = len < 56 ? len : 56;
    for (size_t i = 0; i < fn; ++i) {
        uint8_t c = b[i];
        frag += (c < 0x20 || c == 0x7f) ? '.' : (char)c;
    }
    info("page %s: %zuB hex=%s%s text=\"%s%s\"", key.c_str(), len,
         bytesToHex(b, hx).c_str(), len > hx ? "…" : "",
         frag.c_str(), len > fn ? "…" : "");
}

static void publishPage(int sid, const std::string& hash, const std::string& path,
                        const uint8_t* body, size_t len, bool cache = true)
{
    std::string key = hash + ":" + path;
    if (cache) cachePut(key, body, len);   /* form-submit results aren't cached */
    char k[48];
    sessKey(sid, "page.hash", k, sizeof k); storageSet(k, hash.c_str());
    sessKey(sid, "page.path", k, sizeof k); storageSet(k, path.c_str());
    sessKey(sid, "page.size", k, sizeof k); storageSet(k, (int)len);
    sessKey(sid, "page.fetched_s", k, sizeof k); storageSet(k, nowUnixS());

    /* Publish the body to the (ephemeral) config tree so the SPA renders
     * it via the normal storage DC sync — the browser receives full values
     * (in-device change notifies truncate to STORAGE_NOTIFY_VAL_MAX, 512 B:
     * they're change signals, not value transport; subscribers re-read by
     * key). The full bytes always live in the RAM cache (the LCD renderer
     * reads those).
     * Micron is UTF-8 text, so a cJSON string value is the right carrier. */
    sessKey(sid, "page.body", k, sizeof k);
    char kt[48];
    sessKey(sid, "page.truncated", kt, sizeof kt);
    if ((int)len <= NOMAD_MAX_PAGE_PUBLISH) {
        std::string b(reinterpret_cast<const char*>(body), len);
        storageSet(k, b.c_str());
        storageSet(kt, 0);
    } else {
        storageSet(k, "");
        storageSet(kt, 1);   /* too large for the SPA; see LCD/file */
    }
    logPage(key, body, len);
}

/* Close a session's open link + free our ITS conn. Closing our handle
 * tears the Link down in rnsd (onLinkDisconnect) and frees the slot + tag;
 * itsConnect is synchronous + FIFO-after, so a same-tag reopen right after
 * sees the slot already freed. */
static void dropLink(int sid)
{
    Session& s = s_sess[sid];
    if (s.handle >= 0) { itsDisconnect(s.handle); s.handle = -1; }
    s.link_hash.clear();
}

/* A request concluded. On success keep the link open for same-node reuse;
 * on failure drop it so the next attempt re-establishes cleanly. */
static void fetchDone(int sid, bool ok)
{
    s_sess[sid].terminal = true;
    s_sess[sid].active   = false;
    if (!ok) dropLink(sid);
}

/* The link's packet handle is unused (responses ride the aux port), but
 * rnsd opens it packet-mode — drain defensively. */
static void onFetchLinkRecv(int handle, size_t /*n*/)
{
    uint8_t b[256];
    itsRecv(handle, b, sizeof(b), 0);
}
/* rnsd closed the link (teardown cascade, or idle/STALE close): our conn is
 * gone, so forget it — the next fetch re-establishes. The disconnect cb
 * receives our conn ref, which carries the session id. */
static void onFetchLinkDisc(int ref)
{
    if (!validSid(ref)) return;
    s_sess[ref].handle = -1;
    s_sess[ref].link_hash.clear();
}

/* ─────────────── request-response aux (rnsd → nomad, NOMAD_RESP_PORT) ─────────────── */

static void onNomadAux(TaskHandle_t /*sender*/, const void* data, size_t len)
{
    if (len < sizeof(rnsd_link_resource_done_t)) {
        warn("aux: short frame %zu", len);
        return;
    }
    rnsd_link_resource_done_t d;
    std::memcpy(&d, data, sizeof(d));

    /* Correlate the response to its session by request id. */
    int sid = -1;
    for (int i = 0; i < NOMAD_SESSIONS; i++)
        if (s_sess[i].active && (int)d.opaque_id == s_sess[i].req_id) { sid = i; break; }

    /* Only request/response (page GETs) is handled here. /file Resource
     * downloads (RNSD_LINK_RESOURCE_INBOUND_DONE) aren't handled yet. */
    if (d.opcode == RNSD_LINK_REQUEST_RESPONSE) {
        if (sid < 0) {
            verb("aux: stray response cid=%u (no matching fetch)",
                 (unsigned)d.opaque_id);
            rnsdResourceRelease(d.buf);
            return;
        }
        Session& s = s_sess[sid];
        info("s%d fetch %s:%s done — %uB", sid, s.hash.c_str(),
             s.path.c_str(), (unsigned)d.len);
        publishPage(sid, s.hash, s.path,
                    (const uint8_t*)d.buf, d.len, /*cache=*/!s.submit);
        navSet(sid, "done");
        rnsdResourceRelease(d.buf);   /* we own it on REQUEST_RESPONSE */
        fetchDone(sid, true);         /* keep the link open for same-node reuse */
        return;
    }

    if (d.opcode == RNSD_LINK_REQUEST_FAILED) {
        if (sid < 0) return;
        Session& s = s_sess[sid];
        warn("s%d fetch %s:%s failed", sid, s.hash.c_str(), s.path.c_str());
        char k[48];
        sessKey(sid, "nav.error", k, sizeof k);
        storageSet(k, "request failed");
        navSet(sid, "failed");
        fetchDone(sid, false);       /* drop the link; next attempt re-establishes */
        return;
    }

    if (d.buf) rnsdResourceRelease(d.buf);   /* unexpected opcode w/ buffer */
}

/* rnsd publishes each Link's progress to rnsd.links.nomad<sid>.*. Reflect
 * the .state keys into that session's nav.status while its fetch is active
 * so the frontend sees the Browser.py-style progression without nomad
 * re-deriving it. The terminal states (done/failed) are owned by onNomadAux
 * above — don't clobber them. */
static void onLinkState(const char* key, const char* val)
{
    /* key = "rnsd.links.nomad<sid>.state" (the prefix sub also delivers the
     * tree's other keys — filter to single-digit sid + ".state"). */
    const char* tail = key + sizeof("rnsd.links." NOMAD_FETCH_TAG) - 1;
    if (tail[0] < '0' || tail[0] > '9' || std::strcmp(tail + 1, ".state") != 0) return;
    int sid = tail[0] - '0';
    if (!validSid(sid)) return;
    Session& s = s_sess[sid];
    if (!s.active || s.terminal || !val || !*val) return;
    if      (std::strcmp(val, "awaiting_path") == 0) navSet(sid, "path_requested");
    else if (std::strcmp(val, "establishing")  == 0) navSet(sid, "establishing");
    else if (std::strcmp(val, "active")        == 0) navSet(sid, "requesting");
    /* closing/closed/failed: leave to the aux handler / linkFreeSlot's
     * REQUEST_FAILED, which carries the proper terminal reason. */
}

/* ─────────────── navigate ─────────────── */

/* Fetch a page (packed == nullptr → GET) or submit a form (packed == the
 * msgpack {field_*,var_*} map) on session `sid`. Submits bypass the cache
 * and aren't cached. */
static void startFetch(int sid, const std::string& hash, const std::string& path,
                       bool bypass_cache, const std::vector<uint8_t>* packed = nullptr)
{
    Session& s = s_sess[sid];
    char k[48];
    sessKey(sid, "nav.error", k, sizeof k);

    uint8_t dh[NOMAD_DEST_HASH_LEN];
    if (!hexToDestHash(hash, dh)) {
        warn("s%d go: bad hash %s", sid, hash.c_str());
        storageSet(k, "bad hash");
        navSet(sid, "failed");
        return;
    }

    storageSet(k, "");
    sessKey(sid, "nav.hash", k, sizeof k); storageSet(k, hash.c_str());
    sessKey(sid, "nav.path", k, sizeof k); storageSet(k, path.c_str());

    std::string key = hash + ":" + path;
    /* A bypass fetch (reload) skips the cache READ but leaves the entry in
     * place: fresh bytes replace it on arrival (cachePut refreshes), and if
     * the refetch fails the old copy survives — Back still shows it. */
    if (!bypass_cache && !packed) {
        if (PageEntry* e = cacheGet(key)) {
            info("s%d go %s — cache hit (%zuB, zero air time)", sid, key.c_str(),
                 e->body.size());
            publishPage(sid, hash, path, e->body.data(), e->body.size());
            navSet(sid, "done");
            return;
        }
    }

    /* A previous request still in flight on this session can't be joined by
     * a second (rnsd allows one per link slot) — newest navigation wins:
     * abandon it, and don't reuse its link (the slot's request stays bound
     * until it concludes; a fresh slot sidesteps the in-flight guard). */
    bool wasActive = s.active;
    if (wasActive) { s.active = false; s.terminal = true; }

    char tag[16];
    sessTag(sid, tag);

    /* Reuse the open link if it's already to this node (NomadNet model);
     * otherwise drop a link to a different node and open a fresh one. */
    bool reuse = (!wasActive && s.handle >= 0 && s.link_hash == hash);
    if (!reuse) {
        if (s.handle >= 0) dropLink(sid);   /* link is to a different node */
        /* s.nomad.link_timeout (seconds, 0 = let rnsd derive from interface
         * speed) overrides the establishment timeout for fetch links. */
        uint32_t link_to_ms = (uint32_t)storageGetInt("s.nomad.link_timeout", 0) * 1000;
        int h = rnsdLinkOpen(dh, NOMAD_ASPECT, /*identity_key=*/"", tag,
                             /*path_timeout_ms=*/0, link_to_ms, /*ref=*/sid,
                             onFetchLinkRecv, onFetchLinkDisc);
        if (h < 0) {
            warn("s%d go: rnsdLinkOpen failed (%d)", sid, h);
            sessKey(sid, "nav.error", k, sizeof k);
            storageSet(k, "link open failed");
            navSet(sid, "failed");
            return;
        }
        s.handle    = h;
        s.link_hash = hash;
    }
    /* rnsd holds the request until the Link is ACTIVE, then issues it (or, on
     * reuse, issues it immediately); a link failure fails the request back to
     * us (REQUEST_FAILED). */
    int rid = packed
        ? rnsdLinkRequest(tag, path.c_str(), packed->data(), packed->size(),
                          NOMAD_RESP_PORT, /*data_packed=*/true)
        : rnsdLinkRequest(tag, path.c_str(), nullptr, 0, NOMAD_RESP_PORT);
    if (rid < 0) {
        warn("s%d go: rnsdLinkRequest failed (%d)", sid, rid);
        dropLink(sid);
        sessKey(sid, "nav.error", k, sizeof k);
        storageSet(k, "request failed");
        navSet(sid, "failed");
        return;
    }

    s.active    = true;
    s.terminal  = false;
    s.submit    = (packed != nullptr);
    s.req_id    = rid;
    s.hash      = hash;
    s.path      = path;
    s.started_s = nowUnixS();
    navSet(sid, reuse ? "requesting" : "establishing");
    info("s%d %s %s:%s (req_id=%d%s)", sid, packed ? "submit" : "go",
         hash.c_str(), path.c_str(), rid, reuse ? ", reused link" : "");
}

/* nomad.cmd.go = "[<sid>|]<hash>[:<path>]". Empty path → /page/index.mu. */
static void onCmdGo(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);
    int sid = parseSid(cmd);

    std::string hash = cmd, path = NOMAD_DEFAULT_PAGE;
    size_t colon = cmd.find(':');
    if (colon != std::string::npos) {
        hash = cmd.substr(0, colon);
        path = cmd.substr(colon + 1);
        if (path.empty()) path = NOMAD_DEFAULT_PAGE;
    }
    startFetch(sid, hash, path, /*bypass_cache=*/false);
}

/* nomad.cmd.reload = "[<sid>|]<unique>". */
static void onCmdReload(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);
    int sid = parseSid(cmd);
    char k[48], hash[64] = {}, path[128] = {};
    sessKey(sid, "nav.hash", k, sizeof k);
    storageGetStr(k, hash, sizeof(hash), "");
    sessKey(sid, "nav.path", k, sizeof k);
    storageGetStr(k, path, sizeof(path), NOMAD_DEFAULT_PAGE);
    if (!hash[0]) { warn("s%d reload: nothing navigated yet", sid); return; }
    startFetch(sid, hash, path, /*bypass_cache=*/true);
}

/* Form submit. The frontend stages the field values under
 * `nomad.submit.<field_*|var_*>` (keys are already the NomadNet map keys),
 * then writes `nomad.cmd.submit = "<hash>:<path>"`. We pack those k/v into
 * a msgpack map and issue a request with data_packed=true so µR splices it
 * as the request envelope's 3rd element. */
struct SubmitKV { std::string k, v; };
static std::vector<SubmitKV>* s_submitKVs = nullptr;
static size_t s_submitPfxLen = 0;       /* per-session prefix is dynamic */
static void collectSubmitField(const char* key, const char* val)
{
    if (!s_submitKVs || !key) return;
    const char* tail = key + s_submitPfxLen;
    if (!*tail) return;
    s_submitKVs->push_back({ tail, val ? val : "" });
}

/* nomad.cmd.submit = "[<sid>|]<hash>:<path>"; fields are staged under the
 * session's own tree, nomad.submit.<sid>.<field_*|var_*>, so parallel
 * sessions can't read each other's staged forms. */
static void onCmdSubmit(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);
    int sid = parseSid(cmd);

    std::string hash = cmd, path = NOMAD_DEFAULT_PAGE;
    size_t colon = cmd.find(':');
    if (colon != std::string::npos) {
        hash = cmd.substr(0, colon);
        path = cmd.substr(colon + 1);
        if (path.empty()) path = NOMAD_DEFAULT_PAGE;
    }

    char tree[24], pfx[32];
    std::snprintf(tree, sizeof tree, "nomad.submit.%d", sid);
    std::snprintf(pfx, sizeof pfx, "nomad.submit.%d.", sid);
    std::vector<SubmitKV> kvs;
    s_submitKVs = &kvs;
    s_submitPfxLen = std::strlen(pfx);
    storageForEach(pfx, collectSubmitField);
    s_submitKVs = nullptr;
    storageDeleteTree(tree);                    /* consume the staged fields */

    std::vector<uint8_t> packed;
    mpMapHeader(packed, kvs.size());
    for (auto& kv : kvs) { mpStr(packed, kv.k); mpStr(packed, kv.v); }

    info("s%d submit %s:%s (%zu fields)", sid, hash.c_str(), path.c_str(), kvs.size());
    startFetch(sid, hash, path, /*bypass_cache=*/true, &packed);
}

/* ─────────────── bookmarks ───────────────
 *
 * s.nomad.bookmarks.<id> = "<hash>[:<path>]|<name>|<note>"
 *
 * Bookmarks address a host AND a path (page bookmarks, not just nodes), so
 * the key is an opaque id (unix seconds, de-collided) — the url can't be a
 * storage key (paths contain dots). The note is last and may contain '|';
 * the url and name must not. Re-adding an existing url updates its
 * name/note in place. */

struct BmFind { std::string url; std::string id; };
static BmFind* s_bmFind = nullptr;
static void bmFindLeaf(const char* key, const char* val)
{
    if (!s_bmFind || !s_bmFind->id.empty() || !key || !val) return;
    const char* tail = key + sizeof("s.nomad.bookmarks.") - 1;
    if (std::strchr(tail, '.')) return;
    std::string v = val;
    size_t bar = v.find('|');
    if (bar == std::string::npos) return;
    if (v.substr(0, bar) == s_bmFind->url) s_bmFind->id = tail;
}

static std::string bookmarkIdForUrl(const std::string& url)
{
    BmFind f{ url, "" };
    s_bmFind = &f;
    storageForEach("s.nomad.bookmarks.", bmFindLeaf);
    s_bmFind = nullptr;
    return f.id;
}

static void onCmdBookmarkAdd(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);
    /* "<hash>[:<path>]|<name>|<note>" — note may contain '|' (it's last). */
    size_t p1 = cmd.find('|');
    if (p1 == std::string::npos) { warn("bookmark add: need <hash>[:<path>]|<name>[|<note>]"); return; }
    std::string url = cmd.substr(0, p1);
    std::string hash = url.substr(0, url.find(':'));
    uint8_t dh[NOMAD_DEST_HASH_LEN];
    if (!hexToDestHash(hash, dh)) { warn("bookmark add: bad hash"); return; }
    if (url.find(':') == std::string::npos)
        url += ":" NOMAD_DEFAULT_PAGE;             /* bare host → index page */
    std::string rest = cmd.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string name = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
    std::string note = (p2 == std::string::npos) ? ""   : rest.substr(p2 + 1);

    std::string id = bookmarkIdForUrl(url);        /* re-add = update in place */
    if (id.empty()) {
        char idb[16];
        int t = nowUnixS();
        for (;;) {
            std::snprintf(idb, sizeof idb, "%d", t);
            char probe[64];
            std::snprintf(probe, sizeof probe, "s.nomad.bookmarks.%s", idb);
            if (!storageExists(probe)) break;
            t++;
        }
        id = idb;
    }
    char k[64];
    std::snprintf(k, sizeof(k), "s.nomad.bookmarks.%s", id.c_str());
    storageSet(k, (url + "|" + name + "|" + note).c_str());
    info("bookmark + %s \"%s\"", url.c_str(), sanitizeForLog(name).c_str());
}

/* val = the bookmark id (key tail), or a "<hash>[:<path>]" url to match. */
static void onCmdBookmarkDel(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string v = val;
    storageUnset(key);
    while (!v.empty() && v.back()  == ' ') v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    if (v.size() >= 32) {                          /* url form */
        if (v.find(':') == std::string::npos) v += ":" NOMAD_DEFAULT_PAGE;
        std::string id = bookmarkIdForUrl(v);
        if (id.empty()) { warn("bookmark del: no match for %s", v.c_str()); return; }
        v = id;
    }
    char k[64];
    std::snprintf(k, sizeof(k), "s.nomad.bookmarks.%s", v.c_str());
    storageUnset(k);
    info("bookmark - %s", v.c_str());
}

/* ─────────────── announce-drift feed ─────────────── */

static int s_announce_sub_handle = -1;

struct NodeEntry { int last_s; int hops; std::string name; };

static std::string buildNodeValue(const NodeEntry& e)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d|%d|", e.last_s, e.hops);
    return std::string(buf) + e.name;   /* name last — may contain '|' */
}

/* nomad.nodes.<hex> LRU eviction by last_s, capped at s.nomad.max_nodes. */
struct NodeOldestCtx { int count; int oldest_s; std::string oldest_hex; };
static NodeOldestCtx* s_node_oldest_ctx = nullptr;

static void nodeOldestLeaf(const char* key, const char* val)
{
    if (!s_node_oldest_ctx || !key || !val) return;
    const char* tail = key + sizeof("nomad.nodes.") - 1;
    if (std::strchr(tail, '.')) return;     /* skip nested keys */
    s_node_oldest_ctx->count++;
    int ls = std::atoi(val);
    if (s_node_oldest_ctx->oldest_hex.empty() || ls < s_node_oldest_ctx->oldest_s) {
        s_node_oldest_ctx->oldest_s   = ls;
        s_node_oldest_ctx->oldest_hex = tail;
    }
}

static int nodeCountAndMaybeOldest(int max_entries, std::string* oldest_out)
{
    NodeOldestCtx ctx{};
    s_node_oldest_ctx = &ctx;
    storageForEach("nomad.nodes.", nodeOldestLeaf);
    s_node_oldest_ctx = nullptr;
    if (max_entries > 0 && ctx.count >= max_entries &&
        !ctx.oldest_hex.empty() && oldest_out)
        *oldest_out = ctx.oldest_hex;
    return ctx.count;
}

static void onAnnounceFromRnsd(int handle, size_t /*bytesAvail*/)
{
    if (handle != s_announce_sub_handle) return;
    PSRAM_BSS static uint8_t buf[NOMAD_ANNOUNCE_HDR + 1024];
    size_t n = itsRecv(handle, buf, sizeof(buf), 0);
    if (n < NOMAD_ANNOUNCE_HDR) {
        if (n > 0) warn("announce sub: short frame %zu B", n);
        return;
    }

    int            hops     = buf[0];
    const uint8_t* dh       = buf + 1;
    const uint8_t* app_data = buf + NOMAD_ANNOUNCE_HDR;
    size_t         app_len  = n - NOMAD_ANNOUNCE_HDR;

    /* rnsd already aspect-filtered to nomadnetwork.node. app_data is the
     * node's display name (UTF-8); may be empty (Node.py:217-221). */
    std::string name(reinterpret_cast<const char*>(app_data), app_len);
    std::string dh_hex = bytesToHex(dh, NOMAD_DEST_HASH_LEN);

    char key[64];
    std::snprintf(key, sizeof(key), "nomad.nodes.%s", dh_hex.c_str());

    bool is_new = !storageExists(key);
    if (is_new) {
        int max_nodes = storageGetInt("s.nomad.max_nodes", 256);
        if (max_nodes > 0) {
            std::string oldest;
            if (nodeCountAndMaybeOldest(max_nodes, &oldest) >= max_nodes &&
                !oldest.empty()) {
                char old_key[64];
                std::snprintf(old_key, sizeof(old_key), "nomad.nodes.%s",
                              oldest.c_str());
                storageUnset(old_key);
            }
        }
    }

    NodeEntry e{ nowUnixS(), hops, name };
    storageSet(key, buildNodeValue(e).c_str());
    verb("node %s name=\"%s\" hops=%d", dh_hex.c_str(),
         sanitizeForLog(name).c_str(), hops);
}

static void onAnnounceSubDisconnect(int /*handle*/)
{
    warn("announce sub: disconnected from rnsd");
    s_announce_sub_handle = -1;       /* reconnect on the next 1 Hz tick */
}

static bool connectAnnounceSub(void)
{
    if (s_announce_sub_handle >= 0) return true;
    rnsd_announces_connect_t req = {};
    safeStrncpy(req.aspect, NOMAD_ASPECT, sizeof(req.aspect));
    int h = itsConnect("rnsd", RNSD_PORT_ANNOUNCES, &req, sizeof(req),
                       pdMS_TO_TICKS(2000), /*ref=*/0,
                       onAnnounceFromRnsd, onAnnounceSubDisconnect);
    if (h < 0) { warn("announce sub: connect failed"); return false; }
    s_announce_sub_handle = h;
    info("announce sub: connected (handle=%d aspect=%s)", h, NOMAD_ASPECT);
    return true;
}

/* ─────────────── CLI ─────────────── */

static int  s_node_rows = 0;
static void nodeListLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("nomad.nodes.") - 1;
    if (std::strchr(tail, '.')) return;
    NodeEntry e{};
    /* "<last_s>|<hops>|<name>" */
    const char* p1 = std::strchr(val, '|');
    if (!p1) return;
    const char* p2 = std::strchr(p1 + 1, '|');
    if (!p2) return;
    e.last_s = std::atoi(val);
    e.hops   = std::atoi(p1 + 1);
    e.name   = p2 + 1;
    cliPrintf("%s  %2d hops  %3ds  %s\n", tail, e.hops,
              nowUnixS() - e.last_s, sanitizeForLog(e.name).c_str());
    s_node_rows++;
}

static void bookmarkListLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("s.nomad.bookmarks.") - 1;
    if (std::strchr(tail, '.')) return;
    /* "<hash>[:<path>]|<name>|<note>" */
    std::string v = val ? val : "";
    size_t p1 = v.find('|');
    std::string url  = (p1 == std::string::npos) ? v : v.substr(0, p1);
    std::string rest = (p1 == std::string::npos) ? "" : v.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string name = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
    std::string note = (p2 == std::string::npos) ? ""   : rest.substr(p2 + 1);
    cliPrintf("%-10s  %s  %s%s%s\n", tail, url.c_str(), sanitizeForLog(name).c_str(),
              note.empty() ? "" : "  — ", sanitizeForLog(note).c_str());
}

static void cliNomad(const char* args)
{
    if (args && strcmp(args, "help") == 0) { cliPrintf("%-*s NomadNet browser: nodes, go, bookmarks\n", CLI_HELP_COL, "nomad [...]"); return; }
    if (args && cliWantsHelp(args)) {
        cliPrintf("nomad nodes                     heard nodes (announce drift)\n");
        cliPrintf("nomad go [<sid>|]<hash>[:<path>]  fetch a page (default %s)\n", NOMAD_DEFAULT_PAGE);
        cliPrintf("nomad reload                    re-fetch session 0 (bypass cache)\n");
        cliPrintf("nomad bookmarks                 list bookmarks (first column = id)\n");
        cliPrintf("nomad bookmark add <hash>[:<path>] <name>[ <note>]\n");
        cliPrintf("nomad bookmark del <id | hash[:<path>]>\n");
        cliPrintf("sessions: 0-5 web tabs, 6 LCD; bare go/reload = session 0\n");
        cliPrintf("page bytes are logged on fetch; nav state in nomad.s<sid>.nav.*\n");
        return;
    }
    /* Bare `nomad` → heard-nodes list as status. */
    if (!args || !*args) args = "nodes";

    if (strcmp(args, "nodes") == 0) {
        s_node_rows = 0;
        storageForEach("nomad.nodes.", nodeListLeaf);
        if (s_node_rows == 0) cliPrintf("(no nodes heard yet)\n");
        return;
    }
    if (strcmp(args, "bookmarks") == 0) {
        storageForEach("s.nomad.bookmarks.", bookmarkListLeaf);
        return;
    }
    if (strncmp(args, "go", 2) == 0 && (args[2] == 0 || args[2] == ' ')) {
        const char* rest = args + 2;
        while (*rest == ' ') rest++;
        if (!*rest) { cliPrintf("usage: nomad go <hash>[:<path>]\n"); return; }
        storageSet("nomad.cmd.go", rest);
        cliPrintf("nomad go: queued — watch log for the page, nomad.nav.* for status\n");
        return;
    }
    if (strcmp(args, "reload") == 0) {
        char v[16];   /* unique per invocation — see the sentinel note up top */
        snprintf(v, sizeof v, "%u", (unsigned)xTaskGetTickCount());
        storageSet("nomad.cmd.reload", v);
        cliPrintf("nomad reload: queued\n");
        return;
    }
    if (strncmp(args, "bookmark", 8) == 0 && (args[8] == 0 || args[8] == ' ')) {
        const char* rest = args + 8;
        while (*rest == ' ') rest++;
        if (strncmp(rest, "add", 3) == 0) {
            rest += 3; while (*rest == ' ') rest++;
            /* "<hash> <name>[ <note>]" → cmd "<hash>|<name>|<note>" */
            std::string r = rest;
            size_t sp = r.find(' ');
            if (sp == std::string::npos) { cliPrintf("usage: nomad bookmark add <hash> <name>[ <note>]\n"); return; }
            std::string hash = r.substr(0, sp);
            std::string tail = r.substr(sp + 1);
            std::string name = tail, note;
            size_t sp2 = tail.find(' ');
            if (sp2 != std::string::npos) { name = tail.substr(0, sp2); note = tail.substr(sp2 + 1); }
            storageSet("nomad.cmd.bookmark.add", (hash + "|" + name + "|" + note).c_str());
            cliPrintf("nomad bookmark add: queued\n");
            return;
        }
        if (strncmp(rest, "del", 3) == 0) {
            rest += 3; while (*rest == ' ') rest++;
            if (!*rest) { cliPrintf("usage: nomad bookmark del <hash>\n"); return; }
            storageSet("nomad.cmd.bookmark.del", rest);
            cliPrintf("nomad bookmark del: queued\n");
            return;
        }
        cliPrintf("usage: nomad bookmark add|del …\n");
        return;
    }
    cliPrintf("unknown subcommand. try `nomad -h`\n");
}

/* ─────────────── task ─────────────── */

static TickType_t s_lastPublishTick = 0;

static TickType_t nextDeadline(void)
{
    TickType_t now = xTaskGetTickCount();
    TickType_t due = s_lastPublishTick + pdMS_TO_TICKS(NOMAD_PUBLISH_INTERVAL_MS);
    if (due <= now) return 0;
    return due - now;
}

static void nomadTaskMain(void*)
{
    info("[%s] task up", TAG);

    /* Boot barrier: stay quiet until rns.ready — clock valid, network up (if
     * configured), and the minimum settle floor elapsed. Bounded fallback so a
     * wedged rnsd can't pin us. No rnsd, no
     * point — so bail (don't start) if rns.ready never comes. */
    if (!waitForFlag("rns.ready", 120)) {
        err("[%s] rns.ready never set — not starting", TAG);
        killSelf();
    }

    /* Client of rnsd (announce fanout + RNSD_PORT_LINK) + an aux-only
     * server port for request responses. itsServerInit sets up the shared
     * inbox; itsClientInit reuses it. */
    if (!itsServerInit()) err("nomad itsServerInit failed");
    itsServerPortOpen(NOMAD_RESP_PORT, /*packetBased=*/false,
                      /*maxHandles=*/1, /*toSize=*/0, /*fromSize=*/0);
    itsOnAux(NOMAD_RESP_PORT, onNomadAux);
    /* announce-fanout sub + up to NOMAD_SESSIONS parallel links, with
     * headroom so a transient lingering conn never blocks a fetch. */
    itsClientInit(NOMAD_SESSIONS + 5);

    storageSubscribeChanges("nomad.cmd.go",            onCmdGo);
    storageSubscribeChanges("nomad.cmd.reload",        onCmdReload);
    storageSubscribeChanges("nomad.cmd.submit",        onCmdSubmit);
    storageSubscribeChanges("nomad.cmd.bookmark.add",  onCmdBookmarkAdd);
    storageSubscribeChanges("nomad.cmd.bookmark.del",  onCmdBookmarkDel);
    /* Reflect each session Link's progress into its nav.status (prefix sub
     * over all rnsd.links.nomad<sid>.* trees; the handler filters .state). */
    storageSubscribeChanges("rnsd.links." NOMAD_FETCH_TAG, onLinkState);

    /* Gate first contact with rnsd on a known-valid clock (or the bounded
     * wait), like lxmf and the transports. rnsd itself only stands up its ITS
     * server surface after the same wait, so connecting earlier just spins on
     * "not initialised as a server" rejects for ~30 s of boot. Waiting lets us
     * connect once, cleanly, right after rnsd comes up. */
    waitForTime(0);

    connectAnnounceSub();
    for (int i = 0; i < NOMAD_SESSIONS; i++) {
        s_sess[i].handle = -1;   /* 0 is a valid ITS handle; start unset */
        navSet(i, "idle");
    }

    s_lastPublishTick = xTaskGetTickCount();

    for (;;) {
        itsPoll(nextDeadline());

        TickType_t now = xTaskGetTickCount();
        if (now - s_lastPublishTick >= pdMS_TO_TICKS(NOMAD_PUBLISH_INTERVAL_MS)) {
            if (s_announce_sub_handle < 0) connectAnnounceSub();
            s_lastPublishTick = now;
        }
    }
}

void nomadInit(void)
{
    if (storageGetInt("s.nomad.version", 0) < NOMAD_VERSION) {
        storageBegin();
        storageDefault("s.nomad.max_nodes", 256);          /* announce-drift cap; 0 disables */
        storageSet("s.nomad.version", NOMAD_VERSION);
        storageEnd();
    }

    cliRegisterCmd("nomad", cliNomad);

    /* Core 1, prio 1, 8 KB PSRAM stack — same class as lxmf. */
    spawnTask(nomadTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);

    /* The on-device Nomad-browser launcher tile self-registers via the
     * when:-gated nomadLcdRegister init: hook (spangap/spangap-lcd), defined in
     * conditional/spangap-lcd/ — not called here, so non-LCD builds drop it
     * entirely. */
}
