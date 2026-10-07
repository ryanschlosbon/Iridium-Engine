"""Generates assets/m7c-owner-cases-manifest.v1.json, the M7 completion owner-case
fixtures (docs/milestones/M7-completion.md, M7.P1), and prints per fixture how many
instance bounds intersect the camera frustum at 16:9 (the PC1 half-off and all-off
cameras are chosen with it). Content hashes are computed from the local asset library
(IRIDIUM_LOCAL_ASSET_ROOT, else iridium.local.json); only hashes enter the manifest.

    python tools/m7r/Generate-OwnerCaseManifest.py
"""
import hashlib
import json
import math
import os
import re
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / 'assets' / 'm7c-owner-cases-manifest.v1.json'
ASPECT = 3840 / 2160


def local_asset_root():
    configured = os.environ.get('IRIDIUM_LOCAL_ASSET_ROOT', '').strip()
    if not configured:
        config = REPO / 'iridium.local.json'
        if config.is_file():
            configured = json.loads(config.read_text(encoding='utf-8')).get('localAssetRoot', '').strip()
    if not configured:
        raise SystemExit('No local asset root (IRIDIUM_LOCAL_ASSET_ROOT or iridium.local.json).')
    return Path(configured)


LOCAL = local_asset_root()

P911 = 'models/porsche_911_carrera4s/porsche_911_carrera4s.gltf'
P930 = 'porsche911930t/porsche911930t.gltf'
ALFA = 'models/alfa_romeo/alfa_romeo.gltf'
# Cooked-model bounds (IridiumInspectCookedModel).
BOUNDS = {
    P911: ((-1.390, -0.633, -2.476), (1.360, 0.626, 2.602)),
    P930: ((-1.558, 0.095, -2.892), (1.558, 1.966, 3.341)),
    ALFA: ((-0.840, -0.020, -2.281), (0.840, 1.291, 1.853)),
}


def sha(path):
    h = hashlib.sha256()
    with open(LOCAL / path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def content(*sources):
    files = []
    for source in sources:
        files.append({'path': source, 'sha256': sha(source)})
        try:
            files.append({'path': source + '.iridium.meta', 'sha256': sha(source + '.iridium.meta')})
        except FileNotFoundError:
            pass
    return files


ENV = {'kind': 'procedural_constant', 'constant_linear_rgb': [0.3, 0.35, 0.4]}
SUN = {
    'type': 'directional', 'position': [0.0, 0.0, 0.0],
    'rotation_degrees': [45.0, 210.0, 0.0],
    'color_linear_rec709': [1.0, 0.96, 0.9], 'illuminance_lux': 10000.0,
    'range_meters': 1.0, 'casts_shadows': False, 'shadow_quality': 'high', 'priority': 1,
}


def camera(cid, position, target, fov=40.0, near=0.05, far=200.0):
    return {'id': cid, 'position': [float(v) for v in position], 'target': [float(v) for v in target],
            'up': [0.0, 1.0, 0.0], 'vertical_fov_degrees': float(fov), 'near': near, 'far': far}


def look_at(eye, target, up=(0, 1, 0)):
    eye, target, up = map(np.array, (eye, target, up))
    f = target - eye
    f = f / np.linalg.norm(f)
    s = np.cross(f, up)
    s = s / np.linalg.norm(s)
    u = np.cross(s, f)
    m = np.identity(4)
    m[0, :3], m[1, :3], m[2, :3] = s, u, -f
    m[0, 3], m[1, 3], m[2, 3] = -s @ eye, -u @ eye, f @ eye
    return m


def perspective(fov, aspect, near, far):
    t = math.tan(math.radians(fov) / 2)
    m = np.zeros((4, 4))
    m[0, 0] = 1 / (aspect * t)
    m[1, 1] = 1 / t
    m[2, 2] = far / (near - far)
    m[2, 3] = -(far * near) / (far - near)
    m[3, 2] = -1
    return m


def visible(cam, boxes):
    vp = perspective(cam['vertical_fov_degrees'], ASPECT, cam['near'], cam['far']) @ \
        look_at(cam['position'], cam['target'])
    count = 0
    for lo, hi in boxes:
        corners = np.array([[x, y, z, 1] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
        clip = corners @ vp.T
        w = clip[:, 3]
        outside = (np.all(clip[:, 0] < -w) or np.all(clip[:, 0] > w) or np.all(clip[:, 1] < -w) or
                   np.all(clip[:, 1] > w) or np.all(clip[:, 2] < 0) or np.all(clip[:, 2] > w))
        count += 0 if outside else 1
    return count


def grid_boxes(source, grid, spacing):
    lo, hi = map(np.array, BOUNDS[source])
    center = (np.array(grid) - 1) * 0.5
    boxes = []
    for z in range(grid[2]):
        for y in range(grid[1]):
            for x in range(grid[0]):
                p = (np.array([x, y, z]) - center) * np.array(spacing)
                boxes.append((lo + p, hi + p))
    return boxes


fixtures = []
report = []


def add(fid, source, cam, factory, lights, expected, boxes, sources=None, unavailable=None):
    sources = sources or [source]
    fixture = {
        'id': fid, 'revision': 1, 'required': False, 'source_asset': source,
        'environment': ENV, 'camera': cam, 'scene_factory': factory, 'lights': lights,
        'output_label': fid[:-3] if fid.endswith('_v1') else fid,
        'warmup_frames': 500, 'measured_frames': 2000,
        'content_files': content(*sources),
        'expected_behavior': expected,
    }
    if unavailable:
        fixture['unavailable_capabilities'] = unavailable
    fixtures.append(fixture)
    report.append((fid, visible(cam, boxes), len(boxes)))


# --- PC1 many objects ---------------------------------------------------------------
PC1_GRIDS = {
    1: ([1, 1, 1], [0.0, 0.0, 0.0]),
    4: ([4, 1, 1], [4.0, 0.0, 0.0]),
    16: ([4, 1, 4], [4.0, 0.0, 7.0]),
}
# (all_visible, half_off, all_off) cameras: position, target, fov.
PC1_CAMERAS = {
    1: [((0.0, 1.2, 9.0), (0.0, 0.0, 0.0), 40.0),
        ((0.0, 1.2, 9.0), (5.8, 0.2, 0.0), 40.0),
        ((0.0, 1.2, 9.0), (0.0, 1.2, 18.0), 40.0)],
    4: [((0.0, 4.0, 16.0), (0.0, 0.0, 0.0), 45.0),
        ((0.0, 4.0, 16.0), (11.8, 0.0, 0.0), 45.0),
        ((0.0, 4.0, 16.0), (0.0, 4.0, 32.0), 45.0)],
    16: [((0.0, 6.0, 26.0), (0.0, 0.0, 0.0), 45.0),
         ((0.0, 6.0, 26.0), (22.0, 0.0, 0.0), 45.0),
         ((0.0, 6.0, 26.0), (0.0, 6.0, 52.0), 45.0)],
}
STATES = [('all_visible', 'every instance inside the main-camera frustum'),
          ('half_off', 'about half of the instances outside the main-camera frustum'),
          ('all_off', 'the camera turned away from the same position: every instance outside the main-camera frustum '
                      'but inside the far plane')]
for n in (1, 4, 16):
    grid, spacing = PC1_GRIDS[n]
    for (state, text), (pos, target, fov) in zip(STATES, PC1_CAMERAS[n]):
        fid = f'm7c_pc1_911_n{n}_{state}_v1'
        what = text.format(n=n)
        if n == 1 and state == 'half_off':
            what = 'the single instance straddles the left frustum edge (about half of it outside)'
        count = '1 instance' if n == 1 else f'{n} instances'
        add(fid, P911, camera(fid.replace('_v1', '_camera_v1'), pos, target, fov),
            {'kind': 'instanced_grid', 'instance_grid': grid, 'instance_spacing': spacing},
            [SUN],
            [f'PC1 many objects: Porsche 911 Carrera 4S, {count} in a {grid[0]}x{grid[2]} grid; {what}.',
             'One unshadowed directional light and a constant environment (as m7_porsche_transparency_oblique_v1).'],
            grid_boxes(P911, grid, spacing))

fid = 'm7c_pc1_alfa_n16_all_visible_v1'
add(fid, ALFA, camera(fid.replace('_v1', '_camera_v1'), (0.0, 12.0, 24.0), (0.0, 0.0, 0.0), 45.0),
    {'kind': 'instanced_grid', 'instance_grid': [4, 1, 4], 'instance_spacing': [3.0, 0.0, 6.0]},
    [SUN],
    ['PC1 many objects: Alfa Romeo, 16 instances in a 4x4 grid, all inside the main-camera frustum.',
     'One unshadowed directional light and a constant environment.'],
    grid_boxes(ALFA, [4, 1, 4], [3.0, 0.0, 6.0]))

# --- PC2 close to glass -----------------------------------------------------------------
PC2_POINT = {
    'type': 'point', 'position': [1.2, 1.6, 1.8], 'rotation_degrees': [0.0, 0.0, 0.0],
    'color_linear_rec709': [1.0, 0.9, 0.8], 'luminous_intensity_candela': 10000.0,
    'range_meters': 10.0, 'source_radius_meters': 0.05,
    'casts_shadows': True, 'shadow_quality': 'high', 'priority': 2,
}
PC2_CAMERAS = {
    'far': ((2.2, 1.6, 5.0), (0.0, 0.35, 0.6), 40.0, '5%'),
    'mid': ((0.4, 1.25, 2.11), (0.0, 0.36, 0.55), 40.0, '25%'),
    'near': ((0.0, 1.10, 1.53), (0.0, 0.36, 0.55), 40.0, '60%'),
    'fill': ((0.0, 0.69, 0.99), (0.0, 0.36, 0.55), 40.0, 'about 100%'),
}
for glass, (pos, target, fov, coverage) in PC2_CAMERAS.items():
    fid = f'm7c_pc2_glass_{glass}_v1'
    add(fid, P911, camera(fid.replace('_v1', '_camera_v1'), pos, target, fov, near=0.02),
        {'kind': 'instanced_grid', 'instance_grid': [1, 1, 1], 'instance_spacing': [0.0, 0.0, 0.0]},
        [SUN, PC2_POINT],
        [f'PC2 close to glass: one Porsche 911 Carrera 4S; windshield/side glass covers {coverage} of the screen.',
         'One unshadowed directional light plus one shadowed (High) point light, 1e4 cd, 10 m range.',
         'Coverage is nominal; measure it with a separate --profile-transparent-overdraw run.'],
        grid_boxes(P911, [1, 1, 1], [0, 0, 0]))

# --- PC3 point-light cost ------------------------------------------------------------------
# 930 left, Carrera right, both sitting on y = 0, a ~1.8 m gap between their bounds.
PC3_930 = [-2.1, -0.095, 0.0]
PC3_911 = [1.9, 0.633, 0.0]
PC3_ENTITIES = [
    {'id': 'porsche_930', 'transform': {'translation': PC3_930}},
    {'id': 'porsche_911', 'source_asset': P911, 'transform': {'translation': PC3_911}},
]
PC3_BOXES = [tuple(np.array(b) + np.array(PC3_930) for b in BOUNDS[P930]),
             tuple(np.array(b) + np.array(PC3_911) for b in BOUNDS[P911])]
PC3_CAMERA = ((0.0, 1.3, -3.4), (0.0, 0.8, 4.0), 60.0)
LIGHT_POS = [0.0, 1.5, 0.0]


def point(range_m, candela=10000.0, shadows=True, quality='high'):
    return {'type': 'point', 'position': LIGHT_POS, 'rotation_degrees': [0.0, 0.0, 0.0],
            'color_linear_rec709': [1.0, 0.9, 0.8], 'luminous_intensity_candela': candela,
            'range_meters': range_m, 'source_radius_meters': 0.05,
            'casts_shadows': shadows, 'shadow_quality': quality, 'priority': 2}


SPOT = {'type': 'spot', 'position': LIGHT_POS, 'rotation_degrees': [90.0, 0.0, 0.0],
        'color_linear_rec709': [1.0, 0.9, 0.8], 'luminous_intensity_candela': 10000.0,
        'range_meters': 10.0, 'source_radius_meters': 0.05,
        'inner_cone_degrees': 50.0, 'outer_cone_degrees': 75.0,
        'casts_shadows': True, 'shadow_quality': 'high', 'priority': 2}
PC3_VARIANTS = [
    ('nolight', None, 'no local light (the directional light only)'),
    ('point_r5', point(5.0), 'one shadowed (High) point light, 1e4 cd, 5 m range'),
    ('point_r10', point(10.0), 'one shadowed (High) point light, 1e4 cd, 10 m range'),
    ('point_r20', point(20.0), 'one shadowed (High) point light, 1e4 cd, 20 m range'),
    ('point_r40', point(40.0), 'one shadowed (High) point light, 1e4 cd, 40 m range'),
    ('point_r10_unshadowed', point(10.0, shadows=False), 'one unshadowed point light, 1e4 cd, 10 m range'),
    ('point_r10_ultra', point(10.0, quality='ultra'), 'one shadowed (Ultra, PCSS) point light, 1e4 cd, 10 m range'),
    ('point_r10_1e3cd', point(10.0, candela=1000.0), 'one shadowed (High) point light, 1e3 cd, 10 m range'),
    ('point_r10_1e6cd', point(10.0, candela=1000000.0), 'one shadowed (High) point light, 1e6 cd, 10 m range'),
    ('spot_r10', SPOT, 'one shadowed (High) spot light, 1e4 cd, 10 m range, aimed down, 50/75 degree cone'),
]
for variant, light, text in PC3_VARIANTS:
    fid = f'm7c_pc3_{variant}_v1'
    pos, target, fov = PC3_CAMERA
    add(fid, P930, camera('m7c_pc3_between_cars_camera_v1', pos, target, fov),
        {'kind': 'composition', 'entities': PC3_ENTITIES},
        [SUN] + ([light] if light else []),
        [f'PC3 point-light cost: the 1975 Porsche 911 930 Turbo (-x) and the 911 Carrera 4S (+x) side by side, '
         f'the camera between them behind the rear bumpers looking forward along the gap; {text}.',
         'The local light sits 1.5 m up between the cars; the directional light is unshadowed.'],
        PC3_BOXES, sources=[P930, P911])

manifest = {
    'schema_version': 1,
    'generator': 'Iridium Engine M7.P1 owner-case fixture generator v1',
    'fixtures': fixtures,
}
text = json.dumps(manifest, indent=2)
# Number arrays on one line.
text = re.sub(r'\[\s*(-?[\d.e+-]+(?:,\s*-?[\d.e+-]+)*)\s*\]',
              lambda match: '[' + ', '.join(v.strip() for v in match.group(1).split(',')) + ']', text)
with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write(text + '\n')
for fid, count, total in report:
    print(f'{fid:42s} visible {count}/{total}')
