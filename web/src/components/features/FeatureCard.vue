<script setup lang="ts">
/*
 * One feature.
 *
 * Says what it does and where it is switched on, then links to the
 * documentation, the one place that describes how to use it.
 */
import type { Feature } from '@/content/features'
import StatusTag from '@/components/ui/StatusTag.vue'
import AppIcon from '@/components/ui/AppIcon.vue'

defineProps<{ feature: Feature }>()

function href(link: Feature['links'][number]) {
  return `/docs/${link.doc}${link.hash ? '#' + link.hash : ''}`
}
</script>

<template>
  <article :id="feature.id" class="feature">
    <header class="feature__head">
      <h3 class="feature__name">{{ feature.name }}</h3>
      <StatusTag :status="feature.status" />
    </header>

    <p class="feature__summary">{{ feature.summary }}</p>

    <dl v-if="feature.setting || feature.scope" class="feature__meta">
      <template v-if="feature.setting">
        <dt>Setting</dt>
        <dd>
          <code>{{ feature.setting.row }}</code>
          <span class="feature__default">default: {{ feature.setting.default }}</span>
        </dd>
      </template>
      <template v-if="feature.scope">
        <dt>Applies</dt>
        <dd>{{ feature.scope }}</dd>
      </template>
    </dl>

    <footer class="feature__links">
      <RouterLink v-for="link in feature.links" :key="href(link)" class="feature__link" :to="href(link)">
        {{ link.label }}
        <AppIcon name="arrowRight" :size="14" />
      </RouterLink>
    </footer>
  </article>
</template>

<style scoped lang="scss">
.feature {
  @include card;
  display: flex;
  flex-direction: column;
  gap: var(--space-3);
  padding: var(--space-5);
  scroll-margin-top: calc(var(--header-height) + var(--space-5));

  &__head {
    display: flex;
    align-items: center;
    gap: var(--space-3);
  }

  &__name {
    font-size: var(--text-lg);
    letter-spacing: var(--tracking-normal);
  }

  &__summary {
    color: var(--c-text);
    line-height: var(--leading-relaxed);
  }

  &__meta {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: var(--space-1) var(--space-4);
    margin: 0;
    padding-top: var(--space-3);
    border-top: var(--border-hair) solid var(--c-border);
    font-size: var(--text-base);

    dt {
      @include eyebrow;
      padding-top: 0.2em;
    }

    dd {
      margin: 0;
      color: var(--c-text-muted);
      display: flex;
      align-items: baseline;
      gap: var(--space-2);
      flex-wrap: wrap;
    }

    code {
      color: var(--c-text);
      font-size: var(--text-sm);
      letter-spacing: var(--tracking-wide);
    }
  }

  &__default {
    color: var(--c-text-faint);
    font-size: var(--text-xs);
  }

  &__links {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-4);
    margin-top: auto;
    padding-top: var(--space-2);
  }

  &__link {
    display: inline-flex;
    align-items: center;
    gap: var(--space-1);
    font-size: var(--text-base);
    text-decoration: none;
    @include focus-ring(2px);

    &:hover {
      text-decoration: underline;
    }
  }
}
</style>
