/*
 * Routes.
 *
 * Three pages, because the header links to pages, not to anchors on one page.
 * The docs reader is a nested route so the sidebar and the contents column are
 * mounted once and only the article changes.
 */
import { createRouter, createWebHistory, type RouteRecordRaw } from 'vue-router'
import { site } from '@/content/site'
import { docOrder, getDoc } from '@/content/docs'

const routes: RouteRecordRaw[] = [
  {
    path: '/',
    name: 'home',
    component: () => import('@/views/HomeView.vue'),
    meta: { title: null },
  },
  {
    path: '/features',
    name: 'features',
    component: () => import('@/views/FeaturesView.vue'),
    meta: { title: 'Features' },
  },
  {
    path: '/docs',
    component: () => import('@/views/DocsView.vue'),
    children: [
      {
        path: '',
        name: 'docs',
        // No index page of its own: the reader opens on the first document.
        redirect: () => ({ name: 'doc', params: { slug: docOrder[0]?.slug ?? 'mods' } }),
      },
      {
        path: ':slug',
        name: 'doc',
        component: () => import('@/views/DocView.vue'),
        props: true,
      },
    ],
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'not-found',
    component: () => import('@/views/NotFoundView.vue'),
    meta: { title: 'Not found' },
  },
]

export const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes,

  scrollBehavior(to, from, saved) {
    if (saved) return saved
    if (to.hash) {
      // The heading may belong to a document that is still being swapped in;
      // the reader re-runs this itself once the article is mounted.
      const el = document.querySelector(to.hash)
      if (el) return { el, top: 80 }
      return false
    }
    // Staying inside the same document (a contents click) must not jump to top.
    if (to.name === 'doc' && from.name === 'doc' && to.params.slug === from.params.slug) return false
    return { top: 0 }
  },
})

/*
 * The document title, decided in one place.
 *
 * A document's title comes from the markdown, not `meta`, so the doc route
 * resolves it from the slug here. The view cannot set it reliably: on a cold
 * load the view is created before this hook runs, so its title would be
 * overwritten.
 */
router.afterEach((to) => {
  const title =
    to.name === 'doc'
      ? (getDoc(String(to.params.slug))?.title ?? 'Not found')
      : (to.meta.title as string | null | undefined)

  document.title = title ? `${title} — ${site.name}` : site.name
})
