# nomad — internals

Maintainer reference for the `nomad` page-client task and its two renderers.
The [README](README.md) is the operator guide; this document is for changing the
code without breaking it. It is self-authoritative.

## 1. What nomad implements

nomad is built entirely on top of [rns](../rns) — it has no upstream baseline of
its own; it **reimplements the NomadNet page/web client** against the NomadNet
wire contract (§7). Relative to a desktop NomadNet browser it provides:

- **The page-client task** (`esp-idf/src/nomad.cpp`) — storage-as-API, sentinel
  dispatch, a single `itsPoll` wait point, zero RNS includes. All transport goes
  through rnsd's byte-array API (`rnsdLinkOpen`, `rnsdLinkRequest`, the
  announced-nodes fan-out).
- **Session multiplexing** — `NOMAD_SESSIONS` (7) parallel browser contexts
  (§2), each an independent NomadNet browser with its own Link, in-flight
  request, and published nav/page state.
- **The announced-nodes feed** (§5) — an LRU of heard `nomadnetwork.node`
  announces.
- **A shared page cache** (§4) and **bookmarks** (§6).
- **Forms** — staged field/var values packed into a msgpack map and submitted
  as the request envelope's `data` element (§7.3).
- **Two independent renderers** — a TypeScript Micron→HTML renderer for the SPA
  (`browser/src/lib/micron.ts`) and a C++ Micron→LVGL renderer for the device
  (`esp-idf/conditional/spangap-lcd/src/nomad_lcd.cpp`). Neither lives in the
  task — it never parses Micron.

What nomad deliberately does **not** do: host pages (the `nomadnetwork.node`
server half), download files (`/file/…` Resources), in-page partials (`p:`), or
`rrc://` chat rooms. It browses with the rnsd node identity (`identity_key` is
empty → anonymous), so per-page `.allowed` ACLs and identity-scoped
`field_`/`var_` provenance are not exercised.

## 2. The task

One FreeRTOS task on **core 1, prio 1, 8 KB PSRAM stack** — the same class as
lxmf. It waits on the boot barrier (`waitForFlag("rns.ready", 120)`; if rns is
never ready it `killSelf`s — no rnsd, no point), then `waitForTime(0)` before
first contact so rnsd's ITS server surface is up and the clock is valid, opens
its aux server port, connects the announced-nodes subscription, and runs a
single `itsPoll` loop with a 1 Hz publish tick that reconnects the
announced-nodes subscription if it dropped.

### Sessions

A **session** is an independent NomadNet browser context, identified by its
`sid` (0..`NOMAD_SESSIONS`-1). Sessions 0–5 belong to the web UI (one per tab);
session 6 (`NOMAD_LCD_SESSION`) is the on-device LCD browser. `NOMAD_SESSIONS`
and `NOMAD_LCD_SESSION` live in `nomad.h` so the LCD slice and the SPA share
them. Per session the NomadNet model holds: **one** Link to the current node,
reused for every request to that node, dropped on node change / failure /
idle-STALE close; **one** request in flight at a time (a new `go` while one is in
flight abandons it and re-opens — newest navigation wins). rnsd link tags are
`nomad<sid>`; the session id rides the ITS conn ref (`itsRef` / the
disconnect-cb arg). The page cache is **shared** across sessions.

Link budget: nomad's share is `NOMAD_SESSIONS` = 7 of the device's ~12 parallel
links (6 web tabs + 1 LCD, leaving headroom for lxmf's conversation links etc.).
rnsd has 32 link slots, so this is policy, not a hard table size. The aux server
port opens with a single handle (`itsServerPortOpen(NOMAD_RESP_PORT, …,
maxHandles=1)`); the client side `itsClientInit`s with `NOMAD_SESSIONS + 5`
handles (the announce subscription plus the per-session links, with headroom so a
transient lingering conn never blocks a fetch).

## 3. ITS surface

- **`NOMAD_RESP_PORT` (130)** — nomad's own aux server port (`itsServerPortOpen`,
  not packet-based, one handle). rnsd delivers each page/form response here as
  one `rnsd_link_resource_done_t` frame: `RNSD_LINK_REQUEST_RESPONSE` carries
  the page (nomad owns `buf` and must `rnsdResourceRelease()` it), correlated to
  its session by matching `opaque_id` against the session's `req_id`;
  `RNSD_LINK_REQUEST_FAILED` fails the fetch. (File-download Resources —
  `RNSD_LINK_RESOURCE_INBOUND_DONE` — aren't handled.)
- **`RNSD_PORT_ANNOUNCES` (6)** — client connect with
  `rnsd_announces_connect_t` carrying the `nomadnetwork.node` aspect filter;
  rnsd delivers only matching announces as packet-mode frames
  `hops(1) | dest_hash(16) | identity_hash(16) | app_data(N)`.
- **`RNSD_PORT_LINK` (10)** — one outbound Link per session via `rnsdLinkOpen`
  (tag `nomad<sid>`, ref = sid). The packet handle is unused (responses ride the
  aux port) but rnsd opens it packet-mode, so `onFetchLinkRecv` drains it
  defensively.

## 4. Fetch flow & page cache

Per session, on `nomad.cmd.go`:

1. Parse the `sid` from the value (`parseSid`); split `<hash>[:<path>]`, empty
   path → `/page/index.mu`.
2. **Cache check** (`<hash>:<path>`, shared) — a hit publishes instantly and
   goes straight to `done` with zero air time. `reload` and form submits bypass
   the cache read.
3. **Reuse or open the Link.** If the session's open Link already points at this
   node, reuse it (no re-establish, no ITS-conn churn); otherwise drop a link to
   a different node and `rnsdLinkOpen` a fresh one (`s.nomad.link_timeout`
   overrides the establishment budget; `0` lets rnsd derive it).
4. `rnsdLinkRequest(tag, path[, packed])` — rnsd holds the request until the
   Link is ACTIVE, then issues it. GET = `nullptr/0`; a form passes the packed
   msgpack map with `data_packed=true`.
5. The response arrives on `NOMAD_RESP_PORT` → shared cache + `nomad.s<sid>.page.*`;
   status → `nomad.s<sid>.nav.status`.
6. On success the Link **stays open** for same-node reuse; on failure it is
   dropped so the next attempt re-establishes cleanly.

**nav.status reflection.** rnsd publishes each Link's progress to
`rnsd.links.nomad<sid>.*`; `onLinkState` reflects the `.state` leaf into that
session's `nav.status` (`awaiting_path`→`path_requested`, `establishing`→
`establishing`, `active`→`requesting`) so the frontend sees the Browser.py-style
progression without nomad re-deriving it. The terminal states (`done`/`failed`)
are owned by the aux handler — `onLinkState` must not clobber them (it checks
`active && !terminal`).

**Page cache.** LRU, `NOMAD_CACHE_MAX_ENTRIES` (16) / `NOMAD_CACHE_MAX_BYTES`
(512 KB PSRAM), oldest-`fetched_s` eviction, keyed `<hash>:<path>`, shared across
sessions. `cachePut` refreshes an entry in place; a `reload` leaves the entry
intact until fresh bytes arrive (so Back still shows the old page after a failed
reload). Form-submit responses are never cached.

**Publish cap.** `publishPage` always writes the metadata; the body is published
only if `len <= NOMAD_MAX_PAGE_PUBLISH` (128 KB compile-time — sized to clear
the storage DataChannel's 256 KB max-message after JSON escaping). Larger pages
set `page.truncated=1` and an empty body; the full bytes stay in the cache for
the LCD renderer. There is **no** `s.nomad.max_page_publish` key — the cap is
compile-time only.

## 5. Announced-nodes feed

`onAnnounceFromRnsd` receives an already-aspect-filtered announce; `app_data` is
the node's display name (UTF-8, may be empty). Each becomes a
`nomad.nodes.<dest_hex>` leaf, `"<last_s>|<hops>|<name>"` (name last, may contain
`|`). The feed is LRU-capped at `s.nomad.max_nodes` (default 256, `0` =
unbounded): when adding a *new* node would exceed the cap, the oldest-`last_s`
node is evicted (`nodeCountAndMaybeOldest` walks the tree). Names are sanitized
(C0/DEL → `.`) only when **logged**, never when stored.

## 6. Bookmarks

`s.nomad.bookmarks.<id>` = `"<hash>[:<path>]|<name>|<note>"`, persisted and
browser-synced — the only durable nomad state besides config. Bookmarks address
a host **and** a path (page bookmarks, not just nodes), so the key is an opaque
`<id>` (unix seconds, de-collided against existing keys) because the url can't be
a storage key (paths contain dots). The note is last and may contain `|`; the
url and name must not. Re-adding an existing url updates its name/note in place
(`bookmarkIdForUrl` scans for a url match). `bookmark.del` accepts either the
`<id>` or a `<hash>[:<path>]` url (≥32 chars → treated as a url and resolved to
its id). A bare host bookmark is normalised to `<hash>:/page/index.mu`.

**A bookmarked host is claimed in rnsd's directory** (`rnsdClaim`, consumer
`RNSD_CLAIM_NOMAD`, PERSIST, layer `DIR`), so its identity and route outrank the
announce traffic of a busy public network under eviction and opening a bookmark
is immediate rather than a path request away. The bookmark list is what bounds
the claim population, which is the condition that makes a long-lived claim
legitimate at all (see `rns/INTERNALS.md` §1.1.2). `DIR`, not `DIR_BLOB`: we
want to know who the node is, not to answer path requests on its behalf.
Add asserts the claim; delete releases it only once the last bookmark naming
that host is gone (one host may be bookmarked at several paths); boot re-walks
the whole list, because rnsd compiles claims into its persisted image but a
discarded image must cost only the head start, never the intent.

## 7. NomadNet wire contract

The authoritative contract is markqvist/NomadNet (**GPL-3.0** — read for the
contract, reimplement, never copy). Citations below name the upstream source
file that defines each piece.

### 7.1 Destination & announces
- One destination per node: `Destination(identity, IN, SINGLE, "nomadnetwork",
  "node")` — app `nomadnetwork`, aspect `node` (`Node.py`).
- The announce `app_data` is the node's **display name**, UTF-8 bytes (`Node.py`).

### 7.2 Pages & files (server side, for reference)
A node registers request handlers: pages at `/page/<rel>.mu` (default
`/page/index.mu`), files at `/file/<rel>` (auto-compressed, returned as a
Resource). A dynamic page is an executable whose stdout is the Micron response
and which receives `field_*`/`var_*` from the request `data` as environment
(`Node.py`). nomad does not host any of this.

### 7.3 Request / response (the Link layer)
Client flow (`Browser.py`): `request_path` → wait `has_path` →
`Identity.recall` → build OUT SINGLE destination → `Link` → wait ACTIVE →
`link.request(path, data, …)`. On the wire:

- Request = msgpack `[time, path_hash, data]`, `path_hash = truncated_hash(path)`
  (16 B). ≤ MDU → one packet; larger → a Resource.
- `data` is **nil** for a plain GET, or a **msgpack string→string map** of
  `field_<name>`/`var_<name>` for a form. nomad's `mpMapHeader`/`mpStr` build
  exactly that map (matching Python umsgpack on the node), and `data_packed=true`
  makes rnsd splice it verbatim as the envelope's 3rd element.
- Response = msgpack `[request_id, response_bytes]`; ≤ MDU → packet, larger →
  Resource.

**Form interop note.** A data-less GET packs an empty *bin* (`0xc4 0x00`), not
*nil*; NomadNet static handlers ignore request data so this interoperates. The
field/var map round-trip is verified client↔server in the abstract; against a
real desktop `nomadnet` it is the least-exercised path — if a node rejects the
empty bin, the fix is an empty-→-nil case in `Link::request`.

### 7.4 URL grammar (link targets in Micron)
From `Browser.py`. A target is `<url>` optionally followed by backtick-delimited
`field`/`var` segments:
- `<dest_hash_hex>:<path>` — hash is 32 hex chars; empty path → `/page/index.mu`;
  empty hash → current node.
- Trailing `` `var1=v1|var2=v2 `` → `var_<name>` request entries; field
  references → `field_<name>` form widgets, `*` = all fields.
- `@`-prefixes select destination type: `nnn@` (default page), `lxmf@` (compose
  a message), `rrc@` (chat room).
- `p:<id>` = in-page partial; `#anchor` = in-doc jump; `rrc://…` = chat. These
  are out of scope — both resolvers ignore `@`-scheme and `://` targets so such
  links don't misfire, **except** `lxmf@<32hex>`, which both renderers turn into
  a tappable contact link that hands the dest hash to LXMF (`lxmf.url_lcd` on the
  device).

### 7.5 Micron markup (the renderer contract)
Backtick `` ` `` is the control char (`MicronParser.py` is the grammar of record
for both `micron.ts` and `nomad_lcd.cpp`):
`` `_ `` underline · `` `! `` bold · `` `* `` italic · `` `F<rgb> ``/`` `f ``
foreground colour (3 hex nibbles, doubled to `#RRGGBB`) · `` `B<rgb> ``/`` `b ``
background · `` `` `` reset all · `` `c``/```l``/```r``/```a `` alignment ·
`` `[label`target] `` link (label optional → target is the label) ·
`` `<width|name`value> `` input field. Line-level: `>`/`>>`/`>>>` headings,
`` `= `` divider, a lone `` ` `` line toggles a literal/preformatted block, `#`
starts a comment line. A backslash makes the next char literal (pages write
`\[`/`` \` `` to dodge markup).

### 7.6 Compression
Reticulum/NomadNet compress Resource payloads with bz2 and every client speaks
it, so a page served as a Resource usually arrives compressed. This is handled
in rns's µR Resource layer (vendored bzip2) and is **transparent to nomad**,
which only ever sees decompressed bytes. Reticulum hashes/proofs the Resource
over the *uncompressed* data, so inbound decompresses before hashing — a
correctness detail that lives entirely in rns, not here.

## 8. Renderers

Both consume the same wire Micron; they diverge in layout and interaction by
design (a 320×240 trackball UI vs. a wide mouse/keyboard browser tab), so
pixel-parity is a non-goal.

- **`browser/src/lib/micron.ts`** — Micron→HTML. Every literal run is
  HTML-escaped and only a fixed whitelist of tags/style props is emitted; link
  targets ride a `data-mtarget` attribute (never a real `href`), so untrusted
  page bytes have no `javascript:`/`data:` URL surface — `NomadWindow.vue`
  decides what a target means.
- **`esp-idf/conditional/spangap-lcd/src/nomad_lcd.cpp`** — Micron→LVGL, bound to
  session 6. Chrome (header, list rows) uses Montserrat; the rendered page uses a
  fixed-width font ladder selected by `s.nomad.page_font` — the platform bitmap
  terminal fonts at the bottom (Micro 2×3, Tom Thumb 4×6, Spleen 5×8 — the
  default) and the vector mono sizes above (box-drawing/column graphics need a
  fixed-width cell; the bitmaps measure glyphs by table lookup, far cheaper on
  art pages). fg-only lines flow as spangroup spans (proper inline wrap); lines
  with bg colours or widgets render as a flex-wrap row of styled labels.
  Adjacent same-colour text segments are merged before emitting, and the whole
  page is capped at 600 LVGL objects (`kRenderObjMax`) with a truncation notice
  past it: a cell-art page colours every character cell, and an uncapped render
  builds thousands of labels whose flex re-layout effectively never completes —
  which, on the lcd task, once starved every same-core actor below it. The list screen holds its order under the
  operator's finger for `LIST_HOLD_MS` so announce churn doesn't reorder rows
  mid-scroll. The whole file is under `conditional/spangap-lcd/`, compiled only
  when that straddle is staged, and registers via the `when:`-gated
  `nomadLcdRegister` init hook — no `#if` anywhere.

## 9. Maintainer pitfalls

- **Reload sentinels need a unique value.** A constant re-write to
  `nomad.cmd.reload` (or `nomad.cmd.go`) is swallowed by the storage SET-dedup if
  a change notify was ever dropped, leaving reload permanently dead. Writers put
  a tick/time in the value.
- **The LCD subscribes to its own session only.** A full `rebuildPage()` per
  announce while a large page is on screen makes the lcd task render slower than
  announces arrive, flooding the storage notify queue. The page screen reads
  `nomad.s6.*` only; the list screen reads `nomad.nodes.*` + `s.nomad.bookmarks.*`
  — never the `nomad.` scope wholesale (web sessions 0–5 churn page bodies in
  parallel).
- **Drop our ITS conn before tearing down the rnsd Link.** `dropLink` disconnects
  nomad's handle first; closing the handle tears the Link down in rnsd and frees
  the slot + tag, so a same-tag reopen right after sees a clean slot (no stale
  DISCONNECT hitting a reused handle).
- **A response with no matching session is a stray** — release its buffer and
  drop it (an abandoned in-flight request from a superseded navigation).
- **`s.nomad.version` is a config-version gate** read in `nomadInit`
  (`NOMAD_VERSION`). It conflicts with the project's no-config-migrations policy
  and should be removed (default keys directly); it is **not** a feature.
- **`announce-drift` is a legacy in-house token still in the code** — the LCD
  settings label (`"Announce-drift cap (nodes)"`) and several comments. The
  correct term is **announced-nodes feed / cap**; the docs use it. The code
  strings are a follow-up cleanup.
