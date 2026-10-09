/*
 * The documentation manifest.
 *
 * Titles, leads, headings and body come from the markdown via the build-time
 * plugin. This file only adds the reading order and the grouping.
 *
 * A doc not named in `sections` still appears, under "Other".
 */
import { docs as manifest, loaders } from 'virtual:docs'
import type { DocMeta } from '@/types/docs'

export type Doc = DocMeta

/** Reading order, by slug. */
const sections: { title: string; slugs: string[] }[] = [
  { title: 'Start here', slugs: ['getting-started', 'mod-settings'] },
  { title: 'Playing', slugs: ['hot-cues', 'stems', 'groove-circuit', 'xpad'] },
  { title: 'The deck', slugs: ['themes', 'browsing', 'grid'] },
  { title: 'Help', slugs: ['troubleshooting'] },
  { title: 'About', slugs: ['legal'] },
]

export const docs: Doc[] = manifest

const bySlug = new Map(docs.map((d) => [d.slug, d]))

export function getDoc(slug: string): Doc | undefined {
  return bySlug.get(slug)
}

/** A document's rendered HTML, loaded as its own chunk and cached. */
const htmlCache = new Map<string, string>()

export async function loadDocHtml(slug: string): Promise<string> {
  const cached = htmlCache.get(slug)
  if (cached !== undefined) return cached

  const loader = loaders[slug]
  if (!loader) return ''

  const { html } = await loader()
  htmlCache.set(slug, html)
  return html
}

export interface DocSection {
  title: string
  docs: Doc[]
}

export const docSections: DocSection[] = (() => {
  const placed = new Set<string>()
  const out: DocSection[] = []

  for (const section of sections) {
    const found = section.slugs.map((s) => bySlug.get(s)).filter((d): d is Doc => Boolean(d))
    found.forEach((d) => placed.add(d.slug))
    if (found.length) out.push({ title: section.title, docs: found })
  }

  const rest = docs.filter((d) => !placed.has(d.slug))
  if (rest.length) out.push({ title: 'Other', docs: rest })

  return out
})()

/** Flat reading order, for the previous/next pager. */
export const docOrder: Doc[] = docSections.flatMap((s) => s.docs)

export function docNeighbours(slug: string): { prev?: Doc; next?: Doc } {
  const i = docOrder.findIndex((d) => d.slug === slug)
  if (i < 0) return {}
  return { prev: docOrder[i - 1], next: docOrder[i + 1] }
}
