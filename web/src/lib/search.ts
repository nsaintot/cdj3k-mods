/*
 * Search.
 *
 * Section-level, over the documentation and the feature list. Hand-written
 * rather than a library: the text is about 200 KB, small enough that a linear
 * scan with a scoring function beats shipping an index library, and each
 * ranking rule below is one line.
 *
 * Granularity is the section, not the document: a hit opens
 * `/docs/mods#cues`, not the top of a 700-line document.
 *
 * The index is a separate chunk, fetched the first time the dialog opens. A
 * visitor who never searches never downloads it.
 */
import { allFeatures } from '@/content/features'

export type ResultKind = 'doc' | 'feature'

export interface SearchRecord {
  kind: ResultKind
  /** Route to open on select. */
  to: string
  /** The section heading, or the feature name. */
  title: string
  /** Which document, or which group -- shown as the breadcrumb. */
  context: string
  /** Prose, searched and used for the snippet. */
  text: string
  /** Code, searched at a lower weight and never used for the snippet. */
  code: string
  /* Precomputed lowercase forms: the scan runs on every keystroke. */
  titleLower: string
  textLower: string
  codeLower: string
}

export interface SearchResult {
  record: SearchRecord
  score: number
  /** HTML, already escaped, with matches wrapped in <mark>. */
  snippet: string
}

/* Identifiers such as `ep122_shim` and `pcmbuf` are searched for, so
 * underscores and digits are part of a word. */
const WORD = /[^\p{L}\p{N}_]+/u

export function tokenize(input: string): string[] {
  return input.toLowerCase().split(WORD).filter(Boolean)
}

function featureRecords(): SearchRecord[] {
  return allFeatures.map((feature) => {
    const text = [feature.summary, feature.scope].filter(Boolean).join(' ')
    return {
      kind: 'feature' as const,
      to: `/features#${feature.id}`,
      title: feature.name,
      context: 'Features',
      text,
      code: feature.setting ? `${feature.setting.row} ${feature.setting.default}` : '',
      titleLower: feature.name.toLowerCase(),
      textLower: text.toLowerCase(),
      codeLower: (feature.setting?.row ?? '').toLowerCase(),
    }
  })
}

let index: SearchRecord[] | null = null
let pending: Promise<SearchRecord[]> | null = null

/** Fetch the section text chunk and fold it together with the feature list. */
export function loadIndex(): Promise<SearchRecord[]> {
  if (index) return Promise.resolve(index)

  return (pending ??= import('virtual:docs-search').then(({ entries }) => {
    const records: SearchRecord[] = []

    for (const doc of entries) {
      for (const section of doc.sections) {
        const title = section.id ? section.heading : doc.title
        records.push({
          kind: 'doc',
          to: section.id ? `/docs/${doc.slug}#${section.id}` : `/docs/${doc.slug}`,
          title,
          context: doc.title,
          text: section.text,
          code: section.code,
          titleLower: title.toLowerCase(),
          textLower: section.text.toLowerCase(),
          codeLower: section.code.toLowerCase(),
        })
      }
    }

    records.push(...featureRecords())
    index = records
    return records
  }))
}

/**
 * How well one token scores against one record.
 *
 * A whole-word hit outranks a prefix hit, a heading outranks the body, and
 * repetition is capped, so a section that says "stem" forty times does not
 * score forty times higher.
 */
function scoreToken(record: SearchRecord, token: string): number {
  let score = 0

  if (record.titleLower === token) score += 60
  else if (new RegExp(`\\b${escapeRegExp(token)}\\b`).test(record.titleLower)) score += 30
  else if (record.titleLower.includes(token)) score += 14

  const bodyHits = countOccurrences(record.textLower, token)
  if (bodyHits) score += 6 + Math.min(bodyHits, 6) * 1.5

  const codeHits = countOccurrences(record.codeLower, token)
  if (codeHits) score += 4 + Math.min(codeHits, 4)

  // The feature list is the shorter, plainer answer, so it wins a tie.
  if (score && record.kind === 'feature') score += 3

  return score
}

function countOccurrences(haystack: string, needle: string): number {
  let count = 0
  let from = 0
  for (;;) {
    const at = haystack.indexOf(needle, from)
    if (at < 0) return count
    count++
    from = at + needle.length
  }
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

export function escapeHtml(s: string): string {
  return s
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
}

/**
 * A window of the section's prose around the first match, with every query
 * token marked. Escaped first, so the marks are the only markup in it.
 */
function makeSnippet(text: string, tokens: string[], width = 170): string {
  if (!text) return ''

  const lower = text.toLowerCase()
  let at = -1
  for (const token of tokens) {
    const found = lower.indexOf(token)
    if (found >= 0 && (at < 0 || found < at)) at = found
  }

  let start = at < 0 ? 0 : Math.max(0, at - Math.floor(width / 3))
  // Do not cut a word in half at the front.
  if (start > 0) {
    const space = text.indexOf(' ', start)
    if (space >= 0 && space - start < 20) start = space + 1
  }
  const end = Math.min(text.length, start + width)

  let slice = text.slice(start, end).trim()
  if (start > 0) slice = '… ' + slice
  if (end < text.length) slice = slice + ' …'

  const escaped = escapeHtml(slice)
  const pattern = new RegExp(`(${tokens.map(escapeRegExp).join('|')})`, 'gi')
  return escaped.replace(pattern, '<mark>$1</mark>')
}

/** Runs against whatever is loaded; returns nothing until `loadIndex` settles. */
export function search(query: string, limit = 20): SearchResult[] {
  const tokens = tokenize(query)
  if (!tokens.length || !index) return []

  const results: SearchResult[] = []

  for (const record of index) {
    let total = 0
    let matchedAll = true

    for (const token of tokens) {
      const score = scoreToken(record, token)
      if (!score) {
        matchedAll = false
        break
      }
      total += score
    }

    // Every token must match somewhere in the section. An OR would return
    // nearly every section for any query containing a common word.
    if (!matchedAll) continue

    results.push({ record, score: total, snippet: makeSnippet(record.text, tokens) })
  }

  return results.sort((a, b) => b.score - a.score).slice(0, limit)
}
