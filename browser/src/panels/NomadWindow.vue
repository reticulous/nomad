<!-- Nomad Browser menu (the main browser window). A tabbed Nomad Network
     page browser: a collapsible sidebar (search, bookmarks, on-the-mesh), a
     tab strip (up to 6 pages loaded at once), an address bar, and a Micron
     page view. Each tab is an independent mini-browser with its own
     Back/forward history AND its own firmware browser session (nomad.s<sid>,
     sid 0-5) — tabs fetch in parallel, each over its own Link; a per-session
     watcher mirrors firmware nav/page state into its tab whether or not the
     tab is foreground. Switching tabs just changes which one you look at.
     Sidebar / address-bar opens go to a new tab (or foreground the tab that
     already holds that URL); in-page links stay in the current tab unless
     Ctrl/Cmd-clicked. -->
<template>
  <FloatingWindow
    id="nomad"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :min-size="{ w: 40, h: 24 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #titlebar-right>
      <span class="rfs-btn" title="Smaller" @click="zoomOut">-</span>
      <span class="rfs-btn" title="Larger" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="nomad" :style="{ '--rfs': scale }">
        <!-- tab strip -->
        <div class="tabstrip">
          <div class="tabs">
            <div
              v-for="t in tabs" :key="t.id"
              class="tab" :class="{ active: t.id === activeTabId }"
              :title="tabTitle(t)" @click="activateTab(t.id)"
            >
              <span class="tab-name">{{ tabTitle(t) }}</span>
              <button class="tab-close" title="Close tab" @click.stop="closeTab(t.id)">×</button>
            </div>
          </div>
          <button class="icon-btn tab-new" :disabled="tabs.length >= MAX_TABS"
                  title="New tab" @click="newTab">+</button>
        </div>

        <!-- address bar: ☰ ⟳ ‹ — the same three buttons, same order, as the
             LCD browser's header -->
        <div class="bar">
          <button class="nav burger"
                  :title="sideOpen ? 'Hide site list' : 'Show site list'"
                  @click="toggleSide">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round">
              <line x1="3" y1="6" x2="21" y2="6" /><line x1="3" y1="12" x2="21" y2="12" /><line x1="3" y1="18" x2="21" y2="18" />
            </svg>
          </button>
          <button class="nav glyph" title="Reload" :disabled="!curHash" @click="reload"><span class="g">⟳</span></button>
          <button class="nav glyph" :disabled="!canBack" title="Back" @click="back"><span class="g">‹</span></button>
          <input
            v-model="address"
            class="addr mono"
            placeholder="<32-hex hash>[:/page/index.mu]"
            @keyup.enter="goAddress"
          />
          <button class="nav go" title="Go" @click="goAddress">Go</button>
          <button
            class="nav star"
            :class="{ on: curBookmarked }"
            :disabled="!curHash"
            :title="curBookmarked ? 'Remove bookmark' : 'Bookmark this node'"
            @click="toggleBookmark"
          >{{ curBookmarked ? '★' : '☆' }}</button>
        </div>

        <div class="body">
          <!-- sidebar (collapsible) -->
          <div v-if="sideOpen" class="side" @scroll.passive="onSideScroll">
            <input
              v-model="sideQ" class="side-search"
              placeholder="Search nodes" autocomplete="off"
              @keydown.esc="sideQ = ''"
            />

            <div class="side-h">Bookmarks</div>
            <div v-if="filteredBookmarks.length === 0" class="side-empty">{{ sideQ.trim() ? 'no matches' : 'none' }}</div>
            <div
              v-for="b in filteredBookmarks" :key="b.id"
              class="item" :class="{ active: b.hash === curHash }"
              :title="`${b.hash}:${b.path}`" @click="openUrl(b.hash, b.path)"
            >
              <div class="item-name">{{ b.name || shortHash(b.hash) }}</div>
              <!-- always present (nbsp when blank) so bookmark cells are equal height -->
              <div class="item-sub">{{ bookmarkSub(b) || ' ' }}</div>
            </div>

            <div class="side-h">On the Mesh</div>
            <div v-if="filteredNodes.length === 0" class="side-empty">{{ sideQ.trim() ? 'no matches' : 'none heard yet' }}</div>
            <div
              v-for="n in filteredNodes" :key="n.hash"
              class="item" :class="{ active: n.hash === curHash }"
              :title="n.hash" @click="openUrl(n.hash, DEFAULT_PAGE)"
            >
              <div class="item-name">{{ n.name || shortHash(n.hash) }}</div>
              <div class="item-sub">{{ n.hops }} hops · {{ age(n.lastSeen) }}</div>
            </div>
          </div>

          <!-- page view; the status line hovers top-right, auto-hiding 2 s
               after load and reappearing on scroll -->
          <div class="view">
            <div v-show="statusVisible" class="status" :class="statusClass">
              <span class="dot" /><span class="status-text">{{ statusText }}</span>
              <button
                v-if="activeTab && activeTab.body"
                class="src-toggle"
                :title="viewSource ? 'Show rendered page' : 'Show Micron source'"
                @click="viewSource = !viewSource"
              >{{ viewSource ? 'view page' : 'view source' }}</button>
            </div>
            <div v-if="showError" class="page-msg err">
              Could not load the page.<span v-if="activeTab && activeTab.error"> ({{ activeTab.error }})</span>
            </div>
            <div v-else-if="activeTab && activeTab.truncated" class="page-msg">
              Page is {{ activeTab.size }} bytes — too large to display here.
            </div>
            <div v-else-if="!activeTab || !activeTab.body" class="page-msg dim">
              Pick a bookmark or node, or enter an address above.
            </div>
            <pre v-else-if="viewSource" class="page source" @scroll.passive="pokeStatus">{{ activeTab.body }}</pre>
            <div v-else ref="pageEl" class="page" v-html="pageHtml" @click="onPageClick" @scroll.passive="pokeStatus" />
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, reactive, computed, watch } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { useNomad, nomadOpenUrl, DEFAULT_PAGE, WEB_SESSIONS, type NavStatus, type NomadNode, type Bookmark } from '../modules/nomad'
import { micronToHtml } from '../lib/micron'
import { useWinZoom } from 'rns/lib/winZoom'

const { scale, zoomIn, zoomOut } = useWinZoom('nomad')

defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 14, y: 8, w: 70, h: 80 }
const HASH_RE = /^[0-9a-fA-F]{32}$/
const MAX_TABS = WEB_SESSIONS   /* one firmware session per tab */

const nomad = useNomad()

/* ── Tabs ──
 * A tab is an independent mini-browser bound to one firmware session (sid):
 * its own {hash,path}, Back/forward history, and a live mirror of that
 * session's nav/page state (see the per-session watchers below) — so a
 * background tab keeps loading while you look at another. `title` holds a
 * received page title when one exists; otherwise the tab is named after the
 * node's human-readable name (see tabTitle). */
interface Tab {
  id: number
  sid: number
  hash: string
  path: string
  title: string
  history: { hash: string; path: string }[]
  histPos: number
  body: string
  size: number
  truncated: boolean
  status: NavStatus
  error: string
}

let tabSeq = 0
function makeTab(sid: number): Tab {
  return {
    id: ++tabSeq, sid, hash: '', path: '', title: '',
    history: [], histPos: -1,
    body: '', size: 0, truncated: false, status: 'idle', error: '',
  }
}

const tabs = reactive<Tab[]>([makeTab(0)])
const activeTabId = ref<number>(tabs[0]!.id)

/* Lowest firmware session not bound to a tab; -1 when all are in use. */
function freeSid(): number {
  for (let s = 0; s < MAX_TABS; s++) if (!tabs.some(t => t.sid === s)) return s
  return -1
}
function blankTab(): Tab | null {
  const sid = freeSid()
  return sid < 0 ? null : makeTab(sid)
}
const activeTab = computed<Tab | undefined>(() => tabs.find(t => t.id === activeTabId.value))

const pageEl = ref<HTMLElement | null>(null)
const address = ref('')

/* View toggle: rendered page (default) vs. raw Micron source. */
const viewSource = ref(false)

/* Sidebar collapse, persisted client-side. */
const LS_SIDE = 'nomad.sideOpen'
const sideOpen = ref(localStorage.getItem(LS_SIDE) !== '0')
function toggleSide() {
  sideOpen.value = !sideOpen.value
  localStorage.setItem(LS_SIDE, sideOpen.value ? '1' : '0')
}

/* Sidebar filter — name or hash substring, over both sections. */
const sideQ = ref('')
const filteredBookmarks = computed(() => {
  const needle = sideQ.value.trim().toLowerCase()
  if (!needle) return nomad.bookmarks.value
  return nomad.bookmarks.value.filter(b =>
    b.name.toLowerCase().includes(needle) || b.hash.toLowerCase().includes(needle))
})

/* Bookmark sub line: the note plus when the node was last heard on the
 * mesh (blank until an announce arrives). */
function nodeSeen(hash: string): string {
  const n = nomad.nodes.value.find(nd => nd.hash === hash)
  return n ? `${n.hops} hops · ${age(n.lastSeen)}` : ''
}
function bookmarkSub(b: Bookmark): string {
  const seen = nodeSeen(b.hash)
  if (!b.note) return seen
  return seen ? `${b.note} · ${seen}` : b.note
}

/* "On the Mesh": bookmarked nodes live in the section above (not repeated
 * here), and the ORDER is held still while the operator is using the
 * sidebar — announce churn within SIDE_HOLD_MS of the last scroll updates
 * rows in place and appends newcomers at the end instead of re-sorting
 * the list under the pointer. */
const SIDE_HOLD_MS = 10000
const sideScrollTs = ref(0)
function onSideScroll() { sideScrollTs.value = Date.now() }

const bookmarkedHashes = computed(() => new Set(nomad.bookmarks.value.map(b => b.hash)))
const meshNodes = computed(() =>
  nomad.nodes.value.filter(n => !bookmarkedHashes.value.has(n.hash)))

const orderedNodes = ref<NomadNode[]>([])
watch(meshNodes, nodes => {
  if (Date.now() - sideScrollTs.value >= SIDE_HOLD_MS) {
    orderedNodes.value = nodes                  /* already newest-first */
    return
  }
  const byHash = new Map(nodes.map(n => [n.hash, n]))
  const next: NomadNode[] = []
  for (const o of orderedNodes.value) {
    const n = byHash.get(o.hash)
    if (n) { next.push(n); byHash.delete(o.hash) }
  }
  for (const n of byHash.values()) next.push(n)  /* newcomers at the end */
  orderedNodes.value = next
}, { immediate: true })

const filteredNodes = computed(() => {
  const needle = sideQ.value.trim().toLowerCase()
  if (!needle) return orderedNodes.value
  return orderedNodes.value.filter(n =>
    n.name.toLowerCase().includes(needle) || n.hash.toLowerCase().includes(needle))
})

/* ── Derived from the active tab ── */
const curHash = computed(() => activeTab.value?.hash ?? '')
const curPath = computed(() => activeTab.value?.path || DEFAULT_PAGE)
const curBookmarked = computed(() =>
  !!curHash.value && nomad.isBookmarked(curHash.value, curPath.value))
const pageHtml = computed(() => micronToHtml(activeTab.value?.body ?? ''))
const canBack = computed(() => (activeTab.value?.histPos ?? -1) > 0)

const statusText = computed(() => {
  const t = activeTab.value
  switch (t?.status) {
    case 'path_requested': return 'Requesting path…'
    case 'establishing':   return 'Establishing link…'
    case 'requesting':     return 'Requesting page…'
    case 'done':           return `${t.path || ''} · ${t.size} B`
    case 'failed':         return 'Failed'
    case 'timeout':        return 'Timed out'
    default:               return 'Ready'
  }
})
const statusBusy = computed(() =>
  ['path_requested', 'establishing', 'requesting'].includes(activeTab.value?.status ?? 'idle'))
const statusClass = computed(() => ({
  busy: statusBusy.value,
  ok: activeTab.value?.status === 'done',
  bad: activeTab.value?.status === 'failed' || activeTab.value?.status === 'timeout',
}))
const showError = computed(() => {
  const t = activeTab.value
  return !!t && (t.status === 'failed' || t.status === 'timeout') && !t.body
})

/* Status hover lifecycle: visible while loading/failed; once the page is
 * loaded it lingers 2 s and hides; scrolling the page brings it back for
 * another 2 s. */
const statusVisible = ref(true)
let statusHideTimer: ReturnType<typeof setTimeout> | null = null
function pokeStatus() {
  statusVisible.value = true
  if (statusHideTimer) { clearTimeout(statusHideTimer); statusHideTimer = null }
  if (activeTab.value?.status === 'done')
    statusHideTimer = setTimeout(() => { statusVisible.value = false }, 2000)
}
watch(() => [activeTabId.value, activeTab.value?.status], pokeStatus)

function shortHash(h: string): string { return h.slice(0, 8) + '…' }

function age(epochS: number): string {
  if (!epochS) return '—'
  const s = Math.round(Date.now() / 1000 - epochS)
  if (s < 60) return `${s}s`
  if (s < 3600) return `${Math.floor(s / 60)}m`
  if (s < 86400) return `${Math.floor(s / 3600)}h`
  return `${Math.floor(s / 86400)}d`
}

/* Tab label: a received page title wins; otherwise the node's human-readable
 * name (bookmark name, then announced node name), then a short hash. */
function tabTitle(t: Tab): string {
  if (t.title) return t.title
  if (!t.hash) return 'New tab'
  const bm = nomad.bookmarks.value.find(b => b.hash === t.hash)?.name
  const node = nomad.nodes.value.find(n => n.hash === t.hash)?.name
  return bm || node || shortHash(t.hash)
}

function sameUrl(t: Tab, hash: string, path: string): boolean {
  return t.hash === hash && (t.path || DEFAULT_PAGE) === (path || DEFAULT_PAGE)
}

function syncAddress() {
  const t = activeTab.value
  address.value = t && t.hash ? `${t.hash}:${t.path || DEFAULT_PAGE}` : ''
}

/* Drive the firmware to (re)fetch the active tab's URL on its session. */
function fetchActive() {
  const t = activeTab.value
  if (!t || !HASH_RE.test(t.hash)) return
  t.status = 'path_requested'
  t.error = ''
  nomad.go(t.sid, t.hash, t.path)
}

function activateTab(id: number) {
  activeTabId.value = id
  syncAddress()            // foreground only — no refetch (cache is shown)
}

/* Point a tab at a URL and reset its history to that single entry. */
function loadInto(t: Tab, hash: string, path: string) {
  t.hash = hash; t.path = path; t.title = ''
  t.history = [{ hash, path }]; t.histPos = 0
  t.body = ''; t.size = 0; t.truncated = false
}

/* Open a URL: foreground an existing tab with the same URL, else reuse a
 * blank active tab, else open a new tab (replacing the active tab when at the
 * 5-tab cap). Used by the sidebar, the address bar, and Ctrl/Cmd link clicks. */
function openUrl(hash: string, path: string) {
  const h = hash.trim().toLowerCase()
  if (!HASH_RE.test(h)) return
  const p = path || DEFAULT_PAGE

  const existing = tabs.find(t => sameUrl(t, h, p))
  if (existing) { activateTab(existing.id); return }

  const cur = activeTab.value
  if (cur && !cur.hash) { loadInto(cur, h, p); fetchActive(); syncAddress(); return }

  const nt = tabs.length < MAX_TABS ? blankTab() : null
  if (!nt) {                       /* at the cap: replace the active tab */
    if (!cur) return
    loadInto(cur, h, p); fetchActive(); syncAddress(); return
  }
  loadInto(nt, h, p)
  tabs.push(nt)
  activeTabId.value = nt.id
  fetchActive(); syncAddress()
}

/* Navigate within the active tab (in-page link), pushing its history. */
function navigateInTab(hash: string, path: string) {
  const t = activeTab.value
  const h = hash.trim().toLowerCase()
  if (!t || !HASH_RE.test(h)) return
  const p = path || DEFAULT_PAGE
  if (!t.hash) { loadInto(t, h, p); fetchActive(); syncAddress(); return }
  t.history = t.history.slice(0, t.histPos + 1)
  t.history.push({ hash: h, path: p })
  t.histPos = t.history.length - 1
  t.hash = h; t.path = p; t.title = ''
  t.body = ''; t.size = 0; t.truncated = false
  fetchActive(); syncAddress()
}

function back() {
  const t = activeTab.value
  if (!t || t.histPos <= 0) return
  t.histPos -= 1
  const e = t.history[t.histPos]!
  t.hash = e.hash; t.path = e.path; t.title = ''
  t.body = ''; t.size = 0; t.truncated = false
  fetchActive(); syncAddress()
}

function reload() {
  const t = activeTab.value
  if (!t?.hash) return
  /* A true reload, not a re-`go`: the firmware bypasses the cache read but
   * keeps the cached copy until fresh bytes replace it — so mark the current
   * page in history first; Back returns to the cached copy if the reload
   * can't connect. */
  t.history = t.history.slice(0, t.histPos + 1)
  t.history.push({ hash: t.hash, path: t.path })
  t.histPos = t.history.length - 1
  t.status = 'path_requested'
  t.error = ''
  nomad.reload(t.sid)
}

function newTab() {
  const nt = blankTab()
  if (!nt) return
  tabs.push(nt)
  activeTabId.value = nt.id
  syncAddress()
}

function closeTab(id: number) {
  const idx = tabs.findIndex(t => t.id === id)
  if (idx < 0) return
  const wasActive = tabs[idx]!.id === activeTabId.value
  tabs.splice(idx, 1)
  if (tabs.length === 0) {
    const nt = blankTab()
    if (nt) { tabs.push(nt); activeTabId.value = nt.id }
    syncAddress(); return
  }
  if (wasActive) {
    activeTabId.value = tabs[Math.min(idx, tabs.length - 1)]!.id
    syncAddress()
  }
}

function goAddress() {
  const u = address.value.trim()
  if (!u) return
  const colon = u.indexOf(':')
  if (colon === 32 && HASH_RE.test(u.slice(0, 32))) {
    openUrl(u.slice(0, 32), u.slice(colon + 1) || DEFAULT_PAGE)
  } else if (HASH_RE.test(u)) {
    openUrl(u, DEFAULT_PAGE)
  }
}

/* Resolve a Micron URL (the part before any `field/`var segments) to a
 * {hash,path} against the current node. @-scheme / "://" deferred. */
function resolveUrl(url: string): { hash: string; path: string } | null {
  const u = url.trim()
  if (!u) return null
  if (/^[a-z]+@/.test(u) || u.includes('://')) return null
  const colon = u.indexOf(':')
  if (colon === 32 && HASH_RE.test(u.slice(0, 32)))
    return { hash: u.slice(0, 32), path: u.slice(colon + 1) || DEFAULT_PAGE }
  if (HASH_RE.test(u)) return { hash: u, path: DEFAULT_PAGE }
  const path = u.startsWith(':') ? u.slice(1) : u
  if (HASH_RE.test(curHash.value)) return { hash: curHash.value, path: path || DEFAULT_PAGE }
  return null
}

/* A Micron link target is `url`fields`vars` (backtick-separated after the
 * first backtick, which the renderer already kept). A segment with '=' is
 * vars (name=val|…); without, fields (name|…|*). */
function parseTarget(target: string): { url: string; fields: string[]; vars: Record<string, string> } {
  const parts = target.split('`')
  const fields: string[] = []
  const vars: Record<string, string> = {}
  for (const seg of parts.slice(1)) {
    if (!seg) continue
    if (seg.includes('=')) {
      for (const pair of seg.split('|')) {
        const eq = pair.indexOf('=')
        if (eq > 0) vars[pair.slice(0, eq)] = pair.slice(eq + 1)
      }
    } else {
      for (const f of seg.split('|')) if (f) fields.push(f)
    }
  }
  return { url: parts[0] ?? '', fields, vars }
}

/* Follow a Micron link. Plain GET → current tab (or a new tab when
 * Ctrl/Cmd-clicked). A form (fields/vars) always submits in the current tab,
 * gathering the named `.mfield` values (`*` = all) + var literals. */
function followTarget(target: string, newTabReq: boolean) {
  if (!target.trim()) return
  const { url, fields, vars } = parseTarget(target)
  // A link whose target is an lxmf@<hash> address opens an LXMF conversation
  // instead of navigating to a page.
  const lx = /^lxmf@([0-9a-fA-F]{32})$/i.exec(url.trim())
  if (lx) { nomad.openLxmf(lx[1]!.toLowerCase()); return }
  const r = resolveUrl(url)
  if (!r) return

  const isForm = fields.length > 0 || Object.keys(vars).length > 0
  if (!isForm) {
    if (newTabReq) openUrl(r.hash, r.path)
    else navigateInTab(r.hash, r.path)
    return
  }

  const data: Record<string, string> = {}
  const wantAll = fields.includes('*')
  pageEl.value?.querySelectorAll<HTMLInputElement>('input.mfield').forEach(inp => {
    const name = inp.getAttribute('data-fname') || ''
    if (name && (wantAll || fields.includes(name))) data[`field_${name}`] = inp.value
  })
  for (const [k, v] of Object.entries(vars)) data[`var_${k}`] = v

  const t = activeTab.value
  if (!t) return
  if (!t.hash) { loadInto(t, r.hash, r.path) }
  else {
    t.history = t.history.slice(0, t.histPos + 1)
    t.history.push({ hash: r.hash, path: r.path })
    t.histPos = t.history.length - 1
    t.hash = r.hash; t.path = r.path; t.title = ''
    t.body = ''; t.size = 0; t.truncated = false
  }
  t.status = 'path_requested'; t.error = ''
  nomad.submit(t.sid, r.hash, r.path, data)
  syncAddress()
}

function onPageClick(ev: MouseEvent) {
  const t = ev.target as HTMLElement | null
  const lx = t?.closest('a.mlxmf') as HTMLElement | null
  if (lx) {
    ev.preventDefault()
    const h = lx.getAttribute('data-lxmf')
    if (h) nomad.openLxmf(h)
    return
  }
  const el = t?.closest('a.mlink') as HTMLElement | null
  if (!el) return
  ev.preventDefault()
  const target = el.getAttribute('data-mtarget')
  if (target) followTarget(target, ev.ctrlKey || ev.metaKey)
}

function toggleBookmark() {
  if (!curHash.value) return
  if (curBookmarked.value) {
    const bm = nomad.bookmarks.value.find(
      b => b.hash === curHash.value && b.path === curPath.value)
    if (bm) nomad.delBookmark(bm.id)
  } else {
    const node = nomad.nodes.value.find(n => n.hash === curHash.value)
    nomad.addBookmark(curHash.value, curPath.value, node?.name ?? '', '')
  }
}

/* Open requests from other apps (an LXMF message's Nomad link, via the
 * module's nomadOpenUrl channel) land in a tab like a sidebar pick. The window
 * is already being raised by showNomad() in the module watcher. */
watch(() => nomadOpenUrl.value, (req) => {
  if (req) openUrl(req.hash, req.path)
})

/* Live mirror, one watcher per firmware session: copy the session's
 * nav/page state into whichever tab owns that sid — foreground or not, so
 * parallel fetches land in their tabs as they complete. The hash guard
 * drops stale results when the tab has since navigated elsewhere. */
for (let sid = 0; sid < MAX_TABS; sid++) {
  const sess = nomad.session(sid)
  watch(
    () => [sess.navStatus.value, sess.navHash.value, sess.navPath.value,
           sess.page.value.hash, sess.page.value.body, sess.page.value.size,
           sess.page.value.truncated, sess.navError.value],
    () => {
      const t = tabs.find(tb => tb.sid === sid)
      if (!t || !t.hash || sess.navHash.value !== t.hash) return
      t.status = sess.navStatus.value
      t.error = sess.navError.value
      if (sess.navPath.value) t.path = sess.navPath.value
      if (sess.page.value.hash === t.hash) {
        t.body = sess.page.value.body
        t.size = sess.page.value.size
        t.truncated = sess.page.value.truncated
      }
      if (t.id === activeTabId.value) syncAddress()
    },
  )
}
</script>

<style scoped>
.nomad { display: flex; flex-direction: column; height: 100%; color: #d8d8d8; }

/* tab strip */
.tabstrip {
  display: flex; align-items: stretch; gap: 4px;
  padding: 4px 6px 0; background: #161616;
  border-bottom: 1px solid rgba(255,255,255,0.08);
}
.icon-btn {
  display: flex; align-items: center; justify-content: center; flex: none;
  background: #2a2a2a; border: 1px solid rgba(255,255,255,0.15); color: #ddd;
  border-radius: 5px; cursor: pointer; padding: 0 8px; font-size: 15px; line-height: 1;
}
.icon-btn:hover:not(:disabled) { background: #353535; }
.icon-btn:disabled { opacity: 0.4; cursor: default; }
.tab-new { align-self: center; height: 24px; font-weight: 700; }
.tabs { display: flex; gap: 4px; flex: 1; min-width: 0; overflow-x: auto; }
.tab {
  display: flex; align-items: center; gap: 6px; flex: 0 1 160px; min-width: 60px;
  padding: 4px 8px; cursor: pointer; user-select: none;
  background: #1f1f1f; border: 1px solid rgba(255,255,255,0.08); border-bottom: none;
  border-radius: 6px 6px 0 0; color: #b8b8b8;
}
.tab:hover { background: #262626; }
.tab.active { background: #2a2a2a; color: #fff; }
.tab-name {
  flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  font-size: calc(12px * var(--rfs, 1));
}
.tab-close {
  flex: none; background: none; border: none; color: #888; cursor: pointer;
  font-size: 14px; line-height: 1; padding: 0 2px; border-radius: 3px;
}
.tab-close:hover { background: rgba(255,255,255,0.12); color: #eee; }

.bar {
  display: flex; gap: 6px; padding: 6px; border-bottom: 1px solid rgba(255,255,255,0.08);
  background: #1b1b1b;
}
.nav {
  background: #2a2a2a; border: 1px solid rgba(255,255,255,0.15); color: #ddd;
  border-radius: 5px; padding: 4px 10px; cursor: pointer; font-size: 14px;
}
.nav:hover:not(:disabled) { background: #353535; }
.nav:disabled { opacity: 0.4; cursor: default; }
/* back ‹ and reload ⟳ glyphs were tiny — bump to 250%; Go/star to 125% */
.nav.glyph { font-size: 35px; line-height: 1; padding: 0 12px; }
/* both glyphs sit optically low in their box — raise by 15% of char height */
.nav.glyph .g { display: inline-block; transform: translateY(-15%); }
/* the ☰ site-list toggle, leftmost in the bar (matches the LCD header) */
.nav.burger { display: flex; align-items: center; justify-content: center; padding: 0 10px; }
.nav.go { font-size: 17.5px; }
.nav.star { font-size: 17.5px; }
.nav.star.on { color: #ffd24a; border-color: rgba(255,210,74,0.4); }
.addr {
  flex: 1; background: #141414; border: 1px solid rgba(255,255,255,0.15);
  color: #e8e8e8; border-radius: 5px; padding: 4px 8px; min-width: 0;
}
.mono { font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 12px; }
.body { display: flex; flex: 1; min-height: 0; }
.side {
  width: 30%; max-width: 240px; min-width: 120px; overflow: auto;
  border-right: 1px solid rgba(255,255,255,0.08); background: #181818; padding: 4px;
}
.side-search {
  width: 100%; box-sizing: border-box; margin: 2px 0 4px;
  background: #141414; border: 1px solid rgba(255,255,255,0.15); color: #e8e8e8;
  border-radius: 5px; padding: 5px 8px; font-size: calc(12px * var(--rfs, 1)); outline: none;
}
.side-search:focus { border-color: rgba(120,170,140,0.6); }
.side-h {
  font-size: calc(11px * var(--rfs, 1)); text-transform: uppercase; letter-spacing: 0.06em;
  color: #888; padding: 8px 6px 4px;
}
.side-h .dim, .dim { color: #666; }
.side-empty { color: #666; font-style: italic; font-size: calc(12px * var(--rfs, 1)); padding: 2px 6px; }
.item {
  padding: 5px 6px; border-radius: 5px; cursor: pointer;
}
.item:hover { background: #242424; }
.item.active { background: #2c3340; }
.item-name {
  color: #e0e0e0; font-size: calc(13px * var(--rfs, 1)); overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.item-sub { color: #888; font-size: calc(11px * var(--rfs, 1)); }
.view { flex: 1; display: flex; flex-direction: column; min-width: 0; position: relative; }
/* Floating status chip: top-right over the page, black on #ffc, 2px
 * margin + padding; v-show'd by the hover lifecycle (pokeStatus). */
.status {
  position: absolute; top: 2px; right: 2px; z-index: 5;
  display: flex; align-items: center; gap: 6px;
  font-size: calc(12px * var(--rfs, 1)); color: #000;
  padding: 2px; background: #ffc; border-radius: 6px;
  box-shadow: 0 1px 4px rgba(0,0,0,0.4);
}
.status .dot { width: 8px; height: 8px; border-radius: 50%; background: #666; }
.status .status-text { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
/* view source / view page toggle, pushed to the right edge of the status line */
.src-toggle {
  margin-left: 6px; flex: none; background: rgba(0,0,0,0.08); border: 1px solid rgba(0,0,0,0.25);
  color: #222; border-radius: 4px; padding: 1px 8px; cursor: pointer;
  font-size: calc(11px * var(--rfs, 1)); font-family: inherit;
}
.src-toggle:hover { background: rgba(0,0,0,0.16); color: #000; }
/* raw Micron source view */
.page.source {
  margin: 0; white-space: pre-wrap; word-break: break-word;
  font-family: 'JetBrains Mono', 'Menlo', monospace; color: #cfd3d8;
}
.status.busy .dot { background: #e0a72a; animation: pulse 1s infinite; }
.status.ok .dot { background: #4abf6a; }
.status.bad .dot { background: #d9534f; }
@keyframes pulse { 50% { opacity: 0.3; } }
.page-msg { padding: 20px; color: #bbb; font-size: calc(14px * var(--rfs, 1)); }
.page-msg.err { color: #e89; }
.page {
  flex: 1; overflow: auto; padding: 14px 18px; line-height: 1.45;
  user-select: text; font-size: calc(14px * var(--rfs, 1)); color: #dcdcdc;
  /* Micron is terminal content: box-drawing + column-aligned graphics
     only line up under a fixed-width font. */
  font-family: 'JetBrains Mono', 'Menlo', monospace;
}
/* Micron-rendered content (scoped :deep so v-html children are styled). */
.page :deep(.mline) { white-space: pre-wrap; word-break: break-word; }
/* Graphics rows tile vertically only with no leading: drop the line-height
 * to exactly 1 so block glyphs and background-colour mosaics meet edge-to-
 * edge. Prose rows keep the comfortable .page line-height. */
.page :deep(.mline.mgfx) { line-height: 1; }
.page :deep(.mh) { margin: 0.6em 0 0.3em; color: #fff; line-height: 1.2; }
.page :deep(h1.mh) { font-size: 1.5em; }
.page :deep(h2.mh) { font-size: 1.25em; }
.page :deep(h3.mh) { font-size: 1.1em; }
.page :deep(.mdivider) { border: 0; border-top: 1px solid rgba(255,255,255,0.18); margin: 0.7em 0; }
.page :deep(.mliteral) {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 0.9em;
  background: #141414; border: 1px solid rgba(255,255,255,0.08); border-radius: 5px;
  padding: 8px 10px; white-space: pre; overflow-x: auto;
  /* Preformatted blocks are an exact grid (often ASCII / box-drawing art),
   * so they tile only at line-height 1 — no leading between rows. */
  line-height: 1;
}
.page :deep(.mlink) { color: #6db3ff; cursor: pointer; text-decoration: underline; }
.page :deep(.mlink:hover) { color: #9ccbff; }
/* lxmf@<hash> contact links — green to distinguish from page navigation. */
.page :deep(.mlxmf) { color: #7fd0a0; cursor: pointer; text-decoration: underline; }
.page :deep(.mlxmf:hover) { color: #a6e3c2; }
.page :deep(.mfield) {
  background: #141414; border: 1px solid rgba(255,255,255,0.2); color: #ccc;
  border-radius: 4px; padding: 1px 5px; font-size: 0.92em; margin: 0 2px;
}
/* font +/- buttons in the titlebar (matches Log/CLI zoom controls). */
.rfs-btn {
  display: inline-flex; align-items: center; justify-content: center;
  width: 18px; height: 18px; border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
  user-select: none;
}
.rfs-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }
</style>
