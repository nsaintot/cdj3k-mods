/*
 * spa-fallback.ts - the 404.html GitHub Pages needs.
 *
 * Pages serves static files: a request for /docs/mods has no file behind it,
 * so a deep link, a refresh or a shared URL would 404 even though the router
 * knows the route. Pages serves 404.html for anything it cannot find, so a
 * byte-identical copy of index.html there boots the app, which then routes on
 * the requested URL. This lets the router use history mode instead of hashes.
 */
import type { Plugin } from 'vite'

export function spaFallback(): Plugin {
  return {
    name: 'cdj3k-spa-fallback',
    apply: 'build',
    enforce: 'post',

    generateBundle(_options, bundle) {
      const index = bundle['index.html']
      if (!index || index.type !== 'asset') {
        this.warn('index.html not found in the bundle; no 404.html written')
        return
      }

      this.emitFile({
        type: 'asset',
        fileName: '404.html',
        source: index.source,
      })
    },
  }
}
