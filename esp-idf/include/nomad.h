/**
 * nomad — Nomad Network page-client task.
 *
 * The "text web" layer of Nomad Network: nodes host **pages** (Micron
 * markup) and **files** on a `nomadnetwork.node` destination; clients
 * **browse** them over Reticulum Links via the request/response layer.
 * This task is the client/browser half (the node/server half is later).
 *
 * Pure transport + state, modelled on the lxmf task: **storage is the
 * API**, **zero mR includes** (everything rides rnsd's byte-array API —
 * rnsdLinkOpen + rnsdLinkRequest), a single `itsPoll` wait point, and cmd
 * sentinels drive behaviour. The task **never parses Micron** — bytes in,
 * bytes out; the viewing endpoint (SPA / LCD) renders.
 *
 * Browsing is SESSION-based: NOMAD_SESSIONS parallel browser contexts,
 * each holding (at most) one open Link and one in-flight request. Sessions
 * 0-5 belong to the web UI's tabs; session 6 (NOMAD_LCD_SESSION) to the
 * on-device LCD browser. Command values carry an optional "<sid>|" prefix
 * (bare values act on session 0 — CLI convenience).
 *
 * Frontends (browser, CLI, on-device UI) read/write these keys:
 *   - `nomad.nodes.<dest_hex>`   ephemeral — announce-drift feed
 *                                (`<last_s>|<hops>|<name>`), filtered to
 *                                the nomadnetwork.node aspect.
 *   - `s.nomad.bookmarks.<id>`   persistent — `<hash>[:<path>]|<name>|<note>`
 *                                (bookmarks address host AND path; <id> is
 *                                opaque — paths can't be storage keys).
 *   - `nomad.s<sid>.nav.*`       ephemeral — session navigation status.
 *   - `nomad.s<sid>.page.*`      ephemeral — session's fetched page
 *                                (hash/path/size/fetched_s/body/truncated;
 *                                the full bytes also live in the task's
 *                                shared RAM cache).
 *   - cmd sentinels: `nomad.cmd.go` (`[<sid>|]<hash>[:<path>]`),
 *                    `nomad.cmd.reload` (`[<sid>|]<unique>` — unique, e.g.
 *                    a tick count: a constant would be SET-deduped if the
 *                    key was ever left set by a dropped notify),
 *                    `nomad.cmd.submit` (`[<sid>|]<hash>:<path>`, fields
 *                    staged under `nomad.submit.<sid>.*`),
 *                    `nomad.cmd.bookmark.add` (`<hash>[:<path>]|<name>|<note>`,
 *                    re-add of the same url updates in place),
 *                    `nomad.cmd.bookmark.del` (`<id>` or `<hash>[:<path>]`).
 *
 * History/back is frontend-owned — the firmware is stateless re history.
 *
 * See docs/nomad.md.
 */
#pragma once

/** Parallel browser sessions: 0-5 = web-UI tabs, 6 = the LCD browser. */
#define NOMAD_SESSIONS    7
#define NOMAD_LCD_SESSION 6

/** Bring up the nomad task. Auto-init hook (see straddle.yaml, order 180) —
 *  run by the generated spangapInitStraddles() dispatcher after rnsd + the
 *  transports, so consumers wire nothing. */
void nomadInit(void);
