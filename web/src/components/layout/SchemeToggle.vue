<script setup lang="ts">
import { computed } from 'vue'
import { useScheme } from '@/lib/useScheme'
import AppIcon from '@/components/ui/AppIcon.vue'

const { scheme, toggle } = useScheme()

/* The icon shows what the button will do, not the current scheme. `auto` has
 * no icon of its own; it resolves to one of the two elsewhere. */
const label = computed(() =>
  scheme.value === 'light' ? 'Switch to dark' : scheme.value === 'dark' ? 'Switch to light' : 'Switch colour scheme',
)
</script>

<template>
  <button type="button" class="toggle" :aria-label="label" :title="label" @click="toggle">
    <AppIcon class="toggle__icon toggle__icon--sun" name="sun" :size="18" />
    <AppIcon class="toggle__icon toggle__icon--moon" name="moon" :size="18" />
  </button>
</template>

<style scoped lang="scss">
.toggle {
  display: grid;
  place-items: center;
  width: 2rem;
  height: 2rem;
  border-radius: var(--radius-md);
  color: var(--c-text-muted);
  transition: color var(--duration-fast) var(--ease-out), background var(--duration-fast) var(--ease-out);

  @include focus-ring(1px);

  &:hover {
    color: var(--c-text);
    background: var(--c-surface-raised);
  }

  /* Only one of the pair is ever drawn, and which one follows the same
   * cascade the tokens use: dark by default, light by query or by choice. */
  &__icon {
    grid-area: 1 / 1;
  }

  &__icon--sun {
    display: block;
  }
  &__icon--moon {
    display: none;
  }
}

@media (prefers-color-scheme: light) {
  :root:not([data-scheme='dark']) .toggle {
    .toggle__icon--sun {
      display: none;
    }
    .toggle__icon--moon {
      display: block;
    }
  }
}

:root[data-scheme='light'] .toggle {
  .toggle__icon--sun {
    display: none;
  }
  .toggle__icon--moon {
    display: block;
  }
}
</style>
