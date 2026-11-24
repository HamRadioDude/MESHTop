"""Fixtures for tiles_test.cpp: PMTiles v3 archives written by the reference Python library, plus the
expected tile ids and bytes, so the panel's reader is checked against the real format."""
import gzip, io, json, os, random
from PIL import Image
from pmtiles.writer import Writer
from pmtiles.tile import zxy_to_tileid, TileType, Compression

random.seed(7)
os.makedirs("out", exist_ok=True)

def png(rgb):
    b = io.BytesIO(); Image.new("RGB", (256, 256), rgb).save(b, "PNG"); return b.getvalue()
def jpg(rgb):
    b = io.BytesIO(); Image.new("RGB", (256, 256), rgb).save(b, "JPEG", quality=90); return b.getvalue()

def build(path, kind, zooms, dark=False):
    """All tiles of a small area at each zoom; each tile's bytes are distinct (a varying colour)."""
    entries = []
    for z in zooms:
        n = 2 ** z
        # a block around Chicago's tile at this zoom
        import math
        lat, lon = 41.8781, -87.6298
        cx = int((lon + 180) / 360 * n)
        s = math.sin(math.radians(lat)); cy = int((0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n)
        r = min(n // 2, {6: 1, 8: 2, 10: 4, 12: 12, 13: 30, 14: 60}.get(z, 2))
        for x in range(max(0, cx - r), min(n, cx + r + 1)):
            for y in range(max(0, cy - r), min(n, cy + r + 1)):
                entries.append((z, x, y))
    entries.sort(key=lambda t: zxy_to_tileid(*t))
    tiles = {}
    with open(path, "wb") as f:
        w = Writer(f)
        for i, (z, x, y) in enumerate(entries):
            col = (20, 24, 30) if dark else (225 + i % 25, 222, 214)  # light packs stay light
            # most tiles are tiny distinct blobs (only the directories matter for them); a few real images
            if i % 997 == 0 or (z, x, y) in [(6, 16, 23), (8, 65, 95)]:
                data = (png if kind == "png" else jpg)(col)
            else:
                # random lengths, so the directory doesn't compress away and leaf directories get used
                data = (b"\x89PNG-fake-" if kind == "png" else b"\xff\xd8fake-") + str(i).encode() + os.urandom(random.randrange(40, 900))
            tiles[(z, x, y)] = data
            w.write_tile(zxy_to_tileid(z, x, y), data)
        lat0, lon0 = 41.8781, -87.6298
        w.finalize({
            "tile_type": TileType.PNG if kind == "png" else TileType.JPEG,
            "tile_compression": Compression.NONE,
            "min_zoom": min(zooms), "max_zoom": max(zooms),
            "min_lon_e7": int((lon0 - 1.8) * 1e7), "min_lat_e7": int((lat0 - 1.3) * 1e7),
            "max_lon_e7": int((lon0 + 1.8) * 1e7), "max_lat_e7": int((lat0 + 1.3) * 1e7),
            "center_zoom": 10, "center_lon_e7": int(lon0 * 1e7), "center_lat_e7": int(lat0 * 1e7),
        }, {"name": os.path.basename(path)})
    return tiles

expect = {}
for name, kind, zooms, dark in [("big", "png", [6, 8, 10, 12, 13, 14], False), ("jpg", "jpg", [6, 8, 10], False),
                                ("dark", "png", [6, 8], True)]:
    tiles = build(f"out/{name}.pmtiles", kind, zooms, dark)
    keys = list(tiles.keys())
    sample = random.sample(keys, min(400, len(keys))) + [(6, 16, 23), (8, 65, 95)]
    os.makedirs(f"out/{name}", exist_ok=True)
    lines = []
    for i, (z, x, y) in enumerate(sample):
        fn = f"out/{name}/{z}_{x}_{y}.bin"
        open(fn, "wb").write(tiles[(z, x, y)])
        lines.append(f"{z} {x} {y} {zxy_to_tileid(z, x, y)} {fn}")
    # tiles that are not in the archive
    for (z, x, y) in [(14, 0, 0), (12, 4000, 4000), (5, 8, 11)]:
        lines.append(f"{z} {x} {y} {zxy_to_tileid(z, x, y)} -")
    open(f"out/{name}.expect", "w").write("\n".join(lines) + "\n")
    print(name, len(tiles), "tiles")

# Hilbert ids over a range of zooms
with open("out/ids.txt", "w") as f:
    for z in range(0, 17):
        n = 2 ** z
        for _ in range(60):
            x, y = random.randrange(n), random.randrange(n)
            f.write(f"{z} {x} {y} {zxy_to_tileid(z, x, y)}\n")
# gzip round trip
payload = os.urandom(3000) + b"MESHtop" * 500
open("out/blob.gz", "wb").write(gzip.compress(payload))
open("out/blob.raw", "wb").write(payload)
print("fixtures ok")

# Pictures with detail, in each PNG colour type and as a JPEG, with PIL's decode as the reference: a
# decoder that scrambles rows or swaps colours fails these (solid-colour tiles can't show that).
def pattern():
    im = Image.new("RGB", (256, 256))
    px = im.load()
    for y in range(256):
        for x in range(256):
            px[x, y] = ((x * 3 + y) % 256, (y * 5) % 256, (x ^ y) % 256)
    return im
base = pattern()
variants = {"rgb": base, "pal": base.quantize(64).convert("P"), "rgba": base.convert("RGBA"), "gray": base.convert("L")}
for name, im in variants.items():
    im.save(f"out/pix_{name}.png")
    open(f"out/pix_{name}.rgb", "wb").write(Image.open(f"out/pix_{name}.png").convert("RGB").tobytes())
base.save("out/pix_jpg.jpg", quality=92)
open("out/pix_jpg.rgb", "wb").write(Image.open("out/pix_jpg.jpg").convert("RGB").tobytes())
print("pixel fixtures written")
