<script setup lang="ts">
/*
 * The document list.
 *
 * Grouped by the reading order in content/docs.ts. Each entry shows its lead
 * sentence on the wide layout, since the titles alone do not always say which
 * document you want.
 */
import { docSections } from '@/content/docs'
</script>

<template>
  <nav class="docs-nav" aria-label="Documentation">
    <div v-for="section in docSections" :key="section.title" class="docs-nav__section">
      <p class="docs-nav__heading">{{ section.title }}</p>
      <ul class="docs-nav__list">
        <li v-for="doc in section.docs" :key="doc.slug">
          <RouterLink
            class="docs-nav__link"
            :to="`/docs/${doc.slug}`"
            active-class="docs-nav__link--active"
          >
            {{ doc.title }}
          </RouterLink>
        </li>
      </ul>
    </div>
  </nav>
</template>

<style scoped lang="scss">
.docs-nav {
  display: flex;
  flex-direction: column;
  gap: var(--space-5);

  &__heading {
    @include eyebrow;
    margin-bottom: var(--space-2);
  }

  &__list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 1px;
    /* The rule sits under the items rather than beside each one, so an active
     * item can cover it. */
    border-inline-start: var(--border-hair) solid var(--c-border);
  }

  &__link {
    display: block;
    padding: var(--space-2) var(--space-3);
    margin-inline-start: -1px;
    border-inline-start: 2px solid transparent;
    color: var(--c-text-muted);
    font-size: var(--text-base);
    text-decoration: none;
    transition: color var(--duration-fast) var(--ease-out);

    @include focus-ring(-2px);

    &:hover {
      color: var(--c-text);
    }

    &--active {
      color: var(--c-accent);
      border-inline-start-color: var(--c-accent);
      font-weight: var(--weight-medium);
    }
  }
}
</style>
