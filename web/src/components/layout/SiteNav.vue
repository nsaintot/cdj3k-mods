<script setup lang="ts">
/*
 * The primary navigation.
 *
 * Every entry is a route, not an anchor into the current page, so the active
 * entry always says where you are.
 *
 * Active state is computed rather than left to RouterLink's classes: `/` has to
 * match exactly (or it is active everywhere) while `/docs` has to match its
 * whole subtree (or it goes out the moment you open a document).
 */
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import { nav } from '@/content/site'

const route = useRoute()

const items = computed(() =>
  nav.map((item) => ({
    ...item,
    active: item.to === '/' ? route.path === '/' : route.path.startsWith(item.to),
  })),
)
</script>

<template>
  <nav class="nav" aria-label="Primary">
    <RouterLink
      v-for="item in items"
      :key="item.to"
      class="nav__link"
      :class="{ 'nav__link--active': item.active }"
      :to="item.to"
      :aria-current="item.active ? 'page' : undefined"
    >
      {{ item.label }}
    </RouterLink>
  </nav>
</template>

<style scoped lang="scss">
.nav {
  display: flex;
  align-items: center;
  gap: var(--space-1);

  &__link {
    padding: var(--space-1) var(--space-3);
    border-radius: var(--radius-md);
    color: var(--c-text-muted);
    font-size: var(--text-base);
    text-decoration: none;
    white-space: nowrap;
    transition:
      color var(--duration-fast) var(--ease-out),
      background var(--duration-fast) var(--ease-out);

    @include focus-ring(1px);

    &:hover {
      color: var(--c-text);
      background: var(--c-surface-raised);
    }

    &--active {
      color: var(--c-text);
      background: var(--c-surface-raised);
    }
  }
}
</style>
