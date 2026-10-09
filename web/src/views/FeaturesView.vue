<script setup lang="ts">
/*
 * The feature list.
 *
 * Everything comes from content/features.ts and every card ends in a link into
 * the documentation. Usage is described only in the markdown.
 */
import { featureGroups, allFeatures } from '@/content/features'
import PageIntro from '@/components/ui/PageIntro.vue'
import FeatureGroupSection from '@/components/features/FeatureGroupSection.vue'

/* Whether the partial note is shown, not a count: a tally would need
 * maintaining and adds nothing. */
const hasPartial = allFeatures.some((f) => f.status === 'partial')
</script>

<template>
  <div class="features">
    <PageIntro eyebrow="What the mods do" title="Features">
      <p>
        Everything the mods adds.
        <template v-if="hasPartial">
          Anything marked <em>partial</em> is built and usable, with a known limitation.
        </template>
      </p>
    </PageIntro>

    <div class="features__groups">
      <FeatureGroupSection v-for="group in featureGroups" :key="group.id" :group="group" />
    </div>
  </div>
</template>

<style scoped lang="scss">
.features {
  @include container(var(--width-wide));
  padding-block: var(--space-7) var(--space-8);

  &__groups {
    display: flex;
    flex-direction: column;
    gap: var(--space-9);
    margin-top: var(--space-8);
  }
}
</style>
