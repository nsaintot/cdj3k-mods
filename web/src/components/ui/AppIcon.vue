<script setup lang="ts">
/*
 * The site's icons, as one 20x20 stroke set.
 *
 * Inline rather than a sprite sheet or an icon package: there are ten, they
 * share a grid and a stroke width, and inline icons inherit colour and never
 * load late.
 */
const paths = {
  search: 'M9 3a6 6 0 1 0 0 12A6 6 0 0 0 9 3ZM13.5 13.5 17.5 17.5',
  close: 'M5 5 15 15M15 5 5 15',
  menu: 'M3 6h14M3 10h14M3 14h14',
  sun: 'M10 4V2M10 18v-2M16 10h2M2 10h2M14.2 5.8l1.4-1.4M4.4 15.6l1.4-1.4M14.2 14.2l1.4 1.4M4.4 4.4l1.4 1.4M10 6.5a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z',
  moon: 'M16 11.2A6.5 6.5 0 0 1 8.8 4a6.5 6.5 0 1 0 7.2 7.2Z',
  arrowRight: 'M4 10h12M11 5l5 5-5 5',
  arrowLeft: 'M16 10H4M9 5l-5 5 5 5',
  hash: 'M7 3 5.5 17M14.5 3 13 17M3.5 7h13M3 13h13',
  book: 'M4 4.5A1.5 1.5 0 0 1 5.5 3H16v14H5.5A1.5 1.5 0 0 1 4 15.5v-11ZM4 14.5h12',
  chevron: 'M7.5 4.5 13 10l-5.5 5.5',
} as const

export type IconName = keyof typeof paths

withDefaults(
  defineProps<{
    name: IconName
    size?: number
  }>(),
  { size: 20 },
)

const icons = paths
</script>

<template>
  <svg
    class="icon"
    :width="size"
    :height="size"
    viewBox="0 0 20 20"
    fill="none"
    stroke="currentColor"
    stroke-width="1.5"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
  >
    <path :d="icons[name]" />
  </svg>
</template>

<style scoped lang="scss">
.icon {
  flex: none;
}
</style>
