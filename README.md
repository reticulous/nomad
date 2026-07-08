# nomad — Nomad Network pages on the device

**nomad** is the [Nomad Network](https://github.com/markqvist/NomadNet)
**page client** on [rns](../rns) — the "text web" half of Nomad Network.
NomadNet nodes host **pages** (in Micron markup) and **files** on a
`nomadnetwork.node` destination; this straddle is the client that **browses**
them over Reticulum Links. It rides rnsd's byte-array API directly (not LXMF),
using `rnsdLinkOpen` + `rnsdLinkRequest` for navigation, and uses **storage as
its API**.

## Origins

Nomad Network (markqvist/NomadNet, **GPL-3.0**) fuses two layers: an LXMF
messaging client (reticulous already has that — see [lxmf](../lxmf)) and a
text web of hosted pages and files. nomad is **only** the page/web client half,
**independently reimplemented** in C++/TypeScript from the NomadNet wire
contract — not a port or copy of the GPL sources. The wire grammar (destination,
announces, request/response, URL targets, Micron markup) and how this client
maps onto it are in [INTERNALS.md](INTERNALS.md).

## What it does

The `nomad` task is pure transport + state — it **never parses Micron** (bytes
in, bytes out; the viewing endpoint renders) and holds **zero** RNS types,
everything going through rnsd's byte-array API exactly like lxmf. It:

- subscribes to rnsd's announced-nodes fan-out filtered to `nomadnetwork.node`,
  building an LRU **announced-nodes feed** of heard nodes (the display name
  comes free in the announce, zero fetches);
- **navigates** a page: `request_path` → Link → `link.request(path[, data])` →
  raw Micron bytes published back to the frontend;
- keeps a **page cache** so re-viewing a page costs zero air time;
- packs and submits **forms**;
- persists **bookmarks**.

Browsing is **session-based**: 7 parallel browser contexts, each holding its own
Link and nav/page state, so tabs load in parallel. Sessions 0–5 belong to the
web UI (one per browser tab); session 6 is the on-device LCD browser. A
**session** is identified by its `sid`; command values carry an optional
`<sid>|` prefix (a bare value acts on session 0).

It sits on top of rnsd and is driven entirely through the storage tree:

```
  rnsd announced-nodes (nomadnetwork.node)  ──▶  nomad.nodes.<hash>   (feed)
  nomad.cmd.go = "<sid>|<hash>:<path>"      ──▶  rnsdLinkOpen + rnsdLinkRequest
  page bytes                                ──▶  nomad.s<sid>.page.body  (SPA / LCD render)
```

A frontend never calls a custom RPC: it writes a `nomad.cmd.*` sentinel and
reads the published `nomad.*` state. nomad starts automatically when the
straddle is in the build (it brings up its task, the announced-nodes
subscription, and the LCD launcher tile when `spangap-lcd` is staged).

## ITS ports

nomad is both an rnsd ITS *client* (the announced-nodes feed + the per-session
Links) and an aux-only ITS *server* (one port that receives request responses).

| Port | Role | Name | Purpose |
|---|---|---|---|
| 130 | server (nomad opens) | `NOMAD_RESP_PORT` | request-response aux — rnsd delivers a page/form response here as one `RNSD_LINK_REQUEST_RESPONSE` frame, correlated to its session by request id |
| 6 | client → rnsd | `RNSD_PORT_ANNOUNCES` | announced-nodes fan-out, aspect-filtered to `nomadnetwork.node` |
| 10 | client → rnsd | `RNSD_PORT_LINK` | per-session outbound Link (`rnsdLinkOpen`, tag `nomad<sid>`) |

Exact frame layouts are in rnsd's [`ports.h`](../rns/esp-idf/include/ports.h);
the nomad-facing C API is [`nomad.h`](esp-idf/include/nomad.h).

## Storage variables — the API

Frontends drive nomad entirely through the config tree. `dest_hex` is the
16-byte destination hash as 32 hex chars.

### Settings (persistent, browser-synced — `s.nomad.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.nomad.max_nodes` | `256` | Announced-nodes feed LRU cap; `0` = unbounded (oldest-heard node evicted when full). |
| `s.nomad.page_font` | `2` | LCD page-font index, smallest first: `0` = Micro 2×3 (160 cols — a whole-page thumbnail), `1` = Tom Thumb 4×6 (80 cols), `2` = Spleen 5×8 (default), `3`–`7` = vector mono 10/12/14/16/20 px. Stepped by the LCD page header's −/+ buttons. |
| `s.nomad.link_timeout` | `0` | Seconds a fetch Link may sit establishing before it fails. `0` lets rnsd derive the budget from the next hop's interface speed. |
| `s.nomad.bookmarks.<id>` | — | A saved page: `<hash>[:<path>]\|<name>\|<note>` (addresses a host **and** a path). `<id>` is an opaque key (a url can't be a storage key — paths contain dots). Written via the bookmark sentinels, not by hand. |

### Runtime state & telemetry (ephemeral, RAM-only — lost on reboot)

| Key | Shape | Meaning |
|---|---|---|
| `nomad.nodes.<dest_hex>` | `<last_s>\|<hops>\|<name>` | One leaf per heard node (the announced-nodes feed). `<name>` is the announce display name verbatim (UTF-8, may be empty), last so it may contain `\|`. |
| `nomad.s<sid>.nav.status` | enum | Live navigation status: `idle` → `path_requested` → `establishing` → `requesting` → `done` / `failed`. |
| `nomad.s<sid>.nav.{hash,path,error}` | string | Current target + last error (`""` = none). |
| `nomad.s<sid>.page.{hash,path,size,fetched_s}` | — | Metadata of the last-fetched page on that session. |
| `nomad.s<sid>.page.body` | UTF-8 Micron | The page bytes (capped — see below). |
| `nomad.s<sid>.page.truncated` | `0`/`1` | `1` when the page was too large to publish; the body is empty and lives only in the device-side cache. |

The body is published to the config tree if it fits `NOMAD_MAX_PAGE_PUBLISH`
(128 KB, compile-time). It rides the normal storage DataChannel sync, so the
browser receives the **full** value (the 512 B change-notification cap applies
only to the in-device change *signal*, not the synced value). Larger pages set
`truncated=1` with an empty body; the full bytes always live in the task's
RAM/PSRAM cache, which the on-device LCD renderer reads directly, so the device
renders even oversized pages.

### Command sentinels (self-clearing)

Write the key; nomad acts on the change and `storageUnset`s it immediately.
Values are `|`-delimited where a field may itself contain spaces, and carry an
optional `<sid>|` prefix (bare = session 0).

| Key | Value | Action |
|---|---|---|
| `nomad.cmd.go` | `[<sid>\|]<hash>[:<path>]` | Navigate; empty/missing path → `/page/index.mu`. |
| `nomad.cmd.reload` | `[<sid>\|]<unique>` | Re-fetch the current page, bypassing the cache **read** (the cached copy survives until fresh bytes replace it, so a failed reload keeps the old page reachable). The value must be unique per write (a tick/time) — a constant would be SET-deduped if the key were ever left set by a dropped notify. |
| `nomad.cmd.submit` | `[<sid>\|]<hash>:<path>` | Submit the staged `nomad.submit.<sid>.*` fields as a form. |
| `nomad.cmd.bookmark.add` | `<hash>[:<path>]\|<name>\|<note>` | Add/update a bookmark (note is last and may contain `\|`; re-adding the same url updates it in place). |
| `nomad.cmd.bookmark.del` | `<id>` or `<hash>[:<path>]` | Remove a bookmark by id or by url. |

Form fields are staged under `nomad.submit.<sid>.<field_*\|var_*>` (the keys are
already the NomadNet map keys) before `nomad.cmd.submit` is written; nomad packs
them into a msgpack map, deletes the staging tree, and issues the request.
History/back is **frontend-owned** — the firmware keeps no nav history.

### Open-page triggers (from an LXMF message)

The reverse of nomad's clickable `lxmf@<hash>` links: [lxmf](../lxmf) turns a
`<32-hex hash>:/path` page URL quoted in a message into a tappable link.
Activating one writes the URL to one of two ephemeral sentinels, and the
matching Nomad frontend comes forward and navigates to it:

| Key | Written by | Reaction |
|---|---|---|
| `nomad.url_web` | the lxmf web messenger | the Nomad Browser window comes forward (`showNomad()`) and opens the page in a tab |
| `nomad.url_lcd` | the on-device LXMessenger | the on-device Nomad app comes forward (`lcdShowProgram("Nomad")`) and opens the page |

The value is `<hash>[:<path>]` (default `/page/index.mu`) suffixed `\|<nonce>`
so re-tapping the same link re-fires. Each UI surface reacts only to its own
key. No firmware task consumes these — the frontend that reacts drives the
fetch through the normal `nomad.cmd.go` path.

## CLI — `nomad`

```
nomad nodes                              heard nodes (announced-nodes feed): hash, hops, age, name
nomad go [<sid>|]<hash>[:<path>]         fetch a page (default /page/index.mu)
nomad reload                             re-fetch session 0, bypassing the cache
nomad bookmarks                          list bookmarks (first column = id)
nomad bookmark add <hash>[:<path>] <name>[ <note>]
nomad bookmark del <id | hash[:<path>]>
```

Sessions: `0`–`5` are web tabs, `6` is the LCD; a bare `go`/`reload` acts on
session 0. Each verb just writes the matching `nomad.cmd.*` sentinel, so the CLI
behaves identically to the SPA and LCD paths. Page bytes are logged as a
one-line preview on fetch; nav state lives in `nomad.s<sid>.nav.*`. Run any of
these on-device with `spangap cli "<command>"`.

A lower-level request smoke verb lives on rnsd: `rnsd creq <dest_hash> <path>`
issues a raw request/response over a Link and logs the response, independent of
the nomad task.

## Frontends

Two renderers, each native to its UI; both consume the same wire Micron. 320×240
and a browser tab render differently by design.

- **SPA** — [`browser/src/modules/nomad.ts`](browser/src/modules/nomad.ts)
  (Pinia store + per-session views/RPC over the storage tree), the **"Nomad
  Browser"** floating window
  ([`panels/NomadWindow.vue`](browser/src/panels/NomadWindow.vue)), the
  **Settings → Nomad** panel
  ([`panels/NomadPanel.vue`](browser/src/panels/NomadPanel.vue)), and a **Micron→HTML
  renderer** ([`lib/micron.ts`](browser/src/lib/micron.ts)) — wide layout,
  mouse-clickable links/forms, real text selection.
- **Device** (`spangap-lcd` staged) — the **"Nomad"** launcher app plus a
  **C++ Micron→LVGL renderer**
  ([`conditional/spangap-lcd/src/nomad_lcd.cpp`](esp-idf/conditional/spangap-lcd/src/nomad_lcd.cpp)):
  single column, hard-wrapped to the screen width, trackball/keyboard to step
  links and fill fields, bookmark/announced-node grouping. Inline Micron
  foreground/background colours render; bold/italic/underline style codes are
  dropped (headings are colour-emphasised, links are link-coloured + underlined).
  A **Settings → Mesh Network → Nomad** pane (registered via
  `lcdRegisterSettings`) carries the node-cap slider and add/remove bookmarks.

The Micron renderer is **reticulous-local**, not in `spangap-browser`: Micron is
a NomadNet wire format and the shared platform UI never renders it, so a parser
there would be a layering violation.

## Dependencies

- [rns](../rns) — the Reticulum stack; nomad rides rnsd's Link request/response
  byte-array API and announced-nodes fan-out.

## What it does NOT do

- It is a **client only** — there is no `nomadnetwork.node` server (page
  hosting) side here.
- It does **not** render Micron — the frontends do.
- No file download (`/file/…`), in-page partials (`p:`), or `rrc://` chat.

## Read next

- [INTERNALS.md](INTERNALS.md) — the task/threading model, sessions, the ITS
  framing, the fetch flow, the NomadNet wire contract (destination, announces,
  request/response, URL grammar, Micron markup), and maintainer pitfalls.
