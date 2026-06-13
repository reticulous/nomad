<!-- Settings → Nomad Network → Browser. Config knobs + bookmark admin.
     The actual browsing happens in the Nomad Browser floating window
     (Status menu); this panel manages bookmarks and the announce cap. -->
<template>
  <div class="q-gutter-y-md">
    <PanelHeading title="Nomad Network" />

    <div class="text-caption text-grey-5">
      Browse Nomad Network pages (the "text web" over Reticulum). Open the
      browser from the <b>Nomad Browser</b> menu, or:
    </div>
    <button class="open-btn" @click="openBrowser">Open Nomad Browser</button>

    <q-separator dark />

    <SettingSlider label="Announce-drift cap (nodes)" k="s.nomad.max_nodes"
                   :min="32" :max="2048" :step="32" />
    <div class="text-caption text-grey-5">
      Heard nodes kept in the drift list; oldest evicted past the cap.
    </div>

    <q-separator dark />

    <div class="text-caption text-grey-5">Bookmarks</div>
    <div v-if="nomad.bookmarks.value.length === 0" class="none">
      No bookmarks yet. Bookmark a node from the browser window, or add one below.
    </div>
    <div v-for="b in nomad.bookmarks.value" :key="b.id" class="bm">
      <div class="bm-info">
        <div class="bm-name">{{ b.name || '(unnamed)' }}</div>
        <div class="bm-hash">{{ b.hash }}:{{ b.path }}</div>
        <div v-if="b.note" class="bm-note">{{ b.note }}</div>
      </div>
      <button class="del" @click="nomad.delBookmark(b.id)">Remove</button>
    </div>

    <div class="add">
      <input v-model="addHash" class="in mono" placeholder="32-hex hash[:/page/index.mu]" />
      <input v-model="addName" class="in" placeholder="name (optional)" />
      <input v-model="addNote" class="in" placeholder="note (optional)" />
      <button class="add-btn" :disabled="!validHash" @click="add">Add bookmark</button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, computed } from 'vue'
import { useNomad, nomadVisible } from '../modules/nomad'

const nomad = useNomad()

const addHash = ref('')
const addName = ref('')
const addNote = ref('')

const validHash = computed(() => /^[0-9a-fA-F]{32}(:.*)?$/.test(addHash.value.trim()))

function add() {
  if (!validHash.value) return
  const v = addHash.value.trim()
  const hash = v.slice(0, 32).toLowerCase()
  const path = v.length > 33 && v[32] === ':' ? v.slice(33) : ''
  nomad.addBookmark(hash, path, addName.value.trim(), addNote.value.trim())
  addHash.value = ''; addName.value = ''; addNote.value = ''
}

function openBrowser() { nomadVisible.value = true }
</script>

<style scoped>
.open-btn, .add-btn, .del {
  background: #2a2a2a;
  border: 1px solid rgba(255, 255, 255, 0.15);
  color: #ddd;
  border-radius: 5px;
  padding: 6px 12px;
  cursor: pointer;
  font-size: 13px;
}
.open-btn:hover, .add-btn:hover, .del:hover { background: #353535; }
.add-btn:disabled { opacity: 0.5; cursor: not-allowed; }
.none { color: #888; font-style: italic; font-size: 13px; }
.bm {
  display: flex; align-items: center; justify-content: space-between;
  gap: 8px; padding: 8px; border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 6px; background: #1c1c1c;
}
.bm-info { min-width: 0; }
.bm-name { color: #e8e8e8; font-weight: 500; }
.bm-hash {
  font-family: 'JetBrains Mono', 'Menlo', monospace; font-size: 11px;
  color: #999; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
}
.bm-note { color: #aaa; font-size: 12px; }
.del { color: #e89; border-color: rgba(255, 120, 140, 0.3); }
.add { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.in {
  background: #161616; border: 1px solid rgba(255, 255, 255, 0.15);
  color: #ddd; border-radius: 5px; padding: 6px 8px; font-size: 13px; flex: 1 1 120px;
}
.in.mono { font-family: 'JetBrains Mono', 'Menlo', monospace; flex: 2 1 240px; }
</style>
