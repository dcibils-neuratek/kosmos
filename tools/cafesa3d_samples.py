#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D's sample scenes - a house, a car, a plane - written as glTF.

    cafesa3d_samples.py OUTDIR      house.gltf, car.gltf and plane.gltf there

Diego, 25 September 2026: "When done let's create a couple of sample scenes
with 3d designs", "A house", "A car", "A plane" (`roadmap.md` 4l) - and on
seeing the first cut, made of the kit's primitives alone: "I was expecting a
much more polished scenes and complex to showcase the modeler capabilities",
"With more detail and textures". So these are the second cut. Each is made
of the kit's shapes and of **meshes of its own**, built here the way a
modeller builds them:

- **turned on a lathe** - a profile swept round an axis: a fuselage, a tyre,
  an engine's nacelle, a lamp post;
- **lofted** - sections joined along a path: a car's body with its wheel
  arches, a wing from airfoil sections with taper, sweep and dihedral;
- **rounded** - a box with its edges bevelled, so nothing has the knife
  edge a primitive has;

and every surface that is a material in the world has a **texture**: brick
and mortar that the light rakes across, roof tiles that overlap, asphalt,
grass, wood, concrete (`k3d_texture.c`).

**This file is the scenes**; the build runs it and the image carries what it
writes (`sys.asset("scenes/house.gltf")`), so nothing generated sits in the
tree. Each file is glTF 2.0 twice over:

- **as glTF says**, so another program reads it: nodes with translation, a
  rotation quaternion and scale in glTF's Y-up space, meshes as accessors
  into one base64 buffer, materials in its metallic-roughness terms (and
  `KHR_materials_transmission`, `_ior` and `_emissive_strength`), the lamp
  as `KHR_lights_punctual`, the camera looking down its -Z;
- **as Cafesa3D keeps it**, in each node's `extras.cafesa3d`: the shape and
  its own numbers, which glTF has no word for, and the object's place, turn
  and size in Cafesa3D's Z-up space exactly; and in each material's, its
  texture, which glTF has no word for either.

`tools/test_scenefile.lua` holds the two to each other.
"""

import base64
import json
import math
import os
import struct
import sys

# Cafesa3D's material presets, as the app has them.
PRESETS = {
    "Plastic": dict(metallic=0.0, rough=0.35, trans=0.0, ior=1.5, emit=0.0),
    "Metal":   dict(metallic=1.0, rough=0.22, trans=0.0, ior=1.5, emit=0.0),
    "Mirror":  dict(metallic=1.0, rough=0.0,  trans=0.0, ior=1.5, emit=0.0),
    "Glass":   dict(metallic=0.0, rough=0.0,  trans=1.0, ior=1.5, emit=0.0),
    "Light":   dict(metallic=0.0, rough=1.0,  trans=0.0, ior=1.5, emit=5.0),
}


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hex_rgb(h):
    return [int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)]


def euler_matrix(rx, ry, rz):
    """Rz Ry Rx, degrees, row by row - the kit's."""
    ax, ay, az = (math.radians(v) for v in (rx, ry, rz))
    cx, sx, cy, sy, cz, sz = (math.cos(ax), math.sin(ax), math.cos(ay), math.sin(ay),
                              math.cos(az), math.sin(az))
    return [[cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx],
            [sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx],
            [-sy, cy * sx, cy * cx]]


# Cafesa3D's Z-up to glTF's Y-up: (x, y, z) -> (x, z, -y), which is what
# Blender's exporter does.
C = [[1, 0, 0], [0, 0, 1], [0, -1, 0]]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def transpose(m):
    return [[m[j][i] for j in range(3)] for i in range(3)]


def quaternion(m):
    """A rotation matrix as glTF's [x, y, z, w]."""
    t = m[0][0] + m[1][1] + m[2][2]

    if t > 0:
        s = math.sqrt(t + 1) * 2
        w, x = s / 4, (m[2][1] - m[1][2]) / s
        y, z = (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2
        w, x = (m[2][1] - m[1][2]) / s, s / 4
        y, z = (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = math.sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2
        w, x = (m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s
        y, z = s / 4, (m[1][2] + m[2][1]) / s
    else:
        s = math.sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2
        w, x = (m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s
        y, z = (m[1][2] + m[2][1]) / s, s / 4

    return [round(v, 7) for v in (x, y, z, w)]


def to_gltf(p):
    return [p[0], p[2], -p[1]]


# --------------------------------------------------------------------------
# Meshes, as a modeller makes them: points, and triangles of indices into
# them, wound anticlockwise from outside.
# --------------------------------------------------------------------------

def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


class Mesh:
    def __init__(self):
        self.v, self.t = [], []

    def point(self, p):
        self.v.append(tuple(float(c) for c in p))
        return len(self.v) - 1

    def face(self, a, b, c, out=None):
        """A triangle; turned round if `out` says its outside is the other
        way, and left out if it has no area."""
        n = cross(sub(self.v[b], self.v[a]), sub(self.v[c], self.v[a]))

        if dot(n, n) < 1e-18:
            return

        if out is not None and dot(n, out) < 0:
            b, c = c, b

        self.t.append((a, b, c))

    def quad(self, a, b, c, d, out=None):
        self.face(a, b, c, out)
        self.face(a, c, d, out)

    def weld(self, places=5):
        """Points in the same place made one, so a surface built in pieces is
        one surface to the kit's auto smooth, which finds neighbours by index."""
        seen, remap, kept = {}, [], []

        for p in self.v:
            key = tuple(round(c, places) for c in p)

            if key not in seen:
                seen[key] = len(kept)
                kept.append(p)

            remap.append(seen[key])

        self.v = kept
        self.t = [(remap[a], remap[b], remap[c]) for a, b, c in self.t
                  if len({remap[a], remap[b], remap[c]}) == 3]
        return self


def lathe(profile, segments=48):
    """A profile of (radius, height) swept round Z. Listed up the outside, a
    closed loop comes out a closed solid; a radius of nought is a pole."""
    m = Mesh()
    rings = []

    for r, z in profile:
        if r < 1e-9:
            rings.append([m.point((0, 0, z))] * segments)
        else:
            rings.append([m.point((r * math.cos(2 * math.pi * j / segments),
                                   r * math.sin(2 * math.pi * j / segments), z))
                          for j in range(segments)])

    for i in range(len(profile) - 1):
        (r0, z0), (r1, z1) = profile[i], profile[i + 1]
        nr, nz = (z1 - z0), -(r1 - r0)         # the profile's outward normal

        for j in range(segments):
            k = (j + 1) % segments
            mid = 2 * math.pi * (j + 0.5) / segments
            out = (nr * math.cos(mid), nr * math.sin(mid), nz)
            m.quad(rings[i][j], rings[i][k], rings[i + 1][k], rings[i + 1][j], out)

    return m.weld()


def loft(rings, caps=True):
    """Closed sections of the same number of points, joined in order; the
    ends closed with a fan when `caps`."""
    m = Mesh()
    ids = [[m.point(p) for p in ring] for ring in rings]
    n = len(rings[0])
    centre = [tuple(sum(p[a] for p in ring) / n for a in range(3)) for ring in rings]

    for i in range(len(rings) - 1):
        for j in range(n):
            k = (j + 1) % n
            mid = tuple((rings[i][j][a] + rings[i + 1][k][a]) / 2 for a in range(3))
            axis = tuple((centre[i][a] + centre[i + 1][a]) / 2 for a in range(3))
            m.quad(ids[i][j], ids[i][k], ids[i + 1][k], ids[i + 1][j], sub(mid, axis))

    if caps:
        for end, other in ((0, 1), (len(rings) - 1, len(rings) - 2)):
            c = m.point(centre[end])
            out = sub(centre[end], centre[other])

            for j in range(n):
                m.face(c, ids[end][j], ids[end][(j + 1) % n], out)

    return m.weld()


def rounded_box(size, r, seg=3):
    """A box `size` across, its edges rounded to `r`: a cube's faces cut
    finely near their edges and each point pushed out onto the rounding."""
    h = [s / 2 for s in size]
    r = min(r, *[x * 0.99 for x in h])

    def cuts(half):
        near = [-half + r - r * math.cos(math.pi / 2 * k / seg) for k in range(seg + 1)]
        return near + [-x for x in reversed(near)]

    m = Mesh()
    inner = [x - r for x in h]

    def place(p):
        q = [max(-inner[a], min(inner[a], p[a])) for a in range(3)]
        d = [p[a] - q[a] for a in range(3)]
        l = math.sqrt(sum(x * x for x in d)) or 1
        return tuple(q[a] + r * d[a] / l for a in range(3))

    for axis in range(3):
        u, v = [a for a in range(3) if a != axis]

        for sign in (-1, 1):
            grid = []

            for cu in cuts(h[u]):
                row = []

                for cv in cuts(h[v]):
                    p = [0, 0, 0]
                    p[axis], p[u], p[v] = sign * h[axis], cu, cv
                    row.append(m.point(place(p)))

                grid.append(row)

            out = [0, 0, 0]
            out[axis] = sign

            for i in range(len(grid) - 1):
                for j in range(len(grid[0]) - 1):
                    m.quad(grid[i][j], grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1],
                           tuple(out))

    return m.weld()


def naca(t=0.12, camber=0.02, at=0.4, n=24):
    """A NACA four-digit airfoil, a unit chord along X, thickness along Z:
    upper surface back to front, lower front to back - a closed loop."""
    upper, lower = [], []

    for i in range(n + 1):
        b = (1 - math.cos(math.pi * i / n)) / 2                 # bunched at the ends
        yt = 5 * t * (0.2969 * math.sqrt(b) - 0.126 * b - 0.3516 * b * b
                      + 0.2843 * b ** 3 - 0.1036 * b ** 4)

        if b < at:
            yc = camber / at ** 2 * (2 * at * b - b * b)
        else:
            yc = camber / (1 - at) ** 2 * (1 - 2 * at + 2 * at * b - b * b)

        upper.append((b, yc + yt))
        lower.append((b, yc - yt))

    return list(reversed(upper)) + lower[1:-1]


def wing(stations, section):
    """A wing lofted along Y: each station (y, x of its leading edge, z,
    chord), the airfoil `section` at that chord, trailing back along -X,
    which is aft on a plane whose nose points along +X."""
    rings = [[(x0 - c * sx, y, z0 + c * sz) for sx, sz in section]
             for y, x0, z0, c in stations]
    return loft(rings)


def superellipse(a, b, n, count):
    """A rounded rectangle, `a` across and `b` up from its middle: |y/a|^n +
    |z/b|^n = 1, which is a circle at two and near a box at eight."""
    out = []

    for k in range(count):
        t = 2 * math.pi * k / count
        c, s = math.cos(t), math.sin(t)
        out.append((a * math.copysign(abs(c) ** (2 / n), c),
                    b * math.copysign(abs(s) ** (2 / n), s)))

    return out


# --------------------------------------------------------------------------
# A scene, and glTF of it.
# --------------------------------------------------------------------------

class Scene:
    def __init__(self, name, camera, target, light, power=1500, lamp_radius=0.5,
                 zenith="#6d90c6", horizon="#dfe6ef", strength=0.9):
        self.name = name
        self.nodes, self.materials, self.meshes = [], [], []
        self.accessors, self.views = [], []
        self.buffer = bytearray()
        self.by_name, self.used = {}, set()
        self.camera, self.target = camera, target
        self.light, self.power, self.lamp_radius = light, power, lamp_radius
        self.world = dict(zenith=zenith, horizon=horizon, strength=strength)

    def material(self, name, base, preset="Plastic", texture=None, **over):
        if name in self.by_name:
            return self.by_name[name]

        m = dict(PRESETS[preset])
        m.update(over)
        lin = [round(srgb_to_linear(c), 6) for c in hex_rgb(base)]
        own = {"base": base, "preset": preset}

        # What the preset does not say, said: a roughness or an emission of
        # this material's own, which glTF's factors carry too.
        for key in ("rough", "metallic", "emit"):
            if key in over:
                own[key] = over[key]

        if texture:
            own["texture"] = dict(texture)

        entry = {
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": lin + [1],
                                     "metallicFactor": m["metallic"],
                                     "roughnessFactor": m["rough"]},
            "extras": {"cafesa3d": own},
        }
        ext = {}

        if m["trans"] > 0:
            ext["KHR_materials_transmission"] = {"transmissionFactor": m["trans"]}
            ext["KHR_materials_ior"] = {"ior": m["ior"]}

        if m["emit"] > 0:
            entry["emissiveFactor"] = lin
            ext["KHR_materials_emissive_strength"] = {"emissiveStrength": m["emit"]}

        if ext:
            entry["extensions"] = ext

        self.materials.append(entry)
        self.by_name[name] = len(self.materials) - 1
        return self.by_name[name]

    def unique(self, name):
        """Blender's naming: Picket, then Picket.001."""
        if name not in self.used:
            self.used.add(name)
            return name

        n = 1

        while "%s.%03d" % (name, n) in self.used:
            n += 1

        self.used.add("%s.%03d" % (name, n))
        return "%s.%03d" % (name, n)

    def node(self, name, loc, rot, scale, own, extra=None):
        R = mat_mul(mat_mul(C, euler_matrix(*rot)), transpose(C))
        node = {
            "name": self.unique(name),
            "translation": [round(v, 6) for v in to_gltf(loc)],
            "rotation": quaternion(R),
            "scale": [scale[0], scale[2], scale[1]],
            "extras": {"cafesa3d": own},
        }
        node.update(extra or {})
        self.nodes.append(node)

    def add(self, name, kind, loc, material, rot=(0, 0, 0), scale=(1, 1, 1), smooth=False,
            **shape):
        own = {"kind": kind, "loc": list(loc), "rot": list(rot), "scale": list(scale),
               "material": material, "smooth": smooth}
        own.update({k: (list(v) if isinstance(v, tuple) else v) for k, v in shape.items()})
        self.node(name, loc, rot, scale, own)

    def view(self, data, target):
        while len(self.buffer) % 4:
            self.buffer.append(0)

        self.views.append({"buffer": 0, "byteOffset": len(self.buffer),
                           "byteLength": len(data), "target": target})
        self.buffer += data
        return len(self.views) - 1

    def mesh(self, name, mesh, loc, material, rot=(0, 0, 0), scale=(1, 1, 1), smooth_angle=35):
        """A mesh of this file's own: its points in glTF's Y-up, in one
        buffer with every other mesh's, and the node that places it."""
        pts = [to_gltf(p) for p in mesh.v]
        lo = [round(min(p[a] for p in pts), 6) for a in range(3)]
        hi = [round(max(p[a] for p in pts), 6) for a in range(3)]
        positions = struct.pack("<%df" % (3 * len(pts)), *[c for p in pts for c in p])
        indices = struct.pack("<%dI" % (3 * len(mesh.t)), *[i for t in mesh.t for i in t])

        self.accessors.append({"bufferView": self.view(positions, 34962), "componentType": 5126,
                               "count": len(pts), "type": "VEC3", "min": lo, "max": hi})
        pos_at = len(self.accessors) - 1
        self.accessors.append({"bufferView": self.view(indices, 34963), "componentType": 5125,
                               "count": 3 * len(mesh.t), "type": "SCALAR"})
        self.meshes.append({"name": name, "primitives": [
            {"attributes": {"POSITION": pos_at}, "indices": len(self.accessors) - 1,
             "material": material}]})

        own = {"kind": "mesh", "loc": list(loc), "rot": list(rot), "scale": list(scale),
               "material": material, "smooth": True, "smooth_angle": smooth_angle,
               "mesh": len(self.meshes) - 1}
        self.node(name, loc, rot, scale, own, {"mesh": len(self.meshes) - 1})

    def gltf(self):
        nodes = list(self.nodes)
        lin = [round(srgb_to_linear(c), 6) for c in hex_rgb("#fff2e2")]

        nodes.append({
            "name": "Light",
            "translation": [round(v, 6) for v in to_gltf(self.light)],
            "extensions": {"KHR_lights_punctual": {"light": 0}},
            "extras": {"cafesa3d": {"kind": "light", "loc": list(self.light),
                                    "radius": self.lamp_radius, "power": self.power,
                                    "colour": "#fff2e2"}},
        })

        # The camera: looking down its own -Z with +Y up, in glTF's space.
        p, t = to_gltf(self.camera), to_gltf(self.target)
        f = [t[i] - p[i] for i in range(3)]
        n = math.sqrt(sum(v * v for v in f))
        f = [v / n for v in f]
        r = [f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0]
        rn = math.sqrt(sum(v * v for v in r))
        r = [v / rn for v in r]
        u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
        M = [[r[i], u[i], -f[i]] for i in range(3)]

        nodes.append({
            "name": "Camera",
            "translation": [round(v, 6) for v in p],
            "rotation": quaternion(M),
            "camera": 0,
            "extras": {"cafesa3d": {"kind": "camera", "loc": list(self.camera),
                                    "target": list(self.target), "focal": 50}},
        })

        doc = {
            "asset": {"version": "2.0", "generator": "Cafesa3D, Kosmos"},
            "extensionsUsed": ["KHR_lights_punctual", "KHR_materials_transmission",
                               "KHR_materials_ior", "KHR_materials_emissive_strength"],
            "scene": 0,
            "scenes": [{"name": self.name, "nodes": list(range(len(nodes)))}],
            "nodes": nodes,
            "materials": self.materials,
            "cameras": [{"type": "perspective", "name": "Camera",
                         "perspective": {"yfov": round(2 * math.atan(10.125 / 50), 6),
                                         "aspectRatio": 1.777778, "znear": 0.1,
                                         "zfar": 1000}}],
            "extensions": {"KHR_lights_punctual": {"lights": [
                {"name": "Light", "type": "point", "color": lin,
                 "intensity": round(self.power / (4 * math.pi), 3)}]}},
            "extras": {"cafesa3d": {"world": self.world}},
        }

        if self.meshes:
            doc["meshes"] = self.meshes
            doc["accessors"] = self.accessors
            doc["bufferViews"] = self.views
            doc["buffers"] = [{"byteLength": len(self.buffer),
                               "uri": "data:application/octet-stream;base64,"
                                      + base64.b64encode(bytes(self.buffer)).decode()}]

        return doc


# --------------------------------------------------------------------------
# The house.
# --------------------------------------------------------------------------

BRICK = dict(pattern="brick", colour2="#d8d0c2", scale=13, ratio=2.9, mortar=0.14,
             offset=0.5, bump=0.004)
SHINGLES = dict(pattern="shingles", colour2="#4a1d14", scale=4.5, ratio=1.3, mortar=0.05,
                offset=0.5, bump=0.018)
GRASS = dict(pattern="noise", colour2="#3a6224", scale=2.5, detail=5, distortion=0.6,
             bump=0.012)
WOOD = dict(pattern="wood", colour2="#5c3a20", scale=9, detail=3, distortion=2.2, bump=0.002)
BARK = dict(pattern="noise", colour2="#20150d", scale=14, detail=4, distortion=1.0, bump=0.02)
LEAVES = dict(pattern="noise", colour2="#1e4a1c", scale=5, detail=4, distortion=0.8,
              bump=0.05)
PAVING = dict(pattern="brick", colour2="#7f7a72", scale=2.4, ratio=1.0, mortar=0.06,
              offset=0.5, bump=0.004)


def house():
    s = Scene("House", camera=(14.5, -16.5, 7.2), target=(0.2, 0, 2.5), light=(-8, -11, 15),
              power=5200, lamp_radius=1.1)
    grass = s.material("Lawn", "#6c9a41", rough=1, texture=GRASS)
    brick = s.material("Brick", "#a34b33", rough=0.85, texture=BRICK)
    tiles = s.material("Roof tiles", "#99402c", rough=0.6, texture=SHINGLES)
    trim = s.material("White trim", "#f1efe9", rough=0.4)
    door = s.material("Door", "#2c4d6e", rough=0.35)
    glass = s.material("Glass", "#ffffff", "Glass")
    brass = s.material("Brass", "#c9a44c", "Metal", rough=0.25)
    room = s.material("Room", "#2b2723", rough=0.8)
    stone = s.material("Paving", "#bdb6aa", rough=0.9, texture=PAVING)
    wood = s.material("Fence", "#b98a5a", rough=0.7, texture=WOOD)
    bark = s.material("Bark", "#553a26", rough=0.95, texture=BARK)
    leaves = s.material("Leaves", "#3e7a34", rough=0.8, texture=LEAVES)
    pine = s.material("Pine", "#2b5a33", rough=0.8, texture=LEAVES)
    slate = s.material("Slate", "#4c4f55", rough=0.7)
    lamp = s.material("Lamp", "#ffe2a8", "Light", emit=6)
    iron = s.material("Iron", "#26282c", "Metal", rough=0.5)
    flowers = [s.material("Flower " + c, h, rough=0.6) for c, h in
               (("red", "#d8323c"), ("yellow", "#f2c230"), ("violet", "#8c4bc2"))]

    s.add("Ground", "plane", (0, 0, 0), grass, size=240)

    # The walls: brick, a box each, on a plinth of slate.
    W, D, H = 7.0, 5.6, 3.0
    s.mesh("Plinth", rounded_box((W + 0.3, D + 0.3, 0.3), 0.04), (0, 0, 0.15), slate)
    s.add("Wall", "box", (0, -D / 2 + 0.12, 1.65), brick, size=(W, 0.24, H))
    s.add("Wall", "box", (0, D / 2 - 0.12, 1.65), brick, size=(W, 0.24, H))
    s.add("Wall", "box", (-W / 2 + 0.12, 0, 1.65), brick, size=(0.24, D - 0.48, H))
    s.add("Wall", "box", (W / 2 - 0.12, 0, 1.65), brick, size=(0.24, D - 0.48, H))

    # The gables: triangles of brick up to the ridge, one at each end.
    pitch = 38
    rise = (D / 2) * math.tan(math.radians(pitch))
    for x in (-W / 2 + 0.12, W / 2 - 0.12):
        g = Mesh()
        a, b, c = g.point((0, -D / 2, 0)), g.point((0, D / 2, 0)), g.point((0, 0, rise))
        a2, b2, c2 = g.point((0.24, -D / 2, 0)), g.point((0.24, D / 2, 0)), g.point((0.24, 0, rise))
        g.face(a, c, b, (-1, 0, 0))
        g.face(a2, b2, c2, (1, 0, 0))
        g.quad(a, a2, c2, c, (0, -1, 1))
        g.quad(b, c, c2, b2, (0, 1, 1))
        s.mesh("Gable", g.weld(), (x - 0.12, 0, 3.15), brick, smooth_angle=10)

    # The roof: two slabs of tiles on the pitch, eaves out past the walls, a
    # ridge along the top and fascia boards under the eaves.
    over = 0.45
    slope = (D / 2 + over) / math.cos(math.radians(pitch))
    for side in (-1, 1):
        cy = side * (D / 2 + over) / 2
        cz = 3.15 + rise / 2 - over * math.tan(math.radians(pitch)) / 2 + 0.1
        # Its long side along its own up, turned onto the pitch: tiles
        # course by an object's up, and here up is up the roof.
        s.mesh("Roof", rounded_box((W + 0.9, 0.12, slope), 0.03),
               (0, cy, cz), tiles, rot=(side * (90 - pitch), 0, 0))
        s.mesh("Fascia", rounded_box((W + 0.94, 0.05, 0.2), 0.015),
               (0, side * (D / 2 + over - 0.02), 3.15 - over * math.tan(math.radians(pitch)) + 0.02),
               trim)
    s.add("Ridge", "cylinder", (0, 0, 3.15 + rise + 0.17), tiles, rot=(0, 90, 0),
          radius=0.11, depth=W + 0.9, segments=24, smooth=True)

    # The chimney: brick, a slate cap, two pots.
    s.add("Chimney", "box", (2.1, 1.1, 5.2), brick, size=(0.8, 0.7, 2.4))
    s.mesh("Chimney cap", rounded_box((0.96, 0.86, 0.12), 0.03), (2.1, 1.1, 6.45), slate)
    for i, x in enumerate((1.93, 2.27)):
        s.mesh("Chimney pot", lathe([(0, 0), (0.1, 0), (0.1, 0.32), (0.12, 0.34),
                                     (0.12, 0.38), (0.085, 0.38), (0.085, 0.05), (0, 0.05)], 24),
               (x, 1.1, 6.5), s.material("Terracotta", "#b3563a", rough=0.8))

    # Windows: a frame, a sill, a glazing bar across and down, the glass.
    def window(x, y, z, face, w=1.25, h=1.35):
        horizontal = face in ("front", "back")
        out = -1 if face in ("front", "left") else 1

        def put(name, kind_or_mesh, dx, dz, size, mat, depth_off=0.0):
            if horizontal:
                loc = (x + dx, y + out * depth_off, z + dz)
                box = (size[0], size[1], size[2])
            else:
                loc = (x + out * depth_off, y + dx, z + dz)
                box = (size[1], size[0], size[2])
            s.mesh(name, rounded_box(box, 0.012, 2), loc, mat)

        t = 0.07
        put("Window frame", None, 0, h / 2 - t / 2, (w, 0.12, t), trim, 0.03)
        put("Window frame", None, 0, -h / 2 + t / 2, (w, 0.12, t), trim, 0.03)
        put("Window frame", None, -w / 2 + t / 2, 0, (t, 0.12, h), trim, 0.03)
        put("Window frame", None, w / 2 - t / 2, 0, (t, 0.12, h), trim, 0.03)
        put("Glazing bar", None, 0, 0, (0.035, 0.05, h - 2 * t), trim, 0.03)
        put("Glazing bar", None, 0, 0, (w - 2 * t, 0.05, 0.035), trim, 0.03)
        put("Sill", None, 0, -h / 2 - 0.05, (w + 0.2, 0.26, 0.07), trim, 0.1)
        # The glass, and the room behind it: a dark panel between the glass
        # and the brick, which is what a window shows by day.
        if horizontal:
            s.add("Window glass", "box", (x, y + out * 0.01, z), glass,
                  size=(w - 2 * t, 0.02, h - 2 * t))
            s.add("Room", "box", (x, y - out * 0.012, z), room, size=(w - 0.05, 0.01, h - 0.05))
        else:
            s.add("Window glass", "box", (x + out * 0.01, y, z), glass,
                  size=(0.02, w - 2 * t, h - 2 * t))
            s.add("Room", "box", (x - out * 0.012, y, z), room, size=(0.01, w - 0.05, h - 0.05))

    front, back = -D / 2 - 0.02, D / 2 + 0.02
    for x in (-2.2, 2.2):
        window(x, front, 1.95, "front")
        window(x, back, 1.95, "back")
    window(W / 2 + 0.02, -1.2, 1.95, "right")
    window(W / 2 + 0.02, 1.2, 1.95, "right")
    window(-W / 2 - 0.02, 0, 1.95, "left")

    # The door: panelled, in a white frame, with a brass knob, a letter
    # plate and a stone step; a lamp beside it.
    s.mesh("Door", rounded_box((1.05, 0.08, 2.15), 0.02), (0, front - 0.02, 1.4), door)
    for dz in (0.55, -0.45):
        for dx in (-0.24, 0.24):
            s.mesh("Door panel", rounded_box((0.36, 0.04, 0.72), 0.03),
                   (dx, front - 0.07, 1.4 + dz), door)
    s.mesh("Door frame", rounded_box((1.3, 0.14, 0.1), 0.02), (0, front - 0.03, 2.53), trim)
    for dx in (-0.6, 0.6):
        s.mesh("Door frame", rounded_box((0.1, 0.14, 2.3), 0.02), (dx, front - 0.03, 1.4), trim)
    s.add("Door knob", "sphere", (0.36, front - 0.13, 1.35), brass, radius=0.045,
          segments=24, rings=12, smooth=True)
    s.mesh("Letter plate", rounded_box((0.3, 0.02, 0.07), 0.01), (0, front - 0.1, 1.05), brass)
    s.mesh("Step", rounded_box((1.6, 0.5, 0.18), 0.03), (0, front - 0.3, 0.09), stone)
    s.mesh("Wall lamp", lathe([(0, 0), (0.07, 0.02), (0.09, 0.14), (0.05, 0.24), (0, 0.24)], 20),
           (-0.95, front - 0.12, 2.05), lamp)

    # The path, the lawn's edging, a fence of pickets along the front.
    s.add("Path", "box", (0, -6.2, 0.025), stone, size=(1.3, 6.4, 0.05))
    for i in range(34):
        x = -9 + i * 0.55
        if abs(x) < 0.9:
            continue
        s.mesh("Picket", rounded_box((0.1, 0.03, 1.0), 0.012, 2), (x, -9.4, 0.5), wood)
        s.add("Picket top", "cone", (x, -9.4, 1.06), wood, radius=0.07, radius2=0,
              depth=0.12, segments=4, rot=(0, 0, 45), scale=(0.72, 0.21, 1))
    for z in (0.3, 0.75):
        for x0, x1 in ((-9.1, -0.8), (0.8, 9.1)):
            s.mesh("Fence rail", rounded_box((x1 - x0, 0.03, 0.07), 0.01, 2),
                   ((x0 + x1) / 2, -9.37, z), wood)

    # A tree, a pine, bushes and flowers.
    def tree(x, y, h, crown):
        s.mesh("Trunk", lathe([(0, 0), (0.3, 0), (0.22, 0.25), (0.17, 0.8), (0.14, h),
                               (0.0, h + 0.05)], 20), (x, y, 0), bark)
        for dx, dy, dz, r in ((0, 0, 0, crown), (0.6, 0.3, -0.4, crown * 0.7),
                              (-0.5, -0.4, -0.3, crown * 0.75), (0.1, -0.5, 0.5, crown * 0.6)):
            s.add("Crown", "ico", (x + dx, y + dy, h + crown * 0.55 + dz), leaves, radius=r,
                  subdivisions=3, smooth=True)

    tree(-7.2, -2.5, 2.6, 1.5)
    tree(7.5, 3.8, 2.1, 1.2)
    for z, r in ((1.0, 1.45), (2.1, 1.12), (3.1, 0.78), (3.9, 0.45)):
        s.add("Pine", "cone", (-6.0, 4.5, z), pine, radius=r, radius2=0, depth=1.5,
              segments=24, smooth=True)
    s.mesh("Pine trunk", lathe([(0, 0), (0.18, 0), (0.12, 1.0), (0, 1.0)], 16), (-6.0, 4.5, 0),
           bark)
    for i, (x, y) in enumerate(((-2.6, -3.35), (2.6, -3.35), (-4.1, -2.6), (4.1, -2.6))):
        s.add("Bush", "ico", (x, y, 0.42), leaves, radius=0.55, subdivisions=3,
              scale=(1.35, 1.0, 0.85), smooth=True)
        for k in range(5):
            a = 2 * math.pi * k / 5 + i
            s.add("Flower", "sphere", (x + 0.75 * math.cos(a), y - 0.35 + 0.25 * math.sin(a), 0.2),
                  flowers[(i + k) % 3], radius=0.08, segments=12, rings=6, smooth=True)

    # A lamp post at the gate.
    s.mesh("Lamp post", lathe([(0, 0), (0.16, 0), (0.16, 0.08), (0.07, 0.14), (0.05, 2.3),
                               (0.08, 2.35), (0, 2.35)], 20), (1.3, -9.0, 0), iron)
    s.mesh("Lantern", lathe([(0, 0), (0.14, 0.05), (0.16, 0.34), (0.2, 0.36), (0.0, 0.5)], 6),
           (1.3, -9.0, 2.35), lamp, smooth_angle=5)
    return s


# --------------------------------------------------------------------------
# The car.
# --------------------------------------------------------------------------

ASPHALT = dict(pattern="noise", colour2="#23252a", scale=28, detail=5, distortion=0.6,
               bump=0.0015)
RUBBER = dict(pattern="noise", colour2="#0b0b0c", scale=40, detail=3, bump=0.002)
GRILLE = dict(pattern="brick", colour2="#0d0e10", scale=40, ratio=3.2, mortar=0.35,
              offset=0.5, bump=0.004)
KERB = dict(pattern="brick", colour2="#6f6c67", scale=3.2, ratio=3.0, mortar=0.05,
            offset=0.0, bump=0.004)
GARAGE = dict(pattern="brick", colour2="#c9c0b2", scale=12, ratio=2.9, mortar=0.12,
              offset=0.5, bump=0.004)


def car():
    s = Scene("Car", camera=(6.6, -6.4, 2.35), target=(0.1, 0, 0.72), light=(7, -9, 10),
              power=3600, lamp_radius=1.6, zenith="#6f93c8", horizon="#e3e8ee")
    road = s.material("Asphalt", "#43464b", rough=0.95, texture=ASPHALT)
    # Car paint is a colour under a clear coat - Plastic, in the presets' terms
    # - not a metal: as a metal it mirrored the asphalt and came out nearly
    # black.
    paint = s.material("Red paint", "#b3141c", rough=0.12)
    # A car's windows are dark and glossy from outside: what is seen in them
    # is the sky, not the seats.
    tinted = s.material("Tinted glass", "#0e1014", rough=0.04)
    glass = s.material("Glass", "#ffffff", "Glass")
    tyre = s.material("Rubber", "#1c1c1e", rough=0.9, texture=RUBBER)
    alloy = s.material("Alloy", "#c7ccd3", "Metal", rough=0.18)
    trim = s.material("Black trim", "#16171a", rough=0.45)
    grille = s.material("Grille", "#2a2c30", rough=0.4, texture=GRILLE)
    head = s.material("Headlight", "#fff6d8", "Light", emit=8)
    tail = s.material("Taillight", "#ff2016", "Light", emit=3)
    chrome = s.material("Chrome", "#e3e6ea", "Mirror", rough=0.05)
    white = s.material("Road marking", "#e8e6df", rough=0.6)
    kerb = s.material("Kerb", "#a7a39b", rough=0.9, texture=KERB)
    wall = s.material("Garage brick", "#9c4a36", rough=0.85, texture=GARAGE)
    plate = s.material("Number plate", "#f4f2e8", rough=0.4)
    disc = s.material("Brake disc", "#6b6e73", "Metal", rough=0.45)
    caliper = s.material("Caliper", "#e0b020", rough=0.3)

    s.add("Ground", "plane", (0, 0, 0), road, size=200)
    s.add("Garage wall", "box", (-3.5, 5.2, 2.0), wall, size=(16, 0.3, 4))
    s.mesh("Kerb", rounded_box((20, 0.35, 0.16), 0.04), (0, 3.6, 0.08), kerb)
    for i in range(6):
        s.add("Road marking", "box", (-9 + i * 4, -2.6, 0.004), white, size=(2.0, 0.14, 0.008))

    # The body: lofted along X from sections that are rounded rectangles,
    # lower at the nose and tail, and lifted over each wheel for its arch.
    wheels = (1.38, -1.36)
    stations = [i / 40 for i in range(41)]
    rings = []
    for u in stations:
        x = 2.25 - 4.5 * u
        nose = max(0.0, (abs(x) - 1.55) / 0.7)                 # 0 amidships, 1 at the ends
        half = 0.9 * (1 - 0.3 * nose ** 2) + 0.004
        top = (0.66 + 0.28 * min(1, max(0, (2.25 - x) / 1.2)) if x > 0.5
               else 0.94 + 0.04 * min(1, (0.5 - x) / 1.6))
        if x < -1.8:
            top -= 0.2 * ((-1.8 - x) / 0.45) ** 1.5
        bottom = 0.26 + 0.07 * nose
        for w in wheels:
            k = (x - w) / 0.5
            if abs(k) < 1:
                bottom = max(bottom, 0.26 + 0.5 * math.sqrt(1 - k * k))
        mid, halfh = (top + bottom) / 2, (top - bottom) / 2
        rings.append([(x, y, mid + z) for y, z in superellipse(half, halfh, 4.2, 40)])
    s.mesh("Body", loft(rings), (0, 0, 0), paint, smooth_angle=50)

    # The glasshouse: tinted glass all round and over the top, narrower at
    # the top, windscreen and back window raked.
    rings = []
    for i in range(25):
        x = 0.72 - 1.95 * i / 24
        if x > 0.1:
            top = 1.42 - (x - 0.1) / 0.62 * 0.46
        elif x < -0.75:
            top = 1.42 - (-0.75 - x) / 0.48 * 0.4
        else:
            top = 1.42
        base, lower_half, upper_half = 0.93, 0.8, 0.62
        ring = []
        for y, z in superellipse(1.0, 1.0, 3.0, 36):
            t = (z + 1) / 2                                   # 0 at the belt, 1 on top
            ring.append((x, y * (lower_half + (upper_half - lower_half) * t),
                         base + (top - base) * t))
        rings.append(ring)
    s.mesh("Glasshouse", loft(rings), (0, 0, 0), tinted, smooth_angle=60)

    # Wheels: a tyre turned on a lathe, an alloy rim with five spokes, a
    # brake disc and a yellow caliper behind them.
    tyre_profile = [(0.25, -0.12), (0.31, -0.125), (0.345, -0.11), (0.36, -0.08),
                    (0.365, 0.0), (0.36, 0.08), (0.345, 0.11), (0.31, 0.125), (0.25, 0.12),
                    (0.25, -0.12)]
    for i, (x, y) in enumerate(((1.38, 0.8), (1.38, -0.8), (-1.36, 0.8), (-1.36, -0.8))):
        steer = 12 if x > 0 else 0
        side = 1 if y > 0 else -1

        def at(dx, dy, dz):
            """A point of the wheel, measured from its middle, turned with it."""
            c, sn = math.cos(math.radians(steer)), math.sin(math.radians(steer))
            return (x + dx * c - dy * sn, y + dx * sn + dy * c, 0.365 + dz)

        s.mesh("Tyre", lathe(tyre_profile, 48), at(0, 0, 0), tyre, rot=(90, 0, steer),
               smooth_angle=60)
        s.add("Rim", "cylinder", at(0, side * 0.02, 0), alloy, rot=(90, 0, steer),
              radius=0.25, depth=0.2, segments=40, smooth=True)
        s.add("Brake disc", "cylinder", at(0, -side * 0.03, 0), disc, rot=(90, 0, steer),
              radius=0.19, depth=0.03, segments=32, smooth=True)
        s.mesh("Caliper", rounded_box((0.14, 0.05, 0.1), 0.02, 2), at(-0.14, side * 0.07, 0.125),
               caliper, rot=(0, 0, steer))
        for k in range(5):
            a = 72 * k + 18
            s.mesh("Spoke", rounded_box((0.2, 0.03, 0.05), 0.012, 2),
                   at(0.12 * math.cos(math.radians(a)), side * 0.13,
                      0.12 * math.sin(math.radians(a))), alloy, rot=(0, -a, steer))
        s.add("Hub cap", "sphere", at(0, side * 0.12, 0), chrome, radius=0.05,
              segments=20, rings=10, smooth=True, scale=(1, 0.4, 1))

    # Lamps, grille, bumpers, mirrors, handles, plates, an exhaust.
    for y in (-0.5, 0.5):
        s.add("Headlight", "sphere", (2.2, y, 0.58), head, radius=0.14, segments=24, rings=12,
              smooth=True, scale=(0.35, 1.3, 0.55))
        s.mesh("Headlight bezel", rounded_box((0.06, 0.42, 0.18), 0.03), (2.18, y, 0.58), trim)
        s.mesh("Taillight", rounded_box((0.05, 0.42, 0.13), 0.025), (-2.24, y, 0.64), tail)
        s.mesh("Mirror", rounded_box((0.16, 0.2, 0.12), 0.05), (0.52, y * 1.86, 1.05), paint)
        s.mesh("Mirror arm", rounded_box((0.06, 0.16, 0.03), 0.01, 2), (0.54, y * 1.64, 1.0),
               trim)
        for x in (0.25, -0.75):
            s.mesh("Door handle", rounded_box((0.18, 0.03, 0.03), 0.012, 2),
                   (x, y * 1.67, 0.86), chrome)
    s.mesh("Grille", rounded_box((0.08, 0.6, 0.16), 0.03), (2.22, 0, 0.44), grille)
    s.mesh("Bumper", rounded_box((0.18, 1.72, 0.16), 0.05), (2.2, 0, 0.33), trim)
    s.mesh("Bumper", rounded_box((0.18, 1.72, 0.16), 0.05), (-2.24, 0, 0.36), trim)
    s.mesh("Number plate", rounded_box((0.02, 0.52, 0.11), 0.01), (2.3, 0, 0.36), plate)
    s.mesh("Number plate", rounded_box((0.02, 0.52, 0.11), 0.01), (-2.27, 0, 0.5), plate)
    s.mesh("Sill", rounded_box((1.9, 0.06, 0.07), 0.02), (0.0, 0.76, 0.33), trim)
    s.mesh("Sill", rounded_box((1.9, 0.06, 0.07), 0.02), (0.0, -0.76, 0.33), trim)
    s.mesh("Exhaust", lathe([(0, 0), (0.045, 0), (0.045, 0.2), (0.035, 0.2), (0.035, 0.02),
                             (0, 0.02)], 20), (-2.24, 0.45, 0.24), chrome, rot=(0, -90, 0))
    s.mesh("Wiper", rounded_box((0.02, 0.5, 0.02), 0.008, 2), (0.78, 0.25, 0.965), trim,
           rot=(0, -38, 8))
    return s


# --------------------------------------------------------------------------
# The plane.
# --------------------------------------------------------------------------

CONCRETE = dict(pattern="brick", colour2="#6c6f73", scale=0.2, ratio=1.0, mortar=0.004,
                offset=0.0, bump=0.002)
TARMAC = dict(pattern="noise", colour2="#6d7074", scale=6, detail=5, distortion=0.4,
              bump=0.001)
FIELD = dict(pattern="noise", colour2="#4f6b2e", scale=1.2, detail=4, distortion=0.7,
             bump=0.01)


def plane():
    s = Scene("Plane", camera=(21, -20, 7.2), target=(0.5, 0, 2.6), light=(-8, -12, 20),
              power=8000, lamp_radius=1.6, zenith="#5b8fd6", horizon="#e6eef7")
    field = s.material("Grass", "#6f8f45", rough=1, texture=FIELD)
    runway = s.material("Runway", "#8d9094", rough=0.95, texture=TARMAC)
    apron = s.material("Apron", "#a3a6a9", rough=0.9, texture=CONCRETE)
    white = s.material("Fuselage", "#f0f2f5", rough=0.25)
    blue = s.material("Livery", "#173f8a", rough=0.28)
    wingm = s.material("Wing", "#c8ced6", "Metal", rough=0.32)
    engine = s.material("Nacelle", "#e8ebef", rough=0.25)
    fan = s.material("Fan", "#2a2d33", "Metal", rough=0.35)
    spinner = s.material("Spinner", "#dfe3e8", "Metal", rough=0.2)
    window = s.material("Windows", "#1b222c", rough=0.08)
    tyre = s.material("Rubber", "#1b1b1d", rough=0.9, texture=RUBBER)
    strut = s.material("Strut", "#9aa0a7", "Metal", rough=0.3)
    paint = s.material("Markings", "#f4f2ea", rough=0.6)
    yellow = s.material("Taxi line", "#f0c020", rough=0.6)
    nav_red = s.material("Nav red", "#ff2a1a", "Light", emit=6)
    nav_green = s.material("Nav green", "#1aff5a", "Light", emit=6)

    s.add("Ground", "plane", (0, 0, 0), field, size=600)
    s.add("Apron", "box", (0, 0, 0.01), apron, size=(60, 40, 0.02))
    s.add("Runway", "box", (0, -28, 0.012), runway, size=(140, 18, 0.024))
    for i in range(14):
        s.add("Centre line", "box", (-65 + i * 10, -28, 0.026), paint, size=(5, 0.4, 0.004))
    s.add("Taxi line", "box", (0, -6, 0.022), yellow, size=(60, 0.18, 0.004))

    # The fuselage: a lathe turned about X, its nose rounded and its tail
    # swept up into a cone; white above the waterline and blue below, two
    # halves of one shape.
    prof = []
    for i in range(60):
        u = i / 59
        x = -9.0 + 17.6 * u
        if x > 5.6:
            k = (x - 5.6) / 3.0
            r = 1.0 * math.sqrt(max(0.0, 1 - k ** 2.2))
        elif x < -4.0:
            k = (-4.0 - x) / 5.0
            r = 1.0 * (1 - 0.82 * k ** 1.3)
        else:
            r = 1.0
        prof.append((x, r, 0.62 * max(0.0, (-4.0 - x) / 5.0) ** 1.5 if x < -4.0 else 0.0))

    def half(lo, hi, n=28):
        rings = []
        for x, r, lift in prof:
            ring = []
            for j in range(n + 1):
                a = math.radians(lo + (hi - lo) * j / n)
                ring.append((x, r * math.cos(a), 2.9 + lift + r * math.sin(a)))
            rings.append(ring)
        m = Mesh()
        ids = [[m.point(p) for p in ring] for ring in rings]
        for i in range(len(rings) - 1):
            for j in range(n):
                mid = rings[i][j]
                axis = (mid[0], 0, 2.9 + prof[i][2])
                m.quad(ids[i][j], ids[i][j + 1], ids[i + 1][j + 1], ids[i + 1][j],
                       sub(mid, axis))
        return m.weld()

    s.mesh("Fuselage", half(0, 180, 40), (0, 0, 0), white, smooth_angle=70)
    s.mesh("Belly", half(180, 360, 40), (0, 0, 0), blue, smooth_angle=70)
    s.add("Tail cap", "sphere", (-9.0, 0, 2.9 + prof[0][2]), white, radius=prof[0][1] * 1.02,
          segments=24, rings=12, smooth=True)

    # Windows down each side, a cockpit's in the nose, two doors.
    for side in (-1, 1):
        for i in range(22):
            x = -3.6 + i * 0.4
            s.mesh("Window", rounded_box((0.17, 0.04, 0.24), 0.06, 2),
                   (x, side * 0.985, 3.18), window, rot=(side * -8, 0, 0))
        # Cockpit windows on the nose's skin: a point on it at an angle up
        # from the side, and the pane turned to face out from there.
        for x, up, w in ((6.55, 32, 0.46), (7.0, 56, 0.4)):
            k = (x - 5.6) / 3.0
            r = math.sqrt(max(0.0, 1 - k ** 2.2)) + 0.01
            a = math.radians(up)
            s.mesh("Cockpit window", rounded_box((w, 0.04, 0.24), 0.07, 2),
                   (x, side * r * math.cos(a), 2.9 + r * math.sin(a)), window,
                   rot=(up if side > 0 else 180 - up, 0, side * -18))

    # The wings: NACA 2412 lofted from root to tip, tapered, swept and
    # raised; winglets turned up at the tips.
    airfoil = naca(0.12, 0.02, 0.4, 20)
    for side in (-1, 1):
        stations = [(side * y, 1.8 - 0.36 * y, 2.35 + 0.075 * y, 4.2 - 0.23 * y)
                    for y in (0.6, 3, 6, 9, 11.5)]
        s.mesh("Wing", wing(stations, airfoil), (0, 0, 0), wingm, smooth_angle=40)
        tip = [(side * y, -2.25 - 0.3 * (y - 11.5), 3.2 + (y - 11.5) * 2.4, 1.55 - 0.5 * (y - 11.5))
               for y in (11.5, 11.85)]
        s.mesh("Winglet", wing(tip, naca(0.1, 0.0, 0.4, 14)), (0, 0, 0), blue, smooth_angle=40)
        s.mesh("Tailplane", wing([(side * y, -7.2 - 0.45 * y, 3.35 + 0.05 * y, 2.1 - 0.12 * y)
                                  for y in (0.4, 2.5, 4.4)], naca(0.1, 0.0, 0.4, 14)),
               (0, 0, 0), wingm, smooth_angle=40)
        s.add("Nav light", "sphere", (-2.4, side * 11.9, 3.3), nav_green if side > 0 else nav_red,
              radius=0.08, segments=12, rings=6, smooth=True)

    # The fin: an airfoil stood upright and swept back.
    fin = []
    for z, x0, c in ((3.6, -6.2, 3.4), (5.4, -7.6, 2.3), (7.0, -8.7, 1.5)):
        fin.append([(x0 - c * sx, sz * c, z) for sx, sz in naca(0.1, 0.0, 0.4, 16)])
    s.mesh("Fin", loft(fin), (0, 0, 0), blue, smooth_angle=40)

    # Engines under the wings: a nacelle turned on a lathe about X, a dark
    # fan and a spinner at its mouth, a pylon to the wing.
    nacelle = [(0, 0.54), (0.1, 0.62), (0.4, 0.66), (1.6, 0.62), (2.4, 0.46), (2.7, 0.32),
               (2.7, 0.24), (0.14, 0.5), (0.1, 0.53), (0, 0.54)]
    for side in (-1, 1):
        y = side * 4.3
        s.mesh("Nacelle", lathe([(r, x) for x, r in nacelle], 40), (1.9, y, 1.6), engine,
               rot=(0, -90, 0), smooth_angle=50)
        s.add("Fan", "cylinder", (1.75, y, 1.6), fan, rot=(0, 90, 0), radius=0.5, depth=0.04,
              segments=40, smooth=True)
        s.mesh("Spinner", lathe([(0.2, 0), (0.18, 0.1), (0.1, 0.22), (0, 0.3)], 24),
               (1.74, y, 1.6), spinner, rot=(0, 90, 0))
        s.mesh("Pylon", rounded_box((2.0, 0.18, 0.7), 0.06), (0.6, y, 2.2), wingm,
               rot=(0, 8, 0))

    # Landing gear: a strut and a pair of tyres under each wing, one under
    # the nose; doors folded down beside them.
    tyre_profile = [(0.2, -0.13), (0.3, -0.14), (0.36, -0.12), (0.38, -0.06), (0.385, 0.0),
                    (0.38, 0.06), (0.36, 0.12), (0.3, 0.14), (0.2, 0.13), (0.2, -0.13)]
    for x, y, pair in ((0.2, 2.2, True), (0.2, -2.2, True), (5.6, 0, False)):
        top = 2.3 if pair else 2.1
        s.mesh("Strut", lathe([(0, 0), (0.09, 0), (0.09, top - 0.4), (0.06, top - 0.4),
                               (0.06, top), (0, top)], 16), (x, y, 0.38), strut)
        for dy in ((-0.24, 0.24) if pair else (-0.16, 0.16)):
            s.mesh("Tyre", lathe(tyre_profile, 40), (x, y + dy, 0.385), tyre, rot=(90, 0, 0),
                   smooth_angle=60)
            s.add("Hub", "cylinder", (x, y + dy, 0.385), strut, rot=(90, 0, 0), radius=0.2,
                  depth=0.24, segments=24, smooth=True)
        s.mesh("Axle", rounded_box((0.12, 0.62 if pair else 0.4, 0.12), 0.04),
               (x, y, 0.385), strut)
    return s


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: cafesa3d_samples.py OUTDIR")

    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    for name, make in (("house", house), ("car", car), ("plane", plane)):
        text = json.dumps(make().gltf(), indent=1, sort_keys=True)
        partial = os.path.join(out, name + ".gltf.part")

        with open(partial, "w") as f:
            f.write(text + "\n")

        os.replace(partial, os.path.join(out, name + ".gltf"))


if __name__ == "__main__":
    main()
