<script setup lang="ts">
/*
 * A screenshot at its own size.
 *
 * The screenshots are 1280x720 captures shown at column width, so a detail a
 * caption points at (a lit lamp, a dot in a corner) is smaller on the page
 * than on the deck. Clicking one opens it here.
 *
 * <dialog> rather than a div: showModal() provides the focus trap, the inert
 * background and Escape.
 */
import { nextTick, ref } from 'vue'

const el = ref<HTMLDialogElement | null>(null)
const src = ref('')
const alt = ref('')

/*
 * Opened on the next tick. showModal() called inside the click's dispatch puts
 * the dialog in the top layer while that click is still propagating, and the
 * browser then delivers the click to the dialog, which closes it immediately.
 * Waiting also lets Vue flush, so the figure exists before the dialog is shown.
 */
async function open(imageSrc: string, imageAlt: string) {
  src.value = imageSrc
  alt.value = imageAlt
  await nextTick()
  el.value?.showModal()
}

defineExpose({ open })
</script>

<template>
  <dialog
    ref="el"
    class="lightbox"
    aria-label="Screenshot, full size"
    @click="el?.close()"
    @close="src = ''"
  >
    <figure v-if="src" class="lightbox__figure">
      <img :src="src" :alt="alt" />
      <figcaption v-if="alt">{{ alt }}</figcaption>
    </figure>
  </dialog>
</template>

<style scoped lang="scss">
.lightbox {
  /* The dialog fills the viewport, and a click anywhere closes it. */
  width: 100%;
  max-width: 100%;
  height: 100%;
  max-height: 100%;
  margin: 0;
  padding: var(--space-5);
  border: 0;
  background: transparent;
  cursor: zoom-out;
  overflow: auto;

  &::backdrop {
    background: rgb(0 0 0 / 0.8);
  }

  &__figure {
    display: flex;
    flex-direction: column;
    gap: var(--space-3);
    align-items: center;
    justify-content: center;
    min-height: 100%;
    margin: 0;

    img {
      max-width: 100%;
      /* Room for the caption under it, so a 16:9 capture on a short window is
       * not cropped by its own description. */
      max-height: calc(100vh - 8rem);
      width: auto;
      height: auto;
      border-radius: var(--radius-md);
    }

    figcaption {
      max-width: 68ch;
      color: var(--c-text-muted);
      font-size: var(--text-xs);
      line-height: var(--leading-snug);
      text-align: center;
    }
  }
}
</style>
