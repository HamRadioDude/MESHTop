"""Stand-in map data for testing make-map-pack.mjs without the internet: a small vector PMTiles in
the Protomaps basemap v4 layout (earth, water, landuse, roads, places, buildings) around Chicago.
Shapes are rough on purpose; they only need to exercise the pipeline and look like a map."""
import gzip, math, sys
import mapbox_vector_tile
from shapely.geometry import LineString, Point, Polygon, box, mapping
from shapely.ops import transform
from pmtiles.tile import zxy_to_tileid, TileType, Compression
from pmtiles.writer import Writer

OUT = sys.argv[1] if len(sys.argv) > 1 else "vector.pmtiles"
LAT, LON, RADIUS_MI = 41.8781, -87.6298, 22
ZMIN, ZMAX = 0, 13
R = 6378137.0

def merc(lon, lat):
    return lon * math.pi / 180 * R, math.log(math.tan(math.pi / 4 + lat * math.pi / 360)) * R
def m(geom):
    return transform(lambda xs, ys, zs=None: tuple(zip(*[merc(x, y) for x, y in zip(xs, ys)])), geom)

dlat = RADIUS_MI / 69.0
dlon = RADIUS_MI / (69.17 * math.cos(math.radians(LAT)))
W, S, E, N = LON - dlon, LAT - dlat, LON + dlon, LAT + dlat
shore = [(-87.80, 42.40), (-87.67, 42.05), (-87.645, 41.95), (-87.615, 41.88), (-87.58, 41.80), (-87.52, 41.70), (-87.40, 41.58), (-87.0, 41.45)]
land = Polygon([(W - 1, N + 1)] + [(x, y) for x, y in shore] + [(E + 1, S - 1), (W - 1, S - 1)])
lake = Polygon([(x, y) for x, y in shore] + [(E + 1, S - 1), (E + 1, N + 1), (shore[0][0], N + 1)])
feats = {"earth": [], "water": [], "landuse": [], "roads": [], "places": [], "buildings": []}
feats["earth"].append((land, {"kind": "earth", "min_zoom": 0}))
feats["water"].append((lake, {"kind": "lake", "name": "Lake Michigan", "min_zoom": 0}))
feats["water"].append((LineString([(-87.615, 41.888), (-87.636, 41.887), (-87.64, 41.88), (-87.66, 41.85), (-87.70, 41.83)]),
                       {"kind": "river", "name": "Chicago River", "min_zoom": 8}))
for name, b in (("Grant Park", (-87.625, 41.865, -87.615, 41.886)), ("Lincoln Park", (-87.645, 41.91, -87.627, 41.95)),
                ("Washington Park", (-87.62, 41.785, -87.605, 41.80)), ("Humboldt Park", (-87.71, 41.897, -87.695, 41.91))):
    feats["landuse"].append((box(*b), {"kind": "park", "name": name, "min_zoom": 8}))
mile_lat, mile_lon = 1 / 69.0, 1 / (69.17 * math.cos(math.radians(LAT)))
ns = ["State St", "Halsted St", "Ashland Ave", "Western Ave", "Kedzie Ave", "Pulaski Rd", "Cicero Ave", "Central Ave", "Harlem Ave"]
ew = ["Madison St", "Chicago Ave", "North Ave", "Fullerton Ave", "Belmont Ave", "Irving Park Rd", "Lawrence Ave", "Foster Ave", "Devon Ave"]
ews = ["Roosevelt Rd", "Cermak Rd", "31st St", "Pershing Rd", "47th St", "55th St", "63rd St", "71st St", "79th St"]
for i in range(-12, 13):
    lon = LON + i * mile_lon
    nm = ns[i] if 0 <= i < len(ns) else f"{abs(i) * 800} {'W' if i > 0 else 'E'}"
    feats["roads"].append((LineString([(lon, S), (lon, N)]), {"kind": "major_road", "kind_detail": "primary", "name": nm, "min_zoom": 8}))
    lat = LAT + i * mile_lat
    nm = ew[i] if 0 <= i < len(ew) else (ews[-i - 1] if 0 < -i <= len(ews) else f"{abs(i) * 800} {'N' if i > 0 else 'S'}")
    feats["roads"].append((LineString([(W, lat), (E, lat)]), {"kind": "major_road", "kind_detail": "primary", "name": nm, "min_zoom": 8}))
for i in range(-40, 41):
    lon, lat = LON + i * mile_lon / 8, LAT + i * mile_lat / 8
    feats["roads"].append((LineString([(lon, LAT - 5 * mile_lat), (lon, LAT + 5 * mile_lat)]), {"kind": "minor_road", "kind_detail": "residential", "min_zoom": 12}))
    feats["roads"].append((LineString([(LON - 5 * mile_lon, lat), (LON + 5 * mile_lon, lat)]), {"kind": "minor_road", "kind_detail": "residential", "min_zoom": 12}))
for ref, pts in (("90", [(-87.633, S), (-87.633, 41.89), (-87.66, 41.91), (-87.75, 41.97), (W, 42.03)]),
                 ("290", [(-87.633, 41.875), (W, 41.875)]), ("55", [(-87.63, 41.855), (-87.75, 41.81), (W, 41.75)])):
    feats["roads"].append((LineString(pts), {"kind": "highway", "kind_detail": "motorway", "ref": ref, "network": "US:I", "shield_text": ref, "min_zoom": 5}))
feats["roads"].append((LineString([(x - 0.006, y) for x, y in shore[1:6]]), {"kind": "major_road", "kind_detail": "trunk", "name": "Lake Shore Dr", "min_zoom": 8}))
for name, lon, lat, rank, mz in (("Chicago", -87.6298, 41.8781, 15, 3), ("Evanston", -87.6877, 42.0451, 11, 9), ("Oak Park", -87.7845, 41.885, 11, 9),
                                 ("Cicero", -87.7539, 41.8456, 10, 10), ("Skokie", -87.7334, 42.0324, 10, 10), ("Hammond", -87.5, 41.583, 10, 9)):
    feats["places"].append((Point(lon, lat), {"kind": "locality", "name": name, "name:en": name, "population_rank": rank, "min_zoom": mz}))
for name, lon, lat in (("Lincoln Park", -87.65, 41.925), ("Hyde Park", -87.59, 41.795), ("Pilsen", -87.66, 41.855), ("Wicker Park", -87.677, 41.909)):
    feats["places"].append((Point(lon, lat), {"kind": "neighbourhood", "kind_detail": "neighbourhood", "name": name, "name:en": name, "min_zoom": 12}))
for i in range(12):
    for j in range(8):
        x0, y0 = -87.637 + i * 0.0018, 41.876 + j * 0.0016
        feats["buildings"].append((box(x0, y0, x0 + 0.0011, y0 + 0.001), {"kind": "building", "height": 20 + 10 * ((i * j) % 7), "min_zoom": 13}))
feats = {k: [(m(g), p) for g, p in v] for k, v in feats.items()}

def tile_bounds(z, x, y):
    n = 2 ** z
    size = 2 * math.pi * R / n
    minx = -math.pi * R + x * size
    maxy = math.pi * R - y * size
    return minx, maxy - size, minx + size, maxy

def tiles_for(z):
    n = 2 ** z
    x0 = int((W + 180) / 360 * n); x1 = int((E + 180) / 360 * n)
    def ty(lat):
        r = math.radians(lat)
        return int((1 - math.log(math.tan(r) + 1 / math.cos(r)) / math.pi) / 2 * n)
    return [(x, y) for x in range(max(0, x0), min(n - 1, x1) + 1) for y in range(max(0, ty(N)), min(n - 1, ty(S)) + 1)]

out = []
for z in range(ZMIN, ZMAX + 1):
    for x, y in tiles_for(z):
        b = tile_bounds(z, x, y)
        pad = (b[2] - b[0]) * 0.06
        clip = box(b[0] - pad, b[1] - pad, b[2] + pad, b[3] + pad)
        layers = []
        for name, fs in feats.items():
            items = []
            for g, p in fs:
                if p.get("min_zoom", 0) > z or not g.intersects(clip):
                    continue
                g2 = g if g.geom_type == "Point" else g.intersection(clip)
                if g2.is_empty:
                    continue
                items.append({"geometry": g2.wkt, "properties": p})
            if items:
                layers.append({"name": name, "features": items})
        data = mapbox_vector_tile.encode(layers, default_options={"quantize_bounds": b, "extents": 4096})
        out.append((zxy_to_tileid(z, x, y), gzip.compress(data)))
out.sort()
with open(OUT, "wb") as f:
    w = Writer(f)
    for tid, data in out:
        w.write_tile(tid, data)
    w.finalize({"tile_type": TileType.MVT, "tile_compression": Compression.GZIP, "min_zoom": ZMIN, "max_zoom": ZMAX,
                "min_lon_e7": int(W * 1e7), "min_lat_e7": int(S * 1e7), "max_lon_e7": int(E * 1e7), "max_lat_e7": int(N * 1e7),
                "center_zoom": 10, "center_lon_e7": int(LON * 1e7), "center_lat_e7": int(LAT * 1e7)},
               {"name": "stand-in", "attribution": "test data", "vector_layers": [{"id": k, "fields": {}} for k in feats]})
print(f"{OUT}: {len(out)} vector tiles, z{ZMIN}-{ZMAX}")
