<script setup lang="ts">
/*
 * The rendered document.
 *
 * The HTML is produced at build time. This component turns the links the
 * build marked `data-internal` into router navigations instead of full page
 * loads.
 *
 * One delegated listener on the container, because the anchors live inside
 * v-html and are replaced whole each time the document changes.
 */
import { useRouter } from 'vue-router'

defineProps<{ html: string }>()

const router = useRouter()

function onClick(event: MouseEvent) {
  // Leave modified clicks alone: they mean "open elsewhere".
  if (event.defaultPrevented || event.button !== 0) return
  if (event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return

  const anchor = (event.target as HTMLElement).closest('a')
  if (!anchor) return

  /* Cross-document link. `href` is the deployed URL so the browser's own
   * affordances work; `data-internal` is the route to hand the router. */
  const internal = anchor.getAttribute('data-internal')
  if (internal) {
    event.preventDefault()
    router.push(internal)
    return
  }

  /* An in-document anchor: let the router record it so the address bar and the
   * back button agree with where the page went. */
  const href = anchor.getAttribute('href')
  if (href?.startsWith('#')) {
    event.preventDefault()
    router.push({ hash: href })
  }
}
</script>

<template>
  <!-- eslint-disable-next-line vue/no-v-html -->
  <article class="doc prose" @click="onClick" v-html="html" />
</template>

<style scoped lang="scss">
.doc {
  /* The prose class carries the typography; this only owns the box. */
  min-width: 0;
}
</style>
