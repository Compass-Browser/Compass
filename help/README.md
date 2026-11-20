# help.compassbrowser.org

Compass's help center, built with [Astro Starlight](https://starlight.astro.build).

- Pages are Markdown (`.md`, or `.mdx` when a page needs components such as
  tabs) in `src/content/docs/`. A section is a folder: `start/`, `guide/`,
  `sync/`, `self-hosting/`. The sidebar is generated from the folders and
  ordered by each page's `sidebar.order` in its frontmatter.
- Write for people using Compass, not for its developers. The design docs in
  `docs/design/` are the source of truth for how things work. When a feature
  changes, update its help page in the same commit.

```sh
cd help
npm ci
npm run dev        # http://localhost:4321, reloads on save
npm run build      # static site in dist/, with the search index
```

To publish, copy `dist/` to the web server (see compass-ops,
`website-publish.md`, "help.compassbrowser.org").
