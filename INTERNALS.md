# nomad — internals

## Task

One FreeRTOS task on **core 1, prio 1, 8 KB PSRAM stack**. Same shape as
lxmf: storage-as-API, sentinel-cmd dispatch, no mR includes.

## Sessions

Browsing is session-based: `NOMAD_SESSIONS` (7) parallel browser
contexts, each an independent NomadNet browser with its own Link, its
own in-flight request, and its own published nav/page state. Sessions
0–5 belong to the web UI (one per browser tab); session 6
(`NOMAD_LCD_SESSION`) to the on-device LCD browser. Per session the
NomadNet model holds: ONE Link to the current node, reused for every
request to that node, dropped on node change / failure / idle-STALE
close; one request in flight at a time (a new `go` while one is in
flight abandons it and re-opens — newest navigation wins). rnsd link
tags are `nomad<sid>`; the session id rides the ITS conn ref. The page
cache (16 entries / 512 KB PSRAM) is shared across sessions.

Link budget: nomad's share is `NOMAD_SESSIONS` = 7 of the device's 12
parallel links (6 web tabs + 1 LCD + 5 for other consumers — lxmf's
conversation links etc.; rnsd has 32 slots, so the budget is policy,
not a hard table size).

## Subscriptions

- The **`nomadnetwork.node` announce fan-out** through rnsd — every
  announce updates `nomad.nodes.<hash>` (last-seen, hop count, name).
- The storage sentinels (values carry an optional `<sid>|` prefix;
  bare = session 0):
  - `nomad.cmd.go = [<sid>|]<node_hash>[:<path>]` — navigate.
  - `nomad.cmd.reload = [<sid>|]<unique>` — re-fetch, bypassing the
    cache read; the cached copy survives until fresh bytes replace it
    (failed reload keeps the old page reachable). Unique value: a
    constant would be SET-deduped if the key was ever left set by a
    dropped notify.
  - `nomad.cmd.submit = [<sid>|]<hash>:<path>` — form submit; fields
    staged under `nomad.submit.<sid>.*`.
  - `nomad.cmd.bookmark.add/del` — bookmark management.
- `rnsd.links.nomad*` — each session Link's progress, reflected into
  that session's `nav.status`.

## Fetch flow (per session)

1. Sentinel `nomad.cmd.go` arrives; sid parsed from the value.
2. Cache check (`<hash>:<path>`, shared) — hit publishes instantly.
3. `rnsdLinkOpen` with tag `nomad<sid>` (or reuse the session's open
   link when it already points at this node).
4. `rnsdLinkRequest(tag, path)` — the response rides one
   `REQUEST_RESPONSE` aux frame back to NOMAD_RESP_PORT, correlated to
   the session by request id.
5. Response bytes → shared cache + `nomad.s<sid>.page.*`; status →
   `nomad.s<sid>.nav.status`.
6. The Link stays open for same-node reuse; it drops on node change,
   failure, or Reticulum's idle/STALE close.

## Storage shape

```
nomad.nodes.<32hex>          "<last_s>|<hops>|<name>"   announce drift (LRU)
nomad.s<sid>.nav.
    status            idle|path_requested|establishing|requesting|done|failed
    hash, path        current navigation target
    error             last error string ("" = none)
nomad.s<sid>.page.
    hash, path        what the body belongs to
    size, fetched_s
    body              Micron source (≤128 KB publish cap; larger pages
                      set truncated=1 and live only in the RAM cache)
    truncated
nomad.submit.<sid>.*  staged form fields (consumed by cmd.submit)
s.nomad.bookmarks.<id>       "<hash>[:<path>]|<name>|<note>"
s.nomad.max_nodes            announce-drift cap
s.nomad.page_font            LCD page-font index (see README)
```

## LCD slice

`esp-idf/conditional/spangap-lcd/src/nomad_lcd.cpp` — C++ Micron-to-LVGL
renderer bound to session 6. Picked up by the `spangap-lcd` activator,
registered as a launcher program.

## Activator caveat

Same as lxmf: the LCD slice lives under `esp-idf/conditional/spangap-lcd/`,
compiled only when `spangap-lcd` is staged, and registers via the
when:-gated `nomadLcdRegister` init: hook — no `#if` anywhere.
