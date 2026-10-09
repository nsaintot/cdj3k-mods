<script setup lang="ts">
/*
 * A keyboard key. `mod` renders as the platform's modifier: ⌘ on Apple
 * devices, Ctrl elsewhere.
 */
import { computed } from 'vue'

const props = defineProps<{ keys: string[] }>()

const isApple = computed(
  () => typeof navigator !== 'undefined' && /Mac|iPhone|iPad/.test(navigator.platform),
)

const rendered = computed(() =>
  props.keys.map((k) => (k === 'mod' ? (isApple.value ? '⌘' : 'Ctrl') : k)),
)
</script>

<template>
  <span class="keys">
    <kbd v-for="key in rendered" :key="key" class="keys__key">{{ key }}</kbd>
  </span>
</template>

<style scoped lang="scss">
.keys {
  display: inline-flex;
  gap: 2px;

  &__key {
    min-width: 1.4em;
    padding: 0.05em 0.35em;
    border: var(--border-hair) solid var(--c-border-strong);
    border-radius: var(--radius-sm);
    background: var(--c-surface-raised);
    color: var(--c-text-faint);
    font-family: var(--font-sans);
    font-size: var(--text-2xs);
    line-height: 1.5;
    text-align: center;
  }
}
</style>
