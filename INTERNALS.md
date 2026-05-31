# nomad — internals

## Task

One FreeRTOS task on **core 1, prio 1, 8 KB PSRAM stack**. Same shape as
lxmf: storage-as-API, sentinel-cmd dispatch, no mR includes.

## Subscriptions

- The **`nomadnetwork.node` announce fan-out** through rnsd — every
  announce updates `nomad.nodes.<hash>.*` (last-seen, hop count,
  app-data).
- The storage sentinels:
  - `nomad.cmd.go <node_hash>:<path>` — navigate.
  - `nomad.cmd.refresh` — re-fetch current page.
  - `nomad.cmd.bookmark.add/del` — bookmark management.

## Fetch flow

1. Sentinel `nomad.cmd.go` arrives.
2. Task ensures path-table has a usable path to the destination
   (`rnsdRequestPath` if not).
3. Calls `rnsdLinkOpen(dest_hash)`.
4. On link-up, calls `rnsdLinkRequest(link, "/<path>")` — a Nomad page
   request is just a Resource request with a path string.
5. Reads the response bytes, stashes in `nomad.page.bytes` /
   `nomad.page.mime`, updates `nomad.nav.current`.
6. Closes the link (Nomad is one-shot per page in this client).

## Storage shape

```
nomad.nodes.<32hex-hash>.
    name              announced name
    hops              last hop count
    last_seen_ms
    app_data          base64 (or hex) of the announced app-data
nomad.nav.
    current           "<hash>:<path>"
    history.<i>       past navigation entries (ring buffer)
nomad.page.
    bytes             last fetched page body
    mime              "text/micron" typically
    fetched_at_ms
s.nomad.bookmarks.<n>.
    label
    target            "<hash>:<path>"
```

## LCD slice

`esp-idf/lcd/src/nomad_lcd.cpp` — C++ Micron-to-LVGL renderer. Picked
up by the `spangap-lcd` activator, registered as a launcher program.

## Activator caveat

Same as lxmf: the LCD slice is hard-coded into the firmware
`CMakeLists.txt` for now (activator-driven source-list exclusion is a
future feature). The slice body is wrapped in
`#if CONFIG_SPANGAP_LCD` until then.

## Status

The drift feed, navigation, and the basic request/response path are
working. The Micron renderer is feature-incomplete (covers headers,
emphasis, lists, and links; tables and dividers TBD).
