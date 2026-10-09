<script setup lang="ts">
/*
 * The search dialog.
 *
 * A native <dialog> opened with showModal(), which provides the focus trap,
 * the inert background and Escape.
 *
 * The list is driven by the keyboard first: the input keeps focus throughout
 * and the arrow keys move a selection that the results only render, so typing
 * and choosing never fight over where focus is.
 */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { loadIndex, search } from '@/lib/search'
import SearchResultItem from './SearchResultItem.vue'
import AppIcon from '@/components/ui/AppIcon.vue'

const open = defineModel<boolean>('open', { required: true })

const router = useRouter()
const dialog = ref<HTMLDialogElement | null>(null)
const input = ref<HTMLInputElement | null>(null)
const list = ref<HTMLElement | null>(null)

const query = ref('')
const selected = ref(0)
/* The index is a chunk of its own; until it lands there is nothing to match
 * against, and `ready` is what re-runs the search once it has. */
const ready = ref(false)

const results = computed(() => (ready.value ? search(query.value) : []))

watch(query, () => (selected.value = 0))

watch(open, async (isOpen) => {
  const el = dialog.value
  if (!el) return

  if (isOpen) {
    loadIndex().then(() => (ready.value = true))
    el.showModal()
    await nextTick()
    input.value?.focus()
  } else if (el.open) {
    el.close()
  }
})

/* Escape and the backdrop both fire the dialog's own close event, so this is
 * the single place the model is put back. */
function onClose() {
  open.value = false
  query.value = ''
  selected.value = 0
}

/* showModal() makes the backdrop the dialog itself, so a click that landed on
 * the element rather than on its content is a click outside the panel. */
function onDialogClick(event: MouseEvent) {
  if (event.target === dialog.value) open.value = false
}

function move(delta: number) {
  const n = results.value.length
  if (!n) return
  selected.value = (selected.value + delta + n) % n
  scrollSelectedIntoView()
}

async function scrollSelectedIntoView() {
  await nextTick()
  list.value
    ?.querySelectorAll('.hit')
    [selected.value]?.scrollIntoView({ block: 'nearest' })
}

function go() {
  const hit = results.value[selected.value]
  if (!hit) return
  open.value = false
  router.push(hit.record.to)
}

function onKeydown(event: KeyboardEvent) {
  switch (event.key) {
    case 'ArrowDown':
      event.preventDefault()
      move(1)
      break
    case 'ArrowUp':
      event.preventDefault()
      move(-1)
      break
    case 'Enter':
      event.preventDefault()
      go()
      break
  }
}

/* The global shortcut. Registered here because this component owns the state
 * it toggles, and removed with it. */
function onWindowKeydown(event: KeyboardEvent) {
  if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === 'k') {
    event.preventDefault()
    open.value = !open.value
  }
}

onMounted(() => window.addEventListener('keydown', onWindowKeydown))
onBeforeUnmount(() => window.removeEventListener('keydown', onWindowKeydown))
</script>

<template>
  <dialog ref="dialog" class="dialog" @close="onClose" @click="onDialogClick">
    <div class="dialog__panel">
      <div class="dialog__field">
        <AppIcon name="search" :size="18" class="dialog__icon" />
        <input
          ref="input"
          v-model="query"
          class="dialog__input"
          type="search"
          placeholder="Search the docs and features"
          aria-label="Search"
          autocomplete="off"
          spellcheck="false"
          @keydown="onKeydown"
        />
        <button type="button" class="dialog__close" aria-label="Close" @click="open = false">
          <AppIcon name="close" :size="16" />
        </button>
      </div>

      <div ref="list" class="dialog__results">
        <p v-if="!query" class="dialog__hint">
          Search runs over every section of the documentation, and over the feature list.
        </p>
        <p v-else-if="!ready" class="dialog__hint">Loading the index…</p>
        <p v-else-if="!results.length" class="dialog__hint">
          Nothing matches <strong>{{ query }}</strong
          >. Every word has to appear in the same section.
        </p>
        <ul v-else class="dialog__hits">
          <SearchResultItem
            v-for="(result, i) in results"
            :key="result.record.to + i"
            :result="result"
            :selected="i === selected"
            @mouseenter="selected = i"
            @click="open = false"
          />
        </ul>
      </div>

      <footer v-if="results.length" class="dialog__foot">
        <span>{{ results.length }} {{ results.length === 1 ? 'result' : 'results' }}</span>
        <span class="dialog__keys">↑↓ to move · ↵ to open · esc to close</span>
      </footer>
    </div>
  </dialog>
</template>

<style scoped lang="scss">
.dialog {
  /* Under showModal() the element is the backdrop area; the panel inside it
   * has the surface. */
  width: min(38rem, calc(100vw - var(--space-4) * 2));
  max-height: min(32rem, calc(100dvh - var(--space-8)));
  margin: 0;
  padding: 0;
  border: none;
  background: none;
  color: inherit;
  position: fixed;
  inset: 0;
  margin-inline: auto;
  margin-top: min(12vh, 6rem);

  &::backdrop {
    background: var(--c-overlay);
    backdrop-filter: blur(2px);
  }

  &__panel {
    display: flex;
    flex-direction: column;
    max-height: inherit;
    background: var(--c-surface);
    border: var(--border-hair) solid var(--c-border-strong);
    border-radius: var(--radius-xl);
    box-shadow: var(--shadow-lg);
    overflow: hidden;
  }

  &__field {
    display: flex;
    align-items: center;
    gap: var(--space-3);
    padding: var(--space-3) var(--space-4);
    border-bottom: var(--border-hair) solid var(--c-border);
  }

  &__icon {
    color: var(--c-text-faint);
  }

  &__input {
    flex: 1;
    min-width: 0;
    border: none;
    background: none;
    outline: none;
    font-size: var(--text-lg);

    &::placeholder {
      color: var(--c-text-faint);
    }

    /* The type=search clear button is a second control doing what Escape does. */
    &::-webkit-search-cancel-button {
      display: none;
    }
  }

  &__close {
    display: grid;
    place-items: center;
    width: 1.75rem;
    height: 1.75rem;
    border-radius: var(--radius-sm);
    color: var(--c-text-faint);
    @include focus-ring(1px);

    &:hover {
      color: var(--c-text);
      background: var(--c-surface-raised);
    }
  }

  &__results {
    overflow-y: auto;
    padding: var(--space-2);
    @include thin-scrollbar;
  }

  &__hits {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  &__hint {
    padding: var(--space-5) var(--space-3);
    color: var(--c-text-faint);
    font-size: var(--text-base);
    text-align: center;
  }

  &__foot {
    display: flex;
    justify-content: space-between;
    gap: var(--space-3);
    padding: var(--space-2) var(--space-4);
    border-top: var(--border-hair) solid var(--c-border);
    background: var(--c-surface-raised);
    color: var(--c-text-faint);
    font-size: var(--text-xs);
  }

  &__keys {
    @include below('sm') {
      display: none;
    }
  }
}
</style>
