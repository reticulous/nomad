/**
 * nomad — Nomad Network page browser (SPA side).
 *
 * Storage is the API (mirrors the firmware nomad task, docs/nomad.md):
 *   reads  = computed() over the reactive device mirror
 *   writes = cmd sentinels via device.sendJson()
 *
 * Browsing is SESSION-based: the firmware runs NOMAD_SESSIONS parallel
 * browser contexts, each with its own Link + nav/page state. The web UI
 * owns sessions 0..WEB_SESSIONS-1 (one per tab); the LCD owns session 6.
 *
 *   nomad.nodes.<hex>        "<last_s>|<hops>|<name>"   announce drift
 *   s.nomad.bookmarks.<id>   "<hash>[:<path>]|<name>|<note>"  persistent
 *   nomad.s<sid>.nav.{status,hash,path,error}           session nav state
 *   nomad.s<sid>.page.{hash,path,size,body,truncated,fetched_s}
 *   cmd: nomad.cmd.go=<sid>|<hash>[:<path>] / .reload=<sid>|<unique>
 *        / .submit=<sid>|<hash>:<path> (fields under nomad.submit.<sid>.*)
 *        / .bookmark.add / .bookmark.del
 */
import { ref, computed, type ComputedRef } from 'vue'
import { useDeviceStore } from 'spangap-browser/stores/device'
import { useMenuStore } from 'spangap-browser/stores/menu'
import NomadPanel from '../panels/NomadPanel.vue'

/* Visibility ref for the Nomad Browser floating window. */
export const nomadVisible = ref(false)

/** Focus nonce — bumped to raise the Nomad window even when already open.
 *  MainLayout binds it to the window's `focus-token` prop. */
export const nomadFocus = ref(0)

/* Menu "Nomad Browser" action: only ever show + raise, never hide. */
export function showNomad() { nomadVisible.value = true; nomadFocus.value++ }

export const DEFAULT_PAGE = '/page/index.mu'

/** Parallel firmware browser sessions owned by the web UI (tab ↔ session).
 *  Sessions 0..WEB_SESSIONS-1 are ours; session 6 is the LCD browser's. */
export const WEB_SESSIONS = 6

export interface NomadNode {
  hash: string
  name: string
  hops: number
  lastSeen: number
}

export interface Bookmark {
  id: string      /* opaque storage key tail — delete by this */
  hash: string
  path: string
  name: string
  note: string
}

export interface NomadPage {
  hash: string
  path: string
  size: number
  body: string
  truncated: boolean
  fetchedS: number
}

export type NavStatus =
  | 'idle' | 'path_requested' | 'establishing' | 'requesting'
  | 'done' | 'failed' | 'timeout'

type Patch = Record<string, any>

function nest(path: string, val: any): Patch {
  const parts = path.split('.')
  const root: Patch = {}
  let cur = root
  for (let i = 0; i < parts.length - 1; i++) cur = (cur[parts[i]!] = {})
  cur[parts[parts.length - 1]!] = val
  return root
}

/** Live view over one firmware browser session (nomad.s<sid>.*). */
export interface NomadSession {
  page: ComputedRef<NomadPage>
  navStatus: ComputedRef<NavStatus>
  navHash: ComputedRef<string>
  navPath: ComputedRef<string>
  navError: ComputedRef<string>
  busy: ComputedRef<boolean>
}

export interface UseNomad {
  nodes: ComputedRef<NomadNode[]>
  bookmarks: ComputedRef<Bookmark[]>
  /** Per-session live state; sid 0..WEB_SESSIONS-1. Cached per sid. */
  session: (sid: number) => NomadSession
  go: (sid: number, hash: string, path?: string) => void
  reload: (sid: number) => void
  /** Submit a form on a session. `data` keys are already the NomadNet map
   *  keys (`field_<name>` / `var_<name>`); staged under nomad.submit.<sid>.*
   *  then triggered with nomad.cmd.submit. */
  submit: (sid: number, hash: string, path: string, data: Record<string, string>) => void
  isBookmarked: (hash: string, path?: string) => boolean
  addBookmark: (hash: string, path?: string, name?: string, note?: string) => void
  delBookmark: (idOrUrl: string) => void
  /** Open an LXMF conversation for an lxmf@<hash> address tapped in a page
   *  (writes the shared `lxmf.url_web` var; LXMF reacts). */
  openLxmf: (hash: string) => void
}

const HASH_RE = /^[0-9a-fA-F]{32}$/

export function useNomad(): UseNomad {
  const device = useDeviceStore()

  const nodes = computed<NomadNode[]>(() => {
    const tree = device.get('nomad.nodes') ?? {}
    const out: NomadNode[] = []
    for (const [hash, raw] of Object.entries(tree)) {
      if (typeof raw !== 'string') continue
      // "<last_s>|<hops>|<name>"  (name may contain '|')
      const p1 = raw.indexOf('|')
      const p2 = p1 >= 0 ? raw.indexOf('|', p1 + 1) : -1
      if (p2 < 0) continue
      out.push({
        hash,
        lastSeen: parseInt(raw.slice(0, p1), 10) || 0,
        hops: parseInt(raw.slice(p1 + 1, p2), 10) || 0,
        name: raw.slice(p2 + 1),
      })
    }
    return out.sort((a, b) => b.lastSeen - a.lastSeen)
  })

  const bookmarks = computed<Bookmark[]>(() => {
    const tree = device.get('s.nomad.bookmarks') ?? {}
    const out: Bookmark[] = []
    for (const [id, raw] of Object.entries(tree)) {
      if (typeof raw !== 'string') continue
      // "<hash>[:<path>]|<name>|<note>" — note last (may contain '|')
      const p1 = raw.indexOf('|')
      if (p1 < 0) continue
      const url = raw.slice(0, p1)
      const colon = url.indexOf(':')
      const hash = colon >= 0 ? url.slice(0, colon) : url
      const path = colon >= 0 ? url.slice(colon + 1) : DEFAULT_PAGE
      if (!HASH_RE.test(hash)) continue
      const rest = raw.slice(p1 + 1)
      const p2 = rest.indexOf('|')
      out.push({
        id, hash, path,
        name: p2 >= 0 ? rest.slice(0, p2) : rest,
        note: p2 >= 0 ? rest.slice(p2 + 1) : '',
      })
    }
    return out.sort((a, b) => (a.name || a.hash).localeCompare(b.name || b.hash))
  })

  /* Per-session live views, built lazily and cached (computed identity per
   * sid keeps watchers stable across callers). */
  const sessions = new Map<number, NomadSession>()
  const session = (sid: number): NomadSession => {
    let s = sessions.get(sid)
    if (s) return s
    const base = `nomad.s${sid}`
    const page = computed<NomadPage>(() => {
      const p = device.get(`${base}.page`) ?? {}
      return {
        hash: String(p.hash ?? ''),
        path: String(p.path ?? ''),
        size: Number(p.size ?? 0),
        body: typeof p.body === 'string' ? p.body : '',
        truncated: Number(p.truncated ?? 0) !== 0,
        fetchedS: Number(p.fetched_s ?? 0),
      }
    })
    const navStatus = computed<NavStatus>(() =>
      (device.get(`${base}.nav.status`) as NavStatus) ?? 'idle')
    const navHash = computed(() => String(device.get(`${base}.nav.hash`) ?? ''))
    const navPath = computed(() => String(device.get(`${base}.nav.path`) ?? ''))
    const navError = computed(() => String(device.get(`${base}.nav.error`) ?? ''))
    const busy = computed(() =>
      ['path_requested', 'establishing', 'requesting'].includes(navStatus.value))
    s = { page, navStatus, navHash, navPath, navError, busy }
    sessions.set(sid, s)
    return s
  }

  const go = (sid: number, hash: string, path?: string) => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    const p = (path ?? '').trim()
    device.sendJson(nest('nomad.cmd.go', `${sid}|${p ? `${h}:${p}` : h}`))
  }

  // Unique value per press: a constant would be swallowed by the firmware's
  // storage SET-dedup if the key was ever left set by a dropped notify.
  const reload = (sid: number) => {
    device.sendJson(nest('nomad.cmd.reload', `${sid}|${Date.now()}`))
  }

  const submit = (sid: number, hash: string, path: string, data: Record<string, string>) => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    // Stage the field map + trigger in one patch — the firmware sees the
    // whole tree merged before the cmd.submit subscription fires. Fields go
    // under this session's staging tree.
    const patch: Patch = {}
    const submitTree: Patch = {}
    for (const [k, v] of Object.entries(data)) submitTree[k] = v
    patch.nomad = {
      submit: { [String(sid)]: submitTree },
      cmd: { submit: `${sid}|${h}:${path || DEFAULT_PAGE}` },
    }
    device.sendJson(patch)
  }

  /* path: omit/'' = any bookmark on the host; pass a path for exact match. */
  const isBookmarked = (hash: string, path = '') =>
    bookmarks.value.some(b => b.hash === hash.toLowerCase() &&
                              (!path || b.path === path))

  const addBookmark = (hash: string, path = DEFAULT_PAGE, name = '', note = '') => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    device.sendJson(nest('nomad.cmd.bookmark.add',
                         `${h}:${path || DEFAULT_PAGE}|${name}|${note}`))
  }

  /* By id (from the bookmarks list), or a "<hash>[:<path>]" url. */
  const delBookmark = (idOrUrl: string) => {
    device.sendJson(nest('nomad.cmd.bookmark.del', idOrUrl.trim()))
  }

  /* An lxmf@<hash> address tapped in a page hands the contact to LXMF, which
   * brings its own UI forward and (for an unknown contact) issues a path
   * request. Decoupled — we only write the shared storage var. */
  const openLxmf = (hash: string) => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    /* Nonce: a repeat tap is a fresh value (the firmware no longer consumes
     * the key — its unset raced the browser sync and ate the hash). */
    device.sendJson(nest('lxmf.url_web', `${h}:${Date.now()}`))
  }

  return {
    nodes, bookmarks, session,
    go, reload, submit, isBookmarked, addBookmark, delBookmark, openLxmf,
  }
}

export function registerNomad() {
  const menu = useMenuStore()

  /* Settings → Mesh Network → Nomad Network (the Nomad settings panel). */
  menu.register('settings/mesh/nomad', 'Nomad Network', { type: 'panel', component: NomadPanel }, { placement: 4 })

  /* Top-level "Nomad Browser" menu — single action foregrounds the window. */
  menu.setMenu('nomad', { label: 'Nomad Browser', placement: 3 })
  menu.register('nomad/browser', 'Nomad Browser',
    { type: 'action', action: showNomad })
}
