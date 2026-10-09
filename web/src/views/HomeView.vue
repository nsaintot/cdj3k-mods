<script setup lang="ts">
/*
 * The overview.
 *
 * What the project is, what it runs on, and where to go. The four traits below
 * are one-line labels; anything longer belongs in the documentation.
 */
import { site } from '@/content/site'
import AppIcon from '@/components/ui/AppIcon.vue'

const img = (name: string) => `${import.meta.env.BASE_URL}img/${name}`

const facts = [
  { label: 'Device', value: site.device },
  { label: 'Firmware', value: site.firmware },
  { label: 'Licence', value: site.license },
]

const traits = [
  { label: 'Patch', line: 'Adds to the deck’s own application. Replaces nothing.' },
  { label: 'External', line: 'Stems separate on your own machine, not on the deck.' },
  { label: 'Switchable', line: 'Every feature has an off, and off runs stock code.' },
  { label: 'Reversible', line: 'A key combo at power-on and it is gone.' },
]

/* The shots that show the most for the least explaining. */
const shots = [
  {
    src: 'stems-row-unity.png',
    alt: 'The play screen with the STEMS row open, showing the drums, harmonics and vocals faders.',
    caption: 'Live stems on three faders',
    to: '/docs/stems',
  },
  {
    src: 'mod-settings-open.png',
    alt: 'The MOD SETTINGS list open on the deck’s DJ SETTING screen.',
    caption: 'Settings on the deck',
    to: '/docs/mod-settings',
  },
  {
    src: 'theme-comb.png',
    alt: 'One play screen sliced into vertical bands, each band drawn in a different theme.',
    caption: 'Theming',
    to: '/docs/themes',
  },
]
</script>

<template>
  <div class="home">
    <section class="home__intro">
      <h1 class="home__title">{{ site.name }}</h1>
      <p class="home__lead">
        A collection of modifications for the CDJ-3000: Hot cues, Stems, X-PAD, Theming, and missing features...
      </p>

      <div class="home__actions">
        <RouterLink class="home__cta home__cta--primary" to="/features">
          Features
          <AppIcon name="arrowRight" :size="16" />
        </RouterLink>
        <RouterLink class="home__cta" to="/docs/getting-started">
          Install
          <AppIcon name="arrowRight" :size="16" />
        </RouterLink>
      </div>
    </section>

    <!-- The same amber bar a `> [!WARNING]` gets in the documentation, so a caution
         reads the same wherever it appears. -->
    <aside class="home__notice">
      <p>
        <strong>This is unstable software.</strong> Expect bugs, freezes and
        crashes. Do not put it on a deck you are relying on.<br />
        Not the night of a gig, not on a club's unit, and not on the only player you own.
      </p>
    </aside>

    <!-- One shot per scheme. Which is drawn follows the same cascade the tokens
         use: dark by default, light by query or by choice. -->
    <figure class="home__hero">
      <img class="home__hero-shot home__hero-shot--dark" :src="img('overview.png')"
        alt="The CDJ-3000 play screen with the mods running: X-PAD and STEMS in the quick menu, drums, harmonics and vocals on their own faders, and GATE CUE."
        width="1280" height="720" decoding="async" />
      <img class="home__hero-shot home__hero-shot--light" :src="img('overview-light.png')" alt="" width="1280"
        height="720" decoding="async" />
    </figure>

    <dl class="home__facts">
      <div v-for="fact in facts" :key="fact.label" class="home__fact">
        <dt>{{ fact.label }}</dt>
        <dd>{{ fact.value }}</dd>
      </div>
    </dl>

    <ul class="home__traits">
      <li v-for="trait in traits" :key="trait.label" class="home__trait">
        <span class="home__trait-label">{{ trait.label }}</span>
        <span class="home__trait-line">{{ trait.line }}</span>
      </li>
    </ul>

    <section class="home__shots" aria-label="What it looks like">
      <RouterLink v-for="shot in shots" :key="shot.src" class="home__shot" :to="shot.to">
        <img :src="img(shot.src)" :alt="shot.alt" width="1280" height="720" loading="lazy" decoding="async" />
        <span class="home__shot-caption">
          {{ shot.caption }}
          <AppIcon name="arrowRight" :size="13" />
        </span>
      </RouterLink>
    </section>
  </div>
</template>

<style scoped lang="scss">
.home {
  @include container();
  padding-block: var(--space-8);

  @include from('md') {
    padding-block: var(--space-9) var(--space-8);
  }

  &__intro {
    max-width: var(--measure-prose);
  }

  &__title {
    font-size: var(--text-3xl);

    @include from('md') {
      font-size: 2.75rem;
    }
  }

  &__lead {
    margin-top: var(--space-4);
    color: var(--c-text-muted);
    font-size: var(--text-lg);
    line-height: var(--leading-relaxed);
  }

  &__actions {
    display: flex;
    flex-wrap: wrap;
    gap: var(--space-3);
    margin-top: var(--space-6);
  }

  &__cta {
    display: inline-flex;
    align-items: center;
    gap: var(--space-2);
    padding: var(--space-2) var(--space-4);
    border: var(--border-hair) solid var(--c-border-strong);
    border-radius: var(--radius-md);
    color: var(--c-text);
    font-size: var(--text-base);
    text-decoration: none;
    transition:
      border-color var(--duration-fast) var(--ease-out),
      background var(--duration-fast) var(--ease-out);
    @include focus-ring(2px);

    &:hover {
      border-color: var(--c-accent);
      color: var(--c-text);
    }

    &--primary {
      background: var(--c-accent);
      border-color: var(--c-accent);
      color: var(--c-accent-contrast);

      &:hover {
        background: var(--c-accent-hover);
        border-color: var(--c-accent-hover);
        color: var(--c-accent-contrast);
      }
    }
  }

  /* Capped well under the file's 1280px, so it does not dominate the page. */
  &__notice {
    /* Centred on the hero below it, not the left-aligned intro above: the two
     * form the top of the page. */
    max-width: 44rem;
    margin: var(--space-6) auto 0;
    padding: var(--space-3) var(--space-4);
    border-inline-start: 2px solid var(--c-warn);
    background: var(--c-warn-soft);
    border-radius: 0 var(--radius-md) var(--radius-md) 0;
    color: var(--c-text-muted);
    font-size: var(--text-sm);
    line-height: var(--leading-normal);

    p {
      margin: 0;
    }

    strong {
      color: var(--c-text);
    }
  }

  &__hero {
    margin: var(--space-7) auto 0;
    max-width: 44rem;

    img {
      width: 100%;
      height: auto;
      border: var(--border-hair) solid var(--c-border);
      border-radius: var(--radius-lg);
      background: var(--c-surface-sunken);
    }
  }

  &__hero-shot--dark {
    display: block;
  }

  &__hero-shot--light {
    display: none;
  }

  &__facts {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: var(--space-4);
    margin: var(--space-7) 0 0;
    padding: var(--space-4) 0;
    border-block: var(--border-hair) solid var(--c-border);

    @include from('md') {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }
  }

  &__fact {
    dt {
      @include eyebrow;
    }

    dd {
      margin: var(--space-1) 0 0;
      color: var(--c-text);
      font-size: var(--text-base);
    }
  }

  &__traits {
    list-style: none;
    margin: var(--space-6) 0 0;
    padding: 0;
    display: grid;
    gap: var(--space-4);
    grid-template-columns: 1fr;

    @include from('sm') {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }

    @include from('lg') {
      grid-template-columns: repeat(4, minmax(0, 1fr));
    }
  }

  &__trait {
    display: flex;
    flex-direction: column;
    gap: var(--space-1);
    padding-inline-start: var(--space-3);
    border-inline-start: 2px solid var(--c-accent);
  }

  &__trait-label {
    color: var(--c-text);
    font-weight: var(--weight-semibold);
    font-size: var(--text-base);
  }

  &__trait-line {
    color: var(--c-text-muted);
    font-size: var(--text-base);
    line-height: var(--leading-snug);
  }

  &__shots {
    display: grid;
    gap: var(--space-4);
    grid-template-columns: 1fr;
    margin-top: var(--space-8);

    @include from('md') {
      grid-template-columns: repeat(3, minmax(0, 1fr));
    }
  }

  &__shot {
    display: block;
    min-width: 0;
    text-decoration: none;
    @include focus-ring(2px);

    img {
      display: block;
      width: 100%;
      height: auto;
      border: var(--border-hair) solid var(--c-border);
      border-radius: var(--radius-md);
      background: var(--c-surface-sunken);
      transition: border-color var(--duration-fast) var(--ease-out);
    }

    &:hover img {
      border-color: var(--c-accent);
    }
  }

  &__shot-caption {
    display: inline-flex;
    align-items: center;
    gap: var(--space-1);
    margin-top: var(--space-2);
    color: var(--c-text-muted);
    font-size: var(--text-base);
  }

  &__shot:hover &__shot-caption {
    color: var(--c-accent);
  }
}

@media (prefers-color-scheme: light) {
  :root:not([data-scheme='dark']) .home {
    .home__hero-shot--dark {
      display: none;
    }

    .home__hero-shot--light {
      display: block;
    }
  }
}

:root[data-scheme='light'] .home {
  .home__hero-shot--dark {
    display: none;
  }

  .home__hero-shot--light {
    display: block;
  }
}
</style>
