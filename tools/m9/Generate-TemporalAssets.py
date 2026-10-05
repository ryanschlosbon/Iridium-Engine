#!/usr/bin/env python3
"""M9 G6b: generates the engine-authored temporal fixtures.

Writes, under assets/benchmarks/m9/:
  * one glTF 2.0 file per temporal fixture (embedded base64 buffers and procedural
    PNG textures), each with a deterministic .iridium.meta sidecar;
  * temporal-manifest.v1.json, the benchmark manifest (composition scene factory,
    camera paths, content_files SHA-256s).

Everything here is engine-authored: geometry, textures and materials are computed by
this script. Standard library only. Output is byte-identical between runs:
  * no wall-clock, randomness or dictionary-order dependence (a fixed LCG seeds the
    foliage layout);
  * PNGs use stored (uncompressed) deflate blocks, so the bytes never depend on the
    zlib implementation (CPython 3.14 on Windows ships zlib-ng);
  * vertex values are quantized to 1e-6 before float32 packing, so a last-ulp libm
    difference in sin/cos cannot reach the file.

Each top-level glTF node is one movable unit. Node transforms are baked by the
cooker, so nodes carry no transform: each node's geometry is authored about its own
pivot, and the manifest's composition entities place and animate it (node index ->
entity). See docs/milestones/M9-temporal-and-post.md, slice G6b.

    python tools/m9/Generate-TemporalAssets.py          # write
    python tools/m9/Generate-TemporalAssets.py --check  # verify the tracked files
"""

import base64
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "assets" / "benchmarks" / "m9"
MANIFEST_NAME = "temporal-manifest.v1.json"
GENERATOR = "Iridium Engine M9 G6b deterministic temporal fixture generator v1"


# --- PNG ------------------------------------------------------------------------

def _png_chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def _zlib_stored(raw):
    """A zlib stream of stored deflate blocks: implementation-independent bytes."""
    out = bytearray(b"\x78\x01")
    if not raw:
        out += b"\x01\x00\x00\xff\xff"
    for start in range(0, len(raw), 65535):
        block = raw[start:start + 65535]
        final = 1 if start + 65535 >= len(raw) else 0
        out += bytes([final]) + struct.pack("<HH", len(block), len(block) ^ 0xFFFF) + block
    out += struct.pack(">I", zlib.adler32(raw) & 0xFFFFFFFF)
    return bytes(out)


def encode_png(width, height, channels, pixels):
    """pixels: bytes, row-major, `channels` (3 = RGB, 4 = RGBA) bytes per pixel."""
    color_type = {3: 2, 4: 6}[channels]
    stride = width * channels
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        raw += pixels[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", header) +
            _png_chunk(b"IDAT", _zlib_stored(bytes(raw))) + _png_chunk(b"IEND", b""))


# --- Procedural textures (integer/IEEE-exact arithmetic only) --------------------

def texture_checker_hf():
    """64x64: 4 px checker cells with 1 px grid lines every 8 px. Tiled at 0.25 m on
    the ground, a cell is ~16 mm (~8 px at 6 m in a native-4K 40-degree view) and a
    line ~4 mm: both alias under subpixel motion without temporal resolve."""
    size = 64
    px = bytearray()
    for y in range(size):
        for x in range(size):
            light = ((x // 4) + (y // 4)) % 2 == 0
            value = 214 if light else 34
            if x % 8 == 0 or y % 8 == 0:
                value = 8 if light else 250
            tint = (value, value, min(255, value + 6))
            px += bytes(tint)
    return encode_png(size, size, 3, px), "checker_hf"


def texture_tile():
    """64x64: one 1 m floor tile (warm grey) with a 2 px grout border."""
    size = 64
    px = bytearray()
    for y in range(size):
        for x in range(size):
            if x < 2 or y < 2:
                px += bytes((60, 58, 55))
            else:
                # Gentle deterministic mottling (integer hash), not noise-free flat.
                h = (x * 73856093 ^ y * 19349663) & 0xFF
                v = 150 + (h % 13) - 6
                px += bytes((v, v - 4, v - 10))
    return encode_png(size, size, 3, px), "tile"


def texture_backdrop():
    """64x64: saturated vertical colour bands with thin horizontal lines; tiled on
    walls and the teleport cube so motion, disocclusion and ghosting show as hue."""
    size = 64
    bands = [(220, 40, 40), (240, 200, 30), (40, 170, 60), (30, 120, 230),
             (180, 60, 200), (240, 240, 240), (20, 20, 20), (240, 120, 20)]
    px = bytearray()
    for y in range(size):
        for x in range(size):
            color = bands[x // 8]
            if y % 16 in (0, 1):
                color = (255 - color[0], 255 - color[1], 255 - color[2])
            px += bytes(color)
    return encode_png(size, size, 3, px), "backdrop"


def texture_leaf():
    """128x128 RGBA: five pointed leaves on a shared stem with a midrib; alpha is
    binary (alpha-mask cutoff 0.5). Leaf frames use exact rational unit vectors."""
    size = 128
    # (base x, base y, direction x, direction y [unit, rational], length, half-width)
    leaves = [
        (64, 120, 0.0, -1.0, 62.0, 15.0),
        (64, 92, 0.6, -0.8, 46.0, 12.0),
        (64, 92, -0.6, -0.8, 46.0, 12.0),
        (64, 64, 0.8, -0.6, 40.0, 10.0),
        (64, 64, -0.8, -0.6, 40.0, 10.0),
    ]
    px = bytearray()
    for y in range(size):
        for x in range(size):
            rgba = (0, 0, 0, 0)
            cx, cy = x + 0.5, y + 0.5
            # Stem: a 3 px vertical band from the base to the top leaf origin.
            if 62.0 <= cx <= 66.0 and 60.0 <= cy <= 122.0:
                rgba = (70, 96, 28, 255)
            for bx, by, dx, dy, length, half in leaves:
                rx, ry = cx - bx, cy - by
                u = rx * dx + ry * dy           # along the leaf
                v = -rx * dy + ry * dx          # across
                if u <= 0.0 or u >= length:
                    continue
                t = u / length
                # Pointed leaf: width profile 4 t (1 - t) (max at mid-length).
                width = half * 4.0 * t * (1.0 - t)
                if abs(v) <= width:
                    shade = int(30 * t)
                    if abs(v) <= 0.9:
                        rgba = (150, 190, 70, 255)            # midrib
                    elif v > 0.0:
                        rgba = (46 + shade, 120 + shade, 30, 255)
                    else:
                        rgba = (36 + shade, 100 + shade, 24, 255)
            px += bytes(rgba)
    return encode_png(size, size, 4, px), "leaf"


def texture_particle():
    """64x64 RGBA particle card (M9.3): a soft-edged disc whose alpha plateaus at 1 to
    r = 0.55 and falls linearly to 0 at r = 1, with concentric light/dark colour rings
    (5 per radius) so the card's interior visibly moves with it. Integer arithmetic
    plus correctly rounded sqrt only."""
    size = 64
    px = bytearray()
    for y in range(size):
        for x in range(size):
            dx, dy = 2 * x + 1 - size, 2 * y + 1 - size
            r = math.sqrt(dx * dx + dy * dy) / size
            if r >= 1.0:
                px += bytes((0, 0, 0, 0))
                continue
            alpha = 255 if r <= 0.55 else int(255 * (1.0 - r) / 0.45)
            value = 255 if int(r * 10.0) % 2 == 0 else 110
            px += bytes((value, value, value, alpha))
    return encode_png(size, size, 4, px), "particle"


TEXTURES = {
    "checker_hf": texture_checker_hf,
    "tile": texture_tile,
    "backdrop": texture_backdrop,
    "leaf": texture_leaf,
    "particle": texture_particle,
}


# --- Materials --------------------------------------------------------------------

def pbr(name, base, metallic, roughness, texture=None, **extra):
    material = {
        "name": name,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(base),
            "metallicFactor": metallic,
            "roughnessFactor": roughness,
        },
    }
    if texture:
        material["pbrMetallicRoughness"]["baseColorTexture"] = {"texture": texture}
    material.update(extra)
    return material


def emissive(name, color, strength):
    return pbr(name, (0.02, 0.02, 0.02, 1.0), 0.0, 0.6,
               emissiveFactor=list(color),
               extensions={"KHR_materials_emissive_strength": {"emissiveStrength": strength}})


MATERIALS = {
    "ground_checker": pbr("High-frequency checker ground", (1, 1, 1, 1), 0.0, 0.6, texture="checker_hf"),
    "ground_tile": pbr("Tiled ground", (1, 1, 1, 1), 0.0, 0.75, texture="tile"),
    "backdrop": pbr("Colour-band backdrop", (1, 1, 1, 1), 0.0, 0.55, texture="backdrop"),
    "fence_metal": pbr("Painted grille", (0.72, 0.72, 0.7, 1), 0.0, 0.45),
    "wire": pbr("Steel wire", (0.32, 0.32, 0.34, 1), 1.0, 0.4),
    "leaf": pbr("Alpha-mask leaf card", (1, 1, 1, 1), 0.0, 0.55, texture="leaf",
                alphaMode="MASK", alphaCutoff=0.5, doubleSided=True),
    "chrome": pbr("Polished chrome", (0.95, 0.95, 0.95, 1), 1.0, 0.04),
    "chrome_mirror": pbr("Mirror chrome", (0.97, 0.96, 0.94, 1), 1.0, 0.02),
    "gold": pbr("Polished gold", (1.0, 0.78, 0.34, 1), 1.0, 0.12),
    "occluder": pbr("Matte red occluder", (0.6, 0.08, 0.06, 1), 0.0, 0.5),
    "prop_red": pbr("Red prop", (0.75, 0.12, 0.1, 1), 0.0, 0.45),
    "prop_green": pbr("Green prop", (0.12, 0.6, 0.18, 1), 0.0, 0.45),
    "prop_blue": pbr("Blue prop", (0.1, 0.25, 0.75, 1), 0.0, 0.45),
    "prop_yellow": pbr("Yellow prop", (0.85, 0.7, 0.1, 1), 0.0, 0.45),
    "prop_white": pbr("White prop", (0.85, 0.85, 0.85, 1), 0.0, 0.3),
    "dark_matte": pbr("Dark matte plate", (0.03, 0.03, 0.035, 1), 0.0, 0.9),
    "emissive_orange": emissive("Orange emissive bar", (1.0, 0.55, 0.15), 40.0),
    "emissive_cyan": emissive("Cyan emissive spinner", (0.1, 0.7, 1.0), 25.0),
    "highlight_64": emissive("Highlight 64", (1.0, 0.95, 0.85), 64.0),
    "highlight_512": emissive("Highlight 512", (1.0, 1.0, 1.0), 512.0),
    "highlight_4096": emissive("Highlight 4096", (0.85, 0.92, 1.0), 4096.0),
    # The M6 thin-glass route (rough_metric_refraction.gltf): transmission, IOR and
    # a zero-thickness volume, classified thin_glass by the sidecar policy.
    "glass": pbr("Tinted thin glass panel", (0.75, 0.9, 1.0, 1.0), 0.0, 0.1,
                 alphaMode="BLEND", doubleSided=True,
                 extensions={
                     "KHR_materials_ior": {"ior": 1.5},
                     "KHR_materials_transmission": {"transmissionFactor": 1.0},
                     "KHR_materials_volume": {"thicknessFactor": 0.0, "attenuationDistance": 0.02,
                                              "attenuationColor": [0.6, 0.85, 1.0]},
                 }),
    "glass_frame": pbr("Anodised glass frame", (0.2, 0.21, 0.23, 1), 1.0, 0.3),
    # M9.3 TF-reactive: a strongly tinted, slightly rough frameless thin-glass sheet
    # (the 2 cm sheet's path equals the attenuation distance, so the transmitted
    # scene is scaled by about the attenuation colour at normal incidence).
    "reactive_glass": pbr("Strongly tinted thin glass sheet", (1.0, 1.0, 1.0, 1.0), 0.0, 0.15,
                          alphaMode="BLEND", doubleSided=True,
                          extensions={
                              "KHR_materials_ior": {"ior": 1.5},
                              "KHR_materials_transmission": {"transmissionFactor": 1.0},
                              "KHR_materials_volume": {"thicknessFactor": 0.02,
                                                       "attenuationDistance": 0.02,
                                                       "attenuationColor": [0.45, 0.7, 1.0]},
                          }),
    # M9.3 TF-reactive particle cards: alpha-blended (no transmission), ringed
    # texture with a 0.55-0.6 alpha plateau, emissive. The sorted card resolves to
    # SortedSurface (Auto); the second card carries an explicit WeightedOIT policy.
    "particle_sorted": pbr("Sorted emissive particle card", (1.0, 0.55, 0.15, 0.6), 0.0, 0.6,
                           texture="particle", alphaMode="BLEND", doubleSided=True,
                           emissiveFactor=[1.0, 0.5, 0.12],
                           extensions={"KHR_materials_emissive_strength": {"emissiveStrength": 2.0}}),
    "particle_oit": pbr("WeightedOIT emissive particle card", (0.15, 0.6, 1.0, 0.55), 0.0, 0.6,
                        texture="particle", alphaMode="BLEND", doubleSided=True,
                        emissiveFactor=[0.15, 0.6, 1.0],
                        extensions={"KHR_materials_emissive_strength": {"emissiveStrength": 2.0}}),
}

GLASS_POLICY = {"schema_version": 1, "class": "thin_glass", "quality": "ordinary2",
                "priority": 0, "thin_sheet_thickness_m": 0.02}
OIT_POLICY = {"schema_version": 1, "class": "weighted_oit", "quality": "ordinary2",
              "priority": 0, "thin_sheet_thickness_m": 0.0}
# Material key -> sidecar transparency policy (other materials use Auto).
MATERIAL_POLICIES = {"glass": GLASS_POLICY, "reactive_glass": GLASS_POLICY,
                     "particle_oit": OIT_POLICY}


# --- Geometry ----------------------------------------------------------------------

def q(value):
    return round(value, 6) + 0.0  # quantize; +0.0 folds -0.0


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(a):
    length = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    return (a[0] / length, a[1] / length, a[2] / length)


class Prim:
    """Triangle list for one material: position, normal, uv, indices."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.indices = [], [], [], []

    def vertex(self, p, n, uv):
        self.positions.append(tuple(q(c) for c in p))
        self.normals.append(tuple(q(c) for c in n))
        self.uvs.append((q(uv[0]), q(uv[1])))
        return len(self.positions) - 1

    def quad(self, p0, p1, p2, p3, n, uv0=(0, 1), uv1=(1, 1), uv2=(1, 0), uv3=(0, 0)):
        """Counter-clockwise p0..p3 seen from the side `n` points to."""
        a = self.vertex(p0, n, uv0)
        b = self.vertex(p1, n, uv1)
        c = self.vertex(p2, n, uv2)
        d = self.vertex(p3, n, uv3)
        self.indices += [a, b, c, a, c, d]

    def box(self, center, size, uv_scale=1.0):
        cx, cy, cz = center
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        su, sv = uv_scale, uv_scale
        # +Z, -Z, +X, -X, +Y, -Y faces
        self.quad((cx - hx, cy - hy, cz + hz), (cx + hx, cy - hy, cz + hz), (cx + hx, cy + hy, cz + hz),
                  (cx - hx, cy + hy, cz + hz), (0, 0, 1), (0, sv), (su, sv), (su, 0), (0, 0))
        self.quad((cx + hx, cy - hy, cz - hz), (cx - hx, cy - hy, cz - hz), (cx - hx, cy + hy, cz - hz),
                  (cx + hx, cy + hy, cz - hz), (0, 0, -1), (0, sv), (su, sv), (su, 0), (0, 0))
        self.quad((cx + hx, cy - hy, cz + hz), (cx + hx, cy - hy, cz - hz), (cx + hx, cy + hy, cz - hz),
                  (cx + hx, cy + hy, cz + hz), (1, 0, 0), (0, sv), (su, sv), (su, 0), (0, 0))
        self.quad((cx - hx, cy - hy, cz - hz), (cx - hx, cy - hy, cz + hz), (cx - hx, cy + hy, cz + hz),
                  (cx - hx, cy + hy, cz - hz), (-1, 0, 0), (0, sv), (su, sv), (su, 0), (0, 0))
        self.quad((cx - hx, cy + hy, cz + hz), (cx + hx, cy + hy, cz + hz), (cx + hx, cy + hy, cz - hz),
                  (cx - hx, cy + hy, cz - hz), (0, 1, 0), (0, sv), (su, sv), (su, 0), (0, 0))
        self.quad((cx - hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz - hz), (cx + hx, cy - hy, cz + hz),
                  (cx - hx, cy - hy, cz + hz), (0, -1, 0), (0, sv), (su, sv), (su, 0), (0, 0))

    def plane_xz(self, width, depth, uv_repeat):
        hw, hd = width / 2, depth / 2
        self.quad((-hw, 0, hd), (hw, 0, hd), (hw, 0, -hd), (-hw, 0, -hd), (0, 1, 0),
                  (0, uv_repeat), (uv_repeat, uv_repeat), (uv_repeat, 0), (0, 0))

    def wall_xy(self, width, height, uv_repeat_u, uv_repeat_v, z=0.0):
        hw = width / 2
        self.quad((-hw, 0, z), (hw, 0, z), (hw, height, z), (-hw, height, z), (0, 0, 1),
                  (0, uv_repeat_v), (uv_repeat_u, uv_repeat_v), (uv_repeat_u, 0), (0, 0))

    def cylinder(self, p0, p1, radius, sides):
        axis = normalize(sub(p1, p0))
        helper = (0.0, 1.0, 0.0) if abs(axis[1]) < 0.9 else (1.0, 0.0, 0.0)
        u = normalize(cross(axis, helper))
        v = cross(axis, u)
        base = len(self.positions)
        for side in range(sides + 1):
            angle = 2.0 * math.pi * side / sides
            n = add(scale(u, math.cos(angle)), scale(v, math.sin(angle)))
            self.vertex(add(p0, scale(n, radius)), n, (side / sides, 1.0))
            self.vertex(add(p1, scale(n, radius)), n, (side / sides, 0.0))
        for side in range(sides):
            a, b = base + 2 * side, base + 2 * side + 1
            c, d = base + 2 * side + 2, base + 2 * side + 3
            self.indices += [a, c, d, a, d, b]

    def sphere(self, center, radius, stacks, slices):
        base = len(self.positions)
        for stack in range(stacks + 1):
            phi = math.pi * stack / stacks
            for slice_ in range(slices + 1):
                theta = 2.0 * math.pi * slice_ / slices
                n = (math.sin(phi) * math.cos(theta), math.cos(phi), -math.sin(phi) * math.sin(theta))
                self.vertex(add(center, scale(n, radius)), n, (slice_ / slices, stack / stacks))
        row = slices + 1
        for stack in range(stacks):
            for slice_ in range(slices):
                a = base + stack * row + slice_
                b, c, d = a + 1, a + row, a + row + 1
                if stack != 0:
                    self.indices += [a, c, b]
                if stack != stacks - 1:
                    self.indices += [b, c, d]

    def torus(self, major, minor, segments, sides):
        """Ring in the XY plane (axis +Z), facing the camera."""
        base = len(self.positions)
        for segment in range(segments + 1):
            alpha = 2.0 * math.pi * segment / segments
            ring = (math.cos(alpha), math.sin(alpha), 0.0)
            for side in range(sides + 1):
                beta = 2.0 * math.pi * side / sides
                n = add(scale(ring, math.cos(beta)), (0.0, 0.0, math.sin(beta)))
                p = add(scale(ring, major), scale(n, minor))
                self.vertex(p, n, (segment / segments, side / sides))
        row = sides + 1
        for segment in range(segments):
            for side in range(sides):
                a = base + segment * row + side
                b, c, d = a + 1, a + row, a + row + 1
                self.indices += [a, c, d, a, d, b]


class Lcg:
    """Deterministic 32-bit LCG (Numerical Recipes constants)."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def unit(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state / 4294967296.0


# --- Node library: name -> {material key: Prim}. Geometry is about the node pivot. --

def node_ground_checker():
    p = Prim()
    p.plane_xz(40.0, 40.0, 160.0)  # 0.25 m texture tile
    return {"ground_checker": p}


def node_ground_tile():
    p = Prim()
    p.plane_xz(40.0, 40.0, 40.0)  # 1 m tiles
    return {"ground_tile": p}


def node_backdrop_wall():
    p = Prim()
    p.wall_xy(14.0, 5.0, 7.0, 2.5)
    return {"backdrop": p}


def node_fence():
    """6 m x 2.4 m grille, pivot at the bottom centre. Left half: 8 mm bars (~4 px at
    6 m); right half: 3 mm bars (~1.5 px). 4 cm pitch."""
    p = Prim()
    width, height = 6.0, 2.4
    for x in (-width / 2, width / 2):
        p.box((x, height / 2, 0.0), (0.06, height, 0.06))
    for y in (0.12, height - 0.04):
        p.box((0.0, y, 0.0), (width, 0.04, 0.03))
    bars = int(round(width / 0.04))
    for index in range(1, bars):
        x = -width / 2 + index * 0.04
        thickness = 0.008 if x < 0.0 else 0.003
        p.box((x, (0.14 + height - 0.06) / 2, 0.0), (thickness, height - 0.2, thickness))
    return {"fence_metal": p}


def node_wires():
    """Horizontal and diagonal wires 12 m long; radius 3 mm down to 0.5 mm
    (sub-pixel beyond ~2 m in a native-4K 40-degree view)."""
    p = Prim()
    radii = [0.003, 0.002, 0.0015, 0.001, 0.00075, 0.0005]
    for index, radius in enumerate(radii):
        y = 2.55 + 0.14 * index
        p.cylinder((-6.0, y, 0.0), (6.0, y, 0.0), radius, 6)
    p.cylinder((-6.0, 3.45, 0.0), (6.0, 2.45, 0.0), 0.001, 6)
    p.cylinder((-6.0, 2.5, 0.2), (6.0, 3.4, 0.2), 0.0007, 6)
    return {"wire": p}


def foliage_cluster(seed, cards, radius, max_height):
    rng = Lcg(seed)
    p = Prim()
    for _ in range(cards):
        r = radius * math.sqrt(rng.unit())
        theta = 2.0 * math.pi * rng.unit()
        cx, cz = r * math.cos(theta), r * math.sin(theta)
        size = 0.35 + 0.3 * rng.unit()
        cy = 0.15 + (max_height - 0.15) * rng.unit()
        yaw = 2.0 * math.pi * rng.unit()
        tilt = (rng.unit() - 0.5) * 0.9
        right = (math.cos(yaw) * size / 2, 0.0, math.sin(yaw) * size / 2)
        up = (-math.sin(yaw) * math.sin(tilt) * size / 2, math.cos(tilt) * size / 2,
              math.cos(yaw) * math.sin(tilt) * size / 2)
        n = normalize(cross(right, up))
        center = (cx, cy, cz)
        p.quad(sub(sub(center, right), up), sub(add(center, right), up),
               add(add(center, right), up), add(sub(center, right), up), n)
    return p


def node_foliage_a():
    return {"leaf": foliage_cluster(0x1A2B3C4D, 72, 1.3, 1.9)}


def node_foliage_b():
    return {"leaf": foliage_cluster(0x5EED0042, 40, 0.8, 1.2)}


def node_props():
    """A row of coloured blocks and columns for parallax and disocclusion detail."""
    out = {key: Prim() for key in ("prop_red", "prop_green", "prop_blue", "prop_yellow", "prop_white")}
    out["prop_red"].box((-3.0, 0.4, 0.0), (0.8, 0.8, 0.8))
    out["prop_green"].box((-1.5, 0.6, -0.3), (0.5, 1.2, 0.5))
    out["prop_blue"].box((0.0, 0.35, 0.2), (1.2, 0.7, 0.6))
    out["prop_yellow"].box((1.6, 0.9, -0.2), (0.4, 1.8, 0.4))
    out["prop_white"].cylinder((3.0, 0.0, 0.0), (3.0, 2.2, 0.0), 0.25, 32)
    return out


def node_occluder():
    p = Prim()
    p.box((0.0, 1.3, 0.0), (2.4, 2.6, 0.25))
    return {"occluder": p}


def node_sphere_chrome():
    p = Prim()
    p.sphere((0.0, 0.0, 0.0), 1.0, 48, 96)
    return {"chrome": p}


def node_torus_gold():
    p = Prim()
    p.torus(0.8, 0.28, 96, 40)
    return {"gold": p}


def node_specular_balls():
    """5 x 3 small mirror spheres on a plinth: high curvature, sub-pixel highlights."""
    out = {"chrome_mirror": Prim(), "dark_matte": Prim()}
    out["dark_matte"].box((0.0, 0.2, 0.0), (1.8, 0.4, 1.0))
    for row in range(3):
        for column in range(5):
            out["chrome_mirror"].sphere((-0.7 + 0.35 * column, 0.52, -0.3 + 0.3 * row), 0.12, 12, 24)
    return out


def node_emissive_bar():
    p = Prim()
    p.box((0.0, 0.0, 0.0), (2.4, 0.12, 0.12))
    return {"emissive_orange": p}


def node_emissive_spinner():
    p = Prim()
    p.box((0.0, 0.0, 0.0), (1.6, 0.08, 0.08))
    p.box((0.0, 0.0, 0.0), (0.08, 0.6, 0.08))
    return {"emissive_cyan": p}


def node_glass_panel():
    """2.0 m x 1.5 m thin glass sheet (z = 0) in a 5 cm frame; pivot at its centre."""
    glass, frame = Prim(), Prim()
    w, h = 1.0, 0.75
    glass.quad((-w, -h, 0.0), (w, -h, 0.0), (w, h, 0.0), (-w, h, 0.0), (0, 0, 1))
    frame.box((0.0, h + 0.025, 0.0), (2 * w + 0.1, 0.05, 0.05))
    frame.box((0.0, -h - 0.025, 0.0), (2 * w + 0.1, 0.05, 0.05))
    frame.box((w + 0.025, 0.0, 0.0), (0.05, 2 * h, 0.05))
    frame.box((-w - 0.025, 0.0, 0.0), (0.05, 2 * h, 0.05))
    return {"glass": glass, "glass_frame": frame}


def node_reactive_glass():
    """M9.3: a frameless 1.6 m x 1.0 m thin-glass sheet (z = 0); pivot at its centre."""
    p = Prim()
    p.quad((-0.8, -0.5, 0.0), (0.8, -0.5, 0.0), (0.8, 0.5, 0.0), (-0.8, 0.5, 0.0), (0, 0, 1))
    return {"reactive_glass": p}


def node_particle_sorted():
    """M9.3: a 1.0 m particle card (z = 0); pivot at its centre."""
    p = Prim()
    p.quad((-0.5, -0.5, 0.0), (0.5, -0.5, 0.0), (0.5, 0.5, 0.0), (-0.5, 0.5, 0.0), (0, 0, 1))
    return {"particle_sorted": p}


def node_particle_oit():
    """M9.3: a 0.8 m WeightedOIT particle card (z = 0); pivot at its centre."""
    p = Prim()
    p.quad((-0.4, -0.4, 0.0), (0.4, -0.4, 0.0), (0.4, 0.4, 0.0), (-0.4, 0.4, 0.0), (0, 0, 1))
    return {"particle_oit": p}


def node_highlights():
    """13 x 5 tiny emissive spheres; rows shrink from 10 mm to 0.75 mm radius (from
    ~6 px to well under a pixel at 4.5 m) while strength rises to 4096."""
    out = {"highlight_64": Prim(), "highlight_512": Prim(), "highlight_4096": Prim()}
    rows = [(0.010, "highlight_64"), (0.006, "highlight_512"), (0.003, "highlight_4096"),
            (0.0015, "highlight_4096"), (0.00075, "highlight_4096")]
    for row, (radius, key) in enumerate(rows):
        for column in range(13):
            out[key].sphere((-1.8 + 0.3 * column, 0.5 - 0.25 * row, 0.0), radius, 6, 12)
    return out


def node_hdr_plate():
    p = Prim()
    p.box((0.0, 0.0, 0.0), (4.4, 1.6, 0.04))
    return {"dark_matte": p}


def node_teleport_cube():
    p = Prim()
    p.box((0.0, 0.0, 0.0), (0.9, 0.9, 0.9))
    return {"backdrop": p}


NODES = {
    "ground_checker": node_ground_checker, "ground_tile": node_ground_tile,
    "backdrop_wall": node_backdrop_wall, "fence": node_fence, "wires": node_wires,
    "foliage_a": node_foliage_a, "foliage_b": node_foliage_b, "props": node_props,
    "occluder": node_occluder, "sphere_chrome": node_sphere_chrome,
    "torus_gold": node_torus_gold, "specular_balls": node_specular_balls,
    "emissive_bar": node_emissive_bar, "emissive_spinner": node_emissive_spinner,
    "glass_panel": node_glass_panel, "highlights": node_highlights,
    "hdr_plate": node_hdr_plate, "teleport_cube": node_teleport_cube,
    "reactive_glass": node_reactive_glass, "particle_sorted": node_particle_sorted,
    "particle_oit": node_particle_oit,
}


# --- glTF writer --------------------------------------------------------------------

def build_gltf(title, node_names):
    """Returns (gltf bytes, material keys in index order, primitive source keys,
    image count)."""
    binary = bytearray()
    buffer_views, accessors = [], []

    def view(data, target):
        while len(binary) % 4:
            binary.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data),
                             "target": target})
        binary.extend(data)
        return len(buffer_views) - 1

    def float_accessor(values, kind):
        width = len(values[0])
        data = b"".join(struct.pack("<" + "f" * width, *v) for v in values)
        accessor = {"bufferView": view(data, 34962), "componentType": 5126,
                    "count": len(values), "type": kind}
        if kind == "VEC3":
            stored = [struct.unpack("<fff", struct.pack("<fff", *v)) for v in values]
            accessor["min"] = [min(v[i] for v in stored) for i in range(3)]
            accessor["max"] = [max(v[i] for v in stored) for i in range(3)]
        accessors.append(accessor)
        return len(accessors) - 1

    def index_accessor(indices, vertex_count):
        wide = vertex_count > 65535
        data = struct.pack("<" + ("I" if wide else "H") * len(indices), *indices)
        accessors.append({"bufferView": view(data, 34963),
                          "componentType": 5125 if wide else 5123,
                          "count": len(indices), "type": "SCALAR"})
        return len(accessors) - 1

    material_keys, texture_keys = [], []
    meshes, nodes, primitive_keys = [], [], []
    for node_index, name in enumerate(node_names):
        primitives = []
        for primitive_index, (key, prim) in enumerate(NODES[name]().items()):
            if key not in material_keys:
                material_keys.append(key)
            texture = MATERIALS[key]["pbrMetallicRoughness"].get("baseColorTexture")
            if texture and texture["texture"] not in texture_keys:
                texture_keys.append(texture["texture"])
            primitives.append({
                "attributes": {
                    "POSITION": float_accessor(prim.positions, "VEC3"),
                    "NORMAL": float_accessor(prim.normals, "VEC3"),
                    "TEXCOORD_0": float_accessor(prim.uvs, "VEC2"),
                },
                "indices": index_accessor(prim.indices, len(prim.positions)),
                "material": material_keys.index(key),
            })
            primitive_keys.append(f"nodes/{node_index}/meshes/{node_index}/primitives/{primitive_index}")
        meshes.append({"name": name, "primitives": primitives})
        nodes.append({"name": name, "mesh": node_index})

    materials = []
    for key in material_keys:
        material = json.loads(json.dumps(MATERIALS[key]))
        texture = material["pbrMetallicRoughness"].get("baseColorTexture")
        if texture:
            texture["index"] = texture_keys.index(texture.pop("texture"))
        materials.append(material)
    images = []
    for key in texture_keys:
        png, _ = TEXTURES[key]()
        images.append({"name": key, "mimeType": "image/png",
                       "uri": "data:image/png;base64," + base64.b64encode(png).decode("ascii")})
    extensions = sorted({ext for m in materials for ext in m.get("extensions", {})})

    gltf = {"asset": {"version": "2.0", "generator": GENERATOR}}
    if extensions:
        gltf["extensionsUsed"] = extensions
    gltf.update({
        "scene": 0,
        "scenes": [{"name": title, "nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
    })
    if images:
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
        gltf["images"] = images
        gltf["textures"] = [{"sampler": 0, "source": index} for index in range(len(images))]
    gltf["accessors"] = accessors
    gltf["bufferViews"] = buffer_views
    gltf["buffers"] = [{"byteLength": len(binary),
                        "uri": "data:application/octet-stream;base64," +
                               base64.b64encode(bytes(binary)).decode("ascii")}]
    text = json.dumps(gltf, indent=1, ensure_ascii=True) + "\n"
    return text.encode("ascii"), material_keys, primitive_keys, len(images)


def guid(file_ordinal, sub_ordinal, file_name, source_key):
    """UUIDv7-shaped, deterministic: a fixed timestamp per file, the subasset ordinal
    in the version/variant fields and a SHA-256 tail of the file and source key."""
    tail = hashlib.sha256(f"{file_name}/{source_key}".encode("ascii")).hexdigest()[:12]
    timestamp = 0x01A190000000 + file_ordinal
    return (f"{timestamp >> 16:08x}-{timestamp & 0xFFFF:04x}-{0x7000 | (sub_ordinal & 0xFFF):04x}-"
            f"{0x8000 | ((sub_ordinal >> 12) & 0x3FFF):04x}-{tail}")


def build_sidecar(file_ordinal, file_name, material_keys, primitive_keys, image_count):
    subassets, ordinal = [], 1
    policies = {}
    for index, key in enumerate(material_keys):
        source_key = f"materials/{index}"
        material_guid = guid(file_ordinal, ordinal, file_name, source_key)
        subassets.append({"guid": material_guid, "assetType": "iridium.material", "sourceKey": source_key})
        if key in MATERIAL_POLICIES:
            policies[material_guid] = MATERIAL_POLICIES[key]
        ordinal += 1
    for index in range(image_count):
        source_key = f"images/{index}"
        subassets.append({"guid": guid(file_ordinal, ordinal, file_name, source_key),
                          "assetType": "iridium.texture", "sourceKey": source_key})
        ordinal += 1
    for source_key in primitive_keys:
        subassets.append({"guid": guid(file_ordinal, ordinal, file_name, source_key),
                          "assetType": "iridium.model-primitive", "sourceKey": source_key})
        ordinal += 1
    subassets.sort(key=lambda entry: entry["sourceKey"])
    sidecar = {
        "schemaVersion": 1,
        "assetGuid": guid(file_ordinal, 0, file_name, ""),
        "assetType": "iridium.model",
        "importer": {"id": "iridium.gltf-model", "version": 7},
        "settings": {"schemaVersion": 2, "values": {
            "bake_node_transforms": True,
            "generate_missing_tangents": True,
            "import_scale": 1.0,
            "preserve_rt_geometry": True,
            "transparency_execution_mode": "classified",
            "transparency_policies": policies,
        }},
        "subassets": subassets,
        "tags": ["m9-g6b-fixture", "tracked"],
    }
    return (json.dumps(sidecar, indent=2) + "\n").encode("ascii")


# --- Fixtures --------------------------------------------------------------------

SUN = {"type": "directional", "position": [0.0, 0.0, 0.0], "rotation_degrees": [50.0, 160.0, 0.0],
       "color_linear_rec709": [1.0, 0.96, 0.9], "illuminance_lux": 80000.0, "range_meters": 1.0,
       "casts_shadows": True, "shadow_quality": "ultra", "priority": 1}


def sun(lux):
    light = dict(SUN)
    light["illuminance_lux"] = lux
    return light


def camera(identifier, position, target, near=0.05, far=120.0):
    return {"id": identifier, "position": position, "target": target, "up": [0.0, 1.0, 0.0],
            "vertical_fov_degrees": 40.0, "near": near, "far": far}


def entity(identifier, node, translation=None, rotation=None, motion=None):
    result = {"id": identifier, "node": node}
    transform = {}
    if translation is not None:
        transform["translation"] = translation
    if rotation is not None:
        transform["rotation_degrees"] = rotation
    if transform:
        result["transform"] = transform
    if motion is not None:
        result["motion"] = motion
    return result


def back_and_forth(a, b, period):
    return {"kind": "keyframes", "period_frames": period, "keyframes": [
        {"frame": 0, "translation": a}, {"frame": period // 2, "translation": b},
        {"frame": period, "translation": a}]}


def r4(value):
    return round(value, 4) + 0.0


def orbit_path(center, radius, height, keyframe_step_degrees, frames_per_step):
    steps = int(round(360 / keyframe_step_degrees))
    keyframes = []
    for step in range(steps + 1):
        angle = math.radians(keyframe_step_degrees * (step % steps))
        keyframes.append({"frame": step * frames_per_step,
                          "position": [r4(center[0] + radius * math.sin(angle)), height,
                                       r4(center[2] + radius * math.cos(angle))],
                          "target": center})
    return {"path": {"period_frames": steps * frames_per_step, "keyframes": keyframes}}


# Each fixture: glTF file name, node list, then the manifest record.
FIXTURES = [
    {
        "key": "TF-thin", "id": "m9_tf_thin_v1", "file": "temporal_thin.gltf",
        "nodes": ["ground_checker", "fence", "wires"],
        "environment": [0.05, 0.06, 0.08], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_thin_camera_v1", [0.0, 1.6, 6.0], [0.0, 1.5, 0.0]),
        "entities": [entity("ground", 0), entity("fence", 1), entity("wires", 2, [0.0, 0.0, -0.6])],
        "camera_motion": {"path": {"period_frames": 480, "keyframes": [
            {"frame": 0, "position": [-0.8, 1.6, 6.0], "target": [-0.8, 1.5, 0.0]},
            {"frame": 240, "position": [0.8, 1.6, 6.0], "target": [0.8, 1.5, 0.0]},
            {"frame": 480, "position": [-0.8, 1.6, 6.0], "target": [-0.8, 1.5, 0.0]}]}},
        "expected": [
            "A 6 m grille (8 mm bars left, 3 mm bars right, 4 cm pitch) and 0.5-3 mm wires fill the native-4K view under a slow lateral camera pan.",
            "Thin coverage is continuous: no bar or wire breaks into dashes, crawls or flickers as it slides across pixels.",
            "The 4 mm ground grid lines and 16 mm checker cells stay stable rather than aliasing into moire.",
        ],
    },
    {
        "key": "TF-foliage", "id": "m9_tf_foliage_v1", "file": "temporal_foliage.gltf",
        "nodes": ["ground_tile", "foliage_a", "foliage_b", "backdrop_wall"],
        "environment": [0.06, 0.07, 0.09], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_foliage_camera_v1", [0.0, 1.3, 4.2], [0.0, 0.9, 0.0]),
        "entities": [
            entity("ground", 0),
            entity("bush_turning", 1, [-0.9, 0.0, 0.0],
                   motion={"kind": "rotation", "axis": [0.0, 1.0, 0.0], "degrees_per_frame": 0.15}),
            entity("bush_static", 2, [1.5, 0.0, -0.8]),
            entity("wall", 3, [0.0, 0.0, -3.0]),
        ],
        "expected": [
            "72 + 40 alpha-mask leaf cards (binary alpha, cutoff 0.5) cover most of the view in front of a colour-band wall.",
            "The turning bush keeps crisp, stable mask edges with no shimmering, ghosted leaves or smeared background between cards.",
            "The static bush is stable frame to frame.",
        ],
    },
    {
        "key": "TF-disocclude", "id": "m9_tf_disocclude_v1", "file": "temporal_disocclude.gltf",
        "nodes": ["ground_checker", "backdrop_wall", "props", "occluder"],
        "environment": [0.05, 0.06, 0.08], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_disocclude_camera_v1", [0.0, 1.4, 7.0], [0.0, 1.2, 0.0]),
        "entities": [
            entity("ground", 0), entity("wall", 1, [0.0, 0.0, -2.5]), entity("props", 2, [0.0, 0.0, -0.5]),
            entity("occluder", 3, [-3.5, 0.0, 1.5],
                   motion=back_and_forth([-3.5, 0.0, 1.5], [3.5, 0.0, 1.5], 240)),
        ],
        "expected": [
            "A 2.4 m x 2.6 m matte slab sweeps 7 m across the view every 120 frames in front of props, a colour-band wall and a high-frequency floor.",
            "Disoccluded regions behind the slab's trailing edge recover the background without a red trail or ghost.",
            "The leading edge never smears background over the slab.",
        ],
    },
    {
        "key": "TF-pan", "id": "m9_tf_pan_v1", "file": "temporal_pan.gltf",
        "nodes": ["ground_checker", "fence", "props", "sphere_chrome", "backdrop_wall"],
        "environment": [0.06, 0.07, 0.09], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_pan_camera_v1", [0.0, 1.6, 8.0], [-6.0, 1.2, 0.0]),
        "entities": [
            entity("ground", 0), entity("fence", 1, [0.0, 0.0, -1.0]), entity("props", 2, [0.0, 0.0, 1.2]),
            entity("sphere", 3, [-3.2, 1.0, 2.2]), entity("wall", 4, [0.0, 0.0, -4.0]),
        ],
        "camera_motion": {"path": {"period_frames": 240, "cuts": [0, 90, 180], "keyframes": [
            {"frame": 0, "position": [0.0, 1.6, 8.0], "target": [-6.0, 1.2, 0.0]},
            {"frame": 30, "position": [0.0, 1.6, 8.0], "target": [-6.0, 1.2, 0.0]},
            {"frame": 89, "position": [0.0, 1.6, 8.0], "target": [6.0, 1.2, 0.0]},
            {"frame": 90, "position": [6.0, 2.5, 5.0], "target": [0.0, 1.0, 0.0]},
            {"frame": 179, "position": [4.5, 2.2, 4.0], "target": [0.0, 1.0, 0.0]},
            {"frame": 180, "position": [-5.0, 1.2, 6.0], "target": [0.0, 1.0, 0.0]},
            {"frame": 239, "position": [-4.2, 1.3, 6.6], "target": [0.0, 1.0, 0.0]}]}},
        "expected": [
            "Hold 30 frames, then a fast 74-degree yaw pan in 59 frames (about 1.25 degrees per frame), then cuts at frames 90, 180 and every period wrap (240).",
            "Each cut bumps the view history-reset revision to the cut ordinal; no pre-cut content ghosts into the first post-cut frame.",
            "During the pan, the grille and floor resolve without smearing or excessive blur; history rejection, not accumulation, dominates.",
        ],
    },
    {
        "key": "TF-emissive", "id": "m9_tf_emissive_v1", "file": "temporal_emissive.gltf",
        "nodes": ["ground_tile", "backdrop_wall", "emissive_bar", "emissive_spinner"],
        "environment": [0.01, 0.01, 0.015], "lights": [sun(20000.0)],
        "camera": camera("m9_tf_emissive_camera_v1", [0.0, 1.5, 6.0], [0.0, 1.4, 0.0]),
        "entities": [
            entity("ground", 0), entity("wall", 1, [0.0, 0.0, -2.0]),
            entity("bar", 2, [-3.0, 1.2, 0.5], motion=back_and_forth([-3.0, 1.2, 0.5], [3.0, 1.2, 0.5], 180)),
            entity("spinner", 3, [0.0, 2.4, -0.5],
                   motion={"kind": "rotation", "axis": [0.0, 0.0, 1.0], "degrees_per_frame": 2.0}),
        ],
        "expected": [
            "An emissive bar (strength 40) sweeps 6 m every 90 frames and a cyan emissive cross (strength 25) spins 2 degrees per frame over a dim scene.",
            "Moving emissive surfaces leave no bright trails and do not dim or flicker from history clamping.",
            "Emissive light does not illuminate other surfaces (emission is surface radiance only).",
        ],
    },
    {
        "key": "TF-glass", "id": "m9_tf_glass_v1", "file": "temporal_glass.gltf",
        "nodes": ["ground_checker", "backdrop_wall", "props", "glass_panel"],
        "environment": [0.06, 0.07, 0.09], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_glass_camera_v1", [0.0, 1.4, 6.0], [0.0, 1.2, 0.0]),
        "entities": [
            entity("ground", 0), entity("wall", 1, [0.0, 0.0, -2.5]), entity("props", 2, [0.0, 0.0, -0.5]),
            entity("glass", 3, [-2.5, 1.2, 1.5], motion=back_and_forth([-2.5, 1.2, 1.5], [2.5, 1.2, 1.5], 240)),
        ],
        "expected": [
            "A framed 2 m x 1.5 m blue-tinted thin-glass panel (transmission 1, IOR 1.5, roughness 0.1, thin_glass policy) slides 5 m every 120 frames in front of props and a colour-band wall.",
            "Content seen through the moving glass stays sharp and correctly placed: no ghosted copies of the background, no smearing at the frame edges.",
            "Glass writes no depth, so reprojection behind it follows the opaque background.",
        ],
    },
    {
        "key": "TF-specular", "id": "m9_tf_specular_v1", "file": "temporal_specular.gltf",
        "nodes": ["ground_tile", "sphere_chrome", "torus_gold", "specular_balls"],
        "environment": [0.2, 0.22, 0.26], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_specular_camera_v1", [0.0, 1.8, 5.5], [0.0, 1.0, 0.0]),
        "entities": [
            entity("ground", 0), entity("sphere", 1, [-1.3, 1.0, 0.0]),
            entity("torus", 2, [1.4, 1.1, 0.0],
                   motion={"kind": "rotation", "axis": [0.3, 1.0, 0.2], "degrees_per_frame": 0.25}),
            entity("balls", 3, [0.0, 0.0, 1.4]),
        ],
        "camera_motion": orbit_path([0.0, 1.0, 0.0], 5.5, 1.8, 10.0, 40),
        "expected": [
            "A chrome sphere (roughness 0.04), a turning gold torus (0.12) and fifteen 12 cm mirror spheres (0.02) under a sun, with a full slow camera orbit every 1440 frames.",
            "Sun highlights on the curved low-roughness surfaces stay stable: no sparkle, crawling or popping as they slide across pixels.",
        ],
    },
    {
        "key": "TF-static", "id": "m9_tf_static_v1", "file": "temporal_static.gltf",
        "nodes": ["ground_checker", "fence", "wires", "foliage_a", "sphere_chrome", "specular_balls"],
        "environment": [0.06, 0.07, 0.09], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_static_camera_v1", [0.5, 1.7, 6.5], [0.0, 1.2, 0.0]),
        "entities": [
            entity("ground", 0), entity("fence", 1, [0.0, 0.0, -1.5]), entity("wires", 2, [0.0, 0.0, -1.8]),
            entity("bush", 3, [-1.6, 0.0, 0.4]), entity("sphere", 4, [1.6, 1.0, 0.0]),
            entity("balls", 5, [0.2, 0.0, 1.5]),
        ],
        "expected": [
            "Static camera and content: thin bars and wires, alpha-mask foliage, a chrome sphere, mirror spheres and a high-frequency floor.",
            "With temporal AA the converged image is stable: per-pixel frame-to-frame luma change approaches zero (jitter must not read as flicker).",
        ],
    },
    {
        "key": "TF-hdr", "id": "m9_tf_hdr_v1", "file": "temporal_hdr.gltf",
        "nodes": ["ground_tile", "hdr_plate", "highlights"],
        "environment": [0.004, 0.004, 0.005], "lights": [sun(5000.0)],
        "camera": camera("m9_tf_hdr_camera_v1", [0.0, 1.0, 4.5], [0.0, 1.0, 0.0]),
        "entities": [
            entity("ground", 0), entity("plate", 1, [0.0, 0.85, -0.1]),
            entity("highlights", 2, [0.0, 1.0, 0.0],
                   motion=back_and_forth([0.0, 1.0, 0.0], [0.04, 1.0, 0.0], 240)),
        ],
        "expected": [
            "65 emissive points (strength 64, 512 and 4096; radius 10 mm down to 0.75 mm) over a dark plate, drifting 4 cm (about 0.2 px per frame).",
            "Sub-pixel super-bright points neither flicker (fireflies) nor vanish; their integrated energy is stable frame to frame.",
            "Bloom and exposure adaptation (when enabled) stay stable on the small highlights.",
        ],
    },
    {
        "key": "TF-teleport", "id": "m9_tf_teleport_v1", "file": "temporal_teleport.gltf",
        "nodes": ["ground_checker", "backdrop_wall", "teleport_cube", "props"],
        "environment": [0.05, 0.06, 0.08], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_teleport_camera_v1", [0.0, 1.5, 7.0], [0.0, 0.8, 0.0]),
        "entities": [
            entity("ground", 0), entity("wall", 1, [0.0, 0.0, -2.5]),
            entity("cube", 2, [-3.2, 0.6, 0.8], motion={"kind": "keyframes", "period_frames": 240, "keyframes": [
                {"frame": 0, "translation": [-3.2, 0.6, 0.8], "teleport": True},
                {"frame": 60, "translation": [-0.8, 0.6, 0.8]},
                {"frame": 61, "translation": [1.6, 0.6, 0.8], "teleport": True},
                {"frame": 120, "translation": [3.2, 0.6, 0.8]},
                {"frame": 121, "translation": [0.0, 0.6, -0.6], "teleport": True},
                {"frame": 180, "translation": [0.0, 0.6, 1.8]}]}),
            entity("props", 3, [0.0, 0.0, -1.2]),
        ],
        "expected": [
            "A colour-band cube moves, then teleports 2.4 m at frame 61, again at frame 121 and at every period wrap (240); the evaluated pose flags each teleport.",
            "Neither the cube's old position nor its old appearance ghosts after a teleport; per-instance history resets on the flagged frames.",
        ],
    },
    {
        # M9.3: transparency over a static, high-contrast backdrop under a static camera.
        # Transparent surfaces write no velocity or depth, so every pixel they cover
        # reprojects as still background: the reactive (history suppression) case.
        "key": "TF-reactive", "id": "m9_tf_reactive_v1", "file": "temporal_reactive.gltf",
        "nodes": ["ground_checker", "backdrop_wall", "reactive_glass", "particle_sorted", "particle_oit"],
        "environment": [0.06, 0.07, 0.09], "lights": [sun(80000.0)],
        "camera": camera("m9_tf_reactive_camera_v1", [0.0, 1.4, 6.0], [0.0, 1.2, 0.0]),
        "entities": [
            entity("ground", 0), entity("wall", 1, [0.0, 0.0, -2.5]),
            entity("glass", 2, [-2.0, 2.2, 1.0],
                   motion=back_and_forth([-2.0, 2.2, 1.0], [2.0, 2.2, 1.0], 320)),
            entity("particle", 3, [2.2, 1.15, 1.2],
                   motion=back_and_forth([2.2, 1.15, 1.2], [-2.2, 1.15, 1.2], 720)),
            entity("particle_oit", 4, [-2.4, 0.4, 1.6],
                   motion=back_and_forth([-2.4, 0.4, 1.6], [2.4, 0.4, 1.6], 960)),
        ],
        "expected": [
            "Static camera over a colour-band wall and a high-frequency floor. A frameless, strongly tinted thin-glass sheet (attenuation colour 0.45/0.7/1.0, roughness 0.15) slides 4 m every 160 frames (about 15 px per frame); a ringed emissive alpha-blended card (SortedSurface, alpha 0.6) slides 4.4 m every 360 frames (about 7 px per frame); a ringed emissive WeightedOIT card (alpha 0.55) slides 4.8 m every 480 frames (about 7 px per frame).",
            "No transparent surface leaves a trail or a ghosted copy of itself or of its old tint over the backdrop, and the backdrop seen through each stays sharp.",
            "Transparency writes no velocity or depth: TAA sees the still background under every transparent pixel.",
        ],
    },
]


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def generate():
    files = {}
    records = []
    for ordinal, fixture in enumerate(FIXTURES):
        gltf, material_keys, primitive_keys, image_count = build_gltf(fixture["key"], fixture["nodes"])
        sidecar_name = fixture["file"] + ".iridium.meta"
        sidecar = build_sidecar(ordinal + 1, fixture["file"], material_keys, primitive_keys, image_count)
        files[fixture["file"]] = gltf
        files[sidecar_name] = sidecar
        factory = {"kind": "composition", "entities": fixture["entities"]}
        if "camera_motion" in fixture:
            factory["camera_motion"] = fixture["camera_motion"]
        records.append({
            "id": fixture["id"],
            "revision": 1,
            "required": True,
            "source_asset": fixture["file"],
            "environment": {"kind": "procedural_constant", "constant_linear_rgb": fixture["environment"]},
            "camera": fixture["camera"],
            "scene_factory": factory,
            "lights": fixture["lights"],
            "output_label": fixture["id"],
            "warmup_frames": 120,
            "measured_frames": 600,
            "content_files": [
                {"path": fixture["file"], "sha256": sha256_bytes(gltf)},
                {"path": sidecar_name, "sha256": sha256_bytes(sidecar)},
            ],
            "expected_behavior": fixture["expected"],
            "unavailable_capabilities": [],
        })
    manifest = {"schema_version": 1, "generator": GENERATOR, "fixtures": records}
    files[MANIFEST_NAME] = (json.dumps(manifest, indent=2) + "\n").encode("ascii")
    return files


def main(argv):
    check = "--check" in argv
    files = generate()
    mismatches = 0
    for name, data in files.items():
        path = OUT_DIR / name
        if check:
            if not path.exists() or path.read_bytes() != data:
                print(f"differs: {path.relative_to(ROOT).as_posix()}")
                mismatches += 1
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{len(data):>9} {sha256_bytes(data)[:16]} {path.relative_to(ROOT).as_posix()}")
    if check:
        print("up to date" if mismatches == 0 else f"{mismatches} file(s) differ")
        return 1 if mismatches else 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
