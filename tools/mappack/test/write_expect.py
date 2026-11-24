"""After a test run of make-map-pack.mjs: copy the pack and its folder tiles into sim/tiletest/out and
write <name>.expect (z x y tile-id file), so the panel's own reader can be checked against them."""
import os, shutil, sys
from pmtiles.tile import zxy_to_tileid

maps, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
shutil.copy(os.path.join(maps, name, name + ".pmtiles"), os.path.join(out, name + ".pmtiles"))
dst = os.path.join(out, name)
shutil.rmtree(dst, ignore_errors=True)
lines, seen = [], set()
for root, _, files in os.walk(os.path.join(maps, "osm")):
    for f in files:
        z, x = (int(p) for p in os.path.relpath(root, os.path.join(maps, "osm")).split(os.sep))
        y = int(f[:-4])
        os.makedirs(os.path.join(dst, str(z), str(x)), exist_ok=True)
        shutil.copy(os.path.join(root, f), os.path.join(dst, str(z), str(x), f))
        lines.append(f"{z} {x} {y} {zxy_to_tileid(z, x, y)} out/{name}/{z}/{x}/{f}")
        seen.add((z, x, y))
# a few tiles just outside the area: the reader must say they aren't there
for z, x, y in sorted(seen)[:: max(1, len(seen) // 40)]:
    for dx in (-1, 1):
        if 0 <= x + dx * 64 < 2 ** z and (z, x + dx * 64, y) not in seen:
            lines.append(f"{z} {x + dx * 64} {y} {zxy_to_tileid(z, x + dx * 64, y)} -")
open(os.path.join(out, name + ".expect"), "w").write("\n".join(sorted(lines)) + "\n")
print(f"{name}: {len(seen)} tiles to check")
