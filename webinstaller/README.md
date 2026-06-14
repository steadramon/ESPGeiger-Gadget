# ESPGeiger Gadget web installer

Browser-based flasher (ESP Web Tools) that installs the gadget firmware over USB
with no toolchain. Served from GitHub Pages at <https://gadget.espgeiger.com>.

## How it works

- `index.htm` - the page. Offers the two CYD panel builds and flashes the
  matching merged image. The ESP Web Tools entry is loaded with a `?v={version}`
  cache-bust so a republish never serves a stale shell.
- `brand.css` - shared ESPGeiger brand theme (topbar, palette, card/form
  styles), copied verbatim from the firmware webinstaller so the install pages
  look like one product. Also cache-busted via `?v={version}`.
- `installer/` - ESP Web Tools vendored as code-split chunks (entry
  `install-button.js` plus lazy-loaded flash dialog / esptool-js / chip stubs).
  Built artefacts, committed deliberately - the CDN builds of esp-web-tools 10.x
  are broken (silent button-never-mounts). Regenerate with
  `script/update-esp-web-tools.sh [version]` (needs node/npm).
- `bin/<panel>/manifest.json` - one ESP Web Tools manifest per panel. The
  firmware itself (`firmware.bin`) is not committed; it is injected at publish
  time from the GitHub release.
- `.github/workflows/installer.yml` - run by hand from the Actions tab once you
  are happy with a release (defaults to the latest, or pass a specific tag).
  Downloads that release's `*-merged.bin` assets, drops them next to each
  manifest, stamps the version string, and publishes the folder to the
  `gh-pages` branch as a single orphan commit.

Deep links preselect a panel: `?board=st7789-rv3` or `?board=ili9341-rv2`.

## One-time setup

1. Run the **Web Installer** workflow once (Actions tab) so the `gh-pages` branch
   exists.
2. Repo Settings -> Pages -> Source = `gh-pages` branch, root.
3. Point a DNS `CNAME` for `gadget.espgeiger.com` at `<user>.github.io`. The
   `CNAME` file in this folder sets the custom domain on publish.

After a release, run the **Web Installer** workflow by hand once you are happy
with it to refresh the hosted firmware.
