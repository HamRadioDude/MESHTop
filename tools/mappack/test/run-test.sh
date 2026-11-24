#!/bin/sh
# End-to-end test of make-map-pack.mjs with no internet needed: stand-in map data served the way
# build.protomaps.com serves it (range requests), drawn light and dark as a pack and as folders, then
# read back with the panel's own PMTiles reader and decoders (sim/tiletest).
# Needs: npm install done in tools/mappack; python3 with pmtiles, mapbox-vector-tile and shapely;
# on Linux, xvfb-run. The labels' fonts and icons come from GitHub (or pass ASSETS=<folder>).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
SIM=$(cd "$HERE/../../sim" && pwd)
T=$(mktemp -d)
python3 -I "$HERE/test/make_vector_fixture.py" "$T/vector.pmtiles"
python3 -I "$HERE/test/range_server.py" "$T" 8765 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$T"' EXIT
sleep 1
RUN=node
command -v xvfb-run >/dev/null && RUN="xvfb-run -a node"
for style in light dark; do
  name=mappack
  [ "$style" = dark ] && name=mappack-dark
  (cd "$HERE" && $RUN make-map-pack.mjs --name "$name" --radius 12 --zoom 6-14 --style "$style" --format both \
     --source http://127.0.0.1:8765/vector.pmtiles --work "$T/work" --out "$T/card-$style" ${ASSETS:+--assets "$ASSETS"})
  python3 -I "$HERE/test/write_expect.py" "$T/card-$style/maps" "$name" "$SIM/tiletest/out"
done
(cd "$HERE" && node test/writer-test.mjs "$SIM/tiletest/out")
make -C "$SIM" tiles-test
