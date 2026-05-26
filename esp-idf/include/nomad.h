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
 * Frontends (browser, CLI, on-device UI) read/write these keys:
 *   - `nomad.nodes.<dest_hex>`   ephemeral — announce-drift feed
 *                                (`<last_s>|<hops>|<name>`), filtered to
 *                                the nomadnetwork.node aspect.
 *   - `s.nomad.bookmarks.<hex>`  persistent — `<name>|<note>`.
 *   - `nomad.nav.*`              ephemeral — current navigation status.
 *   - `nomad.page.*`             ephemeral — last-fetched page metadata
 *                                (hash/path/size/fetched_s). The page
 *                                bytes live in the task's RAM cache and
 *                                are logged on fetch (Phase 1 verification
 *                                surface; the SPA byte transport is Phase 2).
 *   - cmd sentinels: `nomad.cmd.go` (`<hash>[:<path>]`), `nomad.cmd.reload`,
 *                    `nomad.cmd.bookmark.add` (`<hash>|<name>|<note>`),
 *                    `nomad.cmd.bookmark.del` (`<hash>`).
 *
 * History/back is frontend-owned — the firmware is stateless re history.
 *
 * See docs/plans/nomad.md (Phase 1) and docs/nomad.md.
 */
#pragma once

/** Bring up the nomad task. Called from app_main between rnsdInit() and
 *  diptychPostAppInit(), like the other consumer tasks. */
void nomadInit(void);
