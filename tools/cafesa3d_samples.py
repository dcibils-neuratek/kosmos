#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Cafesa3D's sample scenes - a house, a car, a plane - written as glTF.

    cafesa3d_samples.py OUTDIR      house.gltf, car.gltf and plane.gltf there

Diego, 25 September 2026: "When done let's create a couple of sample scenes
with 3d designs", "A house", "A car", "A plane" (`roadmap.md` 4l). Each is
made of the 3D Kit's own shapes, turned and sized - a roof is a cylinder of
three sides on its side and flattened, a wheel a cylinder turned a quarter,
a fuselage a cylinder with a stretched sphere for a nose.

**This file is the scenes**; the build runs it and the image carries what it
writes (`sys.asset("scenes/house.gltf")`), so nothing generated sits in the
tree. Each file is glTF 2.0 twice over:

- **as glTF says**, so another program reads it: nodes with translation, a
  rotation quaternion and scale in glTF's Y-up space, materials in its
  metallic-roughness terms (and `KHR_materials_transmission`, `_ior` and
  `_emissive_strength`), the lamp as `KHR_lights_punctual`, the camera as a
  perspective camera looking down its -Z;
- **as Cafesa3D keeps it**, in each node's `extras.cafesa3d`: the shape and
  its own numbers - which glTF has no word for, a node being a mesh or
  nothing - and the object's place, turn and size in Cafesa3D's Z-up space
  exactly, so reading one back loses nothing.

`tools/test_scenefile.lua` holds the two to each other: the reader's
conversion of every node's glTF transform must land where its extras say.
"""

import json
import math
import os
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


class Scene:
    def __init__(self, name, camera, target, light, power=1500, lamp_radius=0.5,
                 zenith="#6d90c6", horizon="#dfe6ef", strength=0.9):
        self.name = name
        self.nodes, self.materials = [], []
        self.by_name = {}
        self.camera, self.target = camera, target
        self.light, self.power, self.lamp_radius = light, power, lamp_radius
        self.world = dict(zenith=zenith, horizon=horizon, strength=strength)

    def material(self, name, base, preset="Plastic", **over):
        if name in self.by_name:
            return self.by_name[name]

        m = dict(PRESETS[preset])
        m.update(over)
        lin = [round(srgb_to_linear(c), 6) for c in hex_rgb(base)]
        entry = {
            "name": name,
            "pbrMetallicRoughness": {"baseColorFactor": lin + [1],
                                     "metallicFactor": m["metallic"],
                                     "roughnessFactor": m["rough"]},
            "extras": {"cafesa3d": {"base": base, "preset": preset}},
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

    def add(self, name, kind, loc, material, rot=(0, 0, 0), scale=(1, 1, 1), smooth=False,
            **shape):
        R = mat_mul(mat_mul(C, euler_matrix(*rot)), transpose(C))
        own = {"kind": kind, "loc": list(loc), "rot": list(rot), "scale": list(scale),
               "material": material, "smooth": smooth}
        own.update({k: (list(v) if isinstance(v, tuple) else v) for k, v in shape.items()})
        self.nodes.append({
            "name": name,
            "translation": [round(v, 6) for v in to_gltf(loc)],
            "rotation": quaternion(R),
            "scale": [scale[0], scale[2], scale[1]],
            "extras": {"cafesa3d": own},
        })

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

        return {
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


def house():
    s = Scene("House", camera=(13, -15, 7.5), target=(0, 0, 2.6), light=(-7, -9, 13),
              power=4000, lamp_radius=0.8)
    grass = s.material("Grass", "#6f9a4a", rough=1)
    walls = s.material("Walls", "#e9dfc6")
    roof = s.material("Tiles", "#a8432f", rough=0.6)
    door = s.material("Door", "#6b4226", rough=0.5)
    glass = s.material("Glass", "#ffffff", "Glass")
    brick = s.material("Brick", "#8a3b2a", rough=0.8)
    stone = s.material("Stone", "#b9b3a8", rough=0.9)
    bark = s.material("Bark", "#5a3d26", rough=0.9)
    leaves = s.material("Leaves", "#3f7d3a", rough=0.8)
    pine = s.material("Pine", "#2f5f36", rough=0.8)

    s.add("Ground", "plane", (0, 0, 0), grass, size=40)
    s.add("Walls", "box", (0, 0, 1.5), walls, size=(6, 5, 3))
    # A cylinder of three sides turned on its side is a prism: a roof. Its
    # point up, it is flattened to half its height.
    s.add("Roof", "cylinder", (0, 0, 3.78), roof, rot=(0, -90, 0), scale=(0.45, 1, 1),
          radius=3.46, depth=7, segments=3)
    s.add("Chimney", "box", (1.8, 1.0, 5.0), brick, size=(0.6, 0.6, 1.8))
    s.add("Door", "box", (0, -2.52, 1.05), door, size=(1.1, 0.1, 2.1))
    s.add("Knob", "sphere", (0.38, -2.6, 1.05), s.material("Brass", "#c9a44c", "Metal"),
          radius=0.06, segments=16, rings=8, smooth=True)
    for i, x in enumerate((-1.9, 1.9)):
        s.add("Window" + ("", ".001")[i], "box", (x, -2.52, 1.8), glass, size=(1.2, 0.1, 1.0))
    for i, y in enumerate((-1.2, 1.2)):
        s.add("Window.%03d" % (i + 2), "box", (3.02, y, 1.8), glass, size=(0.1, 1.0, 1.0))
    s.add("Path", "box", (0, -5.0, 0.02), stone, size=(1.2, 5.0, 0.04))
    s.add("Trunk", "cylinder", (-5.5, -2.0, 1.0), bark, radius=0.2, depth=2, segments=12,
          smooth=True)
    s.add("Tree", "ico", (-5.5, -2.0, 3.0), leaves, radius=1.4, subdivisions=2)
    s.add("Trunk.001", "cylinder", (5.8, 2.6, 0.6), bark, radius=0.18, depth=1.2, segments=12,
          smooth=True)
    s.add("Pine", "cone", (5.8, 2.6, 2.6), pine, radius=1.3, radius2=0, depth=3.2,
          segments=16)
    s.add("Bush", "ico", (-2.4, -3.2, 0.45), leaves, radius=0.55, subdivisions=2,
          scale=(1.3, 1, 0.8))
    return s


def car():
    s = Scene("Car", camera=(6.8, -6.0, 2.9), target=(0, 0, 0.75), light=(-4, -6, 9),
              power=2500, lamp_radius=0.6)
    road = s.material("Asphalt", "#4a4d52", rough=1)
    paint = s.material("Red paint", "#c81d25", "Metal", rough=0.3)
    glass = s.material("Glass", "#ffffff", "Glass")
    tyre = s.material("Rubber", "#1b1b1d", rough=0.9)
    chrome = s.material("Chrome", "#c9ced6", "Metal", rough=0.15)
    trim = s.material("Trim", "#2a2b2e", rough=0.6)
    head = s.material("Headlight", "#fff3c4", "Light")
    tail = s.material("Taillight", "#ff2a1a", "Light", emit=3)
    stripe = s.material("Stripe", "#f2f2f2", rough=0.4)

    s.add("Ground", "plane", (0, 0, 0), road, size=40)
    s.add("Body", "box", (0, 0, 0.62), paint, size=(4.2, 1.8, 0.56))
    s.add("Hood", "box", (1.25, 0, 0.93), paint, size=(1.6, 1.74, 0.1), rot=(0, 6, 0))
    s.add("Cabin", "box", (-0.35, 0, 1.2), glass, size=(2.0, 1.56, 0.5))
    s.add("Roof", "box", (-0.4, 0, 1.49), paint, size=(1.7, 1.58, 0.08))
    s.add("Stripe", "box", (0, 0, 0.905), stripe, size=(4.1, 0.3, 0.02))
    # The front wheels steered fifteen degrees: turned a quarter about X to
    # stand up, then about Z - a turn about two axes, which is what holds
    # the glTF conversion's every term (tools/test_scenefile.lua).
    for i, (x, y) in enumerate(((1.35, 0.92), (1.35, -0.92), (-1.35, 0.92), (-1.35, -0.92))):
        steer = 15 if x > 0 else 0
        s.add("Wheel" + ("" if i == 0 else ".%03d" % i), "cylinder", (x, y, 0.38), tyre,
              rot=(90, 0, steer), radius=0.38, depth=0.3, segments=32, smooth=True)
        s.add("Hub" + ("" if i == 0 else ".%03d" % i), "cylinder", (x, y, 0.38), chrome,
              rot=(90, 0, steer), radius=0.2, depth=0.32, segments=24, smooth=True)
    for i, y in enumerate((0.6, -0.6)):
        s.add("Headlight" + ("", ".001")[i], "sphere", (2.08, y, 0.72), head, radius=0.12,
              segments=16, rings=8, smooth=True, scale=(0.6, 1, 1))
        s.add("Taillight" + ("", ".001")[i], "box", (-2.11, y, 0.78), tail,
              size=(0.04, 0.36, 0.1))
    s.add("Bumper", "box", (2.16, 0, 0.44), trim, size=(0.14, 1.84, 0.2))
    s.add("Bumper.001", "box", (-2.16, 0, 0.44), trim, size=(0.14, 1.84, 0.2))
    s.add("Spoiler", "box", (-1.95, 0, 1.18), trim, size=(0.32, 1.6, 0.05))
    s.add("Mirror", "box", (0.55, 0.95, 1.12), paint, size=(0.12, 0.14, 0.1))
    s.add("Mirror.001", "box", (0.55, -0.95, 1.12), paint, size=(0.12, 0.14, 0.1))
    return s


def plane():
    s = Scene("Plane", camera=(19, -17, 8.5), target=(0, 0, 2.4), light=(-6, -10, 16),
              power=6000, lamp_radius=1.0, zenith="#5b8fd6", horizon="#e6eef7")
    tarmac = s.material("Runway", "#55585c", rough=1)
    white = s.material("Fuselage", "#eef1f4", rough=0.35)
    wing = s.material("Wing", "#d6dbe0", "Metal", rough=0.35)
    blue = s.material("Livery", "#1f4e9c", rough=0.4)
    engine = s.material("Engine", "#a7adb5", "Metal", rough=0.25)
    dark = s.material("Windows", "#2a3440", rough=0.2)
    tyre = s.material("Rubber", "#1b1b1d", rough=0.9)
    strut = s.material("Strut", "#8b9097", "Metal", rough=0.4)
    paint = s.material("Paint", "#f2f2f2", rough=0.5)

    s.add("Ground", "plane", (0, 0, 0), tarmac, size=80)
    for i in range(6):
        s.add("Marking" + ("" if i == 0 else ".%03d" % i), "box", (-15 + i * 6, -6, 0.01),
              paint, size=(3, 0.3, 0.02))
    # The fuselage along X: a cylinder turned a quarter about Y.
    s.add("Fuselage", "cylinder", (0, 0, 2.5), white, rot=(0, 90, 0), radius=0.9, depth=12,
          segments=32, smooth=True)
    s.add("Nose", "sphere", (6.0, 0, 2.5), white, scale=(1.7, 1, 1), radius=0.9,
          segments=32, rings=16, smooth=True)
    s.add("Tail cone", "cone", (-7.5, 0, 2.62), white, rot=(0, -90, 0), radius=0.9,
          radius2=0.3, depth=3, segments=32, smooth=True)
    s.add("Window band", "box", (0.2, 0, 2.9), dark, size=(9.6, 1.86, 0.22))
    s.add("Cockpit", "box", (6.55, 0, 2.95), dark, size=(0.9, 1.3, 0.28), rot=(0, -18, 0))
    s.add("Belly stripe", "box", (0, 0, 2.2), blue, size=(12.2, 1.84, 0.16))
    s.add("Wing", "box", (0.4, 0, 2.2), wing, size=(3.2, 16, 0.18), rot=(0, 0, 0))
    # Winglets canted outwards as well as swept: turns about X and Y.
    s.add("Wing tip", "box", (0.1, 8.05, 2.55), blue, size=(1.4, 0.1, 0.8), rot=(-12, -15, 0))
    s.add("Wing tip.001", "box", (0.1, -8.05, 2.55), blue, size=(1.4, 0.1, 0.8),
          rot=(12, -15, 0))
    s.add("Stabiliser", "box", (-8.3, 0, 2.75), wing, size=(1.6, 6, 0.12))
    s.add("Fin", "box", (-8.3, 0, 4.1), blue, size=(2.0, 0.14, 2.6), rot=(0, -20, 0))
    for i, y in enumerate((3.6, -3.6)):
        n = "" if i == 0 else ".001"
        s.add("Engine" + n, "cylinder", (0.9, y, 1.65), engine, rot=(0, 90, 0), radius=0.48,
              depth=2.2, segments=32, smooth=True)
        s.add("Intake" + n, "cylinder", (2.02, y, 1.65), dark, rot=(0, 90, 0), radius=0.38,
              depth=0.06, segments=32)
        s.add("Pylon" + n, "box", (0.9, y, 2.05), wing, size=(1.4, 0.16, 0.4))
    for i, (x, y) in enumerate(((0.3, 1.6), (0.3, -1.6), (5.0, 0))):
        n = "" if i == 0 else ".%03d" % i
        s.add("Strut" + n, "cylinder", (x, y, 1.05), strut, radius=0.07, depth=1.5,
              segments=12, smooth=True)
        s.add("Tyre" + n, "cylinder", (x, y, 0.36), tyre, rot=(90, 0, 0), radius=0.36,
              depth=0.28, segments=24, smooth=True)
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
