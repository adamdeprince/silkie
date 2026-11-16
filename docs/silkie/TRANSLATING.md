# Website translations

English sources are the `.rst` files in this directory. Simplified Chinese
translations are gettext catalogs in `_locales/zh_CN/LC_MESSAGES/`. The HTML
templates also translate shared navigation, and `web/i18n.mjs` translates the
interactive demo's controls, accessibility labels, and status messages.

Install `docs/silkie/requirements.txt`, then run `make html FAIL_WARN=1` from
the repository root. This builds English in `html/` and Chinese in
`html/zh-CN/`. Jieba is required to index Chinese search terms. Compiled `.mo`
catalogs and Sphinx doctrees are generated files.

After editing English sources, extract their messages with:

```sh
sphinx-build -b gettext docs/silkie /tmp/silkie-gettext
```

Merge each updated `.pot` into its corresponding `.po` with a gettext editor,
translate new or changed entries, and review fuzzy matches. Preserve commands,
protocol fields, code, link targets, and numeric values. Explicit localized
section labels retain the original section target; their `#noqa` suffix tells
Sphinx that the label change is intentional. Validate the generated links too.

Chinese raw HTML uses `../_static/` to share images, videos, demo scripts, and
the guest filesystem with English. Do not copy the guest image into the
Chinese output. The old `index.zh-CN.html` address redirects to the full site.

Run the JavaScript tests and `node web/browser-smoke.mjs --mode chinese
--page zh-CN/index.html` to check the actual guest, language routes, fullscreen,
mobile layout, and Chinese search before publishing.
