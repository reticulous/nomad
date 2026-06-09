<!-- Nomad Browser menu. The Nomad Network page browser: address bar +
     back, a two-section sidebar (bookmarks on top, announce-drift below),
     and a page view rendering Micron via the TS renderer. History is
     frontend-owned (the firmware is stateless re history). -->
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
        <!-- address bar -->
        <div class="bar">
          <button class="nav" :disabled="histPos <= 0" title="Back" @click="back">‹</button>
          <button class="nav" title="Reload" :disabled="!nomad.navHash.value" @click="nomad.reload()">⟳</button>
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
          <!-- sidebar -->
          <div class="side">
            <input
              v-model="sideQ" class="side-search"
              placeholder="Search nodes" autocomplete="off"
              @keydown.esc="sideQ = ''"
            />

            <div class="side-h">Bookmarks</div>
            <div v-if="filteredBookmarks.length === 0" class="side-empty">{{ sideQ.trim() ? 'no matches' : 'none' }}</div>
            <div
              v-for="b in filteredBookmarks" :key="b.hash"
              class="item" :class="{ active: b.hash === curHash }"
              :title="b.hash" @click="navigate(b.hash, DEFAULT_PAGE)"
            >
              <div class="item-name">{{ b.name || shortHash(b.hash) }}</div>
              <div v-if="b.note" class="item-sub">{{ b.note }}</div>
            </div>

            <div class="side-h">On the Mesh</div>
            <div v-if="filteredNodes.length === 0" class="side-empty">{{ sideQ.trim() ? 'no matches' : 'none heard yet' }}</div>
            <div
              v-for="n in filteredNodes" :key="n.hash"
              class="item" :class="{ active: n.hash === curHash }"
              :title="n.hash" @click="navigate(n.hash, DEFAULT_PAGE)"
            >
              <div class="item-name">{{ n.name || shortHash(n.hash) }}</div>
              <div class="item-sub">{{ n.hops }} hops · {{ age(n.lastSeen) }}</div>
            </div>
          </div>

          <!-- page view -->
          <div class="view">
            <div class="status" :class="statusClass">
              <span class="dot" />{{ statusText }}
            </div>
            <div v-if="showError" class="page-msg err">
              Could not load the page.<span v-if="nomad.navError.value"> ({{ nomad.navError.value }})</span>
            </div>
            <div v-else-if="nomad.page.value.truncated" class="page-msg">
              Page is {{ nomad.page.value.size }} bytes — too large to display here.
            </div>
            <div v-else-if="!nomad.page.value.body" class="page-msg dim">
              Pick a bookmark or node, or enter an address above.
            </div>
            <div v-else ref="pageEl" class="page" v-html="pageHtml" @click="onPageClick" />
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, computed, watch } from 'vue'
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue'
import { useNomad, DEFAULT_PAGE } from '../modules/nomad'
import { micronToHtml } from '../lib/micron'
import { useWinZoom } from 'rns/lib/winZoom'

const { scale, zoomIn, zoomOut } = useWinZoom('nomad')

defineProps<{ visible: boolean; title: string }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const defaultGeom = { x: 14, y: 8, w: 70, h: 80 }
const HASH_RE = /^[0-9a-fA-F]{32}$/

const nomad = useNomad()

const address = ref('')
const pageEl = ref<HTMLElement | null>(null)

/* Sidebar filter — name or hash substring, over both sections. */
const sideQ = ref('')
const filteredBookmarks = computed(() => {
  const needle = sideQ.value.trim().toLowerCase()
  if (!needle) return nomad.bookmarks.value
  return nomad.bookmarks.value.filter(b =>
    b.name.toLowerCase().includes(needle) || b.hash.toLowerCase().includes(needle))
})
const filteredNodes = computed(() => {
  const needle = sideQ.value.trim().toLowerCase()
  if (!needle) return nomad.nodes.value
  return nomad.nodes.value.filter(n =>
    n.name.toLowerCase().includes(needle) || n.hash.toLowerCase().includes(needle))
})

/* Frontend-owned history. Each navigate pushes; back replays. */
const history = ref<{ hash: string; path: string }[]>([])
const histPos = ref(-1)

const curHash = computed(() => nomad.navHash.value)
const curBookmarked = computed(() => !!curHash.value && nomad.isBookmarked(curHash.value))
const pageHtml = computed(() => micronToHtml(nomad.page.value.body))

const statusText = computed(() => {
  switch (nomad.navStatus.value) {
    case 'path_requested': return 'Requesting path…'
    case 'establishing':   return 'Establishing link…'
    case 'requesting':     return 'Requesting page…'
    case 'done':           return `${nomad.navPath.value || ''} · ${nomad.page.value.size} B`
    case 'failed':         return 'Failed'
    case 'timeout':        return 'Timed out'
    default:               return 'Ready'
  }
})
const statusClass = computed(() => ({
  busy: nomad.busy.value,
  ok: nomad.navStatus.value === 'done',
  bad: nomad.navStatus.value === 'failed' || nomad.navStatus.value === 'timeout',
}))
const showError = computed(() =>
  (nomad.navStatus.value === 'failed' || nomad.navStatus.value === 'timeout')
  && !nomad.page.value.body)

function shortHash(h: string): string { return h.slice(0, 8) + '…' }

function age(epochS: number): string {
  if (!epochS) return '—'
  const s = Math.round(Date.now() / 1000 - epochS)
  if (s < 60) return `${s}s`
  if (s < 3600) return `${Math.floor(s / 60)}m`
  if (s < 86400) return `${Math.floor(s / 3600)}h`
  return `${Math.floor(s / 86400)}d`
}

/* Single navigation entry: records history, then drives the firmware. */
function navigate(hash: string, path: string, push = true) {
  const h = hash.trim().toLowerCase()
  if (!HASH_RE.test(h)) return
  const p = path || DEFAULT_PAGE
  if (push) {
    history.value = history.value.slice(0, histPos.value + 1)
    history.value.push({ hash: h, path: p })
    histPos.value = history.value.length - 1
  }
  nomad.go(h, p)
}

function back() {
  if (histPos.value <= 0) return
  histPos.value -= 1
  const e = history.value[histPos.value]!
  nomad.go(e.hash, e.path)
}

function goAddress() {
  const u = address.value.trim()
  if (!u) return
  const colon = u.indexOf(':')
  if (colon === 32 && HASH_RE.test(u.slice(0, 32))) {
    navigate(u.slice(0, 32), u.slice(colon + 1) || DEFAULT_PAGE)
  } else if (HASH_RE.test(u)) {
    navigate(u, DEFAULT_PAGE)
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

/* Follow a Micron link, recording history. No fields/vars → a GET; else
 * gather the named `.mfield` values (`*` = all) + var literals and submit. */
function followTarget(target: string) {
  if (!target.trim()) return
  const { url, fields, vars } = parseTarget(target)
  const r = resolveUrl(url)
  if (!r) return

  const isForm = fields.length > 0 || Object.keys(vars).length > 0
  if (!isForm) { navigate(r.hash, r.path); return }

  const data: Record<string, string> = {}
  const wantAll = fields.includes('*')
  pageEl.value?.querySelectorAll<HTMLInputElement>('input.mfield').forEach(inp => {
    const name = inp.getAttribute('data-fname') || ''
    if (name && (wantAll || fields.includes(name))) data[`field_${name}`] = inp.value
  })
  for (const [k, v] of Object.entries(vars)) data[`var_${k}`] = v

  history.value = history.value.slice(0, histPos.value + 1)
  history.value.push({ hash: r.hash, path: r.path })
  histPos.value = history.value.length - 1
  nomad.submit(r.hash, r.path, data)
}

function onPageClick(ev: MouseEvent) {
  const el = (ev.target as HTMLElement)?.closest('a.mlink') as HTMLElement | null
  if (!el) return
  ev.preventDefault()
  const target = el.getAttribute('data-mtarget')
  if (target) followTarget(target)
}

function toggleBookmark() {
  if (!curHash.value) return
  if (curBookmarked.value) nomad.delBookmark(curHash.value)
  else {
    const node = nomad.nodes.value.find(n => n.hash === curHash.value)
    nomad.addBookmark(curHash.value, node?.name ?? '', '')
  }
}

/* Keep the address bar mirroring the active page. */
watch(() => [nomad.navHash.value, nomad.navPath.value], () => {
  if (nomad.navHash.value) {
    address.value = `${nomad.navHash.value}:${nomad.navPath.value || DEFAULT_PAGE}`
  }
})
</script>

<style scoped>
.nomad { display: flex; flex-direction: column; height: 100%; color: #d8d8d8; }
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
.nav.go { font-size: 12px; }
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
.view { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.status {
  display: flex; align-items: center; gap: 6px; font-size: calc(12px * var(--rfs, 1)); color: #aaa;
  padding: 5px 10px; border-bottom: 1px solid rgba(255,255,255,0.06); background: #1a1a1a;
}
.status .dot { width: 8px; height: 8px; border-radius: 50%; background: #666; }
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
.page :deep(.mh) { margin: 0.6em 0 0.3em; color: #fff; line-height: 1.2; }
.page :deep(h1.mh) { font-size: 1.5em; }
.page :deep(h2.mh) { font-size: 1.25em; }
.page :deep(h3.mh) { font-size: 1.1em; }
.page :deep(.mdivider) { border: 0; border-top: 1px solid rgba(255,255,255,0.18); margin: 0.7em 0; }
.page :deep(.mliteral) {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 0.9em;
  background: #141414; border: 1px solid rgba(255,255,255,0.08); border-radius: 5px;
  padding: 8px 10px; white-space: pre; overflow-x: auto;
}
.page :deep(.mlink) { color: #6db3ff; cursor: pointer; text-decoration: underline; }
.page :deep(.mlink:hover) { color: #9ccbff; }
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
