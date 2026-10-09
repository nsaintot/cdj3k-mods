/* The shapes the build-time markdown plugin emits. Kept beside the app rather
 * than imported from build/, so the client bundle never reaches into it.
 *
 * The split into three is for loading; see the note at the top of
 * build/docs-plugin.ts. */

export interface DocHeading {
  id: string
  text: string
  level: number
}

/** What every page knows about every document, without loading any of them. */
export interface DocMeta {
  slug: string
  title: string
  lead: string
  headings: DocHeading[]
  words: number
}

/** One heading's worth of body, for the search index. */
export interface DocContentSection {
  id: string
  heading: string
  level: number
  text: string
  code: string
}

export interface DocSearchEntry {
  slug: string
  title: string
  sections: DocContentSection[]
}
