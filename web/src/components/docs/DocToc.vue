<script setup lang="ts">
/*
 * On this page.
 *
 * The current entry comes from useActiveHeading (an IntersectionObserver);
 * nothing here reads scroll offset or moves with the page. The only state is
 * which link is marked.
 */
import { computed, toRef, type Ref } from 'vue'
import type { DocHeading } from '@/types/docs'
import { useActiveHeading } from '@/lib/useActiveHeading'

const props = defineProps<{
  headings: DocHeading[]
  container: HTMLElement | null
}>()

const ids = computed(() => props.headings.map((h) => h.id))
const container = toRef(props, 'container') as Ref<HTMLElement | null>

const { active } = useActiveHeading(container, ids)
</script>

<template>
  <nav v-if="headings.length > 1" class="toc" aria-label="On this page">
    <p class="toc__heading">On this page</p>
    <ul class="toc__list">
      <li v-for="heading in headings" :key="heading.id">
        <a
          class="toc__link"
          :class="[`toc__link--h${heading.level}`, { 'toc__link--active': active === heading.id }]"
          :href="`#${heading.id}`"
          :aria-current="active === heading.id ? 'true' : undefined"
        >
          {{ heading.text }}
        </a>
      </li>
    </ul>
  </nav>
</template>

<style scoped lang="scss">
.toc {
  &__heading {
    @include eyebrow;
    margin-bottom: var(--space-3);
  }

  &__list {
    list-style: none;
    margin: 0;
    padding: 0;
    border-inline-start: var(--border-hair) solid var(--c-border);
  }

  &__link {
    display: block;
    padding: var(--space-1) var(--space-3);
    margin-inline-start: -1px;
    border-inline-start: 2px solid transparent;
    color: var(--c-text-faint);
    font-size: var(--text-xs);
    line-height: var(--leading-snug);
    text-decoration: none;
    transition: color var(--duration-fast) var(--ease-out);

    @include focus-ring(-2px);

    &:hover {
      color: var(--c-text);
    }

    &--h3 {
      padding-inline-start: var(--space-5);
    }

    &--active {
      color: var(--c-accent);
      border-inline-start-color: var(--c-accent);
    }
  }
}
</style>
