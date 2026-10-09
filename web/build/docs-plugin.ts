/*
 * docs-plugin.ts - the markdown corpus, turned into data at build time.
 *
 * Reads the repo's docs/*.md where they live, renders them once at build time,
 * and exposes the result as `virtual:docs`. Rendering here keeps a markdown
 * parser and a syntax highlighter (roughly a megabyte of grammars) out of the
 * client.
 *
 * Per document it emits rendered HTML, a heading tree for the table of
 * contents, and section-level records for the search index. Ordering, grouping
 * and labels are decided in src/content/docs.ts.
 */
import { readdirSync, readFileSync, statSync } from 'node:fs'
import { join, basename, extname } from 'node:path'
import MarkdownIt from 'markdown-it'
import type Token from 'markdown-it/lib/token.mjs'
import { createHighlighter, type Highlighter } from 'shiki'

/*
 * Three modules, because the rendered docs are roughly half a megabyte and the
 * overview page needs none of it:
 *
 *   virtual:docs         the manifest: titles, leads, heading trees, and a
 *                        loader per document. A few KB, always loaded.
 *   virtual:doc/<slug>   one document's HTML. Loaded when it is opened.
 *   virtual:docs-search  the section text. Loaded when search is first opened.
 */
const VIRTUAL_ID = 'virtual:docs'
const VIRTUAL_SEARCH_ID = 'virtual:docs-search'
const VIRTUAL_DOC_PREFIX = 'virtual:doc/'
const RESOLVED_ID = '\0' + VIRTUAL_ID
const RESOLVED_SEARCH_ID = '\0' + VIRTUAL_SEARCH_ID
const RESOLVED_DOC_PREFIX = '\0' + VIRTUAL_DOC_PREFIX

/* Loaded eagerly so an unknown language renders as plain text instead of
 * throwing mid-render. Anything not in this list renders unhighlighted. */
const LANGUAGES = [
  'c',
  'cpp',
  'rust',
  'python',
  'bash',
  'shell',
  'json',
  'yaml',
  'toml',
  'ini',
  'makefile',
  'diff',
  'javascript',
  'typescript',
  'sql',
  'xml',
  'scss',
]

/* Dual themes: shiki writes both colours as CSS custom properties and the
 * stylesheet picks one, so the code blocks follow the page's scheme without a
 * second render pass. */
const THEMES = { light: 'github-light', dark: 'github-dark-dimmed' } as const

export interface DocHeading {
  id: string
  text: string
  level: number
}

export interface DocSection {
  /** Anchor within the document, empty for the preamble before any heading. */
  id: string
  /** Heading text, or the document title for the preamble. */
  heading: string
  level: number
  /** Prose, flattened. */
  text: string
  /** Fenced and indented code, kept apart so it can score lower. */
  code: string
}

export interface DocRecord {
  slug: string
  title: string
  /** First paragraph, flattened; used as the search/nav blurb. */
  lead: string
  html: string
  headings: DocHeading[]
  sections: DocSection[]
  words: number
}

export interface DocsPluginOptions {
  dir: string
  /** The site's base path, e.g. `/` or `/cdj3k-mods/`. Always ends with a slash. */
  base: string
  /** Route the documentation is mounted at, without a trailing slash, e.g. `/docs`. */
  routePrefix: string
  /** Vite's public directory, where the markdown's image paths resolve. */
  publicDir: string
}

/**
 * A PNG's intrinsic size, straight out of its IHDR chunk.
 *
 * Read so every <img> can carry width and height. Without them a lazy image
 * occupies no space until it loads, and a deep link into a section below one
 * lands at a position that moves as the images above arrive. Only PNG is
 * handled because that is all docs/ uses; anything else gets no attributes.
 */
function pngSize(file: string): { width: number; height: number } | null {
  try {
    const buf = readFileSync(file)
    // 8-byte signature, then the IHDR length/type, then width and height.
    if (buf.length < 24 || buf.readUInt32BE(0) !== 0x89504e47) return null
    return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20) }
  } catch {
    return null
  }
}

/**
 * An mp4's display size, out of the first track header.
 *
 * Same reason as pngSize: a <video> with no dimensions occupies no space until
 * its metadata arrives. tkhd carries width and height as 16.16 fixed point,
 * after a header whose length depends on the box version.
 */
function mp4Size(file: string): { width: number; height: number } | null {
  try {
    const buf = readFileSync(file)
    const i = buf.indexOf('tkhd')
    if (i < 0) return null
    // 'tkhd' + version(1) + flags(3), then a v0/v1 block, reserved, layer,
    // alternate_group, volume, reserved and the 36-byte matrix.
    const at = i + 8 + (buf[i + 4] === 1 ? 32 : 20) + 8 + 8 + 36
    if (at + 8 > buf.length) return null
    const width = buf.readUInt32BE(at) >>> 16
    const height = buf.readUInt32BE(at + 4) >>> 16
    return width && height ? { width, height } : null
  } catch {
    return null
  }
}

function slugifyHeading(text: string, taken: Set<string>): string {
  const base =
    text
      .toLowerCase()
      .replace(/[`*_~]/g, '')
      .replace(/[^\p{L}\p{N}]+/gu, '-')
      .replace(/^-+|-+$/g, '') || 'section'

  let id = base
  let n = 2
  while (taken.has(id)) id = `${base}-${n++}`
  taken.add(id)
  return id
}

/**
 * Flatten an inline token to its visible text, dropping markup.
 *
 * Break tokens become a space: the markdown is hard-wrapped, so dropping the
 * softbreaks would join the last word of one line to the first of the next.
 */
function inlineText(token: Token | undefined): string {
  if (!token) return ''
  if (!token.children?.length) return token.content

  return token.children
    .map((c) => {
      if (c.type === 'text' || c.type === 'code_inline') return c.content
      if (c.type === 'softbreak' || c.type === 'hardbreak') return ' '
      return ''
    })
    .join('')
}

function createRenderer(
  highlighter: Highlighter,
  slugs: Set<string>,
  base: string,
  routePrefix: string,
  publicDir: string,
) {
  const md: MarkdownIt = MarkdownIt({
    // The docs are trusted, but raw HTML is escaped so a stray `<name>` in
    // prose shows up as written.
    html: false,
    linkify: true,
    typographer: false,
    highlight(code, lang) {
      const language = LANGUAGES.includes(lang) ? lang : 'text'
      try {
        return highlighter.codeToHtml(code, {
          lang: language,
          themes: THEMES,
          defaultColor: false,
        })
      } catch {
        return ''
      }
    },
  })

  /*
   * Links.
   *
   * A `*.md` href pointing at a document we carry becomes a route; one pointing
   * anywhere else (../CLAUDE.md, a source file) is unwrapped to plain text so
   * the site has no dead links. External hrefs get the usual target/rel pair.
   * In-document `#anchor` hrefs are left alone.
   */
  const defaultLinkOpen =
    md.renderer.rules.link_open ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.link_open = (tokens, i, options, env, self) => {
    const token = tokens[i]
    const href = token.attrGet('href') ?? ''

    if (/^(https?:)?\/\//i.test(href) || href.startsWith('mailto:')) {
      token.attrSet('target', '_blank')
      token.attrSet('rel', 'noopener noreferrer')
      token.attrSet('data-external', '')
      return defaultLinkOpen(tokens, i, options, env, self)
    }

    if (href.startsWith('#')) return defaultLinkOpen(tokens, i, options, env, self)

    const [path, hash] = href.split('#')
    if (path && /\.md$/i.test(path)) {
      const slug = basename(path, extname(path))
      if (slugs.has(slug)) {
        const route = `${routePrefix}/${slug}${hash ? "#" + hash : ""}`
        /* `href` is the real URL under the deployed base path, for middle-click
         * and "copy link"; `data-internal` is the route the click handler hands
         * to the router, which applies the base itself. */
        token.attrSet('href', base + route.slice(1))
        token.attrSet('data-internal', route)
        return defaultLinkOpen(tokens, i, options, env, self)
      }
      // Unknown target: mark it so link_close can drop the anchor entirely.
      token.meta = { ...(token.meta ?? {}), unlink: true }
      return ''
    }

    return defaultLinkOpen(tokens, i, options, env, self)
  }

  const defaultLinkClose =
    md.renderer.rules.link_close ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.link_close = (tokens, i, options, env, self) => {
    // Walk back to the matching open to see whether it was unwrapped.
    for (let j = i - 1; j >= 0; j--) {
      if (tokens[j].type === 'link_open') {
        return tokens[j].meta?.unlink ? '' : defaultLinkClose(tokens, i, options, env, self)
      }
    }
    return defaultLinkClose(tokens, i, options, env, self)
  }

  /*
   * Images.
   *
   * Screenshots live in `web/public/img/` and are written in the markdown as
   * `img/whatever.png`, which is where they sit relative to the site root. The
   * base path is applied here, as for links: the markdown cannot know it, and
   * a project page is not served at `/`.
   *
   * Loading is lazy, and a PNG's intrinsic size is stamped as width/height
   * (see pngSize), so a long page of screenshots costs nothing until it is
   * scrolled to.
   *
   * A paragraph containing only images becomes a gallery: the images sit side
   * by side and each gets its alt text as a visible caption (e.g. the theme
   * screenshots, for comparison). Anything with prose in it renders as an
   * ordinary inline image, because a figure inside a <p> is not valid HTML and
   * the browser would break the paragraph around it.
   */
  let inGallery = false

  const imagesOnly = (inline: Token) =>
    Boolean(inline.children?.length) &&
    inline.children!.some((c) => c.type === 'image') &&
    inline.children!.every(
      (c) =>
        c.type === 'image' ||
        c.type === 'softbreak' ||
        (c.type === 'text' && !c.content.trim()),
    )

  const defaultParagraphOpen =
    md.renderer.rules.paragraph_open ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.paragraph_open = (tokens, i, options, env, self) => {
    const inline = tokens[i + 1]
    if (inline?.type === 'inline' && imagesOnly(inline)) {
      inGallery = true
      return '<div class="md-gallery">'
    }
    return defaultParagraphOpen(tokens, i, options, env, self)
  }

  const defaultParagraphClose =
    md.renderer.rules.paragraph_close ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.paragraph_close = (tokens, i, options, env, self) => {
    if (inGallery) {
      inGallery = false
      return '</div>'
    }
    return defaultParagraphClose(tokens, i, options, env, self)
  }

  /*
   * Alerts: a blockquote whose first line is `[!WARNING]` or `[!NOTE]`.
   *
   * GitHub's syntax, so the same markdown renders as an alert on GitHub and as
   * a coloured quote here, without HTML. The marker is removed from the text;
   * it becomes the label.
   */
  const ALERT_CLASS: Record<string, string> = {
    WARNING: 'md-quote--warn',
    CAUTION: 'md-quote--warn',
    NOTE: 'md-quote--note',
  }

  const defaultQuoteOpen =
    md.renderer.rules.blockquote_open ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.blockquote_open = (tokens, i, options, env, self) => {
    const inline = tokens[i + 2]
    const first = inline?.type === 'inline' ? inline.children?.[0] : undefined
    if (first?.type === 'text') {
      const marker = /^\[!(\w+)\]\s*$/.exec(first.content)
      const cls = marker ? ALERT_CLASS[marker[1].toUpperCase()] : undefined
      if (cls && inline?.children) {
        inline.children.shift()
        if (inline.children[0]?.type === 'softbreak') inline.children.shift()
        tokens[i].attrJoin('class', cls)
      }
    }
    return defaultQuoteOpen(tokens, i, options, env, self)
  }

  md.renderer.rules.image = (tokens, i, options, _env, self) => {
    const token = tokens[i]
    const src = token.attrGet('src') ?? ''

    /*
     * Video, written the same way as an image: `![what it shows](vid/name.mp4)`.
     *
     * Short demonstrations of a gesture, looping like an animated image, with
     * `playsinline` so iOS does not take them fullscreen. The alt text becomes
     * the caption, so a reader who cannot see the clip knows what it shows.
     *
     * Muted, though the clips carry sound: browsers block autoplay with audio.
     * `controls` lets the reader turn the sound on or pause the loop.
     */
    if (/\.mp4$/i.test(src)) {
      const relative = src.replace(/^\//, '')
      const size = mp4Size(join(publicDir, relative))
      const dims = size ? ` width="${size.width}" height="${size.height}"` : ''
      const caption = md.utils.escapeHtml(inlineText(token))
      /* `preload="auto"`: a few hundred KB each, so they are already playing
       * when the reader arrives.
       *
       * Autoplay is an attribute and cannot be withdrawn in CSS, so a
       * reduced-motion preference is honoured at runtime, in DocView. */
      return (
        `<figure class="md-video">` +
        `<video src="${md.utils.escapeHtml(base + relative)}"${dims} ` +
        `autoplay muted loop playsinline controls preload="auto"></video>` +
        `<figcaption>${caption}</figcaption>` +
        `</figure>`
      )
    }

    if (!/^(https?:)?\/\//i.test(src) && !src.startsWith('data:')) {
      const relative = src.replace(/^\//, '')
      const size = pngSize(join(publicDir, relative))
      if (size) {
        token.attrSet('width', String(size.width))
        token.attrSet('height', String(size.height))
      }
      token.attrSet('src', base + relative)
    }

    token.attrSet('loading', 'lazy')
    token.attrSet('decoding', 'async')
    // markdown-it puts the alt text in the token's children, not in an attr.
    const alt = inlineText(token)
    token.attrSet('alt', alt)

    const img = self.renderToken(tokens, i, options)
    if (!inGallery) return img

    const caption = alt ? `<figcaption>${md.utils.escapeHtml(alt)}</figcaption>` : ''
    return `<figure class="md-figure">${img}${caption}</figure>`
  }

  /* Tables and fences are the two things that can outgrow the column. Each gets
   * its own scroll container so the page body never scrolls sideways. */
  md.renderer.rules.table_open = () => '<div class="md-scroll"><table>'
  md.renderer.rules.table_close = () => '</table></div>'

  /* A permalink inside each heading that has an id. The id itself is assigned
   * by `analyse`, which runs between parse and render, so it is readable here. */
  const defaultHeadingOpen =
    md.renderer.rules.heading_open ??
    ((tokens, i, options, _env, self) => self.renderToken(tokens, i, options))

  md.renderer.rules.heading_open = (tokens, i, options, env, self) => {
    const html = defaultHeadingOpen(tokens, i, options, env, self)
    const id = tokens[i].attrGet('id')
    if (!id) return html
    return (
      html +
      `<a class="heading-anchor" href="#${id}" aria-label="Link to this section" tabindex="-1">#</a>`
    )
  }

  return md
}

/**
 * One pass over the token stream: assign heading ids, build the heading tree,
 * and cut the document into search sections. Mutates `tokens` (the ids), which
 * is why it must run before render.
 */
function analyse(tokens: Token[], title: string): Pick<DocRecord, 'headings' | 'sections' | 'lead'> {
  const headings: DocHeading[] = []
  const sections: DocSection[] = []
  const taken = new Set<string>()

  let current: DocSection = { id: '', heading: title, level: 1, text: '', code: '' }
  let lead = ''

  const push = () => {
    if (current.text.trim() || current.code.trim() || current.id) sections.push(current)
  }

  for (let i = 0; i < tokens.length; i++) {
    const token = tokens[i]

    if (token.type === 'heading_open') {
      const level = Number(token.tag.slice(1))
      const text = inlineText(tokens[i + 1])

      if (level <= 3) {
        const id = slugifyHeading(text, taken)
        token.attrSet('id', id)
        // h1 is the document title, already shown by the page header.
        if (level >= 2) headings.push({ id, text, level })

        push()
        current = { id, heading: text, level, text: '', code: '' }
      }
      continue
    }

    if (token.type === 'fence' || token.type === 'code_block') {
      current.code += token.content + '\n'
      continue
    }

    if (token.type === 'inline') {
      const text = inlineText(token)
      current.text += text + ' '
      if (!lead && tokens[i - 1]?.type === 'paragraph_open') lead = text
    }
  }
  push()

  return { headings, sections, lead }
}

/**
 * Drop the h1 and the paragraph that follows it, which the page header
 * already shows. Anything else between them (a badge line, a blockquote)
 * is left alone; only a plain paragraph directly after the title is the lead.
 */
function stripHeader(tokens: Token[], h1: number): Token[] {
  if (h1 < 0) return tokens

  let end = h1 + 3 // heading_open, inline, heading_close
  if (tokens[end]?.type === 'paragraph_open' && tokens[end + 1]?.type === 'inline') {
    end += 3
  }
  return tokens.slice(0, h1).concat(tokens.slice(end))
}

export function docsPlugin(options: DocsPluginOptions) {
  const { dir } = options
  const routePrefix = options.routePrefix.replace(/\/$/, '')
  const publicDir = options.publicDir
  const base = options.base.endsWith('/') ? options.base : options.base + '/'
  let highlighter: Highlighter | null = null
  let cache: DocRecord[] | null = null

  const sourceFiles = () =>
    readdirSync(dir)
      .filter((f) => f.endsWith('.md'))
      .sort()
      .map((f) => join(dir, f))

  async function load(): Promise<DocRecord[]> {
    if (cache) return cache

    highlighter ??= await createHighlighter({
      themes: [THEMES.light, THEMES.dark],
      langs: LANGUAGES,
    })

    const files = sourceFiles()
    const slugs = new Set(files.map((f) => basename(f, '.md')))
    const md = createRenderer(highlighter, slugs, base, routePrefix, publicDir)

    cache = files.map((file) => {
      const slug = basename(file, '.md')
      const source = readFileSync(file, 'utf8')
      const env = {}
      const tokens = md.parse(source, env)

      const h1 = tokens.findIndex((t) => t.type === 'heading_open' && t.tag === 'h1')
      const title = h1 >= 0 ? inlineText(tokens[h1 + 1]) : slug

      const { headings, sections, lead } = analyse(tokens, title)

      /* The page header renders the title and the lead paragraph, so both are
       * removed from the body. They stay in `sections` so a search for a word
       * that only appears in the lead still finds the document. */
      const body = stripHeader(tokens, h1)
      const html = md.renderer.render(body, md.options, env)

      return {
        slug,
        title,
        lead,
        html,
        headings,
        sections,
        words: source.split(/\s+/).length,
      }
    })

    return cache
  }

  return {
    name: 'cdj3k-docs',
    enforce: 'pre' as const,

    resolveId(id: string) {
      if (id === VIRTUAL_ID) return RESOLVED_ID
      if (id === VIRTUAL_SEARCH_ID) return RESOLVED_SEARCH_ID
      if (id.startsWith(VIRTUAL_DOC_PREFIX)) return '\0' + id
      return null
    },

    async load(id: string) {
      if (id === RESOLVED_ID) {
        const docs = await load()
        const manifest = docs.map(({ slug, title, lead, headings, words }) => ({
          slug,
          title,
          lead,
          headings,
          words,
        }))
        /* The loaders are written out one literal specifier at a time so the
         * bundler can see them and give each document its own chunk. A single
         * import(`virtual:doc/${slug}`) would defeat that. */
        const loaders = docs
          .map((d) => `  ${JSON.stringify(d.slug)}: () => import('${VIRTUAL_DOC_PREFIX}${d.slug}')`)
          .join(',\n')

        return `export const docs = ${JSON.stringify(manifest)}\nexport const loaders = {\n${loaders}\n}\n`
      }

      if (id === RESOLVED_SEARCH_ID) {
        const docs = await load()
        const entries = docs.map(({ slug, title, sections }) => ({ slug, title, sections }))
        return `export const entries = ${JSON.stringify(entries)}\n`
      }

      if (id.startsWith(RESOLVED_DOC_PREFIX)) {
        const slug = id.slice(RESOLVED_DOC_PREFIX.length)
        const doc = (await load()).find((d) => d.slug === slug)
        if (!doc) return `export const html = ''\n`
        return `export const html = ${JSON.stringify(doc.html)}\n`
      }

      return null
    },

    configureServer(server: {
      watcher: { add: (p: string) => void; on: (e: string, cb: (p: string) => void) => void }
      moduleGraph: {
        idToModuleMap: Map<string, unknown>
        invalidateModule: (mod: never) => void
      }
      ws: { send: (p: { type: string }) => void }
    }) {
      server.watcher.add(dir)
      server.watcher.add(publicDir)

      /*
       * Editing a document drops the render cache, invalidates the virtual
       * modules and reloads.
       *
       * The invalidation is required: a virtual module has no file behind it,
       * so nothing in Vite's graph connects `virtual:docs` to its markdown or
       * marks it stale. Without it a reload re-imports the cached manifest
       * while the lazily loaded document chunk is fresh, so the article shows
       * the edit and the table of contents does not.
       *
       * A .png counts too: the build stamps each image's intrinsic size into
       * the HTML, so replacing a screenshot with a different size must
       * re-render.
       */
      const invalidate = (file: string) => {
        const isDoc = file.startsWith(dir) && file.endsWith('.md')
        const isImage = file.startsWith(publicDir) && /\.(png|jpe?g|svg|webp)$/i.test(file)
        if (!isDoc && !isImage) return

        cache = null
        for (const [id, mod] of server.moduleGraph.idToModuleMap) {
          if (id === RESOLVED_ID || id === RESOLVED_SEARCH_ID || id.startsWith(RESOLVED_DOC_PREFIX))
            server.moduleGraph.invalidateModule(mod as never)
        }
        server.ws.send({ type: 'full-reload' })
      }
      for (const event of ['add', 'change', 'unlink']) server.watcher.on(event, invalidate)
    },

    buildStart() {
      // A missing docs/ is a build error, not an empty site.
      try {
        if (!statSync(dir).isDirectory()) throw new Error()
      } catch {
        throw new Error(`[cdj3k-docs] corpus directory not found: ${dir}`)
      }
    },
  }
}
