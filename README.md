# reticulous-nomad

## What is this?

**reticulous-nomad** is the [Nomad
Network](https://github.com/markqvist/NomadNet) **page client** on
[reticulous-core](../reticulous-core) — the "text web" half of Nomad
Network: nodes host pages (in Micron markup) and files on a
`nomadnetwork.node` destination, and this straddle is the
client/browser side. It sits on rnsd's byte-array API directly (not on
LXMF), using `rnsdLinkOpen` + `rnsdLinkRequest` for navigation, and
storage as the API.

## What this straddle owns

```
reticulous-nomad/
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
Micron→LVGL renderer, gated on `CONFIG_SPANGAP_LCD`.

## How others use it

```cpp
nomadInit();    // after rnsdInit
```

The API is storage. Frontends drive navigation by writing to the
`nomad.nav.*` and `nomad.cmd.*` sentinels; lxmf-style.

Key surfaces:

- `nomad.nodes.*` — the live drift feed (nodes seen via the
  `nomadnetwork.node` announce fan-out)
- `nomad.nav.*` — current navigation state (current page, history)
- `nomad.page.*` — last fetched page bytes (Micron source; renderers
  parse this)
- `s.nomad.bookmarks.*` — operator bookmarks

The nomad task **never parses Micron** — it is bytes in, bytes out. The
viewing endpoint (browser panel or LCD slice) renders.

## Dependencies

- [reticulous-core](../reticulous-core)

## What it does NOT do

- It is a **client only** — there is no `nomadnetwork.node` server side
  here.
- It does not render Micron — frontends do.

## Read next

- [INTERNALS.md](INTERNALS.md) — storage key shape, link / request
  flow, the announce fan-out subscription.
- Deep-dive in the consuming app:
  [docs/nomad.md](../reticulous-tdeck/docs/nomad.md) /
  [docs/plans/nomad.md](../reticulous-tdeck/docs/plans/nomad.md).
