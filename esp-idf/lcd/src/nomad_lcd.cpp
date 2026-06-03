/**
 * nomad_lcd.cpp — on-device "Nomad" browser program (LVGL).
 *
 * The on-device half of the Nomad Network page browser (docs/plans/nomad.md
 * Phase 3). Two screens within the one program layer, mirroring the
 * LXMessenger model:
 *   - List:  bookmarks on top, announce-drift nodes below; tap one to open
 *            its index page.
 *   - Page:  a back chevron + node name + reload (top bar) over a scrollable
 *            rendered Micron view.
 *
 * The C++ Micron→LVGL renderer is independent of the SPA's TS renderer and
 * native to this UI: single column, hard-wrapped to the screen width, links
 * are focusable/clickable widgets the trackball steps through. It diverges
 * from the SPA in layout + interaction by design. v1 simplifications (LCD
 * only): inline fg/bg colors and bold/italic are dropped (headings are
 * colour-emphasised; links are link-coloured + underlined); input fields
 * render as placeholders (forms = Phase 4). Reimplemented from the Micron
 * grammar — NomadNet is GPL-3.0, not copied.
 *
 * Storage is the API (same keys the firmware nomad task + SPA use):
 *   nomad.nodes.<hex>        "<last_s>|<hops>|<name>"   announce drift
 *   s.nomad.bookmarks.<hex>  "<name>|<note>"
 *   nomad.nav.{status,hash,path}                        navigation state
 *   nomad.page.{body,size,truncated,hash,path}          current page
 *   nomad.cmd.go = "<hash>:<path>"                      navigate sentinel
 * Everything runs on the lcd task; storage subscriptions dispatch there, so
 * we touch LVGL straight from the change callback.
 */
#include "sdkconfig.h"

#if CONFIG_SPANGAP_LCD

#include "lcd.h"
#include "storage.h"
#include "compat.h"

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

/* Chrome (header symbols, list rows, status) uses Montserrat — it carries the
 * LVGL symbol glyphs and accented node names. The rendered Micron page itself is
 * terminal content: box-drawing + column-aligned graphics only line up under a
 * fixed-width font, so it uses Spleen (the same font as the on-device Log/CLI). */
const lv_font_t* const kFont     = &lv_font_montserrat_12_latin;
const lv_font_t* const kPageFont = &lv_font_spleen_5x8;
const int HDR_H = 20;
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

lv_obj_t* mkLabel(lv_obj_t* parent, const std::string& txt, lv_color_t color,
                  const lv_font_t* font = kFont) {
    lv_obj_t* l = lv_label_create(parent);
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
bool s_subscribed = false;

/* Link targets / row hashes referenced from LVGL click callbacks by index
 * (intptr_t user_data) into these — no per-widget heap, no dangling
 * pointers. Rebuilt with each screen. */
std::vector<std::string> s_linkTargets;
std::vector<std::string> s_rowHashes;

/* Form fields on the current page: name → its LVGL textarea. Read when a
 * form link is followed; cleared with the page. */
std::vector<std::pair<std::string, lv_obj_t*>> s_fields;

void rebuildList();
void rebuildPage();

/* ---- helpers ---- */

bool isHash(std::string_view s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    return true;
}

std::string nodeName(const std::string& hash) {
    char k[80];
    snprintf(k, sizeof k, "s.nomad.bookmarks.%s", hash.c_str());
    std::string v = storageGetStr(k, "");
    if (!v.empty()) {                       /* "<name>|<note>" */
        size_t bar = v.find('|');
        std::string nm = bar == std::string::npos ? v : v.substr(0, bar);
        if (!nm.empty()) return printable(nm, true);
    }
    snprintf(k, sizeof k, "nomad.nodes.%s", hash.c_str());
    v = storageGetStr(k, "");
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

/* Navigate: write the go sentinel; the firmware fetches and republishes
 * nomad.page.* / nomad.nav.*, which our storage subscription renders. */
void navigate(const std::string& hash, const std::string& path) {
    if (!isHash(hash)) return;
    s_curHash = hash;
    std::string p = path.empty() ? DEFAULT_PAGE : path;
    storageSet("nomad.cmd.go", (hash + ":" + p).c_str());
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

    std::string hash, path;
    if (!resolveUrl(url, hash, path)) return;

    if (fields.empty() && vars.empty()) { navigate(hash, path); return; }

    /* Form submit. */
    storageDeleteTree("nomad.submit");
    bool wantAll = std::find(fields.begin(), fields.end(), std::string("*")) != fields.end();
    for (auto& f : s_fields) {
        if (!wantAll && std::find(fields.begin(), fields.end(), f.first) == fields.end()) continue;
        const char* v = lv_textarea_get_text(f.second);
        char k[96]; snprintf(k, sizeof k, "nomad.submit.field_%s", f.first.c_str());
        storageSet(k, v ? v : "");
    }
    for (auto& v : vars) {
        char k[96]; snprintf(k, sizeof k, "nomad.submit.var_%s", v.first.c_str());
        storageSet(k, v.second.c_str());
    }
    s_curHash = hash;
    storageSet("nomad.cmd.submit", (hash + ":" + path).c_str());
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

enum SegKind { SEG_TEXT, SEG_LINK, SEG_FIELD };
struct Seg { SegKind kind; std::string text; std::string target; std::string fname, fvalue; };

std::vector<Seg> scanInline(const std::string& line) {
    std::vector<Seg> segs;
    std::string cur;
    auto flush = [&]() { if (!cur.empty()) { segs.push_back({ SEG_TEXT, cur, "", "", "" }); cur.clear(); } };
    for (size_t i = 0; i < line.size(); i++) {
        char ch = line[i];
        if (ch != '`') { cur += ch; continue; }
        if (i + 1 >= line.size()) break;
        char nx = line[i + 1];
        switch (nx) {
            case '`': case '!': case '_': case '*':
            case 'f': case 'b': case 'c': case 'l': case 'r': case 'a':
                i += 1; break;                                  /* style/align — drop */
            case 'F': case 'B': i += 4; break;                  /* color (3 hex) — drop */
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

/* Render one source line. Plain paragraphs (no widgets) → one wrapping,
 * full-width label. Lines with links/fields → a flex-wrap row of text +
 * link labels + field textareas. */
void addLine(lv_obj_t* parent, const std::string& line, int hlevel) {
    lv_color_t color = hlevel > 0 ? lv_color_white() : lv_color_hex(0xd0d4da);
    std::vector<Seg> segs = scanInline(line);

    bool hasWidget = false;
    for (auto& s : segs) if (s.kind != SEG_TEXT) { hasWidget = true; break; }

    if (!hasWidget) {
        std::string text;
        for (auto& s : segs) text += s.text;
        lv_obj_t* l = mkLabel(parent, printable(text, false, kPageFont),
                              hlevel > 0 ? lv_color_white() : color, kPageFont);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
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
    for (auto& s : segs) {
        if      (s.kind == SEG_LINK)  addLink(row, s);
        else if (s.kind == SEG_FIELD) addField(row, s);
        else                          mkLabel(row, printable(s.text, true, kPageFont), color, kPageFont);
    }
}

void renderMicron(lv_obj_t* parent, const std::string& body) {
    bool literal = false;
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
        size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = (nl == std::string::npos) ? body.size() + 1 : nl + 1;

        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == ' ')) trimmed.pop_back();
        if (trimmed == "`") { if (literal) { flushLit(); literal = false; } else literal = true; continue; }
        if (literal) { lit += line; lit += "\n"; continue; }

        if (line.empty()) { mkLabel(parent, " ", lv_color_hex(0x707880), kPageFont); continue; }
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
        addLine(parent, content, hlevel);
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

    lv_obj_t* back = mkLabel(hdr, LV_SYMBOL_LEFT, lv_color_white());
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 12);
    lv_obj_add_event_cb(back, [](lv_event_t*) { showList(); }, LV_EVENT_CLICKED, nullptr);

    s_pageName = mkLabel(hdr, "", lv_color_white());
    lv_obj_align(s_pageName, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t* rl = mkLabel(hdr, LV_SYMBOL_REFRESH, lv_color_hex(0xc0c8d0));
    lv_obj_align(rl, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_flag(rl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(rl, 12);
    lv_obj_add_event_cb(rl, [](lv_event_t*) { storageSet("nomad.cmd.reload", "1"); },
                        LV_EVENT_CLICKED, nullptr);

    s_status = mkLabel(s_page, "", lv_color_hex(0x8a93a0));
    lv_obj_set_style_pad_hor(s_status, 6, 0);

    s_pageBody = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_pageBody);
    lv_obj_set_width(s_pageBody, lv_pct(100));
    lv_obj_set_flex_grow(s_pageBody, 1);
    lv_obj_set_flex_flow(s_pageBody, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_pageBody, 6, 0);
    lv_obj_set_style_pad_ver(s_pageBody, 2, 0);
    lv_obj_set_style_pad_row(s_pageBody, 1, 0);
}

void rebuildPage() {
    if (!s_pageBody) return;
    lv_obj_clean(s_pageBody);
    s_linkTargets.clear();
    s_fields.clear();

    std::string status = storageGetStr("nomad.nav.status", "");
    std::string pageHash = storageGetStr("nomad.page.hash", "");
    bool current = (pageHash == s_curHash);

    if (status == "failed" || status == "timeout") {
        if (s_status) lv_label_set_text(s_status, "Failed");
        mkLabel(s_pageBody, "Could not load the page.", lv_color_hex(0xe08a8a));
        return;
    }
    if (status != "done" || !current) {
        const char* m = status == "establishing" ? "Establishing link..."
                      : status == "requesting"    ? "Requesting page..."
                      : status == "path_requested"? "Requesting path..."
                      : "Loading...";
        if (s_status) lv_label_set_text(s_status, m);
        return;
    }

    int sz = storageGetInt("nomad.page.size", 0);
    int truncated = storageGetInt("nomad.page.truncated", 0);
    char st[48];
    snprintf(st, sizeof st, "%s  %dB", storageGetStr("nomad.page.path", "").c_str(), sz);
    if (s_status) lv_label_set_text(s_status, st);

    if (truncated) {
        mkLabel(s_pageBody, "Page too large to display on device.", lv_color_hex(0x8a93a0));
        return;
    }
    std::string body = storageGetStr("nomad.page.body", "");
    if (body.empty()) { mkLabel(s_pageBody, "(empty page)", lv_color_hex(0x707880)); return; }
    renderMicron(s_pageBody, body);
    lv_obj_scroll_to_y(s_pageBody, 0, LV_ANIM_OFF);
}

void openPage(const std::string& hash) {
    if (!s_page) buildPageShell();
    if (s_list) lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lcdProgramFullscreen(true);
    lv_label_set_text(s_pageName, nodeName(hash).c_str());
    navigate(hash, DEFAULT_PAGE);
    rebuildPage();
}

/* ---- list screen ---- */

struct Row { std::string hash, name, sub; };

void onListRowClick(lv_event_t* e) {
    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (idx < s_rowHashes.size()) openPage(s_rowHashes[idx]);
}

void addSectionHeader(const std::string& title) {
    lv_obj_t* l = mkLabel(s_list, title, lv_color_hex(0x8a93a0));
    lv_obj_set_style_pad_top(l, 4, 0);
}

void addRow(const Row& r) {
    size_t idx = s_rowHashes.size();
    s_rowHashes.push_back(r.hash);

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
    if (!r.sub.empty()) {
        lv_obj_t* sub = mkLabel(row, r.sub, lv_color_hex(0x8a93a0));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_height(sub, lv_font_get_line_height(kFont));
    }
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
    std::string v = val ? val : "";
    size_t bar = v.find('|');
    std::string name = bar == std::string::npos ? v : v.substr(0, bar);
    std::string note = bar == std::string::npos ? "" : v.substr(bar + 1);
    s_collectBms->push_back({ tail, name.empty() ? std::string(tail).substr(0, 8) + "..."
                                                 : printable(name, true), printable(note, true) });
}

void rebuildList() {
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_rowHashes.clear();

    addSectionHeader("Bookmarks");
    std::vector<Row> bms;
    s_collectBms = &bms;
    storageForEach("s.nomad.bookmarks.", collectBookmark);
    s_collectBms = nullptr;
    if (bms.empty()) mkLabel(s_list, "  none", lv_color_hex(0x707880));
    for (auto& b : bms) addRow(b);

    addSectionHeader("On the Mesh");
    std::vector<NodeRow> nodes;
    s_collectNodes = &nodes;
    storageForEach("nomad.nodes.", collectNode);
    s_collectNodes = nullptr;
    /* newest first */
    for (size_t a = 0; a < nodes.size(); a++)
        for (size_t b = a + 1; b < nodes.size(); b++)
            if (nodes[b].lastSeen > nodes[a].lastSeen) std::swap(nodes[a], nodes[b]);
    if (nodes.empty()) mkLabel(s_list, "  none heard yet", lv_color_hex(0x707880));
    for (auto& n : nodes) {
        char sub[48];
        snprintf(sub, sizeof sub, "%d hops", n.hops);
        addRow({ n.hash, n.name.empty() ? n.hash.substr(0, 8) + "..." : n.name, sub });
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
void onStorageChange(const char* key, const char*) {
    if (!s_layer || !key) return;
    bool pageOpen = s_page && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    if (pageOpen) {
        if (!strncmp(key, "nomad.page.", 11) || !strncmp(key, "nomad.nav.", 10))
            rebuildPage();
    } else {
        if (!strncmp(key, "nomad.nodes.", 12) || !strncmp(key, "s.nomad.bookmarks", 17))
            rebuildList();
    }
}

/* ---- entry point (lcd task, on first open / relaid layer) ---- */

void nomadApp(void* arg) {
    s_layer = static_cast<lv_obj_t*>(arg);
    s_page = nullptr; s_pageBody = nullptr; s_pageName = nullptr; s_status = nullptr;
    s_curHash.clear();
    s_linkTargets.clear();
    s_rowHashes.clear();
    s_fields.clear();

    s_list = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_list, lv_color_hex(0x10141a), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(s_list, 4, 0);
    lv_obj_set_style_pad_ver(s_list, 1, 0);
    lv_obj_set_style_pad_row(s_list, 1, 0);

    rebuildList();

    if (!s_subscribed) {
        storageSubscribeChanges("nomad.", onStorageChange);              /* nodes/nav/page */
        storageSubscribeChanges("s.nomad.bookmarks", onStorageChange);
        s_subscribed = true;
    }
}

/* ---- Settings → Reticulum → Nomad ---- */

void nomadSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection(p, "Nomad Network");
    lcdSettingSlider (p, "Drift cap", "s.nomad.max_nodes", 32, 2048);
    lcdSettingValue  (p, "Last page", "nomad.page.path");
    lcdSettingValue  (p, "Status",    "nomad.nav.status");
}

}  // namespace

extern "C" void nomadLcdRegister(void) {
    lcdRegister("Nomad", "rns", nomadApp);
    lcdRegisterSettings("Reticulum/Nomad", "Nomad", nomadSettingsPane);
}

#else /* !CONFIG_SPANGAP_LCD */

/* No-op stub so nomadInit()'s unconditional nomadLcdRegister() links in non-LCD
 * (--no-lcd) builds, where the launcher program above compiles to nothing. */
extern "C" void nomadLcdRegister(void) {}

#endif /* CONFIG_SPANGAP_LCD */
