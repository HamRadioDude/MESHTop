#!/bin/sh
# MESHtop: make the Chicago map pack (90 miles each way, zoom 6-14). Double-click in Finder.
cd "$(dirname "$0")" || exit 1
if ! command -v node >/dev/null 2>&1; then
  echo "Node.js is not installed. Get the LTS version from https://nodejs.org, then run this again."
  read -r _
  exit 1
fi
if [ ! -d node_modules/@maplibre/maplibre-gl-native ]; then
  echo "Installing what the map maker needs, once..."
  npm install --no-audit --no-fund || { read -r _; exit 1; }
fi
node make-map-pack.mjs "$@"
echo
echo "Press Return to close this window."
read -r _
