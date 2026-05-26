/**
 * nomad — Nomad Network page browser (SPA side).
 *
 * Storage is the API (mirrors the firmware nomad task, docs/nomad.md):
 *   reads  = computed() over the reactive device mirror
 *   writes = cmd sentinels via device.sendJson()
 *
 *   nomad.nodes.<hex>        "<last_s>|<hops>|<name>"   announce drift
 *   s.nomad.bookmarks.<hex>  "<name>|<note>"            persistent
 *   nomad.nav.{status,hash,path,error}                  navigation state
 *   nomad.page.{hash,path,size,body,truncated,fetched_s} last page (body capped)
 *   cmd: nomad.cmd.go=<hash>[:<path>] / .reload / .bookmark.add / .bookmark.del
 */
import { ref, computed, type ComputedRef } from 'vue'
import { useDeviceStore } from 'diptych-browser/stores/device'
import { useMenuStore } from 'diptych-browser/stores/menu'
import NomadPanel from '../panels/NomadPanel.vue'

/* Visibility ref for the Status → Nomad Browser floating window. */
export const nomadVisible = ref(false)

export const DEFAULT_PAGE = '/page/index.mu'

export interface NomadNode {
  hash: string
  name: string
  hops: number
  lastSeen: number
}

export interface Bookmark {
  hash: string
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

export interface UseNomad {
  nodes: ComputedRef<NomadNode[]>
  bookmarks: ComputedRef<Bookmark[]>
  page: ComputedRef<NomadPage>
  navStatus: ComputedRef<NavStatus>
  navHash: ComputedRef<string>
  navPath: ComputedRef<string>
  navError: ComputedRef<string>
  busy: ComputedRef<boolean>
  go: (hash: string, path?: string) => void
  goUrl: (url: string) => void
  reload: () => void
  /** Submit a form. `data` keys are already the NomadNet map keys
   *  (`field_<name>` / `var_<name>`); staged under nomad.submit.* then
   *  triggered with nomad.cmd.submit. */
  submit: (hash: string, path: string, data: Record<string, string>) => void
  isBookmarked: (hash: string) => boolean
  addBookmark: (hash: string, name?: string, note?: string) => void
  delBookmark: (hash: string) => void
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
    for (const [hash, raw] of Object.entries(tree)) {
      if (typeof raw !== 'string') continue
      const bar = raw.indexOf('|')
      out.push({
        hash,
        name: bar >= 0 ? raw.slice(0, bar) : raw,
        note: bar >= 0 ? raw.slice(bar + 1) : '',
      })
    }
    return out.sort((a, b) => (a.name || a.hash).localeCompare(b.name || b.hash))
  })

  const page = computed<NomadPage>(() => {
    const p = device.get('nomad.page') ?? {}
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
    (device.get('nomad.nav.status') as NavStatus) ?? 'idle')
  const navHash = computed(() => String(device.get('nomad.nav.hash') ?? ''))
  const navPath = computed(() => String(device.get('nomad.nav.path') ?? ''))
  const navError = computed(() => String(device.get('nomad.nav.error') ?? ''))
  const busy = computed(() =>
    ['path_requested', 'establishing', 'requesting'].includes(navStatus.value))

  const go = (hash: string, path?: string) => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    const p = (path ?? '').trim()
    device.sendJson(nest('nomad.cmd.go', p ? `${h}:${p}` : h))
  }

  /* Resolve a Micron link target against the current node, then navigate.
   *  Grammar (docs/nomad.md): "<hash>:<path>" | ":<path>" (current node) |
   *  "<path>" (current node). @-prefixed / rrc:// schemes are out of scope
   *  for v1 — ignored so the link doesn't misfire. */
  const goUrl = (url: string) => {
    const u = url.trim()
    if (!u) return
    if (/^[a-z]+@/.test(u) || u.includes('://')) return    // lxmf@ / rrc:// — defer
    const colon = u.indexOf(':')
    if (colon === 32 && HASH_RE.test(u.slice(0, 32))) {
      go(u.slice(0, 32), u.slice(colon + 1) || DEFAULT_PAGE)
      return
    }
    if (HASH_RE.test(u)) { go(u, DEFAULT_PAGE); return }
    // bare path / ":path" → current node
    const path = u.startsWith(':') ? u.slice(1) : u
    const cur = navHash.value
    if (HASH_RE.test(cur)) go(cur, path || DEFAULT_PAGE)
  }

  const reload = () => { device.sendJson(nest('nomad.cmd.reload', '1')) }

  const submit = (hash: string, path: string, data: Record<string, string>) => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    // Stage the field map + trigger in one patch — the firmware sees the
    // whole tree merged before the cmd.submit subscription fires.
    const patch: Patch = {}
    const submitTree: Patch = {}
    for (const [k, v] of Object.entries(data)) submitTree[k] = v
    patch.nomad = { submit: submitTree, cmd: { submit: `${h}:${path || DEFAULT_PAGE}` } }
    device.sendJson(patch)
  }

  const isBookmarked = (hash: string) =>
    bookmarks.value.some(b => b.hash === hash.toLowerCase())

  const addBookmark = (hash: string, name = '', note = '') => {
    const h = hash.trim().toLowerCase()
    if (!HASH_RE.test(h)) return
    device.sendJson(nest('nomad.cmd.bookmark.add', `${h}|${name}|${note}`))
  }

  const delBookmark = (hash: string) => {
    device.sendJson(nest('nomad.cmd.bookmark.del', hash.trim().toLowerCase()))
  }

  return {
    nodes, bookmarks, page, navStatus, navHash, navPath, navError, busy,
    go, goUrl, reload, submit, isBookmarked, addBookmark, delBookmark,
  }
}

export function registerNomad() {
  const menu = useMenuStore()

  menu.register('settings', 'Settings', 10, [
    { id: 'reticulum', label: 'Reticulum', type: 'submenu', order: 30,
      children: [
        { id: 'reticulum.nomad', label: 'Nomad Network', type: 'panel', order: 40,
          component: NomadPanel },
      ],
    },
  ])

  menu.register('status', 'Status', 20, [
    { id: 'status.nomad', label: 'Nomad Browser', type: 'action', order: 30,
      action: () => { nomadVisible.value = !nomadVisible.value } },
  ])
}
