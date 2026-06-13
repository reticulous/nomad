# nomad

## What is this?

**nomad** is the [Nomad
Network](https://github.com/markqvist/NomadNet) **page client** on
[rns](../rns) — the "text web" half of Nomad
Network: nodes host pages (in Micron markup) and files on a
`nomadnetwork.node` destination, and this straddle is the
client/browser side. It sits on rnsd's byte-array API directly (not on
LXMF), using `rnsdLinkOpen` + `rnsdLinkRequest` for navigation, and
storage as the API.

## What this straddle owns

```
nomad/
├── esp-idf/
│   ├── include/nomad.h
│   ├── src/nomad.cpp         the nomad page-client task
│   └── lcd/src/nomad_lcd.cpp  on-device NomadBrowser program (slice)
└── browser/
    └── src/
        ├── modules/nomad.ts          Pinia + RPC
        ├── panels/NomadPanel.vue     Settings → Nomad
        └── panels/NomadWindow.vue    the on-screen Nomad browser
```

The LCD slice is the on-device Nomad browser program: a C++
Micron→LVGL renderer, compiled only when `spangap-lcd` is staged.
Inline micron colours (`` `Fxxx ``/`` `Bxxx ``) render on the device;
bold/italic style codes are still dropped.

## How others use it

```cpp
nomadInit();    // after rnsdInit
```

The API is storage. Frontends drive navigation by writing the
`nomad.cmd.*` sentinels; lxmf-style. Browsing is **session-based**:
7 parallel browser contexts (sessions 0–5 = web-UI tabs, 6 = the LCD
browser), each holding its own Link and nav/page state, so tabs load
in parallel. Command values carry a `<sid>|` prefix (bare = session 0).

Key surfaces:

- `nomad.nodes.*` — the live drift feed (nodes seen via the
  `nomadnetwork.node` announce fan-out)
- `nomad.s<sid>.nav.*` — a session's navigation state
- `nomad.s<sid>.page.*` — a session's fetched page bytes (Micron
  source; renderers parse this)
- `s.nomad.bookmarks.<id>` — operator bookmarks: `<hash>[:<path>]|<name>|<note>` (host AND path)
- `s.nomad.page_font` — LCD page-font index, smallest first (0 = Micro
  2×3, an unreadable whole-page layout thumbnail at 160 columns;
  1 = Tom Thumb 4×6, the default; 2 = Spleen 5×8); stepped by the −/+
  buttons at the right of the LCD page header, greyed at the ladder
  ends; a ★ bookmark toggle for the open page sits left of them (the
  left cluster is ☰ site list / ⟳ reload / ‹ back, mirroring the web
  UI's address bar)

Re-viewing a page is served from the RAM page cache (zero air time);
`nomad.cmd.reload` always refetches — it bypasses the cache read, but
the cached copy stays until fresh bytes replace it, so a failed reload
keeps the old page reachable (Back). Reload writers must put a unique
value in the sentinel (tick/time), not a constant — see the note atop
`nomad.cpp`.

The nomad task **never parses Micron** — it is bytes in, bytes out. The
viewing endpoint (browser panel or LCD slice) renders.

## Dependencies

- [rns](../rns)

## What it does NOT do

- It is a **client only** — there is no `nomadnetwork.node` server side
  here.
- It does not render Micron — frontends do.

## Read next

- [INTERNALS.md](INTERNALS.md) — storage key shape, link / request
  flow, the announce fan-out subscription.
- Deep-dive in the consuming app:
  [docs/nomad.md](../hw-tdeck/docs/nomad.md) /
  [docs/plans/nomad.md](../hw-tdeck/docs/plans/nomad.md).
