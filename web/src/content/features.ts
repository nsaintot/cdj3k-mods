/*
 * The feature list.
 *
 * A catalogue, not documentation: one line per feature saying what it does,
 * where it is switched, and whether it is finished, plus a link into the
 * documentation. There is no long-form field; usage belongs in docs/.
 */

export type FeatureStatus = 'working' | 'partial'

export interface FeatureLink {
  /** Documentation page slug, as in docs/<slug>.md */
  doc: string
  /** Heading id within that page, from the build-time slugifier. */
  hash?: string
  label: string
}

export interface Feature {
  id: string
  name: string
  /** One line: what it does, from the front of the deck. */
  summary: string
  /** The MOD SETTINGS row this appears as, and its shipped default. */
  setting?: { row: string; default: string }
  status: FeatureStatus
  /** A qualifier worth knowing before you open the page. Kept to one clause. */
  scope?: string
  links: FeatureLink[]
}

export interface FeatureGroup {
  id: string
  title: string
  /** What this group of mods touches. Not a tagline. */
  note: string
  features: Feature[]
}

export const featureGroups: FeatureGroup[] = [
  {
    id: 'cues',
    title: 'Hot cues',
    note: 'Changes to what the eight pads do. Each one is independent, and with all of them off the pads behave as stock.',
    features: [
      {
        id: 'gate-cue',
        name: 'Gate cue',
        summary:
          'While paused, the pads are momentary: press to jump and play, release to return to the cue and pause.',
        setting: { row: 'GATE CUE', default: 'off' },
        status: 'working',
        links: [{ doc: 'hot-cues', hash: 'gate-cue', label: 'How to use it' }],
      },
      {
        id: 'smart-cue',
        name: 'Smart cue',
        summary: 'The memory cue follows the hot cue you last pressed, so CUE returns to it.',
        setting: { row: 'SMART CUE', default: 'off' },
        status: 'working',
        links: [{ doc: 'hot-cues', hash: 'smart-cue', label: 'How to use it' }],
      },
      {
        id: 'preview-hotcue',
        name: 'Preview hot cue',
        summary:
          'Hold the preview zone and press an unassigned pad: the cue lands under your finger, on the grid.',
        setting: { row: 'PREVIEW HOTCUE', default: 'off' },
        status: 'working',
        links: [{ doc: 'hot-cues', hash: 'preview-hotcue', label: 'How to use it' }],
      },
    ],
  },
  {
    id: 'stems',
    title: 'Stems',
    note: 'Separation runs on a computer on your network; the deck stores the result and mixes it live.',
    features: [
      {
        id: 'stem-playback',
        name: 'Stem playback',
        summary: 'Drums, harmonics and vocals on three faders on the play screen.',
        setting: { row: 'ENABLE STEMS', default: 'off' },
        status: 'working',
        scope: 'Needs a stem server on your network.',
        links: [{ doc: 'stems', label: 'How to use it' }],
      },
      {
        id: 'groove-circuit',
        name: 'Groove circuit',
        summary:
          'A hot cue pad plays a loop from your stick in place of a stem, locked to the beat grid, while the rest of the track keeps playing.',
        status: 'working',
        scope: 'Needs stems, and the STEMS row open.',
        links: [{ doc: 'groove-circuit', label: 'How to set it up' }],
      },
    ],
  },
  {
    id: 'xpad',
    title: 'X-PAD',
    note: 'A sampler on a touch strip under the waveform. Opening it takes over the eight pads and three buttons; closing it gives them all back.',
    features: [
      {
        id: 'xpad-sampler',
        name: 'X-PAD sampler',
        summary:
          'Six loop lengths across the strip and \u00b112 semitones up and down it, with eight samples from your stick on the pads.',
        setting: { row: 'ENABLE X-PAD', default: 'off' },
        status: 'working',
        scope: 'Needs audio in mods/loops/ on your stick.',
        links: [{ doc: 'xpad', label: 'How to use it' }],
      },
      {
        id: 'xpad-overdub',
        name: 'OVERDUB',
        summary:
          'A four-beat sequencer on the track\u2019s own grid: it stores which pad fired and where in the bar, not the audio.',
        status: 'working',
        scope: 'While the X-PAD panel is open.',
        links: [{ doc: 'xpad', hash: 'hold-and-overdub', label: 'How to use it' }],
      },
    ],
  },
  {
    id: 'interface',
    title: 'Interface',
    note: 'Changes to the deck’s own screens. Each can be switched off, and none is permanent.',
    features: [
      {
        id: 'mod-settings',
        name: 'MOD SETTINGS',
        summary:
          'Tap the Ver. label on the DJ SETTING screen to show the mods’ settings in its place.',
        status: 'working',
        links: [{ doc: 'mod-settings', label: 'How to open it' }],
      },
      {
        id: 'themes',
        name: 'Themes',
        summary:
          'Colour themes for the deck’s screen, applied to everything it draws, including the waveform.',
        setting: { row: 'THEME', default: 'ORIGINAL' },
        status: 'working',
        links: [{ doc: 'themes', label: 'How to use it' }],
      },
      {
        id: 'grid-adjust',
        name: 'BPM adjust',
        summary:
          'A BPM group beside the deck’s own grid buttons: double or halve the tempo, move the beat interval a millisecond at a time, or reset it.',
        status: 'working',
        scope: 'Needs a track the deck has a grid for.',
        links: [{ doc: 'grid', label: 'How to use it' }],
      },
    ],
  },
  {
    id: 'library',
    title: 'Library',
    note: 'The mods read the deck’s own rekordbox database and keep no second copy of your library.',
    features: [
      {
        id: 'playlist-reorder',
        name: 'Playlist reorder',
        summary:
          'An EDIT toggle in the browse header: while it is on, dragging a track moves it and your stick keeps the new order.',
        status: 'partial',
        scope: 'Playlists only.',
        links: [{ doc: 'browsing', hash: 'reordering-a-playlist', label: 'How to use it' }],
      },
    ],
  },
]

/** Flat view, for the search index and for counting. */
export const allFeatures: Feature[] = featureGroups.flatMap((g) => g.features)
