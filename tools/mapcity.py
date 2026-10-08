#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Port Alder, a made-up city, written as a PMTiles archive of vector tiles.

    python3 tools/mapcity.py out.pmtiles

Maps' region for the image and for its tests (`docs/maps.md`, steps M2 and
M3): a city invented for the drawing - its bay, river, parks, streets,
buildings and places - standing on Null Island, 0 degrees 0 degrees, the
point every mapper knows is nowhere, so it cannot be taken for a real one.

**Written exactly as a real region is**, so the made-up city and an
OpenStreetMap extract go through the same code: PMTiles version 3 (its
header, its directory, tiles found by their Hilbert number), each tile a
Mapbox Vector Tile (protobuf) in the OpenMapTiles schema that OpenFreeMap
serves - layers `water`, `waterway`, `park`, `landuse`, `building`,
`transportation`, `transportation_name`, `place`, `water_name` and `poi` -
and both the directory and the tiles gzipped, as Protomaps' and
OpenFreeMap's are. Nothing here is Kosmos's own invention but the city.

Deterministic: the same bytes every run, so the image does not change when
nothing did, and a host test can hold the Map Kit to what this wrote.
"""

import gzip
import io
import json
import math
import struct
import sys

R = 6378137.0                       # Web Mercator's sphere, in metres
EXTENT = 4096                       # a tile's units, as every MVT has
BUFFER = 64                         # drawn past the edge, so seams join
MINZOOM, MAXZOOM = 11, 16

# --------------------------------------------------------------------------
# The city, in metres east and north of Null Island. It leans eight degrees,
# as the drawing's streets do.
# --------------------------------------------------------------------------

LEAN = math.radians(-8)


def lean(x, y):
    c, s = math.cos(LEAN), math.sin(LEAN)
    return (x * c - y * s, x * s + y * c)


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def leaned(points):
    return [lean(x, y) for x, y in points]


def circle(cx, cy, r, n=24):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
            for i in range(n)]


FEATURES = []                       # (layer, kind, geometry, properties, minzoom)


def add(layer, kind, geometry, props, minzoom=MINZOOM):
    FEATURES.append((layer, kind, geometry, props, minzoom))


# The bay, to the south-west, and the open sea beyond it.
add("water", "polygon", [[(-3200, -2600), (-600, -2600), (-300, -1700), (-900, -1150),
                          (-1700, -900), (-2400, -1000), (-3200, -700)]],
    {"class": "ocean"})
add("water_name", "point", (-1900, -1800), {"name": "Alder Bay", "class": "bay"}, 12)

# The Silver River, from the north-east down into the bay: a band of water.
river = [(2600, 2400), (2100, 1700), (1500, 1100), (900, 500), (300, -200), (-400, -900), (-700, -1200)]


def band(line, half):
    left, right = [], []

    for i, (x, y) in enumerate(line):
        a = line[max(0, i - 1)]
        b = line[min(len(line) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = math.hypot(dx, dy) or 1
        nx, ny = -dy / n * half, dx / n * half
        left.append((x + nx, y + ny))
        right.append((x - nx, y - ny))

    return left + right[::-1]


add("water", "polygon", [band(river, 70)], {"class": "river"})
add("waterway", "line", [river], {"class": "river", "name": "Silver River"}, 13)
add("water_name", "line", [river[1:4]], {"name": "Silver River", "class": "river"}, 13)

# Parks.
add("park", "polygon", [leaned(rect(-1500, 900, -700, 1500))], {"class": "park", "name": "Cedar Park"})
add("park", "polygon", [[(1300, -1500), (2100, -1600), (2300, -900), (1500, -800)]],
    {"class": "park", "name": "Gull Point"})
add("park", "polygon", [circle(*lean(150, 150), 120)], {"class": "park", "name": "Lantern Square"}, 13)
add("landuse", "polygon", [leaned(rect(-2400, 300, -1700, 1900))], {"class": "residential"})
add("landuse", "polygon", [leaned(rect(600, 900, 1500, 1800))], {"class": "residential"})

# The streets: a grid every 150 metres, leaned, between -1650 and +1650.
STREET = 150
AVENUES = ["Alder", "Birch", "Cedar", "Dock", "Elm", "Ferry", "Granary", "Harbour",
           "Iron", "Juniper", "Kiln", "Lantern", "Mill", "Net", "Oar", "Pier",
           "Quay", "Rope", "Sail", "Tide", "Union", "Vane", "Wharf"]


def street_class(i):
    if i == 0:
        return "primary"
    if i % 4 == 0:
        return "secondary"
    return "minor"


for k, i in enumerate(range(-11, 12)):
    x = i * STREET
    cls = "primary" if i == 0 else street_class(i)
    name = ("Lantern Street" if i == 0 else AVENUES[k] + " Street")
    add("transportation", "line", [leaned([(x, -1650), (x, 1650)])],
        {"class": cls}, 11 if cls == "primary" else (12 if cls == "secondary" else 13))
    add("transportation_name", "line", [leaned([(x, -1650), (x, 1650)])],
        {"name": name, "class": cls}, 14 if cls == "minor" else 13)

for k, j in enumerate(range(-11, 12)):
    y = j * STREET
    cls = "primary" if j == 0 else street_class(j)
    name = "Harbour Road" if j == 0 else "%d%s Avenue" % (k + 1, {1: "st", 2: "nd", 3: "rd"}.get(
        (k + 1) % 10 if (k + 1) % 100 not in (11, 12, 13) else 0, "th"))
    add("transportation", "line", [leaned([(-1650, y), (1650, y)])],
        {"class": cls}, 11 if cls == "primary" else (12 if cls == "secondary" else 13))
    add("transportation_name", "line", [leaned([(-1650, y), (1650, y)])],
        {"name": name, "class": cls}, 14 if cls == "minor" else 13)

# The Ring Road, a motorway sweeping round the north and east.
ring = [(-3000, 600), (-2000, 1800), (-800, 2400), (600, 2500), (1800, 2000),
        (2600, 1000), (2900, -200), (2800, -1400)]
add("transportation", "line", [ring], {"class": "motorway"})
add("transportation_name", "line", [ring[2:5]], {"name": "Ring Road", "class": "motorway"}, 12)

# Paths through the parks.
add("transportation", "line", [leaned([(-1450, 950), (-1100, 1250), (-750, 1450)])],
    {"class": "path"}, 15)

def near_river(x, y, within):
    """Whether (x, y) is within `within` metres of the river's line."""
    for (ax, ay), (bx, by) in zip(river, river[1:]):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
        if math.hypot(ax + t * dx - x, ay + t * dy - y) < within:
            return True
    return False


# Buildings: four to a block, inside the streets, from zoom 14.
for i in range(-11, 11):
    for j in range(-11, 11):
        x0, y0 = i * STREET, j * STREET
        cx, cy = x0 + STREET / 2, y0 + STREET / 2
        # Not where a park, the square or the river is.
        if -1500 <= cx <= -700 and 900 <= cy <= 1500:
            continue
        if abs(cx - 150) < 200 and abs(cy - 150) < 200:
            continue
        if (i * 7 + j * 3) % 11 == 0:
            continue
        # Nor in the river: a block whose middle, leaned, is near its line.
        if near_river(*lean(cx, cy), 200):
            continue

        for bx, by in ((0, 0), (1, 0), (0, 1), (1, 1)):
            px0 = x0 + 18 + bx * 60
            py0 = y0 + 18 + by * 60
            add("building", "polygon", [leaned(rect(px0, py0, px0 + 48 - (i + j) % 3 * 6,
                                                    py0 + 48 - (i * j) % 2 * 8))], {}, 14)

# Places.
add("place", "point", (0, 0), {"name": "Port Alder", "class": "city", "rank": 1}, 11)
for name, x, y in (("Old Harbour", -600, -500), ("Civic Quarter", 700, 900),
                   ("Northside", -900, 1900), ("Gull Point", 1800, -1200)):
    add("place", "point", lean(x, y), {"name": name, "class": "suburb", "rank": 3}, 13)

# Points of interest.
POIS = [
    ("Lantern Street Market", "marketplace", 20, -330),
    ("Alder Library", "library", 820, 780),
    ("Bayview Lookout", "viewpoint", 1750, -1350),
    ("Ferry Terminal", "ferry_terminal", -700, -900),
    ("Port Alder Station", "railway", 300, 600),
    ("Harbour Cafe", "cafe", -380, -420),
    ("Tidewater Museum", "museum", 480, 120),
    ("Cedar Park Pond", "drinking_water", -1100, 1200),
    ("Alder Bakery", "bakery", -150, 450),
    ("Northside School", "school", -900, 1700),
    ("Gull Point Lighthouse", "lighthouse", 2050, -1450),
    ("Old Harbour Pharmacy", "pharmacy", -500, -150),
    ("Civic Hall", "townhall", 600, 1050),
    ("Rope Walk Cinema", "cinema", 1050, 300),
    ("Pier Market Hall", "marketplace", -800, -700),
]
for name, cls, x, y in POIS:
    add("poi", "point", lean(x, y), {"name": name, "class": cls, "rank": 1}, 14)

# --------------------------------------------------------------------------
# Metres to a tile's units, and clipping to a tile.
# --------------------------------------------------------------------------


def world(x, y, z):
    """Metres (Mercator, which at the equator they are) to pixels of a world
    `2^z` tiles wide, each EXTENT units."""
    size = (2 ** z) * EXTENT
    return ((x + math.pi * R) / (2 * math.pi * R) * size,
            (math.pi * R - y) / (2 * math.pi * R) * size)


def clip_polygon(ring, lo, hi):
    """Sutherland-Hodgman against the square [lo, hi]."""
    def clip(points, inside, cut):
        out = []

        for i, p in enumerate(points):
            q = points[i - 1]

            if inside(p):
                if not inside(q):
                    out.append(cut(q, p))
                out.append(p)
            elif inside(q):
                out.append(cut(q, p))

        return out

    def at_x(xv):
        return lambda a, b: (xv, a[1] + (b[1] - a[1]) * (xv - a[0]) / (b[0] - a[0]))

    def at_y(yv):
        return lambda a, b: (a[0] + (b[0] - a[0]) * (yv - a[1]) / (b[1] - a[1]), yv)

    pts = ring
    for inside, cut in ((lambda p: p[0] >= lo, at_x(lo)), (lambda p: p[0] <= hi, at_x(hi)),
                        (lambda p: p[1] >= lo, at_y(lo)), (lambda p: p[1] <= hi, at_y(hi))):
        if not pts:
            break
        pts = clip(pts, inside, cut)

    return pts


def clip_line(line, lo, hi):
    """A line cut to [lo, hi]: the pieces of it inside, each a line."""
    def code(p):
        return ((p[0] < lo) | (p[0] > hi) << 1 | (p[1] < lo) << 2 | (p[1] > hi) << 3)

    pieces, current = [], []

    for a, b in zip(line, line[1:]):
        p, q = a, b
        cp, cq = code(p), code(q)

        while True:
            if not (cp | cq):
                if not current or current[-1] != p:
                    if current:
                        pieces.append(current)
                    current = [p]
                current.append(q)
                if cq or q != b:
                    pieces.append(current)
                    current = []
                break
            if cp & cq:
                if current:
                    pieces.append(current)
                    current = []
                break
            c = cp or cq
            if c & 1:
                x, y = lo, p[1] + (q[1] - p[1]) * (lo - p[0]) / (q[0] - p[0])
            elif c & 2:
                x, y = hi, p[1] + (q[1] - p[1]) * (hi - p[0]) / (q[0] - p[0])
            elif c & 4:
                x, y = p[0] + (q[0] - p[0]) * (lo - p[1]) / (q[1] - p[1]), lo
            else:
                x, y = p[0] + (q[0] - p[0]) * (hi - p[1]) / (q[1] - p[1]), hi
            if c == cp:
                p, cp = (x, y), code((x, y))
            else:
                q, cq = (x, y), code((x, y))

    if len(current) > 1:
        pieces.append(current)

    return [piece for piece in pieces if len(piece) > 1]


def signed_area(ring):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:] + ring[:1])) / 2

# --------------------------------------------------------------------------
# Protobuf, as much of it as a vector tile uses.
# --------------------------------------------------------------------------


def varint(n):
    out = bytearray()

    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def key(field, wire):
    return varint(field << 3 | wire)


def lendelim(field, payload):
    return key(field, 2) + varint(len(payload)) + payload


def zigzag(n):
    return (n << 1) ^ (n >> 31)


def packed(field, values):
    return lendelim(field, b"".join(varint(v) for v in values))


def geometry(kind, parts):
    """MVT geometry commands for points, lines or rings, in tile units."""
    out, cx, cy = [], 0, 0

    def move(p):
        nonlocal cx, cy
        x, y = int(round(p[0])), int(round(p[1]))
        dx, dy = x - cx, y - cy
        cx, cy = x, y
        return [zigzag(dx), zigzag(dy)]

    if kind == "point":
        out.append(1 | 1 << 3)
        out += move(parts)
        return out

    for part in parts:
        pts = [(int(round(x)), int(round(y))) for x, y in part]
        # Drop repeats, which a rounded clip leaves.
        clean = [pts[0]]
        for p in pts[1:]:
            if p != clean[-1]:
                clean.append(p)
        if kind == "polygon" and len(clean) > 1 and clean[0] == clean[-1]:
            clean.pop()
        if (kind == "line" and len(clean) < 2) or (kind == "polygon" and len(clean) < 3):
            continue
        out.append(1 | 1 << 3)
        out += move(clean[0])
        out.append(2 | (len(clean) - 1) << 3)
        for p in clean[1:]:
            out += move(p)
        if kind == "polygon":
            out.append(7 | 1 << 3)

    return out


def value(v):
    if isinstance(v, str):
        return lendelim(1, v.encode())
    if isinstance(v, int):
        return key(5, 0) + varint(v)
    raise TypeError(v)


def layer_bytes(name, feats):
    keys, values, kidx, vidx = [], [], {}, {}
    body = bytearray()

    for fid, (kind, geom, props) in enumerate(feats, 1):
        tags = []
        for k, v in sorted(props.items()):
            if k not in kidx:
                kidx[k] = len(keys)
                keys.append(k)
            vk = (type(v).__name__, v)
            if vk not in vidx:
                vidx[vk] = len(values)
                values.append(v)
            tags += [kidx[k], vidx[vk]]
        f = key(1, 0) + varint(fid)
        if tags:
            f += packed(2, tags)
        f += key(3, 0) + varint({"point": 1, "line": 2, "polygon": 3}[kind])
        f += packed(4, geom)
        body += lendelim(2, f)

    out = key(15, 0) + varint(2) + lendelim(1, name.encode()) + bytes(body)
    for k in keys:
        out += lendelim(3, k.encode())
    for v in values:
        out += lendelim(4, value(v))
    out += key(5, 0) + varint(EXTENT)
    return out


LAYER_ORDER = ["water", "landuse", "park", "waterway", "building", "transportation",
               "transportation_name", "water_name", "place", "poi"]


def tile_bytes(z, tx, ty):
    lo, hi = -BUFFER, EXTENT + BUFFER
    layers = {}

    for layer, kind, geom, props, minzoom in FEATURES:
        if z < minzoom:
            continue

        def local(p):
            wx, wy = world(p[0], p[1], z)
            return (wx - tx * EXTENT, wy - ty * EXTENT)

        if kind == "point":
            p = local(geom)
            if 0 <= p[0] < EXTENT and 0 <= p[1] < EXTENT:
                layers.setdefault(layer, []).append(("point", p, props))
        elif kind == "line":
            parts = []
            for line in geom:
                parts += clip_line([local(p) for p in line], lo, hi)
            if parts:
                layers.setdefault(layer, []).append(("line", parts, props))
        else:
            rings = []
            for ring in geom:
                cut = clip_polygon([local(p) for p in ring], lo, hi)
                if len(cut) >= 3:
                    # Exterior rings clockwise on the screen: positive area
                    # by the spec's formula, y pointing down.
                    if signed_area(cut) < 0:
                        cut = cut[::-1]
                    rings.append(cut)
            if rings:
                layers.setdefault(layer, []).append(("polygon", rings, props))

    out = bytearray()
    for name in LAYER_ORDER:
        if name in layers:
            feats = []
            for kind, g, props in layers[name]:
                cmds = geometry(kind, g)
                if cmds:
                    feats.append((kind, cmds, props))
            if feats:
                out += lendelim(3, layer_bytes(name, feats))
    return bytes(out)

# --------------------------------------------------------------------------
# PMTiles version 3.
# --------------------------------------------------------------------------


def tile_id(z, x, y):
    """The tile's Hilbert number, as PMTiles counts them: every tile of the
    zooms before, then the Hilbert curve's position of (x, y) at `z`."""
    acc = ((1 << (2 * z)) - 1) // 3
    d, s = 0, (1 << z) // 2
    xy = [x, y]

    while s > 0:
        rx = 1 if xy[0] & s else 0
        ry = 1 if xy[1] & s else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                xy[0] = s - 1 - xy[0]
                xy[1] = s - 1 - xy[1]
            xy[0], xy[1] = xy[1], xy[0]
        s //= 2

    return acc + d


def directory(entries):
    out = bytearray(varint(len(entries)))
    last = 0
    for tid, _, _, _ in entries:
        out += varint(tid - last)
        last = tid
    for _, run, _, _ in entries:
        out += varint(run)
    for _, _, length, _ in entries:
        out += varint(length)
    for i, (_, _, length, offset) in enumerate(entries):
        prev = entries[i - 1] if i else None
        if prev and offset == prev[3] + prev[2]:
            out += varint(0)
        else:
            out += varint(offset + 1)
    return bytes(out)


def gz(data):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as f:
        f.write(data)
    return buf.getvalue()


def tiles_covering(z):
    x0, y0 = world(-3300, 2700, z)
    x1, y1 = world(3300, -2700, z)
    for ty in range(int(y0 // EXTENT), int(y1 // EXTENT) + 1):
        for tx in range(int(x0 // EXTENT), int(x1 // EXTENT) + 1):
            yield tx, ty


def main(path):
    tiles = []

    for z in range(MINZOOM, MAXZOOM + 1):
        for tx, ty in tiles_covering(z):
            data = tile_bytes(z, tx, ty)
            if data:
                tiles.append((tile_id(z, tx, ty), gz(data)))

    tiles.sort()
    blob, entries = bytearray(), []
    for tid, data in tiles:
        entries.append((tid, 1, len(data), len(blob)))
        blob += data

    root = gz(directory(entries))
    meta = gz(json.dumps({
        "name": "Port Alder",
        "description": "A made-up city on Null Island, for Kosmos Maps' tests - not a real place",
        "attribution": "Made up for Kosmos",
        "vector_layers": [{"id": n, "fields": {}} for n in LAYER_ORDER],
    }, sort_keys=True).encode())

    header_len = 127
    root_off = header_len
    meta_off = root_off + len(root)
    leaf_off = meta_off + len(meta)
    data_off = leaf_off

    def e7(deg):
        return int(round(deg * 1e7))

    def deg(xm, ym):
        lon = math.degrees(xm / R)
        lat = math.degrees(2 * math.atan(math.exp(ym / R)) - math.pi / 2)
        return lon, lat

    w, s_ = deg(-3300, -2700)
    e, n = deg(3300, 2700)
    header = (b"PMTiles" + bytes([3])
              + struct.pack("<QQQQQQQQ", root_off, len(root), meta_off, len(meta),
                            leaf_off, 0, data_off, len(blob))
              + struct.pack("<QQQ", len(entries), len(entries), len(entries))
              + bytes([1, 2, 2, 1, MINZOOM, MAXZOOM])
              + struct.pack("<iiii", e7(w), e7(s_), e7(e), e7(n))
              + bytes([15]) + struct.pack("<ii", 0, 0))
    assert len(header) == header_len, len(header)

    with open(path, "wb") as f:
        f.write(header + root + meta + bytes(blob))

    print("mapcity: Port Alder, %d tiles at zooms %d-%d, %d bytes, in %s"
          % (len(entries), MINZOOM, MAXZOOM, header_len + len(root) + len(meta) + len(blob), path))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "port-alder.pmtiles")
