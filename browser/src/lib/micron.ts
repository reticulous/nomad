/**
 * Micron → HTML renderer (reticulous-local).
 *
 * Micron is Nomad Network's text-markup format. The firmware never parses
 * it (bytes in, bytes out); the viewing endpoint renders. This is the SPA
 * renderer — wide layout, mouse-clickable links, real text selection — as
 * opposed to the on-device C++ → LVGL renderer, which diverges in layout +
 * interaction by design.
 *
 * Reimplemented from the Micron grammar (see ../../../INTERNALS.md "Micron
 * markup", which tracks NomadNet's MicronParser.py). NomadNet is GPL-3.0 —
 * this is an independent implementation, not a port.
 *
 * SECURITY: page bytes come from an *untrusted remote node*. Every run of
 * literal text is HTML-escaped, and the renderer only ever emits a fixed
 * whitelist of tags + style props. Link targets are carried in a
 * `data-mtarget` attribute (never a real `href`), so there is no
 * `javascript:` / `data:` URL surface — the NomadWindow click handler
 * decides what a target means. Inline styles are limited to colors we
 * generate and boolean text decorations.
 *
 * Grammar handled:
 *   control char ` :
 *     ``        reset all formatting
 *     `!        toggle bold        `_ underline        `* italic
 *     `Fxxx     fg color (3 hex)   `f reset fg
 *     `Bxxx     bg color (3 hex)   `b reset bg
 *     `c `l `r `a   line alignment (center/left/right/default)
 *     `[label`target]   link (label optional → target is the label)
 *     `<w|name`value>   input field (NomadWindow reads its value on submit)
 *   line level:
 *     `=        horizontal divider
 *     >, >>, >>>   headings (h1/h2/h3)
 *     #            comment (whole line dropped)
 *     a lone `     toggles a literal/preformatted block
 */

export interface MicronLink {
  /** Raw target as written in the page, e.g. "<hash>:/page/x.mu", ":/page/x.mu",
   *  "/page/x.mu", "lxmf@…", "rrc://…". Resolution is the caller's job. */
  target: string
  label: string
}

function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;')
}

/** Micron color = 3 hex nibbles (#RGB, 4-bit each) → #RRGGBB. Returns null
 *  if the 3 chars aren't all hex. */
function micronColor(triplet: string): string | null {
  if (triplet.length !== 3 || !/^[0-9a-fA-F]{3}$/.test(triplet)) return null
  const r = triplet[0]!, g = triplet[1]!, b = triplet[2]!
  return `#${r}${r}${g}${g}${b}${b}`
}

interface Style {
  bold: boolean
  italic: boolean
  underline: boolean
  fg: string | null
  bg: string | null
}

function freshStyle(): Style {
  return { bold: false, italic: false, underline: false, fg: null, bg: null }
}

function styleCss(s: Style): string {
  const css: string[] = []
  if (s.bold) css.push('font-weight:bold')
  if (s.italic) css.push('font-style:italic')
  if (s.underline) css.push('text-decoration:underline')
  if (s.fg) css.push(`color:${s.fg}`)
  if (s.bg) css.push(`background-color:${s.bg}`)
  return css.join(';')
}

function styleActive(s: Style): boolean {
  return s.bold || s.italic || s.underline || s.fg !== null || s.bg !== null
}

/** Render one line's inline content (the `text` after any block prefix has
 *  been stripped). Returns { html, align } — align is set if a `c/`l/`r/`a
 *  control appeared. */
function renderInline(text: string): { html: string; align: string | null; bg: boolean } {
  let out = ''
  let align: string | null = null
  let st = freshStyle()
  let spanOpen = false
  let bgUsed = false   // any visible char drawn over a background color → mosaic row

  const closeSpan = () => { if (spanOpen) { out += '</span>'; spanOpen = false } }
  const syncSpan = () => {
    closeSpan()
    if (styleActive(st)) { out += `<span style="${styleCss(st)}">`; spanOpen = true }
  }

  for (let i = 0; i < text.length; i++) {
    const ch = text[i]!
    if (ch !== '`') {
      // Micron escape: backslash makes the next char literal (pages write
      // \[ or \` to dodge markup) — NomadNet renders it bare.
      if (ch === '\\' && i + 1 < text.length) {
        if (st.bg) bgUsed = true
        out += escapeHtml(text[i + 1]!)
        i += 1
        continue
      }
      // Bare lxmf@<32hex> → clickable contact link (not mid-token).
      if ((ch === 'l' || ch === 'L') && (i === 0 || !/[0-9a-z@._-]/i.test(text[i - 1]!))) {
        const m = /^lxmf@([0-9a-fA-F]{32})\b/.exec(text.slice(i))
        if (m) {
          const wasOpen = spanOpen
          closeSpan()
          const hash = m[1]!.toLowerCase()
          out += `<a class="mlxmf" data-lxmf="${hash}" title="lxmf@${hash}">${escapeHtml(m[0]!)}</a>`
          if (wasOpen && styleActive(st)) { out += `<span style="${styleCss(st)}">`; spanOpen = true }
          i += m[0]!.length - 1
          continue
        }
      }
      if (st.bg) bgUsed = true; out += escapeHtml(ch); continue
    }

    // ` control sequence
    const nx = text[i + 1]
    if (nx === undefined) { break }            // trailing backtick → drop
    if (nx === '`') { st = freshStyle(); syncSpan(); i += 1; continue }  // `` reset
    if (nx === '!') { st.bold = !st.bold; syncSpan(); i += 1; continue }
    if (nx === '_') { st.underline = !st.underline; syncSpan(); i += 1; continue }
    if (nx === '*') { st.italic = !st.italic; syncSpan(); i += 1; continue }
    if (nx === 'f') { st.fg = null; syncSpan(); i += 1; continue }
    if (nx === 'b') { st.bg = null; syncSpan(); i += 1; continue }
    if (nx === 'F' || nx === 'B') {
      const col = micronColor(text.slice(i + 2, i + 5))
      if (col) { if (nx === 'F') st.fg = col; else st.bg = col; syncSpan(); i += 4; continue }
      i += 1; continue                          // malformed color → swallow `F/`B
    }
    if (nx === 'c') { align = 'center'; i += 1; continue }
    if (nx === 'l') { align = 'left'; i += 1; continue }
    if (nx === 'r') { align = 'right'; i += 1; continue }
    if (nx === 'a') { align = null; i += 1; continue }
    if (nx === '[') {
      // link: `[label`target]  or  `[target]
      const end = text.indexOf(']', i + 2)
      if (end < 0) { i += 1; continue }
      const inner = text.slice(i + 2, end)
      const bt = inner.indexOf('`')
      const label = bt >= 0 ? inner.slice(0, bt) : inner
      const target = bt >= 0 ? inner.slice(bt + 1) : inner
      const wasOpen = spanOpen
      closeSpan()
      out += `<a class="mlink" data-mtarget="${escapeHtml(target)}" title="${escapeHtml(target)}">`
        + escapeHtml(label || target) + '</a>'
      if (wasOpen && styleActive(st)) { out += `<span style="${styleCss(st)}">`; spanOpen = true }
      i = end
      continue
    }
    if (nx === '<') {
      // input field: `<[flags]width|name`value>  — rendered disabled in v1
      const end = text.indexOf('>', i + 2)
      if (end < 0) { i += 1; continue }
      const spec = text.slice(i + 2, end)
      const bar = spec.indexOf('|')
      const meta = bar >= 0 ? spec.slice(0, bar) : ''
      let rest = bar >= 0 ? spec.slice(bar + 1) : spec
      const btv = rest.indexOf('`')
      const name = btv >= 0 ? rest.slice(0, btv) : rest
      const value = btv >= 0 ? rest.slice(btv + 1) : ''
      const masked = meta.includes('!')
      closeSpan()
      // Editable form field (Phase 4). NomadWindow reads `.mfield` values by
      // data-fname when a form-link is followed. Uncontrolled input: survives
      // re-render because page HTML only recomputes when the body changes.
      out += `<input class="mfield" type="${masked ? 'password' : 'text'}"`
        + ` data-fname="${escapeHtml(name)}" value="${escapeHtml(value)}"`
        + ` placeholder="${escapeHtml(name)}" />`
      syncSpan()
      i = end
      continue
    }
    // Unknown control → drop the backtick, keep the char literally.
    out += escapeHtml(nx)
    i += 1
  }
  closeSpan()
  return { html: out, align, bg: bgUsed }
}

/* Rows carrying block/box-drawing/braille glyphs (or legacy-computing mosaics)
 * are "graphics" and must tile vertically — see micronToHtml's .mgfx tag. */
const MOSAIC_RE = /[─-▟⠀-⣿\u{1FB00}-\u{1FBFF}]/u

/** Render a full Micron document to an HTML string. */
export function micronToHtml(src: string): string {
  const lines = src.split(/\r?\n/)
  const out: string[] = []
  let literal = false
  let litBuf: string[] = []

  const flushLiteral = () => {
    if (litBuf.length) {
      out.push(`<pre class="mliteral">${escapeHtml(litBuf.join('\n'))}</pre>`)
      litBuf = []
    }
  }

  for (const raw of lines) {
    // A lone backtick toggles a literal/preformatted block.
    if (raw.trim() === '`') {
      if (literal) { flushLiteral(); literal = false } else { literal = true }
      continue
    }
    if (literal) { litBuf.push(raw); continue }

    if (raw.length === 0) { out.push('<div class="mline">&nbsp;</div>'); continue }
    if (raw[0] === '#') continue                                   // comment

    // `= divider (optional repeat char after =, cosmetic)
    if (raw.startsWith('`=')) { out.push('<hr class="mdivider" />'); continue }

    // Headings: leading > run.
    if (raw[0] === '>') {
      let depth = 0
      while (depth < raw.length && raw[depth] === '>') depth++
      const level = Math.min(depth, 3)
      const { html, align } = renderInline(raw.slice(depth).replace(/^\s/, ''))
      const a = align ? ` style="text-align:${align}"` : ''
      out.push(`<h${level} class="mh"${a}>${html}</h${level}>`)
      continue
    }

    const { html, align, bg } = renderInline(raw)
    const a = align ? ` style="text-align:${align}"` : ''
    // Graphics rows (block/box glyphs or background-colour mosaics) tile only
    // with zero leading; tag them so the viewer drops their line-height.
    const gfx = bg || MOSAIC_RE.test(raw)
    out.push(`<div class="mline${gfx ? ' mgfx' : ''}"${a}>${html}</div>`)
  }
  if (literal) flushLiteral()
  return out.join('\n')
}
