/*
 * Site-level facts shared by several components.
 */

export const site = {
  name: 'CDJ3K-Mods',
  /** What the project is, in one sentence. Used by the home page and <meta>. */
  description:
    'A collection of modifications for the CDJ-3000: hot cues, stems, X-PAD, themes, browsing.',
  /** The download, always the newest tag. */
  releases: 'https://github.com/nsaintot/cdj3k-mods/releases/latest',
  repo: 'https://github.com/nsaintot/cdj3k-mods',
  stemd: 'https://github.com/nsaintot/stemd',
  /** The firmware range the mods install into. */
  firmware: '3.13 to 3.22',
  device: 'CDJ-3000',
  license: 'MIT OR Apache-2.0',
} as const

export const nav = [
  { to: '/', label: 'Overview' },
  { to: '/features', label: 'Features' },
  { to: '/docs', label: 'Documentation' },
] as const
