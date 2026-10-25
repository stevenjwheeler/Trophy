# Trophy website

A dependency-free, static GitHub Pages site. `index.html`, `styles.css`,
`headline.js`, and `assets/` are the complete published website. A small script
randomizes the headline; the page also works without JavaScript. No third-party
fonts, analytics, package manager, or build step is needed. All local URLs are relative
so it works at a project path such as `/Trophy/` and on a custom domain.

## Preview locally

From the repository root:

```sh
python3 -m http.server 8080 --directory website
```

Open <http://localhost:8080>. You can also open `index.html` directly.

## Publish on GitHub Pages

1. In the repository's **Settings → Pages → Build and deployment**, select
   **GitHub Actions** as the source.
2. Merge the website and `.github/workflows/pages.yml` into `main`. Subsequent
   changes to the website on `main` deploy automatically.
3. Alternatively, run **Deploy Trophy website → Run workflow** from Actions,
   selecting the branch you want to publish. The `github-pages` environment's
   deployment branch rules must permit that branch.

The expected project URL is <https://stevenjwheeler.github.io/Trophy/>. Creating
these files does not itself enable Pages or publish anything. The workflow uploads
only `website/`; application code and the rest of the repository are not published.

## Maintaining the page

- Update copy in `index.html` and styles in `styles.css`.
- The hero shuffles nine phrases at two seconds per phrase using `headline.js`.
  Each round uses every phrase once, with no immediate repeat at round boundaries.
  CSS reserves space for the longest phrase and provides a sequential fallback
  without JavaScript; keep its delays and keyframe percentages in sync if adding
  phrases. Reduced motion shows a static headline, and assistive technology gets
  one complete, non-changing heading. Rotation timers stop in hidden tabs.
- The download button points to GitHub's latest release, not a hardcoded version.
- Documentation links point to `main` on GitHub; adjust them if the default branch
  changes.
- Screenshot assets are copies of `docs/images/dashboard-grid.png`,
  `docs/images/dashboard.png`, and `docs/images/games-list.png`; the Trophy and
  bookmark SVGs come from `resources/icons/`. Refresh these copies when the app
  screenshots or icons change.
- Keep compatibility and anti-cheat cautions aligned with the user documentation.
  Do not imply all compositor/GPU combinations have been verified.
- The three theme cards appear side by side (stacked on mobile), with AMOLED
  first/default. Its screenshot reuses `assets/dashboard-grid.png`; Charcoal and
  Original use the supplied `assets/theme_charcoal.png` and `assets/theme_original.png`.
  Each opens its full-size image when clicked. Keep matching dashboard layouts and
  image dimensions for easy comparison. The seven accent swatches match
  `qml/styles/Theme.qml`; keep them aligned.
- The player showcase is a labeled CSS timeline illustration. Replace it with a
  current player screenshot when one is available; do not present the illustration
  as a screenshot or interactive player. Keep the two shortcut descriptions free
  of assumed key assignments, since users choose their own bindings.
- Check narrow and wide layouts, keyboard navigation, FAQ toggles, local links,
  screenshot loading, and the download/documentation links before publishing.
