#!/bin/sh
# MESHtop map pack maker for Linux (and macOS terminals). Options as in: node make-map-pack.mjs --help
# Linux needs: sudo apt install libopengl0 xvfb
cd "$(dirname "$0")" || exit 1
command -v node >/dev/null 2>&1 || { echo "Node.js is not installed (get the LTS version from https://nodejs.org)."; exit 1; }
[ -d node_modules/@maplibre/maplibre-gl-native ] || npm install --no-audit --no-fund || exit 1
if [ "$(uname)" = Linux ] && [ -z "$DISPLAY" ] && command -v xvfb-run >/dev/null 2>&1; then
  exec xvfb-run -a node make-map-pack.mjs "$@"
fi
exec node make-map-pack.mjs "$@"
