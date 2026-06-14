#!/usr/bin/env bash
# Re-bundle esp-web-tools and replace installer/install-button.js.
# Usage: script/update-esp-web-tools.sh [version]
#   version defaults to 10.2.1
#
# The CDN builds of esp-web-tools 10.x are broken (atob-lite export mismatch on
# unpkg; duplicate md-focus-ring custom-element on jsdelivr), so the bundle is
# vendored here instead.
set -euo pipefail

VERSION="${1:-10.2.1}"
DEST_DIR="$(cd "$(dirname "$0")/.." && pwd)/installer"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$TMP"
echo '{"private":true,"type":"module"}' > package.json
echo "Installing esp-web-tools@$VERSION ..."
npm install "esp-web-tools@$VERSION" --silent

# Code-splitting: esp-web-tools dynamically imports the flash dialog and
# esptool-js on Install, so --splitting keeps those as lazy-loaded chunks and
# the initial install-button.js stays small. Entry stays install-button.js.
# Chunk names are content-hashed and rotate each rebundle, so clear old .js
# first to avoid orphans.
mkdir -p "$DEST_DIR"
find "$DEST_DIR" -maxdepth 1 -name '*.js' -delete
echo "Bundling to $DEST_DIR ..."
npx --yes esbuild node_modules/esp-web-tools/dist/install-button.js \
  --bundle --splitting --format=esm --minify --target=es2020 \
  --outdir="$DEST_DIR"

ls -lh "$DEST_DIR"
echo "Done. Commit and push when you're happy."
