/**
 * nomad_lcd.cpp — on-device "Nomad" browser program (LVGL).
 *
 * The on-device half of the Nomad Network page browser (see ../../../../INTERNALS.md).
 * Two screens within the one program layer, mirroring the
 * LXMessenger model:
 *   - List:  bookmarks on top, announce-drift nodes below; tap one to open
 *            its index page.
 *   - Page:  top bar — ☰ site list, ⟳ reload, ‹ back (history; site list at
 *            the bottom of the stack), node name, then −/+ font steppers at
 *            the right — over a scrollable rendered Micron view. The left
 *            button cluster matches the web UI's address bar.
 *
 * The C++ Micron→LVGL renderer is independent of the SPA's TS renderer and
 * native to this UI: single column, hard-wrapped to the screen width, links
 * are focusable/clickable widgets the trackball steps through. It diverges
 * from the SPA in layout + interaction by design. Inline `Fxxx/`Bxxx colours
 * render: fg-only lines flow as spangroup spans (proper inline wrap); lines
 * with bg colours or widgets render as a flex row of styled labels. v1
 * simplifications (LCD only): bold/italic/underline style codes are dropped
 * (headings are colour-emphasised; links are link-coloured + underlined);
 * input fields render as editable textareas, submitted as forms. Reimplemented
 * from the Micron grammar — NomadNet is GPL-3.0, not copied.
 *
 * Storage is the API (same keys the firmware nomad task + SPA use):
 *   nomad.nodes.<hex>        "<last_s>|<hops>|<name>"   announced nodes
 *   s.nomad.bookmarks.<id>   "<hash>[:<path>]|<name>|<note>"
 *   nomad.s6.nav.{status,hash,path}                     navigation state
 *   nomad.s6.page.{body,size,truncated,hash,path}       current page
 *   nomad.cmd.go = "6|<hash>:<path>"                    navigate sentinel
 * (the LCD owns nomad session 6 — NOMAD_LCD_SESSION; web tabs own 0-5)
 * Everything runs on the lcd task; storage subscriptions dispatch there, so
 * we touch LVGL straight from the change callback.
 *
 * This whole file lives under conditional/spangap-lcd/, compiled only when the
 * lcd straddle is staged, so no #if is needed and NomadApp is a when:-gated
 * boot-registered service (spangap/spangap-lcd) rather than a self-call.
 */
#include "lcd.h"
#include "lcd_app.h"   /* LcdApp + lcdInstall */
#include "nomad_app.h" /* NomadApp — this straddle's services: class */
#include "mem.h"
#include "storage.h"
#include "compat.h"
#include "nomad.h"
#include "log.h"   /* NOMAD_LCD_SESSION — this browser's session id */

/* Session-prefixed key/value literals. The LCD owns nomad session 6; web
 * tabs own 0-5 (see nomad.h). */
static_assert(NOMAD_LCD_SESSION == 6, "update the NSID literal below");
#define NSID      "6"                      /* session id as a string  */
#define NKEY(t)   "nomad.s" NSID "." t     /* nomad.s6.<tail>         */

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

namespace {

/* Chrome (header symbols, list rows, status) uses Montserrat — it carries the
 * LVGL symbol glyphs and accented node names. The rendered Micron page itself is
 * terminal content: box-drawing + column-aligned graphics only line up under a
 * fixed-width font, so it uses the platform's monospace bitmap fonts. The page
 * font is user-selectable via the header's −/+ buttons, smallest first:
 * Micro 2×3 (160 cols — an unreadable-by-design whole-page thumbnail for the
 * oversized "graphics" some nodes draw), Tom Thumb 4×6 (80 cols, the default),
 * then Spleen 5×8 (64 cols). All carry the complete box-drawing +
 * block-element set, so micron "graphics" render; the 2 bpp fonts grey the
 * partial-coverage pixels. s.nomad.page_font persists the index; kPageFont is
 * refreshed from it on every page rebuild. */
/* Chrome font (headers, links, form fields) — the vector UI face, scaled by the
 * platform zoom. Set by refreshChromeFont() before any UI is built; the bitmap
 * default is only a placeholder for the pre-build window. */
const lv_font_t* kFont = &lv_font_montserrat_12_latin;
/* Page-font ladder, smallest first. The bottom two steps are the platform's
 * bitmap terminal fonts: Micro 2×3 (160 cols — the unreadable-by-design
 * whole-page thumbnail for the oversized "graphics" some nodes draw),
 * Tom Thumb 4×6 (80 cols — a full NomadNet page width fits unwrapped) and
 * Spleen 5×8 (64 cols, the default). All three carry the complete box-drawing
 * + block-element set, so micron art renders; and being bitmaps their glyph
 * measuring is a table lookup, far cheaper than the vector engine on
 * art-heavy pages. Above them, the vector MONO sizes. s.nomad.page_font persists the index; the −/+ header
 * steppers walk it. Not zoom-scaled — the ladder IS the density control. */
struct PageFontStep { const lv_font_t* bitmap; int px; };   /* bitmap set → fixed font; else vector px */
const PageFontStep kPageFontLadder[] = {
    { &lv_font_micro_2x3,    0 },
    { &lv_font_tomthumb_4x6, 0 },
    { &lv_font_spleen_5x8,   0 },
    { nullptr, 10 }, { nullptr, 12 }, { nullptr, 14 }, { nullptr, 16 }, { nullptr, 20 },
};
const int kPageFontN = (int)(sizeof(kPageFontLadder) / sizeof(kPageFontLadder[0]));
const lv_font_t* kPageFont = nullptr;   /* resolved from the index on rebuild */
const int HDR_H = 20;

int pageFontIdx() {
    int idx = storageGetInt("s.nomad.page_font", 2);   /* default: Spleen 5×8 */
    if (idx < 0) idx = 0;
    if (idx >= kPageFontN) idx = kPageFontN - 1;
    return idx;
}

/* Resolve a ladder index: a bitmap step returns its fixed font; a vector step
 * resolves through the engine (lcd task — created lazily). */
const lv_font_t* resolvePageFont(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kPageFontN) idx = kPageFontN - 1;
    const PageFontStep& s = kPageFontLadder[idx];
    return s.bitmap ? s.bitmap : lcdFont(LcdFace::MONO, s.px);
}

/* (Re)resolve the vector chrome + page fonts at the current UI zoom. Call at the
 * top of each top-level build (onCreate / rebuildList / rebuildPage) so both are
 * live before any label reads kFont/kPageFont. Lcd task. */
void refreshFonts() {
    kFont     = lcdFont(LcdFace::UI, (int)(14 * lcdUiScale() + 0.5f));
    kPageFont = resolvePageFont(pageFontIdx());
}
const char* const DEFAULT_PAGE = "/page/index.mu";

/* ---- printable filter (shared idea with lxmf_lcd): drop control bytes and
 * any codepoint the font can't draw, so unrenderable unicode never leaves
 * placeholder boxes. `oneLine` folds CR/LF/TAB to a space. ---- */
std::string printable(std::string_view in, bool oneLine, const lv_font_t* font = kFont) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t b = (uint8_t)in[i];
        uint32_t cp; size_t len;
        if      (b < 0x80)           { cp = b;        len = 1; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; len = 2; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; len = 3; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; len = 4; }
        else { i++; continue; }
        if (i + len > n) break;
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            uint8_t c = (uint8_t)in[i + k];
            if ((c & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (c & 0x3F);
        }
        if (!ok) { i++; continue; }
        size_t at = i;
        i += len;
        if (cp == '\n' || cp == '\r' || cp == '\t') { out += oneLine ? ' ' : (char)cp; continue; }
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0) || g.is_placeholder) continue;
        out.append(in.data() + at, len);
    }
    return out;
}

/* Per-render LVGL-object budget. A Micron art page colours every character
 * cell (`Bxxx per cell), which naively becomes one label per cell — thousands
 * of objects whose flex re-layout (with vector-font glyph measuring per pass)
 * effectively never finishes and, on the lcd task, starves everything below
 * it. Same-style runs are merged in addLine; this cap is the hard backstop:
 * past it the page is truncated with a notice. */
static int s_renderObjs = 0;
constexpr int kRenderObjMax = 600;

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, lv_color_t color,
                  const lv_font_t* font = kFont) {
    lv_obj_t* l = lv_label_create(parent);
    if (!l) return nullptr;                 /* LVGL heap exhausted — skip, don't crash */
    s_renderObjs++;
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, txt.c_str());
    return l;
}

/* ---- state (lcd-task-only) ---- */

lv_obj_t* s_layer    = nullptr;
lv_obj_t* s_list     = nullptr;   /* bookmarks + nodes screen */
lv_obj_t* s_page     = nullptr;   /* page screen (built once, reused) */
lv_obj_t* s_pageBody = nullptr;   /* scroll container for rendered Micron */
lv_obj_t* s_pageName = nullptr;   /* header node-name label */
lv_obj_t* s_status   = nullptr;   /* header status label */
std::string s_curHash;            /* node of the open page */
std::string s_curPath;            /* path of the open page */
bool s_subscribed = false;

/* Back history (this browser context only; the firmware is stateless re
 * history). Pushed on every in-page navigation; the header's ‹ pops it,
 * falling back to the site list at the bottom of the stack. */
std::vector<std::pair<std::string, std::string>> s_hist;

/* Site-list interaction hold: while the operator is touching/scrolling the
 * list, announce churn must not reorder it under their finger. Storage
 * changes within the hold window mark the list dirty; a timer rebuilds it
 * once the interaction has been quiet for LIST_HOLD_MS. */
constexpr uint32_t LIST_HOLD_MS = 10000;
uint32_t    s_listTouchTick = 0;
bool        s_listDirty     = false;
bool        s_listRestoring = false;   /* programmatic scroll — not a touch */
lv_timer_t* s_listTimer     = nullptr;

/* Status hover: the connection-status / path+size line floats top-right
 * over the page, auto-hiding 2 s after the page is loaded and reappearing
 * on scroll. While loading/failed it stays. */
constexpr uint32_t STATUS_HIDE_MS = 2000;
lv_timer_t* s_statusTimer  = nullptr;
bool        s_statusSticky = false;    /* loading/failed — no auto-hide */

/* Header widgets that change state: ★ bookmark toggle, −/+ font steppers
 * (greyed at the ends of the font ladder). */
lv_obj_t* s_starBtn   = nullptr;
lv_obj_t* s_fontMinus = nullptr;
lv_obj_t* s_fontPlus  = nullptr;

/* FontAwesome star (U+F005) — added to lv_font_montserrat_12_latin by
 * scripts/gen-text-font.py; not part of LVGL's stock LV_SYMBOL set. */
#define SYMBOL_STAR "\xEF\x80\x85"

/* Link targets / row targets referenced from LVGL click callbacks by index
 * (intptr_t user_data) into these — no per-widget heap, no dangling
 * pointers. Rebuilt with each screen. */
std::vector<std::string> s_linkTargets;
std::vector<std::string> s_lxmfTargets;   /* lxmf@<hash> link dest hashes, by index */
std::vector<std::pair<std::string, std::string>> s_rowTargets;   /* {hash, path} */

/* Form fields on the current page: name → its LVGL textarea. Read when a
 * form link is followed; cleared with the page. */
std::vector<std::pair<std::string, lv_obj_t*>> s_fields;

void rebuildList();
void rebuildPage();
void showList();

/* ---- helpers ---- */

bool isHash(std::string_view s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

/* Match "lxmf@" + 32 hex at line[pos], not followed by a 33rd hex digit.
 * Case-insensitive prefix + hex; fills `hash` with the lowercase 32-hex. */
bool matchLxmfAt(const std::string& line, size_t pos, std::string& hash) {
    static const char pfx[] = "lxmf@";
    auto hex = [](char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    if (pos + 5 + 32 > line.size()) return false;
    for (int k = 0; k < 5; k++) {
        char a = line[pos + k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != pfx[k]) return false;
    }
    for (int k = 0; k < 32; k++) if (!hex(line[pos + 5 + k])) return false;
    if (pos + 5 + 32 < line.size() && hex(line[pos + 5 + 32])) return false;
    hash.assign(line, pos + 5, 32);
    for (auto& c : hash) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return true;
}

/* Scan the bookmark tree ("<hash>[:<path>]|<name>|<note>", id-keyed) for a
 * host (+ optional exact path) match. Empty id in the result = no match. */
struct BmScan { std::string hash, path /* "" = any */, id, name; };
BmScan* s_bmScan = nullptr;
void bmScanLeaf(const char* key, const char* val) {
    if (!s_bmScan || !s_bmScan->id.empty() || !key || !val) return;
    const char* tail = key + sizeof("s.nomad.bookmarks.") - 1;
    if (strchr(tail, '.')) return;
    std::string v = val;
    size_t p1 = v.find('|');
    if (p1 == std::string::npos) return;
    std::string url = v.substr(0, p1);
    size_t colon = url.find(':');
    std::string h = colon == std::string::npos ? url : url.substr(0, colon);
    std::string p = colon == std::string::npos ? DEFAULT_PAGE : url.substr(colon + 1);
    if (h != s_bmScan->hash) return;
    if (!s_bmScan->path.empty() && p != s_bmScan->path) return;
    std::string rest = v.substr(p1 + 1);
    size_t p2 = rest.find('|');
    s_bmScan->id   = tail;
    s_bmScan->name = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
}
BmScan bookmarkFor(const std::string& hash, const std::string& path) {
    BmScan s{ hash, path, "", "" };
    s_bmScan = &s;
    storageForEach("s.nomad.bookmarks.", bmScanLeaf);
    s_bmScan = nullptr;
    return s;
}

std::string nodeName(const std::string& hash) {
    BmScan bm = bookmarkFor(hash, "");
    if (!bm.id.empty() && !bm.name.empty()) return printable(bm.name, true);
    char k[80];
    snprintf(k, sizeof k, "nomad.nodes.%s", hash.c_str());
    std::string v = storageGetStr(k, "");
    if (!v.empty()) {                       /* "<last_s>|<hops>|<name>" */
        size_t p1 = v.find('|');
        size_t p2 = p1 == std::string::npos ? std::string::npos : v.find('|', p1 + 1);
        if (p2 != std::string::npos) {
            std::string nm = v.substr(p2 + 1);
            if (!nm.empty()) return printable(nm, true);
        }
    }
    return hash.substr(0, 8) + "...";
}

/* ---- status hover + header button state ---- */

void statusTimerCb(lv_timer_t* t) {
    lv_timer_pause(t);
    if (s_status && lv_obj_is_valid(s_status))
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
}

/* Show the status hover; with autohide it disappears after STATUS_HIDE_MS
 * (page loaded), without it stays (loading / failed). */
void statusShow(bool autohide) {
    if (!s_status) return;
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_status);
    if (!s_statusTimer) return;
    if (autohide) {
        lv_timer_set_period(s_statusTimer, STATUS_HIDE_MS);
        lv_timer_reset(s_statusTimer);
        lv_timer_resume(s_statusTimer);
    } else {
        lv_timer_pause(s_statusTimer);
    }
}

/* Star lit when the open page is bookmarked; −/+ grey at the ladder ends. */
void updateHdrButtons() {
    if (s_starBtn) {
        std::string path = s_curPath.empty() ? DEFAULT_PAGE : s_curPath;
        bool bm = isHash(s_curHash) && !bookmarkFor(s_curHash, path).id.empty();
        lv_obj_set_style_text_color(s_starBtn,
            lv_color_hex(bm ? 0xFFD24A : 0x565C64), 0);
    }
    int idx = pageFontIdx();
    if (s_fontMinus)
        lv_obj_set_style_text_color(s_fontMinus,
            lv_color_hex(idx <= 0 ? 0x565C64 : 0xC0C8D0), 0);
    if (s_fontPlus)
        lv_obj_set_style_text_color(s_fontPlus,
            lv_color_hex(idx >= kPageFontN - 1 ? 0x565C64 : 0xC0C8D0), 0);
}

/* Navigate: write the go sentinel; the firmware fetches and republishes
 * nomad.page.* / nomad.nav.*, which our storage subscription renders. */
void pushHist() {
    if (!isHash(s_curHash) || s_curPath.empty()) return;
    s_hist.push_back({ s_curHash, s_curPath });
    if (s_hist.size() > 32) s_hist.erase(s_hist.begin());
}

void navigate(const std::string& hash, const std::string& path, bool push = true) {
    if (!isHash(hash)) return;
    std::string p = path.empty() ? DEFAULT_PAGE : path;
    if (push && (hash != s_curHash || p != s_curPath)) pushHist();
    s_curHash = hash;
    s_curPath = p;
    storageSet("nomad.cmd.go", (NSID "|" + hash + ":" + p).c_str());
}

/* ‹ back: pop the history; at the bottom of the stack, the site list. */
void goBack() {
    if (s_hist.empty()) { showList(); return; }
    auto e = s_hist.back();
    s_hist.pop_back();
    navigate(e.first, e.second, /*push=*/false);
}

/* Resolve a Micron URL (before any `field/`var segments) → hash+path.
 * "<hash>:<path>" | ":<path>"/"<path>" (current node) | "<hash>".
 * @-scheme / "://" deferred. Returns false if unresolvable. */
bool resolveUrl(const std::string& url, std::string& hash, std::string& path) {
    std::string u = url;
    if (u.empty()) return false;
    if (u.find("://") != std::string::npos) return false;
    size_t at = u.find('@');
    if (at != std::string::npos && at > 0 && u.find('/') == std::string::npos) return false;
    size_t colon = u.find(':');
    if (colon == 32 && isHash(u.substr(0, 32))) {
        hash = u.substr(0, 32);
        path = u.substr(colon + 1); if (path.empty()) path = DEFAULT_PAGE;
        return true;
    }
    if (isHash(u)) { hash = u; path = DEFAULT_PAGE; return true; }
    std::string p = (u[0] == ':') ? u.substr(1) : u;
    if (!isHash(s_curHash)) return false;
    hash = s_curHash; path = p.empty() ? DEFAULT_PAGE : p;
    return true;
}

/* Follow a Micron link target `url`fields`vars`. No fields/vars → navigate
 * (GET). Else gather the named field textareas (`*` = all) + var literals,
 * stage under nomad.submit.*, and trigger nomad.cmd.submit — the nomad task
 * packs the msgpack map and issues the request. */
void followTarget(const std::string& target) {
    if (target.empty()) return;

    std::vector<std::string> parts;     /* split by '`' */
    {
        size_t s = 0;
        for (size_t i = 0; i <= target.size(); i++) {
            if (i == target.size() || target[i] == '`') { parts.push_back(target.substr(s, i - s)); s = i + 1; }
        }
    }
    std::string url = parts.empty() ? "" : parts[0];
    std::vector<std::string> fields;
    std::vector<std::pair<std::string, std::string>> vars;
    for (size_t i = 1; i < parts.size(); i++) {
        const std::string& seg = parts[i];
        if (seg.empty()) continue;
        if (seg.find('=') != std::string::npos) {            /* vars: name=val|… */
            size_t s = 0;
            for (size_t j = 0; j <= seg.size(); j++) {
                if (j == seg.size() || seg[j] == '|') {
                    std::string pair = seg.substr(s, j - s); s = j + 1;
                    size_t eq = pair.find('=');
                    if (eq != std::string::npos && eq > 0)
                        vars.push_back({ pair.substr(0, eq), pair.substr(eq + 1) });
                }
            }
        } else {                                             /* fields: name|…|* */
            size_t s = 0;
            for (size_t j = 0; j <= seg.size(); j++) {
                if (j == seg.size() || seg[j] == '|') {
                    std::string f = seg.substr(s, j - s); s = j + 1;
                    if (!f.empty()) fields.push_back(f);
                }
            }
        }
    }

    /* An lxmf@<hash> target opens an LXMF conversation instead of navigating —
     * hand the dest hash to LXMF via its inbound link var (lxmf_lcd subscribes). */
    std::string lxh;
    if (url.size() == 5 + 32 && matchLxmfAt(url, 0, lxh)) {
        char v[48];                            /* nonce: repeat taps re-fire */
        snprintf(v, sizeof v, "%s:%u", lxh.c_str(), (unsigned)lv_tick_get());
        storageSet("lxmf.url_lcd", v);
        return;
    }

    std::string hash, path;
    if (!resolveUrl(url, hash, path)) return;

    if (fields.empty() && vars.empty()) { navigate(hash, path); return; }

    /* Form submit (staged under this session's tree). */
    storageBegin();
    storageDeleteTree("nomad.submit." NSID);
    bool wantAll = std::find(fields.begin(), fields.end(), std::string("*")) != fields.end();
    for (auto& f : s_fields) {
        if (!wantAll && std::find(fields.begin(), fields.end(), f.first) == fields.end()) continue;
        const char* v = lv_textarea_get_text(f.second);
        char k[96]; snprintf(k, sizeof k, "nomad.submit." NSID ".field_%s", f.first.c_str());
        storageSet(k, v ? v : "");
    }
    for (auto& v : vars) {
        char k[96]; snprintf(k, sizeof k, "nomad.submit." NSID ".var_%s", v.first.c_str());
        storageSet(k, v.second.c_str());
    }
    if (hash != s_curHash || path != s_curPath) pushHist();
    s_curHash = hash;
    s_curPath = path;
    storageSet("nomad.cmd.submit", (NSID "|" + hash + ":" + path).c_str());
    storageEnd();
}

void onLinkClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_linkTargets.size()) followTarget(s_linkTargets[idx]);
}

/* ---- Micron → LVGL ----
 *
 * Inline scan: text runs + links. We strip style controls (`!/`_/`*, colors,
 * align) for the LCD and keep links. A run is plain text; a link is a
 * clickable, focusable label. */

enum SegKind { SEG_TEXT, SEG_LINK, SEG_LXMF, SEG_FIELD };

/* "No colour" — fall back to the line's default (heading/body) colour. */
constexpr uint32_t NOCOL = 0xFFFFFFFFu;

struct Seg {
    SegKind kind; std::string text; std::string target; std::string fname, fvalue;
    uint32_t fg = NOCOL, bg = NOCOL;   /* NSDMI: brace-inits may omit these */
};

/* Inline style state. Micron colours persist across lines until reset, so
 * renderMicron owns one of these for the whole page and threads it through. */
struct MStyle { uint32_t fg = NOCOL, bg = NOCOL; };

std::vector<Seg> scanInline(const std::string& line, MStyle& st) {
    std::vector<Seg> segs;
    std::string cur;
    auto flush = [&]() {   /* push under the style active while `cur` accumulated */
        if (!cur.empty()) { segs.push_back({ SEG_TEXT, cur, "", "", "", st.fg, st.bg }); cur.clear(); }
    };
    auto hexv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (ch != '`') {
            /* Micron escape: backslash makes the next char literal (pages
             * write \[ or \` to dodge markup) — NomadNet renders it bare. */
            if (ch == '\\' && i + 1 < line.size()) {
                cur += line[i + 1];
                i++;
                continue;
            }
            /* Bare lxmf@<32hex> → tappable contact link (not mid-token). */
            std::string lxh;
            char prev = i ? line[i - 1] : 0;
            bool prevWord = (prev>='0'&&prev<='9')||(prev>='a'&&prev<='z')||(prev>='A'&&prev<='Z');
            if ((ch == 'l' || ch == 'L') && !prevWord && matchLxmfAt(line, i, lxh)) {
                flush();
                segs.push_back({ SEG_LXMF, line.substr(i, 5 + 32), lxh, "", "" });
                i += (5 + 32) - 1;
                continue;
            }
            cur += ch; continue;
        }
        if (i + 1 >= line.size()) break;
        char nx = line[i + 1];
        switch (nx) {
            case '`':                                           /* `` = reset all */
                flush(); st = MStyle{};
                i += 1; break;
            case '!': case '_': case '*':
            case 'c': case 'l': case 'r': case 'a':
                i += 1; break;                                  /* style/align — drop */
            case 'f': flush(); st.fg = NOCOL; i += 1; break;    /* reset fg */
            case 'b': flush(); st.bg = NOCOL; i += 1; break;    /* reset bg */
            case 'F': case 'B': {                               /* colour, 3 hex */
                int h0 = -1, h1 = -1, h2 = -1;
                if (i + 4 < line.size()) {
                    h0 = hexv(line[i + 2]); h1 = hexv(line[i + 3]); h2 = hexv(line[i + 4]);
                }
                flush();
                if (h0 >= 0 && h1 >= 0 && h2 >= 0) {            /* nibble-doubled, f00 → ff0000 */
                    uint32_t rgb = ((uint32_t)(h0 * 17) << 16) | ((uint32_t)(h1 * 17) << 8) | (uint32_t)(h2 * 17);
                    if (nx == 'F') st.fg = rgb; else st.bg = rgb;
                }
                i += 4; break;
            }
            case '[': {
                size_t end = line.find(']', i + 2);
                if (end == std::string::npos) { i += 1; break; }
                std::string inner = line.substr(i + 2, end - (i + 2));
                size_t bt = inner.find('`');
                std::string label  = bt == std::string::npos ? inner : inner.substr(0, bt);
                std::string target = bt == std::string::npos ? inner : inner.substr(bt + 1);
                flush();
                segs.push_back({ SEG_LINK, label.empty() ? target : label, target, "", "" });
                i = end;
                break;
            }
            case '<': {
                size_t end = line.find('>', i + 2);
                if (end == std::string::npos) { i += 1; break; }
                std::string spec = line.substr(i + 2, end - (i + 2));
                size_t bar = spec.find('|');
                std::string rest = bar == std::string::npos ? spec : spec.substr(bar + 1);
                size_t btv = rest.find('`');
                std::string name  = btv == std::string::npos ? rest : rest.substr(0, btv);
                std::string value = btv == std::string::npos ? ""   : rest.substr(btv + 1);
                flush();
                segs.push_back({ SEG_FIELD, "", "", name, value });
                i = end;
                break;
            }
            default: cur += nx; i += 1; break;                  /* unknown — keep char */
        }
    }
    flush();
    return segs;
}

void addLink(lv_obj_t* parent, const Seg& s) {
    lv_obj_t* l = mkLabel(parent, printable(s.text, true, kPageFont), lv_color_hex(0x6db3ff), kPageFont);
    lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, 6);
    size_t idx = s_linkTargets.size();
    s_linkTargets.push_back(s.target);
    lv_obj_add_event_cb(l, onLinkClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), l);
}

void onLxmfClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= s_lxmfTargets.size()) return;
    char v[48];                                /* nonce: repeat taps re-fire */
    snprintf(v, sizeof v, "%s:%u", s_lxmfTargets[idx].c_str(), (unsigned)lv_tick_get());
    storageSet("lxmf.url_lcd", v);
}

/* lxmf@<hash> link: like addLink but green and writes lxmf.url_lcd (LXMF
 * brings itself forward + opens the thread) instead of navigating a page. */
void addLxmf(lv_obj_t* parent, const Seg& s) {
    lv_obj_t* l = mkLabel(parent, printable(s.text, true, kPageFont), lv_color_hex(0x7fd0a0), kPageFont);
    lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(l, 6);
    size_t idx = s_lxmfTargets.size();
    s_lxmfTargets.push_back(s.target);
    lv_obj_add_event_cb(l, onLxmfClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), l);
}

void addField(lv_obj_t* parent, const Seg& s) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, s.fname.c_str());
    if (!s.fvalue.empty()) lv_textarea_set_text(ta, printable(s.fvalue, true, kPageFont).c_str());
    lv_obj_set_style_text_font(ta, kPageFont, 0);
    lv_obj_set_style_pad_ver(ta, 1, 0);
    lv_obj_set_style_pad_hor(ta, 4, 0);
    lv_obj_set_width(ta, lv_pct(60));
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
    s_fields.push_back({ s.fname, ta });
}

/* Render one source line. Plain paragraphs (no widgets, no colours) → one
 * wrapping, full-width label. fg-coloured paragraphs → a spangroup (spans
 * flow + wrap inline, which a flex row of labels can't). Lines with
 * links/fields/bg colours → a flex-wrap row of text + link labels + field
 * textareas (spans carry no bg and can't host widgets). */
void addLine(lv_obj_t* parent, const std::string& line, int hlevel, MStyle& st) {
    lv_color_t color = hlevel > 0 ? lv_color_white() : lv_color_hex(0xd0d4da);
    std::vector<Seg> segs = scanInline(line, st);

    /* End-of-line background paints the whole line out to the right edge of
     * the screen (NomadNet fills each row with the current bg), so tag the
     * full-width container with it below. NOCOL → no fill. */
    uint32_t eolBg = st.bg;
    auto fillBg = [&](lv_obj_t* o) {
        if (eolBg == NOCOL) return;
        lv_obj_set_style_bg_color(o, lv_color_hex(eolBg), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    };

    /* Merge adjacent plain-text segments with identical colours: cell-art pages
     * emit one segment per character cell, and each unmerged segment below
     * becomes an LVGL span/label. Merging same-style runs collapses the common
     * case (runs of same-coloured cells) by orders of magnitude. */
    {
        size_t w = 0;
        for (size_t r = 0; r < segs.size(); r++) {
            if (w > 0 && segs[r].kind == SEG_TEXT && segs[w - 1].kind == SEG_TEXT &&
                segs[r].fg == segs[w - 1].fg && segs[r].bg == segs[w - 1].bg) {
                segs[w - 1].text += segs[r].text;
            } else {
                if (w != r) segs[w] = std::move(segs[r]);
                w++;
            }
        }
        segs.resize(w);
    }

    bool hasWidget = false, hasFg = false, hasBg = false;
    for (auto& s : segs) {
        if (s.kind != SEG_TEXT) hasWidget = true;
        else {
            if (s.fg != NOCOL) hasFg = true;
            if (s.bg != NOCOL) hasBg = true;
        }
    }

    if (!hasWidget && !hasFg && !hasBg && eolBg == NOCOL) {
        std::string text;
        for (auto& s : segs) text += s.text;
        lv_obj_t* l = mkLabel(parent, printable(text, false, kPageFont), color, kPageFont);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        return;
    }

    if (!hasWidget && !hasBg) {        /* fg colours only: inline-flowing spans */
        lv_obj_t* sg = lv_spangroup_create(parent);
        /* A fixed width (here full-width) drives wrapping in LVGL 9.5;
         * lv_spangroup_set_mode() is deprecated and warns, so it's gone. */
        lv_obj_set_width(sg, lv_pct(100));
        lv_obj_set_style_text_font(sg, kPageFont, 0);
        lv_obj_set_style_text_color(sg, color, 0);
        lv_obj_set_style_text_line_space(sg, 0, 0);
        fillBg(sg);
        int spans = 0;
        for (auto& s : segs) {
            std::string txt = printable(s.text, false, kPageFont);
            if (txt.empty()) continue;
            lv_span_t* sp = lv_spangroup_add_span(sg);
            lv_span_set_text(sp, txt.c_str());
            if (s.fg != NOCOL)
                lv_style_set_text_color(lv_span_get_style(sp), lv_color_hex(s.fg));
            spans++;
        }
        if (!spans) lv_obj_delete(sg);   /* everything filtered → no empty stub */
        return;
    }

    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 0, 0);
    lv_obj_set_style_pad_row(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    fillBg(row);      /* eol bg fills the row's tail past the last segment */
    for (auto& s : segs) {
        if      (s.kind == SEG_LINK)  addLink(row, s);
        else if (s.kind == SEG_LXMF)  addLxmf(row, s);
        else if (s.kind == SEG_FIELD) addField(row, s);
        else {
            lv_obj_t* l = mkLabel(row, printable(s.text, true, kPageFont),
                                  s.fg != NOCOL ? lv_color_hex(s.fg) : color, kPageFont);
            if (l && s.bg != NOCOL) {
                lv_obj_set_style_bg_color(l, lv_color_hex(s.bg), 0);
                lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            }
        }
    }
}

void renderMicron(lv_obj_t* parent, const std::string& body) {
    int lineNo = 0;
    s_renderObjs = 0;
    bool literal = false;
    MStyle st;             /* inline colour state persists across lines */
    std::string lit;
    auto flushLit = [&]() {
        if (!lit.empty()) {
            lv_obj_t* l = mkLabel(parent, printable(lit, false, kPageFont), lv_color_hex(0xb8c0c8), kPageFont);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, lv_pct(100));
            lv_obj_set_style_bg_color(l, lv_color_hex(0x14181e), 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            lv_obj_set_style_pad_all(l, 4, 0);
            lit.clear();
        }
    };

    size_t pos = 0;
    while (pos <= body.size()) {
        ++lineNo;
        if (s_renderObjs > kRenderObjMax) {
            warn("page too complex (%d objects at line %d) — truncated\n",
                 s_renderObjs, lineNo);
            lv_obj_t* l = mkLabel(parent, "… page too complex — truncated …",
                                  lv_color_hex(0x808890), kPageFont);
            if (l) {
                lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(l, lv_pct(100));
            }
            break;
        }
        size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;

        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ' ')) trimmed.pop_back();
        if (trimmed == "`") { if (literal) { flushLit(); literal = false; } else literal = true; continue; }
        if (literal) { lit += line; lit += "\n"; continue; }

        if (line.empty()) {
            lv_obj_t* l = mkLabel(parent, " ", lv_color_hex(0x707880), kPageFont);
            if (st.bg != NOCOL) {   /* active bg bleeds through blank lines too */
                lv_obj_set_width(l, lv_pct(100));
                lv_obj_set_style_bg_color(l, lv_color_hex(st.bg), 0);
                lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            }
            continue;
        }
        if (line[0] == '#') continue;                                   /* comment */
        if (line.rfind("`=", 0) == 0) {                                 /* divider */
            lv_obj_t* d = lv_obj_create(parent);
            lv_obj_remove_style_all(d);
            lv_obj_set_width(d, lv_pct(100));
            lv_obj_set_height(d, 1);
            lv_obj_set_style_bg_color(d, lv_color_hex(0x3a4048), 0);
            lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            continue;
        }
        int hlevel = 0;
        std::string content = line;
        if (line[0] == '>') {
            while (hlevel < (int)line.size() && line[hlevel] == '>') hlevel++;
            content = line.substr(hlevel);
            if (!content.empty() && content[0] == ' ') content.erase(0, 1);
        }
        /* Headings latch the running style: colour changes *inside* a heading
         * line render but don't bleed into the body that follows. */
        MStyle saved = st;
        addLine(parent, content, hlevel, st);
        if (hlevel > 0) st = saved;
    }
    if (literal) flushLit();
}

/* ---- navigation between the two screens ---- */

void showList() {
    lcdProgramFullscreen(false);
    if (s_page) lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    if (s_list) { rebuildList(); lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN); }
}

void buildPageShell() {
    s_page = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* hdr = lv_obj_create(s_page);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), HDR_H);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x222b38), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);

    /* Left cluster — same three buttons as the web UI's address bar, same
     * order: ☰ site list, ⟳ reload, ‹ back (history; site list when empty). */
    lv_obj_t* menu = mkLabel(hdr, LV_SYMBOL_BARS, lv_color_white());
    lv_obj_align(menu, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(menu, 10);
    lv_obj_add_event_cb(menu, [](lv_event_t*) { showList(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* rl = mkLabel(hdr, LV_SYMBOL_REFRESH, lv_color_hex(0xc0c8d0));
    lv_obj_align(rl, LV_ALIGN_LEFT_MID, 30, 0);
    lv_obj_add_flag(rl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(rl, 10);
    lv_obj_add_event_cb(rl, [](lv_event_t*) {
        /* Mark the current page in history first: the firmware keeps the
         * cached copy until fresh bytes land, so ‹ returns to it even when
         * the reload can't connect. Unique value per press: a repeated
         * constant would be swallowed by the storage SET-dedup if a change
         * notify was ever dropped, leaving reload permanently dead. */
        pushHist();
        char v[20];
        snprintf(v, sizeof v, NSID "|%u", (unsigned)lv_tick_get());
        storageSet("nomad.cmd.reload", v);
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_hex(0xc0c8d0));
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 54, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_add_event_cb(back, [](lv_event_t*) { goBack(); }, LV_EVENT_CLICKED, nullptr);

    s_pageName = mkLabel(hdr, "", lv_color_white());
    lv_obj_align(s_pageName, LV_ALIGN_LEFT_MID, 78, 0);

    /* ★ bookmark toggle for the open page (host + path), left of the font
     * steppers; lit when the current page is bookmarked. */
    s_starBtn = mkLabel(hdr, SYMBOL_STAR, lv_color_hex(0x565c64));
    lv_obj_align(s_starBtn, LV_ALIGN_RIGHT_MID, -104, 0);
    lv_obj_add_flag(s_starBtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_starBtn, 12);
    lv_obj_add_event_cb(s_starBtn, [](lv_event_t*) {
        if (!isHash(s_curHash)) return;
        std::string path = s_curPath.empty() ? DEFAULT_PAGE : s_curPath;
        BmScan bm = bookmarkFor(s_curHash, path);
        if (!bm.id.empty()) {
            storageSet("nomad.cmd.bookmark.del", bm.id.c_str());
        } else {
            std::string add = s_curHash + ":" + path + "|" + nodeName(s_curHash);
            storageSet("nomad.cmd.bookmark.add", add.c_str());
        }
    }, LV_EVENT_CLICKED, nullptr);

    /* Page-font stepper: − smaller / + larger (Montserrat 14 — easier to
     * hit), persisted in s.nomad.page_font; user_data is the step. The end
     * of the ladder greys out (updateHdrButtons). */
    auto fontStep = [](lv_event_t* e) {
        int idx = pageFontIdx() + (int)(intptr_t)lv_event_get_user_data(e);
        if (idx < 0) idx = 0;
        if (idx >= kPageFontN) idx = kPageFontN - 1;
        storageSet("s.nomad.page_font", idx);
        rebuildPage();
    };
    s_fontMinus = mkLabel(hdr, LV_SYMBOL_MINUS, lv_color_hex(0xc0c8d0), lcdFont(LcdFace::SYMBOLS, 16));
    lv_obj_align(s_fontMinus, LV_ALIGN_RIGHT_MID, -45, 0);
    lv_obj_add_flag(s_fontMinus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_fontMinus, 12);
    lv_obj_add_event_cb(s_fontMinus, fontStep, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
    s_fontPlus = mkLabel(hdr, LV_SYMBOL_PLUS, lv_color_hex(0xc0c8d0), lcdFont(LcdFace::SYMBOLS, 16));
    lv_obj_align(s_fontPlus, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(s_fontPlus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_fontPlus, 12);
    lv_obj_add_event_cb(s_fontPlus, fontStep, LV_EVENT_CLICKED, (void*)(intptr_t)+1);

    s_pageBody = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_pageBody);
    lv_obj_set_width(s_pageBody, lv_pct(100));
    lv_obj_set_flex_grow(s_pageBody, 1);
    lv_obj_set_flex_flow(s_pageBody, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_pageBody, 6, 0);
    lv_obj_set_style_pad_ver(s_pageBody, 2, 0);
    lv_obj_set_style_pad_row(s_pageBody, 1, 0);
    /* Scrolling brings the status hover back for another 2 s. */
    lv_obj_add_event_cb(s_pageBody, [](lv_event_t*) { statusShow(!s_statusSticky); },
                        LV_EVENT_SCROLL, nullptr);

    /* Status hover: floats over the page's top-right corner (out of the
     * flex flow), black on #ffc, 2 px margin + padding; statusShow() runs
     * its lifecycle. Created last so it composites above the body. */
    s_status = lv_label_create(s_page);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(s_status, kFont, 0);
    lv_obj_set_style_text_color(s_status, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_status, lv_color_hex(0xFFFFCC), 0);
    lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_status, 4, 0);
    lv_obj_set_style_pad_all(s_status, 2, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_RIGHT, -2, HDR_H + 2);
    lv_label_set_text(s_status, "");
}

void rebuildPage() {
    if (!s_pageBody) return;
    refreshFonts();
    lv_obj_clean(s_pageBody);
    s_linkTargets.clear();
    s_lxmfTargets.clear();
    s_fields.clear();

    std::string status = storageGetStr(NKEY("nav.status"), "");
    std::string pageHash = storageGetStr(NKEY("page.hash"), "");
    bool current = (pageHash == s_curHash);

    updateHdrButtons();
    if (status == "failed" || status == "timeout") {
        if (s_status) lv_label_set_text(s_status, "Failed");
        s_statusSticky = true;
        statusShow(false);
        mkLabel(s_pageBody, "Could not load the page.", lv_color_hex(0xe08a8a));
        return;
    }
    if (status != "done" || !current) {
        const char* m = status == "establishing" ? "Establishing link..."
                      : status == "requesting"    ? "Requesting page..."
                      : status == "path_requested"? "Requesting path..."
                      : "Loading...";
        if (s_status) lv_label_set_text(s_status, m);
        s_statusSticky = true;
        statusShow(false);
        return;
    }

    int sz = storageGetInt(NKEY("page.size"), 0);
    int truncated = storageGetInt(NKEY("page.truncated"), 0);
    char st[48];
    snprintf(st, sizeof st, "%s  %dB", storageGetStr(NKEY("page.path"), "").c_str(), sz);
    if (s_status) lv_label_set_text(s_status, st);
    s_statusSticky = false;
    statusShow(true);                 /* loaded — linger 2 s, back on scroll */

    if (truncated) {
        mkLabel(s_pageBody, "Page too large to display on device.", lv_color_hex(0x8a93a0));
        return;
    }
    std::string body = storageGetStr(NKEY("page.body"), "");
    if (body.empty()) { mkLabel(s_pageBody, "(empty page)", lv_color_hex(0x707880)); return; }
    renderMicron(s_pageBody, body);
    lv_obj_scroll_to_y(s_pageBody, 0, LV_ANIM_OFF);
}

void openPage(const std::string& hash, const std::string& path = "") {
    if (!s_page) buildPageShell();
    if (s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(true);
    lv_label_set_text(s_pageName, nodeName(hash).c_str());
    s_hist.clear();                       /* fresh context from the site list */
    navigate(hash, path.empty() ? DEFAULT_PAGE : path, /*push=*/false);
    rebuildPage();
}

/* An LXMF message's Nomad link was tapped (lxmf_lcd wrote nomad.url_lcd).
 * Runs on the lcd task (subscribed via lcdRun in NomadApp::appInit), so LVGL is
 * safe here. Bring the Nomad browser forward and open the page. Mirrors
 * lxmf_lcd's onLcdOpenUrl (the reverse direction). */
void onLcdOpenPage(const char* /*key*/, const char* val) {
    if (!val || !*val) return;                     /* a clear */
    std::string s(val);
    size_t bar = s.rfind('|');                     /* "<hash>:<path>|<nonce>" */
    if (bar != std::string::npos) s.erase(bar);
    size_t colon = s.find(':');
    std::string hash = colon == std::string::npos ? s : s.substr(0, colon);
    std::string path = colon == std::string::npos ? std::string() : s.substr(colon + 1);
    if (!isHash(hash)) return;
    for (auto& c : hash) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    lcdShowProgram("Nomad");                        /* build (first open) + raise */
    openPage(hash, path);                           /* navigate this session */
}

/* ---- list screen ---- */

struct Row { std::string id, hash, path, name, sub; };

void onListRowClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_rowTargets.size())
        openPage(s_rowTargets[idx].first, s_rowTargets[idx].second);
}

void addSectionHeader(const std::string& title) {
    lv_obj_t* l = mkLabel(s_list, title, lv_color_hex(0x8a93a0));
    lv_obj_set_style_pad_top(l, 4, 0);
}

/* "42s" / "3m" / "2h" / "5d" since `last_s` (unix). "" when unknown. */
std::string ageStr(int last_s) {
    if (last_s <= 0) return "";
    int s = (int)time(nullptr) - last_s;
    char b[16];
    if      (s < 0)      return "";
    else if (s < 60)     snprintf(b, sizeof b, "%ds", s);
    else if (s < 3600)   snprintf(b, sizeof b, "%dm", s / 60);
    else if (s < 86400)  snprintf(b, sizeof b, "%dh", s / 3600);
    else                 snprintf(b, sizeof b, "%dd", s / 86400);
    return b;
}

/* "<hops> hops · <age>" from the announce-drift entry; "" if never heard. */
std::string nodeSeenStr(const std::string& hash) {
    char k[64];
    snprintf(k, sizeof k, "nomad.nodes.%s", hash.c_str());
    std::string v = storageGetStr(k, "");
    if (v.empty()) return "";
    /* "<last_s>|<hops>|<name>" */
    size_t p1 = v.find('|');
    if (p1 == std::string::npos) return "";
    int last_s = atoi(v.c_str());
    int hops   = atoi(v.c_str() + p1 + 1);
    std::string age = ageStr(last_s);
    if (age.empty()) return "";
    char b[32];
    snprintf(b, sizeof b, "%d hops · %s", hops, age.c_str());
    return b;
}

void addRow(const Row& r) {
    size_t idx = s_rowTargets.size();
    s_rowTargets.push_back({ r.hash, r.path });

    lv_obj_t* row = lv_button_create(s_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x20262e), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 1, 0);
    lv_obj_set_style_pad_hor(row, 6, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, onListRowClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), row);

    mkLabel(row, r.name, lv_color_white());
    /* The sub line is always present (empty when unknown) so every row —
     * bookmark or announce — is the same height. */
    lv_obj_t* sub = mkLabel(row, r.sub.empty() ? " " : r.sub, lv_color_hex(0x8a93a0));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, lv_pct(100));
    lv_obj_set_height(sub, lv_font_get_line_height(kFont));
}

struct NodeRow { std::string hash, name; int hops, lastSeen; };
std::vector<NodeRow>* s_collectNodes = nullptr;

void collectNode(const char* key, const char* val) {
    if (!s_collectNodes || !key || !val) return;
    const char* tail = key + sizeof("nomad.nodes.") - 1;
    if (strchr(tail, '.')) return;
    /* "<last_s>|<hops>|<name>" */
    const char* p1 = strchr(val, '|'); if (!p1) return;
    const char* p2 = strchr(p1 + 1, '|'); if (!p2) return;
    s_collectNodes->push_back({ tail, std::string(p2 + 1),
                                atoi(p1 + 1), atoi(val) });
}

std::vector<Row>* s_collectBms = nullptr;
void collectBookmark(const char* key, const char* val) {
    if (!s_collectBms || !key) return;
    const char* tail = key + sizeof("s.nomad.bookmarks.") - 1;
    if (strchr(tail, '.')) return;
    /* "<hash>[:<path>]|<name>|<note>" — id-keyed; note last (may hold '|') */
    std::string v = val ? val : "";
    size_t p1 = v.find('|');
    if (p1 == std::string::npos) return;
    std::string url = v.substr(0, p1);
    size_t colon = url.find(':');
    std::string hash = colon == std::string::npos ? url : url.substr(0, colon);
    std::string path = colon == std::string::npos ? "" : url.substr(colon + 1);
    if (!isHash(hash)) return;
    std::string rest = v.substr(p1 + 1);
    size_t p2 = rest.find('|');
    std::string name = (p2 == std::string::npos) ? rest : rest.substr(0, p2);
    std::string note = (p2 == std::string::npos) ? ""   : rest.substr(p2 + 1);
    s_collectBms->push_back({ tail, hash, path,
                              name.empty() ? hash.substr(0, 8) + "..." : printable(name, true),
                              printable(note, true) });
}

void rebuildList() {
    if (!s_list) return;
    refreshFonts();
    s_listDirty = false;
    int scrollY = lv_obj_get_scroll_y(s_list);   /* hold the reading position */
    lv_obj_clean(s_list);
    s_rowTargets.clear();

    addSectionHeader("Bookmarks");
    std::vector<Row> bms;
    s_collectBms = &bms;
    storageForEach("s.nomad.bookmarks.", collectBookmark);
    s_collectBms = nullptr;
    if (bms.empty()) mkLabel(s_list, "  none", lv_color_hex(0x707880));
    for (auto& b : bms) {
        /* sub = note + when the node was last heard (blank until announced) */
        std::string seen = nodeSeenStr(b.hash);
        std::string sub = b.sub.empty() ? seen
                        : (seen.empty() ? b.sub : b.sub + " · " + seen);
        addRow({ b.id, b.hash, b.path, b.name, sub });
    }

    addSectionHeader("On the Mesh");
    std::vector<NodeRow> nodes;
    s_collectNodes = &nodes;
    storageForEach("nomad.nodes.", collectNode);
    s_collectNodes = nullptr;
    /* newest first */
    for (size_t a = 0; a < nodes.size(); a++)
        for (size_t b = a + 1; b < nodes.size(); b++)
            if (nodes[b].lastSeen > nodes[a].lastSeen) std::swap(nodes[a], nodes[b]);
    int shown = 0;
    for (auto& n : nodes) {
        /* Bookmarked nodes live in the section above — don't repeat them. */
        bool bookmarked = false;
        for (auto& b : bms) if (b.hash == n.hash) { bookmarked = true; break; }
        if (bookmarked) continue;
        char sub[48];
        snprintf(sub, sizeof sub, "%d hops · %s", n.hops, ageStr(n.lastSeen).c_str());
        addRow({ "", n.hash, "", n.name.empty() ? n.hash.substr(0, 8) + "..." : n.name, sub });
        shown++;
    }
    if (!shown) mkLabel(s_list, "  none heard yet", lv_color_hex(0x707880));

    if (scrollY > 0) {
        s_listRestoring = true;
        lv_obj_scroll_to_y(s_list, scrollY, LV_ANIM_OFF);
        s_listRestoring = false;
    }
}

/* ---- live updates ---- */

/* Rebuild only the screen the changed key actually feeds. We subscribe to the
 * whole "nomad." scope, which also carries nomad.nodes.* announce-drift churn
 * (and our own nomad.cmd.* sentinels). Doing a full rebuildPage() per announce
 * while a large page is on screen makes the lcd task render slower than
 * announces arrive, so it stops draining its inbox and storage floods
 * "notify drop … → [lcd]" — the same trap maps.cpp hit with the gps scope.
 * So: the page screen reads nomad.page.* / nomad.nav.* only; the list screen
 * reads nomad.nodes.* + s.nomad.bookmarks.*; everything else is ignored. */
/* Deferred list refresh: fires once the operator has left the list alone
 * for LIST_HOLD_MS; re-arms itself while they keep interacting. */
void listTimerCb(lv_timer_t* t) {
    lv_timer_pause(t);
    if (!s_listDirty || !s_list || !lv_obj_is_valid(s_list)) return;
    uint32_t since = lv_tick_elaps(s_listTouchTick);
    if (since < LIST_HOLD_MS) {
        lv_timer_set_period(t, LIST_HOLD_MS - since + 50);
        lv_timer_reset(t);
        lv_timer_resume(t);
        return;
    }
    if (!lv_obj_has_flag(s_list, LV_OBJ_FLAG_HIDDEN)) rebuildList();
    else s_listDirty = false;          /* hidden — rebuilt on next showList */
}

void markListDirty() {
    s_listDirty = true;
    if (!s_listTimer) return;
    uint32_t since = lv_tick_elaps(s_listTouchTick);
    lv_timer_set_period(s_listTimer, since < LIST_HOLD_MS ? LIST_HOLD_MS - since + 50 : 50);
    lv_timer_reset(s_listTimer);
    lv_timer_resume(s_listTimer);
}

void onListTouched(lv_event_t*) {
    if (!s_listRestoring) s_listTouchTick = lv_tick_get();
}

void onStorageChange(const char* key, const char*) {
    if (!s_layer || !key) return;
    bool pageOpen = s_page && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    if (pageOpen) {
        /* Only OUR session's nav/page keys — web-tab sessions (s0-s5) churn
         * in parallel and must not trigger LCD rebuilds. */
        if (!strncmp(key, NKEY(""), sizeof(NKEY("")) - 1))
            rebuildPage();
        else if (!strncmp(key, "s.nomad.bookmarks", 17))
            updateHdrButtons();           /* star reflects the toggle */
    } else {
        if (!strncmp(key, "nomad.nodes.", 12) || !strncmp(key, "s.nomad.bookmarks", 17)) {
            /* Announce churn must not reorder the list under the operator's
             * finger: within the hold window just mark it dirty; the timer
             * rebuilds once the interaction has been quiet. */
            if (lv_tick_elaps(s_listTouchTick) < LIST_HOLD_MS) markListDirty();
            else rebuildList();
        }
    }
}

/* ---- entry point (lcd task, on first open / relaid layer) ---- */

void nomadApp(void* arg) {
    refreshFonts();   /* vector chrome/page fonts live before any label is built */
    s_layer = static_cast<lv_obj_t*>(arg);
    s_page = nullptr; s_pageBody = nullptr; s_pageName = nullptr; s_status = nullptr;
    s_starBtn = nullptr; s_fontMinus = nullptr; s_fontPlus = nullptr;
    s_curHash.clear();
    s_linkTargets.clear();
    s_lxmfTargets.clear();
    s_rowTargets.clear();
    s_fields.clear();

    s_list = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_list, 4, 0);
    lv_obj_set_style_pad_ver(s_list, 1, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);

    /* Track interaction (touch + scroll) for the announce-churn hold. */
    lv_obj_add_event_cb(s_list, onListTouched, LV_EVENT_PRESSED,  nullptr);
    lv_obj_add_event_cb(s_list, onListTouched, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_list, onListTouched, LV_EVENT_SCROLL,   nullptr);
    if (!s_listTimer) {
        s_listTimer = lv_timer_create(listTimerCb, LIST_HOLD_MS, nullptr);
        lv_timer_pause(s_listTimer);
    }
    if (!s_statusTimer) {
        s_statusTimer = lv_timer_create(statusTimerCb, STATUS_HIDE_MS, nullptr);
        lv_timer_pause(s_statusTimer);
    }
    s_hist.clear();
    s_curPath.clear();

    rebuildList();

    if (!s_subscribed) {
        /* OUR session + the node drift only — never "nomad." wholesale: the
         * web sessions (s0-s5) churn page bodies in parallel and every one
         * of those notifies would wake (and at body sizes, overflow) the
         * lcd task's inbox for keys we'd filter out anyway. */
        storageSubscribeChanges("nomad.s" NSID ".", onStorageChange);
        storageSubscribeChanges("nomad.nodes.", onStorageChange);
        storageSubscribeChanges("s.nomad.bookmarks", onStorageChange);
        s_subscribed = true;
    }
}

/* ---- Settings → Mesh Network → Nomad Network ---- */

/* Keyboard-path "Add bookmark" fields; each nulls its own global on delete. */
lv_obj_t* s_bmHash = nullptr;
lv_obj_t* s_bmName = nullptr;
lv_obj_t* s_bmNote = nullptr;

void bmFieldDelete(lv_event_t* e) {
    auto** slot = static_cast<lv_obj_t**>(lv_event_get_user_data(e));
    *slot = nullptr;
}

void onAddBookmark(lv_event_t*) {
    if (!s_bmHash) return;
    const char* h = lv_textarea_get_text(s_bmHash);
    if (!h || !*h) return;
    const char* nm = s_bmName ? lv_textarea_get_text(s_bmName) : "";
    const char* nt = s_bmNote ? lv_textarea_get_text(s_bmNote) : "";
    std::string payload = std::string(h) + "|" + (nm ? nm : "") + "|" + (nt ? nt : "");
    storageSet("nomad.cmd.bookmark.add", payload.c_str());
    lv_textarea_set_text(s_bmHash, "");
    if (s_bmName) lv_textarea_set_text(s_bmName, "");
    if (s_bmNote) lv_textarea_set_text(s_bmNote, "");
}

/* Remove sentinel = the bookmark hash; user_data is a malloc'd copy freed on
 * the button's DELETE so it outlives the build but never leaks. */
void onRemoveBookmark(lv_event_t* e) {
    auto* hash = static_cast<char*>(lv_event_get_user_data(e));
    if (hash && *hash) storageSet("nomad.cmd.bookmark.del", hash);
}
void onRemoveBookmarkDelete(lv_event_t* e) { free(lv_event_get_user_data(e)); }

lv_obj_t* bmField(lv_obj_t* parent, const char* ph, lv_obj_t** slot) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, ph);
    lv_obj_set_style_text_font(ta, kFont, 0);
    lv_obj_set_width(ta, lv_pct(100));
    if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), ta);
    *slot = ta;
    lv_obj_add_event_cb(ta, onAddBookmark, LV_EVENT_READY,  nullptr);   /* Enter commits */
    lv_obj_add_event_cb(ta, bmFieldDelete, LV_EVENT_DELETE, slot);
    return ta;
}

void nomadSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "Nomad Network");
    lcdSettingSlider (p, "Announce-drift cap (nodes)", "s.nomad.max_nodes", 32, 2048);
    lcdSettingCaption(p, "Heard nodes kept in the drift list; oldest evicted past the cap.");
    lcdSettingValue  (p, "Last page", NKEY("page.path"));
    lcdSettingValue  (p, "Status",    NKEY("nav.status"));

    lcdSettingSection(p, "Bookmarks");
    std::vector<Row> bms;
    s_collectBms = &bms;
    storageForEach("s.nomad.bookmarks.", collectBookmark);
    s_collectBms = nullptr;
    if (bms.empty()) lcdSettingCaption(p, "No bookmarks yet.");
    for (auto& b : bms) {
        lv_obj_t* row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(row, 4, 0);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* info = lv_obj_create(row);
        lv_obj_remove_style_all(info);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);
        mkLabel(info, b.name, lv_color_white());
        std::string url = b.hash + (b.path.empty() ? "" : ":" + b.path);
        lv_obj_t* hl = mkLabel(info, url, lv_color_hex(0x8a93a0));
        lv_label_set_long_mode(hl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hl, lv_pct(100));
        lv_obj_set_height(hl, lv_font_get_line_height(kFont));

        lv_obj_t* rm = lv_button_create(row);
        lv_obj_set_style_pad_ver(rm, 2, 0);
        lv_obj_set_style_pad_hor(rm, 8, 0);
        lv_obj_set_style_bg_color(rm, lv_color_hex(0x5a2a2a), 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), rm);
        lv_obj_t* rl = lv_label_create(rm);
        lv_obj_set_style_text_font(rl, kFont, 0);   /* symbol face is kFont's fallback */
        lv_label_set_text(rl, LV_SYMBOL_TRASH);
        lv_obj_center(rl);
        size_t hn = b.id.size() + 1;          /* del sentinel takes the id */
        char* h = static_cast<char*>(gp_alloc(hn));
        memcpy(h, b.id.c_str(), hn);
        lv_obj_add_event_cb(rm, onRemoveBookmark,       LV_EVENT_CLICKED, h);
        lv_obj_add_event_cb(rm, onRemoveBookmarkDelete, LV_EVENT_DELETE,  h);
    }

    lcdSettingSection(p, "Add bookmark");
    if (lcdHasKeyboard()) {
        lv_obj_t* col = lv_obj_create(p);
        lv_obj_remove_style_all(col);
        lv_obj_set_width(col, lv_pct(100));
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        bmField(col, "32-hex hash[:<path>]",     &s_bmHash);
        bmField(col, "name (optional)",         &s_bmName);
        bmField(col, "note (optional)",         &s_bmNote);

        lv_obj_t* add = lv_button_create(col);
        lv_obj_set_width(add, lv_pct(100));
        lv_obj_set_style_pad_ver(add, 2, 0);
        if (lcdInputGroup()) lv_group_add_obj(lcdInputGroup(), add);
        lv_obj_t* al = lv_label_create(add);
        lv_obj_set_style_text_font(al, kFont, 0);
        lv_label_set_text(al, "Add bookmark");
        lv_obj_center(al);
        lv_obj_add_event_cb(add, onAddBookmark, LV_EVENT_CLICKED, nullptr);
    } else {
        /* Touch-only: one field; type the pipe-delimited add sentinel directly. */
        lcdSettingText(p, "Add (hash|name|note)", "nomad.cmd.bookmark.add");
    }
}

}  // namespace

/* NomadApp — the Nomad-browser launcher program (declared in nomad_app.h,
 * global so the generated services: trampoline can `new` it; methods defined
 * here where the file-static browser state lives). onCreate builds the browser;
 * onClose nulls the widget handles so a storage change (the session
 * subscription outlives the layer) early-returns instead of touching freed
 * objects after eviction. The persistent list/status timers guard on these same
 * handles. */
NomadApp::NomadApp() : LcdApp({ .name = "Nomad", .iconBasename = "nomad" }) {}

void NomadApp::onCreate(lv_obj_t* root) { nomadApp(root); }

void NomadApp::onClose() {
    s_list = nullptr; s_page = nullptr; s_pageBody = nullptr; s_pageName = nullptr;
    s_status = nullptr; s_starBtn = nullptr; s_fontMinus = nullptr; s_fontPlus = nullptr;
}

/* NomadApp::appInit — the boot-task half of bring-up, run once by
 * LcdApp::onInit() right after it hops the launcher-tile install onto the lcd
 * task. This whole file lives under conditional/spangap-lcd/, compiled only when
 * the lcd straddle is staged, so no #if is needed — no lcd, no NomadApp, no
 * services: registration. */
void NomadApp::appInit() {
    lcdRegisterSettings("Mesh Network/Nomad", "Nomad Network", nomadSettingsPane, 3);

    /* An LXMF message's Nomad link (tapped in LXMessenger) writes nomad.url_lcd.
     * Subscribe ON the lcd task so the callback may touch LVGL directly — and
     * register here at init, not in nomadApp, so the trigger works even if Nomad
     * was never opened. */
    lcdRun([](void*) { storageSubscribeChanges("nomad.url_lcd", onLcdOpenPage); });
}
