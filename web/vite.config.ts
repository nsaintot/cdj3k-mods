import { fileURLToPath, URL } from 'node:url'
import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { docsPlugin } from './build/docs-plugin'
import { spaFallback } from './build/spa-fallback'

// Served from the root of cdj3k-mods.com. Set BASE_PATH to /<repo>/ to build
// for a GitHub project page instead.
const base = process.env.BASE_PATH ?? '/'

export default defineConfig({
  base,
  plugins: [
    vue(),
    docsPlugin({
      // Pages come from the repo's docs/ directory; nothing is copied into the
      // app source.
      dir: fileURLToPath(new URL('../docs', import.meta.url)),
      // Both are needed to write real hrefs for page-to-page links in the
      // markdown. The prefix must match the route in src/router/index.ts.
      base,
      routePrefix: '/docs',
      // Where `img/...` in the markdown resolves, so the build can read each
      // screenshot's intrinsic size and stamp it on the <img>.
      publicDir: fileURLToPath(new URL('./public', import.meta.url)),
    }),
    spaFallback(),
  ],
  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url)),
    },
  },
  css: {
    preprocessorOptions: {
      scss: {
        api: 'modern-compiler',
        additionalData: `@use "@/styles/abstracts" as *;`,
      },
    },
  },
  build: {
    target: 'es2022',
    cssCodeSplit: true,
  },
})
