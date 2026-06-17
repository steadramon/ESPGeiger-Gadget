# ESPGeiger Gadget web installer

Browser-based flasher (ESP Web Tools) that installs the gadget firmware over USB
with no toolchain. Served from GitHub Pages at <https://gadget.espgeiger.com>.

## How it works

- `index.htm` - the page. Offers the two CYD panel builds and flashes the
  matching build. The ESP Web Tools entry is loaded with a `?v={version}`
  cache-bust so a republish never serves a stale shell.
- `brand.css` - shared ESPGeiger brand theme (topbar, palette, card/form
  styles), copied verbatim from the firmware webinstaller so the install pages
  look like one product. Also cache-busted via `?v={version}`.
- `installer/` - ESP Web Tools vendored as code-split chunks (entry
  `install-button.js` plus lazy-loaded flash dialog / esptool-js / chip stubs).
  Built artefacts, committed deliberately - the CDN builds of esp-web-tools 10.x
  are broken (silent button-never-mounts). Regenerate with
  `script/update-esp-web-tools.sh [version]` (needs node/npm).
- `bin/<panel>/manifest.json` - one ESP Web Tools manifest per panel. Each lists
  four parts at their flash offsets (bootloader 0x1000, partitions 0x8000,
  boot_app0 0xe000, app 0x10000). Flashing the parts individually leaves the NVS
  region (0x9000-0xe000) untouched, so wifi and settings survive an update. A
  single merged image at offset 0 would pad that gap with 0xFF and wipe NVS.
- `bin/<panel>/{bootloader,partitions,boot_app0,firmware}.bin` - the four flash
  parts. None are committed (see `.gitignore`); they are produced by the build and
  bundled into the release zip, so they always match the firmware exactly.

## Build and publish flow

- `.github/workflows/release.yml` (on `release: published`) builds both panels,
  attaches the firmware binaries, and - in its `webinstaller` job - drops each
  panel's freshly built flash parts next to its manifest, stamps the version, and
  uploads the whole folder as `espgadget-webinstaller-<tag>.zip` on the release.
  That bundle is the self-contained, ready-to-serve site (also handy for
  self-hosting).
- `.github/workflows/installer.yml` - run by hand from the Actions tab once you
  are happy with a release (defaults to the latest, or pass a specific tag). It
  just downloads `espgadget-webinstaller-<tag>.zip` and publishes its contents to
  the `gh-pages` branch as a single orphan commit. No assembly happens here, so
  the published site is byte-for-byte the release artefact.

Deep links preselect a panel: `?board=st7789-rv3` or `?board=ili9341-rv2`.

## One-time setup

1. Publish a release first (so `espgadget-webinstaller-<tag>.zip` exists), then
   run the **Web Installer** workflow once (Actions tab) so the `gh-pages` branch
   exists.
2. Repo Settings -> Pages -> Source = `gh-pages` branch, root.
3. Point a DNS `CNAME` for `gadget.espgeiger.com` at `<user>.github.io`. The
   `CNAME` file in this folder sets the custom domain on publish.

After a release, run the **Web Installer** workflow by hand once you are happy
with it to refresh the hosted firmware.
