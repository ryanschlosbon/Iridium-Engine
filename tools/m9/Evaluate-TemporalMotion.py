"""M9 temporal motion evaluation (harness tooling only).

Scores the captures of tools/m9/Run-MotionEvaluation.ps1: every candidate frame of a moving
fixture against the accumulation reference held at that frame (out/m9/motion/ref64), with
IridiumTemporalMetrics:
  - reference error (tone-mapped RMSE and the share of pixels above 1/64);
  - trail energy: the tone-mapped luma error over the pixels whose reference changed since
    the previous frame (ghosting, smearing and disocclusion recovery concentrate there).
With --baseline (a no-AA capture set of the same plan), each frame also reports the
baseline's values.

usage:
  python tools/m9/Evaluate-TemporalMotion.py --candidate taa-def [--baseline noaa] [--out FILE]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MOTION = os.path.join(ROOT, 'out', 'm9', 'motion')
METRICS = os.path.join(ROOT, 'out', 'build', 'x64-release', 'bin', 'IridiumTemporalMetrics.exe')
WARMUP = 120


def run(args):
    result = subprocess.run([METRICS] + args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f'{args[0]} failed: {result.stderr.strip()}')
    return json.loads(result.stdout)


def exposure_ev(pfm):
    try:
        with open(os.path.splitext(pfm)[0] + '.json', encoding='utf-8-sig') as stream:
            return float(json.load(stream)['render_configuration'].get('manual_exposure_ev', 0.0))
    except (OSError, KeyError, ValueError):
        return 0.0


def flatten(node, out=None):
    """Leaf values by their own key (the metric reports nest them by section)."""
    out = {} if out is None else out
    if isinstance(node, dict):
        for key, value in node.items():
            if isinstance(value, (dict, list)):
                flatten(value, out)
            else:
                out.setdefault(key, value)
    elif isinstance(node, list):
        for value in node:
            flatten(value, out)
    return out


def groups(label):
    """(fixture, [(measured frame, pfm)]) per captured range, in plan order."""
    out = []
    for group in sorted(glob.glob(os.path.join(MOTION, label, '*-*-*'))):
        if not os.path.isdir(group):
            continue
        name = os.path.basename(group)
        fixture = re.sub(r'-\d+-\d+$', '', name)
        pfms = glob.glob(os.path.join(group, fixture, '*', '*.pfm'))
        frames = sorted((int(re.search(r'__mf(\d+)\.pfm$', p).group(1)), p) for p in pfms)
        out.append((fixture, name, frames))
    return out


def main_checkout_motion():
    """out/m9/motion of the main checkout when this is a git worktree (shared references)."""
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute',
                                 '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    except OSError:
        return None
    return os.path.join(os.path.dirname(os.path.normpath(common)), 'out', 'm9', 'motion') if common else None


MAIN_MOTION = main_checkout_motion()


def reference(fixture, measured, samples):
    for motion in [MOTION] + ([MAIN_MOTION] if MAIN_MOTION and MAIN_MOTION != MOTION else []):
        hits = glob.glob(os.path.join(motion, f'ref{samples}', f'{fixture}-h{WARMUP + measured}',
                                      fixture, 'reference', f'*__ref{samples}.pfm'))
        if hits:
            return hits[0]
    return None


def score(fixture, frames, samples):
    refs = [reference(fixture, m, samples) for m, _ in frames]
    if any(r is None for r in refs):
        missing = [m for (m, _), r in zip(frames, refs) if r is None]
        raise RuntimeError(f'{fixture}: no reference for measured frames {missing}')
    ev = exposure_ev(frames[-1][1])
    common = ['--domain', 'scene', '--exposure-ev', str(ev)]
    ghost = run(['ghosting', '--frames'] + [p for _, p in frames] + ['--references'] + refs +
                ['--trail-mask', 'auto', '--trail-k', '1'] + common)
    rows = {}
    for (m, pfm), ref, g in zip(frames, refs, ghost['frames']):
        e = flatten(run(['reference-error', '--reference', ref, '--test', pfm] + common))
        rows[m] = {
            'benchmark_frame': WARMUP + m,
            'tm_rmse': e.get('tone_mapped_rmse'),
            'above_1_64': e.get('above_threshold_fraction'),
            'trail_area': g.get('trail_area'),
            'trail_energy': g.get('trail_energy'),
        }
    return ev, rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--candidate', required=True)
    parser.add_argument('--baseline')
    parser.add_argument('--samples', type=int, default=64)
    parser.add_argument('--out')
    options = parser.parse_args()
    report = {}
    baseline = {}
    if options.baseline:
        for fixture, name, frames in groups(options.baseline):
            _, rows = score(fixture, frames, options.samples)
            baseline.setdefault(fixture, {}).update(rows)
    for fixture, name, frames in groups(options.candidate):
        ev, rows = score(fixture, frames, options.samples)
        entry = report.setdefault(fixture, {'exposure_ev': ev, 'frames': {}})
        for m, row in rows.items():
            if fixture in baseline and m in baseline[fixture]:
                row['baseline'] = baseline[fixture][m]
            entry['frames'][str(m)] = row
    text = json.dumps(report, indent=1, sort_keys=True)
    if options.out:
        with open(options.out, 'w', encoding='utf-8') as stream:
            stream.write(text)
    # Table: candidate (baseline) per frame.
    def fmt(v, digits=4):
        return '-' if v is None else f'{v:.{digits}f}'
    print(f'{"fixture":<14}{"bench":>6}  {"tmRMSE":>15}  {">1/64":>15}  {"trail":>15}  {"area":>8}')
    for fixture, entry in report.items():
        for m, row in sorted(entry['frames'].items(), key=lambda kv: int(kv[0])):
            b = row.get('baseline', {})
            pair = lambda k, d=4: f'{fmt(row[k], d)} ({fmt(b.get(k), d)})' if b else fmt(row[k], d)
            print(f'{fixture:<14}{row["benchmark_frame"]:>6}  {pair("tm_rmse"):>15}  '
                  f'{pair("above_1_64", 3):>15}  {pair("trail_energy"):>15}  '
                  f'{row["trail_area"] if row["trail_area"] is not None else "-":>8}')


if __name__ == '__main__':
    sys.exit(main())
