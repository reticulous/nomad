/**
 * nomad — Nomad Network page-client task (docs/plans/nomad.md Phase 1).
 *
 * Modelled on the lxmf task: storage is the API, zero mR includes (rides
 * rnsd's byte-array API: announce fan-out + rnsdLinkOpen + rnsdLinkRequest),
 * one itsPoll wait point, cmd sentinels drive behaviour. Never parses
 * Micron — bytes in, bytes out.
 *
 *   announce drift : subscribe RNSD_PORT_ANNOUNCES filtered to
 *                    nomadnetwork.node → nomad.nodes.<hex> LRU.
 *   navigate       : nomad.cmd.go = <hash>[:<path>] → rnsdLinkOpen +
 *                    rnsdLinkRequest(path) → response bytes (REQUEST_RESPONSE
 *                    aux). Statuses → nomad.nav.*.
 *   page cache     : hash:path → bytes (RAM/PSRAM). Re-view = zero air time.
 *                    nomad.cmd.reload bypasses.
 *   bookmarks      : s.nomad.bookmarks.<hex> = <name>|<note> (persistent).
 */
#include "nomad.h"
#include "spangap.h"
#include "ports.h"
#include "rnsd.h"     /* rnsdLinkOpen / rnsdLinkRequest / rnsdLinkTeardown / release */

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
#define NOMAD_FETCH_TAG            "nomad"          /* rnsdLinkOpen tag (≤23) */
#define NOMAD_RESP_PORT            130              /* request-response aux port */
#define NOMAD_DEFAULT_PAGE         "/page/index.mu"

/* Page cache caps (PSRAM). Re-viewing a cached page costs zero air time. */
#define NOMAD_CACHE_MAX_ENTRIES    16
#define NOMAD_CACHE_MAX_BYTES      (512 * 1024)

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

/* ─────────────── navigation / fetch state ─────────────── */

/* Like NomadNet's browser, we keep ONE Link to the current node open and
 * reuse it for every request to that node; it's dropped only on a node
 * change or a failure (or when Reticulum closes it idle/STALE, surfaced via
 * onFetchLinkDisc). So same-node navigation has no re-establish cost and no
 * per-fetch ITS-conn churn. One request in flight at a time (v1). */
static struct {
    bool        active;     /* a request is in flight */
    bool        terminal;   /* done/failed already published for this fetch */
    bool        submit;     /* form submit (don't cache the response) */
    int         req_id;     /* rnsdLinkRequest correlation id */
    int         handle;     /* open RNSD_PORT_LINK handle (-1 = no link) */
    std::string link_hash;  /* node the open link is connected to ("" = none) */
    std::string hash;       /* 32-hex target of the current request */
    std::string path;
    int         started_s;
} s_fetch = {};   /* handle set to -1 in nomadTaskMain before any fetch */

static void navSet(const char* status) { storageSet("nomad.nav.status", status); }

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

static void publishPage(const std::string& hash, const std::string& path,
                        const uint8_t* body, size_t len, bool cache = true)
{
    std::string key = hash + ":" + path;
    if (cache) cachePut(key, body, len);   /* form-submit results aren't cached */
    storageSet("nomad.page.hash", hash.c_str());
    storageSet("nomad.page.path", path.c_str());
    storageSet("nomad.page.size", (int)len);
    storageSet("nomad.page.fetched_s", nowUnixS());

    /* Publish the body to the (ephemeral) config tree so the SPA renders
     * it via the normal storage DC sync — the browser receives full
     * values (the 128 B cap is only the in-device change notification).
     * Capped to stay well under the DC patch limit; the full bytes always
     * live in the RAM cache (the LCD renderer reads those). Micron is
     * UTF-8 text, so a cJSON string value is the right carrier. */
    int cap = storageGetInt("s.nomad.max_page_publish", 32768);
    if ((int)len <= cap) {
        std::string b(reinterpret_cast<const char*>(body), len);
        storageSet("nomad.page.body", b.c_str());
        storageSet("nomad.page.truncated", 0);
    } else {
        storageSet("nomad.page.body", "");
        storageSet("nomad.page.truncated", 1);   /* too large for the SPA; see LCD/file */
    }
    logPage(key, body, len);
}

/* Close the open link + free our ITS conn. Drop our conn FIRST, then
 * teardown: the disconnect makes rnsd null the slot's handle, so the
 * teardown's linkFreeSlot skips its own itsDisconnect — only one side ever
 * disconnects the conn, so no stale DISCONNECT can hit a reused handle (the
 * earlier double-free). itsConnect is synchronous + FIFO-after the teardown
 * aux, so a same-tag reopen right after sees the slot already freed. */
static void dropLink(void)
{
    if (s_fetch.handle >= 0) { itsDisconnect(s_fetch.handle); s_fetch.handle = -1; }
    rnsdLinkTeardown(NOMAD_FETCH_TAG);
    s_fetch.link_hash.clear();
}

/* A request concluded. On success keep the link open for same-node reuse;
 * on failure drop it so the next attempt re-establishes cleanly. */
static void fetchDone(bool ok)
{
    s_fetch.terminal = true;
    s_fetch.active   = false;
    if (!ok) dropLink();
}

/* The link's packet handle is unused (responses ride the aux port), but
 * rnsd opens it packet-mode — drain defensively. */
static void onFetchLinkRecv(int handle, size_t /*n*/)
{
    uint8_t b[256];
    itsRecv(handle, b, sizeof(b), 0);
}
/* rnsd closed the link (teardown cascade, or idle/STALE close): our conn is
 * gone, so forget it — the next fetch re-establishes. */
static void onFetchLinkDisc(int /*handle*/)
{
    s_fetch.handle = -1;
    s_fetch.link_hash.clear();
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

    /* Phase 1 only drives request/response (page GETs). /file Resource
     * downloads (RNSD_LINK_RESOURCE_INBOUND_DONE) are Phase 5. */
    if (d.opcode == RNSD_LINK_REQUEST_RESPONSE) {
        if (!s_fetch.active || (int)d.opaque_id != s_fetch.req_id) {
            verb("aux: stray response cid=%u (no matching fetch)",
                 (unsigned)d.opaque_id);
            rnsdResourceRelease(d.buf);
            return;
        }
        info("fetch %s:%s done — %uB", s_fetch.hash.c_str(),
             s_fetch.path.c_str(), (unsigned)d.len);
        publishPage(s_fetch.hash, s_fetch.path,
                    (const uint8_t*)d.buf, d.len, /*cache=*/!s_fetch.submit);
        navSet("done");
        rnsdResourceRelease(d.buf);   /* we own it on REQUEST_RESPONSE */
        fetchDone(true);              /* keep the link open for same-node reuse */
        return;
    }

    if (d.opcode == RNSD_LINK_REQUEST_FAILED) {
        if (!s_fetch.active || (int)d.opaque_id != s_fetch.req_id) return;
        warn("fetch %s:%s failed", s_fetch.hash.c_str(), s_fetch.path.c_str());
        storageSet("nomad.nav.error", "request failed");
        navSet("failed");
        fetchDone(false);            /* drop the link; next attempt re-establishes */
        return;
    }

    if (d.buf) rnsdResourceRelease(d.buf);   /* unexpected opcode w/ buffer */
}

/* rnsd publishes the Link's progress to rnsd.links.<tag>.state. Reflect it
 * into nomad.nav.status while a fetch is active so the frontend sees the
 * Browser.py-style progression without nomad re-deriving it. The terminal
 * states (done/failed) are owned by onNomadAux above — don't clobber them. */
static void onLinkState(const char* /*key*/, const char* val)
{
    if (!s_fetch.active || s_fetch.terminal || !val || !*val) return;
    if      (std::strcmp(val, "awaiting_path") == 0) navSet("path_requested");
    else if (std::strcmp(val, "establishing")  == 0) navSet("establishing");
    else if (std::strcmp(val, "active")        == 0) navSet("requesting");
    /* closing/closed/failed: leave to the aux handler / linkFreeSlot's
     * REQUEST_FAILED, which carries the proper terminal reason. */
}

/* ─────────────── navigate ─────────────── */

/* Fetch a page (packed == nullptr → GET) or submit a form (packed == the
 * msgpack {field_*,var_*} map). Submits bypass the cache and aren't cached. */
static void startFetch(const std::string& hash, const std::string& path,
                       bool bypass_cache, const std::vector<uint8_t>* packed = nullptr)
{
    uint8_t dh[NOMAD_DEST_HASH_LEN];
    if (!hexToDestHash(hash, dh)) {
        warn("go: bad hash %s", hash.c_str());
        storageSet("nomad.nav.error", "bad hash");
        navSet("failed");
        return;
    }

    storageSet("nomad.nav.hash", hash.c_str());
    storageSet("nomad.nav.path", path.c_str());
    storageSet("nomad.nav.error", "");

    std::string key = hash + ":" + path;
    if (!bypass_cache && !packed) {
        if (PageEntry* e = cacheGet(key)) {
            info("go %s — cache hit (%zuB, zero air time)", key.c_str(),
                 e->body.size());
            publishPage(hash, path, e->body.data(), e->body.size());
            navSet("done");
            return;
        }
    }

    /* Reuse the open link if it's already to this node (NomadNet model);
     * otherwise drop a link to a different node and open a fresh one. */
    bool reuse = (s_fetch.handle >= 0 && s_fetch.link_hash == hash);
    if (!reuse) {
        if (s_fetch.handle >= 0) dropLink();   /* link is to a different node */
        int h = rnsdLinkOpen(dh, NOMAD_ASPECT, /*identity_key=*/"", NOMAD_FETCH_TAG,
                             /*path_timeout_ms=*/0, /*ref=*/0,
                             onFetchLinkRecv, onFetchLinkDisc);
        if (h < 0) {
            warn("go: rnsdLinkOpen failed (%d)", h);
            storageSet("nomad.nav.error", "link open failed");
            navSet("failed");
            return;
        }
        s_fetch.handle    = h;
        s_fetch.link_hash = hash;
    }
    /* rnsd holds the request until the Link is ACTIVE, then issues it (or, on
     * reuse, issues it immediately); a link failure fails the request back to
     * us (REQUEST_FAILED). */
    int rid = packed
        ? rnsdLinkRequest(NOMAD_FETCH_TAG, path.c_str(), packed->data(), packed->size(),
                          NOMAD_RESP_PORT, /*data_packed=*/true)
        : rnsdLinkRequest(NOMAD_FETCH_TAG, path.c_str(), nullptr, 0, NOMAD_RESP_PORT);
    if (rid < 0) {
        warn("go: rnsdLinkRequest failed (%d)", rid);
        dropLink();
        storageSet("nomad.nav.error", "request failed");
        navSet("failed");
        return;
    }

    s_fetch.active    = true;
    s_fetch.terminal  = false;
    s_fetch.submit    = (packed != nullptr);
    s_fetch.req_id    = rid;
    s_fetch.hash      = hash;
    s_fetch.path      = path;
    s_fetch.started_s = nowUnixS();
    navSet(reuse ? "requesting" : "establishing");
    info("%s %s:%s (req_id=%d%s)", packed ? "submit" : "go",
         hash.c_str(), path.c_str(), rid, reuse ? ", reused link" : "");
}

/* nomad.cmd.go = "<hash>[:<path>]". Empty path → /page/index.mu. */
static void onCmdGo(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);

    std::string hash = cmd, path = NOMAD_DEFAULT_PAGE;
    size_t colon = cmd.find(':');
    if (colon != std::string::npos) {
        hash = cmd.substr(0, colon);
        path = cmd.substr(colon + 1);
        if (path.empty()) path = NOMAD_DEFAULT_PAGE;
    }
    startFetch(hash, path, /*bypass_cache=*/false);
}

static void onCmdReload(const char* key, const char* val)
{
    if (!val || !*val) return;
    storageUnset(key);
    char hash[64] = {}, path[128] = {};
    storageGetStr("nomad.nav.hash", hash, sizeof(hash), "");
    storageGetStr("nomad.nav.path", path, sizeof(path), NOMAD_DEFAULT_PAGE);
    if (!hash[0]) { warn("reload: nothing navigated yet"); return; }
    startFetch(hash, path, /*bypass_cache=*/true);
}

/* Form submit (Phase 4). The frontend stages the field values under
 * `nomad.submit.<field_*|var_*>` (keys are already the NomadNet map keys),
 * then writes `nomad.cmd.submit = "<hash>:<path>"`. We pack those k/v into
 * a msgpack map and issue a request with data_packed=true so µR splices it
 * as the request envelope's 3rd element. */
struct SubmitKV { std::string k, v; };
static std::vector<SubmitKV>* s_submitKVs = nullptr;
static void collectSubmitField(const char* key, const char* val)
{
    if (!s_submitKVs || !key) return;
    const char* tail = key + sizeof("nomad.submit.") - 1;
    if (!*tail) return;
    s_submitKVs->push_back({ tail, val ? val : "" });
}

static void onCmdSubmit(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);

    std::string hash = cmd, path = NOMAD_DEFAULT_PAGE;
    size_t colon = cmd.find(':');
    if (colon != std::string::npos) {
        hash = cmd.substr(0, colon);
        path = cmd.substr(colon + 1);
        if (path.empty()) path = NOMAD_DEFAULT_PAGE;
    }

    std::vector<SubmitKV> kvs;
    s_submitKVs = &kvs;
    storageForEach("nomad.submit.", collectSubmitField);
    s_submitKVs = nullptr;
    storageDeleteTree("nomad.submit");          /* consume the staged fields */

    std::vector<uint8_t> packed;
    mpMapHeader(packed, kvs.size());
    for (auto& kv : kvs) { mpStr(packed, kv.k); mpStr(packed, kv.v); }

    info("submit %s:%s (%zu fields)", hash.c_str(), path.c_str(), kvs.size());
    startFetch(hash, path, /*bypass_cache=*/true, &packed);
}

/* ─────────────── bookmarks (s.nomad.bookmarks.<hex> = <name>|<note>) ─────────────── */

static void onCmdBookmarkAdd(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string cmd = val;
    storageUnset(key);
    /* "<hash>|<name>|<note>" — note may contain '|' (it's last). */
    size_t p1 = cmd.find('|');
    if (p1 == std::string::npos) { warn("bookmark add: need <hash>|<name>[|<note>]"); return; }
    std::string hash = cmd.substr(0, p1);
    uint8_t dh[NOMAD_DEST_HASH_LEN];
    if (!hexToDestHash(hash, dh)) { warn("bookmark add: bad hash"); return; }
    std::string rest = cmd.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string name = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
    std::string note = (p2 == std::string::npos) ? ""   : rest.substr(p2 + 1);
    char k[64];
    std::snprintf(k, sizeof(k), "s.nomad.bookmarks.%s", hash.c_str());
    storageSet(k, (name + "|" + note).c_str());
    info("bookmark + %s \"%s\"", hash.c_str(), sanitizeForLog(name).c_str());
}

static void onCmdBookmarkDel(const char* key, const char* val)
{
    if (!val || !*val) return;
    std::string hash = val;
    storageUnset(key);
    /* strip surrounding whitespace */
    while (!hash.empty() && hash.back()  == ' ') hash.pop_back();
    while (!hash.empty() && hash.front() == ' ') hash.erase(0, 1);
    char k[64];
    std::snprintf(k, sizeof(k), "s.nomad.bookmarks.%s", hash.c_str());
    storageUnset(k);
    info("bookmark - %s", hash.c_str());
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
    static uint8_t buf[NOMAD_ANNOUNCE_HDR + 1024];
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
    cliPrintf("  %s  %2d hops  %3ds  %s\n", tail, e.hops,
              nowUnixS() - e.last_s, sanitizeForLog(e.name).c_str());
    s_node_rows++;
}

static void bookmarkListLeaf(const char* key, const char* val)
{
    const char* tail = key + sizeof("s.nomad.bookmarks.") - 1;
    if (std::strchr(tail, '.')) return;
    std::string name = val ? val : "";
    size_t bar = name.find('|');
    std::string note = (bar == std::string::npos) ? "" : name.substr(bar + 1);
    if (bar != std::string::npos) name = name.substr(0, bar);
    cliPrintf("  %s  %s%s%s\n", tail, sanitizeForLog(name).c_str(),
              note.empty() ? "" : "  — ", sanitizeForLog(note).c_str());
}

static void cliNomad(const char* args)
{
    if (!args || !*args || strcmp(args, "help") == 0) {
        cliPrintf("usage: nomad nodes                     — heard nodes (announce drift)\n");
        cliPrintf("       nomad go <hash>[:<path>]        — fetch a page (default %s)\n",
                  NOMAD_DEFAULT_PAGE);
        cliPrintf("       nomad reload                    — re-fetch current (bypass cache)\n");
        cliPrintf("       nomad bookmarks                 — list bookmarks\n");
        cliPrintf("       nomad bookmark add <hash> <name>[ <note>]\n");
        cliPrintf("       nomad bookmark del <hash>\n");
        cliPrintf("  page bytes are logged on fetch; nav state in nomad.nav.*\n");
        return;
    }

    if (strcmp(args, "nodes") == 0) {
        s_node_rows = 0;
        storageForEach("nomad.nodes.", nodeListLeaf);
        if (s_node_rows == 0) cliPrintf("  (no nodes heard yet)\n");
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
        storageSet("nomad.cmd.reload", "1");
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
    cliPrintf("usage: nomad [nodes|go|reload|bookmarks|bookmark …]\n");
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

    /* Client of rnsd (announce fanout + RNSD_PORT_LINK) + an aux-only
     * server port for request responses. itsServerInit sets up the shared
     * inbox; itsClientInit reuses it. */
    if (!itsServerInit()) err("nomad itsServerInit failed");
    itsServerPortOpen(NOMAD_RESP_PORT, /*packetBased=*/false,
                      /*maxHandles=*/1, /*toSize=*/0, /*fromSize=*/0);
    itsOnAux(NOMAD_RESP_PORT, onNomadAux);
    /* announce-fanout sub + the (reused) link, with generous headroom so a
     * transient lingering conn never blocks a fetch. */
    itsClientInit(8);

    storageSubscribeChanges("nomad.cmd.go",            onCmdGo);
    storageSubscribeChanges("nomad.cmd.reload",        onCmdReload);
    storageSubscribeChanges("nomad.cmd.submit",        onCmdSubmit);
    storageSubscribeChanges("nomad.cmd.bookmark.add",  onCmdBookmarkAdd);
    storageSubscribeChanges("nomad.cmd.bookmark.del",  onCmdBookmarkDel);
    /* Reflect rnsd's per-fetch Link progress into nomad.nav.status. */
    storageSubscribeChanges("rnsd.links." NOMAD_FETCH_TAG ".state", onLinkState);

    connectAnnounceSub();
    s_fetch.handle = -1;   /* 0 is a valid ITS handle; start unset */
    navSet("idle");

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
        storageDefault("s.nomad.max_page_publish", 32768); /* max page bytes synced to the SPA */
        storageSet("s.nomad.version", NOMAD_VERSION);
        storageEnd();
    }

    cliRegisterCmd("nomad", cliNomad);

    /* Core 1, prio 1, 8 KB PSRAM stack — same class as lxmf. */
    spawnTask(nomadTaskMain, TAG, 8192, nullptr, 1, 1, STACK_PSRAM);
}
