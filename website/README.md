# Trophy website

A dependency-free, static GitHub Pages site. `index.html`, `styles.css`,
`headline.js`, `screenshots.js`, `downloads.js`, `assets/`, `robots.txt`, `sitemap.xml`, and `CNAME`
make up the site. Small scripts randomize the headline, enlarge screenshots, and show AppImage launch help;
the page and original-image links also work without JavaScript. No third-party
fonts, analytics, package manager, or build step is needed. Local asset URLs are
relative for previews; SEO URLs use the intended production domain, `gettrophy.app`.

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

Published releases also refresh the deployment. Before uploading, the workflow
resolves the latest stable release's AppImage, native packages, archive, and
checksums using its read-only GitHub token. It also renders the visible release
version and release-notes link from that same release. It writes these into the
published HTML; visitors need no API access or JavaScript to download. A release
must contain all five expected assets before deployment can succeed. If assets
are uploaded after the release is published, run the workflow again afterward.
Release downloads in a private repository require visitor access to that repo;
public Pages hosting does not make private release assets public.

The intended production URL is <https://gettrophy.app/>. Creating these files does
not itself enable Pages, register the domain, configure DNS, or publish anything.
The workflow uploads only `website/`; application code and the rest of the
repository are not published.

## Custom domain and search launch checklist

1. Register the domain, then verify domain ownership in GitHub using its TXT-record
   instructions. Keep that verification record in Cloudflare DNS.
2. Configure Cloudflare DNS for GitHub Pages following GitHub's
   [custom-domain instructions](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site).
   Registrar and DNS remain with Cloudflare; hosting remains with GitHub Pages.
3. Set **Settings → Pages → Custom domain** to `gettrophy.app`. `website/CNAME`
   records the same intended domain but does not replace that setting in an Actions
   deployment. Enable **Enforce HTTPS** once GitHub provisions the certificate.
4. If using `www.gettrophy.app`, configure its DNS too and check that it redirects
   to the canonical apex URL. Verify the old GitHub Pages address redirects as well.
5. Verify `https://gettrophy.app/`, `/robots.txt`, `/sitemap.xml`, and
   `/assets/social-preview.png` return successfully over HTTPS after deployment.
6. Add a **Domain property** in Google Search Console, verify it with its DNS TXT
   record, and submit `https://gettrophy.app/sitemap.xml`. These are account actions,
   not performed by the Pages workflow. Use URL Inspection to check indexing.
7. Run PageSpeed Insights on the live URL and test social previews after launch.
   Local checks do not establish indexing, live redirects, or Core Web Vitals.

The page includes a canonical URL, Open Graph/social-card metadata, and truthful
`SoftwareApplication` JSON-LD. No review scores, ratings, or download counts are
invented; structured data does not guarantee a rich search result. The sitemap
lists the landing page and the three documentation URLs, not their section anchors.

## Maintaining the page

- Update copy in `index.html` and styles in `styles.css`.
- Documentation pages under `website/docs/` are generated from `docs/*.md` by
  `python3 website/build_docs.py`, which needs the `markdown` and `pygments`
  packages. The output is gitignored: the Pages workflow renders it before
  uploading, and also runs for `docs/**` pushes, so editing a guide republishes
  the site without touching `website/`. Run the script yourself to preview at
  `http://localhost:8080/docs/getting-started.html`. `/docs/` is a redirect stub
  to the first guide, because a static host cannot issue a server redirect.
- Page order, titles and summaries live in `PAGES` in `build_docs.py`. Relative
  links between guides are rewritten to the generated pages; links that leave
  `docs/` (`../packaging/`, `../reporters/`, `../README.md`) point at GitHub.
  Keep the header and footer markup inside `build_docs.py` in step with
  `index.html` if the shared site chrome changes.
- Keep the canonical, social URLs, JSON-LD, sitemap, robots sitemap URL, and `CNAME`
  aligned if the production domain changes.
- `assets/social-preview.png` is the 1200×630 share image. Its editable source is
  `assets/social-preview.svg`, which references `dashboard-grid.png`. To refresh
  it, run `rsvg-convert social-preview.svg -o social-preview.png` from
  `website/assets/` (librsvg is a maintainer-only tool, not a hosting dependency).
- The hero shuffles nine phrases at two seconds per phrase using `headline.js`.
  Each round uses every phrase once, with no immediate repeat at round boundaries.
  CSS reserves space for the longest phrase and provides a sequential fallback
  without JavaScript; keep its delays and keyframe percentages in sync if adding
  phrases. Reduced motion shows a static headline, and assistive technology gets
  one complete, non-changing heading. Rotation timers stop in hidden tabs.
- AppImage buttons directly download the release asset; the secondary hero button
  goes to the native-package/archive/source options. Checked-in links use the
  verified v0.9.6 assets as a local/no-build baseline. Pages replaces these with
  latest-release URLs on each deployment. Keep `data-release-asset` markers and
  the workflow's expected link count in sync when changing download links. The
  `data-release-version` and `data-release-notes` markers are updated alongside them.
- AppImage clicks show launch instructions without cancelling normal browser
  download navigation. The actual clicked URL supplies the filename, retry link,
  and shell-quoted terminal commands; the dialog does not claim to track download
  completion. It supports its close button, Escape, backdrop dismissal, and focus
  return. Modified clicks and no-JavaScript visits keep ordinary link behavior.
- Documentation links point to `main` on GitHub; adjust them if the default branch
  changes.
- Screenshot assets are copies of `docs/images/dashboard-grid.png`,
  `docs/images/dashboard.png`, and `docs/images/games-list.png`; the Trophy and
  bookmark SVGs come from `resources/icons/`. Refresh these copies when the app
  screenshots or icons change.
- After replacing screenshots, run `python3 .github/scripts/optimize_screenshots.py`
  from the repo root with Pillow/WebP support installed. It regenerates the checked-in
  480/800/1200px WebP files; no image tooling is required by Pages or visitors.
  Keep `<picture>` srcsets, intrinsic dimensions, and `sizes` aligned with layout
  changes. The PNGs remain unchanged as full-resolution originals/fallbacks.
- Screenshot links open a native modal dialog containing only the original image.
  Clicking the backdrop or pressing Escape closes it and returns keyboard focus
  to the opener. Normal modified clicks and no-JavaScript visits retain the direct
  PNG link. Keep a useful `alt` and `data-screenshot` label on each screenshot.
- Keep compatibility and anti-cheat cautions aligned with the user documentation.
  Do not imply all compositor/GPU combinations have been verified.
- `docs/getting-started.md#requirements` is the canonical requirements section;
  its `#package-compatibility` table covers AppImage, native packages and the
  friend-testing kit. Keep the hero, compatibility and download links pointing
  there rather than maintaining a second package/version table on the website.
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

## Accessibility and image check — 2026-10-25

- The 1200px WebP set totals 348,620 bytes versus 1,766,839 bytes for the five PNGs
  (80.3% smaller); narrower variants are available. This is a byte comparison, not
  a live loading-speed or Core Web Vitals benchmark.
- Local Chromium Lighthouse checks, with the image dialog both closed and open:
  accessibility, SEO, and best practices each scored 100. Automated scores are not
  a complete accessibility assessment or a WCAG-conformance claim.
- Keyboard checks include Skip to content, screenshot activation, modal focus,
  Escape dismissal/focus return, and FAQ Enter/Space activation. Layout checks cover
  320–1440 CSS pixels, plus short landscape viewports and increased text spacing.
- Known limitation: the continuously rotating headline has no pause/stop control
  (removed by design). Reduced motion gives a static phrase, but does not replace
  a pause mechanism for every visitor under WCAG 2.2.2. Do not claim full compliance
  while this remains unresolved.
- Real screen-reader sessions, other browser engines, native browser-zoom behavior,
  and post-deployment network/performance checks remain manual follow-ups. Narrow
  CSS viewport checks cover reflow layouts, not every browser's zoom implementation.
