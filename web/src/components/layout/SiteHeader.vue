<script setup lang="ts">
import { ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { site } from '@/content/site'
import SiteNav from './SiteNav.vue'
import SchemeToggle from './SchemeToggle.vue'
import SearchTrigger from '@/components/search/SearchTrigger.vue'
import AppIcon from '@/components/ui/AppIcon.vue'

defineEmits<{ openSearch: [] }>()

const route = useRoute()
const menuOpen = ref(false)

/* Only navigating closes it. There is no overlay to click, because the panel
 * pushes the page down instead of covering it. */
watch(() => route.fullPath, () => (menuOpen.value = false))
</script>

<template>
  <header class="header">
    <div class="header__bar">
      <RouterLink class="header__brand" to="/" :aria-label="site.name">
        <span class="header__name" aria-hidden="true">CDJ3K-Mods</span>
      </RouterLink>

      <div class="header__desktop">
        <SiteNav />
      </div>

      <div class="header__actions">
        <SearchTrigger @click="$emit('openSearch')" />
        <SchemeToggle />
        <a
          class="header__repo"
          :href="site.repo"
          target="_blank"
          rel="noopener noreferrer"
          aria-label="Source on GitHub"
          title="Source on GitHub"
        >
          <svg width="18" height="18" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path
              d="M8 0C3.58 0 0 3.58 0 8a8 8 0 0 0 5.47 7.59c.4.07.55-.17.55-.38l-.01-1.49c-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82a7.4 7.4 0 0 1 4 0c1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48l-.01 2.19c0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8Z"
            />
          </svg>
        </a>

        <button
          type="button"
          class="header__burger"
          :aria-expanded="menuOpen"
          aria-controls="header-mobile-nav"
          :aria-label="menuOpen ? 'Close menu' : 'Open menu'"
          @click="menuOpen = !menuOpen"
        >
          <AppIcon :name="menuOpen ? 'close' : 'menu'" :size="18" />
        </button>
      </div>
    </div>

    <div v-show="menuOpen" id="header-mobile-nav" class="header__mobile">
      <SiteNav />
    </div>
  </header>
</template>

<style scoped lang="scss">
.header {
  position: sticky;
  top: 0;
  z-index: var(--z-header);
  background: color-mix(in srgb, var(--c-bg) 88%, transparent);
  backdrop-filter: blur(10px);
  border-bottom: var(--border-hair) solid var(--c-border);

  &__bar {
    @include container(var(--width-wide));
    display: flex;
    align-items: center;
    gap: var(--space-4);
    height: var(--header-height);
  }

  &__brand {
    display: flex;
    align-items: center;
    gap: var(--space-2);
    color: var(--c-text);
    text-decoration: none;
    font-weight: var(--weight-semibold);
    letter-spacing: var(--tracking-tight);
    @include focus-ring(2px);
  }

  &__mark {
    display: grid;
    place-items: center;
    height: 1.5rem;
    padding-inline: var(--space-2);
    border-radius: var(--radius-sm);
    background: var(--c-accent);
    color: var(--c-accent-contrast);
    font-size: var(--text-2xs);
    font-weight: var(--weight-bold);
    letter-spacing: 0;
  }

  &__name {
    font-size: var(--text-base);
  }

  &__desktop {
    display: none;

    @include from('md') {
      display: block;
    }
  }

  &__actions {
    display: flex;
    align-items: center;
    gap: var(--space-1);
    margin-inline-start: auto;
  }

  &__repo {
    display: grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: var(--radius-md);
    color: var(--c-text-muted);
    transition:
      color var(--duration-fast) var(--ease-out),
      background var(--duration-fast) var(--ease-out);
    @include focus-ring(1px);

    &:hover {
      color: var(--c-text);
      background: var(--c-surface-raised);
    }
  }

  &__burger {
    display: grid;
    place-items: center;
    width: 2rem;
    height: 2rem;
    border-radius: var(--radius-md);
    color: var(--c-text-muted);
    @include focus-ring(1px);

    &:hover {
      color: var(--c-text);
      background: var(--c-surface-raised);
    }

    @include from('md') {
      display: none;
    }
  }

  &__mobile {
    border-top: var(--border-hair) solid var(--c-border);
    padding: var(--space-3) var(--space-4);
    background: var(--c-surface);

    @include from('md') {
      display: none;
    }

    :deep(.nav) {
      flex-direction: column;
      align-items: stretch;
      gap: var(--space-1);
    }
  }
}
</style>
